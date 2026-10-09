/* SPDX-License-Identifier: BSD-3-Clause */
/* Source-controlled plans: operations per commit, the byte model of tracked
 * files, namespace expectations, and the checks of a stage. */
#define _POSIX_C_SOURCE 200809L
#include "scenario.h"

static void
check_text(struct btrfs_fs *fs, const char *path, const char *expected)
{
	struct btrfs_inode inode;
	uint8_t buffer[64];
	size_t completed;

	REQUIRE(btrfs_image_lookup(fs, path, &inode) == BTRFS_OK);
	REQUIRE(btrfs_read(fs, &inode, 0, buffer, sizeof(buffer), &completed) == BTRFS_OK);
	REQUIRE(completed == strlen(expected) && memcmp(buffer, expected, completed) == 0);
}

/* Objects outside the transaction's write set keep their Linux contents. */
/* Linux converts a block group's free space between extent items and bitmaps
 * as soon as its extent count crosses a threshold, so no state holds an
 * extent-mode group above the high threshold or a bitmap group below the low
 * one (set_free_space_tree_thresholds, derived here independently). */
static void
check_free_space_thresholds(struct btrfs_fs *fs)
{
	const struct bt_disk_free_space_info *info;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	struct bt_key key;
	uint64_t range = (uint64_t)fs->info.sector_size * LINUX_FREE_SPACE_BITMAP_BITS;
	uint64_t bitmaps;
	uint64_t high;
	uint64_t low;
	uint32_t count;
	size_t i;

	if (bt_find_root(fs, BT_FREE_SPACE_TREE, &root) != BTRFS_OK) {
		return;
	}
	for (i = 0; i < fs->chunk_count; i++) {
		key = (struct bt_key){ fs->chunks[i].logical, fs->chunks[i].length,
			BT_FREE_SPACE_INFO };
		bt_cursor_init(&cursor, fs, root);
		REQUIRE(bt_cursor_seek(&cursor, key, 0) == BTRFS_OK);
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		REQUIRE(bt_key_compare(record.key, key) == 0 && record.size == sizeof(*info));
		info = (const void *)record.data;
		bitmaps = (fs->chunks[i].length + range - 1) / range;
		high =
		    bitmaps * (LINUX_ITEM_BYTES + LINUX_FREE_SPACE_BITMAP_BYTES) / LINUX_ITEM_BYTES;
		low = high > LINUX_FREE_SPACE_MARGIN ? high - LINUX_FREE_SPACE_MARGIN : 0;
		count = bt_u32(info->extent_count);
		if ((bt_u32(info->flags) & BT_FREE_SPACE_USING_BITMAPS) ? count < low
									: count > high) {
			fprintf(stderr,
			    "block group %llu: %u free extents as %s (thresholds %llu, %llu)\n",
			    (unsigned long long)fs->chunks[i].logical, count,
			    (bt_u32(info->flags) & BT_FREE_SPACE_USING_BITMAPS) ? "bitmaps"
										: "extents",
			    (unsigned long long)low, (unsigned long long)high);
			exit(1);
		}
		bt_cursor_fini(&cursor);
	}
}

void
check_invariants(struct btrfs_fs *fs)
{
	struct btrfs_inode inode;
	struct btrfs_inode link;
	uint8_t value[32];
	size_t length;

	REQUIRE(btrfs_image_lookup(fs, "/greeting", &inode) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/hardlink", &link) == BTRFS_OK);
	REQUIRE(inode.id.tree == link.id.tree && inode.id.inode == link.id.inode);
	REQUIRE(inode.links == 2 && inode.uid == 1001 && inode.gid == 1002 &&
	    (inode.mode & 07777U) == 0640);
	REQUIRE(btrfs_get_xattr(fs, &inode, "user.text", strlen("user.text"), value, sizeof(value),
		    &length) == BTRFS_OK);
	REQUIRE(length == strlen("Linux xattr") && memcmp(value, "Linux xattr", length) == 0);
	check_text(fs, "/snapshot/value", "snapshot original\n");
	check_text(fs, "/subvol/value", "subvolume changed\n");
	check_free_space_thresholds(fs);
}

static uint8_t *
read_file(struct btrfs_fs *fs, const char *path, size_t *size)
{
	struct btrfs_inode inode;
	uint8_t *buffer;
	size_t completed;

	REQUIRE(btrfs_image_lookup(fs, path, &inode) == BTRFS_OK);
	REQUIRE(inode.size <= MAX_FILE_BYTES);
	buffer = malloc((size_t)inode.size + 1);
	REQUIRE(buffer != NULL);
	REQUIRE(btrfs_read(fs, &inode, 0, buffer, (size_t)inode.size, &completed) == BTRFS_OK);
	REQUIRE(completed == inode.size);
	*size = completed;
	return buffer;
}

static int
compare_entries(const void *left, const void *right)
{
	const struct btrfs_dir_entry *a = left;
	const struct btrfs_dir_entry *b = right;
	size_t length = a->name_length < b->name_length ? a->name_length : b->name_length;
	int order = memcmp(a->name, b->name, length);

	if (order != 0) {
		return order;
	}
	return (a->name_length > b->name_length) - (a->name_length < b->name_length);
}

/* Lists a directory through DIR_INDEX enumeration and confirms that each name
 * resolves through its DIR_ITEM to the same object. */
static uint8_t *
list_directory(struct btrfs_fs *fs, const struct btrfs_inode *directory, size_t *size)
{
	struct btrfs_dir_entry *entries = NULL;
	struct btrfs_inode child;
	uint8_t *listing;
	uint64_t cookie = 0;
	size_t count = 0;
	size_t capacity = 0;
	size_t used = 0;
	size_t i;
	enum btrfs_result result;

	for (;;) {
		if (count == capacity) {
			capacity = capacity == 0 ? 64 : capacity * 2;
			entries = realloc(entries, capacity * sizeof(*entries));
			REQUIRE(entries != NULL);
		}
		result = btrfs_next_dir(fs, directory, &cookie, &entries[count]);
		if (result == BTRFS_NOT_FOUND) {
			break;
		}
		REQUIRE(result == BTRFS_OK);
		REQUIRE(btrfs_lookup(fs, directory, entries[count].name, entries[count].name_length,
			    &child) == BTRFS_OK);
		REQUIRE(child.id.tree == entries[count].id.tree &&
		    child.id.inode == entries[count].id.inode);
		used += entries[count].name_length + 1U;
		count++;
	}
	qsort(entries, count, sizeof(*entries), compare_entries);
	listing = malloc(used + 1);
	REQUIRE(listing != NULL);
	*size = 0;
	for (i = 0; i < count; i++) {
		memcpy(listing + *size, entries[i].name, entries[i].name_length);
		*size += entries[i].name_length;
		listing[(*size)++] = '\n';
	}
	free(entries);
	return listing;
}

/* Which back reference holds the last name of path: 0 for INODE_REF, 1 for
 * INODE_EXTREF, -1 for none. */
static int
reference_kind(struct btrfs_fs *fs, const char *path, const struct btrfs_inode *inode)
{
	const struct bt_disk_inode_ref *ref;
	const struct bt_disk_inode_extref *extref;
	struct btrfs_inode parent;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	struct bt_key key;
	char directory[LINUX_PATH_MAX];
	const char *name = strrchr(path, '/') + 1;
	size_t length = strlen(name);
	size_t position;
	size_t entry;
	int kind = -1;
	int pass;

	REQUIRE(name - path < (ptrdiff_t)sizeof(directory));
	memcpy(directory, path, (size_t)(name - path));
	directory[name - path == 1 ? 1 : name - path - 1] = '\0';
	REQUIRE(btrfs_image_lookup(fs, directory, &parent) == BTRFS_OK);
	REQUIRE(bt_find_root(fs, inode->id.tree, &root) == BTRFS_OK);
	for (pass = 0; pass < 2 && kind < 0; pass++) {
		key = pass == 0
		    ? (struct bt_key){ inode->id.inode, parent.id.inode, BT_INODE_REF }
		    : (struct bt_key){ inode->id.inode,
			      bt_crc32c((uint32_t)parent.id.inode, name, length), BT_INODE_EXTREF };
		bt_cursor_init(&cursor, fs, root);
		if (bt_cursor_seek(&cursor, key, 0) == BTRFS_OK &&
		    bt_cursor_record(&cursor, &record) == BTRFS_OK &&
		    bt_key_compare(record.key, key) == 0) {
			for (position = 0; kind < 0 && position < record.size; position += entry) {
				if (pass == 0) {
					ref = (const void *)(record.data + position);
					entry = sizeof(*ref) + bt_u16(ref->name_length);
					kind = bt_u16(ref->name_length) == length &&
						memcmp(ref + 1, name, length) == 0
					    ? 0
					    : -1;
				} else {
					extref = (const void *)(record.data + position);
					entry = sizeof(*extref) + bt_u16(extref->name_length);
					kind = bt_u64(extref->parent) == parent.id.inode &&
						bt_u16(extref->name_length) == length &&
						memcmp(extref + 1, name, length) == 0
					    ? 1
					    : -1;
				}
			}
		}
		bt_cursor_fini(&cursor);
	}
	return kind;
}

/* The newest root item of tree. */
static void
root_item(struct btrfs_fs *fs, uint64_t tree, struct bt_disk_root_full *item)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { tree, UINT64_MAX, BT_ROOT_ITEM };

	bt_cursor_init(&cursor, fs, fs->root_tree);
	REQUIRE(bt_cursor_seek(&cursor, key, 1) == BTRFS_OK);
	REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
	REQUIRE(record.key.objectid == tree && record.key.type == BT_ROOT_ITEM &&
	    record.size == sizeof(*item));
	memcpy(item, record.data, sizeof(*item));
	bt_cursor_fini(&cursor);
}

