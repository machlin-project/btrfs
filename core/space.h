/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_SPACE_H
#define MACHLIN_BTRFS_SPACE_H

#include "mutable.h"

struct bt_space;

struct bt_space_change {
	uint64_t start;
	uint64_t length;
	size_t chunk;
	int allocate;
};

/* Transaction-local reservations from an independently validated extent tree.
 * Released reservations are not reused within the transaction. The committed
 * extent map stays pinned until the owner discards this entire allocator. */
/* fs->chunks must have room for BT_MAX_CHUNKS: growth appends chunks to it. */
enum btrfs_result bt_space_create(
    struct btrfs_fs *fs, struct bt_root extent_root, size_t node_limit, struct bt_space **result);
/* Verifies the device item and device extents against the chunk map and enables
 * chunk growth from the device's unallocated space. */
enum btrfs_result bt_space_devices(
    struct bt_space *space, struct bt_root chunk_tree, struct bt_root device_tree);
enum btrfs_result bt_space_grow(struct bt_space *space, uint64_t kind, uint64_t minimum);
/* Chunks at or after this index were created by growth in this transaction. */
size_t bt_space_original_chunks(const struct bt_space *space);
void bt_space_allocator(struct bt_space *space, struct bt_mutation_allocator *allocator);
/* Returns up to length bytes (at least one sector) of contiguous free data space;
 * callers repeat for the remainder. Ranges are never handed out twice. */
enum btrfs_result bt_space_reserve_data(
    struct bt_space *space, uint64_t length, uint64_t *logical, uint64_t *size);
enum btrfs_result bt_space_change_used(
    struct bt_space *space, uint64_t address, uint64_t size, int allocate);
uint64_t bt_space_used(const struct bt_space *space, size_t chunk);
size_t bt_space_change_count(const struct bt_space *space);
const struct bt_space_change *bt_space_change(const struct bt_space *space, size_t index);
void bt_space_destroy(struct bt_space *space);

#endif
