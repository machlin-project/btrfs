/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_IDENTITY_H
#define MACHLIN_BTRFS_IDENTITY_H
#include <btrfs/btrfs.h>

#define BTRFS_NATIVE_ROOT_ID UINT64_C(2)
/* A direct native number is (tree slot << BTRFS_NATIVE_SLOT_SHIFT) | inode for
 * inodes from BTRFS_ROOT_INODE through BTRFS_NATIVE_INODE_MASK. The mount
 * root's tree has slot 0, so its objects keep their Btrfs inode numbers; other
 * trees get slots in the order they are first seen. */
#define BTRFS_NATIVE_SLOT_SHIFT 48U
#define BTRFS_NATIVE_INODE_MASK ((UINT64_C(1) << BTRFS_NATIVE_SLOT_SHIFT) - 1U)
#define BTRFS_NATIVE_TREE_LIMIT 32768U
/* Objects without a direct number (other inode values, or trees after the
 * slots are exhausted) get indirect numbers from this base, at most
 * BTRFS_NATIVE_INDIRECT_LIMIT per mount. */
#define BTRFS_NATIVE_INDIRECT_BASE (UINT64_C(1) << 63)
#define BTRFS_NATIVE_INDIRECT_LIMIT 65536U

struct btrfs_identity_table;

/* Native adapters serialize this mount-lifetime table. IDs never alias or get
 * reused, including after vnode/FSItem reclaim; they are not persistent file
 * handles. Core objects always retain their full (tree, inode) identity.
 * Lookup decodes any direct number of a tree that has a slot, issued or not;
 * the caller still has to find the object on disk. */
enum btrfs_result btrfs_identity_create(const struct btrfs_environment *environment,
    struct btrfs_object_id root, struct btrfs_identity_table **result);
void btrfs_identity_destroy(struct btrfs_identity_table *table);
enum btrfs_result btrfs_identity_get(
    struct btrfs_identity_table *table, struct btrfs_object_id object, uint64_t *number);
enum btrfs_result btrfs_identity_lookup(
    const struct btrfs_identity_table *table, uint64_t number, struct btrfs_object_id *object);
#endif
