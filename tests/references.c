/* SPDX-License-Identifier: BSD-3-Clause */
#include "references.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A backreference as Linux records it: tree references name a root, shared
 * references name a parent block, data references name (root, inode, offset). */
struct reference {
	uint64_t target;
	uint64_t parent;
	uint64_t root;
	uint64_t inode;
	uint64_t offset;
	uint64_t count;
	uint8_t type;
};

struct reference_list {
	struct reference *items;
	size_t count;
	size_t capacity;
};

struct extent_record {
	uint64_t bytenr;
	uint64_t length;
	uint64_t refs;
	uint64_t flags;
	uint64_t counted;
	int matched;
};

struct audit_state {
	const struct btrfs_fs *fs;
	struct reference_audit *audit;
	struct reference_list expected;
	struct reference_list actual;
	struct extent_record *extents;
	size_t extent_count;
	size_t extent_capacity;
	uint64_t *visited;
	size_t visited_count;
	size_t visited_capacity;
	int failed;
};

static int
fail(struct audit_state *state, const char *format, ...)
{
	va_list arguments;

	if (!state->failed) {
		va_start(arguments, format);
		vsnprintf(state->audit->failure, sizeof(state->audit->failure), format, arguments);
		va_end(arguments);
		state->failed = 1;
	}
	return -1;
}

static void *
grow(void *items, size_t *capacity, size_t count, size_t size)
{
	void *grown;

	if (count < *capacity) {
		return items;
	}
	*capacity = *capacity == 0 ? 256 : *capacity * 2;
	grown = realloc(items, *capacity * size);
	if (grown == NULL) {
		abort();
	}
	return grown;
}

static void
add_reference(struct reference_list *list, struct reference reference)
{
	list->items = grow(list->items, &list->capacity, list->count, sizeof(*list->items));
	list->items[list->count++] = reference;
}

static struct extent_record *
find_extent(struct audit_state *state, uint64_t bytenr)
{
	size_t low = 0;
	size_t high = state->extent_count;
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (state->extents[middle].bytenr < bytenr) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return low < state->extent_count && state->extents[low].bytenr == bytenr
	    ? &state->extents[low]
	    : NULL;
}

/* Returns 1 when newly inserted. */
static int
visit(struct audit_state *state, uint64_t bytenr)
{
	uint64_t *table;
	size_t capacity;
	size_t slot;
	size_t i;

	if (2 * (state->visited_count + 1) > state->visited_capacity) {
		capacity = state->visited_capacity == 0 ? 1024 : state->visited_capacity * 2;
		table = calloc(capacity, sizeof(*table));
		if (table == NULL) {
			abort();
		}
		for (i = 0; i < state->visited_capacity; i++) {
			if (state->visited[i] != 0) {
				for (slot = (size_t)(state->visited[i] *
						UINT64_C(0x9e3779b97f4a7c15)) &
					(capacity - 1);
				    table[slot] != 0; slot = (slot + 1) & (capacity - 1)) {
				}
				table[slot] = state->visited[i];
			}
		}
		free(state->visited);
		state->visited = table;
		state->visited_capacity = capacity;
	}
	for (slot = (size_t)(bytenr * UINT64_C(0x9e3779b97f4a7c15)) & (state->visited_capacity - 1);
	    state->visited[slot] != 0; slot = (slot + 1) & (state->visited_capacity - 1)) {
		if (state->visited[slot] == bytenr) {
			return 0;
		}
	}
	state->visited[slot] = bytenr;
	state->visited_count++;
	return 1;
}

