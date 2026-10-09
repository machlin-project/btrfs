/* SPDX-License-Identifier: BSD-3-Clause */
#include "namespace_audit.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wire layout of an extended inode reference entry. */
struct extref_entry {
	struct bt_le64 parent;
	struct bt_le64 index;
	struct bt_le16 name_length;
};

_Static_assert(sizeof(struct extref_entry) == 18, "extended reference layout");

enum name_source { SOURCE_ITEM, SOURCE_INDEX, SOURCE_REF, SOURCE_SOURCES };

/* One name as one of DIR_ITEM, DIR_INDEX or an inode reference records it.
 * Subvolume entries name a root instead of an inode. */
struct name_record {
	uint64_t directory;
	uint64_t inode;
	uint64_t index;
	uint16_t length;
	uint8_t type;
	uint8_t subvolume;
	uint8_t extended;
	uint8_t name[BTRFS_NAME_MAX];
};

struct name_list {
	struct name_record *items;
	size_t count;
	size_t capacity;
};

/* File extent coverage of an inode as btrfs check's process_file_extent
 * tracks it: the bytes its items count, where the last one ends and where the
 * first gap starts. */
struct inode_record {
	uint64_t inode;
	uint64_t size;
	uint64_t nbytes;
	uint64_t flags;
	/* VERITY_DESC and VERITY_MERKLE items, and whether the descriptor's
	 * size item (VERITY_DESC offset 0) is among them. */
	uint64_t verity_items;
	int verity_size;
	uint32_t mode;
	uint32_t links;
	uint32_t names;
	uint32_t orphan;
	uint64_t name_bytes;
	uint64_t extent_bytes;
	uint64_t extent_end;
	uint64_t first_gap;
};

struct tree_state {
	const struct btrfs_fs *fs;
	struct namespace_audit *audit;
	uint64_t tree;
	struct name_list names[SOURCE_SOURCES];
	struct inode_record *inodes;
	size_t inode_count;
	size_t inode_capacity;
	uint64_t *orphans;
	size_t orphan_count;
	size_t orphan_capacity;
	uint64_t *owners;
	size_t owner_count;
	size_t owner_capacity;
	int failed;
};

static int
fail(struct tree_state *state, const char *format, ...)
{
	va_list arguments;
	int used;

	used = snprintf(state->audit->failure, sizeof(state->audit->failure),
	    "tree %llu: ", (unsigned long long)state->tree);
	va_start(arguments, format);
	vsnprintf(state->audit->failure + used, sizeof(state->audit->failure) - (size_t)used,
	    format, arguments);
	va_end(arguments);
	state->failed = 1;
	return -1;
}

static void *
grow(void *items, size_t *capacity, size_t count, size_t size)
{
	void *larger;

	if (count < *capacity) {
		return items;
	}
	*capacity = *capacity == 0 ? 64 : *capacity * 2;
	larger = realloc(items, *capacity * size);
	if (larger == NULL) {
		abort();
	}
	return larger;
}

static void
add_name(struct name_list *list, const struct name_record *record)
{
	list->items = grow(list->items, &list->capacity, list->count, sizeof(*list->items));
	list->items[list->count++] = *record;
}

static void
add_value(uint64_t **items, size_t *count, size_t *capacity, uint64_t value)
{
	*items = grow(*items, capacity, *count, sizeof(**items));
	(*items)[(*count)++] = value;
}

static int
compare_names(const void *left, const void *right)
{
	const struct name_record *a = left;
	const struct name_record *b = right;
	int order;

	if (a->directory != b->directory) {
		return a->directory < b->directory ? -1 : 1;
	}
	if (a->length != b->length) {
		return a->length < b->length ? -1 : 1;
	}
	order = memcmp(a->name, b->name, a->length);
	if (order != 0) {
		return order;
	}
	if (a->inode != b->inode) {
		return a->inode < b->inode ? -1 : 1;
	}
	return 0;
}

static int
compare_values(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return a < b ? -1 : a > b;
}

static struct inode_record *
find_inode(struct tree_state *state, uint64_t inode)
{
	size_t low = 0;
	size_t high = state->inode_count;
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (state->inodes[middle].inode == inode) {
			return &state->inodes[middle];
		}
		if (state->inodes[middle].inode < inode) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return NULL;
}

