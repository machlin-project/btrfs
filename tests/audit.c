/* SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 200809L
#include "../adapters/posix/image.h"
#include "namespace_audit.h"
#include "references.h"
#include <stdio.h>

/* Validates the independent reference and namespace audits on Linux-authored
 * images before the writer tests rely on them. */
int
main(int argc, char **argv)
{
	struct reference_audit audit;
	struct namespace_audit names;
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
			printf(
			    "%s: %zu trees, %zu blocks, %zu data extents; refs tree %zu, shared "
			    "block "
			    "%zu, data %zu, shared data %zu, keyed %zu; %zu full-backref blocks; "
			    "%zu checksums PASS\n",
			    argv[i], audit.trees, audit.blocks, audit.data_extents, audit.tree_refs,
			    audit.shared_block_refs, audit.data_refs, audit.shared_data_refs,
			    audit.keyed_refs, audit.full_backref_blocks, audit.checksums);
		}
		if (namespace_audit(fs, &names) != 0) {
			fprintf(stderr, "%s: namespace: %s\n", argv[i], names.failure);
			failures++;
		} else {
			printf("%s: %zu file trees, %zu inodes, %zu names (%zu extended), %zu "
			       "subvolume entries, %zu collision items, %zu xattrs, %zu orphans "
			       "PASS\n",
			    argv[i], names.trees, names.inodes, names.names, names.extended_names,
			    names.subvolume_entries, names.collisions, names.xattrs, names.orphans);
		}
		btrfs_unmount(fs);
		btrfs_image_close(&image);
	}
	return failures != 0;
}
