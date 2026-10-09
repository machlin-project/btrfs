/* SPDX-License-Identifier: BSD-3-Clause */
/* Quota scenarios on the Linux image with qgroups (tests/prepare_linux.py):
 * 0/5, 0/256 (/subvol), 0/257 (/snapshot), 0/258 (/data, a 128 MiB referenced
 * limit) and its snapshots 0/259 (/data-snap, writable) and 0/260 (/data-ro),
 * with 0/258 and 0/259 in 1/100 (a 160 MiB exclusive limit). Every state passes
 * the independent qgroup audit, and Linux's btrfs check verifies the numbers
 * of every exported state. */
#include "scenario.h"

#define QUOTA_GROUPS 7U
#define QUOTA_FILE_BYTES (300U * 1024U)
#define QUOTA_FITS_BYTES (2U * 1024U * 1024U)
/* More than the 128 MiB limit of /data leaves, less than the free space. */
#define QUOTA_OVER_BYTES (140U * 1024U * 1024U)

/* A new subvolume gets its qgroup; deleted and dropped, the qgroup goes. */
static void
quota_subvolume_plan(struct context *context)
{
	static uint8_t data[QUOTA_FILE_BYTES];
	struct plan plan;

	fill_pattern(data, sizeof(data), 81);
	plan_init(&plan);
	/* An unchanged file gives every stage its generation. */
	plan_file(context, &plan, "/greeting");
	plan.name = "quota-subvolume";
	plan_subvolume(&plan, 1, "/q");
	plan_create(&plan, 1, "/q/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/q/file", 0, data, sizeof(data));
	plan_create(&plan, 2, "/data/quota", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 2, "/data/quota", 0, data, sizeof(data));
	plan_delete_subvolume(&plan, 3, "/q");
	plan_clean_subvolumes(&plan, 3, 4096, 1, 0);
	expect_file(&plan, 1, 2, "/q/file", data, sizeof(data));
	expect_file(&plan, 2, LAST_STAGE, "/data/quota", data, sizeof(data));
	expect_absent(&plan, 3, LAST_STAGE, "/q");
	expect_quota(&plan, 0, 0, 0, QUOTA_GROUPS);
	expect_quota(&plan, 1, 2, 0, QUOTA_GROUPS + 1);
	expect_quota(&plan, 3, LAST_STAGE, 0, QUOTA_GROUPS);
	run_plan(context, &plan);
}

/* Snapshots as btrfs_qgroup_inherit counts them: one into the top level, one
 * inside its own source, whose entry changes the source after the copy, then
 * files in both. */
static void
quota_snapshot_plan(struct context *context)
{
	static uint8_t data[QUOTA_FILE_BYTES];
	struct plan plan;

	fill_pattern(data, sizeof(data), 82);
	plan_init(&plan);
	/* An unchanged file gives every stage its generation. */
	plan_file(context, &plan, "/greeting");
	plan.name = "quota-snapshot";
	plan_snapshot(&plan, 1, "/subvol", "/subvol-copy", 0);
	plan_snapshot(&plan, 2, "/subvol", "/subvol/inner", 1);
	plan_create(&plan, 3, "/subvol/after", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 3, "/subvol/after", 0, data, sizeof(data));
	plan_create(&plan, 3, "/subvol-copy/after", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 3, "/subvol-copy/after", 0, data, sizeof(data));
	expect_subvolume(&plan, 1, LAST_STAGE, "/subvol-copy", "/subvol", 0);
	expect_subvolume(&plan, 2, LAST_STAGE, "/subvol/inner", "/subvol", 1);
	expect_text(&plan, 1, LAST_STAGE, "/subvol-copy/value", "subvolume changed\n");
	expect_file(&plan, 3, LAST_STAGE, "/subvol/after", data, sizeof(data));
	expect_file(&plan, 3, LAST_STAGE, "/subvol-copy/after", data, sizeof(data));
	expect_quota(&plan, 0, 0, 0, QUOTA_GROUPS);
	expect_quota(&plan, 1, 1, 0, QUOTA_GROUPS + 1);
	expect_quota(&plan, 2, LAST_STAGE, 0, QUOTA_GROUPS + 2);
	run_plan(context, &plan);
}