static int
directory_entries(struct tree_state *state, const struct bt_record *record)
{
	const struct bt_disk_dir *header;
	const uint8_t *name;
	const uint8_t *data;
	struct name_record entry;
	struct bt_key location;
	size_t offset = 0;
	size_t count = 0;
	enum btrfs_result error;

	for (;;) {
		error = bt_dir_record(record, &offset, &header, &name, &data);
		if (error == BTRFS_NOT_FOUND) {
			break;
		}
		if (error != BTRFS_OK) {
			return fail(state, "malformed entry item (%llu %u %llu)",
			    (unsigned long long)record->key.objectid, record->key.type,
			    (unsigned long long)record->key.offset);
		}
		count++;
		if (record->key.type == BT_XATTR_ITEM) {
			state->audit->xattrs++;
			continue;
		}
		memset(&entry, 0, sizeof(entry));
		location = bt_key_decode(&header->location);
		entry.directory = record->key.objectid;
		entry.length = bt_u16(header->name_length);
		entry.type = header->type;
		memcpy(entry.name, name, entry.length);
		if (location.type == BT_ROOT_ITEM) {
			entry.subvolume = 1;
			entry.inode = location.objectid;
		} else if (location.type == BT_INODE_ITEM && location.offset == 0) {
			entry.inode = location.objectid;
		} else {
			return fail(state, "entry in %llu has location type %u",
			    (unsigned long long)entry.directory, location.type);
		}
		if (record->key.type == BT_DIR_INDEX) {
			if (count > 1 || record->key.offset < BT_DIR_START_INDEX) {
				return fail(state, "DIR_INDEX %llu of %llu is invalid",
				    (unsigned long long)record->key.offset,
				    (unsigned long long)entry.directory);
			}
			entry.index = record->key.offset;
			add_name(&state->names[SOURCE_INDEX], &entry);
		} else {
			add_name(&state->names[SOURCE_ITEM], &entry);
		}
	}
	if (count > 1 && record->key.type == BT_DIR_ITEM) {
		state->audit->collisions++;
	}
	return 0;
}

static int
inode_refs(struct tree_state *state, const struct bt_record *record)
{
	const struct bt_disk_inode_ref *ref;
	struct name_record entry;
	size_t position = 0;
	size_t length;

	while (position < record->size) {
		if (record->size - position < sizeof(*ref)) {
			return fail(state, "short INODE_REF of %llu",
			    (unsigned long long)record->key.objectid);
		}
		ref = (const void *)(record->data + position);
		length = bt_u16(ref->name_length);
		if (length == 0 || length > BTRFS_NAME_MAX ||
		    length > record->size - position - sizeof(*ref)) {
			return fail(state, "bad INODE_REF name of %llu",
			    (unsigned long long)record->key.objectid);
		}
		position += sizeof(*ref) + length;
		/* The root directory's ".." reference names no directory entry. */
		if (record->key.objectid == BTRFS_ROOT_INODE &&
		    record->key.offset == BTRFS_ROOT_INODE) {
			if (length != 2 || memcmp(ref + 1, "..", 2) != 0 ||
			    bt_u64(ref->index) != 0) {
				return fail(state, "root directory reference is not \"..\"");
			}
			continue;
		}
		memset(&entry, 0, sizeof(entry));
		entry.directory = record->key.offset;
		entry.inode = record->key.objectid;
		entry.index = bt_u64(ref->index);
		entry.length = (uint16_t)length;
		memcpy(entry.name, ref + 1, length);
		add_name(&state->names[SOURCE_REF], &entry);
	}
	return 0;
}

static int
extended_refs(struct tree_state *state, const struct bt_record *record)
{
	const struct extref_entry *ref;
	struct name_record entry;
	size_t position = 0;
	size_t length;

	while (position < record->size) {
		if (record->size - position < sizeof(*ref)) {
			return fail(state, "short INODE_EXTREF of %llu",
			    (unsigned long long)record->key.objectid);
		}
		ref = (const void *)(record->data + position);
		length = bt_u16(ref->name_length);
		if (length == 0 || length > BTRFS_NAME_MAX ||
		    length > record->size - position - sizeof(*ref)) {
			return fail(state, "bad INODE_EXTREF name of %llu",
			    (unsigned long long)record->key.objectid);
		}
		/* Linux's btrfs_extref_hash seeds CRC32C with the parent number. */
		if (bt_crc32c((uint32_t)bt_u64(ref->parent), ref + 1, length) !=
		    record->key.offset) {
			return fail(state, "INODE_EXTREF hash of %llu",
			    (unsigned long long)record->key.objectid);
		}
		memset(&entry, 0, sizeof(entry));
		entry.directory = bt_u64(ref->parent);
		entry.inode = record->key.objectid;
		entry.index = bt_u64(ref->index);
		entry.length = (uint16_t)length;
		entry.extended = 1;
		memcpy(entry.name, ref + 1, length);
		add_name(&state->names[SOURCE_REF], &entry);
		position += sizeof(*ref) + length;
	}
	return 0;
}

/* Items arrive in key order, so an inode's file extents follow its inode
 * item; items of a missing inode are reported by check_inodes. Inline extents
 * count their decoded bytes and cover whole sectors; hole items (disk_bytenr
 * 0) cover their range without counting. */
static int
file_extent(struct tree_state *state, const struct bt_record *record)
{
	const struct bt_disk_extent *extent = (const void *)record->data;
	struct inode_record *inode = NULL;
	uint64_t sector = state->fs->info.sector_size;
	uint64_t length;

	if (state->inode_count != 0 &&
	    state->inodes[state->inode_count - 1].inode == record->key.objectid) {
		inode = &state->inodes[state->inode_count - 1];
	}
	if (inode == NULL) {
		return 0;
	}
	if (record->size < sizeof(extent->header) ||
	    (extent->header.type != BT_EXTENT_INLINE && record->size != sizeof(*extent))) {
		return fail(state, "malformed file extent %llu:%llu",
		    (unsigned long long)record->key.objectid,
		    (unsigned long long)record->key.offset);
	}
	if (record->key.offset < inode->extent_end) {
		return fail(state, "file extents of inode %llu overlap at %llu",
		    (unsigned long long)inode->inode, (unsigned long long)record->key.offset);
	}
	if (record->key.offset > inode->extent_end && inode->first_gap == UINT64_MAX) {
		inode->first_gap = inode->extent_end;
	}
	if (extent->header.type == BT_EXTENT_INLINE) {
		length = bt_u64(extent->header.ram_bytes);
		inode->extent_bytes += length;
		length += (sector - length % sector) % sector;
	} else {
		length = bt_u64(extent->length);
		if (bt_u64(extent->disk_bytenr) != 0) {
			inode->extent_bytes += length;
		} else {
			state->audit->hole_items++;
		}
	}
	inode->extent_end = record->key.offset + length;
	return 0;
}

