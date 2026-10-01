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

#define BTRFS_SUPER_COPIES 3U

struct btrfs_super_copy {
	uint64_t offset;
	uint64_t generation;
	/* OK for a checksum-valid copy at its own offset; NOT_FOUND beyond the device. */
	enum btrfs_result status;
	/* Equal to the selected copy except for its offset and checksum. */
	int current;
};

struct btrfs_recovery_report {
	struct btrfs_super_copy copies[BTRFS_SUPER_COPIES];
	unsigned present;
	unsigned selected;
	uint64_t generation;
	unsigned rewritten;
};

/* Explicit, exclusive superblock recovery; mount never selects a mirror. Chooses
 * the newest checksum-valid copy and requires equal-generation copies to agree.
 * It never selects a generation below acknowledged (STALE), a pending tree log
 * (UNSUPPORTED) or a copy of another filesystem (CORRUPT). When copies disagree,
 * it first opens the selection's chunk, root, extent and device trees and its
 * allocation map. A NULL writer only inspects and then returns RECOVERY_REQUIRED.
 * With a writer, each disagreeing copy is rewritten from the selected copy, which
 * itself is never written, followed by one barrier. The report's status fields
 * describe the copies as found; current describes them after a successful call. */
enum btrfs_result btrfs_recover_supers(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t acknowledged,
    struct btrfs_recovery_report *report);

/* Current admission: CRC32C, skinny metadata, SINGLE/DUP, no free-space cache
 * tree, quotas or mixed groups. At least two superblock copies must exist and
 * agree with the mounted primary; otherwise begin returns RECOVERY_REQUIRED.
 * Changes are confined to unshared top-level tree paths. Unsupported layouts
 * return before any media write. Native adapters remain read-only until their
 * visibility and page-cache contracts are wired.
 *
 * Commit writes new metadata, barrier, secondary superblocks, barrier, primary,
 * barrier. Every crash point therefore retains a valid copy at the last
 * acknowledged generation or later; btrfs_recover_supers selects it. */
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
