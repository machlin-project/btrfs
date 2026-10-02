/* SPDX-License-Identifier: BSD-3-Clause */
/* Admission, superblock copies, damaged allocation maps, the reference audit
 * self-test and metadata exhaustion. */
#define _POSIX_C_SOURCE 200809L
#include "scenario.h"
#include <btrfs/volume.h>

void
admission_tests(struct context *context)
{
	static const char replacement[] = "written by Machlin CoW transaction\n";
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct btrfs_inode inode;
	struct btrfs_inode snapshot;
	struct btrfs_time time = { 1700000000, 0 };
	struct device *device = context->device;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/snapshot/value", &snapshot) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/greeting", &inode) == BTRFS_OK);
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_write_inline(transaction, snapshot.id, replacement,
		    sizeof(replacement) - 1, time) == BTRFS_READ_ONLY);
	REQUIRE(btrfs_transaction_write_inline(transaction,
		    (struct btrfs_object_id){ BT_EXTENT_TREE, inode.id.inode }, replacement,
		    sizeof(replacement) - 1, time) == BTRFS_INVALID_ARGUMENT);
	REQUIRE(btrfs_transaction_write_inline(transaction,
		    (struct btrfs_object_id){ ABSENT_TREE, inode.id.inode }, replacement,
		    sizeof(replacement) - 1, time) == BTRFS_NOT_FOUND);
	REQUIRE(btrfs_transaction_write_inline(transaction, inode.id, replacement, INLINE_LIMIT + 1,
		    time) == BTRFS_UNSUPPORTED);
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
	REQUIRE(device->count == 0 && device->flushes == 0);
	btrfs_transaction_destroy(transaction);
	/* Destroying an uncommitted transaction discards private work without I/O. */
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_write_inline(
		    transaction, inode.id, replacement, sizeof(replacement) - 1, time) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	REQUIRE(device->count == 0);
	btrfs_unmount(fs);
	REQUIRE(context->image.live_allocations == 0);
	printf("admission, no-op and abort: PASS\n");
}

static void
super_copy(struct context *context, unsigned mirror, struct bt_disk_super *super)
{
	read_exact(context, bt_super_offset(mirror), super, sizeof(*super));
}

static enum btrfs_result
begin_and_commit(struct context *context, struct btrfs_fs *fs, int damage_after_begin,
    const struct bt_disk_super *damage, unsigned mirror)
{
	struct btrfs_transaction *transaction;
	struct btrfs_inode inode;
	struct btrfs_time time = { 1700000000, 0 };
	enum btrfs_result result;

	REQUIRE(btrfs_image_lookup(fs, "/greeting", &inode) == BTRFS_OK);
	result = btrfs_transaction_begin(fs, &context->writer, &transaction);
	if (result != BTRFS_OK) {
		return result;
	}
	REQUIRE(
	    btrfs_transaction_write_inline(transaction, inode.id, "stale\n", 6, time) == BTRFS_OK);
	if (damage_after_begin) {
		synthetic(context, bt_super_offset(mirror), damage, sizeof(*damage));
	}
	result = btrfs_transaction_commit(transaction);
	btrfs_transaction_destroy(transaction);
	return result;
}

/* Admission requires agreeing copies; commit rejects copies changed underneath
 * it; recovery never rolls back, merges filesystems or discards a tree log. */