static int
collect(struct tree_state *state, struct bt_root root)
{
	const struct bt_disk_inode *item;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key first = { 0, 0, 0 };
	struct inode_record *inode;
	enum btrfs_result error;
	int result = 0;

	bt_cursor_init(&cursor, state->fs, root);
	error = bt_cursor_seek(&cursor, first, 0);
	while (error == BTRFS_OK && result == 0) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid == BT_ORPHAN_OBJECTID &&
		    record.key.type == BT_ORPHAN_ITEM) {
			add_value(&state->orphans, &state->orphan_count, &state->orphan_capacity,
			    record.key.offset);
		} else if (record.key.objectid >= BTRFS_ROOT_INODE &&
		    record.key.objectid <= BT_LAST_FREE_OBJECTID) {
			add_value(&state->owners, &state->owner_count, &state->owner_capacity,
			    record.key.objectid);
			switch (record.key.type) {
			case BT_INODE_ITEM:
				if (record.size != sizeof(*item) || record.key.offset != 0) {
					result = fail(state, "malformed inode %llu",
					    (unsigned long long)record.key.objectid);
					break;
				}
				item = (const void *)record.data;
				state->inodes = grow(state->inodes, &state->inode_capacity,
				    state->inode_count, sizeof(*state->inodes));
				inode = &state->inodes[state->inode_count++];
				memset(inode, 0, sizeof(*inode));
				inode->inode = record.key.objectid;
				inode->size = bt_u64(item->size);
				inode->nbytes = bt_u64(item->nbytes);
				inode->mode = bt_u32(item->mode);
				inode->links = bt_u32(item->links);
				inode->flags = bt_u64(item->flags);
				inode->first_gap = UINT64_MAX;
				break;
			case BT_VERITY_DESC_ITEM:
			case BT_VERITY_MERKLE_ITEM:
				inode = find_inode(state, record.key.objectid);
				if (inode != NULL) {
					inode->verity_items++;
					inode->verity_size |=
					    record.key.type == BT_VERITY_DESC_ITEM &&
					    record.key.offset == 0;
				}
				break;
			case BT_INODE_REF:
				result = inode_refs(state, &record);
				break;
			case BT_INODE_EXTREF:
				result = extended_refs(state, &record);
				break;
			case BT_DIR_ITEM:
			case BT_DIR_INDEX:
			case BT_XATTR_ITEM:
				result = directory_entries(state, &record);
				break;
			case BT_EXTENT_DATA:
				result = file_extent(state, &record);
				break;
			default:
				break;
			}
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (result == 0 && error != BTRFS_NOT_FOUND) {
		result = fail(state, "walk: %s", btrfs_result_string(error));
	}
	return result;
}

static int
same_name(const struct name_record *a, const struct name_record *b)
{
	return a->directory == b->directory && a->inode == b->inode && a->length == b->length &&
	    memcmp(a->name, b->name, a->length) == 0;
}

static int
describe(struct tree_state *state, const char *what, const struct name_record *record)
{
	return fail(state, "%s: directory %llu inode %llu name %.*s", what,
	    (unsigned long long)record->directory, (unsigned long long)record->inode,
	    (int)record->length, (const char *)record->name);
}

/* DIR_ITEM, DIR_INDEX and inode references must list the same names. */
static int
match_names(struct tree_state *state)
{
	struct name_list *items = &state->names[SOURCE_ITEM];
	struct name_list *indexes = &state->names[SOURCE_INDEX];
	struct name_list *refs = &state->names[SOURCE_REF];
	const struct name_record *item;
	const struct name_record *index;
	const struct name_record *ref;
	struct inode_record *inode;
	struct inode_record *directory;
	size_t i;
	size_t r = 0;

	for (i = 0; i < SOURCE_SOURCES; i++) {
		qsort(state->names[i].items, state->names[i].count, sizeof(struct name_record),
		    compare_names);
	}
	if (items->count != indexes->count) {
		return fail(state, "%zu DIR_ITEM names but %zu DIR_INDEX names", items->count,
		    indexes->count);
	}
	for (i = 0; i < items->count; i++) {
		item = &items->items[i];
		index = &indexes->items[i];
		if (!same_name(item, index) || item->type != index->type ||
		    item->subvolume != index->subvolume) {
			return describe(state, "DIR_ITEM without matching DIR_INDEX", item);
		}
		if (i > 0 && items->items[i - 1].directory == item->directory &&
		    items->items[i - 1].length == item->length &&
		    memcmp(items->items[i - 1].name, item->name, item->length) == 0) {
			return describe(state, "duplicate name", item);
		}
		directory = find_inode(state, item->directory);
		if (directory == NULL ||
		    (directory->mode & BTRFS_MODE_TYPE) != BTRFS_MODE_DIRECTORY) {
			return describe(state, "entry outside a directory", item);
		}
		directory->name_bytes += 2 * (uint64_t)item->length;
		if (item->subvolume) {
			state->audit->subvolume_entries++;
			continue;
		}
		if (r == refs->count) {
			return describe(state, "entry without inode reference", item);
		}
		ref = &refs->items[r++];
		if (!same_name(item, ref) || ref->index != index->index) {
			return describe(state, "entry and inode reference differ", item);
		}
		inode = find_inode(state, item->inode);
		if (inode == NULL) {
			return describe(state, "entry names a missing inode", item);
		}
		if (btrfs_mode_for_type(item->type) != (inode->mode & BTRFS_MODE_TYPE)) {
			return describe(state, "entry type differs from inode mode", item);
		}
		inode->names++;
		state->audit->names++;
		state->audit->extended_names += ref->extended;
	}
	if (r != refs->count) {
		return describe(state, "inode reference without entry", &refs->items[r]);
	}
	return 0;
}

