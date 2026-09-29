/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_SPACE_H
#define MACHLIN_BTRFS_SPACE_H

#include "mutable.h"

struct bt_space;

/* Transaction-local reservations from an independently validated extent tree.
 * Released reservations are not reused within the transaction. The committed
 * extent map stays pinned until the owner discards this entire allocator. */
enum btrfs_result bt_space_create(const struct btrfs_fs *fs, struct bt_root extent_root,
    size_t node_limit, struct bt_space **result);
void bt_space_allocator(struct bt_space *space, struct bt_mutation_allocator *allocator);
enum btrfs_result bt_space_change_used(struct bt_space *space, uint64_t address, int allocate);
uint64_t bt_space_used(const struct bt_space *space, size_t chunk);
void bt_space_destroy(struct bt_space *space);

#endif