void
copy_tests(struct context *context)
{
	struct bt_disk_super *primary;
	struct bt_disk_super *secondary;
	struct bt_disk_super *damage;
	struct btrfs_recovery_report report;
	struct btrfs_fs *fs;
	struct device *device = context->device;
	uint64_t generation = context->base_generation;
	size_t writes;
	unsigned copies = 0;
	unsigned mirror;

	for (mirror = 0; mirror < BTRFS_SUPER_COPIES; mirror++) {
		copies += bt_super_present(context->image.environment.size_bytes, mirror) != 0;
	}
	primary = malloc(sizeof(*primary));
	secondary = malloc(sizeof(*secondary));
	damage = malloc(sizeof(*damage));
	REQUIRE(primary != NULL && secondary != NULL && damage != NULL);
	super_copy(context, 0, primary);
	super_copy(context, 1, secondary);
	REQUIRE(bt_super_same(primary, secondary));

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	*damage = *primary;
	bt_put64(&damage->generation, generation + 1);
	bt_super_seal(damage, BT_SUPER_OFFSET);
	synthetic(context, BT_SUPER_OFFSET, damage, sizeof(*damage));
	REQUIRE(begin_and_commit(context, fs, 0, NULL, 0) == BTRFS_STALE && device->count == 1);
	truncate_writes(device, 0);
	*damage = *primary;
	damage->label[0] ^= 1;
	bt_super_seal(damage, BT_SUPER_OFFSET);
	REQUIRE(begin_and_commit(context, fs, 1, damage, 0) == BTRFS_STALE && device->count == 1);
	truncate_writes(device, 0);
	*damage = *secondary;
	damage->label[0] ^= 1;
	bt_super_seal(damage, bt_super_offset(1));
	REQUIRE(begin_and_commit(context, fs, 1, damage, 1) == BTRFS_STALE && device->count == 1);
	truncate_writes(device, 0);

	/* A torn secondary blocks admission until explicit recovery rewrites it. */
	*damage = *secondary;
	((uint8_t *)damage)[DEVICE_SECTOR * 3] ^= 1;
	synthetic(context, bt_super_offset(1), damage, sizeof(*damage));
	REQUIRE(begin_and_commit(context, fs, 0, NULL, 0) == BTRFS_RECOVERY_REQUIRED);
	REQUIRE(btrfs_recover_supers(&context->env, NULL, generation, &report) ==
		BTRFS_RECOVERY_REQUIRED &&
	    report.selected == 0 && report.copies[1].status == BTRFS_CORRUPT);
	REQUIRE(btrfs_recover_supers(&context->env, NULL, generation + 1, &report) == BTRFS_STALE);
	writes = device->count;
	REQUIRE(btrfs_recover_supers(&context->env, &context->writer, generation, &report) ==
		BTRFS_OK &&
	    report.rewritten == 1 && device->count == writes + 1);
	show(&device->writes[writes], 1);
	REQUIRE(btrfs_recover_supers(&context->env, NULL, generation, &report) == BTRFS_OK &&
	    report.rewritten == 0);
	truncate_writes(device, 0);

	/* On devices past 256 GiB, a torn third copy alone blocks admission too,
	 * and recovery rewrites only it. */
	if (copies == BTRFS_SUPER_COPIES) {
		super_copy(context, 2, damage);
		REQUIRE(bt_super_same(primary, damage));
		((uint8_t *)damage)[DEVICE_SECTOR * 3] ^= 1;
		synthetic(context, bt_super_offset(2), damage, sizeof(*damage));
		REQUIRE(begin_and_commit(context, fs, 0, NULL, 0) == BTRFS_RECOVERY_REQUIRED);
		writes = device->count;
		REQUIRE(btrfs_recover_supers(
			    &context->env, &context->writer, generation, &report) == BTRFS_OK &&
		    report.selected == 0 && report.copies[2].status == BTRFS_CORRUPT &&
		    report.copies[1].current && report.rewritten == 1 &&
		    device->count == writes + 1 &&
		    device->writes[writes].offset == bt_super_offset(2));
		truncate_writes(device, 0);
	}
	btrfs_unmount(fs);

	/* No valid copy: nothing is selected or written. */
	for (mirror = 0; mirror < copies; mirror++) {
		super_copy(context, mirror, damage);
		((uint8_t *)damage)[DEVICE_SECTOR * 3] ^= 1;
		synthetic(context, bt_super_offset(mirror), damage, sizeof(*damage));
	}
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_CORRUPT);
	REQUIRE(
	    btrfs_recover_supers(&context->env, &context->writer, 0, &report) == BTRFS_CORRUPT &&
	    device->count == copies);
	truncate_writes(device, 0);

	/* A torn primary with an intact secondary recovers the same generation. */
	*damage = *primary;
	((uint8_t *)damage)[DEVICE_SECTOR * 3] ^= 1;
	synthetic(context, BT_SUPER_OFFSET, damage, sizeof(*damage));
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_CORRUPT);
	REQUIRE(btrfs_recover_supers(&context->env, &context->writer, generation, &report) ==
		BTRFS_OK &&
	    report.selected == 1 && report.generation == generation && report.rewritten == 1);
	show(&device->writes[device->count - 1], 1);
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	check_invariants(fs);
	btrfs_unmount(fs);
	truncate_writes(device, 0);

	/* A newer copy from another filesystem is ambiguous, never selected. */
	*damage = *secondary;
	damage->fsid[0] ^= 1;
	bt_put64(&damage->generation, generation + 1);
	bt_super_seal(damage, bt_super_offset(1));
	synthetic(context, bt_super_offset(1), damage, sizeof(*damage));
	REQUIRE(
	    btrfs_recover_supers(&context->env, &context->writer, 0, &report) == BTRFS_CORRUPT &&
	    device->count == 1);
	truncate_writes(device, 0);

	/* Choosing a copy without the pending log would drop fsynced data. */
	*damage = *primary;
	bt_put64(&damage->log_root, bt_u64(primary->root));
	bt_super_seal(damage, BT_SUPER_OFFSET);
	synthetic(context, BT_SUPER_OFFSET, damage, sizeof(*damage));
	REQUIRE(btrfs_recover_supers(&context->env, &context->writer, 0, &report) ==
		BTRFS_UNSUPPORTED &&
	    device->count == 1);
	truncate_writes(device, 0);
	REQUIRE(context->image.live_allocations == 0);
	free(damage);
	free(secondary);
	free(primary);
	printf("superblock copies (%u on this device): stale rejection, admission and explicit "
	       "recovery PASS\n",
	    copies);
}

