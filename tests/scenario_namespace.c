/* SPDX-License-Identifier: BSD-3-Clause */
/* Namespace scenarios, refusals and limits on the namespace fixture. */
#define _POSIX_C_SOURCE 200809L
#include "scenario.h"

/* The namespace fixture: four names share one CRC32C name hash (also as
 * user. xattrs); Linux wrote the first two. /ns/extref/target has more links
 * than its INODE_REF item holds, so Linux stored the rest as extended
 * references. */
static const char *const collisions[] = { "ethvq997ethvq997", "ethvq997wdkjavbx",
	"wdkjavbxethvq997", "wdkjavbxwdkjavbx" };

static const char *const collision_blocks[] = { "ethvq997", "wdkjavbx" };

static void
extref_path(char *path, size_t size, size_t index)
{
	char name[EXTREF_NAME_BYTES + 1];

	memset(name, 'x', EXTREF_NAME_BYTES);
	name[EXTREF_NAME_BYTES] = '\0';
	REQUIRE(snprintf(path, size, "/ns/extref/l%03zu%s", index, name) < (int)size);
}

/* Names of COLLISION_BLOCKS blocks, each one of collision_blocks: equal-length
 * CRC32C collisions stay collisions when concatenated. */
static void
collision_name(char *name, size_t index)
{
	size_t block;

	for (block = 0; block < COLLISION_BLOCKS; block++) {
		memcpy(name + block * COLLISION_BLOCK_BYTES,
		    collision_blocks[(index >> block) & 1U], COLLISION_BLOCK_BYTES);
	}
	name[COLLISION_BLOCKS * COLLISION_BLOCK_BYTES] = '\0';
}

static size_t
item_limit(const struct context *context)
{
	return context->node_size - sizeof(struct bt_disk_header) - sizeof(struct bt_disk_item);
}

static void
namespace_plan(struct context *context, struct plan *plan, const char *name)
{
	plan_init(plan);
	plan->name = name;
	plan->namespace = 1;
	/* stages.tsv names every stage's generation through a tracked file. */
	(void)plan_file(context, plan, "/greeting");
}

/* New objects of every type, inherited inode flags, data in new files, and
 * enough entries in the second commit to split leaves. */
static void
namespace_create_plan(struct context *context)
{
	static const char *const added[] = { "new", NULL };
	static uint8_t data[NAMESPACE_DATA_BYTES];
	static char names[NAMESPACE_FILES][8];
	const char *listing[NAMESPACE_FILES + 8];
	struct plan plan;
	char path[64];
	size_t count = 0;
	size_t i;

	namespace_plan(context, &plan, "namespace-create");
	plan_create(&plan, 1, "/ns/new", BTRFS_MODE_DIRECTORY | 0755, NULL);
	plan_create(&plan, 1, "/ns/new/file", BTRFS_MODE_REGULAR | 0644, NULL);
	fill_pattern(data, 10000, 21);
	plan_write_new(&plan, 1, "/ns/new/file", 0, data, 10000);
	plan_create(&plan, 1, "/ns/new/link", BTRFS_MODE_SYMLINK | 0777, "../tree/a/b/deep");
	plan_device(&plan, 1, "/ns/new/char", BTRFS_MODE_CHARACTER | 0620, NAMESPACE_DEVICE);
	plan_create(&plan, 1, "/ns/new/pipe", BTRFS_MODE_FIFO | 0600, NULL);
	plan_create(&plan, 1, "/ns/new/sock", BTRFS_MODE_SOCKET | 0755, NULL);
	plan_create(&plan, 1, "/ns/nocow/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/ns/nocow/file", 0, data, 5000);
	plan_create(&plan, 1, "/ns/compress/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/ns/compress/file", 0, data, 5000);
	plan_create(&plan, 1, "/subvol/created", BTRFS_MODE_REGULAR | 0600, NULL);
	plan_write_new(&plan, 1, "/subvol/created", 0, "in a subvolume\n", 15);
	plan_create(&plan, 2, "/ns/new/sub", BTRFS_MODE_DIRECTORY | 0700, NULL);
	plan_create(&plan, 2, "/ns/new/sub/inner", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 2, "/ns/new/sub/inner", 0, data + 4096, 4096);
	for (i = 0; i < NAMESPACE_FILES; i++) {
		REQUIRE(snprintf(names[i], sizeof(names[i]), "f%03zu", i) < (int)sizeof(names[i]));
		REQUIRE(snprintf(path, sizeof(path), "/ns/new/%s", names[i]) < (int)sizeof(path));
		plan_create(&plan, 2, path, BTRFS_MODE_REGULAR | 0644, NULL);
	}
	plan_write_new(&plan, 2, "/ns/new/file", 10000, data + 10000, 3000);

	expect_listing(context, &plan, 0, 0, "/ns", NULL, NULL);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns", added, NULL);
	expect_absent(&plan, 0, 0, "/ns/new");
	expect_absent(&plan, 0, 0, "/subvol/created");
	listing[count++] = "char";
	listing[count++] = "file";
	listing[count++] = "link";
	listing[count++] = "pipe";
	listing[count++] = "sock";
	expect_names(&plan, 1, 1, "/ns/new", listing, count);
	listing[count++] = "sub";
	for (i = 0; i < NAMESPACE_FILES; i++) {
		listing[count++] = names[i];
	}
	expect_names(&plan, 2, LAST_STAGE, "/ns/new", listing, count);
	expect_stat(&plan, 1, LAST_STAGE, "/ns/new", BTRFS_MODE_DIRECTORY | 0755, 1);
	expect_file(&plan, 1, 1, "/ns/new/file", data, 10000);
	expect_file(&plan, 2, LAST_STAGE, "/ns/new/file", data, 13000);
	expect_stat(&plan, 1, LAST_STAGE, "/ns/new/file", BTRFS_MODE_REGULAR | 0644, 1);
	expect_symlink(&plan, 1, LAST_STAGE, "/ns/new/link", "../tree/a/b/deep");
	expect_stat(&plan, 1, LAST_STAGE, "/ns/new/link", BTRFS_MODE_SYMLINK | 0777, 1);
	expect_value(&plan, 1, LAST_STAGE, EXPECT_DEVICE, "/ns/new/char", NAMESPACE_DEVICE);
	expect_stat(&plan, 1, LAST_STAGE, "/ns/new/char", BTRFS_MODE_CHARACTER | 0620, 1);
	expect_stat(&plan, 1, LAST_STAGE, "/ns/new/pipe", BTRFS_MODE_FIFO | 0600, 1);
	expect_stat(&plan, 1, LAST_STAGE, "/ns/new/sock", BTRFS_MODE_SOCKET | 0755, 1);
	expect_file(&plan, 1, LAST_STAGE, "/ns/nocow/file", data, 5000);
	expect_value(&plan, 1, LAST_STAGE, EXPECT_FLAGS, "/ns/nocow/file",
	    BT_INODE_NODATACOW | BT_INODE_NODATASUM);
	expect_file(&plan, 1, LAST_STAGE, "/ns/compress/file", data, 5000);
	expect_value(&plan, 1, LAST_STAGE, EXPECT_FLAGS, "/ns/compress/file", BT_INODE_COMPRESS);
	expect_text(&plan, 1, LAST_STAGE, "/subvol/created", "in a subvolume\n");
	expect_names(&plan, 2, LAST_STAGE, "/ns/new/sub", (const char *[]){ "inner" }, 1);
	expect_file(&plan, 2, LAST_STAGE, "/ns/new/sub/inner", data + 4096, 4096);
	expect_stat(&plan, 2, LAST_STAGE, "/ns/new/sub", BTRFS_MODE_DIRECTORY | 0700, 1);
	run_plan(context, &plan);
}

/* Names and xattrs whose hashes collide share packed items: entries are
 * appended, removed from the front, replaced and renamed within one item. */
