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
/* Extent-tree items per rescan step: a part of the tree, and all of it. */
#define QUOTA_RESCAN_STEP 10U
#define QUOTA_RESCAN_ALL 1000000U

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

/* Linux's quota rescan after a snapshot left quotas inconsistent: refused in
 * the snapshot's transaction, begun with a partial step as the first change
 * of the next, which also writes files and takes another step, then finished;
 * a snapshot waits for it. While it runs, the counts cover exactly the extents
 * below its progress, which the audit checks; once it ends they are
 * consistent, which Linux's btrfs check verifies. */
static void
quota_rescan_plan(struct context *context)
{
	static uint8_t data[QUOTA_FILE_BYTES];
	struct plan plan;

	fill_pattern(data, sizeof(data), 85);
	plan_init(&plan);
	/* An unchanged file gives every stage its generation. */
	plan_file(context, &plan, "/greeting");
	plan.name = "quota-rescan";
	plan_snapshot(&plan, 1, "/data", "/data-copy", 0);
	plan_quota_rescan(&plan, 1, QUOTA_RESCAN_STEP, 0);
	plan_expect_refusal(&plan, 1, BTRFS_UNSUPPORTED);
	plan_quota_rescan(&plan, 2, QUOTA_RESCAN_STEP, 0);
	plan_create(&plan, 2, "/subvol/rescan", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 2, "/subvol/rescan", 0, data, sizeof(data));
	plan_create(&plan, 2, "/data-copy/rescan", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 2, "/data-copy/rescan", 0, data, sizeof(data));
	plan_quota_rescan(&plan, 2, QUOTA_RESCAN_STEP, 0);
	plan_snapshot(&plan, 3, "/subvol", "/late", 0);
	plan_expect_refusal(&plan, 3, BTRFS_UNSUPPORTED);
	plan_quota_rescan(&plan, 3, QUOTA_RESCAN_ALL, 1);
	plan_create(&plan, 3, "/data/after", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 3, "/data/after", 0, data, sizeof(data));
	expect_file(&plan, 2, LAST_STAGE, "/subvol/rescan", data, sizeof(data));
	expect_file(&plan, 2, LAST_STAGE, "/data-copy/rescan", data, sizeof(data));
	expect_file(&plan, 3, LAST_STAGE, "/data/after", data, sizeof(data));
	expect_absent(&plan, 3, LAST_STAGE, "/late");
	expect_quota(&plan, 0, 0, 0, QUOTA_GROUPS);
	expect_quota(&plan, 1, 1, 1, QUOTA_GROUPS + 1);
	expect_quota_rescan(&plan, 2, 2, QUOTA_GROUPS + 1);
	expect_quota(&plan, 3, LAST_STAGE, 0, QUOTA_GROUPS + 1);
	run_plan(context, &plan);
}

/* /data's qgroup and the qgroup above it, 1/100. */
#define QUOTA_DATA_QGROUP UINT64_C(258)
#define QUOTA_PARENT_QGROUP ((UINT64_C(1) << BT_QGROUP_LEVEL_SHIFT) | 100U)
/* Nodes Linux charges: a new file, a link, an xattr change, a shrinking
 * truncation, a preallocated extent, and what the plan leaves free. */
#define QUOTA_CREATE_NODES 4U
#define QUOTA_LINK_NODES 5U
#define QUOTA_XATTR_NODES 2U
#define QUOTA_TRUNCATE_NODES 2U
#define QUOTA_PREALLOC_NODES 3U
#define QUOTA_LEFT_NODES 2U