typedef int (*item_match)(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument);

/* Copy the leaf holding the first matching item of a tree. */
static int
find_leaf(const struct btrfs_fs *fs, struct bt_root root, item_match match, void *argument,
    uint8_t *leaf, uint64_t *logical, uint32_t *slot)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key first = { 0 };
	enum btrfs_result error;
	int found = 0;

	bt_cursor_init(&cursor, fs, root);
	error = bt_cursor_seek(&cursor, first, 0);
	while (error == BTRFS_OK && !found) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		found = match(fs, &cursor, &record, argument);
		if (found) {
			memcpy(leaf, cursor.blocks[0], fs->info.node_size);
			*logical = cursor.loaded[0].address;
			*slot = cursor.slots[0];
		} else {
			error = bt_cursor_next(&cursor);
		}
	}
	REQUIRE(found || error == BTRFS_NOT_FOUND);
	bt_cursor_fini(&cursor);
	return found;
}

static void
replace_leaf(struct context *context, const struct btrfs_fs *fs, uint8_t *leaf, uint64_t logical,
    uint64_t kind)
{
	struct bt_le32 checksum;
	uint64_t physical;
	unsigned mirrors = 1;
	unsigned mirror;

	bt_put32(&checksum,
	    ~bt_crc32c(UINT32_MAX, leaf + BT_CSUM_SIZE, fs->info.node_size - BT_CSUM_SIZE));
	memset(leaf, 0, BT_CSUM_SIZE);
	memcpy(leaf, &checksum, sizeof(checksum));
	for (mirror = 0; mirror < mirrors; mirror++) {
		REQUIRE(bt_map(fs, logical, fs->info.node_size, kind, mirror, &physical,
			    &mirrors) == BTRFS_OK);
		synthetic(context, physical, leaf, fs->info.node_size);
	}
}

static struct bt_disk_item *
leaf_item(uint8_t *leaf, uint32_t slot)
{
	return (struct bt_disk_item *)(leaf + sizeof(struct bt_disk_header)) + slot;
}

static void *
leaf_data(uint8_t *leaf, uint32_t slot)
{
	return leaf + sizeof(struct bt_disk_header) + bt_u32(leaf_item(leaf, slot)->offset);
}

/* Key edits avoid slot zero, whose key a parent pointer also records. */
static int
match_metadata(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument)
{
	(void)fs;
	(void)argument;
	return record->key.type == BT_METADATA_ITEM && cursor->slots[0] > 0;
}

static int
match_successor(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument)
{
	const struct bt_disk_item *items =
	    (const void *)(cursor->blocks[0] + sizeof(struct bt_disk_header));

	(void)fs;
	(void)argument;
	return record->key.type == BT_METADATA_ITEM && cursor->slots[0] > 0 &&
	    items[cursor->slots[0] - 1].key.type == BT_METADATA_ITEM;
}