static int
load_inline(struct audit_state *state, struct extent_record *extent, const uint8_t *data,
    size_t size, size_t offset)
{
	const struct bt_disk_data_ref *data_ref;
	const struct bt_disk_shared_data_ref *shared;
	struct bt_le64 value;
	struct reference reference;
	uint8_t type;

	while (offset < size) {
		memset(&reference, 0, sizeof(reference));
		reference.target = extent->bytenr;
		type = data[offset++];
		reference.type = type;
		if (type == BT_TREE_BLOCK_REF || type == BT_SHARED_BLOCK_REF) {
			if (size - offset < sizeof(value)) {
				return fail(state, "truncated inline tree reference at %llu",
				    (unsigned long long)extent->bytenr);
			}
			memcpy(&value, data + offset, sizeof(value));
			offset += sizeof(value);
			reference.count = 1;
			if (type == BT_TREE_BLOCK_REF) {
				reference.root = bt_u64(value);
				state->audit->tree_refs++;
			} else {
				reference.parent = bt_u64(value);
				state->audit->shared_block_refs++;
			}
		} else if (type == BT_EXTENT_DATA_REF) {
			if (size - offset < sizeof(*data_ref)) {
				return fail(state, "truncated inline data reference at %llu",
				    (unsigned long long)extent->bytenr);
			}
			data_ref = (const void *)(data + offset);
			offset += sizeof(*data_ref);
			reference.root = bt_u64(data_ref->root);
			reference.inode = bt_u64(data_ref->objectid);
			reference.offset = bt_u64(data_ref->offset);
			reference.count = bt_u32(data_ref->count);
			state->audit->data_refs++;
		} else if (type == BT_SHARED_DATA_REF) {
			if (size - offset < sizeof(value) + sizeof(*shared)) {
				return fail(state, "truncated inline shared data reference at %llu",
				    (unsigned long long)extent->bytenr);
			}
			memcpy(&value, data + offset, sizeof(value));
			shared = (const void *)(data + offset + sizeof(value));
			offset += sizeof(value) + sizeof(*shared);
			reference.parent = bt_u64(value);
			reference.count = bt_u32(shared->count);
			state->audit->shared_data_refs++;
		} else {
			return fail(state, "unknown inline reference %u at %llu", type,
			    (unsigned long long)extent->bytenr);
		}
		if (reference.count == 0) {
			return fail(state, "zero-count reference at %llu",
			    (unsigned long long)extent->bytenr);
		}
		extent->counted += reference.count;
		add_reference(&state->actual, reference);
	}
	return 0;
}

