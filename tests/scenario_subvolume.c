/* SPDX-License-Identifier: BSD-3-Clause */
/* Subvolume and snapshot scenarios on the namespace fixture, where Linux made
 * the subvolume /subvol and its read-only snapshot /snapshot. */
#define _POSIX_C_SOURCE 200809L
#include "scenario.h"

#define COMPRESSION_PROPERTY "btrfs.compression"
#define VALUE_BYTES 18U
#define DROP_DATA_BYTES 100000U
#define DROP_FILES 120U
#define DROP_FILE_BYTES 3000U
#define DROP_UNSHARED_FILES 40U

static const char subvol_value[] = "subvolume changed\n";
static const char source_value[] = "subvolume CHANGED\n";
static const char snapshot_value[] = "snapshot  edited!\n";

_Static_assert(sizeof(subvol_value) - 1 == VALUE_BYTES, "fixture value length");
_Static_assert(sizeof(source_value) - 1 == VALUE_BYTES, "source value length");
_Static_assert(sizeof(snapshot_value) - 1 == VALUE_BYTES, "snapshot value length");

static struct btrfs_new_inode
subvolume_inode(void)
{
	struct btrfs_new_inode attributes;

	memset(&attributes, 0, sizeof(attributes));
	attributes.mode = BTRFS_MODE_DIRECTORY | 0755;
	attributes.uid = NAMESPACE_UID;
	attributes.gid = NAMESPACE_GID;
	attributes.time.seconds = 1800000000;
	return attributes;
}

static void
refused(struct btrfs_transaction *transaction, enum btrfs_result result, enum btrfs_result expected,
    const char *what)
{
	if (result != expected) {
		fprintf(stderr, "%s: %s, expected %s\n", what, btrfs_result_string(result),
		    btrfs_result_string(expected));
		exit(1);
	}
	REQUIRE(transaction->failure == BTRFS_OK);
}

/* New subvolumes, nested ones, files inside them, and the compression
 * property each inherits from its parent subvolume's root directory rather
 * than from the directory that holds it. */
