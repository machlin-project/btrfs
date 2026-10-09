/* SPDX-License-Identifier: BSD-3-Clause */
#include "qgroup_audit.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A backreference: a root, or a parent block (shared and implied references). */
struct qa_ref {
	uint64_t bytenr;
	uint64_t root;
	uint64_t parent;
};

struct qa_extent {
	uint64_t bytenr;
	uint64_t bytes;
	uint64_t generation;
	/* A data extent's owner reference, under simple quotas. */
	uint64_t owner;
	uint8_t level;
	int tree;
};

struct qa_group {
	uint64_t id;
	uint64_t referenced;
	uint64_t exclusive;
	uint64_t stored_referenced;
	uint64_t stored_referenced_compressed;
	uint64_t stored_exclusive;
	uint64_t stored_exclusive_compressed;
	uint64_t seen;
	uint64_t visit;
	uint64_t count;
	int info;
};

struct qa_relation {
	uint64_t member;
	uint64_t parent;
};

struct qa_state {
	const struct btrfs_fs *fs;
	struct qgroup_audit *audit;
	struct qa_ref *refs;
	size_t ref_count;
	size_t ref_capacity;
	struct qa_extent *extents;
	size_t extent_count;
	size_t extent_capacity;
	struct qa_group *groups;
	size_t group_count;
	size_t group_capacity;
	struct qa_relation *relations;
	size_t relation_count;
	size_t relation_capacity;
	uint64_t seen;
	uint64_t visit;
	/* A running rescan has counted the extents below this bytenr. */
	uint64_t progress;
	/* Simple quotas count each extent from enable_gen on for its owner. */
	uint64_t enable_gen;
	int simple;
	int failed;
};

static int
qa_fail(struct qa_state *state, const char *format, ...)
{
	va_list arguments;

	if (!state->failed) {
		va_start(arguments, format);
		vsnprintf(state->audit->failure, sizeof(state->audit->failure), format, arguments);
		va_end(arguments);
	}
	state->failed = 1;
	return -1;
}

static void *
qa_grow(void *items, size_t *capacity, size_t count, size_t size)
{
	void *grown;

	if (count < *capacity) {
		return items;
	}
	*capacity = *capacity == 0 ? 64 : *capacity * 2;
	grown = realloc(items, *capacity * size);
	if (grown == NULL) {
		abort();
	}
	return grown;
}

static void
qa_add_ref(struct qa_state *state, uint64_t bytenr, uint64_t root, uint64_t parent)
{
	state->refs =
	    qa_grow(state->refs, &state->ref_capacity, state->ref_count, sizeof(*state->refs));
	state->refs[state->ref_count++] = (struct qa_ref){ bytenr, root, parent };
}

static int
qa_ref_order(const void *left, const void *right)
{
	const struct qa_ref *a = left;
	const struct qa_ref *b = right;

	return a->bytenr != b->bytenr ? (a->bytenr < b->bytenr ? -1 : 1) : 0;
}

static struct qa_group *
qa_group(struct qa_state *state, uint64_t id, int create)
{
	size_t i;

	for (i = 0; i < state->group_count; i++) {
		if (state->groups[i].id == id) {
			return &state->groups[i];
		}
	}
	if (!create) {
		return NULL;
	}
	state->groups = qa_grow(
	    state->groups, &state->group_capacity, state->group_count, sizeof(*state->groups));
	memset(&state->groups[state->group_count], 0, sizeof(*state->groups));
	state->groups[state->group_count].id = id;
	return &state->groups[state->group_count++];
}

