/* SPDX-License-Identifier: BSD-3-Clause */
#include "../adapters/xnu/mount_args.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

#define OPTION_DEVICE "device="
#define OPTION_DEGRADED "degraded"
#define MAX_FURTHER_DEVICES 15U

/* Mounts read-only unless -w asks for a read-write mount; -s makes a
 * read-write mount commit every operation before it returns (MNT_SYNCHRONOUS)
 * instead of grouping operations until fsync, sync or the commit interval.
 * As Linux's mount, -o device=PATH names a further device of a volume on
 * several (each in its own option or comma-separated), and -o degraded mounts
 * one read-only without some of them. */
static int
parse_options(char *list, struct machlin_btrfs_devices *devices, uint64_t *paths)
{
	char *option;

	while ((option = strsep(&list, ",")) != NULL) {
		if (strncmp(option, OPTION_DEVICE, strlen(OPTION_DEVICE)) == 0 &&
		    option[strlen(OPTION_DEVICE)] != '\0' && devices->count < MAX_FURTHER_DEVICES) {
			paths[devices->count++] =
			    (uint64_t)(uintptr_t)(option + strlen(OPTION_DEVICE));
		} else if (strcmp(option, OPTION_DEGRADED) == 0) {
			devices->flags |= MACHLIN_BTRFS_ARGS_DEGRADED;
		} else {
			return -1;
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct machlin_btrfs_args arguments;
	uint64_t paths[MAX_FURTHER_DEVICES];
	int flags = MNT_RDONLY | MNT_NOSUID | MNT_NODEV;
	int first = 1;
	int bad = 0;

	memset(&arguments, 0, sizeof(arguments));
	arguments.devices.magic = MACHLIN_BTRFS_ARGS_MAGIC;
	arguments.devices.version = MACHLIN_BTRFS_ARGS_VERSION;
	arguments.devices.paths = (uint64_t)(uintptr_t)paths;
	for (; first < argc && argv[first][0] == '-'; first++) {
		if (strcmp(argv[first], "-w") == 0) {
			flags &= ~MNT_RDONLY;
		} else if (strcmp(argv[first], "-s") == 0) {
			flags |= MNT_SYNCHRONOUS;
		} else if (strcmp(argv[first], "-o") == 0 && first + 1 < argc) {
			first++;
			bad |= parse_options(argv[first], &arguments.devices, paths) != 0;
		} else {
			bad = 1;
		}
	}
	if (bad || argc - first != 2 || ((flags & MNT_SYNCHRONOUS) && (flags & MNT_RDONLY)) ||
	    ((arguments.devices.flags & MACHLIN_BTRFS_ARGS_DEGRADED) && !(flags & MNT_RDONLY))) {
		fprintf(stderr,
		    "usage: mount_machlin_btrfs [-w [-s]] [-o device=PATH,...] "
		    "[-o degraded] DEVICE MOUNTPOINT\n");
		return 2;
	}
	arguments.fspec = (uint64_t)(uintptr_t)argv[first];
	if (mount("machlin_btrfs", argv[first + 1], flags, &arguments) != 0) {
		perror("mount_machlin_btrfs");
		return 1;
	}
	return 0;
}