/* The bytes a qgroup of the fixture may still take under its limits. */
static uint64_t
quota_headroom(struct btrfs_fs *fs, uint64_t id)
{
	const struct bt_disk_qgroup_info *info;
	const struct bt_disk_qgroup_limit *limit;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	uint64_t referenced;
	uint64_t exclusive;
	uint64_t headroom = UINT64_MAX;

	REQUIRE(bt_find_root(fs, BT_QUOTA_TREE, &root) == BTRFS_OK);
	bt_cursor_init(&cursor, fs, root);
	REQUIRE(bt_cursor_seek(&cursor,
		    (struct bt_key){ .objectid = 0, .type = BT_QGROUP_INFO, .offset = id },
		    0) == BTRFS_OK &&
	    bt_cursor_record(&cursor, &record) == BTRFS_OK && record.key.offset == id &&
	    record.key.type == BT_QGROUP_INFO && record.size == sizeof(*info));
	info = (const void *)record.data;
	referenced = bt_u64(info->referenced);
	exclusive = bt_u64(info->exclusive);
	REQUIRE(bt_cursor_seek(&cursor,
		    (struct bt_key){ .objectid = 0, .type = BT_QGROUP_LIMIT, .offset = id },
		    0) == BTRFS_OK &&
	    bt_cursor_record(&cursor, &record) == BTRFS_OK && record.key.offset == id &&
	    record.key.type == BT_QGROUP_LIMIT && record.size == sizeof(*limit));
	limit = (const void *)record.data;
	if ((bt_u64(limit->flags) & BT_QGROUP_LIMIT_MAX_REFERENCED) != 0) {
		REQUIRE(bt_u64(limit->max_referenced) >= referenced);
		headroom = bt_u64(limit->max_referenced) - referenced;
	}
	if ((bt_u64(limit->flags) & BT_QGROUP_LIMIT_MAX_EXCLUSIVE) != 0) {
		REQUIRE(bt_u64(limit->max_exclusive) >= exclusive);
		if (bt_u64(limit->max_exclusive) - exclusive < headroom) {
			headroom = bt_u64(limit->max_exclusive) - exclusive;
		}
	}
	bt_cursor_fini(&cursor);
	return headroom;
}

/* Metadata reserved as Linux's btrfs_start_transaction reserves it counts
 * against limits within a transaction: a preallocation fills /data's
 * qgroups until two nodes remain; a new file and a link are then refused, an
 * xattr fits and takes the rest, a shrinking truncation is refused, and an
 * unlink, which Linux charges to the global reserve, still passes. */