static void
namespace_collide_plan(struct context *context)
{
	static const char *const first_added[] = { collisions[2], NULL };
	static const char *const first_removed[] = { collisions[0], NULL };
	static const char *const second_added[] = { collisions[2], collisions[3], NULL };
	static const char *const second_removed[] = { collisions[1], NULL };
	char paths[4][64];
	char xattrs[4][64];
	struct plan plan;
	size_t i;

	for (i = 0; i < 4; i++) {
		REQUIRE(snprintf(paths[i], sizeof(paths[i]), "/ns/collide/%s", collisions[i]) <
		    (int)sizeof(paths[i]));
		REQUIRE(snprintf(xattrs[i], sizeof(xattrs[i]), "user.%s", collisions[i]) <
		    (int)sizeof(xattrs[i]));
	}
	namespace_plan(context, &plan, "namespace-collide");
	plan_create(&plan, 1, paths[2], BTRFS_MODE_REGULAR | 0644, NULL);
	plan_unlink(&plan, 1, paths[0], 0);
	plan_set_xattr(&plan, 1, "/ns/one", xattrs[2], "third", 5, BTRFS_XATTR_CREATE);
	plan_set_xattr(&plan, 1, "/ns/one", xattrs[0], "replaced", 8, BTRFS_XATTR_REPLACE);
	plan_remove_xattr(&plan, 1, "/ns/one", xattrs[1]);
	plan_rename(&plan, 2, paths[1], paths[3], 0);
	plan_create(&plan, 2, paths[0], BTRFS_MODE_REGULAR | 0600, NULL);
	plan_set_xattr(&plan, 2, "/ns/one", xattrs[3], "fourth", 6, 0);
	plan_remove_xattr(&plan, 2, "/ns/one", xattrs[0]);

	expect_listing(context, &plan, 0, 0, "/ns/collide", NULL, NULL);
	expect_listing(context, &plan, 1, 1, "/ns/collide", first_added, first_removed);
	expect_listing(context, &plan, 2, LAST_STAGE, "/ns/collide", second_added, second_removed);
	expect_text(&plan, 0, 0, paths[0], "first\n");
	expect_absent(&plan, 1, 1, paths[0]);
	expect_text(&plan, 2, LAST_STAGE, paths[0], "");
	expect_stat(&plan, 2, LAST_STAGE, paths[0], BTRFS_MODE_REGULAR | 0600, 1);
	expect_text(&plan, 0, 1, paths[1], "second\n");
	expect_absent(&plan, 2, LAST_STAGE, paths[1]);
	expect_text(&plan, 1, LAST_STAGE, paths[2], "");
	expect_absent(&plan, 0, 1, paths[3]);
	expect_text(&plan, 2, LAST_STAGE, paths[3], "second\n");
	expect_xattr(&plan, 0, 0, "/ns/one", xattrs[0], "first", 5);
	expect_xattr(&plan, 1, 1, "/ns/one", xattrs[0], "replaced", 8);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/one", xattrs[0], NULL, 0);
	expect_xattr(&plan, 0, 0, "/ns/one", xattrs[1], "second", 6);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/one", xattrs[1], NULL, 0);
	expect_xattr(&plan, 0, 0, "/ns/one", xattrs[2], NULL, 0);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/one", xattrs[2], "third", 5);
	expect_xattr(&plan, 0, 1, "/ns/one", xattrs[3], NULL, 0);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/one", xattrs[3], "fourth", 6);
	run_plan(context, &plan);
}

/* Hard links: across directories, to a device node, to an inode with
 * extended references, and a second name in a colliding item. */
static void
namespace_link_plan(struct context *context)
{
	static const char *const root_added[] = { "third", "target-link", NULL };
	static const char *const tree_added[] = { "null2", NULL };
	static const char *const a_added[] = { "second", NULL };
	static const char *const b_added[] = { "fourth", NULL };
	static const char *const collide_added[] = { collisions[2], NULL };
	char first[64];
	char third[64];
	char extref[320];
	struct plan plan;

	REQUIRE(
	    snprintf(first, sizeof(first), "/ns/collide/%s", collisions[0]) < (int)sizeof(first));
	REQUIRE(
	    snprintf(third, sizeof(third), "/ns/collide/%s", collisions[2]) < (int)sizeof(third));
	extref_path(extref, sizeof(extref), EXTREF_LINKS - 1);
	namespace_plan(context, &plan, "namespace-link");
	plan_link(&plan, 1, "/ns/one", "/ns/tree/a/second");
	plan_link(&plan, 1, "/ns/one", "/ns/third");
	plan_link(&plan, 1, "/ns/null", "/ns/tree/null2");
	plan_link(&plan, 1, "/ns/extref/target", "/ns/target-link");
	plan_link(&plan, 1, first, third);
	plan_link(&plan, 2, "/ns/tree/a/second", "/ns/tree/a/b/fourth");

	expect_listing(context, &plan, 0, 0, "/ns", NULL, NULL);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns", root_added, NULL);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns/tree", tree_added, NULL);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns/tree/a", a_added, NULL);
	expect_listing(context, &plan, 0, 1, "/ns/tree/a/b", NULL, NULL);
	expect_listing(context, &plan, 2, LAST_STAGE, "/ns/tree/a/b", b_added, NULL);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns/collide", collide_added, NULL);
	expect_links(context, &plan, 0, 0, "/ns/one", "/ns/one", 2);
	expect_links(context, &plan, 1, 1, "/ns/one", "/ns/one", 4);
	expect_links(context, &plan, 2, LAST_STAGE, "/ns/one", "/ns/one", 5);
	expect_same(&plan, 1, LAST_STAGE, "/ns/tree/a/second", "/ns/one");
	expect_same(&plan, 1, LAST_STAGE, "/ns/third", "/ns/one");
	expect_same(&plan, 2, LAST_STAGE, "/ns/tree/a/b/fourth", "/ns/one");
	expect_same(&plan, 1, LAST_STAGE, "/ns/tree/null2", "/ns/null");
	expect_value(&plan, 1, LAST_STAGE, EXPECT_DEVICE, "/ns/tree/null2", LINUX_NULL_DEVICE);
	expect_same(&plan, 1, LAST_STAGE, "/ns/target-link", extref);
	expect_links(context, &plan, 1, LAST_STAGE, "/ns/target-link", "/ns/extref/target",
	    EXTREF_LINKS + 2);
	expect_same(&plan, 1, LAST_STAGE, third, first);
	expect_links(context, &plan, 1, LAST_STAGE, first, first, 2);
	run_plan(context, &plan);
}

/* Unlinks drop link counts, empty directories, a shared data extent's
 * reference and then its last one, and names of an inode with extended
 * references. */
