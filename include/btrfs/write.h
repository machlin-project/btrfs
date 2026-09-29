/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_WRITE_H
#define MACHLIN_BTRFS_WRITE_H

#include <btrfs/btrfs.h>

struct btrfs_transaction;

/* Separate from the immutable reader's environment: write authority is explicit.
 * The owner must hold exclusive resource access, drain readers before commit,
 * and retire the original btrfs_fs after a successful or uncertain commit.
 * A failed write/flush may have reached media; never reuse that writer or claim
 * rollback. flush must order and durably persist all preceding writes. */
struct btrfs_write_environment {
	void *context;
	enum btrfs_result (*write)(void *context, uint64_t offset, const void *bytes, size_t size);
	enum btrfs_result (*flush)(void *context);
};

/* Current admission: CRC32C, skinny metadata, SINGLE/DUP, no free-space cache
 * tree, quotas or mixed groups. Changes are confined to unshared top-level tree
 * paths. Unsupported layouts return before any media write. Native adapters
 * remain read-only until their visibility and page-cache contracts are wired. */
enum btrfs_result btrfs_transaction_begin(const struct btrfs_fs *base,
    const struct btrfs_write_environment *environment, struct btrfs_transaction **result);
/* Replace an existing, uncompressed inline regular file (including hardlinks).
 * No file creation, external extent conversion or implicit truncation of other
 * extents. Payloads are bounded by Btrfs's 2 KiB default inline-write policy. */
enum btrfs_result btrfs_transaction_write_inline(struct btrfs_transaction *transaction,
    struct btrfs_object_id id, const void *bytes, size_t size, struct btrfs_time modified);
enum btrfs_result btrfs_transaction_commit(struct btrfs_transaction *transaction);
void btrfs_transaction_destroy(struct btrfs_transaction *transaction);

#endif
