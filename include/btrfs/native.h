/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_NATIVE_H
#define MACHLIN_BTRFS_NATIVE_H

#include <btrfs/btrfs.h>

/* Darwin adapters currently expose only Linux user.* attributes. Security and
 * trusted namespaces require their owning authorization policy, not aliases. */
#define BTRFS_NATIVE_XATTR_LIMIT (64U * 1024U)
#define BTRFS_NATIVE_XATTR_NAME_MAX 127U
int btrfs_native_xattr_visible(const void *name, size_t length);
enum btrfs_result btrfs_native_filter_xattrs(void *buffer, size_t length, size_t *filtered);
enum btrfs_result btrfs_native_inode_supported(
    const struct btrfs_fs *fs, const struct btrfs_inode *inode);

#endif