static void
namespace_unlink_plan(struct context *context)
{
	static const char *const root_first[] = { "data", "fifo", "empty", NULL };
	static const char *const root_second[] = { "data", "fifo", "empty", "clone", NULL };
	static const char *const tree_removed[] = { "one-link", NULL };
	static const char *const a_removed[] = { "b", NULL };
	char extref_first[320];
	char extref_remaining[320];
	const char *extref_one[] = { "target", NULL };
	const char *extref_two[] = { "target", NULL, NULL };
	struct plan plan;

	extref_path(extref_first, sizeof(extref_first), 0);
	extref_path(extref_remaining, sizeof(extref_remaining), 10);
	extref_two[1] = strrchr(extref_first, '/') + 1;
	namespace_plan(context, &plan, "namespace-unlink");
	plan_unlink(&plan, 1, "/ns/tree/one-link", 0);
	plan_unlink(&plan, 1, "/ns/tree/a/b/deep", 0);
	plan_unlink(&plan, 1, "/ns/tree/a/b", 0);
	plan_unlink(&plan, 1, "/ns/data", 0);
	plan_unlink(&plan, 1, "/ns/extref/target", 0);
	plan_unlink(&plan, 1, "/ns/fifo", 0);
	plan_unlink(&plan, 1, "/ns/empty", 0);
	plan_unlink(&plan, 2, "/ns/clone", 0);
	plan_unlink(&plan, 2, extref_first, 0);

	expect_listing(context, &plan, 0, 0, "/ns", NULL, NULL);
	expect_listing(context, &plan, 1, 1, "/ns", NULL, root_first);
	expect_listing(context, &plan, 2, LAST_STAGE, "/ns", NULL, root_second);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns/tree", NULL, tree_removed);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns/tree/a", NULL, a_removed);
	expect_listing(context, &plan, 1, 1, "/ns/extref", NULL, extref_one);
	expect_listing(context, &plan, 2, LAST_STAGE, "/ns/extref", NULL, extref_two);
	expect_links(context, &plan, 1, LAST_STAGE, "/ns/one", "/ns/one", 1);
	expect_current(context, &plan, 0, 1, "/ns/clone", "/ns/clone");
	expect_absent(&plan, 2, LAST_STAGE, "/ns/clone");
	expect_absent(&plan, 1, LAST_STAGE, "/ns/data");
	expect_links(context, &plan, 0, 0, extref_remaining, "/ns/extref/target", EXTREF_LINKS + 1);
	expect_links(context, &plan, 1, 1, extref_remaining, "/ns/extref/target", EXTREF_LINKS);
	expect_links(
	    context, &plan, 2, LAST_STAGE, extref_remaining, "/ns/extref/target", EXTREF_LINKS - 1);
	run_plan(context, &plan);
}

/* Renames across directories, over files, over an empty directory, between
 * names of one inode, and within and into a colliding item. */
static void
namespace_rename_plan(struct context *context)
{
	static const char *const root_first_added[] = { "moved", NULL };
	static const char *const root_first_removed[] = { "victim", "full", NULL };
	static const char *const root_second_removed[] = { "victim", "full", "clone", "data",
		NULL };
	static const char *const tree_first_removed[] = { "a", NULL };
	static const char *const tree_second_added[] = { "b2", NULL };
	static const char *const collide_added[] = { collisions[2], collisions[3], NULL };
	static const char *const collide_removed[] = { collisions[0], NULL };
	char paths[4][64];
	struct plan plan;
	size_t i;

	for (i = 0; i < 4; i++) {
		REQUIRE(snprintf(paths[i], sizeof(paths[i]), "/ns/collide/%s", collisions[i]) <
		    (int)sizeof(paths[i]));
	}
	namespace_plan(context, &plan, "namespace-rename");
	plan_rename(&plan, 1, "/ns/tree/a", "/ns/moved", 0);
	plan_rename(&plan, 1, "/ns/victim", "/ns/data", 0);
	plan_rename(&plan, 1, "/ns/full", "/ns/empty", 0);
	plan_rename(&plan, 1, "/ns/tree/one-link", "/ns/one", 0);
	plan_rename(&plan, 1, "/ns/compress", "/ns/compress", 0);
	plan_rename(&plan, 2, "/ns/moved/b", "/ns/tree/b2", 0);
	plan_rename(&plan, 2, "/ns/clone", paths[2], 0);
	plan_rename(&plan, 2, "/ns/data", "/ns/tree/one-link", 0);
	plan_rename(&plan, 2, paths[0], paths[3], 0);

	expect_listing(context, &plan, 0, 0, "/ns", NULL, NULL);
	expect_listing(context, &plan, 1, 1, "/ns", root_first_added, root_first_removed);
	expect_listing(context, &plan, 2, LAST_STAGE, "/ns", root_first_added, root_second_removed);
	expect_listing(context, &plan, 0, 0, "/ns/tree", NULL, NULL);
	expect_listing(context, &plan, 1, 1, "/ns/tree", NULL, tree_first_removed);
	expect_listing(
	    context, &plan, 2, LAST_STAGE, "/ns/tree", tree_second_added, tree_first_removed);
	expect_names(&plan, 1, 1, "/ns/moved", (const char *[]){ "b" }, 1);
	expect_names(&plan, 2, LAST_STAGE, "/ns/moved", NULL, 0);
	expect_text(&plan, 1, 1, "/ns/moved/b/deep", "deep\n");
	expect_text(&plan, 2, LAST_STAGE, "/ns/tree/b2/deep", "deep\n");
	expect_current(context, &plan, 0, 0, "/ns/data", "/ns/data");
	expect_text(&plan, 1, 1, "/ns/data", "victim\n");
	expect_same(&plan, 0, 1, "/ns/tree/one-link", "/ns/one");
	expect_text(&plan, 2, LAST_STAGE, "/ns/tree/one-link", "victim\n");
	expect_names(&plan, 1, LAST_STAGE, "/ns/empty", (const char *[]){ "inner" }, 1);
	expect_text(&plan, 1, LAST_STAGE, "/ns/empty/inner", "inner\n");
	expect_absent(&plan, 1, LAST_STAGE, "/ns/full");
	expect_links(context, &plan, 0, 1, "/ns/one", "/ns/one", 2);
	expect_links(context, &plan, 2, LAST_STAGE, "/ns/one", "/ns/one", 1);
	expect_current(context, &plan, 0, 1, "/ns/clone", "/ns/clone");
	expect_current(context, &plan, 2, LAST_STAGE, paths[2], "/ns/clone");
	expect_listing(
	    context, &plan, 2, LAST_STAGE, "/ns/collide", collide_added, collide_removed);
	expect_text(&plan, 2, LAST_STAGE, paths[3], "first\n");
	run_plan(context, &plan);
}

/* Last names of open inodes go, leaving orphan items; the final state keeps
 * them, so Linux's read-write mount must clean them up. */
static void
namespace_open_plan(struct context *context)
{
	static const char *const removed[] = { "victim", "clone", "fifo", NULL };
	struct plan plan;

	namespace_plan(context, &plan, "namespace-open");
	plan_unlink(&plan, 1, "/ns/victim", 1);
	plan_unlink(&plan, 1, "/ns/clone", 1);
	plan_rename(&plan, 1, "/ns/fifo", "/ns/null", 1);
	expect_listing(context, &plan, 0, 0, "/ns", NULL, NULL);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns", NULL, removed);
	expect_current(context, &plan, 0, LAST_STAGE, "/ns/data", "/ns/data");
	expect_links(context, &plan, 1, LAST_STAGE, "/ns/null", "/ns/fifo", 1);
	run_plan(context, &plan);
}

/* Orphans go by eviction, then by cleanup as after a crash. */
static void
namespace_orphan_plan(struct context *context)
{
	static const char *const removed[] = { "victim", "clone", NULL };
	struct plan plan;

	namespace_plan(context, &plan, "namespace-orphans");
	plan_unlink(&plan, 1, "/ns/victim", 1);
	plan_unlink(&plan, 1, "/ns/clone", 1);
	plan_evict(context, &plan, 2, "/ns/victim");
	plan_clean(&plan, 3, BTRFS_TOP_LEVEL_TREE, 1);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns", NULL, removed);
	expect_current(context, &plan, 0, LAST_STAGE, "/ns/data", "/ns/data");
	run_plan(context, &plan);
}

/* The largest xattr one item holds, an empty value, special files, then
 * replacement and removal. */
