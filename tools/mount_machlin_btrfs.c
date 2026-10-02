/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

/* Mounts read-only unless -w asks for a read-write mount; -s makes a
 * read-write mount commit every operation before it returns (MNT_SYNCHRONOUS)
 * instead of grouping operations until fsync, sync or the commit interval. */
int
main(int argc, char **argv)
{
	struct {
		char *fspec;
	} arguments;

	int flags = MNT_RDONLY | MNT_NOSUID | MNT_NODEV;
	int first = 1;

	for (; first < argc && argv[first][0] == '-'; first++) {
		if (strcmp(argv[first], "-w") == 0) {
			flags &= ~MNT_RDONLY;
		} else if (strcmp(argv[first], "-s") == 0) {
			flags |= MNT_SYNCHRONOUS;
		} else {
			first = argc;
		}
	}
	if (argc - first != 2 || ((flags & MNT_SYNCHRONOUS) && (flags & MNT_RDONLY))) {
		fprintf(stderr, "usage: mount_machlin_btrfs [-w [-s]] DEVICE MOUNTPOINT\n");
		return 2;
	}
	arguments.fspec = argv[first];
	if (mount("machlin_btrfs", argv[first + 1], flags, &arguments) != 0) {
		perror("mount_machlin_btrfs");
		return 1;
	}
	return 0;
}