static int
check_inodes(struct tree_state *state)
{
	struct inode_record *inode;
	size_t i;

	qsort(state->orphans, state->orphan_count, sizeof(*state->orphans), compare_values);
	/* A linked inode keeps an orphan item only while fs-verity is being
	 * enabled on it, which Linux does for regular files alone. */
	for (i = 0; i < state->orphan_count; i++) {
		inode = find_inode(state, state->orphans[i]);
		if (inode == NULL ||
		    (inode->links != 0 && (inode->mode & BTRFS_MODE_TYPE) != BTRFS_MODE_REGULAR) ||
		    (i > 0 && state->orphans[i - 1] == state->orphans[i])) {
			return fail(state, "orphan item for inode %llu",
			    (unsigned long long)state->orphans[i]);
		}
		inode->orphan = 1;
		state->audit->orphans++;
	}
	for (i = 0; i < state->owner_count; i++) {
		if (find_inode(state, state->owners[i]) == NULL) {
			return fail(state, "items of missing inode %llu",
			    (unsigned long long)state->owners[i]);
		}
	}
	for (i = 0; i < state->inode_count; i++) {
		inode = &state->inodes[i];
		state->audit->inodes++;
		if (inode->inode == BTRFS_ROOT_INODE) {
			if (inode->names != 0 || inode->links != 1 ||
			    (inode->mode & BTRFS_MODE_TYPE) != BTRFS_MODE_DIRECTORY) {
				return fail(state, "root directory has %u names and %u links",
				    inode->names, inode->links);
			}
		} else if (inode->links != inode->names) {
			return fail(state, "inode %llu has %u links but %u names",
			    (unsigned long long)inode->inode, inode->links, inode->names);
		} else if (inode->links == 0 && !inode->orphan) {
			return fail(state, "unlinked inode %llu has no orphan item",
			    (unsigned long long)inode->inode);
		}
		if ((inode->mode & BTRFS_MODE_TYPE) == BTRFS_MODE_REGULAR ||
		    (inode->mode & BTRFS_MODE_TYPE) == BTRFS_MODE_SYMLINK) {
			if (inode->extent_bytes != inode->nbytes) {
				return fail(state, "inode %llu counts %llu bytes, its extents %llu",
				    (unsigned long long)inode->inode,
				    (unsigned long long)inode->nbytes,
				    (unsigned long long)inode->extent_bytes);
			}
			/* Without NO_HOLES, items cover every byte below the size. */
			if (!(state->fs->info.incompat_features & BT_FEATURE_NO_HOLES) &&
			    inode->links != 0 &&
			    (inode->extent_end < inode->size || inode->first_gap < inode->size)) {
				return fail(state,
				    "file extents of inode %llu leave a gap below %llu",
				    (unsigned long long)inode->inode,
				    (unsigned long long)inode->size);
			}
		}
		/* fs-verity items belong to a regular file that has fs-verity, with
		 * its descriptor, or to one an enable or its cleanup has not
		 * finished (its orphan item). */
		if ((inode->flags & BT_INODE_RO_VERITY) != 0
			? (inode->mode & BTRFS_MODE_TYPE) != BTRFS_MODE_REGULAR ||
			    !inode->verity_size
			: inode->verity_items != 0 && !inode->orphan) {
			return fail(state, "inode %llu has %llu fs-verity items, flags 0x%llx",
			    (unsigned long long)inode->inode,
			    (unsigned long long)inode->verity_items,
			    (unsigned long long)inode->flags);
		}
		state->audit->verity_items += inode->verity_items;
		if ((inode->mode & BTRFS_MODE_TYPE) == BTRFS_MODE_DIRECTORY) {
			if (inode->names > 1) {
				return fail(state, "directory %llu has %u names",
				    (unsigned long long)inode->inode, inode->names);
			}
			if (inode->size != inode->name_bytes) {
				return fail(state, "directory %llu size %llu, names %llu",
				    (unsigned long long)inode->inode,
				    (unsigned long long)inode->size,
				    (unsigned long long)inode->name_bytes);
			}
		}
	}
	return 0;
}

