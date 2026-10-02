/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_FST_H
#define MACHLIN_BTRFS_FST_H

#include "mutable.h"

struct bt_space_change;

/* Free-space tree (tree 10). Each block group has an info item (extent count
 * and bitmap flag) followed by free extents or by bitmaps of one bit per
 * sector. Free space is the complement of the extent tree inside the block
 * group; superblock stripes are not subtracted. Items of block groups that no
 * longer exist are left alone, as Linux does. */
struct bt_fst_run {
	uint64_t start, end;
};

/* Verifies one block group's free-space items against its free runs as the
 * extent tree gives them (ascending, disjoint and never adjacent). */
enum btrfs_result bt_fst_verify_group(const struct btrfs_fs *fs, struct bt_root tree,
    const struct bt_chunk *chunk, const struct bt_fst_run *expected, size_t count);
/* Linux's set_free_space_tree_thresholds for a block group of length bytes:
 * free extent items take more room than the group's bitmaps above high; a
 * bitmap group returns to extent items below low (high minus 100, or 0). */
void bt_fst_thresholds(uint64_t sector, uint64_t length, uint32_t *high, uint32_t *low);
/* Deletes the info item and every free extent or bitmap item of a block group
 * being removed, as remove_block_group_free_space does. */
enum btrfs_result bt_fst_remove_group(
    struct bt_mutation *mutation, struct bt_root *tree, const struct bt_chunk *chunk);
/* Marks [start, start + length) of the chunk allocated (removed from free
 * space) or freed and updates the block group's extent count; a count past a
 * threshold converts the group between extent items and bitmaps at once. */
enum btrfs_result bt_fst_change(struct bt_mutation *mutation, struct bt_root *tree,
    const struct bt_chunk *chunk, uint64_t start, uint64_t length, int allocate);
/* Applies a normalized batch within one group, reading its info item once
 * and replacing it only if its final count or representation changed. */
enum btrfs_result bt_fst_changes(struct bt_mutation *mutation, struct bt_root *tree,
    const struct bt_chunk *chunk, const struct bt_space_change *changes, size_t count);

#endif
