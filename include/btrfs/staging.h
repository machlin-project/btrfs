/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_STAGING_H
#define MACHLIN_BTRFS_STAGING_H

#include <btrfs/write.h>

/* Adapter-owned write combining, independent of the disk format. Pending bytes
 * overlay device reads. flush issues them all before the device's persistence
 * barrier; a failed write or barrier is terminal. Destroy never issues writes.
 * The owner holds a shared lock for reads and an exclusive lock for every other
 * operation, and keeps both callback contexts alive until destroy returns. */
struct btrfs_staging;

#define BTRFS_STAGING_MAX_BYTES (32U * 1024U * 1024U)
#define BTRFS_STAGING_MAX_RUN_BYTES (8U * 1024U * 1024U)
#define BTRFS_STAGING_MAX_RUNS 4096U

struct btrfs_staging_limits {
	size_t bytes;
	size_t run_bytes;
	size_t runs;
};

/* Nonzero limits may be smaller than the maxima. Payload stays within bytes;
 * geometric buffer capacities use less than twice that, plus run_bytes while
 * growing a run. At most runs records are searched, overlaid or drained.
 * An individual write larger than run_bytes drains and bypasses the buffer.
 * The adapter checks any device alignment requirements before staging. */
enum btrfs_result btrfs_staging_create(const struct btrfs_environment *reader,
    const struct btrfs_write_environment *writer, struct btrfs_staging_limits limits,
    struct btrfs_staging **result);
void btrfs_staging_destroy(struct btrfs_staging *staging);
enum btrfs_result btrfs_staging_read(
    struct btrfs_staging *staging, uint64_t offset, void *bytes, size_t length);
enum btrfs_result btrfs_staging_write(
    struct btrfs_staging *staging, uint64_t offset, const void *bytes, size_t length);
enum btrfs_result btrfs_staging_flush(struct btrfs_staging *staging);

#endif