static int
match_group(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument)
{
	const struct bt_disk_block_group *group = (const void *)record->data;

	(void)fs;
	(void)argument;
	return record->key.type == BT_BLOCK_GROUP_ITEM && cursor->slots[0] > 0 &&
	    (bt_u64(group->flags) & BT_BLOCK_METADATA) != 0;
}

static int
match_last(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument)
{
	struct bt_key *last = argument;

	(void)fs;
	(void)cursor;
	return bt_key_compare(record->key, *last) == 0;
}

static int
match_group_at(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument)
{
	const uint64_t *logical = argument;

	(void)fs;
	(void)cursor;
	return record->key.type == BT_BLOCK_GROUP_ITEM && record->key.objectid == *logical;
}

static int
match_chunk(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument)
{
	const struct bt_disk_chunk *chunk = (const void *)record->data;
	const uint64_t *type = argument;

	(void)fs;
	(void)cursor;
	return record->key.type == BT_CHUNK_ITEM &&
	    (bt_u64(chunk->type) & (BT_BLOCK_DATA | BT_BLOCK_METADATA | BT_BLOCK_SYSTEM)) == *type;
}

static void
expect_corrupt_map(struct context *context, const char *name)
{
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction = NULL;
	size_t writes = context->device->count;
	uint64_t live = context->image.live_allocations;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_CORRUPT);
	REQUIRE(transaction == NULL && context->device->count == writes);
	btrfs_unmount(fs);
	REQUIRE(context->image.live_allocations == live);
	truncate_writes(context->device, 0);
	printf("allocation map %s: PASS (corrupt)\n", name);
}

