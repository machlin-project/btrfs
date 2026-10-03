/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_IO_PROFILE_H
#define MACHLIN_BTRFS_IO_PROFILE_H

#include <stdint.h>

/* Diagnostic builds only: no clocks or logging in a normal extension. The
 * monotonic timestamps share the guest's clock across extension and daemon,
 * so nested intervals (sync, drain, XPC, ioctl) must not be added together. */
#if BTRFS_PROFILE_IO
#include <os/log.h>
#include <time.h>

static inline uint64_t
btrfs_io_profile_start(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0;
	}
	return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static inline void
btrfs_io_profile_end(
    const char *operation, uint64_t start, uint64_t offset, uint64_t bytes, int result)
{
	uint64_t end = btrfs_io_profile_start();

	os_log(OS_LOG_DEFAULT,
	    "btrfs-io operation=%{public}s start_ns=%{public}llu duration_ns=%{public}llu "
	    "offset=%{public}llu bytes=%{public}llu result=%{public}d",
	    operation, (unsigned long long)start, (unsigned long long)(end - start),
	    (unsigned long long)offset, (unsigned long long)bytes, result);
}
#else
static inline uint64_t
btrfs_io_profile_start(void)
{
	return 0;
}

static inline void
btrfs_io_profile_end(
    const char *operation, uint64_t start, uint64_t offset, uint64_t bytes, int result)
{
	(void)operation;
	(void)start;
	(void)offset;
	(void)bytes;
	(void)result;
}
#endif

#endif
