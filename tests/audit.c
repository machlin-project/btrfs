/* SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 200809L
#include "../adapters/posix/image.h"
#include "references.h"
#include <stdio.h>

/* Validates the independent reference audit on Linux-authored images before the
 * writer tests rely on it. */
int
main(int argc, char **argv)
{
	struct reference_audit audit;
	struct btrfs_image image;
	struct btrfs_fs *fs;
	int i;
	int failures = 0;

	for (i = 1; i < argc; i++) {
		if (btrfs_image_open(argv[i], &image) != 0) {
			fprintf(stderr, "%s: cannot open\n", argv[i]);
			return 1;
		}
		if (btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) != BTRFS_OK) {
			fprintf(stderr, "%s: mount failed\n", argv[i]);
			return 1;
		}
		if (reference_audit(fs, &audit) != 0) {
			fprintf(stderr, "%s: %s\n", argv[i], audit.failure);
			failures++;
		} else {
			printf("%s: %zu trees, %zu blocks, %zu data extents; refs tree %zu, shared "
			       "block "
			       "%zu, data %zu, shared data %zu, keyed %zu; %zu full-backref blocks "
			       "PASS\n",
			    argv[i], audit.trees, audit.blocks, audit.data_extents, audit.tree_refs,
			    audit.shared_block_refs, audit.data_refs, audit.shared_data_refs,
			    audit.keyed_refs, audit.full_backref_blocks);
		}
		btrfs_unmount(fs);
		btrfs_image_close(&image);
	}
	return failures != 0;
}