/* The number of live subvolumes below the top level (or, with dead, of
 * deleted ones the cleaner has not dropped). */
static size_t
count_subvolumes(struct btrfs_fs *fs, int dead)
{
	const struct bt_disk_root *item;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key first = { 0, 0, 0 };
	size_t count = 0;
	enum btrfs_result result;

	bt_cursor_init(&cursor, fs, fs->root_tree);
	result = bt_cursor_seek(&cursor, first, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		item = (const void *)record.data;
		count += record.key.type == BT_ROOT_ITEM &&
		    record.key.objectid >= BTRFS_ROOT_INODE &&
		    record.key.objectid < BT_LAST_FREE_OBJECTID &&
		    (bt_u32(item->refs) == 0) == dead;
		result = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	return count;
}

static void
expectation_failed(
    const struct plan *plan, size_t stage, const struct expectation *expectation, const char *what)
{
	fprintf(stderr, "%s stage %zu: %s: %s\n", plan->name, stage, expectation->path, what);
	exit(1);
}

static void
check_subvolume(struct btrfs_fs *fs, const struct plan *plan, size_t stage,
    const struct expectation *e, const struct btrfs_inode *inode)
{
	static const uint8_t none[BTRFS_UUID_SIZE];
	struct bt_disk_root_full item;
	struct bt_disk_root_full source;
	struct btrfs_inode origin;

	if (inode->id.inode != BTRFS_ROOT_INODE || inode->id.tree < BTRFS_ROOT_INODE) {
		expectation_failed(plan, stage, e, "not a subvolume");
	}
	root_item(fs, inode->id.tree, &item);
	if (((bt_u64(item.legacy.flags) & BT_ROOT_SUBVOL_READ_ONLY) != 0) != (e->value != 0)) {
		expectation_failed(plan, stage, e, "read-only flag");
	}
	if (e->other == NULL) {
		if (memcmp(item.parent_uuid, none, sizeof(none)) != 0) {
			expectation_failed(plan, stage, e, "parent UUID");
		}
		return;
	}
	REQUIRE(btrfs_image_lookup(fs, e->other, &origin) == BTRFS_OK);
	root_item(fs, origin.id.tree, &source);
	if (memcmp(item.parent_uuid, source.uuid, sizeof(source.uuid)) != 0) {
		expectation_failed(plan, stage, e, "parent UUID");
	}
}

/* Counts the inode's compressed file extent items by codec and kind. */
static void
check_compressed(struct btrfs_fs *fs, const struct plan *plan, size_t stage,
    const struct expectation *e, const struct btrfs_inode *inode)
{
	const struct bt_disk_extent_header *extent;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	struct bt_key key = { inode->id.inode, 0, BT_EXTENT_DATA };
	uint32_t regular = 0;
	uint32_t inline_extents = 0;
	uint32_t other = 0;
	enum btrfs_result result;

	REQUIRE(bt_find_root(fs, inode->id.tree, &root) == BTRFS_OK);
	bt_cursor_init(&cursor, fs, root);
	result = bt_cursor_seek(&cursor, key, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		if (record.key.objectid != inode->id.inode || record.key.type != BT_EXTENT_DATA) {
			break;
		}
		REQUIRE(record.size >= sizeof(*extent));
		extent = (const void *)record.data;
		if (extent->compression != BTRFS_COMPRESSION_NONE) {
			if (extent->compression != e->value) {
				other++;
			} else if (extent->type == BT_EXTENT_INLINE) {
				inline_extents++;
			} else {
				regular++;
			}
		}
		result = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (regular != e->links || inline_extents != e->mode || other != 0) {
		fprintf(stderr, "%s stage %zu: %s: %u compressed extents, %u inline, %u other\n",
		    plan->name, stage, e->path, regular, inline_extents, other);
		exit(1);
	}
}

static void
check_subvolumes(
    struct btrfs_fs *fs, const struct plan *plan, size_t stage, const struct expectation *e)
{
	struct btrfs_inode inode;
	char path[LINUX_PATH_MAX];
	size_t start = 0;
	size_t end;
	size_t count = 0;

	while (start < e->size) {
		for (end = start; end < e->size && e->bytes[end] != '\n'; end++) {
		}
		REQUIRE(end - start + 2 <= sizeof(path));
		path[0] = '/';
		memcpy(path + 1, e->bytes + start, end - start);
		path[end - start + 1] = '\0';
		if (btrfs_image_lookup(fs, path, &inode) != BTRFS_OK ||
		    inode.id.inode != BTRFS_ROOT_INODE || inode.id.tree < BTRFS_ROOT_INODE) {
			fprintf(stderr, "%s stage %zu: %s is not a subvolume\n", plan->name, stage,
			    path);
			exit(1);
		}
		count++;
		start = end + 1;
	}
	if (count != count_subvolumes(fs, 0)) {
		expectation_failed(plan, stage, e, "subvolume count");
	}
}

/* Whether bytes (the expected size) differ from e's only inside the volatile
 * ranges crash_commit wrote in place, each sector holding e's bytes or the
 * crash commit's. */
static int
volatile_match(const struct plan *plan, size_t stage, size_t crash_commit,
    const struct expectation *e, const uint8_t *bytes, size_t size)
{
	const struct expectation *next = NULL;
	const struct volatile_range *range;
	size_t sector;
	size_t start;
	size_t length;
	size_t i;
	int inside;

	if (crash_commit != stage + 1 || size != e->size) {
		return 0;
	}
	for (i = 0; i < plan->expectation_count && next == NULL; i++) {
		if (plan->expectations[i].kind == EXPECT_FILE &&
		    strcmp(plan->expectations[i].path, e->path) == 0 &&
		    plan->expectations[i].first <= crash_commit &&
		    crash_commit <= plan->expectations[i].last) {
			next = &plan->expectations[i];
		}
	}
	if (next == NULL || next->size != size) {
		return 0;
	}
	for (sector = 0; sector < size; sector += DEVICE_VOLATILE_SECTOR) {
		start = sector;
		length =
		    size - sector < DEVICE_VOLATILE_SECTOR ? size - sector : DEVICE_VOLATILE_SECTOR;
		if (memcmp(bytes + start, e->bytes + start, length) == 0) {
			continue;
		}
		inside = 0;
		for (i = 0; i < plan->volatile_count; i++) {
			range = &plan->volatiles[i];
			inside |= range->commit == crash_commit &&
			    strcmp(range->path, e->path) == 0 && start >= range->offset &&
			    start + length <= range->offset + range->length;
		}
		if (!inside || memcmp(bytes + start, next->bytes + start, length) != 0) {
			return 0;
		}
	}
	return 1;
}

/* Counts the inode's regular and preallocated file extent items and their
 * distinct disk extents. */
static void
check_extents(struct btrfs_fs *fs, const struct plan *plan, size_t stage,
    const struct expectation *e, const struct btrfs_inode *inode)
{
	const struct bt_disk_extent *extent;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	struct bt_key key = { inode->id.inode, 0, BT_EXTENT_DATA };
	uint64_t disks[MAX_EXTENT_DISKS];
	uint32_t regular = 0;
	uint32_t prealloc = 0;
	size_t distinct = 0;
	size_t i;
	enum btrfs_result result;

	REQUIRE(bt_find_root(fs, inode->id.tree, &root) == BTRFS_OK);
	bt_cursor_init(&cursor, fs, root);
	result = bt_cursor_seek(&cursor, key, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		if (record.key.objectid != inode->id.inode || record.key.type != BT_EXTENT_DATA) {
			break;
		}
		extent = (const void *)record.data;
		if (record.size == sizeof(*extent) && bt_u64(extent->disk_bytenr) != 0) {
			regular += extent->header.type == BT_EXTENT_REGULAR;
			prealloc += extent->header.type == BT_EXTENT_PREALLOC;
			for (i = 0; i < distinct && disks[i] != bt_u64(extent->disk_bytenr); i++) {
			}
			if (i == distinct) {
				REQUIRE(distinct < MAX_EXTENT_DISKS);
				disks[distinct++] = bt_u64(extent->disk_bytenr);
			}
		}
		result = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (regular != e->links || prealloc != e->mode || distinct != e->value) {
		fprintf(stderr, "%s stage %zu: %s: %u regular, %u preallocated, %zu disk extents\n",
		    plan->name, stage, e->path, regular, prealloc, distinct);
		exit(1);
	}
}

static void
check_holes(struct btrfs_fs *fs, const struct plan *plan, size_t stage, const struct expectation *e,
    const struct btrfs_inode *inode)
{
	const struct bt_disk_extent *extent;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	struct bt_key key = { inode->id.inode, 0, BT_EXTENT_DATA };
	uint64_t holes = 0;
	enum btrfs_result result;

	REQUIRE(bt_find_root(fs, inode->id.tree, &root) == BTRFS_OK);
	bt_cursor_init(&cursor, fs, root);
	result = bt_cursor_seek(&cursor, key, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		if (record.key.objectid != inode->id.inode || record.key.type != BT_EXTENT_DATA) {
			break;
		}
		extent = (const void *)record.data;
		holes += record.size == sizeof(*extent) &&
		    extent->header.type == BT_EXTENT_REGULAR && bt_u64(extent->disk_bytenr) == 0;
		result = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (holes != e->value) {
		fprintf(stderr, "%s stage %zu: %s: %llu hole items\n", plan->name, stage, e->path,
		    (unsigned long long)holes);
		exit(1);
	}
}

static void
check_bitmaps(struct btrfs_fs *fs, const struct plan *plan, size_t stage,
    const struct expectation *e, const struct btrfs_inode *inode)
{
	const struct bt_disk_free_space_info *info;
	const struct bt_disk_extent *extent;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	struct bt_key key = { inode->id.inode, 0, BT_EXTENT_DATA };
	uint64_t address = 0;
	uint64_t bitmaps = 2;
	size_t i;
	enum btrfs_result result;

	REQUIRE(bt_find_root(fs, inode->id.tree, &root) == BTRFS_OK);
	bt_cursor_init(&cursor, fs, root);
	result = bt_cursor_seek(&cursor, key, 0);
	while (result == BTRFS_OK && address == 0) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		if (record.key.objectid != inode->id.inode || record.key.type != BT_EXTENT_DATA) {
			break;
		}
		extent = (const void *)record.data;
		if (record.size == sizeof(*extent)) {
			address = bt_u64(extent->disk_bytenr);
		}
		result = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	REQUIRE(bt_find_root(fs, BT_FREE_SPACE_TREE, &root) == BTRFS_OK);
	for (i = 0; address != 0 && i < fs->chunk_count; i++) {
		if (address - fs->chunks[i].logical >= fs->chunks[i].length) {
			continue;
		}
		key = (struct bt_key){ fs->chunks[i].logical, fs->chunks[i].length,
			BT_FREE_SPACE_INFO };
		bt_cursor_init(&cursor, fs, root);
		REQUIRE(bt_cursor_seek(&cursor, key, 0) == BTRFS_OK);
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		REQUIRE(bt_key_compare(record.key, key) == 0 && record.size == sizeof(*info));
		info = (const void *)record.data;
		bitmaps = (bt_u32(info->flags) & BT_FREE_SPACE_USING_BITMAPS) != 0;
		bt_cursor_fini(&cursor);
	}
	if (bitmaps != e->value) {
		fprintf(stderr, "%s stage %zu: %s: data at %llu, bitmaps %llu\n", plan->name, stage,
		    e->path, (unsigned long long)address, (unsigned long long)bitmaps);
		for (i = 0; i < fs->chunk_count; i++) {
			key = (struct bt_key){ fs->chunks[i].logical, fs->chunks[i].length,
				BT_FREE_SPACE_INFO };
			bt_cursor_init(&cursor, fs, root);
			if (bt_cursor_seek(&cursor, key, 0) == BTRFS_OK &&
			    bt_cursor_record(&cursor, &record) == BTRFS_OK &&
			    record.size == sizeof(*info)) {
				info = (const void *)record.data;
				fprintf(stderr, "  group %llu: %u free extents, flags %u\n",
				    (unsigned long long)fs->chunks[i].logical,
				    bt_u32(info->extent_count), bt_u32(info->flags));
			}
			bt_cursor_fini(&cursor);
		}
		exit(1);
	}
}

/* The tree holding block-group items: the block-group tree when the filesystem
 * has one, otherwise the extent tree. */
struct bt_root
group_root(const struct btrfs_fs *fs)
{
	struct bt_root root;
	uint64_t tree = (fs->info.readonly_features & BT_COMPAT_RO_BLOCK_GROUP_TREE) != 0
	    ? BT_BLOCK_GROUP_TREE
	    : BT_EXTENT_TREE;

	REQUIRE(bt_find_root(fs, tree, &root) == BTRFS_OK);
	return root;
}

/* Block groups as chunk items and as block group items, which must agree,
 * and the entries of the primary superblock's system chunk array. */
static void
check_groups(
    struct btrfs_fs *fs, const struct plan *plan, size_t stage, const struct expectation *e)
{
	const struct bt_disk_chunk *chunk;
	struct bt_disk_super super;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	struct bt_key key = { 0, 0, BT_BLOCK_GROUP_ITEM };
	uint64_t groups = 0;
	uint32_t entries = 0;
	size_t offset = 0;
	enum btrfs_result result;

	root = group_root(fs);
	bt_cursor_init(&cursor, fs, root);
	result = bt_cursor_seek(&cursor, key, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		groups += record.key.type == BT_BLOCK_GROUP_ITEM;
		result = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	REQUIRE(fs->env.read(fs->env.context, BT_SUPER_OFFSET, &super, sizeof(super)) == BTRFS_OK);
	while (offset < bt_u32(super.system_array_size)) {
		chunk = (const void *)(super.system_array + offset + sizeof(struct bt_disk_key));
		offset += sizeof(struct bt_disk_key) + sizeof(*chunk) +
		    bt_u16(chunk->stripes) * sizeof(struct bt_disk_stripe);
		entries++;
	}
	if (groups != fs->chunk_count || groups != e->value || entries != e->links) {
		fprintf(stderr, "%s stage %zu: %zu chunks, %llu block groups, %u system entries\n",
		    plan->name, stage, fs->chunk_count, (unsigned long long)groups, entries);
		exit(1);
	}
}

const char *const quota_state_names[] = { "consistent", "inconsistent", "rescan" };

/* The quota tree's status item, stamped with the stage's generation, and its
 * qgroup info items. */
static void
check_quota(struct btrfs_fs *fs, const struct plan *plan, size_t stage, const struct expectation *e)
{
	const struct bt_disk_qgroup_status *status = NULL;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	uint64_t qgroups = 0;
	unsigned state = QUOTA_CONSISTENT;
	enum btrfs_result result;

	REQUIRE(bt_find_root(fs, BT_QUOTA_TREE, &root) == BTRFS_OK);
	bt_cursor_init(&cursor, fs, root);
	result = bt_cursor_seek(&cursor, (struct bt_key){ 0 }, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		if (record.key.type == BT_QGROUP_STATUS) {
			REQUIRE(record.size >= sizeof(*status));
			status = (const void *)record.data;
			state = (bt_u64(status->flags) & BT_QGROUP_STATUS_RESCAN) != 0
			    ? QUOTA_RESCANNING
			    : (bt_u64(status->flags) & BT_QGROUP_STATUS_INCONSISTENT) != 0
			    ? QUOTA_INCONSISTENT
			    : QUOTA_CONSISTENT;
			REQUIRE(bt_u64(status->generation) == fs->info.generation);
		}
		qgroups += record.key.type == BT_QGROUP_INFO;
		result = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	REQUIRE(result == BTRFS_NOT_FOUND && status != NULL);
	if (state != e->links || qgroups != e->value) {
		fprintf(stderr, "%s stage %zu: quotas %s with %llu qgroups\n", plan->name, stage,
		    quota_state_names[state], (unsigned long long)qgroups);
		exit(1);
	}
}

/* The fs-verity digest the reader reports and the inode's VERITY_DESC and
 * VERITY_MERKLE items. */
static void
check_verity(struct btrfs_fs *fs, const struct plan *plan, size_t stage,
    const struct expectation *e, const struct btrfs_inode *inode)
{
	struct bt_root root;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = inode->id.inode, .type = BT_VERITY_DESC_ITEM };
	uint8_t digest[BTRFS_VERITY_DIGEST_MAX];
	uint64_t items = 0;
	size_t length;
	unsigned algorithm;
	enum btrfs_result result;

	result = btrfs_verity_digest(fs, inode, &algorithm, digest, sizeof(digest), &length);
	if (e->size == 0 ? result != BTRFS_NOT_FOUND
			 : result != BTRFS_OK || algorithm != e->mode || length != e->size ||
		    memcmp(digest, e->bytes, length) != 0) {
		expectation_failed(plan, stage, e, "fs-verity digest");
	}
	REQUIRE(bt_find_root(fs, inode->id.tree, &root) == BTRFS_OK);
	bt_cursor_init(&cursor, fs, root);
	result = bt_cursor_seek(&cursor, key, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		if (record.key.objectid != inode->id.inode ||
		    record.key.type > BT_VERITY_MERKLE_ITEM) {
			break;
		}
		items++;
		result = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	REQUIRE(result == BTRFS_OK || result == BTRFS_NOT_FOUND);
	if (items != e->value) {
		fprintf(stderr, "%s stage %zu: %s has %llu fs-verity items, expected %llu\n",
		    plan->name, stage, e->path, (unsigned long long)items,
		    (unsigned long long)e->value);
		exit(1);
	}
}

static void
check_expectation(struct btrfs_fs *fs, const struct plan *plan, size_t stage, size_t crash_commit,
    const struct expectation *e)
{
	struct btrfs_inode inode;
	struct btrfs_inode other;
	struct btrfs_info info;
	uint8_t *bytes = NULL;
	size_t size = 0;
	enum btrfs_result result;

	result = btrfs_image_lookup(fs, e->path, &inode);
	if (e->kind == EXPECT_ABSENT) {
		if (result != BTRFS_NOT_FOUND) {
			expectation_failed(plan, stage, e, "present");
		}
		return;
	}
	if (result != BTRFS_OK) {
		expectation_failed(plan, stage, e, btrfs_result_string(result));
	}
	switch (e->kind) {
	case EXPECT_FILE:
		REQUIRE((inode.mode & BTRFS_MODE_TYPE) == BTRFS_MODE_REGULAR);
		bytes = read_file(fs, e->path, &size);
		break;
	case EXPECT_SYMLINK:
		REQUIRE((inode.mode & BTRFS_MODE_TYPE) == BTRFS_MODE_SYMLINK);
		bytes = read_file(fs, e->path, &size);
		break;
	case EXPECT_DIRECTORY:
		REQUIRE((inode.mode & BTRFS_MODE_TYPE) == BTRFS_MODE_DIRECTORY);
		if (inode.size != e->value) {
			expectation_failed(plan, stage, e, "directory size");
		}
		bytes = list_directory(fs, &inode, &size);
		break;
	case EXPECT_XATTR:
		bytes = malloc(XATTR_VALUE_LIMIT + 1);
		REQUIRE(bytes != NULL);
		result = btrfs_get_xattr(
		    fs, &inode, e->other, strlen(e->other), bytes, XATTR_VALUE_LIMIT, &size);
		if (result != BTRFS_OK) {
			expectation_failed(plan, stage, e, btrfs_result_string(result));
		}
		break;
	case EXPECT_NO_XATTR:
		result = btrfs_get_xattr(fs, &inode, e->other, strlen(e->other), NULL, 0, &size);
		if (result != BTRFS_NOT_FOUND) {
			expectation_failed(plan, stage, e, "xattr present");
		}
		return;
	case EXPECT_SAME:
		REQUIRE(btrfs_image_lookup(fs, e->other, &other) == BTRFS_OK);
		if (inode.id.tree != other.id.tree || inode.id.inode != other.id.inode) {
			expectation_failed(plan, stage, e, "different object");
		}
		return;
	case EXPECT_STAT:
		if (inode.mode != e->mode || inode.uid != e->uid || inode.gid != e->gid ||
		    inode.links != e->links) {
			fprintf(stderr, "%s stage %zu: %s: mode %o uid %u gid %u links %u\n",
			    plan->name, stage, e->path, inode.mode, inode.uid, inode.gid,
			    inode.links);
			exit(1);
		}
		return;
	case EXPECT_DEVICE:
		if (inode.device != e->value) {
			expectation_failed(plan, stage, e, "device number");
		}
		return;
	case EXPECT_TIMES:
		if (inode.access_time.seconds != e->access_seconds ||
		    inode.modify_time.seconds != e->modify_seconds) {
			expectation_failed(plan, stage, e, "access or modification time");
		}
		return;
	case EXPECT_FLAGS:
		if ((inode.flags & e->mask) != e->value) {
			expectation_failed(plan, stage, e, "inode flags");
		}
		return;
	case EXPECT_FEATURE:
		btrfs_get_info(fs, &info);
		if ((info.incompat_features & e->value) == 0) {
			expectation_failed(plan, stage, e, "incompat feature");
		}
		return;
	case EXPECT_REFERENCE:
		if (reference_kind(fs, e->path, &inode) != (int)e->value) {
			expectation_failed(plan, stage, e, "back reference kind");
		}
		return;
	case EXPECT_SUBVOLUME:
		check_subvolume(fs, plan, stage, e, &inode);
		return;
	case EXPECT_SUBVOLUMES:
		check_subvolumes(fs, plan, stage, e);
		return;
	case EXPECT_DELETED:
		if (count_subvolumes(fs, 1) != e->value) {
			expectation_failed(plan, stage, e, "deleted subvolumes");
		}
		return;
	case EXPECT_COMPRESSED:
		check_compressed(fs, plan, stage, e, &inode);
		return;
	case EXPECT_EXTENTS:
		check_extents(fs, plan, stage, e, &inode);
		return;
	case EXPECT_HOLES:
		check_holes(fs, plan, stage, e, &inode);
		return;
	case EXPECT_BITMAPS:
		check_bitmaps(fs, plan, stage, e, &inode);
		return;
	case EXPECT_GROUPS:
		check_groups(fs, plan, stage, e);
		return;
	case EXPECT_QUOTA:
		check_quota(fs, plan, stage, e);
		return;
	case EXPECT_VERITY:
		check_verity(fs, plan, stage, e, &inode);
		return;
	case EXPECT_COMPAT_RO:
		btrfs_get_info(fs, &info);
		if ((info.readonly_features & e->value) != e->value) {
			expectation_failed(plan, stage, e, "read-only compatible feature");
		}
		return;
	default:
		REQUIRE(0);
	}
	if ((size != e->size || (size != 0 && memcmp(bytes, e->bytes, size) != 0)) &&
	    !(e->kind == EXPECT_FILE &&
		volatile_match(plan, stage, crash_commit, e, bytes, size))) {
		expectation_failed(plan, stage, e, "contents differ");
	}
	free(bytes);
}

static void
check_namespace(struct btrfs_fs *fs, const struct plan *plan, size_t stage, size_t crash_commit)
{
	struct namespace_audit audit;
	size_t i;

	for (i = 0; i < plan->expectation_count; i++) {
		if (plan->expectations[i].first <= stage && stage <= plan->expectations[i].last) {
			check_expectation(fs, plan, stage, crash_commit, &plan->expectations[i]);
		}
	}
	if (namespace_audit(fs, &audit) != 0) {
		fprintf(stderr, "%s stage %zu: namespace audit: %s\n", plan->name, stage,
		    audit.failure);
		exit(1);
	}
}

void
check_stage(struct context *context, struct btrfs_fs *fs, const struct plan *plan, size_t stage)
{
	struct btrfs_info info;
	const struct tracked *file;
	uint8_t *contents;
	size_t size;
	size_t i;

	btrfs_get_info(fs, &info);
	REQUIRE(info.generation == context->base_generation + stage);
	for (i = 0; i < plan->file_count; i += plan->verify_stride == 0 ? 1 : plan->verify_stride) {
		file = &plan->files[i];
		contents = read_file(fs, file->path, &size);
		if (size != file->size[stage] ||
		    (size != 0 && memcmp(contents, file->data[stage], size) != 0)) {
			fprintf(stderr, "%s stage %zu: %s differs (%zu bytes, expected %zu)\n",
			    plan->name, stage, file->path, size, file->size[stage]);
			exit(1);
		}
		free(contents);
	}
	check_invariants(fs);
	if (plan->namespace) {
		check_namespace(fs, plan, stage, context->crash_commit);
	}
}

void
plan_init(struct plan *plan)
{
	size_t stage;

	memset(plan, 0, sizeof(*plan));
	plan->files = calloc(MAX_FILES, sizeof(*plan->files));
	REQUIRE(plan->files != NULL);
	for (stage = 1; stage < MAX_STAGES; stage++) {
		plan->operations[stage] = calloc(MAX_OPERATIONS, sizeof(*plan->operations[stage]));
		REQUIRE(plan->operations[stage] != NULL);
	}
	plan->fault_points = FAULT_POINTS;
	plan->expectations = calloc(MAX_EXPECTATIONS, sizeof(*plan->expectations));
	REQUIRE(plan->expectations != NULL);
}

/* Plans are built against the committed state, which no write changes until
 * run_plan releases this mount. */
static struct btrfs_fs *
plan_mount(struct context *context)
{
	if (context->plan_fs == NULL) {
		REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &context->plan_fs) ==
		    BTRFS_OK);
	}
	return context->plan_fs;
}

size_t
plan_file(struct context *context, struct plan *plan, const char *path)
{
	struct btrfs_fs *fs;
	struct tracked *file;
	size_t i;

	for (i = 0; i < plan->file_count; i++) {
		if (strcmp(plan->files[i].path, path) == 0) {
			return i;
		}
	}
	REQUIRE(plan->file_count < MAX_FILES && strlen(path) < sizeof(file->path));
	file = &plan->files[plan->file_count];
	memset(file, 0, sizeof(*file));
	strcpy(file->path, path);
	fs = plan_mount(context);
	file->data[0] = read_file(fs, path, &file->size[0]);
	return plan->file_count++;
}

static void
plan_operation(struct context *context, struct plan *plan, size_t commit, const char *path,
    enum operation_kind kind, uint64_t offset, const void *data, size_t size)
{
	struct operation *operation;

	REQUIRE(
	    commit > 0 && commit < MAX_STAGES && plan->operation_count[commit] < MAX_OPERATIONS);
	REQUIRE(kind != OPERATION_INLINE || size <= INLINE_LIMIT);
	operation = &plan->operations[commit][plan->operation_count[commit]++];
	operation->file = plan_file(context, plan, path);
	operation->path = strdup(path);
	REQUIRE(operation->path != NULL);
	operation->kind = kind;
	operation->offset = offset;
	operation->size = size;
	operation->data = malloc(size + 1);
	REQUIRE(operation->data != NULL);
	if (size != 0) {
		memcpy(operation->data, data, size);
	}
	if (commit > plan->commits) {
		plan->commits = commit;
	}
}

void
plan_update(struct context *context, struct plan *plan, size_t commit, const char *path,
    const void *data, size_t size)
{
	plan_operation(context, plan, commit, path, OPERATION_INLINE, 0, data, size);
}

void
plan_write(struct context *context, struct plan *plan, size_t commit, const char *path,
    uint64_t offset, const void *data, size_t size)
{
	plan_operation(context, plan, commit, path, OPERATION_WRITE, offset, data, size);
}

void
plan_truncate(
    struct context *context, struct plan *plan, size_t commit, const char *path, uint64_t size)
{
	plan_operation(context, plan, commit, path, OPERATION_TRUNCATE, size, NULL, 0);
}

static void
model_resize(struct tracked *file, size_t stage, uint64_t size)
{
	uint8_t *grown;

	REQUIRE(size <= MAX_FILE_BYTES);
	grown = realloc(file->data[stage], (size_t)size + 1);
	REQUIRE(grown != NULL);
	if (size > file->size[stage]) {
		memset(grown + file->size[stage], 0, (size_t)size - file->size[stage]);
	}
	file->data[stage] = grown;
	file->size[stage] = (size_t)size;
}

void
plan_truncate_step(struct context *context, struct plan *plan, size_t commit, const char *path,
    uint64_t size, size_t budget)
{
	plan_operation(context, plan, commit, path, OPERATION_TRUNCATE_STEP, size, NULL, 0);
	plan->operations[commit][plan->operation_count[commit] - 1].size = budget;
}

void
plan_fallocate(struct context *context, struct plan *plan, size_t commit, const char *path,
    unsigned mode, uint64_t offset, uint64_t length)
{
	plan_operation(context, plan, commit, path, OPERATION_FALLOCATE, offset, NULL, 0);
	plan->operations[commit][plan->operation_count[commit] - 1].size = (size_t)length;
	plan->operations[commit][plan->operation_count[commit] - 1].flags = (int)mode;
}

/* The bytes a fallocate leaves: punching and zeroing read as zeros, and
 * without KEEP_SIZE allocation and zeroing grow the file. */
static void
model_fallocate(struct tracked *file, size_t stage, const struct operation *operation)
{
	unsigned mode = (unsigned)operation->flags;
	uint64_t end = operation->offset + operation->size;
	uint64_t zero_end = end < file->size[stage] ? end : file->size[stage];

	if ((mode & (BTRFS_FALLOCATE_PUNCH_HOLE | BTRFS_FALLOCATE_ZERO_RANGE)) != 0 &&
	    operation->offset < zero_end) {
		memset(file->data[stage] + operation->offset, 0,
		    (size_t)(zero_end - operation->offset));
	}
	if ((mode & BTRFS_FALLOCATE_KEEP_SIZE) == 0 && end > file->size[stage]) {
		model_resize(file, stage, end);
	}
}

void
resolve_bounds(struct btrfs_fs *fs, struct plan *plan, size_t stage)
{
	struct tracked *file;
	uint8_t *contents;
	size_t size;
	size_t i;

	for (i = 0; i < plan->file_count; i++) {
		file = &plan->files[i];
		if (!file->bounded[stage]) {
			continue;
		}
		contents = read_file(fs, file->path, &size);
		/* The step made progress and left work, as its plan intends. */
		if (size <= file->minimum[stage] || size >= file->size[stage] ||
		    (size != 0 && memcmp(contents, file->data[stage], size) != 0)) {
			fprintf(stderr, "%s stage %zu: %s is no prefix of %zu..%zu bytes (%zu)\n",
			    plan->name, stage, file->path, file->minimum[stage], file->size[stage],
			    size);
			exit(1);
		}
		free(contents);
		printf("%s stage %zu: %s shrank from %zu to %zu bytes toward %zu\n", plan->name,
		    stage, file->path, file->size[stage], size, file->minimum[stage]);
		file->size[stage] = size;
		file->bounded[stage] = 0;
	}
}

void
plan_finish(struct plan *plan)
{
	const struct operation *operation;
	struct tracked *file;
	size_t stage;
	size_t i;

	for (stage = 1; stage <= plan->commits; stage++) {
		for (i = 0; i < plan->file_count; i++) {
			file = &plan->files[i];
			file->size[stage] = file->size[stage - 1];
			file->data[stage] = malloc(file->size[stage] + 1);
			REQUIRE(file->data[stage] != NULL);
			memcpy(file->data[stage], file->data[stage - 1], file->size[stage]);
		}
		for (i = 0; i < plan->operation_count[stage]; i++) {
			operation = &plan->operations[stage][i];
			if (operation->file == NO_FILE || operation->kind > OPERATION_FALLOCATE ||
			    operation->expected != BTRFS_OK) {
				continue;
			}
			file = &plan->files[operation->file];
			if (operation->kind == OPERATION_FALLOCATE) {
				model_fallocate(file, stage, operation);
			} else if (operation->kind == OPERATION_TRUNCATE_STEP) {
				/* Only completing the truncation may follow in the plan. */
				REQUIRE(operation->offset < file->size[stage]);
				file->bounded[stage] = 1;
				file->minimum[stage] = (size_t)operation->offset;
			} else if (operation->kind == OPERATION_INLINE) {
				model_resize(file, stage, operation->size);
				memcpy(file->data[stage], operation->data, operation->size);
			} else if (operation->kind == OPERATION_WRITE) {
				if (operation->offset + operation->size > file->size[stage]) {
					model_resize(
					    file, stage, operation->offset + operation->size);
				}
				memcpy(file->data[stage] + operation->offset, operation->data,
				    operation->size);
			} else {
				model_resize(file, stage, operation->offset);
			}
		}
	}
}

void
plan_destroy(struct plan *plan)
{
	size_t stage;
	size_t i;

	for (i = 0; i < plan->file_count; i++) {
		for (stage = 0; stage < MAX_STAGES; stage++) {
			free(plan->files[i].data[stage]);
		}
	}
	for (stage = 0; stage < MAX_STAGES; stage++) {
		for (i = 0; i < plan->operation_count[stage]; i++) {
			free(plan->operations[stage][i].data);
			free(plan->operations[stage][i].path);
			free(plan->operations[stage][i].target);
		}
		free(plan->operations[stage]);
	}
	for (i = 0; i < plan->expectation_count; i++) {
		free(plan->expectations[i].path);
		free(plan->expectations[i].other);
		free(plan->expectations[i].bytes);
	}
	free(plan->expectations);
	free(plan->files);
	for (i = 0; i < plan->volatile_count; i++) {
		free(plan->volatiles[i].path);
	}
}

void
fill_pattern(uint8_t *data, size_t size, unsigned seed)
{
	size_t i;

	for (i = 0; i < size; i++) {
		data[i] = (uint8_t)(seed + i * 131U + (i >> 9));
	}
}

/* Compressible bytes: numbered lines of one sentence. */
void
fill_text(uint8_t *data, size_t size, unsigned seed)
{
	char line[96];
	size_t done = 0;
	size_t length;
	unsigned number = 0;

	while (done < size) {
		length = (size_t)snprintf(line, sizeof(line),
		    "line %06u of %u: the quick brown fox jumps over the lazy dog\n", number++,
		    seed);
		length = length < size - done ? length : size - done;
		memcpy(data + done, line, length);
		done += length;
	}
}

/* Incompressible bytes from a xorshift generator. */
void
fill_random(uint8_t *data, size_t size, unsigned seed)
{
	uint32_t state = 0x9e3779b9U ^ seed;
	size_t i;

	for (i = 0; i < size; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		data[i] = (uint8_t)(state >> 24);
	}
}

static struct operation *
plan_namespace(struct plan *plan, size_t commit, enum operation_kind kind, const char *path,
    const char *target)
{
	struct operation *operation;

	REQUIRE(
	    commit > 0 && commit < MAX_STAGES && plan->operation_count[commit] < MAX_OPERATIONS);
	operation = &plan->operations[commit][plan->operation_count[commit]++];
	memset(operation, 0, sizeof(*operation));
	operation->file = NO_FILE;
	operation->kind = kind;
	operation->path = strdup(path);
	operation->target = target == NULL ? NULL : strdup(target);
	operation->data = malloc(1);
	REQUIRE(operation->path != NULL && (target == NULL || operation->target != NULL) &&
	    operation->data != NULL);
	if (commit > plan->commits) {
		plan->commits = commit;
	}
	plan->namespace = 1;
	return operation;
}

static void
operation_data(struct operation *operation, const void *data, size_t size)
{
	free(operation->data);
	operation->data = malloc(size + 1);
	REQUIRE(operation->data != NULL);
	if (size != 0) {
		memcpy(operation->data, data, size);
	}
	operation->size = size;
}

void
plan_create(struct plan *plan, size_t commit, const char *path, uint32_t mode, const char *symlink)
{
	struct operation *operation = plan_namespace(plan, commit, OPERATION_CREATE, path, NULL);

	operation->mode = mode;
	operation->uid = NAMESPACE_UID;
	operation->gid = NAMESPACE_GID;
	if (symlink != NULL) {
		operation_data(operation, symlink, strlen(symlink));
	}
}

void
plan_device(struct plan *plan, size_t commit, const char *path, uint32_t mode, uint64_t device)
{
	plan_create(plan, commit, path, mode, NULL);
	plan->operations[commit][plan->operation_count[commit] - 1].device = device;
}

/* A UUID for each new subvolume, unique within one test process. */
static void
subvolume_uuid(struct operation *operation)
{
	static uint32_t counter;
	uint8_t uuid[BTRFS_UUID_SIZE];
	unsigned i;

	counter++;
	for (i = 0; i < BTRFS_UUID_SIZE; i++) {
		uuid[i] = (uint8_t)(UINT32_C(0x9e3779b9) * (counter + i) >> 24);
	}
	memcpy(uuid, &counter, sizeof(counter));
	operation_data(operation, uuid, sizeof(uuid));
}

void
plan_subvolume(struct plan *plan, size_t commit, const char *path)
{
	struct operation *operation = plan_namespace(plan, commit, OPERATION_SUBVOLUME, path, NULL);

	operation->mode = BTRFS_MODE_DIRECTORY | 0755;
	operation->uid = NAMESPACE_UID;
	operation->gid = NAMESPACE_GID;
	subvolume_uuid(operation);
}

void
plan_delete_subvolume(struct plan *plan, size_t commit, const char *path)
{
	(void)plan_namespace(plan, commit, OPERATION_DELETE_SUBVOLUME, path, NULL);
}

void
plan_clean_subvolumes(struct plan *plan, size_t commit, size_t budget, size_t dropped, int pending)
{
	struct operation *operation =
	    plan_namespace(plan, commit, OPERATION_CLEAN_SUBVOLUMES, "/", NULL);

	operation->offset = budget;
	operation->size = dropped;
	operation->flags = pending;
}

void
plan_tmpfile(struct plan *plan, size_t commit, const char *handle, uint32_t mode)
{
	struct operation *operation = plan_namespace(plan, commit, OPERATION_TMPFILE, handle, NULL);

	operation->mode = mode;
	operation->uid = NAMESPACE_UID;
	operation->gid = NAMESPACE_GID;
}

void
plan_link_tmpfile(struct plan *plan, size_t commit, const char *handle, const char *path)
{
	(void)plan_namespace(plan, commit, OPERATION_LINK_TMPFILE, handle, path);
}

void
plan_exchange(struct plan *plan, size_t commit, const char *path, const char *target)
{
	(void)plan_namespace(plan, commit, OPERATION_EXCHANGE, path, target);
}

void
plan_rename_whiteout(
    struct plan *plan, size_t commit, const char *path, const char *target, int open)
{
	struct operation *operation =
	    plan_namespace(plan, commit, OPERATION_RENAME_WHITEOUT, path, target);

	operation->uid = NAMESPACE_UID;
	operation->gid = NAMESPACE_GID;
	operation->flags = open;
}

void
plan_remove_groups(struct plan *plan, size_t commit, size_t removed)
{
	plan_namespace(plan, commit, OPERATION_REMOVE_GROUPS, "/", NULL)->size = removed;
}

void
plan_system_growth(struct plan *plan, size_t commit)
{
	(void)plan_namespace(plan, commit, OPERATION_SYSTEM_GROWTH, "/", NULL);
}

void
plan_snapshot(
    struct plan *plan, size_t commit, const char *source, const char *target, int read_only)
{
	struct operation *operation =
	    plan_namespace(plan, commit, OPERATION_SNAPSHOT, source, target);

	operation->flags = read_only;
	subvolume_uuid(operation);
}

void
plan_link(struct plan *plan, size_t commit, const char *path, const char *target)
{
	(void)plan_namespace(plan, commit, OPERATION_LINK, path, target);
}

void
plan_unlink(struct plan *plan, size_t commit, const char *path, int open)
{
	plan_namespace(plan, commit, OPERATION_UNLINK, path, NULL)->flags = open;
}

void
plan_unlink_deferred(struct plan *plan, size_t commit, const char *path)
{
	plan_namespace(plan, commit, OPERATION_UNLINK, path, NULL)->keep_deferred = 1;
}

void
plan_rename(struct plan *plan, size_t commit, const char *path, const char *target, int target_open)
{
	plan_namespace(plan, commit, OPERATION_RENAME, path, target)->flags = target_open;
}

void
plan_set_xattr(struct plan *plan, size_t commit, const char *path, const char *name,
    const void *value, size_t size, int flags)
{
	struct operation *operation = plan_namespace(plan, commit, OPERATION_SET_XATTR, path, name);

	operation_data(operation, value, size);
	operation->flags = flags;
}

void
plan_remove_xattr(struct plan *plan, size_t commit, const char *path, const char *name)
{
	(void)plan_namespace(plan, commit, OPERATION_REMOVE_XATTR, path, name);
}

void
plan_write_new(struct plan *plan, size_t commit, const char *path, uint64_t offset,
    const void *data, size_t size)
{
	struct operation *operation = plan_namespace(plan, commit, OPERATION_WRITE, path, NULL);

	operation_data(operation, data, size);
	operation->offset = offset;
}

/* The inode is named at plan time; it has no name when the commit runs. */
void
plan_evict(struct context *context, struct plan *plan, size_t commit, const char *path)
{
	struct btrfs_inode inode;

	REQUIRE(btrfs_image_lookup(plan_mount(context), path, &inode) == BTRFS_OK);
	plan_namespace(plan, commit, OPERATION_EVICT, path, NULL)->id = inode.id;
}

void
plan_clean(struct plan *plan, size_t commit, uint64_t tree, size_t expected)
{
	struct operation *operation =
	    plan_namespace(plan, commit, OPERATION_CLEAN_ORPHANS, "/", NULL);

	operation->id.tree = tree;
	operation->size = expected;
}

/* chmod, chown and utimes with mask BTRFS_ATTRIBUTE_*; the change time is
 * the commit's time. */
void
plan_set_attributes(struct plan *plan, size_t commit, const char *path, unsigned mask,
    uint32_t mode, uint32_t uid, uint32_t gid, int64_t access_seconds, int64_t modify_seconds)
{
	struct operation *operation =
	    plan_namespace(plan, commit, OPERATION_SET_ATTRIBUTES, path, NULL);

	operation->flags = (int)mask;
	operation->mode = mode;
	operation->uid = uid;
	operation->gid = gid;
	operation->access_time.seconds = access_seconds;
	operation->modify_time.seconds = modify_seconds;
}

/* The last operation of commit must be refused with result. */
void
plan_expect_refusal(struct plan *plan, size_t commit, enum btrfs_result result)
{
	REQUIRE(plan->operation_count[commit] != 0);
	plan->operations[commit][plan->operation_count[commit] - 1].expected = result;
}

/* Truncation of a file the plan models through expectations. */
void
plan_truncate_new(struct plan *plan, size_t commit, const char *path, uint64_t size)
{
	plan_namespace(plan, commit, OPERATION_TRUNCATE, path, NULL)->offset = size;
}

void
plan_fallocate_new(struct plan *plan, size_t commit, const char *path, unsigned mode,
    uint64_t offset, uint64_t length)
{
	struct operation *operation = plan_namespace(plan, commit, OPERATION_FALLOCATE, path, NULL);

	operation->offset = offset;
	operation->size = (size_t)length;
	operation->flags = (int)mode;
}

void
plan_set_fsflags(struct plan *plan, size_t commit, const char *path, unsigned flags)
{
	plan_namespace(plan, commit, OPERATION_SET_FSFLAGS, path, NULL)->flags = (int)flags;
}

void
plan_privileges(struct plan *plan, size_t commit, const char *path, int keep)
{
	(void)plan_namespace(
	    plan, commit, keep ? OPERATION_KEEP_PRIVILEGES : OPERATION_DROP_PRIVILEGES, path, NULL);
}

struct expectation *
expect(struct plan *plan, size_t first, size_t last, enum expectation_kind kind, const char *path)
{
	struct expectation *e;

	REQUIRE(plan->expectation_count < MAX_EXPECTATIONS && first <= last);
	e = &plan->expectations[plan->expectation_count++];
	memset(e, 0, sizeof(*e));
	e->first = first;
	e->last = last;
	e->kind = kind;
	e->path = strdup(path);
	REQUIRE(e->path != NULL);
	plan->namespace = 1;
	return e;
}

static void
expectation_bytes(struct expectation *e, const void *bytes, size_t size)
{
	e->bytes = malloc(size + 1);
	REQUIRE(e->bytes != NULL);
	if (size != 0) {
		memcpy(e->bytes, bytes, size);
	}
	e->size = size;
}

void
expect_absent(struct plan *plan, size_t first, size_t last, const char *path)
{
	(void)expect(plan, first, last, EXPECT_ABSENT, path);
}

void
expect_file(
    struct plan *plan, size_t first, size_t last, const char *path, const void *bytes, size_t size)
{
	expectation_bytes(expect(plan, first, last, EXPECT_FILE, path), bytes, size);
}

void
expect_text(struct plan *plan, size_t first, size_t last, const char *path, const char *text)
{
	expect_file(plan, first, last, path, text, strlen(text));
}

/* Contents of source in the committed state, expected at path. */
void
expect_current(struct context *context, struct plan *plan, size_t first, size_t last,
    const char *path, const char *source)
{
	uint8_t *bytes;
	size_t size;

	bytes = read_file(plan_mount(context), source, &size);
	expect_file(plan, first, last, path, bytes, size);
	free(bytes);
}

uint8_t *
fixture_bytes(struct context *context, const char *path, size_t *size)
{
	return read_file(plan_mount(context), path, size);
}

void
expect_symlink(struct plan *plan, size_t first, size_t last, const char *path, const char *target)
{
	expectation_bytes(expect(plan, first, last, EXPECT_SYMLINK, path), target, strlen(target));
}

void
expect_same(struct plan *plan, size_t first, size_t last, const char *path, const char *other)
{
	struct expectation *e = expect(plan, first, last, EXPECT_SAME, path);

	e->other = strdup(other);
	REQUIRE(e->other != NULL);
}

/* value NULL expects the xattr to be absent. */
void
expect_xattr(struct plan *plan, size_t first, size_t last, const char *path, const char *name,
    const void *value, size_t size)
{
	struct expectation *e =
	    expect(plan, first, last, value == NULL ? EXPECT_NO_XATTR : EXPECT_XATTR, path);

	e->other = strdup(name);
	REQUIRE(e->other != NULL);
	if (value != NULL) {
		expectation_bytes(e, value, size);
	}
}

void
expect_stat(
    struct plan *plan, size_t first, size_t last, const char *path, uint32_t mode, uint32_t links)
{
	struct expectation *e = expect(plan, first, last, EXPECT_STAT, path);

	e->mode = mode;
	e->uid = NAMESPACE_UID;
	e->gid = NAMESPACE_GID;
	e->links = links;
}

/* Mode, owner and links that need not be the scenario's own owner. */
void
expect_owner(struct plan *plan, size_t first, size_t last, const char *path, uint32_t mode,
    uint32_t uid, uint32_t gid, uint32_t links)
{
	struct expectation *e = expect(plan, first, last, EXPECT_STAT, path);

	e->mode = mode;
	e->uid = uid;
	e->gid = gid;
	e->links = links;
}

void
expect_times(struct plan *plan, size_t first, size_t last, const char *path, int64_t access_seconds,
    int64_t modify_seconds)
{
	struct expectation *e = expect(plan, first, last, EXPECT_TIMES, path);

	e->access_seconds = access_seconds;
	e->modify_seconds = modify_seconds;
}

/* Owner and mode of source in the committed state, with links at path. */
void
expect_links(struct context *context, struct plan *plan, size_t first, size_t last,
    const char *path, const char *source, uint32_t links)
{
	struct btrfs_inode inode;
	struct expectation *e = expect(plan, first, last, EXPECT_STAT, path);

	REQUIRE(btrfs_image_lookup(plan_mount(context), source, &inode) == BTRFS_OK);
	e->mode = inode.mode;
	e->uid = inode.uid;
	e->gid = inode.gid;
	e->links = links;
}

void
expect_value(struct plan *plan, size_t first, size_t last, enum expectation_kind kind,
    const char *path, uint64_t value)
{
	struct expectation *e = expect(plan, first, last, kind, path);

	e->value = value;
	e->mask = value;
}

/* The inode flags in mask equal value. */
void
expect_flags(
    struct plan *plan, size_t first, size_t last, const char *path, uint64_t mask, uint64_t value)
{
	struct expectation *e = expect(plan, first, last, EXPECT_FLAGS, path);

	e->value = value;
	e->mask = mask;
}

static int
compare_names(const void *left, const void *right)
{
	const char *a = *(const char *const *)left;
	const char *b = *(const char *const *)right;

	return strcmp(a, b);
}

/* A directory listing: names (sorted bytewise, each followed by a newline)
 * and the size Linux keeps, twice the length of the names. */
void
expect_names(struct plan *plan, size_t first, size_t last, const char *path, const char **names,
    size_t count)
{
	struct expectation *e = expect(plan, first, last, EXPECT_DIRECTORY, path);
	size_t used = 0;
	size_t i;

	if (count > 1) {
		qsort(names, count, sizeof(*names), compare_names);
	}
	for (i = 0; i < count; i++) {
		used += strlen(names[i]) + 1;
	}
	e->bytes = malloc(used + 1);
	REQUIRE(e->bytes != NULL);
	for (i = 0; i < count; i++) {
		memcpy(e->bytes + e->size, names[i], strlen(names[i]));
		e->size += strlen(names[i]);
		e->bytes[e->size++] = '\n';
		e->value += 2 * (uint64_t)strlen(names[i]);
	}
}

void
expect_subvolume(struct plan *plan, size_t first, size_t last, const char *path, const char *source,
    int read_only)
{
	struct expectation *e = expect(plan, first, last, EXPECT_SUBVOLUME, path);

	e->value = (uint64_t)read_only;
	if (source != NULL) {
		e->other = strdup(source);
		REQUIRE(e->other != NULL);
	}
}

void
expect_compressed(struct plan *plan, size_t first, size_t last, const char *path,
    enum btrfs_compression codec, uint32_t regular, uint32_t inline_extents)
{
	struct expectation *e = expect(plan, first, last, EXPECT_COMPRESSED, path);

	e->value = (uint64_t)codec;
	e->links = regular;
	e->mode = inline_extents;
}

void
expect_extents(struct plan *plan, size_t first, size_t last, const char *path, uint32_t regular,
    uint32_t prealloc, uint64_t distinct)
{
	struct expectation *e = expect(plan, first, last, EXPECT_EXTENTS, path);

	e->links = regular;
	e->mode = prealloc;
	e->value = distinct;
}

void
expect_holes(struct plan *plan, size_t first, size_t last, const char *path, uint64_t holes)
{
	expect(plan, first, last, EXPECT_HOLES, path)->value = holes;
}

void
expect_bitmaps(struct plan *plan, size_t first, size_t last, const char *path, int bitmaps)
{
	expect(plan, first, last, EXPECT_BITMAPS, path)->value = bitmaps != 0;
}

void
expect_groups(
    struct plan *plan, size_t first, size_t last, uint64_t groups, uint32_t system_entries)
{
	struct expectation *e = expect(plan, first, last, EXPECT_GROUPS, "/");

	e->value = groups;
	e->links = system_entries;
}

void
plan_volatile(struct plan *plan, size_t commit, const char *path, uint64_t offset, uint64_t length)
{
	struct volatile_range *range;

	REQUIRE(plan->volatile_count < MAX_VOLATILE);
	range = &plan->volatiles[plan->volatile_count++];
	range->commit = commit;
	range->path = strdup(path);
	REQUIRE(range->path != NULL);
	range->offset = offset;
	range->length = length;
}

void
expect_quota(struct plan *plan, size_t first, size_t last, int inconsistent, uint64_t qgroups)
{
	struct expectation *e = expect(plan, first, last, EXPECT_QUOTA, "/");

	e->value = qgroups;
	e->links = inconsistent != 0 ? QUOTA_INCONSISTENT : QUOTA_CONSISTENT;
}

void
expect_quota_rescan(struct plan *plan, size_t first, size_t last, uint64_t qgroups)
{
	struct expectation *e = expect(plan, first, last, EXPECT_QUOTA, "/");

	e->value = qgroups;
	e->links = QUOTA_RESCANNING;
}

void
plan_quota_rescan(struct plan *plan, size_t commit, size_t budget, int done)
{
	struct operation *operation =
	    plan_namespace(plan, commit, OPERATION_QUOTA_RESCAN, "/", NULL);

	operation->offset = budget;
	operation->flags = done;
}

void
plan_verity(struct plan *plan, size_t commit, const char *path, enum verity_mode mode,
    unsigned algorithm, uint32_t block_size, const void *salt, size_t salt_size,
    size_t signature_size, size_t budget, size_t stop)
{
	struct operation *operation = plan_namespace(plan, commit, OPERATION_VERITY, path, NULL);

	operation_data(operation, salt, salt_size);
	operation->flags = (int)mode;
	operation->mode = algorithm;
	operation->uid = block_size;
	operation->gid = (uint32_t)signature_size;
	operation->offset = budget;
	operation->device = stop;
	operation->target = malloc(signature_size + 1);
	REQUIRE(operation->target != NULL);
	fill_pattern((uint8_t *)operation->target, signature_size, (unsigned)commit);
	operation->target[signature_size] = '\0';
}

void
expect_verity(struct plan *plan, size_t first, size_t last, const char *path, unsigned algorithm,
    const uint8_t *digest, size_t digest_size, uint64_t items)
{
	struct expectation *e = expect(plan, first, last, EXPECT_VERITY, path);

	e->mode = algorithm;
	e->value = items;
	e->size = digest != NULL ? digest_size : 0;
	if (e->size != 0) {
		e->bytes = malloc(e->size);
		REQUIRE(e->bytes != NULL);
		memcpy(e->bytes, digest, e->size);
	}
}

void
expect_compat_ro(struct plan *plan, size_t first, size_t last, uint64_t features)
{
	expect(plan, first, last, EXPECT_COMPAT_RO, "/")->value = features;
}

void
expect_deleted(struct plan *plan, size_t first, size_t last, size_t count)
{
	expect(plan, first, last, EXPECT_DELETED, "/")->value = count;
}

/* paths: NULL-terminated subvolume paths below the top level, without the
 * leading slash. */
void
expect_subvolumes(struct plan *plan, size_t first, size_t last, const char **paths)
{
	struct expectation *e;
	size_t count = 0;

	while (paths[count] != NULL) {
		count++;
	}
	expect_names(plan, first, last, "/", paths, count);
	e = &plan->expectations[plan->expectation_count - 1];
	e->kind = EXPECT_SUBVOLUMES;
	e->value = 0;
}

/* The committed listing of path with names added and removed (NULL-terminated
 * lists, either may be NULL). */
void
expect_listing(struct context *context, struct plan *plan, size_t first, size_t last,
    const char *path, const char *const *added, const char *const *removed)
{
	expect_listing_from(context, plan, first, last, path, path, added, removed);
}

/* The committed listing of base, changed as expect_listing describes, for the
 * directory at path (a snapshot of base, for example). */
void
expect_listing_from(struct context *context, struct plan *plan, size_t first, size_t last,
    const char *path, const char *base, const char *const *added, const char *const *removed)
{
	struct btrfs_inode directory;
	const char **names;
	uint8_t *listing;
	size_t size;
	size_t count = 0;
	size_t start = 0;
	size_t i;
	size_t j;

	REQUIRE(btrfs_image_lookup(plan_mount(context), base, &directory) == BTRFS_OK);
	listing = list_directory(context->plan_fs, &directory, &size);
	names = calloc(size + 64, sizeof(*names));
	REQUIRE(names != NULL);
	for (i = 0; i < size; i++) {
		if (listing[i] == '\n') {
			listing[i] = '\0';
			names[count++] = (const char *)listing + start;
			start = i + 1;
		}
	}
	for (i = 0; removed != NULL && removed[i] != NULL; i++) {
		for (j = 0; j < count && strcmp(names[j], removed[i]) != 0; j++) {
		}
		REQUIRE(j < count);
		names[j] = names[--count];
	}
	for (i = 0; added != NULL && added[i] != NULL; i++) {
		names[count++] = added[i];
	}
	expect_names(plan, first, last, path, names, count);
	free(names);
	free(listing);
}
