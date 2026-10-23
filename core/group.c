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

/* Items of a v1 space-cache inode removed at most: its inode item and
 * whatever else Linux's eviction would truncate. */
#define BT_GROUP_CACHE_ITEMS 64U

/* The v1 space-cache inode the root tree's free-space header names for the
 * group at logical; *ino is 0 when the group has none. */
static enum btrfs_result
bt_group_cache_inode(struct btrfs_transaction *transaction, uint64_t logical, uint64_t *ino)
{
	struct bt_disk_free_space_header header;
	struct bt_key key = { .objectid = BT_FREE_SPACE_OBJECTID, .offset = logical };
	struct bt_key location;
	size_t length;
	enum btrfs_result error;

	*ino = 0;
	error = bt_mutation_find(
	    transaction->mutation, transaction->roots, key, &header, sizeof(header), &length);
	if (error == BTRFS_NOT_FOUND) {
		return BTRFS_OK;
	}
	if (error != BTRFS_OK || length != sizeof(header)) {
		return error == BTRFS_OK || error == BTRFS_RANGE ? BTRFS_CORRUPT : error;
	}
	location = bt_key_decode(&header.location);
	if (location.type != BT_INODE_ITEM || location.objectid < BT_FIRST_CHUNK_OBJECTID ||
	    location.objectid > BT_LAST_FREE_OBJECTID) {
		return BTRFS_CORRUPT;
	}
	*ino = location.objectid;
	return BTRFS_OK;
}

enum btrfs_result
bt_group_cache_drop(struct btrfs_transaction *transaction, uint64_t ino, int remove)
{
	struct bt_owned_root tree;
	struct bt_disk_inode inode;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = ino, .type = BT_INODE_ITEM };
	uint64_t removed = 0;
	size_t length;
	size_t steps;
	int found = 1;
	enum btrfs_result error;

	/* The root tree as the file tree of its cache inode. */
	bt_zero(&tree, sizeof(tree));
	tree.root = transaction->roots;
	error = bt_tx_drop_range(transaction, &tree, ino, 0, UINT64_MAX, &removed);
	transaction->roots = tree.root;
	if (error != BTRFS_OK) {
		return error;
	}
	if (!remove) {
		/* btrfs_truncate_free_space_cache: the inode stays, empty. */
		error = bt_mutation_find(
		    transaction->mutation, transaction->roots, key, &inode, sizeof(inode), &length);
		if (error == BTRFS_NOT_FOUND) {
			return BTRFS_OK;
		}
		if (error != BTRFS_OK || length != sizeof(inode)) {
			return error == BTRFS_OK || error == BTRFS_RANGE ? BTRFS_CORRUPT : error;
		}
		bt_put64(&inode.size, 0);
		bt_put64(&inode.nbytes, 0);
		bt_put64(&inode.transid, transaction->base->info.generation + 1);
		return bt_tx_edit(
		    transaction, &transaction->roots, key, &inode, sizeof(inode), BT_REPLACE);
	}
	/* Evicting it removes every item it has left. */
	for (steps = 0; error == BTRFS_OK && found; steps++) {
		if (steps == BT_GROUP_CACHE_ITEMS) {
			return BTRFS_UNSUPPORTED;
		}
		bt_cursor_init(
		    &cursor, bt_mutation_view(transaction->mutation), transaction->roots);
		error = bt_cursor_seek(&cursor, (struct bt_key){ .objectid = ino }, 0);
		found = 0;
		if (error == BTRFS_OK) {
			(void)bt_cursor_record(&cursor, &record);
			found = record.key.objectid == ino;
		}
		bt_cursor_fini(&cursor);
		if (error == BTRFS_NOT_FOUND) {
			error = BTRFS_OK;
		}
		if (error == BTRFS_OK && found) {
			error = bt_tx_edit(
			    transaction, &transaction->roots, record.key, NULL, 0, BT_DELETE);
		}
	}
	return error;
}

enum btrfs_result
bt_group_cache_truncate(struct btrfs_transaction *transaction, uint64_t logical)
{
	uint64_t ino;
	enum btrfs_result error;

	error = bt_group_cache_inode(transaction, logical, &ino);
	return error == BTRFS_OK && ino != 0 ? bt_group_cache_drop(transaction, ino, 0) : error;
}

enum btrfs_result
bt_group_cache_remove(struct btrfs_transaction *transaction, uint64_t logical)
{
	struct bt_key key = { .objectid = BT_FREE_SPACE_OBJECTID, .offset = logical };
	uint64_t ino;
	enum btrfs_result error;

	error = bt_group_cache_inode(transaction, logical, &ino);
	if (error != BTRFS_OK || ino == 0) {
		return error;
	}
	error = bt_group_cache_drop(transaction, ino, 1);
	if (error == BTRFS_OK) {
		error = bt_tx_edit(transaction, &transaction->roots, key, NULL, 0, BT_DELETE);
	}
	return error;
}

enum btrfs_result
btrfs_transaction_remove_unused_groups(struct btrfs_transaction *transaction, size_t *removed)
{
	size_t i;
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
		/* The chunk item removal and device item update need system space,
		 * as Linux's btrfs_remove_chunk checks first. */
		error = bt_space_check_system(transaction->space);
		/* Its v1 space cache goes with it (btrfs_remove_block_group). */
		if (error == BTRFS_OK) {
			error =
			    bt_group_cache_remove(transaction, transaction->fs.chunks[i].logical);
		}
		if (error == BTRFS_OK) {
			error = bt_space_retire(transaction->space, i);
		}
		if (error == BTRFS_OK) {
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
 * items, the device extents with their devices' used bytes, the chunk item
 * (and system array entry). Its device space is free from the next
 * transaction on. */
static enum btrfs_result
bt_group_remove(struct btrfs_transaction *transaction, const struct bt_chunk *chunk)
{
	const struct bt_device *device;
	struct bt_key key = { chunk->logical, chunk->length, BT_BLOCK_GROUP_ITEM };
	uint64_t length = bt_chunk_stripe_length(chunk);
	unsigned stripe;
	enum btrfs_result error;

	error = bt_tx_edit(transaction, bt_tx_groups(transaction), key, NULL, 0, BT_DELETE);
	if (error == BTRFS_OK && transaction->has_free_space) {
		error = bt_fst_remove_group(
		    transaction->mutation, &transaction->free_space.root, chunk);
	}
	for (stripe = 0; error == BTRFS_OK && stripe < chunk->stripes; stripe++) {
		device = &transaction->fs.devices[chunk->devices[stripe]];
		key = (struct bt_key){ device->id, chunk->physical[stripe], BT_DEV_EXTENT };
		error =
		    bt_tx_edit(transaction, &transaction->devices.root, key, NULL, 0, BT_DELETE);
		if (error == BTRFS_OK) {
			error = bt_tx_device_used(transaction, device->id, length, 0);
		}
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