static int
load_extents(struct audit_state *state)
{
	const struct btrfs_fs *fs = state->fs;
	const struct bt_disk_extent_item *item;
	const struct bt_disk_data_ref *data_ref;
	const struct bt_disk_shared_data_ref *shared;
	struct extent_record *extent = NULL;
	struct reference reference;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	struct bt_key first = { 0 };
	size_t offset;
	enum btrfs_result error;

	if (bt_find_root(fs, BT_EXTENT_TREE, &root) != BTRFS_OK) {
		return fail(state, "extent tree root missing");
	}
	bt_cursor_init(&cursor, fs, root);
	error = bt_cursor_seek(&cursor, first, 0);
	while (error == BTRFS_OK && !state->failed) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.type == BT_EXTENT_ITEM || record.key.type == BT_METADATA_ITEM) {
			if (record.size < sizeof(*item)) {
				fail(state, "short extent item %llu",
				    (unsigned long long)record.key.objectid);
				break;
			}
			item = (const void *)record.data;
			state->extents = grow(state->extents, &state->extent_capacity,
			    state->extent_count, sizeof(*state->extents));
			extent = &state->extents[state->extent_count++];
			memset(extent, 0, sizeof(*extent));
			extent->bytenr = record.key.objectid;
			extent->length = record.key.type == BT_METADATA_ITEM ? fs->info.node_size
									     : record.key.offset;
			extent->refs = bt_u64(item->refs);
			extent->flags = bt_u64(item->flags);
			offset = sizeof(*item);
			if (record.key.type == BT_EXTENT_ITEM &&
			    (extent->flags & BT_EXTENT_FLAG_TREE)) {
				offset += sizeof(struct bt_disk_tree_block_info);
			}
			if (offset > record.size) {
				fail(state, "short tree extent item %llu",
				    (unsigned long long)extent->bytenr);
				break;
			}
			if (extent->flags & BT_EXTENT_FLAG_DATA) {
				state->audit->data_extents++;
			} else if (extent->flags & BT_EXTENT_FLAG_FULL_BACKREF) {
				state->audit->full_backref_blocks++;
			}
			(void)load_inline(state, extent, record.data, record.size, offset);
		} else if (record.key.type == BT_TREE_BLOCK_REF ||
		    record.key.type == BT_SHARED_BLOCK_REF ||
		    record.key.type == BT_EXTENT_DATA_REF ||
		    record.key.type == BT_SHARED_DATA_REF) {
			if (extent == NULL || extent->bytenr != record.key.objectid) {
				fail(state, "keyed reference without extent at %llu",
				    (unsigned long long)record.key.objectid);
				break;
			}
			memset(&reference, 0, sizeof(reference));
			reference.target = extent->bytenr;
			reference.type = record.key.type;
			reference.count = 1;
			if (record.key.type == BT_TREE_BLOCK_REF) {
				reference.root = record.key.offset;
				state->audit->tree_refs++;
			} else if (record.key.type == BT_SHARED_BLOCK_REF) {
				reference.parent = record.key.offset;
				state->audit->shared_block_refs++;
			} else if (record.key.type == BT_EXTENT_DATA_REF) {
				if (record.size != sizeof(*data_ref)) {
					fail(state, "keyed data reference size at %llu",
					    (unsigned long long)extent->bytenr);
					break;
				}
				data_ref = (const void *)record.data;
				reference.root = bt_u64(data_ref->root);
				reference.inode = bt_u64(data_ref->objectid);
				reference.offset = bt_u64(data_ref->offset);
				reference.count = bt_u32(data_ref->count);
				state->audit->data_refs++;
			} else {
				if (record.size != sizeof(*shared)) {
					fail(state, "keyed shared data reference size at %llu",
					    (unsigned long long)extent->bytenr);
					break;
				}
				shared = (const void *)record.data;
				reference.parent = record.key.offset;
				reference.count = bt_u32(shared->count);
				state->audit->shared_data_refs++;
			}
			state->audit->keyed_refs++;
			extent->counted += reference.count;
			add_reference(&state->actual, reference);
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (!state->failed && error != BTRFS_NOT_FOUND) {
		return fail(state, "extent tree walk: %s", btrfs_result_string(error));
	}
	return state->failed ? -1 : 0;
}

static void
expect_tree(struct audit_state *state, uint64_t child, uint64_t parent, uint64_t owner)
{
	struct extent_record *extent = parent == 0 ? NULL : find_extent(state, parent);
	struct reference reference;

	memset(&reference, 0, sizeof(reference));
	reference.target = child;
	reference.count = 1;
	if (extent != NULL && (extent->flags & BT_EXTENT_FLAG_FULL_BACKREF)) {
		reference.type = BT_SHARED_BLOCK_REF;
		reference.parent = parent;
	} else {
		reference.type = BT_TREE_BLOCK_REF;
		reference.root = owner;
	}
	add_reference(&state->expected, reference);
}

static void
expect_data(
    struct audit_state *state, uint64_t leaf, uint64_t owner, const struct bt_record *record)
{
	const struct bt_disk_extent *file = (const void *)record->data;
	struct extent_record *parent = find_extent(state, leaf);
	struct extent_record *extent;
	struct reference reference;