/* Status, qgroup items and member-to-parent relations of the quota tree. */
static int
qa_load(struct qa_state *state, struct bt_root quota)
{
	const struct bt_disk_qgroup_status *status;
	const struct bt_disk_qgroup_info *info;
	struct qa_group *group;
	struct bt_cursor cursor;
	struct bt_record record;
	enum btrfs_result error;
	int found = 0;

	bt_cursor_init(&cursor, state->fs, quota);
	error = bt_cursor_seek(&cursor, (struct bt_key){ 0 }, 0);
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.type == BT_QGROUP_STATUS && record.size >= sizeof(*status)) {
			status = (const void *)record.data;
			found = 1;
			/* A running rescan's counts cover the extents below its
			 * progress exactly, whatever the inconsistent flag says. */
			if ((bt_u64(status->flags) & BT_QGROUP_STATUS_RESCAN) != 0) {
				state->progress = bt_u64(status->rescan);
				state->audit->rescanning = 1;
			} else if ((bt_u64(status->flags) & BT_QGROUP_STATUS_INCONSISTENT) != 0) {
				state->audit->skipped = 1;
			}
			if ((bt_u64(status->flags) & BT_QGROUP_STATUS_SIMPLE) != 0 &&
			    record.size == sizeof(struct bt_disk_qgroup_status_simple)) {
				state->simple = 1;
				state->enable_gen =
				    bt_u64(((const struct bt_disk_qgroup_status_simple *)status)
					    ->enable_gen);
			} else if (bt_u64(status->generation) != state->fs->info.generation) {
				state->audit->skipped = 1;
			}
		} else if (record.key.type == BT_QGROUP_INFO && record.size == sizeof(*info)) {
			info = (const void *)record.data;
			group = qa_group(state, record.key.offset, 1);
			group->info = 1;
			group->stored_referenced = bt_u64(info->referenced);
			group->stored_referenced_compressed = bt_u64(info->referenced_compressed);
			group->stored_exclusive = bt_u64(info->exclusive);
			group->stored_exclusive_compressed = bt_u64(info->exclusive_compressed);
		} else if (record.key.type == BT_QGROUP_LIMIT) {
			(void)qa_group(state, record.key.offset, 1);
		} else if (record.key.type == BT_QGROUP_RELATION &&
		    (record.key.objectid >> BT_QGROUP_LEVEL_SHIFT) <
			(record.key.offset >> BT_QGROUP_LEVEL_SHIFT)) {
			state->relations = qa_grow(state->relations, &state->relation_capacity,
			    state->relation_count, sizeof(*state->relations));
			state->relations[state->relation_count++] =
			    (struct qa_relation){ record.key.objectid, record.key.offset };
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (error != BTRFS_NOT_FOUND) {
		return qa_fail(state, "quota tree walk: %s", btrfs_result_string(error));
	}
	return found ? 0 : qa_fail(state, "quota tree without a status item");
}

/* Every extent item and its inline and keyed references. */
static int
qa_scan(struct qa_state *state)
{
	const struct bt_disk_extent_item *item;
	const struct bt_disk_data_ref *data;
	const uint8_t *refs;
	struct qa_extent *extent = NULL;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root extents;
	enum btrfs_result error;
	uint64_t offset;
	size_t position;
	size_t start;
	uint8_t type;

	if (bt_find_root(state->fs, BT_EXTENT_TREE, &extents) != BTRFS_OK) {
		return qa_fail(state, "no extent tree");
	}
	bt_cursor_init(&cursor, state->fs, extents);
	error = bt_cursor_seek(&cursor, (struct bt_key){ 0 }, 0);
	while (error == BTRFS_OK && !state->failed) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.type == BT_EXTENT_ITEM || record.key.type == BT_METADATA_ITEM) {
			item = (const void *)record.data;
			if (record.size < sizeof(*item)) {
				qa_fail(state, "short extent item %llu",
				    (unsigned long long)record.key.objectid);
				break;
			}
			state->extents = qa_grow(state->extents, &state->extent_capacity,
			    state->extent_count, sizeof(*state->extents));
			extent = &state->extents[state->extent_count++];
			memset(extent, 0, sizeof(*extent));
			extent->bytenr = record.key.objectid;
			extent->generation = bt_u64(item->generation);
			extent->tree = (bt_u64(item->flags) & BT_EXTENT_FLAG_TREE) != 0;
			start = sizeof(*item);
			if (record.key.type == BT_METADATA_ITEM) {
				extent->bytes = state->fs->info.node_size;
				extent->level = (uint8_t)record.key.offset;
			} else {
				extent->bytes = record.key.offset;
				if (extent->tree) {
					start += sizeof(struct bt_disk_tree_block_info);
					extent->level = record.data[sizeof(*item) +
					    offsetof(struct bt_disk_tree_block_info, level)];
				}
			}
			refs = record.data;
			for (position = start; position < record.size && !state->failed;) {
				type = refs[position];
				if (record.size - position < 1 + sizeof(struct bt_le64)) {
					qa_fail(state, "short reference in extent %llu",
					    (unsigned long long)extent->bytenr);
					break;
				}
				offset = bt_u64(*(const struct bt_le64 *)(refs + position + 1));
				if (type == BT_EXTENT_OWNER_REF && !extent->tree &&
				    position == start) {
					extent->owner = offset;
					position += sizeof(struct bt_disk_inline_ref);
				} else if (type == BT_TREE_BLOCK_REF) {
					qa_add_ref(state, extent->bytenr, offset, 0);
					position += 1 + sizeof(struct bt_le64);
				} else if (type == BT_SHARED_BLOCK_REF) {
					qa_add_ref(state, extent->bytenr, 0, offset);
					position += 1 + sizeof(struct bt_le64);
				} else if (type == BT_EXTENT_DATA_REF) {
					data = (const void *)(refs + position + 1);
					qa_add_ref(state, extent->bytenr, bt_u64(data->root), 0);
					position += 1 + sizeof(*data);
				} else if (type == BT_SHARED_DATA_REF) {
					qa_add_ref(state, extent->bytenr, 0, offset);
					position += 1 + sizeof(struct bt_le64) +
					    sizeof(struct bt_disk_shared_data_ref);
				} else {
					qa_fail(state, "extent %llu has reference type %u",
					    (unsigned long long)extent->bytenr, type);
				}
			}
		} else if (extent != NULL && record.key.objectid == extent->bytenr) {
			if (record.key.type == BT_TREE_BLOCK_REF) {
				qa_add_ref(state, extent->bytenr, record.key.offset, 0);
			} else if (record.key.type == BT_SHARED_BLOCK_REF ||
			    record.key.type == BT_SHARED_DATA_REF) {
				qa_add_ref(state, extent->bytenr, 0, record.key.offset);
			} else if (record.key.type == BT_EXTENT_DATA_REF &&
			    record.size == sizeof(*data)) {
				data = (const void *)record.data;
				qa_add_ref(state, extent->bytenr, bt_u64(data->root), 0);
			}
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (!state->failed && error != BTRFS_NOT_FOUND) {
		return qa_fail(state, "extent tree walk: %s", btrfs_result_string(error));
	}
	if (!state->failed) {
		qsort(state->refs, state->ref_count, sizeof(*state->refs), qa_ref_order);
	}
	return state->failed ? -1 : 0;
}

