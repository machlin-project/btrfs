/* SPDX-License-Identifier: BSD-3-Clause */
#include "encode.h"
#include "fst.h"
#include "space.h"
#include "transaction.h"

/* Another group of the same kind and profile that stays: Linux keeps the last
 * one, whose flags the next chunk of that kind copies. */
static int
bt_group_has_sibling(const struct btrfs_transaction *transaction, size_t chunk)
{
	const struct btrfs_fs *fs = &transaction->fs;
	size_t i;

	for (i = 0; i < fs->chunk_count; i++) {
		if (i != chunk && !fs->chunks[i].removed &&
		    fs->chunks[i].type == fs->chunks[chunk].type) {
			return 1;
		}
	}
	return 0;
}

/* Whether the root tree names a v1 space-cache inode for the group; Linux
 * deletes that inode with the group, which this writer does not. */
static enum btrfs_result
bt_group_cached(struct btrfs_transaction *transaction, const struct bt_chunk *chunk, int *cached)
{
	struct bt_key key = { BT_FREE_SPACE_OBJECTID, chunk->logical, 0 };
	uint8_t header[64];
	size_t length;
	enum btrfs_result error;

	error = bt_mutation_find(
	    transaction->mutation, transaction->roots, key, header, sizeof(header), &length);
	*cached = error == BTRFS_OK || error == BTRFS_RANGE;
	return error == BTRFS_NOT_FOUND || error == BTRFS_RANGE ? BTRFS_OK : error;
}

enum btrfs_result
btrfs_transaction_remove_unused_groups(struct btrfs_transaction *transaction, size_t *removed)
{
	size_t i;
	int cached = 0;
	int unused = 0;
	enum btrfs_result error = BTRFS_OK;

	if (transaction == NULL || removed == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*removed = 0;
	if (transaction->failure != BTRFS_OK || transaction->finished) {
		return transaction->failure == BTRFS_OK ? BTRFS_READ_ONLY : transaction->failure;
	}
	for (i = 0; error == BTRFS_OK && i < transaction->fs.chunk_count; i++) {
		error = bt_space_unused(transaction->space, i, &unused);
		if (error != BTRFS_OK || !unused || !bt_group_has_sibling(transaction, i)) {
			continue;
		}
		error = bt_group_cached(transaction, &transaction->fs.chunks[i], &cached);
		if (error == BTRFS_OK && !cached) {
			/* The chunk item removal and device item update need system
			 * space, as Linux's btrfs_remove_chunk checks first. */
			error = bt_space_check_system(transaction->space);
		}
		if (error == BTRFS_OK && !cached) {
			error = bt_space_retire(transaction->space, i);
		}
		if (error == BTRFS_OK && !cached) {
			transaction->changed = 1;
			(*removed)++;
		}
	}
	if (error != BTRFS_OK) {
		transaction->failure = error;
	}
	return error;
}

/* Drops a system chunk's entry from the superblock's bootstrap array, as
 * btrfs_del_sys_chunk does. */
static enum btrfs_result
bt_group_system_array_remove(struct btrfs_transaction *transaction, uint64_t logical)
{
	const struct bt_disk_key *key;
	const struct bt_disk_chunk *chunk;
	uint8_t *array = transaction->super.system_array;
	size_t size = bt_u32(transaction->super.system_array_size);
	size_t offset = 0;
	size_t length;

	if (size > BT_SYSTEM_ARRAY_SIZE) {
		return BTRFS_CORRUPT;
	}
	while (offset < size) {
		if (size - offset < sizeof(*key) + sizeof(*chunk)) {
			return BTRFS_CORRUPT;
		}
		key = (const void *)(array + offset);
		chunk = (const void *)(key + 1);
		length = sizeof(*key) + sizeof(*chunk) +
		    bt_u16(chunk->stripes) * sizeof(struct bt_disk_stripe);
		if (length > size - offset) {
			return BTRFS_CORRUPT;
		}
		if (bt_key_decode(key).offset == logical) {
			bt_move(array + offset, array + offset + length, size - offset - length);
			bt_zero(array + size - length, length);
			bt_put32(&transaction->super.system_array_size, (uint32_t)(size - length));
			return BTRFS_OK;
		}
		offset += length;
	}
	return BTRFS_CORRUPT;
}

/* The items of one removed group, as btrfs_remove_chunk and
 * btrfs_remove_block_group delete them: the block group and its free-space
 * items, the device extents with the device's used bytes, and the chunk item
 * (and system array entry). Its device space is free from the next
 * transaction on. */
static enum btrfs_result
bt_group_remove(struct btrfs_transaction *transaction, const struct bt_chunk *chunk)
{
	struct bt_disk_device device;
	const struct btrfs_fs *base = transaction->base;
	struct bt_key key = { chunk->logical, chunk->length, BT_BLOCK_GROUP_ITEM };
	size_t size;
	unsigned stripe;
	enum btrfs_result error;

	error = bt_tx_edit(transaction, bt_tx_groups(transaction), key, NULL, 0, BT_DELETE);
	if (error == BTRFS_OK && transaction->has_free_space) {
		error = bt_fst_remove_group(
		    transaction->mutation, &transaction->free_space.root, chunk);
	}
	for (stripe = 0; error == BTRFS_OK && stripe < chunk->mirrors; stripe++) {
		key = (struct bt_key){ base->device_id, chunk->physical[stripe], BT_DEV_EXTENT };
		error =
		    bt_tx_edit(transaction, &transaction->devices.root, key, NULL, 0, BT_DELETE);
	}
	key = (struct bt_key){ BT_DEV_ITEMS_OBJECTID, base->device_id, BT_DEV_ITEM };
	if (error == BTRFS_OK) {
		error = bt_mutation_find(transaction->mutation, transaction->chunks, key, &device,
		    sizeof(device), &size);
		error = error == BTRFS_OK &&
			(size != sizeof(device) ||
			    bt_u64(device.used_bytes) < chunk->length * chunk->mirrors)
		    ? BTRFS_CORRUPT
		    : error;
	}
	if (error == BTRFS_OK) {
		bt_put64(
		    &device.used_bytes, bt_u64(device.used_bytes) - chunk->length * chunk->mirrors);
		transaction->super.device.used_bytes = device.used_bytes;
		error = bt_tx_edit(
		    transaction, &transaction->chunks, key, &device, sizeof(device), BT_REPLACE);
	}
	if (error == BTRFS_OK) {
		key = (struct bt_key){ BT_FIRST_CHUNK_OBJECTID, chunk->logical, BT_CHUNK_ITEM };
		error = bt_tx_edit(transaction, &transaction->chunks, key, NULL, 0, BT_DELETE);
	}
	if (error == BTRFS_OK && (chunk->type & BT_BLOCK_SYSTEM) != 0) {
		error = bt_group_system_array_remove(transaction, chunk->logical);
	}
	return error;
}

enum btrfs_result
bt_tx_remove_groups(struct btrfs_transaction *transaction)
{
	size_t i;
	enum btrfs_result error = BTRFS_OK;

	for (i = 0; error == BTRFS_OK && i < transaction->fs.chunk_count; i++) {
		if (transaction->fs.chunks[i].removed) {
			error = bt_group_remove(transaction, &transaction->fs.chunks[i]);
		}
	}
	return error;
}
