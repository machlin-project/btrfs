/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_TEST_NAMESPACE_AUDIT_H
#define MACHLIN_BTRFS_TEST_NAMESPACE_AUDIT_H

#include "internal.h"

struct namespace_audit {
	size_t trees;
	size_t inodes;
	size_t names;
	size_t extended_names;
	size_t subvolume_entries;
	size_t collisions;
	size_t xattrs;
	size_t orphans;
	char failure[256];
};

/* Independent namespace check used by tests: in every file tree, each name
 * must appear exactly once as DIR_ITEM, DIR_INDEX and inode reference (or
 * extended reference) with matching index, inode and type; link counts equal
 * the names of each inode; a directory's size is twice the length of its
 * names; inodes without links have orphan items and orphan items name such
 * inodes; every inode item key belongs to an existing inode. */
int namespace_audit(const struct btrfs_fs *fs, struct namespace_audit *audit);

#endif