/* The first reference of bytenr in the sorted list, or ref_count. */
static size_t
qa_first(const struct qa_state *state, uint64_t bytenr)
{
	size_t low = 0;
	size_t high = state->ref_count;
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (state->refs[middle].bytenr < bytenr) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return low;
}

/* resolve_one_root: the root of a block's first reference, through parents. */
static uint64_t
qa_one_root(const struct qa_state *state, uint64_t bytenr, unsigned depth)
{
	size_t first = qa_first(state, bytenr);

	if (first == state->ref_count || state->refs[first].bytenr != bytenr ||
	    depth >= BT_MAX_LEVEL) {
		return 0;
	}
	if (state->refs[first].root != 0) {
		return state->refs[first].root;
	}
	return qa_one_root(state, state->refs[first].parent, depth + 1);
}

/* travel_tree: an implied reference from top to every block and data extent
 * below it. */
static int
qa_travel(struct qa_state *state, struct bt_root block, uint64_t top, struct qa_ref **implied,
    size_t *count, size_t *capacity)
{
	const struct bt_disk_header *header;
	const struct bt_disk_item *items;
	const struct bt_disk_pointer *pointers;
	const struct bt_disk_extent *file;
	struct bt_root child;
	uint8_t *node;
	uint32_t i;
	int result = 0;

	if (block.address != top) {
		*implied = qa_grow(*implied, capacity, *count, sizeof(**implied));
		(*implied)[(*count)++] = (struct qa_ref){ block.address, 0, top };
	}
	node = malloc(state->fs->info.node_size);
	if (node == NULL) {
		abort();
	}
	if (bt_tree_read(state->fs, block, node) != BTRFS_OK) {
		free(node);
		return qa_fail(
		    state, "tree block %llu unreadable", (unsigned long long)block.address);
	}
	header = (const void *)node;
	items = (const void *)(header + 1);
	pointers = (const void *)(header + 1);
	for (i = 0; i < bt_u32(header->count) && result == 0; i++) {
		if (block.level == 0) {
			if (items[i].key.type != BT_EXTENT_DATA) {
				continue;
			}
			file =
			    (const void *)((const uint8_t *)(header + 1) + bt_u32(items[i].offset));
			if (bt_u32(items[i].size) < sizeof(*file) ||
			    file->header.type == BT_EXTENT_INLINE ||
			    bt_u64(file->disk_bytenr) == 0) {
				continue;
			}
			*implied = qa_grow(*implied, capacity, *count, sizeof(**implied));
			(*implied)[(*count)++] =
			    (struct qa_ref){ bt_u64(file->disk_bytenr), 0, top };
		} else {
			child = block;
			child.address = bt_u64(pointers[i].bytenr);
			child.generation = bt_u64(pointers[i].generation);
			child.level = (uint8_t)(block.level - 1);
			result = qa_travel(state, child, top, implied, count, capacity);
		}
	}
	free(node);
	return result;
}

