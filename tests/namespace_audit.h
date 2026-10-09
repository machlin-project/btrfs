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
	size_t subvolume_refs;
	size_t dead_trees;
	size_t collisions;
	size_t xattrs;
	size_t orphans;
	size_t hole_items;
	size_t verity_items;
	char failure[256];
};

/* Independent namespace check used by tests: in every file tree, each name
 * must appear exactly once as DIR_ITEM, DIR_INDEX and inode reference (or
 * extended reference) with matching index, inode and type; link counts equal
 * the names of each inode; a directory's size is twice the length of its
 * names; inodes without links have orphan items and orphan items name such
 * inodes, or regular files whose fs-verity enable has not finished; fs-verity
 * items belong to a regular file with fs-verity and its descriptor, or to such
 * an enable; every inode item key belongs to an existing inode. Like btrfs
 * check, the file extents of a regular file or symlink do not overlap and
 * count its bytes, and without NO_HOLES they cover it below its size. */
int namespace_audit(const struct btrfs_fs *fs, struct namespace_audit *audit);

struct namespace_digest {
	uint64_t hash;
	size_t objects;
	uint64_t bytes;
	char failure[256];
};

/* A hash of everything the public read interface shows from the mounted
 * tree's root: names in stream order, every inode attribute, file and symlink
 * bytes, and extended attributes, descending into subvolumes. Two views with
 * equal digests read the same. */
int namespace_digest(const struct btrfs_fs *fs, struct namespace_digest *digest);

#endif