/* Checksum-correct allocation maps damaged by this test, not by the writer. */
void
allocation_map_tests(struct context *context)
{
	struct btrfs_fs *fs;
	struct bt_root extents;
	struct bt_disk_extent_item *extent;
	struct bt_disk_block_group *group;
	struct bt_disk_item *item;
	struct bt_disk_chunk *chunk;
	struct bt_disk_stripe *stripes;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key last = { UINT64_MAX, UINT64_MAX, UINT8_MAX };
	uint8_t *leaf;
	uint64_t logical;
	uint64_t metadata_physical = 0;
	uint64_t type;
	uint64_t chunk_end = 0;
	uint64_t group_logical = UINT64_MAX;
	uint64_t length;
	uint32_t slot;
	size_t i;

	leaf = malloc(BT_MAX_NODE_SIZE);
	REQUIRE(leaf != NULL);
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(bt_find_root(fs, BT_EXTENT_TREE, &extents) == BTRFS_OK);
	for (i = 0; i < fs->chunk_count; i++) {
		if (fs->chunks[i].logical + fs->chunks[i].length > chunk_end) {
			chunk_end = fs->chunks[i].logical + fs->chunks[i].length;
		}
		if (fs->chunks[i].type & BT_BLOCK_METADATA) {
			metadata_physical = fs->chunks[i].physical[0];
		}
	}

	REQUIRE(find_leaf(fs, extents, match_group, NULL, leaf, &logical, &slot));
	group = leaf_data(leaf, slot);
	bt_put64(&group->used_bytes, bt_u64(group->used_bytes) + fs->info.node_size);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_corrupt_map(context, "block-group total");

	REQUIRE(find_leaf(fs, extents, match_group, NULL, leaf, &logical, &slot));
	leaf_item(leaf, slot)->key.type = BT_BLOCK_GROUP_ITEM + 1;
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_corrupt_map(context, "missing block group");

	REQUIRE(find_leaf(fs, extents, match_metadata, NULL, leaf, &logical, &slot));
	extent = leaf_data(leaf, slot);
	bt_put64(&extent->refs, 0);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_corrupt_map(context, "zero references");

	REQUIRE(find_leaf(fs, extents, match_metadata, NULL, leaf, &logical, &slot));
	extent = leaf_data(leaf, slot);
	bt_put64(&extent->generation, fs->info.generation + 1);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_corrupt_map(context, "future generation");

	REQUIRE(find_leaf(fs, extents, match_metadata, NULL, leaf, &logical, &slot));
	extent = leaf_data(leaf, slot);
	bt_put64(&extent->flags, BT_EXTENT_FLAG_DATA);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_corrupt_map(context, "data extent in metadata");

	REQUIRE(find_leaf(fs, extents, match_metadata, NULL, leaf, &logical, &slot));
	bt_put64(&leaf_item(leaf, slot)->key.offset, BT_MAX_LEVEL);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_corrupt_map(context, "tree level bound");

	REQUIRE(find_leaf(fs, extents, match_successor, NULL, leaf, &logical, &slot));
	item = leaf_item(leaf, slot);
	bt_put64(&item->key.objectid, bt_u64(item->key.objectid) + DEVICE_SECTOR);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_corrupt_map(context, "unaligned extent");

	if (fs->info.node_size > fs->info.sector_size) {
		REQUIRE(find_leaf(fs, extents, match_successor, NULL, leaf, &logical, &slot));
		item = leaf_item(leaf, slot);
		bt_put64(&item->key.objectid,
		    bt_u64(leaf_item(leaf, slot - 1)->key.objectid) + fs->info.sector_size);
		replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
		expect_corrupt_map(context, "overlapping extents");
	}

	/* The last extent record moves beyond every chunk while its block group total
	 * is reduced to match, so only the containment rule can reject it. Moving a
	 * record keeps key order only when it is the last record of the tree. */
	bt_cursor_init(&cursor, fs, extents);
	REQUIRE(bt_cursor_seek(&cursor, last, 1) == BTRFS_OK);
	REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
	last = record.key;
	bt_cursor_fini(&cursor);
	if (last.type == BT_EXTENT_ITEM || last.type == BT_METADATA_ITEM) {
		length = last.type == BT_METADATA_ITEM ? fs->info.node_size : last.offset;
		for (i = 0; i < fs->chunk_count; i++) {
			if (last.objectid - fs->chunks[i].logical < fs->chunks[i].length) {
				group_logical = fs->chunks[i].logical;
			}
		}
		REQUIRE(find_leaf(fs, extents, match_last, &last, leaf, &logical, &slot));
		bt_put64(&leaf_item(leaf, slot)->key.objectid, chunk_end + fs->info.node_size);
		replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
		REQUIRE(
		    find_leaf(fs, extents, match_group_at, &group_logical, leaf, &logical, &slot));
		group = leaf_data(leaf, slot);
		bt_put64(&group->used_bytes, bt_u64(group->used_bytes) - length);
		replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
		expect_corrupt_map(context, "extent outside chunks");
	} else {
		printf("allocation map extent outside chunks: not constructed on this image "
		       "(last record is a keyed backreference)\n");
	}

	/* Logical separation does not authorize writes into another chunk's stripe. */
	type = BT_BLOCK_DATA;
	REQUIRE(find_leaf(fs, fs->chunk_tree, match_chunk, &type, leaf, &logical, &slot));
	chunk = leaf_data(leaf, slot);
	stripes = (void *)(chunk + 1);
	REQUIRE(
	    metadata_physical != 0 && metadata_physical + bt_u64(chunk->length) <= fs->device_size);
	bt_put64(&stripes[0].offset, metadata_physical);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_SYSTEM);
	btrfs_unmount(fs);
	expect_corrupt_map(context, "physical chunk alias");
	free(leaf);
}

static int
match_inline_ref(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument)
{
	const uint8_t *type = argument;

	(void)fs;
	(void)cursor;
	return (record->key.type == BT_METADATA_ITEM || record->key.type == BT_EXTENT_ITEM) &&
	    record->size > sizeof(struct bt_disk_extent_item) &&
	    record->data[sizeof(struct bt_disk_extent_item)] == *type;
}

static void
expect_audit_failure(struct context *context, const char *name)
{
	struct reference_audit audit;
	struct btrfs_fs *fs;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(reference_audit(fs, &audit) != 0);
	btrfs_unmount(fs);
	truncate_writes(context->device, 0);
	printf("reference audit detects %s: PASS (%s)\n", name, audit.failure);
}

