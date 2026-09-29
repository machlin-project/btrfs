/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_IDENTITY_H
#define MACHLIN_BTRFS_IDENTITY_H
#include <btrfs/btrfs.h>

#define BTRFS_NATIVE_ROOT_ID UINT64_C(2)
#define BTRFS_NATIVE_ID_LIMIT 65536U

struct btrfs_identity_table;

/* Native adapters serialize this mount-lifetime table. IDs never alias or get
 * reused, including after vnode/FSItem reclaim; they are not persistent file
 * handles. Core objects always retain their full (tree, inode) identity. */
enum btrfs_result btrfs_identity_create(const struct btrfs_environment *environment,
    struct btrfs_object_id root, struct btrfs_identity_table **result);
void btrfs_identity_destroy(struct btrfs_identity_table *table);
enum btrfs_result btrfs_identity_get(
    struct btrfs_identity_table *table, struct btrfs_object_id object, uint64_t *number);
enum btrfs_result btrfs_identity_lookup(
    const struct btrfs_identity_table *table, uint64_t number, struct btrfs_object_id *object);
#endif