static void
quota_metadata_plan(struct context *context)
{
	struct btrfs_fs *fs;
	struct plan plan;
	uint64_t node;
	uint64_t sector;
	uint64_t headroom;
	uint64_t fill;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	node = fs->info.node_size;
	sector = fs->info.sector_size;
	headroom = quota_headroom(fs, QUOTA_DATA_QGROUP);
	if (quota_headroom(fs, QUOTA_PARENT_QGROUP) < headroom) {
		headroom = quota_headroom(fs, QUOTA_PARENT_QGROUP);
	}
	btrfs_unmount(fs);
	/* The new file, its preallocation (one sector beyond the range and one
	 * extent's items) and what remains. */
	REQUIRE(headroom >
	    (QUOTA_CREATE_NODES + QUOTA_PREALLOC_NODES + QUOTA_LEFT_NODES) * node + sector);
	fill = headroom - (QUOTA_CREATE_NODES + QUOTA_PREALLOC_NODES + QUOTA_LEFT_NODES) * node -
	    sector;
	REQUIRE(fill % sector == 0 && fill <= BT_DATA_EXTENT - sector);
	REQUIRE(QUOTA_XATTR_NODES == QUOTA_LEFT_NODES && QUOTA_CREATE_NODES > QUOTA_LEFT_NODES &&
	    QUOTA_LINK_NODES > QUOTA_LEFT_NODES);
	plan_init(&plan);
	/* An unchanged file gives every stage its generation. */
	plan_file(context, &plan, "/greeting");
	plan.name = "quota-metadata";
	plan_create(&plan, 1, "/data/fill", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_fallocate_new(&plan, 1, "/data/fill", 0, 0, fill);
	plan_create(&plan, 1, "/data/denied", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_expect_refusal(&plan, 1, BTRFS_QUOTA_EXCEEDED);
	plan_link(&plan, 1, "/data/fill", "/data/fill-link");
	plan_expect_refusal(&plan, 1, BTRFS_QUOTA_EXCEEDED);
	plan_set_xattr(&plan, 1, "/data/fill", "user.fits", "x", 1, 0);
	plan_truncate_new(&plan, 1, "/data/fill", fill / 2);
	plan_expect_refusal(&plan, 1, BTRFS_QUOTA_EXCEEDED);
	plan_unlink(&plan, 1, "/data/fill", 0);
	expect_absent(&plan, 1, LAST_STAGE, "/data/fill");
	expect_absent(&plan, 1, LAST_STAGE, "/data/denied");
	expect_absent(&plan, 1, LAST_STAGE, "/data/fill-link");
	expect_quota(&plan, 0, LAST_STAGE, 0, QUOTA_GROUPS);
	run_plan(context, &plan);
}

/* Steps a maintenance test allows before its work must be done. */
#define QUOTA_MAINTENANCE_STEPS 1000U

/* The quota status flags and the deleted subvolumes a state holds. */
static void
quota_leftovers(struct context *context, uint64_t *flags, size_t *deleted)
{
	const struct bt_disk_qgroup_status *status;
	const struct bt_disk_root *item;
	struct btrfs_fs *fs;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	enum btrfs_result result;

	*flags = 0;
	*deleted = 0;
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(bt_find_root(fs, BT_QUOTA_TREE, &root) == BTRFS_OK);
	bt_cursor_init(&cursor, fs, root);
	result = bt_cursor_seek(
	    &cursor, (struct bt_key){ .objectid = 0, .type = BT_QGROUP_STATUS, .offset = 0 }, 0);
	REQUIRE(result == BTRFS_OK && bt_cursor_record(&cursor, &record) == BTRFS_OK &&
	    record.key.type == BT_QGROUP_STATUS && record.size >= sizeof(*status));
	status = (const void *)record.data;
	*flags = bt_u64(status->flags);
	bt_cursor_fini(&cursor);
	bt_cursor_init(&cursor, fs, fs->root_tree);
	result = bt_cursor_seek(&cursor, (struct bt_key){ 0 }, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		item = (const void *)record.data;
		*deleted += record.key.type == BT_ROOT_ITEM && record.size >= sizeof(*item) &&
		    bt_u32(item->refs) == 0;
		result = bt_cursor_next(&cursor);
	}
	REQUIRE(result == BTRFS_NOT_FOUND);
	bt_cursor_fini(&cursor);
	btrfs_unmount(fs);
}

/* The native volume's maintenance finishes Linux's background work in bounded
 * steps: the drop of a deleted read-only snapshot that shares its blocks, and
 * a rescan begun before it, until no work remains. The audits then pass,
 * quotas are consistent and no deleted subvolume is left. */
static void
quota_maintenance(struct context *context)
{
	struct btrfs_volume *volume;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct btrfs_time time = { 1800000000, 0 };
	uint64_t flags;
	size_t deleted;
	size_t steps;
	int pending = 1;
	int done = 0;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_quota_rescan(transaction, QUOTA_RESCAN_STEP, &done) == BTRFS_OK &&
	    !done);
	REQUIRE(btrfs_transaction_delete_subvolume(
		    transaction, object(fs, "/"), "data-ro", 7, time) == BTRFS_OK);
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	quota_leftovers(context, &flags, &deleted);
	REQUIRE((flags & BT_QGROUP_STATUS_RESCAN) != 0 && deleted == 1);
	REQUIRE(btrfs_volume_open(&context->env, &context->writer, &single_thread_locks,
		    BTRFS_TOP_LEVEL_TREE, &volume) == BTRFS_OK);
	for (steps = 0; pending; steps++) {
		REQUIRE(steps < QUOTA_MAINTENANCE_STEPS);
		REQUIRE(btrfs_volume_maintain(volume, &pending) == BTRFS_OK);
	}
	REQUIRE(btrfs_volume_sync(volume, btrfs_volume_pending(volume)) == BTRFS_OK);
	btrfs_volume_close(volume);
	audit_state(context, "quota-maintenance");
	quota_leftovers(context, &flags, &deleted);
	REQUIRE(flags == BT_QGROUP_STATUS_ON && deleted == 0);
	truncate_writes(context->device, 0);
	REQUIRE(context->image.live_allocations == 0);
	printf("native maintenance: a drop and a rescan finished in %zu steps PASS\n", steps);
}

/* Simple quotas on the Linux image whose quotas Linux enabled in simple mode
 * after the base payload and before the data payload, with the same qgroups,
 * relations and limits as the full-quota image. Each extent counts once, for
 * its owner, from the enabling generation on; Linux's btrfs check verifies the
 * numbers of every exported state, and so does the audit of every state. */

/* A subvolume and a snapshot of it inside /data join 1/100 as /data's qgroup
 * is in it; the subvolume deleted and dropped keeps its qgroup while the
 * snapshot still holds extents it owns. */
static void
squota_subvolume_plan(struct context *context)
{
	static uint8_t data[QUOTA_FILE_BYTES];
	struct plan plan;

	fill_pattern(data, sizeof(data), 91);
	plan_init(&plan);
	/* An unchanged file gives every stage its generation. */
	plan_file(context, &plan, "/greeting");
	plan.name = "squota-subvolume";
	plan_subvolume(&plan, 1, "/data/sv");
	plan_create(&plan, 1, "/data/sv/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/data/sv/file", 0, data, sizeof(data));
	plan_snapshot(&plan, 2, "/data/sv", "/data/sv-snap", 0);
	plan_create(&plan, 2, "/data/sv-snap/more", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 2, "/data/sv-snap/more", 0, data, sizeof(data));
	plan_delete_subvolume(&plan, 3, "/data/sv");
	plan_clean_subvolumes(&plan, 3, 4096, 1, 0);
	expect_file(&plan, 1, 2, "/data/sv/file", data, sizeof(data));
	expect_file(&plan, 2, LAST_STAGE, "/data/sv-snap/file", data, sizeof(data));
	expect_file(&plan, 2, LAST_STAGE, "/data/sv-snap/more", data, sizeof(data));
	expect_absent(&plan, 3, LAST_STAGE, "/data/sv");
	expect_quota(&plan, 0, 0, 0, QUOTA_GROUPS);
	expect_quota(&plan, 1, 1, 0, QUOTA_GROUPS + 1);
	expect_quota(&plan, 2, LAST_STAGE, 0, QUOTA_GROUPS + 2);
	run_plan(context, &plan);
}

/* A snapshot of /data in the top level joins no qgroup and counts what it
 * writes; a shared extent goes back from /data when the snapshot frees it
 * last, and an extent from before simple quotas changes nothing when freed.
 * Snapshots need not be a transaction's first change: an unchanged source
 * suffices. */
static void
squota_owner_plan(struct context *context)
{
	static uint8_t data[QUOTA_FILE_BYTES];
	struct plan plan;

	fill_pattern(data, sizeof(data), 92);
	plan_init(&plan);
	/* An unchanged file gives every stage its generation. */
	plan_file(context, &plan, "/greeting");
	plan.name = "squota-owner";
	plan_create(&plan, 1, "/before", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_snapshot(&plan, 1, "/data", "/copy", 0);
	plan_create(&plan, 2, "/copy/new", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 2, "/copy/new", 0, data, sizeof(data));
	plan_write_new(&plan, 2, "/copy/big", 4096, data, sizeof(data));
	plan_unlink(&plan, 2, "/data/small", 0);
	plan_unlink(&plan, 3, "/copy/small", 0);
	plan_unlink(&plan, 3, "/big", 0);
	expect_absent(&plan, 2, LAST_STAGE, "/data/small");
	expect_absent(&plan, 3, LAST_STAGE, "/copy/small");
	expect_absent(&plan, 3, LAST_STAGE, "/big");
	expect_file(&plan, 2, LAST_STAGE, "/copy/new", data, sizeof(data));
	expect_quota(&plan, 0, 0, 0, QUOTA_GROUPS);
	expect_quota(&plan, 1, LAST_STAGE, 0, QUOTA_GROUPS + 1);
	run_plan(context, &plan);
}

/* Refusals before any change: a rescan, which simple quotas have no use for,
 * and a preallocation past /data's referenced limit. Deleted snapshots that
 * owned only their own blocks lose their qgroups, one of them in 1/100. */
static void
squota_drop_plan(struct context *context)
{
	struct plan plan;

	plan_init(&plan);
	/* An unchanged file gives every stage its generation. */
	plan_file(context, &plan, "/greeting");
	plan.name = "squota-drop";
	plan_quota_rescan(&plan, 1, QUOTA_RESCAN_STEP, 0);
	plan_expect_refusal(&plan, 1, BTRFS_INVALID_ARGUMENT);
	plan_create(&plan, 1, "/data/over", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_fallocate_new(&plan, 1, "/data/over", 0, 0, QUOTA_OVER_BYTES);
	plan_expect_refusal(&plan, 1, BTRFS_QUOTA_EXCEEDED);
	plan_delete_subvolume(&plan, 2, "/data-ro");
	plan_delete_subvolume(&plan, 2, "/data-snap");
	plan_clean_subvolumes(&plan, 3, 4096, 2, 0);
	expect_file(&plan, 1, LAST_STAGE, "/data/over", NULL, 0);
	expect_absent(&plan, 2, LAST_STAGE, "/data-ro");
	expect_absent(&plan, 2, LAST_STAGE, "/data-snap");
	expect_quota(&plan, 0, 2, 0, QUOTA_GROUPS);
	expect_quota(&plan, 3, LAST_STAGE, 0, QUOTA_GROUPS - 2);
	run_plan(context, &plan);
}

void
squota_scenarios(struct context *context)
{
	squota_subvolume_plan(context);
	squota_owner_plan(context);
	squota_drop_plan(context);
}

void
quota_scenarios(struct context *context)
{
	quota_subvolume_plan(context);
	quota_snapshot_plan(context);
	quota_inconsistent_plan(context);
	quota_refusal_plan(context);
	quota_drop_plan(context);
	quota_rescan_plan(context);
	quota_maintenance(context);
	quota_metadata_plan(context);
}