	if (record->size < sizeof(*file) ||
	    (file->header.type != BT_EXTENT_REGULAR && file->header.type != BT_EXTENT_PREALLOC)) {
		return;
	}
	if (bt_u64(file->disk_bytenr) == 0) {
		return;
	}
	extent = find_extent(state, bt_u64(file->disk_bytenr));
	if (extent == NULL || !(extent->flags & BT_EXTENT_FLAG_DATA) ||
	    extent->length != bt_u64(file->disk_bytes)) {
		fail(state, "file extent %llu:%llu names missing data extent %llu",
		    (unsigned long long)record->key.objectid,
		    (unsigned long long)record->key.offset,
		    (unsigned long long)bt_u64(file->disk_bytenr));
		return;
	}
	extent->matched = 1;
	memset(&reference, 0, sizeof(reference));
	reference.target = extent->bytenr;
	reference.count = 1;
	if (parent != NULL && (parent->flags & BT_EXTENT_FLAG_FULL_BACKREF)) {
		reference.type = BT_SHARED_DATA_REF;
		reference.parent = leaf;
	} else {
		reference.type = BT_EXTENT_DATA_REF;
		reference.root = owner;
		reference.inode = record->key.objectid;
		reference.offset = record->key.offset - bt_u64(file->offset);
	}
	add_reference(&state->expected, reference);
}

static int
walk(struct audit_state *state, struct bt_root root, uint64_t parent, uint64_t owner)
{
	const struct btrfs_fs *fs = state->fs;
	const struct bt_disk_header *header;
	const struct bt_disk_item *items;
	const struct bt_disk_pointer *pointers;
	struct extent_record *extent;
	struct bt_record record;
	struct bt_root child;
	uint8_t *node;
	uint32_t count;
	uint32_t i;
	int result = 0;

	expect_tree(state, root.address, parent, owner);
	if (!visit(state, root.address)) {
		return 0;
	}
	extent = find_extent(state, root.address);
	if (extent == NULL || !(extent->flags & BT_EXTENT_FLAG_TREE) ||
	    extent->length != fs->info.node_size) {
		return fail(state, "tree block %llu of tree %llu has no extent item",
		    (unsigned long long)root.address, (unsigned long long)root.owner);
	}
	extent->matched = 1;
	state->audit->blocks++;
	node = malloc(fs->info.node_size);
	if (node == NULL) {
		abort();
	}
	if (bt_tree_read(fs, root, node) != BTRFS_OK) {
		free(node);
		return fail(state, "tree block %llu unreadable", (unsigned long long)root.address);
	}
	header = (const void *)node;
	items = (const void *)(header + 1);
	pointers = (const void *)(header + 1);
	count = bt_u32(header->count);
	for (i = 0; i < count && result == 0; i++) {
		if (root.level == 0) {
			if (items[i].key.type == BT_EXTENT_DATA) {
				record.key = bt_key_decode(&items[i].key);
				record.data =
				    (const uint8_t *)(header + 1) + bt_u32(items[i].offset);
				record.size = bt_u32(items[i].size);
				expect_data(state, root.address, bt_u64(header->owner), &record);
				result = state->failed ? -1 : 0;
			}
		} else {
			child = root;
			child.address = bt_u64(pointers[i].bytenr);
			child.generation = bt_u64(pointers[i].generation);
			child.level = (uint8_t)(root.level - 1);
			result = walk(state, child, root.address, bt_u64(header->owner));
		}
	}
	free(node);
	return result;
}

static int
walk_roots(struct audit_state *state)
{
	const struct btrfs_fs *fs = state->fs;
	const struct bt_disk_root *item;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	struct bt_key first = { 0 };
	enum btrfs_result error;

	if (walk(state, fs->root_tree, 0, BT_ROOT_TREE) != 0 ||
	    walk(state, fs->chunk_tree, 0, BT_CHUNK_TREE) != 0) {
		return -1;
	}
	state->audit->trees = 2;
	bt_cursor_init(&cursor, fs, fs->root_tree);
	error = bt_cursor_seek(&cursor, first, 0);
	while (error == BTRFS_OK && !state->failed) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.type == BT_ROOT_ITEM) {
			if (record.size < sizeof(*item)) {
				fail(state, "short root item %llu",
				    (unsigned long long)record.key.objectid);
				break;
			}
			item = (const void *)record.data;
			root.address = bt_u64(item->bytenr);
			root.generation = bt_u64(item->generation);
			root.level = item->level;
			root.owner = record.key.objectid;
			state->audit->trees++;
			if (walk(state, root, 0, record.key.objectid) != 0) {
				break;
			}
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (!state->failed && error != BTRFS_NOT_FOUND) {
		return fail(state, "root tree walk: %s", btrfs_result_string(error));
	}
	return state->failed ? -1 : 0;
}