static int
audit_tree(const struct btrfs_fs *fs, struct namespace_audit *audit, struct bt_root root)
{
	struct tree_state state;
	size_t i;
	int result;

	memset(&state, 0, sizeof(state));
	state.fs = fs;
	state.audit = audit;
	state.tree = root.owner;
	result = collect(&state, root);
	if (result == 0) {
		qsort(state.inodes, state.inode_count, sizeof(*state.inodes), compare_values);
		for (i = 1; result == 0 && i < state.inode_count; i++) {
			if (state.inodes[i - 1].inode == state.inodes[i].inode) {
				result = fail(&state, "duplicate inode %llu",
				    (unsigned long long)state.inodes[i].inode);
			}
		}
	}
	if (result == 0) {
		result = match_names(&state);
	}
	if (result == 0) {
		result = check_inodes(&state);
	}
	for (i = 0; i < SOURCE_SOURCES; i++) {
		free(state.names[i].items);
	}
	free(state.inodes);
	free(state.orphans);
	free(state.owners);
	return result;
}

/* The entry of name in a packed DIR_ITEM or DIR_INDEX record, if present. */
static const struct bt_disk_dir *
packed_entry(const struct bt_record *record, const uint8_t *name, size_t length)
{
	const struct bt_disk_dir *header;
	const uint8_t *entry_name;
	const uint8_t *data;
	size_t position = 0;

	while (bt_dir_record(record, &position, &header, &entry_name, &data) == BTRFS_OK) {
		if (bt_u16(header->name_length) == length &&
		    memcmp(entry_name, name, length) == 0) {
			return header;
		}
	}
	return NULL;
}

/* Whether tree holds a DIR_ITEM and a DIR_INDEX entry for name in directory
 * at index whose location is the root of child. */
static int
subvolume_entry(const struct btrfs_fs *fs, uint64_t tree, uint64_t directory, uint64_t index,
    uint64_t child, const uint8_t *name, size_t length)
{
	const struct bt_disk_dir *header;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	struct bt_key keys[2];
	struct bt_key location;
	int found = 0;
	int pass;

	if (bt_find_root(fs, tree, &root) != BTRFS_OK) {
		return 0;
	}
	keys[0] =
	    (struct bt_key){ directory, bt_crc32c(UINT32_MAX - 1U, name, length), BT_DIR_ITEM };
	keys[1] = (struct bt_key){ directory, index, BT_DIR_INDEX };
	for (pass = 0; pass < 2; pass++) {
		bt_cursor_init(&cursor, fs, root);
		header = NULL;
		if (bt_cursor_seek(&cursor, keys[pass], 0) == BTRFS_OK &&
		    bt_cursor_record(&cursor, &record) == BTRFS_OK &&
		    bt_key_compare(record.key, keys[pass]) == 0) {
			header = packed_entry(&record, name, length);
		}
		if (header != NULL) {
			location = bt_key_decode(&header->location);
			found += location.objectid == child && location.type == BT_ROOT_ITEM;
		}
		bt_cursor_fini(&cursor);
	}
	return found == 2;
}

struct root_record {
	uint64_t id;
	uint32_t refs;
	uint32_t backrefs;
	int orphan;
	uint8_t uuid[BTRFS_UUID_SIZE];
	uint8_t received[BTRFS_UUID_SIZE];
};

static struct root_record *
find_root_record(struct root_record *roots, size_t count, uint64_t id)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (roots[i].id == id) {
			return &roots[i];
		}
	}
	return NULL;
}

/* Checks the UUID tree against the live roots: every listed id is a live
 * root with that UUID (or received UUID), and every live root's UUIDs are
 * listed. */
