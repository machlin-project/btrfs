/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_TEST_REFERENCES_H
#define MACHLIN_BTRFS_TEST_REFERENCES_H

#include "internal.h"

struct reference_audit {
	size_t trees;
	size_t blocks;
	size_t data_extents;
	size_t tree_refs;
	size_t shared_block_refs;
	size_t data_refs;
	size_t shared_data_refs;
	size_t keyed_refs;
	size_t full_backref_blocks;
	size_t checksums;
	size_t checked_copies;
	char failure[256];
};

/* Independent reference check used by tests: walks every tree from the root
 * tree and the superblock, derives the backreferences each parent pointer and
 * file extent item requires, and compares them exactly with the extent tree.
 * Checksum items must lie in data extents, match the stored sectors of every
 * copy and cover every extent of a checksummed file. */
int reference_audit(const struct btrfs_fs *fs, struct reference_audit *audit);

#endif