static int
compare_references(const void *left, const void *right)
{
	const struct reference *a = left;
	const struct reference *b = right;
	const uint64_t keys_a[] = { a->target, a->type, a->parent, a->root, a->inode, a->offset };
	const uint64_t keys_b[] = { b->target, b->type, b->parent, b->root, b->inode, b->offset };
	size_t i;

	for (i = 0; i < sizeof(keys_a) / sizeof(keys_a[0]); i++) {
		if (keys_a[i] != keys_b[i]) {
			return keys_a[i] < keys_b[i] ? -1 : 1;
		}
	}
	return 0;
}

static void
aggregate(struct reference_list *list)
{
	size_t out = 0;
	size_t i;

	qsort(list->items, list->count, sizeof(*list->items), compare_references);
	for (i = 0; i < list->count; i++) {
		if (out != 0 && compare_references(&list->items[out - 1], &list->items[i]) == 0) {
			list->items[out - 1].count += list->items[i].count;
		} else {
			list->items[out++] = list->items[i];
		}
	}
	list->count = out;
}

static int
describe(struct audit_state *state, const char *what, const struct reference *reference)
{
	return fail(state,
	    "%s reference: extent %llu type %u parent %llu root %llu inode %llu offset %llu "
	    "count %llu",
	    what, (unsigned long long)reference->target, reference->type,
	    (unsigned long long)reference->parent, (unsigned long long)reference->root,
	    (unsigned long long)reference->inode, (unsigned long long)reference->offset,
	    (unsigned long long)reference->count);
}

int
reference_audit(const struct btrfs_fs *fs, struct reference_audit *audit)
{
	struct audit_state state;
	size_t i;
	size_t j;
	int result;

	memset(audit, 0, sizeof(*audit));
	memset(&state, 0, sizeof(state));
	state.fs = fs;
	state.audit = audit;
	result = load_extents(&state);
	for (i = 0; result == 0 && i < state.extent_count; i++) {
		if (state.extents[i].counted != state.extents[i].refs) {
			result = fail(&state, "extent %llu records %llu refs but lists %llu",
			    (unsigned long long)state.extents[i].bytenr,
			    (unsigned long long)state.extents[i].refs,
			    (unsigned long long)state.extents[i].counted);
		}
	}
	if (result == 0) {
		result = walk_roots(&state);
	}
	for (i = 0; result == 0 && i < state.extent_count; i++) {
		if (!state.extents[i].matched) {
			result = fail(&state, "unreferenced extent %llu",
			    (unsigned long long)state.extents[i].bytenr);
		}
	}
	if (result == 0) {
		aggregate(&state.expected);
		aggregate(&state.actual);
		for (i = j = 0; result == 0 && (i < state.expected.count || j < state.actual.count);
		    i++, j++) {
			if (i == state.expected.count) {
				result = describe(&state, "unexpected", &state.actual.items[j]);
			} else if (j == state.actual.count) {
				result = describe(&state, "missing", &state.expected.items[i]);
			} else if (compare_references(
				       &state.expected.items[i], &state.actual.items[j]) != 0 ||
			    state.expected.items[i].count != state.actual.items[j].count) {
				result = compare_references(
					     &state.expected.items[i], &state.actual.items[j]) <= 0
				    ? describe(
					  &state, "missing or miscounted", &state.expected.items[i])
				    : describe(&state, "unexpected", &state.actual.items[j]);
			}
		}
	}
	free(state.expected.items);
	free(state.actual.items);
	free(state.extents);
	free(state.visited);
	return result;
}