/* map_implied_refs over every interior block of a subvolume tree. */
static int
qa_implied(struct qa_state *state)
{
	struct qa_ref *implied = NULL;
	size_t count = 0;
	size_t capacity = 0;
	size_t i;
	uint64_t root;
	int result = 0;

	for (i = 0; i < state->extent_count && result == 0; i++) {
		if (!state->extents[i].tree || state->extents[i].level == 0) {
			continue;
		}
		root = qa_one_root(state, state->extents[i].bytenr, 0);
		if (!bt_file_tree(root)) {
			continue;
		}
		result = qa_travel(state,
		    (struct bt_root){ state->extents[i].bytenr, state->extents[i].generation, root,
			state->extents[i].level },
		    state->extents[i].bytenr, &implied, &count, &capacity);
	}
	for (i = 0; result == 0 && i < count; i++) {
		qa_add_ref(state, implied[i].bytenr, implied[i].root, implied[i].parent);
	}
	state->audit->implied = count;
	free(implied);
	if (result == 0) {
		qsort(state->refs, state->ref_count, sizeof(*state->refs), qa_ref_order);
	}
	return result;
}

static void
qa_add_root(uint64_t **roots, size_t *count, size_t *capacity, uint64_t root)
{
	size_t i;

	for (i = 0; i < *count; i++) {
		if ((*roots)[i] == root) {
			return;
		}
	}
	*roots = qa_grow(*roots, capacity, *count, sizeof(**roots));
	(*roots)[(*count)++] = root;
}

/* find_parent_roots: the roots of bytenr's references, through parents. */
static int
qa_roots(struct qa_state *state, uint64_t bytenr, unsigned depth, uint64_t **roots, size_t *count,
    size_t *capacity)
{
	size_t i;

	if (depth > BT_MAX_LEVEL + 1) {
		return qa_fail(
		    state, "reference chain of %llu too deep", (unsigned long long)bytenr);
	}
	for (i = qa_first(state, bytenr); i < state->ref_count && state->refs[i].bytenr == bytenr;
	    i++) {
		if (state->refs[i].root != 0) {
			if (bt_file_tree(state->refs[i].root)) {
				qa_add_root(roots, count, capacity, state->refs[i].root);
			}
		} else if (qa_roots(state, state->refs[i].parent, depth + 1, roots, count,
			       capacity) != 0) {
			return -1;
		}
	}
	return 0;
}

/* account_one_extent: each root's qgroup and every qgroup above it count once
 * per root; a qgroup reached by every root holds the bytes exclusively. */
static void
qa_account(struct qa_state *state, const uint64_t *roots, size_t count, uint64_t bytes)
{
	struct qa_group **reached = NULL;
	struct qa_group *group;
	struct qa_group *parent;
	size_t reached_count = 0;
	size_t reached_capacity = 0;
	size_t start;
	size_t i;
	size_t j;
	size_t r;

	state->seen++;
	for (i = 0; i < count; i++) {
		group = qa_group(state, roots[i], 0);
		if (group == NULL) {
			continue;
		}
		/* This root's qgroup and its ancestors, each once. */
		state->visit++;
		start = reached_count;
		reached = qa_grow(reached, &reached_capacity, reached_count, sizeof(*reached));
		reached[reached_count++] = group;
		group->visit = state->visit;
		for (j = start; j < reached_count; j++) {
			if (reached[j]->seen != state->seen) {
				reached[j]->seen = state->seen;
				reached[j]->count = 0;
			}
			reached[j]->count++;
			for (r = 0; r < state->relation_count; r++) {
				parent = state->relations[r].member == reached[j]->id
				    ? qa_group(state, state->relations[r].parent, 0)
				    : NULL;
				if (parent == NULL || parent->visit == state->visit) {
					continue;
				}
				parent->visit = state->visit;
				reached = qa_grow(
				    reached, &reached_capacity, reached_count, sizeof(*reached));
				reached[reached_count++] = parent;
			}
		}
	}
	/* Each distinct qgroup once: its count is the roots that reach it. */
	for (i = 0; i < reached_count; i++) {
		group = reached[i];
		if (group->seen != state->seen) {
			continue;
		}
		group->referenced += bytes;
		if (group->count == count) {
			group->exclusive += bytes;
		}
		group->seen = 0;
	}
	free(reached);
}

/* Simple quotas, as btrfs check's simple_quota_account_extent counts them:
 * each extent from enable_gen on counts for one root, a data extent's owner
 * reference or a tree block's header owner, when that is a subvolume. */
