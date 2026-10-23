/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_XNU_FSCTL_H
#define MACHLIN_BTRFS_XNU_FSCTL_H

#include <stdint.h>
#include <sys/ioccom.h>

/* File system controls of a mounted machlin_btrfs volume, shared by the kext
 * and machlin-btrfs, as Linux's BTRFS_IOC_BALANCE_V2 and BTRFS_IOC_SCRUB
 * take them on any file of the volume, for the superuser (CAP_SYS_ADMIN on
 * Linux). Each runs to its end before fsctl(2) returns. */

/* A kind's selection: enabled, Linux's usage filter (groups used below that
 * percentage; 100 selects all), the profile it converts to
 * (MACHLIN_BTRFS_KEEP, 0 for single, or a BTRFS_GROUP_ profile bit) and soft,
 * which leaves groups that already have it. */
struct machlin_btrfs_balance_kind {
	uint32_t enabled;
	uint32_t usage;
	uint64_t convert;
	uint32_t soft;
	uint32_t reserved;
};

#define MACHLIN_BTRFS_KEEP UINT64_MAX

/* btrfs balance start on the mounted volume (read-write): the arguments in,
 * the groups relocated and what they held out. */
struct machlin_btrfs_balance {
	struct machlin_btrfs_balance_kind data;
	struct machlin_btrfs_balance_kind metadata;
	struct machlin_btrfs_balance_kind system;
	uint32_t force;
	uint32_t reserved;
	uint64_t groups;
	uint64_t blocks;
	uint64_t extents;
	uint64_t bytes;
	uint64_t commits;
};

/* btrfs scrub start on the mounted volume: with repair (read-write mounts
 * only), copies that fail are rewritten from good ones; what was found out. */
struct machlin_btrfs_scrub {
	uint32_t repair;
	uint32_t reserved;
	uint64_t tree_copies;
	uint64_t data_copies;
	uint64_t unchecked;
	uint64_t corrupted;
	uint64_t unreadable;
	uint64_t repaired;
	uint64_t unrepairable;
	uint64_t super_copies;
	uint64_t super_errors;
	uint64_t parity_rows;
	uint64_t parity_errors;
};

/* FS_IOC_ENABLE_VERITY on a regular file of a read-write mount, by a caller
 * who may write it, while nothing has it open for writing or mapped writable
 * (ETXTBSY): a hash algorithm (1 SHA-256, 2 SHA-512), a Merkle tree block
 * size (a power of two from 1 KiB up to the sector size) and a salt of at
 * most 32 bytes. Builtin signatures, which Linux checks against a keyring,
 * are not taken. */
#define MACHLIN_BTRFS_VERITY_SALT 32U
#define MACHLIN_BTRFS_VERITY_DIGEST 64U

struct machlin_btrfs_verity_enable {
	uint32_t algorithm;
	uint32_t block_size;
	uint32_t salt_size;
	uint32_t reserved;
	uint8_t salt[MACHLIN_BTRFS_VERITY_SALT];
};

/* FS_IOC_MEASURE_VERITY: the file's digest algorithm and digest; ENODATA
 * for a file without fs-verity. */
struct machlin_btrfs_verity_digest {
	uint32_t algorithm;
	uint32_t size;
	uint8_t digest[MACHLIN_BTRFS_VERITY_DIGEST];
};

#define MACHLIN_BTRFS_FSCTL_BALANCE _IOWR('B', 1, struct machlin_btrfs_balance)
#define MACHLIN_BTRFS_FSCTL_SCRUB _IOWR('B', 2, struct machlin_btrfs_scrub)
#define MACHLIN_BTRFS_FSCTL_ENABLE_VERITY _IOW('B', 3, struct machlin_btrfs_verity_enable)
#define MACHLIN_BTRFS_FSCTL_MEASURE_VERITY _IOR('B', 4, struct machlin_btrfs_verity_digest)

#endif