static int
audit_uuids(const struct btrfs_fs *fs, struct namespace_audit *audit, struct root_record *roots,
    size_t count)
{
	static const uint8_t empty[BTRFS_UUID_SIZE];
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	struct bt_key first = { 0, 0, 0 };
	struct root_record *found;
	uint8_t uuid[BTRFS_UUID_SIZE];
	size_t listed = 0;
	size_t expected = 0;
	size_t i;
	size_t j;
	enum btrfs_result error;

	if (bt_find_root(fs, BT_UUID_TREE, &root) != BTRFS_OK) {
		return 0;
	}
	bt_cursor_init(&cursor, fs, root);
	error = bt_cursor_seek(&cursor, first, 0);
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		/* The key holds the UUID's two little-endian halves. */
		for (j = 0; j < sizeof(uuid) / 2; j++) {
			uuid[j] = (uint8_t)(record.key.objectid >> (8 * j));
			uuid[j + sizeof(uuid) / 2] = (uint8_t)(record.key.offset >> (8 * j));
		}
		if ((record.key.type != BT_UUID_SUBVOL &&
			record.key.type != BT_UUID_RECEIVED_SUBVOL) ||
		    record.size == 0 || record.size % sizeof(struct bt_le64) != 0) {
			bt_cursor_fini(&cursor);
			snprintf(audit->failure, sizeof(audit->failure), "malformed UUID item");
			return -1;
		}
		for (j = 0; j < record.size / sizeof(struct bt_le64); j++) {
			found = find_root_record(
			    roots, count, bt_u64(((const struct bt_le64 *)record.data)[j]));
			if (found == NULL || found->refs == 0 ||
			    memcmp(
				record.key.type == BT_UUID_SUBVOL ? found->uuid : found->received,
				uuid, sizeof(uuid)) != 0) {
				bt_cursor_fini(&cursor);
				snprintf(audit->failure, sizeof(audit->failure),
				    "UUID tree names %llu, which is not a live root with that UUID",
				    (unsigned long long)bt_u64(
					((const struct bt_le64 *)record.data)[j]));
				return -1;
			}
			listed++;
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	for (i = 0; i < count; i++) {
		if (roots[i].refs != 0) {
			expected += memcmp(roots[i].uuid, empty, sizeof(empty)) != 0;
			expected += memcmp(roots[i].received, empty, sizeof(empty)) != 0;
		}
	}
	if (error != BTRFS_NOT_FOUND || listed != expected) {
		snprintf(audit->failure, sizeof(audit->failure),
		    "UUID tree lists %zu live root UUIDs, expected %zu", listed, expected);
		return -1;
	}
	return 0;
}

/* Cross-tree subvolume records: each ROOT_REF has an identical ROOT_BACKREF
 * and names the DIR_ITEM and DIR_INDEX entry of the child in its parent tree;
 * a live subvolume has refs back references (the top level none), a dead one
 * (refs 0) none and an orphan item. Entries without root references are
 * placeholders that snapshots copied, which Linux allows. */
static int
audit_subvolumes(const struct btrfs_fs *fs, struct namespace_audit *audit)
{
	const struct bt_disk_root_full *item;
	const struct bt_disk_root_ref *ref;
	struct root_record *roots = NULL;
	struct root_record *found;
	struct bt_cursor cursor;
	struct bt_cursor mirror;
	struct bt_record record;
	struct bt_record other;
	struct bt_key first = { 0, 0, 0 };
	struct bt_key key;
	size_t count = 0;
	size_t capacity = 0;
	size_t i;
	enum btrfs_result error;
	int result = 0;

	bt_cursor_init(&cursor, fs, fs->root_tree);
	error = bt_cursor_seek(&cursor, first, 0);
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.type == BT_ROOT_ITEM && bt_file_tree(record.key.objectid) &&
		    record.size >= sizeof(*item)) {
			item = (const void *)record.data;
			roots = grow(roots, &capacity, count, sizeof(*roots));
			memset(&roots[count], 0, sizeof(roots[count]));
			roots[count].id = record.key.objectid;
			roots[count].refs = bt_u32(item->legacy.refs);
			memcpy(roots[count].uuid, item->uuid, BTRFS_UUID_SIZE);
			memcpy(roots[count].received, item->received_uuid, BTRFS_UUID_SIZE);
			count++;
		}
		error = bt_cursor_next(&cursor);
	}
	error = bt_cursor_seek(&cursor, first, 0);
	while (error == BTRFS_OK && result == 0) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.type == BT_ROOT_REF || record.key.type == BT_ROOT_BACKREF) {
			ref = (const void *)record.data;
			key = (struct bt_key){ record.key.offset, record.key.objectid,
				record.key.type == BT_ROOT_REF ? BT_ROOT_BACKREF : BT_ROOT_REF };
			bt_cursor_init(&mirror, fs, fs->root_tree);
			result = record.size < sizeof(*ref) ||
				record.size != sizeof(*ref) + bt_u16(ref->name_length) ||
				bt_cursor_seek(&mirror, key, 0) != BTRFS_OK ||
				bt_cursor_record(&mirror, &other) != BTRFS_OK ||
				bt_key_compare(other.key, key) != 0 || other.size != record.size ||
				memcmp(other.data, record.data, record.size) != 0
			    ? -1
			    : 0;
			bt_cursor_fini(&mirror);
			if (result == 0 && record.key.type == BT_ROOT_REF) {
				audit->subvolume_refs++;
				result =
				    subvolume_entry(fs, record.key.objectid, bt_u64(ref->directory),
					bt_u64(ref->index), record.key.offset,
					(const uint8_t *)(ref + 1), bt_u16(ref->name_length))
				    ? 0
				    : -1;
			} else if (result == 0) {
				found = find_root_record(roots, count, record.key.objectid);
				result = found == NULL ? -1 : 0;
				if (found != NULL) {
					found->backrefs++;
				}
			}
			if (result != 0) {
				snprintf(audit->failure, sizeof(audit->failure),
				    "root reference (%llu %u %llu) without its pair, entry or root",
				    (unsigned long long)record.key.objectid, record.key.type,
				    (unsigned long long)record.key.offset);
			}
		} else if (record.key.objectid == BT_ORPHAN_OBJECTID &&
		    record.key.type == BT_ORPHAN_ITEM) {
			found = find_root_record(roots, count, record.key.offset);
			if (found != NULL) {
				found->orphan = 1;
			}
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	for (i = 0; result == 0 && i < count; i++) {
		if (roots[i].id == BTRFS_TOP_LEVEL_TREE ? roots[i].backrefs != 0
			: roots[i].refs != 0 ? roots[i].backrefs != roots[i].refs || roots[i].orphan
					     : roots[i].backrefs != 0 || !roots[i].orphan) {
			snprintf(audit->failure, sizeof(audit->failure),
			    "root %llu has %u references, %u back references%s",
			    (unsigned long long)roots[i].id, roots[i].refs, roots[i].backrefs,
			    roots[i].orphan ? " and an orphan item" : "");
			result = -1;
		}
		audit->dead_trees += roots[i].refs == 0;
	}
	if (result == 0) {
		result = audit_uuids(fs, audit, roots, count);
	}
	free(roots);
	return result;
}

