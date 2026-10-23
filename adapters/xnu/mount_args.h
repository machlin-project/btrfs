/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_XNU_MOUNT_ARGS_H
#define MACHLIN_BTRFS_XNU_MOUNT_ARGS_H

#include <stdint.h>

/* mount(2)'s data for machlin_btrfs, shared by mount_machlin_btrfs and the
 * kext. XNU reads the device path's address first, as for every local
 * volume, and passes the file system what follows it: the volume's further
 * devices, as Linux's mount takes them with -o device=PATH. */
#define MACHLIN_BTRFS_ARGS_MAGIC 0x6274726dU
#define MACHLIN_BTRFS_ARGS_VERSION 1U
/* Mount read-only without some devices, as Linux's -o degraded. */
#define MACHLIN_BTRFS_ARGS_DEGRADED 0x1U

struct machlin_btrfs_devices {
	uint32_t magic;
	uint32_t version;
	/* User address of count user addresses of device paths. */
	uint64_t paths;
	uint32_t count;
	uint32_t flags;
};

struct machlin_btrfs_args {
	/* User address of the mounted device's path. */
	uint64_t fspec;
	struct machlin_btrfs_devices devices;
};

#endif
