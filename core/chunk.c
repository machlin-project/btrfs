/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

enum btrfs_result
bt_chunk_add(struct btrfs_fs *fs, struct bt_key key, const void *data, size_t length, int bootstrap)
{
	const struct bt_disk_chunk *disk = data;
	const struct bt_disk_stripe *stripes;
	struct bt_chunk chunk;
	struct bt_chunk *existing;
	uint64_t profile;
	uint64_t kind;
	unsigned count;
	unsigned j;
	size_t i;

	if (key.objectid != BT_FIRST_CHUNK_OBJECTID || key.type != BT_CHUNK_ITEM ||
	    length < sizeof(*disk)) {
		return BTRFS_CORRUPT;
	}
	bt_zero(&chunk, sizeof(chunk));
	chunk.logical = key.offset;
	chunk.length = bt_u64(disk->length);
	chunk.type = bt_u64(disk->type);
	chunk.confirmed = !bootstrap;
	count = bt_u16(disk->stripes);
	if (count == 0 || (length - sizeof(*disk)) / sizeof(*stripes) != count ||
	    (length - sizeof(*disk)) % sizeof(*stripes) != 0) {
		return BTRFS_CORRUPT;
	}
	profile = chunk.type & ~(BT_BLOCK_DATA | BT_BLOCK_METADATA | BT_BLOCK_SYSTEM);
	kind = chunk.type & (BT_BLOCK_DATA | BT_BLOCK_METADATA | BT_BLOCK_SYSTEM);
	if (profile != 0 && profile != BT_BLOCK_DUP) {
		return BTRFS_UNSUPPORTED;
	}
	if ((profile == BT_BLOCK_DUP ? count != 2 : count != 1) ||
	    (kind != BT_BLOCK_DATA && kind != BT_BLOCK_METADATA && kind != BT_BLOCK_SYSTEM &&
		kind != (BT_BLOCK_DATA | BT_BLOCK_METADATA)) ||
	    (bootstrap && kind != BT_BLOCK_SYSTEM) || bt_u64(disk->owner) != BT_EXTENT_TREE ||
	    bt_u64(disk->stripe_length) != BT_STRIPE_LENGTH ||
	    bt_u32(disk->sector_size) != fs->info.sector_size || chunk.length == 0 ||
	    chunk.length > UINT64_MAX - chunk.logical ||
	    chunk.logical % fs->info.sector_size != 0 || chunk.length % fs->info.sector_size != 0) {
		return BTRFS_CORRUPT;
	}
	stripes = (const void *)((const uint8_t *)data + sizeof(*disk));
	chunk.mirrors = (uint8_t)count;
	for (j = 0; j < count; j++) {
		chunk.physical[j] = bt_u64(stripes[j].offset);
		if (bt_u64(stripes[j].device) != fs->device_id ||
		    !bt_equal(stripes[j].uuid, fs->device_uuid, BTRFS_UUID_SIZE) ||
		    chunk.physical[j] % fs->info.sector_size != 0 ||
		    chunk.physical[j] > fs->device_size ||
		    chunk.length > fs->device_size - chunk.physical[j]) {
			return BTRFS_CORRUPT;
		}
	}
	if (count == 2 && chunk.physical[0] < chunk.physical[1] + chunk.length &&
	    chunk.physical[1] < chunk.physical[0] + chunk.length) {
		return BTRFS_CORRUPT;
	}
	for (i = 0; i < fs->chunk_count; i++) {
		existing = &fs->chunks[i];
		if (existing->logical == chunk.logical) {
			if (bootstrap || existing->confirmed || existing->length != chunk.length ||
			    existing->type != chunk.type || existing->mirrors != chunk.mirrors ||
			    !bt_equal(existing->physical, chunk.physical, sizeof(chunk.physical))) {
				return BTRFS_CORRUPT;
			}
			existing->confirmed = 1;
			return BTRFS_OK;
		}
		if (existing->logical < chunk.logical + chunk.length &&
		    chunk.logical < existing->logical + existing->length) {
			return BTRFS_CORRUPT;
		}
	}
	if (fs->chunk_count == BT_MAX_CHUNKS) {
		return BTRFS_UNSUPPORTED;
	}
	i = fs->chunk_count;
	while (i > 0 && fs->chunks[i - 1].logical > chunk.logical) {
		fs->chunks[i] = fs->chunks[i - 1];
		i--;
	}
	fs->chunks[i] = chunk;
	fs->chunk_count++;
	return BTRFS_OK;
}

enum btrfs_result
bt_map(const struct btrfs_fs *fs, uint64_t logical, size_t length, uint64_t kind, unsigned mirror,
    uint64_t *physical, unsigned *mirrors)
{
	const struct bt_chunk *chunk;
	size_t low = 0;
	size_t high = fs->chunk_count;
	size_t middle;
	uint64_t within;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (fs->chunks[middle].logical <= logical) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	if (low == 0) {
		return BTRFS_CORRUPT;
	}
	chunk = &fs->chunks[low - 1];
	within = logical - chunk->logical;
	*mirrors = chunk->mirrors;
	if ((chunk->type & kind) == 0 || within >= chunk->length ||
	    length > chunk->length - within || mirror >= chunk->mirrors) {
		return BTRFS_CORRUPT;
	}
	*physical = chunk->physical[mirror] + within;
	return BTRFS_OK;
}