/* The audit is an oracle only if it rejects damaged references. */
void
audit_self_test(struct context *context)
{
	struct bt_disk_extent_item *extent;
	struct bt_disk_data_ref *data;
	struct btrfs_fs *fs;
	struct bt_root extents;
	struct bt_le64 value;
	uint8_t *leaf;
	uint8_t *reference;
	uint8_t type;
	uint64_t logical;
	uint32_t slot;

	leaf = malloc(BT_MAX_NODE_SIZE);
	REQUIRE(leaf != NULL);
	audit_state(context, "fixture");
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(bt_find_root(fs, BT_EXTENT_TREE, &extents) == BTRFS_OK);
	type = BT_TREE_BLOCK_REF;
	REQUIRE(find_leaf(fs, extents, match_inline_ref, &type, leaf, &logical, &slot));
	reference = (uint8_t *)leaf_data(leaf, slot) + sizeof(*extent) + 1;
	memcpy(&value, reference, sizeof(value));
	bt_put64(&value, bt_u64(value) + 1);
	memcpy(reference, &value, sizeof(value));
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_audit_failure(context, "a wrong tree reference");

	REQUIRE(find_leaf(fs, extents, match_inline_ref, &type, leaf, &logical, &slot));
	extent = leaf_data(leaf, slot);
	bt_put64(&extent->refs, bt_u64(extent->refs) + 1);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_audit_failure(context, "an inconsistent reference total");

	type = BT_EXTENT_DATA_REF;
	REQUIRE(find_leaf(fs, extents, match_inline_ref, &type, leaf, &logical, &slot));
	extent = leaf_data(leaf, slot);
	data = (void *)((uint8_t *)extent + sizeof(*extent) + 1);
	bt_put64(&extent->refs, bt_u64(extent->refs) + 1);
	bt_put32(&data->count, bt_u32(data->count) + 1);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_audit_failure(context, "an overcounted data reference");
	btrfs_unmount(fs);
	REQUIRE(context->image.live_allocations == 0);
	free(leaf);
}

static int
inline_regular(const struct btrfs_inode *inode)
{
	return (inode->mode & BTRFS_MODE_TYPE) == BTRFS_MODE_REGULAR && inode->size <= INLINE_LIMIT;
}

static size_t
collect_inline(struct btrfs_fs *fs, const char *path, struct btrfs_object_id *ids, size_t count,
    size_t capacity)
{
	struct btrfs_directory *stream;
	struct btrfs_dir_entry entry;
	struct btrfs_inode directory;
	struct btrfs_inode inode;
	uint64_t cookie;
	enum btrfs_result error;

	REQUIRE(btrfs_image_lookup(fs, path, &directory) == BTRFS_OK);
	REQUIRE(btrfs_directory_open(fs, &directory, 0, &stream) == BTRFS_OK);
	while ((error = btrfs_directory_next(stream, &entry, &cookie)) == BTRFS_OK) {
		REQUIRE(btrfs_get_inode(fs, entry.id, &inode) == BTRFS_OK);
		if (inline_regular(&inode)) {
			REQUIRE(count < capacity);
			ids[count++] = entry.id;
		}
	}
	REQUIRE(error == BTRFS_NOT_FOUND);
	btrfs_directory_close(stream);
	return count;
}

/* Replaces the first count inline files in one transaction: the commit's
 * result, or the first failing edit's result with *edits_ok cleared. */
static enum btrfs_result
replace_inline(struct context *context, struct btrfs_fs *fs, const struct btrfs_object_id *ids,
    size_t count, int *edits_ok)
{
	struct btrfs_transaction *transaction;
	struct btrfs_time time = { 1700000000, 0 };
	uint8_t data[INLINE_LIMIT];
	size_t i;
	enum btrfs_result result = BTRFS_OK;

	memset(data, 0x33, sizeof(data));
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	for (i = 0; result == BTRFS_OK && i < count; i++) {
		result =
		    btrfs_transaction_write_inline(transaction, ids[i], data, INLINE_LIMIT, time);
	}
	*edits_ok = result == BTRFS_OK;
	if (result == BTRFS_OK) {
		result = btrfs_transaction_commit(transaction);
		/* A failed commit leaves the transaction failed. */
		REQUIRE(result == BTRFS_OK || btrfs_transaction_commit(transaction) == result);
	}
	btrfs_transaction_destroy(transaction);
	return result;
}

/* Every edit fits but the commit's accounting fixed point (extent items, block
 * groups, free space and root items of the CoW blocks) does not: the commit
 * fails with NO_SPACE before any media write, and the volume stays usable for
 * a transaction one edit smaller. The boundary is found by bisection over the
 * number of replaced files. */
