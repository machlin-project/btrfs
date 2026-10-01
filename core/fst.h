/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_FST_H
#define MACHLIN_BTRFS_FST_H

#include "mutable.h"

/* Free-space tree (tree 10). Each block group has an info item (extent count
 * and bitmap flag) followed by free extents or by bitmaps of one bit per
 * sector. Free space is the complement of the extent tree inside the block
 * group; superblock stripes are not subtracted. Items of block groups that no
 * longer exist are left alone, as Linux does. */
enum btrfs_result bt_fst_verify(
    const struct btrfs_fs *fs, struct bt_root tree, struct bt_root extents);
/* Marks [start, start + length) of the chunk allocated (removed from free
 * space) or freed, keeping the block group's representation and extent count. */
enum btrfs_result bt_fst_change(struct bt_mutation *mutation, struct bt_root *tree,
    const struct bt_chunk *chunk, uint64_t start, uint64_t length, int allocate);

#endif
