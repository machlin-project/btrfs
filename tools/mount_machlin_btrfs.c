/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdio.h>
#include <sys/mount.h>

int
main(int argc, char **argv)
{
	struct {
		char *fspec;
	} arguments;

	if (argc != 3) {
		fprintf(stderr, "usage: mount_machlin_btrfs DEVICE MOUNTPOINT\n");
		return 2;
	}
	arguments.fspec = argv[1];
	if (mount("machlin_btrfs", argv[2], MNT_RDONLY | MNT_NOSUID | MNT_NODEV, &arguments) != 0) {
		perror("mount_machlin_btrfs");
		return 1;
	}
	return 0;
}