static void
exhaustion_commit_test(
    struct context *context, struct btrfs_fs *fs, const struct btrfs_object_id *ids, size_t count)
{
	struct reference_audit audit;
	struct btrfs_fs *after;
	size_t low = 0;
	size_t high = count;
	size_t middle;
	int edits_ok = 0;
	enum btrfs_result result;

	/* Invariant: low replacements commit; high replacements do not. */
	REQUIRE(replace_inline(context, fs, ids, high, &edits_ok) != BTRFS_OK);
	truncate_writes(context->device, 0);
	while (high - low > 1) {
		middle = low + (high - low) / 2;
		result = replace_inline(context, fs, ids, middle, &edits_ok);
		truncate_writes(context->device, 0);
		if (result == BTRFS_OK) {
			low = middle;
		} else {
			high = middle;
		}
	}
	context->device->issued = 0;
	context->device->flushes = 0;
	REQUIRE(replace_inline(context, fs, ids, high, &edits_ok) == BTRFS_NO_SPACE);
	REQUIRE(edits_ok);
	REQUIRE(context->device->count == 0 && context->device->issued == 0 &&
	    context->device->flushes == 0);
	REQUIRE(replace_inline(context, fs, ids, low, &edits_ok) == BTRFS_OK);
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &after) == BTRFS_OK);
	REQUIRE(reference_audit(after, &audit) == 0);
	btrfs_unmount(after);
	truncate_writes(context->device, 0);
	printf("commit exhaustion: %zu edits fit but their commit returns NO_SPACE before any "
	       "write; %zu commit PASS\n",
	    high, low);
}

/* Tree nodes a grouped inline rewrite declares. */
#define GROUPED_REWRITE_NODES 16U

/* The exhaustion test runs one thread: the volume never has to wait. */
static void
single_lock(void *context)
{
	(void)context;
}

static void
single_wait(void *context, const void *channel)
{
	(void)context;
	(void)channel;
	REQUIRE(0);
}

static void
single_wake(void *context, const void *channel)
{
	(void)context;
	(void)channel;
}

/* Grouped operations on full metadata: the volume refuses an operation
 * before its first change, commits the running transaction when it lacks
 * room, and no commit runs out of space; every acknowledged rewrite is on the
 * medium after the last sync. The device is restored afterwards. */
static void
grouped_exhaustion_test(struct context *context, const struct btrfs_object_id *ids, size_t count,
    const uint8_t *data, size_t size)
{
	struct btrfs_volume_locks locks = { NULL, single_lock, single_lock, single_wait,
		single_wake };
	struct btrfs_volume *volume;
	struct btrfs_volume_view *view;
	struct btrfs_transaction *transaction;
	struct btrfs_time time = { 1700000001, 0 };
	struct btrfs_inode inode;
	const struct btrfs_fs *fs;
	uint8_t check[INLINE_LIMIT];
	uint64_t generation;
	size_t acknowledged;
	size_t completed;
	enum btrfs_result result = BTRFS_OK;

	REQUIRE(btrfs_volume_open(&context->env, &context->writer, &locks, BTRFS_TOP_LEVEL_TREE,
		    &volume) == BTRFS_OK);
	generation = btrfs_volume_generation(volume);
	for (acknowledged = 0; acknowledged < count; acknowledged++) {
		result = btrfs_volume_join(volume, GROUPED_REWRITE_NODES, &transaction);
		if (result != BTRFS_OK) {
			break;
		}
		REQUIRE(btrfs_transaction_write_inline(
			    transaction, ids[acknowledged], data, size, time) == BTRFS_OK);
		btrfs_volume_leave(volume, transaction);
		REQUIRE(btrfs_volume_failure(volume) == BTRFS_OK);
	}
	REQUIRE(result == BTRFS_NO_SPACE && acknowledged != 0);
	REQUIRE(btrfs_volume_failure(volume) == BTRFS_OK);
	REQUIRE(btrfs_volume_sync(volume, btrfs_volume_pending(volume)) == BTRFS_OK);
	REQUIRE(btrfs_volume_generation(volume) > generation);
	fs = btrfs_volume_pin(volume, &view);
	for (completed = 0; completed < acknowledged; completed++) {
		REQUIRE(
		    btrfs_get_inode(fs, ids[completed], &inode) == BTRFS_OK && inode.size == size);
	}
	REQUIRE(btrfs_read(fs, &inode, 0, check, size, &completed) == BTRFS_OK &&
	    completed == size && memcmp(check, data, size) == 0);
	btrfs_volume_unpin(volume, view);
	printf("grouped exhaustion: %zu rewrites acknowledged in %llu commits, the next refused "
	       "before any change PASS\n",
	    acknowledged, (unsigned long long)(btrfs_volume_generation(volume) - generation));
	btrfs_volume_close(volume);
	truncate_writes(context->device, 0);
}

