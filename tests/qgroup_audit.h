/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_TEST_QGROUP_AUDIT_H
#define MACHLIN_BTRFS_TEST_QGROUP_AUDIT_H

#include "internal.h"

struct qgroup_audit {
	/* Nonzero when the filesystem has a quota tree. */
	int quotas;
	/* Nonzero when its status leaves the numbers unchecked: inconsistent, a
	 * running rescan or a status generation that is not the filesystem's. */
	int skipped;
	size_t qgroups;
	size_t extents;
	size_t implied;
	char failure[256];
};

/* Independent qgroup check, computed as btrfs check's qgroup verify computes
 * it: every extent's roots from its references, where tree and data
 * references name a root and shared references lead to their parent's roots,
 * with an implied shared reference from each interior block of a subvolume
 * tree to every block and data extent below it; then referenced and exclusive
 * bytes for each qgroup and every qgroup above it, compared exactly with the
 * stored qgroup items. */
int qgroup_audit(const struct btrfs_fs *fs, struct qgroup_audit *audit);

#endif