/* A snapshot of a subvolume in a higher qgroup leaves quotas inconsistent, as
 * Linux leaves them without an inherit request; later changes keep them so. */
static void
quota_inconsistent_plan(struct context *context)
{
	static uint8_t data[QUOTA_FILE_BYTES];
	struct plan plan;

	fill_pattern(data, sizeof(data), 83);
	plan_init(&plan);
	/* An unchanged file gives every stage its generation. */
	plan_file(context, &plan, "/greeting");
	plan.name = "quota-inconsistent";
	plan_snapshot(&plan, 1, "/data", "/data-copy", 0);
	plan_create(&plan, 2, "/data-copy/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 2, "/data-copy/file", 0, data, sizeof(data));
	expect_file(&plan, 2, LAST_STAGE, "/data-copy/file", data, sizeof(data));
	expect_quota(&plan, 0, 0, 0, QUOTA_GROUPS);
	expect_quota(&plan, 1, LAST_STAGE, 1, QUOTA_GROUPS + 1);
	run_plan(context, &plan);
}

/* Refusals before any change: a snapshot that is not its transaction's first
 * change, and a preallocation past the referenced limit of /data, which a
 * smaller write still fits. */
static void
quota_refusal_plan(struct context *context)
{
	static uint8_t data[QUOTA_FITS_BYTES];
	struct plan plan;

	fill_pattern(data, sizeof(data), 84);
	plan_init(&plan);
	/* An unchanged file gives every stage its generation. */
	plan_file(context, &plan, "/greeting");
	plan.name = "quota-refusals";
	plan_create(&plan, 1, "/order", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_snapshot(&plan, 1, "/subvol", "/late", 0);
	plan_expect_refusal(&plan, 1, BTRFS_UNSUPPORTED);
	plan_create(&plan, 1, "/data/fits", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/data/fits", 0, data, sizeof(data));
	plan_create(&plan, 1, "/data/over", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_fallocate_new(&plan, 1, "/data/over", 0, 0, QUOTA_OVER_BYTES);
	plan_expect_refusal(&plan, 1, BTRFS_QUOTA_EXCEEDED);
	expect_absent(&plan, 1, LAST_STAGE, "/late");
	expect_file(&plan, 1, LAST_STAGE, "/data/fits", data, sizeof(data));
	expect_file(&plan, 1, LAST_STAGE, "/data/over", NULL, 0);
	expect_quota(&plan, 0, LAST_STAGE, 0, QUOTA_GROUPS);
	run_plan(context, &plan);
}

/* Dropping snapshots that share their blocks: each shared subtree the drop
 * leaves is traced, the numbers of the trees that keep it change, and the
 * dropped subvolumes' qgroups go, one of them from 1/100. */
static void
quota_drop_plan(struct context *context)
{
	struct plan plan;

	plan_init(&plan);
	/* An unchanged file gives every stage its generation. */
	plan_file(context, &plan, "/greeting");
	plan.name = "quota-drop";
	plan_delete_subvolume(&plan, 1, "/data-ro");
	plan_delete_subvolume(&plan, 1, "/data-snap");
	plan_clean_subvolumes(&plan, 2, 4096, 2, 0);
	expect_absent(&plan, 1, LAST_STAGE, "/data-ro");
	expect_absent(&plan, 1, LAST_STAGE, "/data-snap");
	expect_deleted(&plan, 1, 1, 2);
	expect_deleted(&plan, 2, LAST_STAGE, 0);
	expect_quota(&plan, 0, 1, 0, QUOTA_GROUPS);
	expect_quota(&plan, 2, LAST_STAGE, 0, QUOTA_GROUPS - 2);
	run_plan(context, &plan);
}

void
quota_scenarios(struct context *context)
{
	quota_subvolume_plan(context);
	quota_snapshot_plan(context);
	quota_inconsistent_plan(context);
	quota_refusal_plan(context);
	quota_drop_plan(context);
}
