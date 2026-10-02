/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_SPACE_H
#define MACHLIN_BTRFS_SPACE_H

#include "mutable.h"
#include <btrfs/write.h>

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
/* Linux's check_system_chunk before a chunk is added or removed: grows a
 * system chunk when free system space is below the nodes one chunk item and
 * one device item update may need; failing to grow is not an error. */
enum btrfs_result bt_space_check_system(struct bt_space *space);
/* Whether a group of the committed state held nothing and this transaction has
 * neither allocated nor freed anything in it. */
int bt_space_unused(const struct bt_space *space, size_t chunk);
/* Takes an unused group out of allocation and marks it removed. */
void bt_space_retire(struct bt_space *space, size_t chunk);
/* Chunks at or after this index were created by growth in this transaction. */
size_t bt_space_original_chunks(const struct bt_space *space);
void bt_space_allocator(struct bt_space *space, struct bt_mutation_allocator *allocator);
/* Returns up to length bytes (at least one sector) of contiguous free data space;
 * callers repeat for the remainder. Ranges are never handed out twice. */
enum btrfs_result bt_space_reserve_data(
    struct bt_space *space, uint64_t length, uint64_t *logical, uint64_t *size);
/* Whether length bytes of data fit in free data space and in device space a
 * new data chunk could take: Linux reserves a buffered write's data space
 * before accepting it. */
int bt_space_data_available(const struct bt_space *space, uint64_t length);
/* Returns exactly length bytes of contiguous free data space (a compressed
 * extent's size), from the first gap that holds them. */
enum btrfs_result bt_space_reserve_exact(
    struct bt_space *space, uint64_t length, uint64_t *logical);
enum btrfs_result bt_space_change_used(
    struct bt_space *space, uint64_t address, uint64_t size, int allocate);
uint64_t bt_space_used(const struct bt_space *space, size_t chunk);
size_t bt_space_change_count(const struct bt_space *space);
const struct bt_space_change *bt_space_change(const struct bt_space *space, size_t index);
void bt_space_destroy(struct bt_space *space);

/* The allocation map an owner keeps across transactions (btrfs_allocation_map
 * in write.h). A begin whose base is the map's generation copies the map
 * instead of loading and verifying the extent, free-space and device trees;
 * otherwise it verifies as usual and saves the fresh state. A successful commit
 * replays its chunk growth, allocation log and removed groups into the map. */
int bt_space_map_fits(const struct btrfs_allocation_map *map, const struct btrfs_fs *fs);
enum btrfs_result bt_space_from_map(struct btrfs_fs *fs, struct btrfs_allocation_map *map,
    size_t node_limit, struct bt_space **result);
enum btrfs_result bt_space_map_save(const struct bt_space *space, struct btrfs_allocation_map *map);
enum btrfs_result bt_space_map_commit(
    const struct bt_space *space, struct btrfs_allocation_map *map, uint64_t generation);
void bt_space_map_invalidate(struct btrfs_allocation_map *map);
/* Tests: whether the map equals a freshly verified space of its generation. */
enum btrfs_result bt_space_map_check(
    const struct btrfs_allocation_map *map, const struct bt_space *space);

#endif