int
namespace_audit(const struct btrfs_fs *fs, struct namespace_audit *audit)
{
	const struct bt_disk_root *item;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	struct bt_key first = { 0, 0, 0 };
	enum btrfs_result error;
	int result = 0;

	memset(audit, 0, sizeof(*audit));
	bt_cursor_init(&cursor, fs, fs->root_tree);
	error = bt_cursor_seek(&cursor, first, 0);
	while (error == BTRFS_OK && result == 0) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.type == BT_ROOT_ITEM && bt_file_tree(record.key.objectid)) {
			if (record.size < sizeof(*item)) {
				snprintf(audit->failure, sizeof(audit->failure),
				    "short root item %llu",
				    (unsigned long long)record.key.objectid);
				result = -1;
				break;
			}
			item = (const void *)record.data;
			root.address = bt_u64(item->bytenr);
			root.generation = bt_u64(item->generation);
			root.level = item->level;
			root.owner = record.key.objectid;
			/* A deleted subvolume has no names; the cleaner may have freed
			 * part of its tree. */
			if (bt_u32(item->refs) != 0) {
				audit->trees++;
				result = audit_tree(fs, audit, root);
			}
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (result == 0 && error != BTRFS_NOT_FOUND) {
		snprintf(audit->failure, sizeof(audit->failure), "root tree walk: %s",
		    btrfs_result_string(error));
		result = -1;
	}
	if (result == 0) {
		result = audit_subvolumes(fs, audit);
	}
	return result;
}

/* FNV-1a, 64-bit. */
#define DIGEST_OFFSET UINT64_C(0xcbf29ce484222325)
#define DIGEST_PRIME UINT64_C(0x100000001b3)
/* Directory nesting the digest follows. */
#define DIGEST_DEPTH 64U
#define DIGEST_CHUNK (64U * 1024U)
#define DIGEST_XATTRS (64U * 1024U)
/* Larger files contribute their file extent items and their first and last
 * chunk; sparse test files reach many GiB. */
#define DIGEST_FULL_BYTES (1024U * 1024U)

struct digest_state {
	const struct btrfs_fs *fs;
	struct namespace_digest *digest;
	uint8_t *buffer;
	char *names;
};

static void
digest_mix(struct namespace_digest *digest, const void *bytes, size_t length)
{
	const uint8_t *in = bytes;
	uint64_t word;
	size_t i = 0;

	for (; length - i >= sizeof(word); i += sizeof(word)) {
		memcpy(&word, in + i, sizeof(word));
		digest->hash = (digest->hash ^ word) * DIGEST_PRIME;
	}
	for (; i < length; i++) {
		digest->hash = (digest->hash ^ in[i]) * DIGEST_PRIME;
	}
}

static void
digest_u64(struct namespace_digest *digest, uint64_t value)
{
	digest_mix(digest, &value, sizeof(value));
}

static int
digest_fail(struct digest_state *state, const char *what, enum btrfs_result result,
    struct btrfs_object_id id)
{
	snprintf(state->digest->failure, sizeof(state->digest->failure), "%s of %llu:%llu: %s",
	    what, (unsigned long long)id.tree, (unsigned long long)id.inode,
	    btrfs_result_string(result));
	return -1;
}

/* The file extent items of a large file, through its tree's root as the
 * view resolves it. */
static int
digest_extents(struct digest_state *state, const struct btrfs_inode *inode)
{
	struct bt_root root;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = inode->id.inode, .type = BT_EXTENT_DATA };
	enum btrfs_result result;

	result = bt_find_root(state->fs, inode->id.tree, &root);
	if (result != BTRFS_OK) {
		return digest_fail(state, "tree", result, inode->id);
	}
	bt_cursor_init(&cursor, state->fs, root);
	result = bt_cursor_seek(&cursor, key, 0);
	while (result == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != inode->id.inode || record.key.type != BT_EXTENT_DATA) {
			break;
		}
		digest_u64(state->digest, record.key.offset);
		digest_mix(state->digest, record.data, record.size);
		result = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (result != BTRFS_OK && result != BTRFS_NOT_FOUND) {
		return digest_fail(state, "extents", result, inode->id);
	}
	return 0;
}

