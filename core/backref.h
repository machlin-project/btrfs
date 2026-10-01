/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_BACKREF_H
#define MACHLIN_BTRFS_BACKREF_H

#include "mutable.h"

/* One extent backreference. A nonzero parent selects the shared (full)
 * form naming the referencing block; otherwise tree references name a root and
 * data references name (root, inode, file offset minus extent offset). */
struct bt_backref {
	uint64_t parent;
	uint64_t root;
	uint64_t inode;
	uint64_t offset;
	int data;
};

/* Extent items are keyed (bytenr, METADATA_ITEM, level) for skinny tree blocks
 * and (bytenr, EXTENT_ITEM, length) for data. Edits go through the private
 * mutation of the extent tree rooted at *extents, never to media. Inline versus
 * keyed placement, ordering, counts and hash-collision probing follow Linux so
 * that Linux can continue the tree. Each operation validates the item first. */
enum btrfs_result bt_backref_info(struct bt_mutation *mutation, struct bt_root extents,
    struct bt_key extent, uint64_t *refs, uint64_t *flags);
/* The count of one reference: an inline or keyed item, 0 when absent. */
enum btrfs_result bt_backref_count(struct bt_mutation *mutation, struct bt_root extents,
    struct bt_key extent, const struct bt_backref *reference, uint64_t *count);
enum btrfs_result bt_backref_add(struct bt_mutation *mutation, struct bt_root *extents,
    struct bt_key extent, const struct bt_backref *reference, uint32_t count);
/* Removes count references; deletes the extent item when its last reference
 * goes and reports that through freed. Freeing space is the caller's job. */
enum btrfs_result bt_backref_drop(struct bt_mutation *mutation, struct bt_root *extents,
    struct bt_key extent, const struct bt_backref *reference, uint32_t count, int *freed);
enum btrfs_result bt_backref_set_flags(
    struct bt_mutation *mutation, struct bt_root *extents, struct bt_key extent, uint64_t flags);
uint64_t bt_backref_data_hash(uint64_t root, uint64_t inode, uint64_t offset);

#endif