/* The Linux fixture's metadata is full and fragmented. A transaction needing
 * more nodes than remain must fail with NO_SPACE before any media write. */
void
exhaustion_test(struct context *context)
{
	struct bt_mutation_allocator allocator;
	struct btrfs_object_id *ids;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct btrfs_time time = { 1700000000, 0 };
	struct bt_space *space;
	struct bt_root extents;
	struct btrfs_inode inode;
	uint8_t data[INLINE_LIMIT];
	uint8_t *big;
	uint64_t logical;
	size_t available = 0;
	size_t count = 0;
	size_t i;
	enum btrfs_result result;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(bt_find_root(fs, BT_EXTENT_TREE, &extents) == BTRFS_OK);
	REQUIRE(bt_space_create(fs, extents, TRANSACTION_NODE_LIMIT, &space) == BTRFS_OK);
	bt_space_allocator(space, &allocator);
	while (available < RESERVATION_PROBE_LIMIT &&
	    allocator.reserve(allocator.context, BTRFS_TOP_LEVEL_TREE, 0, &logical) == BTRFS_OK) {
		available++;
	}
	bt_space_destroy(space);
	REQUIRE(available != 0 && available < TRANSACTION_NODE_LIMIT / 2);
	ids = malloc(RESERVATION_PROBE_LIMIT * sizeof(*ids));
	REQUIRE(ids != NULL);
	count = collect_inline(fs, "/meta", ids, count, RESERVATION_PROBE_LIMIT);
	count = collect_inline(fs, "/many", ids, count, RESERVATION_PROBE_LIMIT);
	for (i = 0; i < INLINE_LIMIT; i++) {
		data[i] = (uint8_t)i;
	}
	exhaustion_commit_test(context, fs, ids, count);
	grouped_exhaustion_test(context, ids, count, data, INLINE_LIMIT);
	context->device->issued = 0;
	context->device->flushes = 0;
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	result = BTRFS_OK;
	for (i = 0; result == BTRFS_OK && i < count; i++) {
		result =
		    btrfs_transaction_write_inline(transaction, ids[i], data, INLINE_LIMIT, time);
	}
	if (result == BTRFS_OK) {
		result = btrfs_transaction_commit(transaction);
	}
	REQUIRE(result == BTRFS_NO_SPACE);
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_NO_SPACE);
	REQUIRE(context->device->count == 0 && context->device->issued == 0 &&
	    context->device->flushes == 0);
	btrfs_transaction_destroy(transaction);
	/* Data space is exhausted as well: a large write is refused before any
	 * change or I/O, as Linux reserves data space at write time, and the
	 * transaction stays usable. */
	big = malloc(EXHAUSTION_WRITE_BYTES);
	REQUIRE(big != NULL);
	memset(big, 0x5a, EXHAUSTION_WRITE_BYTES);
	REQUIRE(btrfs_image_lookup(fs, "/big", &inode) == BTRFS_OK);
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_write(
		    transaction, inode.id, 0, big, EXHAUSTION_WRITE_BYTES, time) == BTRFS_NO_SPACE);
	REQUIRE(transaction->failure == BTRFS_OK);
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
	REQUIRE(context->device->count == 0 && context->device->issued == 0);
	btrfs_transaction_destroy(transaction);
	free(big);
	check_invariants(fs);
	btrfs_unmount(fs);
	free(ids);
	REQUIRE(context->image.live_allocations == 0);
	printf("metadata and data exhaustion: %zu reservable nodes, %zu files stopped after %zu "
	       "edits; "
	       "no write PASS\n",
	    available, count, i);
}