static int
digest_inode(struct digest_state *state, const struct btrfs_inode *inode)
{
	struct namespace_digest *digest = state->digest;
	const struct btrfs_time *times[] = { &inode->access_time, &inode->modify_time,
		&inode->change_time, &inode->birth_time };
	size_t length;
	size_t completed;
	size_t value;
	size_t offset;
	uint64_t position;
	unsigned i;
	enum btrfs_result result;

	digest->objects++;
	digest_u64(digest, inode->id.tree);
	digest_u64(digest, inode->id.inode);
	digest_u64(digest, inode->generation);
	digest_u64(digest, inode->size);
	digest_u64(digest, inode->allocated_bytes);
	digest_u64(digest, inode->flags);
	digest_u64(digest, inode->device);
	digest_u64(digest, inode->mode);
	digest_u64(digest, inode->uid);
	digest_u64(digest, inode->gid);
	digest_u64(digest, inode->links);
	for (i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
		digest_u64(digest, (uint64_t)times[i]->seconds);
		digest_u64(digest, times[i]->nanoseconds);
	}
	result = btrfs_list_xattrs(state->fs, inode, state->names, DIGEST_XATTRS, &length);
	if (result != BTRFS_OK) {
		return digest_fail(state, "xattr list", result, inode->id);
	}
	for (offset = 0; offset < length; offset += strlen(state->names + offset) + 1) {
		digest_mix(digest, state->names + offset, strlen(state->names + offset) + 1);
		result = btrfs_get_xattr(state->fs, inode, state->names + offset,
		    strlen(state->names + offset), state->buffer, DIGEST_CHUNK, &value);
		if (result != BTRFS_OK) {
			return digest_fail(state, "xattr", result, inode->id);
		}
		digest_mix(digest, state->buffer, value);
	}
	if ((inode->mode & BTRFS_MODE_TYPE) != BTRFS_MODE_REGULAR &&
	    (inode->mode & BTRFS_MODE_TYPE) != BTRFS_MODE_SYMLINK) {
		return 0;
	}
	if (inode->size > DIGEST_FULL_BYTES && digest_extents(state, inode) != 0) {
		return -1;
	}
	for (position = 0; position < inode->size; position += completed) {
		if (inode->size > DIGEST_FULL_BYTES && position == DIGEST_CHUNK) {
			position = inode->size - DIGEST_CHUNK;
		}
		result =
		    btrfs_read(state->fs, inode, position, state->buffer, DIGEST_CHUNK, &completed);
		if (result != BTRFS_OK || completed == 0) {
			return digest_fail(state, "read", result, inode->id);
		}
		digest_mix(digest, state->buffer, completed);
		digest->bytes += completed;
	}
	return 0;
}

static int
same_time(struct btrfs_time a, struct btrfs_time b)
{
	return a.seconds == b.seconds && a.nanoseconds == b.nanoseconds;
}

static int
same_inode(const struct btrfs_inode *a, const struct btrfs_inode *b)
{
	return a->id.tree == b->id.tree && a->id.inode == b->id.inode &&
	    a->generation == b->generation && a->size == b->size &&
	    a->allocated_bytes == b->allocated_bytes && a->flags == b->flags &&
	    a->device == b->device && a->mode == b->mode && a->uid == b->uid && a->gid == b->gid &&
	    a->links == b->links && same_time(a->access_time, b->access_time) &&
	    same_time(a->modify_time, b->modify_time) &&
	    same_time(a->change_time, b->change_time) && same_time(a->birth_time, b->birth_time);
}

static int
digest_directory(struct digest_state *state, const struct btrfs_inode *directory, unsigned depth)
{
	struct btrfs_directory *stream;
	struct btrfs_dir_entry entry;
	struct btrfs_inode child;
	struct btrfs_inode alone;
	uint64_t cookie;
	enum btrfs_result result;

	if (depth == DIGEST_DEPTH) {
		return digest_fail(state, "depth", BTRFS_RANGE, directory->id);
	}
	result = btrfs_directory_open(state->fs, directory, 0, &stream);
	if (result != BTRFS_OK) {
		return digest_fail(state, "stream", result, directory->id);
	}
	while ((result = btrfs_directory_next(stream, &entry, &cookie)) == BTRFS_OK) {
		digest_mix(state->digest, entry.name, entry.name_length);
		digest_u64(state->digest, entry.type);
		digest_u64(state->digest, cookie);
		/* The stream's own inode path reads what a lookup alone reads. */
		result = btrfs_directory_inode(stream, &entry, &child);
		if (result == BTRFS_OK) {
			result = btrfs_get_inode(state->fs, entry.id, &alone);
		}
		if (result == BTRFS_OK && !same_inode(&child, &alone)) {
			result = BTRFS_CORRUPT;
		}
		if (result != BTRFS_OK) {
			btrfs_directory_close(stream);
			return digest_fail(state, "inode", result, entry.id);
		}
		if (digest_inode(state, &child) != 0 ||
		    ((child.mode & BTRFS_MODE_TYPE) == BTRFS_MODE_DIRECTORY &&
			digest_directory(state, &child, depth + 1) != 0)) {
			btrfs_directory_close(stream);
			return -1;
		}
	}
	btrfs_directory_close(stream);
	return result == BTRFS_NOT_FOUND ? 0 : digest_fail(state, "next", result, directory->id);
}

int
namespace_digest(const struct btrfs_fs *fs, struct namespace_digest *digest)
{
	struct digest_state state;
	struct btrfs_inode root;
	enum btrfs_result result;
	int status = -1;

	memset(digest, 0, sizeof(*digest));
	digest->hash = DIGEST_OFFSET;
	state.fs = fs;
	state.digest = digest;
	state.buffer = malloc(DIGEST_CHUNK);
	state.names = malloc(DIGEST_XATTRS);
	result = btrfs_root(fs, &root);
	if (state.buffer == NULL || state.names == NULL) {
		snprintf(digest->failure, sizeof(digest->failure), "out of memory");
	} else if (result != BTRFS_OK) {
		status = digest_fail(&state, "root", result, root.id);
	} else if (digest_inode(&state, &root) == 0) {
		status = digest_directory(&state, &root, 0);
	}
	free(state.buffer);
	free(state.names);
	return status;
}