static void
namespace_xattr_plan(struct context *context)
{
	struct plan plan;
	uint8_t *big;
	size_t size = item_limit(context) - sizeof(struct bt_disk_dir) - strlen("user.big");

	big = malloc(size);
	REQUIRE(big != NULL);
	fill_pattern(big, size, 31);
	namespace_plan(context, &plan, "namespace-xattr");
	plan_set_xattr(&plan, 1, "/ns/tree", "user.big", big, size, BTRFS_XATTR_CREATE);
	/* Linux exposes user xattrs only on regular files and directories. */
	plan_set_xattr(&plan, 1, "/ns/null", "trusted.empty", "", 0, 0);
	plan_set_xattr(&plan, 1, "/ns/fifo", "trusted.fifo", "special", 7, 0);
	plan_set_xattr(&plan, 2, "/ns/tree", "user.big", "small", 5, BTRFS_XATTR_REPLACE);
	plan_remove_xattr(&plan, 2, "/ns/null", "trusted.empty");
	expect_xattr(&plan, 0, 0, "/ns/tree", "user.big", NULL, 0);
	expect_xattr(&plan, 1, 1, "/ns/tree", "user.big", big, size);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/tree", "user.big", "small", 5);
	expect_xattr(&plan, 1, 1, "/ns/null", "trusted.empty", "", 0);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/null", "trusted.empty", NULL, 0);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/fifo", "trusted.fifo", "special", 7);
	free(big);
	run_plan(context, &plan);
}

/* A subvolume's own tree: new names, a hard link and a rename. */
static void
namespace_subvolume_plan(struct context *context)
{
	static const char *const added[] = { "dir", "renamed", NULL };
	struct plan plan;

	namespace_plan(context, &plan, "namespace-subvolume");
	plan_create(&plan, 1, "/subvol/dir", BTRFS_MODE_DIRECTORY | 0755, NULL);
	plan_create(&plan, 1, "/subvol/dir/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/subvol/dir/file", 0, "moved file\n", 11);
	plan_link(&plan, 1, "/subvol/value", "/subvol/dir/value-link");
	plan_rename(&plan, 1, "/subvol/dir/file", "/subvol/renamed", 0);
	expect_listing(context, &plan, 0, 0, "/subvol", NULL, NULL);
	expect_listing(context, &plan, 1, LAST_STAGE, "/subvol", added, NULL);
	expect_names(&plan, 1, LAST_STAGE, "/subvol/dir", (const char *[]){ "value-link" }, 1);
	expect_same(&plan, 1, LAST_STAGE, "/subvol/dir/value-link", "/subvol/value");
	expect_links(context, &plan, 1, LAST_STAGE, "/subvol/value", "/subvol/value", 2);
	expect_text(&plan, 1, LAST_STAGE, "/subvol/renamed", "moved file\n");
	run_plan(context, &plan);
}

/* The btrfs.compression property: inherited by new regular files and
 * directories, applied to inode flags when set or removed, and recording the
 * codec's incompat feature. */