static void
subvolume_create_plan(struct context *context)
{
	static const char *const root_added[] = { "sv1", NULL };
	static const char *const ns_added[] = { "sv2", NULL };
	static const char *const zstd_added[] = { "sv3", NULL };
	const char *first[] = { "snapshot", "subvol", "sv1", "ns/sv2", NULL };
	const char *later[] = { "snapshot", "subvol", "sv1", "ns/sv2", "sv1/inner", "ns/zstd/sv3",
		NULL };
	const char *fixture[] = { "snapshot", "subvol", NULL };
	const char *sv1[] = { "file", "inner" };
	const char *inner[] = { "deep" };
	uint8_t data[5000];
	struct plan plan;

	fill_pattern(data, sizeof(data), 61);
	namespace_plan(context, &plan, "subvolume-create");
	plan_subvolume(&plan, 1, "/sv1");
	plan_subvolume(&plan, 1, "/ns/sv2");
	plan_set_xattr(&plan, 1, "/", COMPRESSION_PROPERTY, "zlib", 4, 0);
	plan_create(&plan, 2, "/sv1/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 2, "/sv1/file", 0, data, sizeof(data));
	plan_subvolume(&plan, 2, "/sv1/inner");
	plan_subvolume(&plan, 2, "/ns/zstd/sv3");
	plan_create(&plan, 3, "/sv1/inner/deep", BTRFS_MODE_DIRECTORY | 0700, NULL);
	/* A file stays in its subvolume, and unlink does not remove a subvolume. */
	plan_rename(&plan, 3, "/sv1/file", "/moved", 0);
	plan_expect_refusal(&plan, 3, BTRFS_CROSS_TREE);
	plan_unlink(&plan, 3, "/ns/sv2", 0);
	plan_expect_refusal(&plan, 3, BTRFS_CROSS_TREE);

	expect_listing(context, &plan, 1, LAST_STAGE, "/", root_added, NULL);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns", ns_added, NULL);
	expect_listing(context, &plan, 2, LAST_STAGE, "/ns/zstd", zstd_added, NULL);
	expect_subvolume(&plan, 1, LAST_STAGE, "/sv1", NULL, 0);
	expect_subvolume(&plan, 1, LAST_STAGE, "/ns/sv2", NULL, 0);
	expect_subvolume(&plan, 2, LAST_STAGE, "/sv1/inner", NULL, 0);
	expect_subvolume(&plan, 2, LAST_STAGE, "/ns/zstd/sv3", NULL, 0);
	expect_owner(&plan, 1, LAST_STAGE, "/sv1", BTRFS_MODE_DIRECTORY | 0755, NAMESPACE_UID,
	    NAMESPACE_GID, 1);
	expect_names(&plan, 1, 1, "/sv1", NULL, 0);
	expect_names(&plan, 2, LAST_STAGE, "/sv1", sv1, 2);
	expect_names(&plan, 2, 2, "/sv1/inner", NULL, 0);
	expect_names(&plan, 3, LAST_STAGE, "/sv1/inner", inner, 1);
	expect_file(&plan, 2, LAST_STAGE, "/sv1/file", data, sizeof(data));
	expect_xattr(&plan, 1, LAST_STAGE, "/sv1", COMPRESSION_PROPERTY, NULL, 0);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/zstd/sv3", COMPRESSION_PROPERTY, "zlib", 4);
	expect_flags(&plan, 2, LAST_STAGE, "/ns/zstd/sv3", BT_INODE_COMPRESS | BT_INODE_NOCOMPRESS,
	    BT_INODE_COMPRESS);
	expect_flags(&plan, 1, LAST_STAGE, "/sv1", BT_INODE_COMPRESS | BT_INODE_NOCOMPRESS, 0);
	expect_subvolumes(&plan, 0, 0, fixture);
	expect_subvolumes(&plan, 1, 1, first);
	expect_subvolumes(&plan, 2, LAST_STAGE, later);
	run_plan(context, &plan);
}

/* Writable and read-only snapshots of a subvolume, of the top level (from a
 * leaf root and from a multi-level tree) and of snapshots; edits on either
 * side stay isolated, and copied subvolume entries become empty stubs. */
static void
subvolume_snapshot_plan(struct context *context)
{
	static const char *const root_first[] = { "snap-rw", "top-snap", NULL };
	static const char *const root_third[] = { "snap-rw", "top-snap", "source-only", "snap-rw2",
		NULL };
	static const char *const root_second[] = { "snap-rw", "top-snap", "source-only", NULL };
	static const char *const ns_added[] = { "snap-ro", NULL };
	static const char *const ns_third[] = { "snap-ro", "snap-ro-rw", NULL };
	static const char *const top_added[] = { "only-in-snap", NULL };
	static const char *const top_removed[] = { "random", NULL };
	static const char *const snap_added[] = { "new", NULL };
	static const char *const snap_third[] = { "new", "sub", NULL };
	const char *fixture[] = { "snapshot", "subvol", NULL };
	const char *first[] = { "snapshot", "subvol", "snap-rw", "ns/snap-ro", "top-snap", NULL };
	const char *third[] = { "snapshot", "subvol", "snap-rw", "ns/snap-ro", "top-snap",
		"snap-rw2", "ns/snap-ro-rw", "snap-rw/sub", NULL };
	struct plan plan;

	namespace_plan(context, &plan, "subvolume-snapshot");
	/* The top level first: a source changed earlier in the transaction is
	 * refused. */
	plan_snapshot(&plan, 1, "/", "/top-snap", 0);
	plan_snapshot(&plan, 1, "/subvol", "/snap-rw", 0);
	plan_snapshot(&plan, 1, "/subvol", "/ns/snap-ro", 1);
	/* /subvol/value keeps its Linux contents in every state (a suite
	 * invariant); a new file changes the source's shared leaf instead. */
	plan_create(&plan, 2, "/subvol/source-new", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 2, "/subvol/source-new", 0, source_value, VALUE_BYTES);
	plan_write_new(&plan, 2, "/snap-rw/value", 0, snapshot_value, VALUE_BYTES);
	plan_create(&plan, 2, "/snap-rw/new", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 2, "/ns/snap-ro/value", 0, snapshot_value, VALUE_BYTES);
	plan_expect_refusal(&plan, 2, BTRFS_READ_ONLY);
	plan_create(&plan, 2, "/top-snap/only-in-snap", BTRFS_MODE_REGULAR | 0600, NULL);
	plan_unlink(&plan, 2, "/top-snap/random", 0);
	plan_create(&plan, 2, "/source-only", BTRFS_MODE_REGULAR | 0600, NULL);
	plan_snapshot(&plan, 3, "/snap-rw", "/snap-rw2", 0);
	plan_snapshot(&plan, 3, "/ns/snap-ro", "/ns/snap-ro-rw", 0);
	plan_subvolume(&plan, 3, "/snap-rw/sub");

	expect_listing(context, &plan, 1, 1, "/", root_first, NULL);
	expect_listing(context, &plan, 2, 2, "/", root_second, NULL);
	expect_listing(context, &plan, 3, LAST_STAGE, "/", root_third, NULL);
	expect_listing(context, &plan, 1, 2, "/ns", ns_added, NULL);
	expect_listing(context, &plan, 3, LAST_STAGE, "/ns", ns_third, NULL);
	/* The snapshot of the top level holds no entry of itself. */
	expect_listing_from(context, &plan, 1, 1, "/top-snap", "/", NULL, NULL);
	expect_listing_from(
	    context, &plan, 2, LAST_STAGE, "/top-snap", "/", top_added, top_removed);
	expect_listing_from(context, &plan, 1, 1, "/snap-rw", "/subvol", NULL, NULL);
	expect_listing_from(context, &plan, 2, 2, "/snap-rw", "/subvol", snap_added, NULL);
	expect_listing_from(context, &plan, 3, LAST_STAGE, "/snap-rw", "/subvol", snap_third, NULL);
	expect_listing_from(
	    context, &plan, 3, LAST_STAGE, "/snap-rw2", "/subvol", snap_added, NULL);
	/* Subvolume entries copied into a snapshot are empty stubs. */
	expect_names(&plan, 1, LAST_STAGE, "/top-snap/subvol", NULL, 0);
	expect_owner(
	    &plan, 1, LAST_STAGE, "/top-snap/subvol", BTRFS_MODE_DIRECTORY | 0755, 0, 0, 1);
	expect_names(&plan, 1, LAST_STAGE, "/top-snap/snapshot", NULL, 0);
	expect_subvolume(&plan, 1, LAST_STAGE, "/snap-rw", "/subvol", 0);
	expect_subvolume(&plan, 1, LAST_STAGE, "/ns/snap-ro", "/subvol", 1);
	expect_subvolume(&plan, 1, LAST_STAGE, "/top-snap", "/", 0);
	expect_subvolume(&plan, 3, LAST_STAGE, "/snap-rw2", "/snap-rw", 0);
	expect_subvolume(&plan, 3, LAST_STAGE, "/ns/snap-ro-rw", "/ns/snap-ro", 0);
	expect_subvolume(&plan, 3, LAST_STAGE, "/snap-rw/sub", NULL, 0);
	expect_text(&plan, 0, LAST_STAGE, "/subvol/value", subvol_value);
	expect_text(&plan, 2, LAST_STAGE, "/subvol/source-new", source_value);
	expect_absent(&plan, 2, LAST_STAGE, "/snap-rw/source-new");
	expect_absent(&plan, 2, LAST_STAGE, "/ns/snap-ro/source-new");
	expect_text(&plan, 1, 1, "/snap-rw/value", subvol_value);
	expect_text(&plan, 2, LAST_STAGE, "/snap-rw/value", snapshot_value);
	expect_text(&plan, 3, LAST_STAGE, "/snap-rw2/value", snapshot_value);
	expect_text(&plan, 1, LAST_STAGE, "/ns/snap-ro/value", subvol_value);
	expect_text(&plan, 3, LAST_STAGE, "/ns/snap-ro-rw/value", subvol_value);
	expect_current(context, &plan, 1, LAST_STAGE, "/top-snap/greeting", "/greeting");
	expect_current(context, &plan, 1, 1, "/top-snap/random", "/random");
	expect_absent(&plan, 2, LAST_STAGE, "/top-snap/random");
	expect_current(context, &plan, 0, LAST_STAGE, "/random", "/random");
	expect_absent(&plan, 2, LAST_STAGE, "/top-snap/source-only");
	expect_subvolumes(&plan, 0, 0, fixture);
	expect_subvolumes(&plan, 1, 2, first);
	expect_subvolumes(&plan, 3, LAST_STAGE, third);
	run_plan(context, &plan);
}

/* Deleted subvolumes, a stub entry removed alone, and the cleaner resuming a
 * partial drop across commits. Budgets count visited blocks: a 120-file
 * subvolume has one unshared leaf per step, so its drop records progress after
 * each; the final state leaves it partly dropped, with three more deleted
 * subvolumes, for Linux's cleaner to finish in the oracle. */
static void
subvolume_delete_plan(struct context *context)
{
	static const char *const subvol_first[] = { "many-sv", "victim1", "empty-sv", "top2",
		"big-snap", NULL };
	static const char *const subvol_later[] = { "top2", NULL };
	static const char *const top_removed[] = { "subvol", NULL };
	const char *fixture[] = { "snapshot", "subvol", NULL };
	const char *first[] = { "snapshot", "subvol", "subvol/many-sv", "subvol/victim1",
		"subvol/empty-sv", "subvol/top2", "subvol/big-snap", NULL };
	const char *later[] = { "snapshot", "subvol", "subvol/top2", NULL };
	char path[64];
	uint8_t file[DROP_FILE_BYTES];
	uint8_t *data;
	struct plan plan;
	unsigned i;

	data = malloc(DROP_DATA_BYTES);
	REQUIRE(data != NULL);
	fill_pattern(data, DROP_DATA_BYTES, 71);
	namespace_plan(context, &plan, "subvolume-delete");
	/* Entries in /subvol keep the top level unchanged for its snapshots;
	 * the cleaner takes deleted subvolumes in id order. */
	plan_subvolume(&plan, 1, "/subvol/many-sv");
	for (i = 0; i < DROP_FILES; i++) {
		REQUIRE(
		    snprintf(path, sizeof(path), "/subvol/many-sv/f%03u", i) < (int)sizeof(path));
		fill_pattern(file, sizeof(file), i);
		plan_create(&plan, 1, path, BTRFS_MODE_REGULAR | 0644, NULL);
		plan_write_new(&plan, 1, path, 0, file, sizeof(file));
	}
	plan_subvolume(&plan, 1, "/subvol/victim1");
	plan_subvolume(&plan, 1, "/subvol/empty-sv");
	plan_snapshot(&plan, 1, "/", "/subvol/top2", 0);
	plan_snapshot(&plan, 1, "/", "/subvol/big-snap", 0);
	plan_create(&plan, 1, "/subvol/big-snap/data-only", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/subvol/big-snap/data-only", 0, data, DROP_DATA_BYTES);
	plan_delete_subvolume(&plan, 2, "/subvol/many-sv");
	plan_delete_subvolume(&plan, 2, "/subvol/victim1");
	plan_delete_subvolume(&plan, 2, "/subvol/empty-sv");
	plan_delete_subvolume(&plan, 2, "/subvol/big-snap");
	plan_delete_subvolume(&plan, 2, "/subvol/top2/subvol");
	plan_delete_subvolume(&plan, 2, "/subvol");
	plan_expect_refusal(&plan, 2, BTRFS_NOT_EMPTY);
	plan_delete_subvolume(&plan, 2, "/ns");
	plan_expect_refusal(&plan, 2, BTRFS_INVALID_ARGUMENT);
	plan_delete_subvolume(&plan, 2, "/missing");
	plan_expect_refusal(&plan, 2, BTRFS_NOT_FOUND);
	/* The root and two leaves of many-sv per commit. */
	plan_clean_subvolumes(&plan, 2, 3, 0, 1);
	plan_clean_subvolumes(&plan, 3, 3, 0, 1);

	expect_listing(context, &plan, 1, 1, "/subvol", subvol_first, NULL);
	expect_listing(context, &plan, 2, LAST_STAGE, "/subvol", subvol_later, NULL);
	expect_listing_from(context, &plan, 1, 1, "/subvol/top2", "/", NULL, NULL);
	expect_listing_from(context, &plan, 2, LAST_STAGE, "/subvol/top2", "/", NULL, top_removed);
	expect_names(&plan, 1, 1, "/subvol/top2/subvol", NULL, 0);
	expect_absent(&plan, 2, LAST_STAGE, "/subvol/top2/subvol");
	expect_absent(&plan, 2, LAST_STAGE, "/subvol/big-snap");
	expect_absent(&plan, 2, LAST_STAGE, "/subvol/many-sv");
	expect_file(&plan, 1, 1, "/subvol/big-snap/data-only", data, DROP_DATA_BYTES);
	expect_subvolume(&plan, 1, LAST_STAGE, "/subvol/top2", "/", 0);
	expect_subvolumes(&plan, 0, 0, fixture);
	expect_subvolumes(&plan, 1, 1, first);
	expect_subvolumes(&plan, 2, LAST_STAGE, later);
	expect_deleted(&plan, 0, 1, 0);
	expect_deleted(&plan, 2, LAST_STAGE, 4);
	run_plan(context, &plan);
	free(data);
}

/* A subvolume whose leaves a snapshot shares: deleting it converts every
 * shared block it owns to parent references before its own references go
 * (Linux's UPDATE_BACKREF stage); the snapshot keeps its files. An unshared
 * subvolume dropped with it frees its leaves, file extents and data. */
static void
subvolume_drop_shared_plan(struct context *context)
{
	const char *fixture[] = { "snapshot", "subvol", NULL };
	const char *first[] = { "snapshot", "subvol", "sv-d", "sv-u", NULL };
	const char *second[] = { "snapshot", "subvol", "sv-d", "sv-u", "sv-d-snap", NULL };
	const char *third[] = { "snapshot", "subvol", "sv-d-snap", NULL };
	char path[64];
	uint8_t data[DROP_FILE_BYTES];
	struct plan plan;
	unsigned i;

	namespace_plan(context, &plan, "subvolume-drop-shared");
	plan_subvolume(&plan, 1, "/sv-d");
	for (i = 0; i < DROP_FILES; i++) {
		REQUIRE(snprintf(path, sizeof(path), "/sv-d/f%03u", i) < (int)sizeof(path));
		fill_pattern(data, sizeof(data), i);
		plan_create(&plan, 1, path, BTRFS_MODE_REGULAR | 0644, NULL);
		plan_write_new(&plan, 1, path, 0, data, sizeof(data));
	}
	/* An unshared subvolume: its leaves' file extents and data go too. */
	plan_subvolume(&plan, 1, "/sv-u");
	for (i = 0; i < DROP_UNSHARED_FILES; i++) {
		REQUIRE(snprintf(path, sizeof(path), "/sv-u/u%03u", i) < (int)sizeof(path));
		fill_pattern(data, sizeof(data), 1000 + i);
		plan_create(&plan, 1, path, BTRFS_MODE_REGULAR | 0644, NULL);
		plan_write_new(&plan, 1, path, 0, data, sizeof(data));
	}
	plan_snapshot(&plan, 2, "/sv-d", "/sv-d-snap", 0);
	plan_delete_subvolume(&plan, 3, "/sv-d");
	plan_delete_subvolume(&plan, 3, "/sv-u");
	plan_clean_subvolumes(&plan, 3, 4096, 2, 0);

	for (i = 0; i < DROP_FILES; i += DROP_FILES / 4) {
		fill_pattern(data, sizeof(data), i);
		REQUIRE(snprintf(path, sizeof(path), "/sv-d/f%03u", i) < (int)sizeof(path));
		expect_file(&plan, 1, 2, path, data, sizeof(data));
		REQUIRE(snprintf(path, sizeof(path), "/sv-d-snap/f%03u", i) < (int)sizeof(path));
		expect_file(&plan, 2, LAST_STAGE, path, data, sizeof(data));
	}
	expect_absent(&plan, 3, LAST_STAGE, "/sv-d");
	expect_absent(&plan, 3, LAST_STAGE, "/sv-u");
	expect_subvolume(&plan, 2, 2, "/sv-d-snap", "/sv-d", 0);
	expect_subvolumes(&plan, 0, 0, fixture);
	expect_subvolumes(&plan, 1, 1, first);
	expect_subvolumes(&plan, 2, 2, second);
	expect_subvolumes(&plan, 3, LAST_STAGE, third);
	expect_deleted(&plan, 0, LAST_STAGE, 0);
	run_plan(context, &plan);
}

/* Subvolume entries renamed and exchanged as btrfs_rename and
 * btrfs_rename_exchange move them: within a directory, to another directory
 * of the same subvolume and into another subvolume, in the transaction that
 * made them, over an empty directory or a stub, exchanged with each other
 * across subvolumes and with an inode of their subvolume; the refusals
 * Linux makes are decided before any change. */
static void
subvolume_rename_plan(struct context *context)
{
	static const char *const root_first[] = { "top-snap", "ro-snap", "sv-a", "plain", "dir-x",
		NULL };
	static const char *const root_later[] = { "top-snap", "ro-snap2", "plain", "dir-x", NULL };
	static const char *const ns_first[] = { "moved", "fresh", "sv-b", "sv-c", NULL };
	static const char *const ns_second[] = { "moved", "fresh", "sv-a2", "sv-c", NULL };
	static const char *const ns_third[] = { "sv-a2", "sv-c", NULL };
	static const char *const subvol_added[] = { "sv-b", NULL };
	static const char *const top_added[] = { "file", NULL };
	const char *fixture[] = { "snapshot", "subvol", NULL };
	const char *first[] = { "snapshot", "subvol", "top-snap", "ro-snap", "ns/moved", "ns/fresh",
		"sv-a", "ns/sv-b", "ns/sv-b/nested", "ns/sv-c", NULL };
	const char *second[] = { "snapshot", "subvol", "top-snap", "ro-snap2", "ns/moved",
		"ns/fresh", "ns/sv-a2", "subvol/sv-b", "subvol/sv-b/nested", "ns/sv-c", NULL };
	const char *third[] = { "snapshot", "subvol", "top-snap", "plain", "ns/empty",
		"subvol/sv-b", "ns/sv-a2", "ns/sv-a2/nested", "ns/sv-a2/inner", "dir-x", NULL };
	const char *sv_a[] = { "file" };
	const char *sv_b[] = { "inner", "nested" };
	const char *kept[] = { "kept" };
	uint8_t data[5000];
	struct plan plan;

	fill_pattern(data, sizeof(data), 62);
	namespace_plan(context, &plan, "subvolume-rename");
	/* The top level is snapshotted before it changes; its subvolume entries
	 * become stubs in the copy. */
	plan_snapshot(&plan, 1, "/", "/top-snap", 0);
	plan_snapshot(&plan, 1, "/subvol", "/ro-snap", 1);
	/* Renamed in the transaction that made them: a snapshot's entry then
	 * names its root item key, which records the transaction. */
	plan_snapshot(&plan, 1, "/subvol", "/moving", 0);
	plan_rename(&plan, 1, "/moving", "/ns/moved", 0);
	plan_subvolume(&plan, 1, "/fresh");
	plan_rename(&plan, 1, "/fresh", "/ns/fresh", 0);
	plan_subvolume(&plan, 1, "/sv-a");
	plan_create(&plan, 1, "/sv-a/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/sv-a/file", 0, data, sizeof(data));
	plan_subvolume(&plan, 1, "/ns/sv-b");
	plan_create(&plan, 1, "/ns/sv-b/inner", BTRFS_MODE_DIRECTORY | 0755, NULL);
	plan_subvolume(&plan, 1, "/ns/sv-b/nested");
	plan_subvolume(&plan, 1, "/ns/sv-c");
	plan_create(&plan, 1, "/plain", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/plain", 0, subvol_value, VALUE_BYTES);
	plan_create(&plan, 1, "/dir-x", BTRFS_MODE_DIRECTORY | 0700, NULL);
	plan_create(&plan, 1, "/dir-x/kept", BTRFS_MODE_REGULAR | 0600, NULL);

	/* To another directory and into another subvolume; a read-only
	 * subvolume only within its directory. */
	plan_rename(&plan, 2, "/sv-a", "/ns/sv-a2", 0);
	plan_rename(&plan, 2, "/ns/sv-b", "/subvol/sv-b", 0);
	plan_rename(&plan, 2, "/ro-snap", "/ro-snap2", 0);
	plan_rename(&plan, 2, "/ns/sv-c", "/ns/sv-c", 0);
	plan_rename(&plan, 2, "/ro-snap2", "/ns/ro", 0);
	plan_expect_refusal(&plan, 2, BTRFS_READ_ONLY);
	plan_rename(&plan, 2, "/ns/sv-c", "/ns/full", 0);
	plan_expect_refusal(&plan, 2, BTRFS_NOT_EMPTY);
	plan_rename(&plan, 2, "/ns/sv-c", "/plain", 0);
	plan_expect_refusal(&plan, 2, BTRFS_NOT_DIRECTORY);
	plan_rename(&plan, 2, "/plain", "/ns/sv-c", 0);
	plan_expect_refusal(&plan, 2, BTRFS_IS_DIRECTORY);
	plan_rename(&plan, 2, "/ns/full", "/ns/sv-c", 0);
	plan_expect_refusal(&plan, 2, BTRFS_NOT_EMPTY);
	plan_rename(&plan, 2, "/ns/sv-c", "/subvol", 0);
	plan_expect_refusal(&plan, 2, BTRFS_NOT_EMPTY);
	plan_rename(&plan, 2, "/ns/sv-c", "/ns/sv-c/below", 0);
	plan_expect_refusal(&plan, 2, BTRFS_INVALID_ARGUMENT);
	plan_rename(&plan, 2, "/ns/one", "/subvol/one", 0);
	plan_expect_refusal(&plan, 2, BTRFS_CROSS_TREE);
	/* A stub does not move and takes no names; a directory replaces it. */
	plan_create(&plan, 2, "/top-snap/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_create(&plan, 2, "/top-snap/dir", BTRFS_MODE_DIRECTORY | 0700, NULL);
	plan_rename(&plan, 2, "/top-snap/snapshot", "/top-snap/stub", 0);
	plan_expect_refusal(&plan, 2, BTRFS_NOT_EMPTY);
	plan_rename(&plan, 2, "/top-snap/file", "/top-snap/snapshot/file", 0);
	plan_expect_refusal(&plan, 2, BTRFS_NOT_PERMITTED);
	plan_rename(&plan, 2, "/top-snap/file", "/top-snap/subvol", 0);
	plan_expect_refusal(&plan, 2, BTRFS_IS_DIRECTORY);
	plan_rename(&plan, 2, "/top-snap/dir", "/top-snap/subvol", 0);

	/* Refusals first: a subvolume below itself, two levels down, and
	 * exchanges Linux refuses. */
	plan_rename(&plan, 3, "/subvol/sv-b", "/subvol/sv-b/nested/below", 0);
	plan_expect_refusal(&plan, 3, BTRFS_INVALID_ARGUMENT);
	plan_rename(&plan, 3, "/subvol", "/subvol/sv-b/nested/below", 0);
	plan_expect_refusal(&plan, 3, BTRFS_INVALID_ARGUMENT);
	plan_exchange(&plan, 3, "/subvol/sv-b", "/subvol/sv-b/nested");
	plan_expect_refusal(&plan, 3, BTRFS_INVALID_ARGUMENT);
	plan_exchange(&plan, 3, "/ns/one", "/subvol/sv-b");
	plan_expect_refusal(&plan, 3, BTRFS_CROSS_TREE);
	plan_exchange(&plan, 3, "/top-snap/snapshot", "/top-snap/subvol");
	plan_expect_refusal(&plan, 3, BTRFS_NOT_EMPTY);
	plan_exchange(&plan, 3, "/ro-snap2", "/ns/fresh");
	plan_expect_refusal(&plan, 3, BTRFS_READ_ONLY);
	/* Over an empty directory, in its directory and in another subvolume. */
	plan_rename(&plan, 3, "/ns/fresh", "/ns/empty", 0);
	plan_rename(&plan, 3, "/ns/moved", "/subvol/sv-b/inner", 0);
	/* Exchanges: two subvolumes across subvolumes, then a subvolume with a
	 * file in its directory and with a directory in another one. */
	plan_exchange(&plan, 3, "/ns/sv-a2", "/subvol/sv-b");
	plan_exchange(&plan, 3, "/plain", "/ro-snap2");
	plan_exchange(&plan, 3, "/dir-x", "/ns/sv-c");

	expect_listing(context, &plan, 1, 1, "/", root_first, NULL);
	expect_listing(context, &plan, 2, LAST_STAGE, "/", root_later, NULL);
	expect_listing(context, &plan, 1, 1, "/ns", ns_first, NULL);
	expect_listing(context, &plan, 2, 2, "/ns", ns_second, NULL);
	expect_listing(context, &plan, 3, LAST_STAGE, "/ns", ns_third, NULL);
	expect_listing(context, &plan, 2, LAST_STAGE, "/subvol", subvol_added, NULL);
	expect_listing_from(context, &plan, 2, LAST_STAGE, "/top-snap", "/", top_added, NULL);
	expect_names(&plan, 1, 1, "/sv-a", sv_a, 1);
	expect_names(&plan, 2, 2, "/ns/sv-a2", sv_a, 1);
	expect_names(&plan, 3, LAST_STAGE, "/subvol/sv-b", sv_a, 1);
	expect_names(&plan, 1, 1, "/ns/sv-b", sv_b, 2);
	expect_names(&plan, 2, 2, "/subvol/sv-b", sv_b, 2);
	expect_names(&plan, 3, LAST_STAGE, "/ns/sv-a2", sv_b, 2);
	expect_names(&plan, 1, 2, "/dir-x", kept, 1);
	expect_names(&plan, 3, LAST_STAGE, "/ns/sv-c", kept, 1);
	expect_names(&plan, 3, LAST_STAGE, "/dir-x", NULL, 0);
	expect_file(&plan, 1, 1, "/sv-a/file", data, sizeof(data));
	expect_file(&plan, 2, 2, "/ns/sv-a2/file", data, sizeof(data));
	expect_file(&plan, 3, LAST_STAGE, "/subvol/sv-b/file", data, sizeof(data));
	expect_text(&plan, 1, 2, "/plain", subvol_value);
	expect_text(&plan, 3, LAST_STAGE, "/ro-snap2", subvol_value);
	expect_text(&plan, 3, LAST_STAGE, "/ns/sv-a2/inner/value", subvol_value);
	expect_subvolume(&plan, 1, 1, "/ro-snap", "/subvol", 1);
	expect_subvolume(&plan, 2, 2, "/ro-snap2", "/subvol", 1);
	expect_subvolume(&plan, 3, LAST_STAGE, "/plain", "/subvol", 1);
	expect_subvolume(&plan, 1, 2, "/ns/moved", "/subvol", 0);
	expect_subvolume(&plan, 3, LAST_STAGE, "/ns/sv-a2/inner", "/subvol", 0);
	expect_subvolume(&plan, 1, 2, "/ns/fresh", NULL, 0);
	expect_subvolume(&plan, 3, LAST_STAGE, "/ns/empty", NULL, 0);
	expect_subvolume(&plan, 3, LAST_STAGE, "/dir-x", NULL, 0);
	/* The directory that replaced the stub, and the stub kept. */
	expect_owner(&plan, 2, LAST_STAGE, "/top-snap/subvol", BTRFS_MODE_DIRECTORY | 0700,
	    NAMESPACE_UID, NAMESPACE_GID, 1);
	expect_owner(
	    &plan, 1, LAST_STAGE, "/top-snap/snapshot", BTRFS_MODE_DIRECTORY | 0755, 0, 0, 1);
	expect_absent(&plan, 2, LAST_STAGE, "/top-snap/dir");
	expect_absent(&plan, 2, LAST_STAGE, "/sv-a");
	expect_absent(&plan, 1, LAST_STAGE, "/moving");
	expect_absent(&plan, 1, LAST_STAGE, "/fresh");
	expect_subvolumes(&plan, 0, 0, fixture);
	expect_subvolumes(&plan, 1, 1, first);
	expect_subvolumes(&plan, 2, 2, second);
	expect_subvolumes(&plan, 3, LAST_STAGE, third);
	run_plan(context, &plan);
}

/* Every refusal is decided before a change and leaves the transaction able
 * to commit. */
static void
subvolume_refusals(struct context *context)
{
	static const uint8_t uuid[BTRFS_UUID_SIZE] = { 0x6d, 0x62, 0x74, 0x72, 0x66, 0x73 };
	static char long_name[BTRFS_NAME_MAX + 2];
	struct btrfs_new_inode attributes = subvolume_inode();
	struct btrfs_new_inode file = subvolume_inode();
	struct btrfs_object_id root;
	struct btrfs_object_id ns;
	struct btrfs_object_id one;
	struct btrfs_object_id made;
	struct btrfs_object_id snapshot;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct btrfs_counters *counters;
	struct btrfs_time time = { 1800000000, 0 };
	uint64_t subvol;
	uint64_t id;

	memset(long_name, 's', BTRFS_NAME_MAX + 1);
	file.mode = BTRFS_MODE_REGULAR | 0644;
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	root = object(fs, "/");
	ns = object(fs, "/ns");
	one = object(fs, "/ns/one");
	snapshot = object(fs, "/snapshot");
	subvol = object(fs, "/subvol").tree;
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	refused(transaction,
	    btrfs_transaction_create_subvolume(transaction, ns, "one", 3, &attributes, uuid, &id),
	    BTRFS_EXISTS, "subvolume over a name");
	refused(transaction,
	    btrfs_transaction_create_subvolume(
		transaction, root, "subvol", 6, &attributes, uuid, &id),
	    BTRFS_EXISTS, "subvolume over a subvolume");
	refused(transaction,
	    btrfs_transaction_create_subvolume(transaction, ns, ".", 1, &attributes, uuid, &id),
	    BTRFS_INVALID_ARGUMENT, "subvolume named dot");
	refused(transaction,
	    btrfs_transaction_create_subvolume(transaction, ns, "a/b", 3, &attributes, uuid, &id),
	    BTRFS_INVALID_ARGUMENT, "subvolume with a slash");
	refused(transaction,
	    btrfs_transaction_create_subvolume(
		transaction, ns, long_name, BTRFS_NAME_MAX + 1, &attributes, uuid, &id),
	    BTRFS_NAME_TOO_LONG, "subvolume with a long name");
	refused(transaction,
	    btrfs_transaction_create_subvolume(transaction, ns, "x", 1, &file, uuid, &id),
	    BTRFS_INVALID_ARGUMENT, "subvolume that is not a directory");
	refused(transaction,
	    btrfs_transaction_create_subvolume(transaction, one, "x", 1, &attributes, uuid, &id),
	    BTRFS_NOT_DIRECTORY, "subvolume below a file");
	refused(transaction,
	    btrfs_transaction_create_subvolume(
		transaction, snapshot, "x", 1, &attributes, uuid, &id),
	    BTRFS_READ_ONLY, "subvolume in a read-only snapshot");
	refused(transaction,
	    btrfs_transaction_snapshot(transaction, subvol, snapshot, "x", 1, 0, time, uuid, &id),
	    BTRFS_READ_ONLY, "snapshot into a read-only snapshot");
	refused(transaction,
	    btrfs_transaction_snapshot(transaction, 999, ns, "x", 1, 0, time, uuid, &id),
	    BTRFS_NOT_FOUND, "snapshot of a missing subvolume");
	refused(transaction,
	    btrfs_transaction_snapshot(transaction, BT_EXTENT_TREE, ns, "x", 1, 0, time, uuid, &id),
	    BTRFS_INVALID_ARGUMENT, "snapshot of an internal tree");
	refused(transaction,
	    btrfs_transaction_snapshot(transaction, subvol, ns, "one", 3, 0, time, uuid, &id),
	    BTRFS_EXISTS, "snapshot over a name");
	/* A source changed earlier in the transaction has unreferenced blocks. */
	REQUIRE(btrfs_transaction_create(
		    transaction, object(fs, "/subvol"), "new", 3, &file, &made) == BTRFS_OK);
	refused(transaction,
	    btrfs_transaction_snapshot(transaction, subvol, ns, "x", 1, 0, time, uuid, &id),
	    BTRFS_UNSUPPORTED, "snapshot of a changed source");
	/* A read-only source is admissible; the snapshot is writable. */
	REQUIRE(btrfs_transaction_snapshot(
		    transaction, snapshot.tree, ns, "of-ro", 5, 0, time, uuid, &id) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);

	/* Tree ids are qgroup ids of level zero. */
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_counters_create(&context->env, &counters) == BTRFS_OK);
	counters->next_root = BT_ROOT_ID_LIMIT;
	REQUIRE(btrfs_transaction_use_counters(transaction, counters) == BTRFS_OK);
	refused(transaction,
	    btrfs_transaction_create_subvolume(transaction, ns, "x", 1, &attributes, uuid, &id),
	    BTRFS_NO_SPACE, "subvolume beyond the last tree id");
	btrfs_transaction_destroy(transaction);
	btrfs_counters_destroy(counters);

	/* Linux creates the UUID tree before any subvolume. */
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	transaction->has_uuids = 0;
	refused(transaction,
	    btrfs_transaction_create_subvolume(transaction, ns, "x", 1, &attributes, uuid, &id),
	    BTRFS_UNSUPPORTED, "subvolume without a UUID tree");
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	REQUIRE(context->device->count == 0);
	REQUIRE(context->image.live_allocations == 0);
	printf("subvolume refusals PASS\n");
}

static void
require_text(struct btrfs_fs *fs, const char *path, const char *text)
{
	struct btrfs_inode inode;
	char buffer[64];
	size_t completed;

	REQUIRE(btrfs_image_lookup(fs, path, &inode) == BTRFS_OK);
	REQUIRE(inode.size == strlen(text) && inode.size < sizeof(buffer));
	REQUIRE(btrfs_read(fs, &inode, 0, buffer, (size_t)inode.size, &completed) == BTRFS_OK);
	REQUIRE(completed == inode.size && memcmp(buffer, text, completed) == 0);
}

/* One transaction snapshots a subvolume and then edits both sides, and
 * creates a subvolume with a file inside it; a read-only snapshot refuses
 * edits in the transaction that made it. */
static void
subvolume_same_transaction(struct context *context)
{
	static const uint8_t first[BTRFS_UUID_SIZE] = { 0x73, 0x61, 0x6d, 0x65, 1 };
	static const uint8_t second[BTRFS_UUID_SIZE] = { 0x73, 0x61, 0x6d, 0x65, 2 };
	static const uint8_t third[BTRFS_UUID_SIZE] = { 0x73, 0x61, 0x6d, 0x65, 3 };
	struct btrfs_new_inode attributes = subvolume_inode();
	struct btrfs_new_inode file = subvolume_inode();
	struct btrfs_object_id root;
	struct btrfs_object_id value;
	struct btrfs_object_id inside;
	struct btrfs_object_id created;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct bt_owned_root *tree;
	struct btrfs_time time = { 1800000100, 0 };
	uint64_t subvol;
	uint64_t snapshot;
	uint64_t read_only;
	uint64_t fresh;

	file.mode = BTRFS_MODE_REGULAR | 0644;
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	root = object(fs, "/");
	subvol = object(fs, "/subvol").tree;
	value = object(fs, "/subvol/value");
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_snapshot(
		    transaction, subvol, root, "same", 4, 0, time, first, &snapshot) == BTRFS_OK);
	REQUIRE(btrfs_transaction_snapshot(transaction, subvol, root, "same-ro", 7, 1, time, second,
		    &read_only) == BTRFS_OK);
	REQUIRE(bt_tx_tree(transaction, read_only, &tree) == BTRFS_READ_ONLY);
	REQUIRE(btrfs_transaction_write(transaction, value, 0, source_value, VALUE_BYTES, time) ==
	    BTRFS_OK);
	inside = value;
	inside.tree = snapshot;
	REQUIRE(btrfs_transaction_write(
		    transaction, inside, 0, snapshot_value, VALUE_BYTES, time) == BTRFS_OK);
	REQUIRE(btrfs_transaction_create_subvolume(
		    transaction, root, "fresh", 5, &attributes, third, &fresh) == BTRFS_OK);
	REQUIRE(btrfs_transaction_create(transaction,
		    (struct btrfs_object_id){ fresh, BTRFS_ROOT_INODE }, "file", 4, &file,
		    &created) == BTRFS_OK);
	REQUIRE(created.tree == fresh && created.inode == BTRFS_ROOT_INODE + 1);
	REQUIRE(btrfs_transaction_write(transaction, created, 0, subvol_value, VALUE_BYTES, time) ==
	    BTRFS_OK);
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	audit_state(context, "subvolume-same-transaction");
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	require_text(fs, "/subvol/value", source_value);
	require_text(fs, "/same/value", snapshot_value);
	require_text(fs, "/same-ro/value", subvol_value);
	require_text(fs, "/fresh/file", subvol_value);
	btrfs_unmount(fs);
	truncate_writes(context->device, 0);
	REQUIRE(context->image.live_allocations == 0);
	printf("subvolume edits in the transaction that created them PASS\n");
}

/* Commits one transaction on the current state through apply. */
static void
commit_step(struct context *context,
    void (*apply)(struct btrfs_fs *fs, struct btrfs_transaction *transaction, uint64_t *tree),
    uint64_t *tree)
{
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	apply(fs, transaction, tree);
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
}

static void
create_doomed(struct btrfs_fs *fs, struct btrfs_transaction *transaction, uint64_t *tree)
{
	static const uint8_t uuid[BTRFS_UUID_SIZE] = { 0x64, 0x6f, 0x6f, 0x6d };
	struct btrfs_new_inode attributes = subvolume_inode();

	REQUIRE(btrfs_transaction_create_subvolume(transaction, object(fs, "/"), "doomed", 6,
		    &attributes, uuid, tree) == BTRFS_OK);
}

static void
snapshot_top(struct btrfs_fs *fs, struct btrfs_transaction *transaction, uint64_t *tree)
{
	static const uint8_t uuid[BTRFS_UUID_SIZE] = { 0x74, 0x6f, 0x70 };
	struct btrfs_time time = { 1800000200, 0 };
	uint64_t snapshot;

	(void)tree;
	REQUIRE(btrfs_transaction_snapshot(transaction, BTRFS_TOP_LEVEL_TREE, object(fs, "/subvol"),
		    "top", 3, 0, time, uuid, &snapshot) == BTRFS_OK);
}

static void
delete_doomed(struct btrfs_fs *fs, struct btrfs_transaction *transaction, uint64_t *tree)
{
	struct btrfs_time time = { 1800000300, 0 };

	(void)tree;
	REQUIRE(btrfs_transaction_delete_subvolume(
		    transaction, object(fs, "/"), "doomed", 6, time) == BTRFS_OK);
}

static void
clean_all(struct btrfs_fs *fs, struct btrfs_transaction *transaction, uint64_t *tree)
{
	size_t dropped;
	int pending;

	(void)fs;
	(void)tree;
	REQUIRE(
	    btrfs_transaction_clean_subvolumes(transaction, 4096, &dropped, &pending) == BTRFS_OK);
	REQUIRE(dropped == 1 && !pending);
}

/* A deleted subvolume cannot be opened (Linux's ENOENT), before and after the
 * cleaner; its entry copied into a snapshot stays an empty stub. */
static void
subvolume_deleted_reader(struct context *context)
{
	struct btrfs_inode stub;
	struct btrfs_dir_entry entry;
	struct btrfs_fs *fs;
	uint64_t doomed = 0;
	uint64_t cookie = 0;
	size_t length;
	int pass;

	commit_step(context, create_doomed, &doomed);
	commit_step(context, snapshot_top, NULL);
	REQUIRE(btrfs_mount(&context->env, doomed, &fs) == BTRFS_OK);
	btrfs_unmount(fs);
	commit_step(context, delete_doomed, NULL);
	for (pass = 0; pass < 2; pass++) {
		REQUIRE(btrfs_mount(&context->env, doomed, &fs) == BTRFS_NOT_FOUND);
		REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
		REQUIRE(btrfs_image_lookup(fs, "/doomed", &stub) == BTRFS_NOT_FOUND);
		REQUIRE(btrfs_image_lookup(fs, "/subvol/top/doomed", &stub) == BTRFS_OK);
		REQUIRE(stub.id.inode == BTRFS_EMPTY_SUBVOLUME_INODE &&
		    stub.mode == (BTRFS_MODE_DIRECTORY | 0755) && stub.links == 1);
		REQUIRE(btrfs_next_dir(fs, &stub, &cookie, &entry) == BTRFS_NOT_FOUND);
		REQUIRE(btrfs_list_xattrs(fs, &stub, NULL, 0, &length) == BTRFS_OK && length == 0);
		REQUIRE(btrfs_parent(fs, &stub, &stub) == BTRFS_NOT_FOUND);
		btrfs_unmount(fs);
		if (pass == 0) {
			commit_step(context, clean_all, NULL);
		}
	}
	audit_state(context, "subvolume-deleted-reader");
	truncate_writes(context->device, 0);
	REQUIRE(context->image.live_allocations == 0);
	printf("deleted subvolumes and stub entries in the reader PASS\n");
}

void
subvolume_scenarios(struct context *context)
{
	subvolume_refusals(context);
	subvolume_same_transaction(context);
	subvolume_deleted_reader(context);
	subvolume_create_plan(context);
	subvolume_snapshot_plan(context);
	subvolume_delete_plan(context);
	subvolume_drop_shared_plan(context);
	subvolume_rename_plan(context);
}