static int
qa_simple(struct qa_state *state)
{
	const struct bt_disk_header *header;
	const struct qa_extent *extent;
	uint8_t *block;
	uint64_t owner;
	size_t chunk;
	size_t i;

	block = malloc(state->fs->info.node_size);
	if (block == NULL) {
		return qa_fail(state, "no memory");
	}
	header = (const void *)block;
	for (i = 0; !state->failed && i < state->extent_count; i++) {
		extent = &state->extents[i];
		if (extent->generation < state->enable_gen) {
			continue;
		}
		owner = extent->owner;
		/* Linux names the owner of every data extent it allocates while
		 * simple quotas are on. */
		if (!extent->tree && owner == 0) {
			qa_fail(state, "data extent %llu of generation %llu has no owner",
			    (unsigned long long)extent->bytenr,
			    (unsigned long long)extent->generation);
			break;
		}
		if (extent->tree) {
			chunk = bt_chunk_containing(state->fs, extent->bytenr);
			if (chunk == state->fs->chunk_count ||
			    (state->fs->chunks[chunk].type & BT_BLOCK_SYSTEM) != 0) {
				continue;
			}
			if (bt_tree_read(state->fs,
				(struct bt_root){ extent->bytenr, extent->generation, BT_OWNER_ANY,
				    extent->level },
				block) != BTRFS_OK) {
				qa_fail(state, "tree block %llu unreadable",
				    (unsigned long long)extent->bytenr);
				break;
			}
			owner = bt_u64(header->owner);
		}
		if (bt_file_tree(owner)) {
			qa_account(state, &owner, 1, extent->bytes);
		}
	}
	free(block);
	return state->failed ? -1 : 0;
}

int
qgroup_audit(const struct btrfs_fs *fs, struct qgroup_audit *audit)
{
	struct qa_state state;
	struct bt_root quota;
	struct qa_group *group;
	uint64_t *roots = NULL;
	size_t root_count;
	size_t root_capacity = 0;
	size_t i;
	enum btrfs_result error;
	int result;

	memset(audit, 0, sizeof(*audit));
	memset(&state, 0, sizeof(state));
	state.fs = fs;
	state.audit = audit;
	state.progress = UINT64_MAX;
	error = bt_find_root(fs, BT_QUOTA_TREE, &quota);
	if (error == BTRFS_NOT_FOUND) {
		return 0;
	}
	if (error != BTRFS_OK) {
		return qa_fail(&state, "quota root: %s", btrfs_result_string(error));
	}
	audit->quotas = 1;
	result = qa_load(&state, quota);
	if (result == 0 && !audit->skipped) {
		result = qa_scan(&state);
	}
	if (result == 0 && !audit->skipped && state.simple) {
		result = qa_simple(&state);
	}
	if (result == 0 && !audit->skipped && !state.simple) {
		result = qa_implied(&state);
	}
	for (i = 0; result == 0 && !audit->skipped && !state.simple && i < state.extent_count;
	    i++) {
		if (state.extents[i].bytenr >= state.progress) {
			continue;
		}
		root_count = 0;
		result = qa_roots(
		    &state, state.extents[i].bytenr, 0, &roots, &root_count, &root_capacity);
		if (result == 0 && root_count != 0) {
			qa_account(&state, roots, root_count, state.extents[i].bytes);
		}
	}
	for (i = 0; result == 0 && !audit->skipped && i < state.group_count; i++) {
		group = &state.groups[i];
		/* Simple quotas leave the compressed counts alone. */
		if (group->info &&
		    (group->referenced != group->stored_referenced ||
			group->exclusive != group->stored_exclusive ||
			(!state.simple &&
			    (group->referenced != group->stored_referenced_compressed ||
				group->exclusive != group->stored_exclusive_compressed)))) {
			result = qa_fail(&state,
			    "qgroup %u/%llu stores referenced %llu exclusive %llu, counts %llu and "
			    "%llu",
			    (unsigned)(group->id >> BT_QGROUP_LEVEL_SHIFT),
			    (unsigned long long)(group->id &
				((UINT64_C(1) << BT_QGROUP_LEVEL_SHIFT) - 1)),
			    (unsigned long long)group->stored_referenced,
			    (unsigned long long)group->stored_exclusive,
			    (unsigned long long)group->referenced,
			    (unsigned long long)group->exclusive);
		}
	}
	audit->qgroups = state.group_count;
	audit->extents = state.extent_count;
	free(roots);
	free(state.refs);
	free(state.extents);
	free(state.groups);
	free(state.relations);
	return result;
}