static void
namespace_property_plan(struct context *context)
{
	static const char *const zstd_added[] = { "file", "sub", "link", "after", NULL };
	static uint8_t data[NAMESPACE_DATA_BYTES];
	const uint64_t codec_flags = BT_INODE_COMPRESS | BT_INODE_NOCOMPRESS;
	struct plan plan;

	fill_pattern(data, 6000, 41);
	namespace_plan(context, &plan, "namespace-property");
	plan_create(&plan, 1, "/ns/zstd/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/ns/zstd/file", 0, data, 6000);
	plan_create(&plan, 1, "/ns/zstd/sub", BTRFS_MODE_DIRECTORY | 0755, NULL);
	plan_create(&plan, 1, "/ns/zstd/sub/deeper", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_create(&plan, 1, "/ns/zstd/link", BTRFS_MODE_SYMLINK | 0777, "file");
	plan_create(&plan, 1, "/ns/nocompress/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_set_xattr(&plan, 1, "/ns/tree", "btrfs.compression", "lzo", 3, 0);
	plan_create(&plan, 1, "/ns/tree/lzo-file", BTRFS_MODE_REGULAR | 0600, NULL);
	plan_remove_xattr(&plan, 2, "/ns/zstd", "btrfs.compression");
	plan_set_xattr(
	    &plan, 2, "/ns/tree/lzo-file", "btrfs.compression", "no", 2, BTRFS_XATTR_REPLACE);
	plan_create(&plan, 2, "/ns/zstd/after", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_set_xattr(&plan, 2, "/ns/nocompress", "btrfs.compression", "", 0, 0);

	expect_listing(context, &plan, 2, LAST_STAGE, "/ns/zstd", zstd_added, NULL);
	expect_xattr(&plan, 0, 1, "/ns/zstd", "btrfs.compression", "zstd", 4);
	expect_flags(&plan, 0, 1, "/ns/zstd", codec_flags, BT_INODE_COMPRESS);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/zstd", "btrfs.compression", NULL, 0);
	expect_flags(&plan, 2, LAST_STAGE, "/ns/zstd", codec_flags, 0);
	expect_file(&plan, 1, LAST_STAGE, "/ns/zstd/file", data, 6000);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/zstd/file", "btrfs.compression", "zstd", 4);
	expect_flags(&plan, 1, LAST_STAGE, "/ns/zstd/file", codec_flags, BT_INODE_COMPRESS);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/zstd/sub", "btrfs.compression", "zstd", 4);
	expect_flags(&plan, 1, LAST_STAGE, "/ns/zstd/sub", codec_flags, BT_INODE_COMPRESS);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/zstd/sub/deeper", "btrfs.compression", "zstd", 4);
	expect_flags(&plan, 1, LAST_STAGE, "/ns/zstd/sub/deeper", codec_flags, BT_INODE_COMPRESS);
	expect_symlink(&plan, 1, LAST_STAGE, "/ns/zstd/link", "file");
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/zstd/after", "btrfs.compression", NULL, 0);
	expect_flags(&plan, 2, LAST_STAGE, "/ns/zstd/after", codec_flags, 0);
	expect_xattr(&plan, 0, 1, "/ns/nocompress", "btrfs.compression", "no", 2);
	expect_flags(&plan, 0, 1, "/ns/nocompress", codec_flags, BT_INODE_NOCOMPRESS);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/nocompress", "btrfs.compression", NULL, 0);
	expect_flags(&plan, 2, LAST_STAGE, "/ns/nocompress", codec_flags, 0);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/nocompress/file", "btrfs.compression", NULL, 0);
	expect_flags(&plan, 1, LAST_STAGE, "/ns/nocompress/file", codec_flags, BT_INODE_NOCOMPRESS);
	expect_xattr(&plan, 0, 0, "/ns/tree", "btrfs.compression", NULL, 0);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/tree", "btrfs.compression", "lzo", 3);
	expect_flags(&plan, 1, LAST_STAGE, "/ns/tree", codec_flags, BT_INODE_COMPRESS);
	expect_xattr(&plan, 1, 1, "/ns/tree/lzo-file", "btrfs.compression", "lzo", 3);
	expect_flags(&plan, 1, 1, "/ns/tree/lzo-file", codec_flags, BT_INODE_COMPRESS);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/tree/lzo-file", "btrfs.compression", "no", 2);
	expect_flags(&plan, 2, LAST_STAGE, "/ns/tree/lzo-file", codec_flags, BT_INODE_NOCOMPRESS);
	expect_value(&plan, 1, LAST_STAGE, EXPECT_FEATURE, "/", BT_FEATURE_COMPRESS_LZO);
	run_plan(context, &plan);
}

/* Colliding names fill one DIR_ITEM to the largest item a leaf holds. */
/* Linux's struct vfs_cap_data, revision 2: the security.capability value. */
#define LINUX_CAPABILITY_REVISION_2 UINT32_C(0x02000000)
#define LINUX_CAPABILITY_EFFECTIVE UINT32_C(0x000001)
#define LINUX_CAP_NET_BIND_SERVICE 10U
#define ATTRIBUTE_ACCESS_SECONDS INT64_C(1600000000)
#define ATTRIBUTE_MODIFY_SECONDS INT64_C(1600000100)
#define CHANGED_UID 1234U
#define CHANGED_GID 5678U
#define CHOWNED_UID 777U

struct linux_capability {
	struct bt_le32 magic;

	struct {
		struct bt_le32 permitted;
		struct bt_le32 inheritable;
	} data[2];
};

_Static_assert(sizeof(struct linux_capability) == 20, "vfs_cap_data revision 2 layout");

/* Attribute changes and the privilege decision for set-id files: a privileged
 * write keeps S_ISUID; S_ISGID without group execution needs no decision; an
 * unprivileged write drops S_ISUID/S_ISGID and the file capability; chown
 * through set_attributes removes the capability with the mode its caller chose. */
static void
namespace_attributes_plan(struct context *context)
{
	struct linux_capability capability;
	const unsigned all = BTRFS_ATTRIBUTE_MODE | BTRFS_ATTRIBUTE_UID | BTRFS_ATTRIBUTE_GID |
	    BTRFS_ATTRIBUTE_ACCESS_TIME | BTRFS_ATTRIBUTE_MODIFY_TIME;
	struct plan plan;

	memset(&capability, 0, sizeof(capability));
	bt_put32(&capability.magic, LINUX_CAPABILITY_REVISION_2 | LINUX_CAPABILITY_EFFECTIVE);
	bt_put32(&capability.data[0].permitted, UINT32_C(1) << LINUX_CAP_NET_BIND_SERVICE);
	namespace_plan(context, &plan, "namespace-attributes");
	plan_create(&plan, 1, "/ns/suid", BTRFS_MODE_REGULAR | 04755, NULL);
	plan_privileges(&plan, 1, "/ns/suid", 1);
	plan_write_new(&plan, 1, "/ns/suid", 0, "privileged\n", 11);
	plan_create(&plan, 1, "/ns/sgid", BTRFS_MODE_REGULAR | 02644, NULL);
	plan_write_new(&plan, 1, "/ns/sgid", 0, "lock marker\n", 12);
	plan_create(&plan, 1, "/ns/capable", BTRFS_MODE_REGULAR | 02755, NULL);
	plan_set_xattr(
	    &plan, 1, "/ns/capable", "security.capability", &capability, sizeof(capability), 0);
	plan_privileges(&plan, 2, "/ns/capable", 0);
	plan_write_new(&plan, 2, "/ns/capable", 0, "dropped\n", 8);
	plan_set_attributes(&plan, 2, "/ns/one", all, 0600, CHANGED_UID, CHANGED_GID,
	    ATTRIBUTE_ACCESS_SECONDS, ATTRIBUTE_MODIFY_SECONDS);
	plan_set_attributes(&plan, 2, "/ns/suid",
	    BTRFS_ATTRIBUTE_MODE | BTRFS_ATTRIBUTE_UID | BTRFS_ATTRIBUTE_REMOVE_CAPABILITY, 0755,
	    CHOWNED_UID, 0, 0, 0);

	expect_absent(&plan, 0, 0, "/ns/suid");
	expect_stat(&plan, 1, 1, "/ns/suid", BTRFS_MODE_REGULAR | 04755, 1);
	expect_owner(&plan, 2, LAST_STAGE, "/ns/suid", BTRFS_MODE_REGULAR | 0755, CHOWNED_UID,
	    NAMESPACE_GID, 1);
	expect_text(&plan, 1, LAST_STAGE, "/ns/suid", "privileged\n");
	expect_stat(&plan, 1, LAST_STAGE, "/ns/sgid", BTRFS_MODE_REGULAR | 02644, 1);
	expect_text(&plan, 1, LAST_STAGE, "/ns/sgid", "lock marker\n");
	expect_stat(&plan, 1, 1, "/ns/capable", BTRFS_MODE_REGULAR | 02755, 1);
	expect_xattr(
	    &plan, 1, 1, "/ns/capable", "security.capability", &capability, sizeof(capability));
	expect_text(&plan, 1, 1, "/ns/capable", "");
	expect_stat(&plan, 2, LAST_STAGE, "/ns/capable", BTRFS_MODE_REGULAR | 0755, 1);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/capable", "security.capability", NULL, 0);
	expect_text(&plan, 2, LAST_STAGE, "/ns/capable", "dropped\n");
	expect_links(context, &plan, 0, 1, "/ns/one", "/ns/one", 2);
	expect_owner(&plan, 2, LAST_STAGE, "/ns/one", BTRFS_MODE_REGULAR | 0600, CHANGED_UID,
	    CHANGED_GID, 2);
	expect_times(
	    &plan, 2, LAST_STAGE, "/ns/one", ATTRIBUTE_ACCESS_SECONDS, ATTRIBUTE_MODIFY_SECONDS);
	run_plan(context, &plan);
}

static void
namespace_full_item_plan(struct context *context)
{
	const char **listing;
	char (*names)[COLLISION_NAME_BYTES + 1];
	char path[COLLISION_NAME_BYTES + 32];
	struct plan plan;
	size_t count = item_limit(context) / (sizeof(struct bt_disk_dir) + COLLISION_NAME_BYTES);
	size_t i;

	names = calloc(count, sizeof(*names));
	listing = calloc(count, sizeof(*listing));
	REQUIRE(names != NULL && listing != NULL);
	namespace_plan(context, &plan, "namespace-full-item");
	for (i = 0; i < count; i++) {
		collision_name(names[i], i);
		listing[i] = names[i];
		REQUIRE(snprintf(path, sizeof(path), "/ns/empty/%s", names[i]) < (int)sizeof(path));
		plan_create(&plan, 1, path, BTRFS_MODE_REGULAR | 0644, NULL);
	}
	expect_names(&plan, 0, 0, "/ns/empty", NULL, 0);
	expect_names(&plan, 1, LAST_STAGE, "/ns/empty", listing, count);
	free(listing);
	free(names);
	run_plan(context, &plan);
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
	/* Refusals before any change leave the transaction usable. */
	REQUIRE(transaction->failure == BTRFS_OK);
}

struct btrfs_object_id
object(struct btrfs_fs *fs, const char *path)
{
	struct btrfs_inode inode;

	REQUIRE(btrfs_image_lookup(fs, path, &inode) == BTRFS_OK);
	return inode.id;
}

static struct btrfs_new_inode
new_inode(uint32_t mode)
{
	struct btrfs_new_inode attributes;

	memset(&attributes, 0, sizeof(attributes));
	attributes.mode = mode;
	attributes.uid = NAMESPACE_UID;
	attributes.gid = NAMESPACE_GID;
	attributes.time.seconds = 1800000000;
	return attributes;
}

/* Every refusal happens before a change and leaves the transaction able to
 * commit; an operation that names one inode twice changes nothing. */
static void
namespace_refusals(struct context *context)
{
	static char long_name[BTRFS_NAME_MAX + 2];
	static char long_target[LINUX_PATH_MAX + 1];
	struct btrfs_new_inode file = new_inode(BTRFS_MODE_REGULAR | 0644);
	struct btrfs_new_inode attributes;
	struct btrfs_object_id ns;
	struct btrfs_object_id one;
	struct btrfs_object_id id;
	struct btrfs_object_id root;
	struct btrfs_object_id subvolume;
	struct btrfs_object_id snapshot;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct btrfs_time time = { 1800000000, 0 };
	char extref[320];
	char xattr[64];
	uint8_t *value;
	size_t limit = item_limit(context);

	memset(long_name, 'n', BTRFS_NAME_MAX + 1);
	memset(long_target, 't', LINUX_PATH_MAX);
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	ns = object(fs, "/ns");
	one = object(fs, "/ns/one");
	root = object(fs, "/");
	subvolume = object(fs, "/subvol");
	snapshot = object(fs, "/snapshot");
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	refused(transaction, btrfs_transaction_create(transaction, ns, "one", 3, &file, &id),
	    BTRFS_EXISTS, "create over a name");
	refused(transaction, btrfs_transaction_create(transaction, root, "subvol", 6, &file, &id),
	    BTRFS_EXISTS, "create over a subvolume");
	refused(transaction, btrfs_transaction_create(transaction, ns, ".", 1, &file, &id),
	    BTRFS_INVALID_ARGUMENT, "create dot");
	refused(transaction, btrfs_transaction_create(transaction, ns, "..", 2, &file, &id),
	    BTRFS_INVALID_ARGUMENT, "create dot-dot");
	refused(transaction, btrfs_transaction_create(transaction, ns, "a/b", 3, &file, &id),
	    BTRFS_INVALID_ARGUMENT, "create with a slash");
	refused(transaction, btrfs_transaction_create(transaction, ns, "", 0, &file, &id),
	    BTRFS_INVALID_ARGUMENT, "create an empty name");
	refused(transaction,
	    btrfs_transaction_create(transaction, ns, long_name, BTRFS_NAME_MAX + 1, &file, &id),
	    BTRFS_NAME_TOO_LONG, "create a long name");
	refused(transaction, btrfs_transaction_create(transaction, one, "x", 1, &file, &id),
	    BTRFS_NOT_DIRECTORY, "create below a file");
	attributes = new_inode(0644);
	refused(transaction, btrfs_transaction_create(transaction, ns, "x", 1, &attributes, &id),
	    BTRFS_INVALID_ARGUMENT, "create without a type");
	attributes = new_inode(BTRFS_MODE_SYMLINK | 0777);
	refused(transaction, btrfs_transaction_create(transaction, ns, "x", 1, &attributes, &id),
	    BTRFS_INVALID_ARGUMENT, "symlink without a target");
	attributes.target = long_target;
	attributes.target_length = LINUX_PATH_MAX;
	refused(transaction, btrfs_transaction_create(transaction, ns, "x", 1, &attributes, &id),
	    BTRFS_NAME_TOO_LONG, "symlink beyond PATH_MAX");
	attributes = file;
	attributes.target = "t";
	attributes.target_length = 1;
	refused(transaction, btrfs_transaction_create(transaction, ns, "x", 1, &attributes, &id),
	    BTRFS_INVALID_ARGUMENT, "regular file with a target");
	refused(transaction, btrfs_transaction_create(transaction, snapshot, "x", 1, &file, &id),
	    BTRFS_READ_ONLY, "create in a read-only snapshot");
	refused(transaction,
	    btrfs_transaction_link(transaction, object(fs, "/ns/tree"), ns, "x", 1, time),
	    BTRFS_IS_DIRECTORY, "link a directory");
	refused(transaction, btrfs_transaction_link(transaction, one, subvolume, "x", 1, time),
	    BTRFS_CROSS_TREE, "link across trees");
	refused(transaction, btrfs_transaction_link(transaction, one, ns, "victim", 6, time),
	    BTRFS_EXISTS, "link over a name");
	/* A short name still fits the INODE_REF item; a long one needs an
	 * extended reference. */
	extref_path(extref, sizeof(extref), EXTREF_LINKS);
	refused(transaction,
	    btrfs_transaction_link(transaction, object(fs, "/ns/extref/target"),
		object(fs, "/ns/extref"), strrchr(extref, '/') + 1,
		strlen(strrchr(extref, '/') + 1), time),
	    BTRFS_UNSUPPORTED, "link beyond a full INODE_REF");
	refused(transaction, btrfs_transaction_unlink(transaction, ns, "missing", 7, time, 0),
	    BTRFS_NOT_FOUND, "unlink a missing name");
	refused(transaction, btrfs_transaction_unlink(transaction, ns, "full", 4, time, 0),
	    BTRFS_NOT_EMPTY, "unlink a non-empty directory");
	refused(transaction, btrfs_transaction_unlink(transaction, root, "subvol", 6, time, 0),
	    BTRFS_CROSS_TREE, "unlink a subvolume");
	extref_path(extref, sizeof(extref), EXTREF_LINKS - 1);
	refused(transaction,
	    btrfs_transaction_unlink(transaction, object(fs, "/ns/extref"),
		strrchr(extref, '/') + 1, strlen(strrchr(extref, '/') + 1), time, 0),
	    BTRFS_UNSUPPORTED, "unlink an extended reference");
	refused(transaction, btrfs_transaction_unlink(transaction, snapshot, "value", 5, time, 0),
	    BTRFS_READ_ONLY, "unlink in a read-only snapshot");
	refused(transaction,
	    btrfs_transaction_rename(
		transaction, ns, "tree", 4, object(fs, "/ns/tree/a"), "inside", 6, time, 0),
	    BTRFS_INVALID_ARGUMENT, "rename a directory below itself");
	refused(transaction,
	    btrfs_transaction_rename(transaction, ns, "one", 3, ns, "empty", 5, time, 0),
	    BTRFS_IS_DIRECTORY, "rename a file over a directory");
	refused(transaction,
	    btrfs_transaction_rename(transaction, ns, "empty", 5, ns, "one", 3, time, 0),
	    BTRFS_NOT_DIRECTORY, "rename a directory over a file");
	refused(transaction,
	    btrfs_transaction_rename(transaction, ns, "empty", 5, ns, "full", 4, time, 0),
	    BTRFS_NOT_EMPTY, "rename over a non-empty directory");
	refused(transaction,
	    btrfs_transaction_rename(transaction, subvolume, "value", 5, ns, "value", 5, time, 0),
	    BTRFS_CROSS_TREE, "rename across trees");
	refused(transaction,
	    btrfs_transaction_rename(transaction, root, "subvol", 6, root, "other", 5, time, 0),
	    BTRFS_CROSS_TREE, "rename a subvolume");
	refused(transaction,
	    btrfs_transaction_rename(transaction, ns, "missing", 7, ns, "other", 5, time, 0),
	    BTRFS_NOT_FOUND, "rename a missing name");
	refused(transaction,
	    btrfs_transaction_rename(
		transaction, object(fs, "/ns/tree"), "one-link", 8, ns, "one", 3, time, 0),
	    BTRFS_OK, "rename between names of one inode");
	REQUIRE(snprintf(xattr, sizeof(xattr), "user.%s", collisions[0]) < (int)sizeof(xattr));
	refused(transaction,
	    btrfs_transaction_set_xattr(
		transaction, one, xattr, strlen(xattr), "x", 1, BTRFS_XATTR_CREATE, time),
	    BTRFS_EXISTS, "create an existing xattr");
	refused(transaction,
	    btrfs_transaction_set_xattr(
		transaction, one, "user.none", 9, "x", 1, BTRFS_XATTR_REPLACE, time),
	    BTRFS_NOT_FOUND, "replace a missing xattr");
	refused(transaction, btrfs_transaction_remove_xattr(transaction, one, "user.none", 9, time),
	    BTRFS_NOT_FOUND, "remove a missing xattr");
	value = calloc(1, limit);
	REQUIRE(value != NULL);
	refused(transaction,
	    btrfs_transaction_set_xattr(transaction, one, "user.big", 8, value,
		limit - sizeof(struct bt_disk_dir) - 8 + 1, 0, time),
	    BTRFS_NO_SPACE, "an xattr beyond one item");
	free(value);
	refused(transaction,
	    btrfs_transaction_set_xattr(
		transaction, one, long_name, BTRFS_NAME_MAX + 1, "x", 1, 0, time),
	    BTRFS_RANGE, "a long xattr name");
	refused(transaction, btrfs_transaction_set_xattr(transaction, one, "", 0, "x", 1, 0, time),
	    BTRFS_INVALID_ARGUMENT, "an empty xattr name");
	refused(transaction,
	    btrfs_transaction_set_xattr(transaction, one, "user.x", 6, "x", 1, 4, time),
	    BTRFS_INVALID_ARGUMENT, "unknown xattr flags");
	refused(transaction, btrfs_transaction_evict(transaction, one), BTRFS_INVALID_ARGUMENT,
	    "evict a linked inode");
	refused(transaction,
	    btrfs_transaction_set_xattr(transaction, one, "btrfs.other", 11, "x", 1, 0, time),
	    BTRFS_INVALID_ARGUMENT, "an unknown property");
	refused(transaction,
	    btrfs_transaction_set_xattr(
		transaction, one, "btrfs.compression", 17, "bogus", 5, 0, time),
	    BTRFS_INVALID_ARGUMENT, "an invalid compression property");
	refused(transaction,
	    btrfs_transaction_set_xattr(
		transaction, object(fs, "/ns/nocow"), "btrfs.compression", 17, "zstd", 4, 0, time),
	    BTRFS_INVALID_ARGUMENT, "compression without data checksums");
	refused(transaction,
	    btrfs_transaction_remove_xattr(transaction, one, "btrfs.compression", 17, time),
	    BTRFS_NOT_FOUND, "remove a missing property");
	/* Linux ignores compression on objects other than files and directories. */
	refused(transaction,
	    btrfs_transaction_set_xattr(
		transaction, object(fs, "/symlink"), "btrfs.compression", 17, "zstd", 4, 0, time),
	    BTRFS_OK, "compression on a symlink");
	REQUIRE(transaction->changed == 0);
	/* The same transaction still commits a change. */
	REQUIRE(btrfs_transaction_create(transaction, ns, "after", 5, &file, &id) == BTRFS_OK);
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	audit_state(context, "namespace-refusals");
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(object(fs, "/ns/after").inode == id.inode);
	btrfs_unmount(fs);
	truncate_writes(context->device, 0);
	REQUIRE(context->image.live_allocations == 0);
	printf("namespace refusals PASS\n");
}

/* Limits of numbering and of packed items, refused before any change. */
static void
namespace_limits(struct context *context)
{
	struct btrfs_new_inode file = new_inode(BTRFS_MODE_REGULAR | 0644);
	struct btrfs_new_inode directory = new_inode(BTRFS_MODE_DIRECTORY | 0755);
	struct bt_disk_inode inode;
	struct bt_owned_root *tree;
	struct btrfs_object_id ns;
	struct btrfs_object_id empty;
	struct btrfs_object_id id;
	struct btrfs_object_id *made;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct btrfs_time time = { 1800000000, 0 };
	struct bt_key key;
	char name[COLLISION_NAME_BYTES + 1];
	size_t fits = item_limit(context) / (sizeof(struct bt_disk_dir) + COLLISION_NAME_BYTES);
	size_t i;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	ns = object(fs, "/ns");
	empty = object(fs, "/ns/empty");

	/* The last directory index is UINT64_MAX; then the directory is full. */
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	REQUIRE(bt_tx_tree(transaction, BTRFS_TOP_LEVEL_TREE, &tree) == BTRFS_OK);
	key = (struct bt_key){ empty.inode, UINT64_MAX - 1, BT_DIR_INDEX };
	REQUIRE(bt_tx_edit(transaction, &tree->root, key, "x", 1, BT_INSERT) == BTRFS_OK);
	REQUIRE(btrfs_transaction_create(transaction, empty, "a", 1, &file, &id) == BTRFS_OK);
	refused(transaction, btrfs_transaction_create(transaction, empty, "b", 1, &file, &id),
	    BTRFS_RANGE, "create beyond the last directory index");
	btrfs_transaction_destroy(transaction);

	/* Inode numbers end below BTRFS_LAST_FREE_OBJECTID. */
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	REQUIRE(bt_tx_tree(transaction, BTRFS_TOP_LEVEL_TREE, &tree) == BTRFS_OK);
	memset(&inode, 0, sizeof(inode));
	key = (struct bt_key){ BT_LAST_FREE_OBJECTID - 2, 0, BT_INODE_ITEM };
	REQUIRE(bt_tx_edit(transaction, &tree->root, key, &inode, sizeof(inode), BT_INSERT) ==
	    BTRFS_OK);
	REQUIRE(btrfs_transaction_create(transaction, empty, "a", 1, &file, &id) == BTRFS_OK);
	REQUIRE(id.inode == BT_LAST_FREE_OBJECTID - 1);
	refused(transaction, btrfs_transaction_create(transaction, empty, "b", 1, &file, &id),
	    BTRFS_NO_SPACE, "create beyond the last inode number");
	btrfs_transaction_destroy(transaction);

	/* A transaction numbers entries of a bounded set of directories. */
	made = calloc(BT_TRANSACTION_INDEXES + 1, sizeof(*made));
	REQUIRE(made != NULL);
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	for (i = 0; i <= BT_TRANSACTION_INDEXES; i++) {
		REQUIRE(snprintf(name, sizeof(name), "d%03zu", i) < (int)sizeof(name));
		REQUIRE(btrfs_transaction_create(
			    transaction, ns, name, strlen(name), &directory, &made[i]) == BTRFS_OK);
	}
	for (i = 0; i + 1 < BT_TRANSACTION_INDEXES; i++) {
		REQUIRE(
		    btrfs_transaction_create(transaction, made[i], "f", 1, &file, &id) == BTRFS_OK);
	}
	refused(transaction, btrfs_transaction_create(transaction, made[i], "f", 1, &file, &id),
	    BTRFS_UNSUPPORTED, "index more directories than one transaction tracks");
	REQUIRE(btrfs_transaction_create(transaction, ns, "later", 5, &file, &id) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	free(made);

	/* Colliding names fill their DIR_ITEM; Linux reports EOVERFLOW. */
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	for (i = 0; i < fits; i++) {
		collision_name(name, i);
		REQUIRE(btrfs_transaction_create(
			    transaction, empty, name, strlen(name), &file, &id) == BTRFS_OK);
	}
	collision_name(name, fits);
	refused(transaction,
	    btrfs_transaction_create(transaction, empty, name, strlen(name), &file, &id),
	    BTRFS_RANGE, "create beyond a full DIR_ITEM");
	refused(transaction,
	    btrfs_transaction_link(
		transaction, object(fs, "/ns/one"), empty, name, strlen(name), time),
	    BTRFS_RANGE, "link beyond a full DIR_ITEM");
	refused(transaction,
	    btrfs_transaction_rename(
		transaction, ns, "victim", 6, empty, name, strlen(name), time, 0),
	    BTRFS_RANGE, "rename beyond a full DIR_ITEM");
	collision_name(name, 0);
	REQUIRE(
	    btrfs_transaction_unlink(transaction, empty, name, strlen(name), time, 0) == BTRFS_OK);
	collision_name(name, fits);
	REQUIRE(btrfs_transaction_create(transaction, empty, name, strlen(name), &file, &id) ==
	    BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	REQUIRE(context->device->count == 0);
	REQUIRE(context->image.live_allocations == 0);
	printf("namespace limits: %zu colliding names per DIR_ITEM PASS\n", fits);
}

/* The namespace audit rejects committed states that Linux would reject. */
static void
namespace_audit_self_test(struct context *context)
{
	struct namespace_audit audit;
	struct bt_disk_inode inode;
	struct bt_owned_root *tree;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct bt_key key;
	size_t length;
	size_t pass;

	for (pass = 0; pass < 2; pass++) {
		REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
		key = (struct bt_key){ object(fs, pass == 0 ? "/ns/one" : "/ns/tree").inode, 0,
			BT_INODE_ITEM };
		REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
		REQUIRE(bt_tx_tree(transaction, BTRFS_TOP_LEVEL_TREE, &tree) == BTRFS_OK);
		REQUIRE(bt_mutation_find(transaction->mutation, tree->root, key, &inode,
			    sizeof(inode), &length) == BTRFS_OK);
		if (pass == 0) {
			bt_put32(&inode.links, bt_u32(inode.links) + 1);
		} else {
			bt_put64(&inode.size, bt_u64(inode.size) + 2);
		}
		REQUIRE(bt_tx_edit(transaction, &tree->root, key, &inode, sizeof(inode),
			    BT_REPLACE) == BTRFS_OK);
		transaction->changed = 1;
		REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
		btrfs_transaction_destroy(transaction);
		btrfs_unmount(fs);
		REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
		REQUIRE(namespace_audit(fs, &audit) != 0);
		printf("namespace audit detects: %s\n", audit.failure);
		btrfs_unmount(fs);
		truncate_writes(context->device, 0);
	}
	REQUIRE(context->image.live_allocations == 0);
}

/* Sets inode flags directly, as chattr would, inside a transaction. */
static void
set_inode_flags(struct btrfs_transaction *transaction, struct btrfs_object_id id, uint64_t flags)
{
	struct bt_disk_inode inode;
	struct bt_owned_root *tree;
	struct bt_key key = { id.inode, 0, BT_INODE_ITEM };
	size_t length;

	REQUIRE(bt_tx_tree(transaction, id.tree, &tree) == BTRFS_OK);
	REQUIRE(bt_mutation_find(transaction->mutation, tree->root, key, &inode, sizeof(inode),
		    &length) == BTRFS_OK);
	bt_put64(&inode.flags, bt_u64(inode.flags) | flags);
	REQUIRE(bt_tx_edit(transaction, &tree->root, key, &inode, sizeof(inode), BT_REPLACE) ==
	    BTRFS_OK);
}

/* Linux's immutable and append-only rules and the set-id decision, refused
 * before any change. */
static void
namespace_flag_refusals(struct context *context)
{
	struct btrfs_new_inode file = new_inode(BTRFS_MODE_REGULAR | 0644);
	struct btrfs_new_inode setuid = new_inode(BTRFS_MODE_REGULAR | 04755);
	struct btrfs_attributes changes;
	struct btrfs_object_id ns;
	struct btrfs_object_id victim;
	struct btrfs_object_id clone;
	struct btrfs_object_id full;
	struct btrfs_object_id tree;
	struct btrfs_object_id id;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct btrfs_time time = { 1800000000, 0 };

	memset(&changes, 0, sizeof(changes));
	changes.mask = BTRFS_ATTRIBUTE_MODE;
	changes.mode = 0600;
	changes.time = time;
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	ns = object(fs, "/ns");
	victim = object(fs, "/ns/victim");
	clone = object(fs, "/ns/clone");
	full = object(fs, "/ns/full");
	tree = object(fs, "/ns/tree");
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	set_inode_flags(transaction, victim, BT_INODE_IMMUTABLE);
	set_inode_flags(transaction, clone, BT_INODE_APPEND);
	set_inode_flags(transaction, full, BT_INODE_IMMUTABLE);
	set_inode_flags(transaction, tree, BT_INODE_APPEND);
	refused(transaction, btrfs_transaction_write(transaction, victim, 0, "x", 1, time),
	    BTRFS_NOT_PERMITTED, "write an immutable file");
	refused(transaction, btrfs_transaction_truncate(transaction, victim, 0, time),
	    BTRFS_NOT_PERMITTED, "truncate an immutable file");
	refused(transaction, btrfs_transaction_unlink(transaction, ns, "victim", 6, time, 0),
	    BTRFS_NOT_PERMITTED, "unlink an immutable file");
	refused(transaction,
	    btrfs_transaction_rename(transaction, ns, "victim", 6, ns, "moved", 5, time, 0),
	    BTRFS_NOT_PERMITTED, "rename an immutable file");
	refused(transaction, btrfs_transaction_link(transaction, victim, ns, "linked", 6, time),
	    BTRFS_NOT_PERMITTED, "link an immutable file");
	refused(transaction,
	    btrfs_transaction_set_xattr(transaction, victim, "user.x", 6, "x", 1, 0, time),
	    BTRFS_NOT_PERMITTED, "set an xattr of an immutable file");
	refused(transaction, btrfs_transaction_set_attributes(transaction, victim, &changes),
	    BTRFS_NOT_PERMITTED, "chmod an immutable file");
	refused(transaction, btrfs_transaction_drop_privileges(transaction, victim, time),
	    BTRFS_NOT_PERMITTED, "drop privileges of an immutable file");
	refused(transaction, btrfs_transaction_write(transaction, clone, 0, "x", 1, time),
	    BTRFS_NOT_PERMITTED, "overwrite an append-only file");
	refused(transaction, btrfs_transaction_truncate(transaction, clone, 0, time),
	    BTRFS_NOT_PERMITTED, "truncate an append-only file");
	refused(transaction, btrfs_transaction_unlink(transaction, ns, "clone", 5, time, 0),
	    BTRFS_NOT_PERMITTED, "unlink an append-only file");
	refused(transaction, btrfs_transaction_set_attributes(transaction, clone, &changes),
	    BTRFS_NOT_PERMITTED, "chmod an append-only file");
	refused(transaction, btrfs_transaction_create(transaction, full, "x", 1, &file, &id),
	    BTRFS_NOT_PERMITTED, "create in an immutable directory");
	refused(transaction, btrfs_transaction_unlink(transaction, full, "inner", 5, time, 0),
	    BTRFS_NOT_PERMITTED, "unlink from an immutable directory");
	refused(transaction,
	    btrfs_transaction_rename(transaction, ns, "one", 3, full, "one", 3, time, 0),
	    BTRFS_NOT_PERMITTED, "rename into an immutable directory");
	refused(transaction, btrfs_transaction_unlink(transaction, tree, "one-link", 8, time, 0),
	    BTRFS_NOT_PERMITTED, "unlink from an append-only directory");
	refused(transaction,
	    btrfs_transaction_rename(transaction, tree, "one-link", 8, ns, "moved", 5, time, 0),
	    BTRFS_NOT_PERMITTED, "rename out of an append-only directory");
	/* Appending and creating in an append-only directory remain allowed. */
	REQUIRE(btrfs_transaction_write(transaction, clone, 65536, "tail", 4, time) == BTRFS_OK);
	REQUIRE(btrfs_transaction_create(transaction, tree, "added", 5, &file, &id) == BTRFS_OK);
	REQUIRE(btrfs_transaction_create(transaction, ns, "suid", 4, &setuid, &id) == BTRFS_OK);
	refused(transaction, btrfs_transaction_write(transaction, id, 0, "x", 1, time),
	    BTRFS_UNSUPPORTED, "write a set-id file without a decision");
	refused(transaction, btrfs_transaction_truncate(transaction, id, 0, time),
	    BTRFS_UNSUPPORTED, "truncate a set-id file without a decision");
	REQUIRE(btrfs_transaction_keep_privileges(transaction, id) == BTRFS_OK);
	REQUIRE(btrfs_transaction_write(transaction, id, 0, "x", 1, time) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	REQUIRE(context->device->count == 0);
	REQUIRE(context->image.live_allocations == 0);
	printf("immutable, append-only and set-id refusals PASS\n");
}

void
namespace_scenarios(struct context *context)
{
	namespace_refusals(context);
	namespace_flag_refusals(context);
	namespace_limits(context);
	namespace_audit_self_test(context);
	namespace_create_plan(context);
	namespace_collide_plan(context);
	namespace_link_plan(context);
	namespace_unlink_plan(context);
	namespace_rename_plan(context);
	namespace_open_plan(context);
	namespace_orphan_plan(context);
	namespace_xattr_plan(context);
	namespace_subvolume_plan(context);
	namespace_property_plan(context);
	namespace_attributes_plan(context);
	namespace_full_item_plan(context);
}
