/* SPDX-License-Identifier: BSD-3-Clause */
/* Linux's full qgroup accounting (fs/btrfs/qgroup.c). The transaction records
 * every extent whose references change. At commit each is accounted from the
 * subvolume trees that reach it in the committed base (old roots) and in the
 * transaction's view (new roots), resolved as btrfs_find_all_roots resolves
 * backreferences, and the counters of those subvolumes' qgroups and of every
 * qgroup above them change as btrfs_qgroup_account_extent changes them. */
#include "qgroup.h"
#include "mutable.h"

/* Qgroups and relations a quota tree may hold: the table is loaded whole. */
#define BT_QGROUP_MAX 65536U
/* Linux searches from offset 0 for a data reference whose offset underflowed
 * (above LLONG_MAX, as the clone ioctl can leave it). */
#define BT_QGROUP_OFFSET_LIMIT (UINT64_C(1) << 63)
#define BT_QGROUP_INITIAL 64U
#define BT_QGROUP_KNOWN_FLAGS                                                                      \
	(BT_QGROUP_STATUS_ON | BT_QGROUP_STATUS_RESCAN | BT_QGROUP_STATUS_INCONSISTENT |           \
	    BT_QGROUP_STATUS_SIMPLE)

struct bt_qgroup {
	uint64_t id;
	uint64_t referenced;
	uint64_t referenced_compressed;
	uint64_t exclusive;
	uint64_t exclusive_compressed;
	uint64_t limit_flags;
	uint64_t max_referenced;
	uint64_t max_exclusive;
	uint64_t reserved_referenced;
	uint64_t reserved_exclusive;
	/* Bytes admitted against limits during this transaction. */
	uint64_t reserved;
	/* Linux's old and new reference counts for the extent being accounted. */
	uint64_t old_count;
	uint64_t new_count;
	/* The root traversal and the extent this qgroup was last counted for. */
	uint64_t visit;
	uint64_t extent;
	int info;
	int limit;
	int dirty;
	int dropped;
};

/* member is in parent: Linux's (member, QGROUP_RELATION, parent) item. */
struct bt_qgroup_relation {
	uint64_t member;
	uint64_t parent;
};

/* Subvolume tree ids, ascending without repeats. */
struct bt_rootset {
	uint64_t *ids;
	size_t count;
	size_t capacity;
};

/* A tree block whose root set is known: its ids in the view's pool. */
struct bt_qgroup_memo {
	uint64_t address;
	size_t first;
	size_t count;
};

/* A subvolume's root item as one view sees it. */
struct bt_qgroup_tree {
	uint64_t id;
	struct bt_root root;
	int found;
	int deleted;
};

/* One backreference of an extent, inline or keyed. */
struct bt_qgroup_ref {
	uint8_t type;
	uint64_t root;
	uint64_t parent;
	uint64_t inode;
	uint64_t offset;
	uint32_t count;
};

struct bt_qgroup_view {
	struct bt_qgroups *qgroups;
	const struct btrfs_fs *fs;
	struct bt_root roots;
	struct bt_root extents;
	/* The transaction's view: a deleted subvolume counts as a root without a
	 * search, as Linux's backref walk treats a root being dropped. */
	int current;
	struct bt_qgroup_memo *memo;
	size_t memo_count;
	size_t memo_capacity;
	uint64_t *pool;
	size_t pool_count;
	size_t pool_capacity;
	struct bt_qgroup_tree *trees;
	size_t tree_count;
	size_t tree_capacity;
};

struct bt_qgroups {
	const struct btrfs_environment *env;
	struct bt_qgroup *groups;
	size_t count;
	size_t capacity;
	struct bt_qgroup_relation *relations;
	size_t relation_count;
	size_t relation_capacity;
	/* The status item as found; generation and flags change at commit. */
	uint8_t status[sizeof(struct bt_disk_qgroup_status) + sizeof(struct bt_le64)];
	size_t status_size;
	uint64_t flags;
	/* Off once quotas are inconsistent (Linux's NO_ACCOUNTING). */
	int accounting;
	struct bt_key *traced;
	size_t traced_count;
	size_t traced_capacity;
	/* Accounting scratch: qgroups reached for one extent, traversal queue. */
	size_t *touched;
	size_t touched_count;
	size_t *queue;
	size_t scratch_capacity;
	uint64_t visit;
	uint64_t extent;
	uint64_t generation;
	/* A snapshot of this transaction: its subvolumes and root nodes. */
	uint64_t snapshot_source;
	uint64_t snapshot_target;
	uint64_t snapshot_root;
	uint64_t snapshot_copy;
};

/* Grows an array of size-byte elements to hold one more than count, up to
 * limit elements. */
static enum btrfs_result
bt_qg_grow(const struct btrfs_environment *env, void **items, size_t *capacity, size_t count,
    size_t size, size_t limit)
{
	void *grown;
	size_t next;

	if (count < *capacity) {
		return BTRFS_OK;
	}
	if (*capacity >= limit) {
		return BTRFS_UNSUPPORTED;
	}
	next = *capacity == 0 ? BT_QGROUP_INITIAL : *capacity * 2;
	next = next > limit ? limit : next;
	grown = env->allocate(env->context, next * size);
	if (grown == NULL) {
		return BTRFS_NO_MEMORY;
	}
	if (*items != NULL) {
		bt_copy(grown, *items, count * size);
		env->release(env->context, *items, *capacity * size);
	}
	*items = grown;
	*capacity = next;
	return BTRFS_OK;
}

static void
bt_qg_free(const struct btrfs_environment *env, void *items, size_t capacity, size_t size)
{
	if (items != NULL) {
		env->release(env->context, items, capacity * size);
	}
}

static enum btrfs_result
bt_rootset_add(const struct btrfs_environment *env, struct bt_rootset *set, uint64_t id)
{
	size_t low = 0;
	size_t high = set->count;
	size_t middle;
	enum btrfs_result error;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (set->ids[middle] < id) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	if (low < set->count && set->ids[low] == id) {
		return BTRFS_OK;
	}
	error = bt_qg_grow(env, (void **)&set->ids, &set->capacity, set->count, sizeof(*set->ids),
	    SIZE_MAX / sizeof(*set->ids));
	if (error != BTRFS_OK) {
		return error;
	}
	bt_move(set->ids + low + 1, set->ids + low, (set->count - low) * sizeof(*set->ids));
	set->ids[low] = id;
	set->count++;
	return BTRFS_OK;
}

static void
bt_rootset_free(const struct btrfs_environment *env, struct bt_rootset *set)
{
	bt_qg_free(env, set->ids, set->capacity, sizeof(*set->ids));
	bt_zero(set, sizeof(*set));
}

/* The qgroup with id, or SIZE_MAX. */
static size_t
bt_qg_find(const struct bt_qgroups *qgroups, uint64_t id)
{
	size_t low = 0;
	size_t high = qgroups->count;
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (qgroups->groups[middle].id < id) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return low < qgroups->count && qgroups->groups[low].id == id ? low : SIZE_MAX;
}

/* The qgroup with id, inserted when absent. */
static enum btrfs_result
bt_qg_insert(struct bt_qgroups *qgroups, uint64_t id, size_t *result)
{
	size_t low = 0;
	size_t high = qgroups->count;
	size_t middle;
	enum btrfs_result error;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (qgroups->groups[middle].id < id) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	if (low == qgroups->count || qgroups->groups[low].id != id) {
		error = bt_qg_grow(qgroups->env, (void **)&qgroups->groups, &qgroups->capacity,
		    qgroups->count, sizeof(*qgroups->groups), BT_QGROUP_MAX);
		if (error != BTRFS_OK) {
			return error;
		}
		bt_move(qgroups->groups + low + 1, qgroups->groups + low,
		    (qgroups->count - low) * sizeof(*qgroups->groups));
		bt_zero(&qgroups->groups[low], sizeof(qgroups->groups[low]));
		qgroups->groups[low].id = id;
		qgroups->count++;
	}
	*result = low;
	return BTRFS_OK;
}

static uint16_t
bt_qg_level(uint64_t id)
{
	return (uint16_t)(id >> BT_QGROUP_LEVEL_SHIFT);
}

/* Quotas become inconsistent and accounting stops, as qgroup_mark_inconsistent. */
static void
bt_qg_inconsistent(struct bt_qgroups *qgroups)
{
	qgroups->flags |= BT_QGROUP_STATUS_INCONSISTENT;
	qgroups->accounting = 0;
}

static enum btrfs_result
bt_qg_record(struct bt_qgroups *qgroups, const struct bt_record *record)
{
	const struct bt_disk_qgroup_status *status;
	const struct bt_disk_qgroup_info *info;
	const struct bt_disk_qgroup_limit *limit;
	struct bt_qgroup *group;
	size_t index;
	enum btrfs_result error;

	if (record->key.type == BT_QGROUP_STATUS) {
		if (record->key.objectid != 0 || record->key.offset != 0 ||
		    record->size < sizeof(*status) || record->size > sizeof(qgroups->status) ||
		    qgroups->status_size != 0) {
			return BTRFS_CORRUPT;
		}
		status = (const void *)record->data;
		qgroups->flags = bt_u64(status->flags);
		bt_copy(qgroups->status, record->data, record->size);
		qgroups->status_size = record->size;
		/* Older versions, simple quotas, a rescan in progress, disabled quotas
		 * and unknown flags need semantics this writer does not have. */
		if (bt_u64(status->version) != BT_QGROUP_STATUS_VERSION ||
		    (qgroups->flags & ~BT_QGROUP_KNOWN_FLAGS) != 0 ||
		    (qgroups->flags & (BT_QGROUP_STATUS_SIMPLE | BT_QGROUP_STATUS_RESCAN)) != 0 ||
		    !(qgroups->flags & BT_QGROUP_STATUS_ON)) {
			return BTRFS_UNSUPPORTED;
		}
		if (bt_u64(status->generation) != qgroups->generation - 1) {
			bt_qg_inconsistent(qgroups);
		}
		return BTRFS_OK;
	}
	if (record->key.type == BT_QGROUP_INFO || record->key.type == BT_QGROUP_LIMIT) {
		if (record->key.objectid != 0 ||
		    record->size !=
			(record->key.type == BT_QGROUP_INFO ? sizeof(*info) : sizeof(*limit))) {
			return BTRFS_CORRUPT;
		}
		error = bt_qg_insert(qgroups, record->key.offset, &index);
		if (error != BTRFS_OK) {
			return error;
		}
		group = &qgroups->groups[index];
		if (record->key.type == BT_QGROUP_INFO) {
			info = (const void *)record->data;
			group->info = 1;
			group->referenced = bt_u64(info->referenced);
			group->referenced_compressed = bt_u64(info->referenced_compressed);
			group->exclusive = bt_u64(info->exclusive);
			group->exclusive_compressed = bt_u64(info->exclusive_compressed);
		} else {
			limit = (const void *)record->data;
			group->limit = 1;
			group->limit_flags = bt_u64(limit->flags);
			group->max_referenced = bt_u64(limit->max_referenced);
			group->max_exclusive = bt_u64(limit->max_exclusive);
			group->reserved_referenced = bt_u64(limit->reserved_referenced);
			group->reserved_exclusive = bt_u64(limit->reserved_exclusive);
		}
		return BTRFS_OK;
	}
	if (record->key.type == BT_QGROUP_RELATION) {
		if (record->size != 0) {
			return BTRFS_CORRUPT;
		}
		/* Each relation is stored in both directions; the member's copy, whose
		 * level is lower, builds the hierarchy. */
		if (bt_qg_level(record->key.objectid) == bt_qg_level(record->key.offset)) {
			return BTRFS_CORRUPT;
		}
		if (bt_qg_level(record->key.objectid) > bt_qg_level(record->key.offset)) {
			return BTRFS_OK;
		}
		error = bt_qg_grow(qgroups->env, (void **)&qgroups->relations,
		    &qgroups->relation_capacity, qgroups->relation_count,
		    sizeof(*qgroups->relations), BT_QGROUP_MAX);
		if (error == BTRFS_OK) {
			qgroups->relations[qgroups->relation_count++] =
			    (struct bt_qgroup_relation){ record->key.objectid, record->key.offset };
		}
		return error;
	}
	return BTRFS_CORRUPT;
}

static enum btrfs_result
bt_qg_load(struct btrfs_transaction *transaction, struct bt_qgroups *qgroups)
{
	struct bt_cursor cursor;
	struct bt_record record;
	size_t i;
	size_t kept = 0;
	enum btrfs_result error;

	bt_cursor_init(&cursor, transaction->base, transaction->quota.root);
	error = bt_cursor_seek(&cursor, (struct bt_key){ 0 }, 0);
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		error = bt_qg_record(qgroups, &record);
		if (error == BTRFS_OK) {
			error = bt_cursor_next(&cursor);
		}
	}
	bt_cursor_fini(&cursor);
	if (error != BTRFS_NOT_FOUND) {
		return error;
	}
	if (qgroups->status_size == 0) {
		return BTRFS_CORRUPT;
	}
	for (i = 0; i < qgroups->count; i++) {
		/* As btrfs_read_qgroup_config: a qgroup lacking an item is inconsistent. */
		if (!qgroups->groups[i].info || !qgroups->groups[i].limit) {
			bt_qg_inconsistent(qgroups);
		}
	}
	/* Relations naming a missing qgroup are ignored, as Linux ignores them. */
	for (i = 0; i < qgroups->relation_count; i++) {
		if (bt_qg_find(qgroups, qgroups->relations[i].member) != SIZE_MAX &&
		    bt_qg_find(qgroups, qgroups->relations[i].parent) != SIZE_MAX) {
			qgroups->relations[kept++] = qgroups->relations[i];
		}
	}
	qgroups->relation_count = kept;
	return BTRFS_OK;
}

static void
bt_qg_view_init(struct bt_qgroup_view *view, struct bt_qgroups *qgroups, const struct btrfs_fs *fs,
    struct bt_root roots, struct bt_root extents, int current)
{
	bt_zero(view, sizeof(*view));
	view->qgroups = qgroups;
	view->fs = fs;
	view->roots = roots;
	view->extents = extents;
	view->current = current;
}

static void
bt_qg_view_fini(struct bt_qgroup_view *view)
{
	const struct btrfs_environment *env = view->qgroups->env;

	bt_qg_free(env, view->memo, view->memo_capacity, sizeof(*view->memo));
	bt_qg_free(env, view->pool, view->pool_capacity, sizeof(*view->pool));
	bt_qg_free(env, view->trees, view->tree_capacity, sizeof(*view->trees));
}

static size_t
bt_qg_hash(uint64_t address, size_t capacity)
{
	address ^= address >> 33;
	address *= UINT64_C(0xff51afd7ed558ccd);
	address ^= address >> 33;
	return (size_t)address & (capacity - 1);
}

static const struct bt_qgroup_memo *
bt_qg_memo_find(const struct bt_qgroup_view *view, uint64_t address)
{
	size_t slot;

	if (view->memo_capacity == 0) {
		return NULL;
	}
	for (slot = bt_qg_hash(address, view->memo_capacity); view->memo[slot].address != 0;
	    slot = (slot + 1) & (view->memo_capacity - 1)) {
		if (view->memo[slot].address == address) {
			return &view->memo[slot];
		}
	}
	return NULL;
}

/* Remembers a block's root set; the table stays at most half full. */
static enum btrfs_result
bt_qg_memo_add(struct bt_qgroup_view *view, uint64_t address, const struct bt_rootset *set)
{
	const struct btrfs_environment *env = view->qgroups->env;
	struct bt_qgroup_memo *old = view->memo;
	size_t old_capacity = view->memo_capacity;
	size_t first = view->pool_count;
	size_t capacity;
	size_t slot;
	size_t i;
	enum btrfs_result error;

	if (2 * (view->memo_count + 1) > view->memo_capacity) {
		capacity =
		    view->memo_capacity == 0 ? 2 * BT_QGROUP_INITIAL : 2 * view->memo_capacity;
		if (capacity > SIZE_MAX / sizeof(*view->memo)) {
			return BTRFS_NO_MEMORY;
		}
		view->memo = env->allocate(env->context, capacity * sizeof(*view->memo));
		if (view->memo == NULL) {
			view->memo = old;
			return BTRFS_NO_MEMORY;
		}
		bt_zero(view->memo, capacity * sizeof(*view->memo));
		view->memo_capacity = capacity;
		for (i = 0; i < old_capacity; i++) {
			if (old[i].address == 0) {
				continue;
			}
			slot = bt_qg_hash(old[i].address, capacity);
			while (view->memo[slot].address != 0) {
				slot = (slot + 1) & (capacity - 1);
			}
			view->memo[slot] = old[i];
		}
		bt_qg_free(env, old, old_capacity, sizeof(*old));
	}
	for (i = 0; i < set->count; i++) {
		error = bt_qg_grow(env, (void **)&view->pool, &view->pool_capacity,
		    view->pool_count, sizeof(*view->pool), SIZE_MAX / sizeof(*view->pool));
		if (error != BTRFS_OK) {
			return error;
		}
		view->pool[view->pool_count++] = set->ids[i];
	}
	slot = bt_qg_hash(address, view->memo_capacity);
	while (view->memo[slot].address != 0) {
		slot = (slot + 1) & (view->memo_capacity - 1);
	}
	view->memo[slot] = (struct bt_qgroup_memo){ address, first, set->count };
	view->memo_count++;
	return BTRFS_OK;
}

/* A subvolume's root as the view's root tree names it. */
static enum btrfs_result
bt_qg_tree(struct bt_qgroup_view *view, uint64_t id, const struct bt_qgroup_tree **result)
{
	const struct bt_disk_root *disk;
	struct bt_qgroup_tree *tree;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = id, .type = BT_ROOT_ITEM, .offset = UINT64_MAX };
	size_t i;
	enum btrfs_result error;

	for (i = 0; i < view->tree_count; i++) {
		if (view->trees[i].id == id) {
			*result = &view->trees[i];
			return BTRFS_OK;
		}
	}
	error = bt_qg_grow(view->qgroups->env, (void **)&view->trees, &view->tree_capacity,
	    view->tree_count, sizeof(*view->trees), SIZE_MAX / sizeof(*view->trees));
	if (error != BTRFS_OK) {
		return error;
	}
	tree = &view->trees[view->tree_count];
	bt_zero(tree, sizeof(*tree));
	tree->id = id;
	bt_cursor_init(&cursor, view->fs, view->roots);
	error = bt_cursor_seek(&cursor, key, 1);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid == id && record.key.type == BT_ROOT_ITEM) {
			if (record.size < sizeof(*disk)) {
				error = BTRFS_CORRUPT;
			} else {
				disk = (const void *)record.data;
				tree->root = (struct bt_root){ bt_u64(disk->bytenr),
					bt_u64(disk->generation), id, disk->level };
				tree->found = 1;
				tree->deleted = bt_u32(disk->refs) == 0;
				error = disk->level < BT_MAX_LEVEL ? BTRFS_OK : BTRFS_CORRUPT;
			}
		}
	}
	bt_cursor_fini(&cursor);
	if (error != BTRFS_OK && error != BTRFS_NOT_FOUND) {
		return error;
	}
	view->tree_count++;
	*result = tree;
	return BTRFS_OK;
}

static enum btrfs_result
bt_qg_add_ref(const struct btrfs_environment *env, struct bt_qgroup_ref **refs, size_t *count,
    size_t *capacity, struct bt_qgroup_ref ref)
{
	enum btrfs_result error;

	error = bt_qg_grow(
	    env, (void **)refs, capacity, *count, sizeof(**refs), SIZE_MAX / sizeof(**refs));
	if (error == BTRFS_OK) {
		(*refs)[(*count)++] = ref;
	}
	return error;
}

/* An inline reference of an extent item, at data[position]. */
static enum btrfs_result
bt_qg_inline_ref(const uint8_t *data, size_t size, size_t *position, struct bt_qgroup_ref *ref)
{
	const struct bt_disk_data_ref *wire;
	const struct bt_le64 *value;
	const struct bt_disk_shared_data_ref *shared;
	size_t length;

	bt_zero(ref, sizeof(*ref));
	ref->type = data[*position];
	switch (ref->type) {
	case BT_TREE_BLOCK_REF:
	case BT_SHARED_BLOCK_REF:
		length = 1 + sizeof(*value);
		break;
	case BT_EXTENT_DATA_REF:
		length = 1 + sizeof(*wire);
		break;
	case BT_SHARED_DATA_REF:
		length = 1 + sizeof(*value) + sizeof(*shared);
		break;
	default:
		return BTRFS_CORRUPT;
	}
	if (length > size - *position) {
		return BTRFS_CORRUPT;
	}
	value = (const void *)(data + *position + 1);
	if (ref->type == BT_TREE_BLOCK_REF) {
		ref->root = bt_u64(*value);
		ref->count = 1;
	} else if (ref->type == BT_SHARED_BLOCK_REF) {
		ref->parent = bt_u64(*value);
		ref->count = 1;
	} else if (ref->type == BT_EXTENT_DATA_REF) {
		wire = (const void *)(data + *position + 1);
		ref->root = bt_u64(wire->root);
		ref->inode = bt_u64(wire->objectid);
		ref->offset = bt_u64(wire->offset);
		ref->count = bt_u32(wire->count);
	} else {
		shared = (const void *)(data + *position + 1 + sizeof(*value));
		ref->parent = bt_u64(*value);
		ref->count = bt_u32(shared->count);
	}
	*position += length;
	return BTRFS_OK;
}

/* The references of the extent item at key, inline then keyed, with the
 * item's generation and, for a tree block, its level. A tree block key with
 * any_level names a block by address alone. *found is zero when the view has
 * no such extent. */
static enum btrfs_result
bt_qg_refs(struct bt_qgroup_view *view, struct bt_key key, int any_level, uint64_t *generation,
    uint8_t *level, struct bt_qgroup_ref **refs, size_t *count, size_t *capacity, int *found)
{
	const struct btrfs_environment *env = view->qgroups->env;
	const struct bt_disk_extent_item *item;
	const struct bt_disk_data_ref *wire;
	const struct bt_disk_shared_data_ref *shared;
	struct bt_qgroup_ref ref;
	struct bt_cursor cursor;
	struct bt_record record;
	size_t position;
	enum btrfs_result error;

	*found = 0;
	*count = 0;
	if (any_level) {
		key.offset = 0;
	}
	bt_cursor_init(&cursor, view->fs, view->extents);
	error = bt_cursor_seek(&cursor, key, 0);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != key.objectid || record.key.type != key.type ||
		    (!any_level && record.key.offset != key.offset)) {
			error = BTRFS_NOT_FOUND;
		} else if (key.type == BT_METADATA_ITEM && record.key.offset >= BT_MAX_LEVEL) {
			error = BTRFS_CORRUPT;
		}
	}
	if (error == BTRFS_OK) {
		item = (const void *)record.data;
		if (record.size < sizeof(*item) ||
		    (key.type == BT_METADATA_ITEM) !=
			!(bt_u64(item->flags) & BT_EXTENT_FLAG_DATA)) {
			error = BTRFS_CORRUPT;
		} else {
			*found = 1;
			*generation = bt_u64(item->generation);
			*level = key.type == BT_METADATA_ITEM ? (uint8_t)record.key.offset : 0;
		}
		for (position = sizeof(*item); error == BTRFS_OK && position < record.size;) {
			error = bt_qg_inline_ref(record.data, record.size, &position, &ref);
			if (error == BTRFS_OK) {
				error = bt_qg_add_ref(env, refs, count, capacity, ref);
			}
		}
		if (error == BTRFS_OK) {
			error = bt_cursor_next(&cursor);
		}
		while (error == BTRFS_OK) {
			(void)bt_cursor_record(&cursor, &record);
			if (record.key.objectid != key.objectid ||
			    record.key.type < BT_TREE_BLOCK_REF ||
			    record.key.type > BT_SHARED_DATA_REF) {
				break;
			}
			bt_zero(&ref, sizeof(ref));
			ref.type = record.key.type;
			ref.count = 1;
			if (ref.type == BT_TREE_BLOCK_REF) {
				ref.root = record.key.offset;
			} else if (ref.type == BT_SHARED_BLOCK_REF) {
				ref.parent = record.key.offset;
			} else if (ref.type == BT_EXTENT_DATA_REF && record.size == sizeof(*wire)) {
				wire = (const void *)record.data;
				ref.root = bt_u64(wire->root);
				ref.inode = bt_u64(wire->objectid);
				ref.offset = bt_u64(wire->offset);
				ref.count = bt_u32(wire->count);
			} else if (ref.type == BT_SHARED_DATA_REF &&
			    record.size == sizeof(*shared)) {
				shared = (const void *)record.data;
				ref.parent = record.key.offset;
				ref.count = bt_u32(shared->count);
			} else if (ref.type == BT_EXTENT_DATA_REF ||
			    ref.type == BT_SHARED_DATA_REF) {
				error = BTRFS_CORRUPT;
				break;
			} else {
				/* Types between the reference types name nothing here. */
				error = bt_cursor_next(&cursor);
				continue;
			}
			error = bt_qg_add_ref(env, refs, count, capacity, ref);
			if (error == BTRFS_OK) {
				error = bt_cursor_next(&cursor);
			}
		}
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* Heap sort with constant extra storage: elements of at most BT_QG_ELEMENT
 * bytes, ordered by less. */
#define BT_QG_ELEMENT 32U

static void
bt_qg_swap(uint8_t *items, size_t size, size_t a, size_t b)
{
	uint8_t swap[BT_QG_ELEMENT];

	bt_copy(swap, items + a * size, size);
	bt_copy(items + a * size, items + b * size, size);
	bt_copy(items + b * size, swap, size);
}

static void
bt_qg_sift(
    uint8_t *items, size_t size, size_t root, size_t end, int (*less)(const void *, const void *))
{
	size_t child;

	while (2 * root + 1 < end) {
		child = 2 * root + 1;
		if (child + 1 < end && less(items + child * size, items + (child + 1) * size)) {
			child++;
		}
		if (!less(items + root * size, items + child * size)) {
			return;
		}
		bt_qg_swap(items, size, root, child);
		root = child;
	}
}

static void
bt_qg_sort(void *items, size_t count, size_t size, int (*less)(const void *, const void *))
{
	size_t i;

	if (count < 2 || size > BT_QG_ELEMENT) {
		return;
	}
	for (i = count / 2; i > 0; i--) {
		bt_qg_sift(items, size, i - 1, count, less);
	}
	for (i = count - 1; i > 0; i--) {
		bt_qg_swap(items, size, 0, i);
		bt_qg_sift(items, size, 0, i, less);
	}
}

static int
bt_qg_key_less(const void *a, const void *b)
{
	return bt_key_compare(*(const struct bt_key *)a, *(const struct bt_key *)b) < 0;
}

static int
bt_qg_relation_less(const void *a, const void *b)
{
	const struct bt_qgroup_relation *x = a;
	const struct bt_qgroup_relation *y = b;

	return x->member != y->member ? x->member < y->member : x->parent < y->parent;
}

static enum btrfs_result bt_qg_block_roots(struct bt_qgroup_view *view, uint64_t address,
    unsigned depth, int optional, struct bt_rootset *out);

/* The node one level above a tree block on the search path of the block's
 * first key in tree, or 0: Linux's resolve_indirect_ref, which takes that node
 * as the parent. Kept out of line so that no cursor stays on the stack of the
 * recursive walk. */
static BT_NOINLINE enum btrfs_result
bt_qg_parent(struct bt_qgroup_view *view, struct bt_root tree, uint64_t address, uint8_t level,
    uint64_t generation, uint64_t *parent)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key first = { 0 };
	enum btrfs_result error;

	*parent = 0;
	/* The block's first key is its first record's. */
	bt_cursor_init(&cursor, view->fs,
	    (struct bt_root){ address, generation, BTRFS_TOP_LEVEL_TREE, level });
	error = bt_cursor_seek(&cursor, first, 0);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		first = record.key;
	}
	bt_cursor_fini(&cursor);
	if (error != BTRFS_OK) {
		return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
	}
	bt_cursor_init(&cursor, view->fs, tree);
	error = bt_cursor_seek(&cursor, first, 1);
	if (error == BTRFS_OK) {
		*parent = cursor.loaded[level + 1].address;
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* Linux's resolve_indirect_ref for a tree block. A block at the tree's root
 * level, a subvolume being dropped (in the transaction's view) and a search
 * that finds nothing leave the root itself. */
static enum btrfs_result
bt_qg_tree_ref(struct bt_qgroup_view *view, uint64_t root, uint64_t address, uint8_t level,
    uint64_t generation, unsigned depth, struct bt_rootset *out)
{
	const struct bt_qgroup_tree *tree;
	uint64_t parent = 0;
	enum btrfs_result error;

	if (!bt_file_tree(root)) {
		return BTRFS_OK;
	}
	error = bt_qg_tree(view, root, &tree);
	if (error == BTRFS_OK && tree->found && !(view->current && tree->deleted) &&
	    level < tree->root.level) {
		error = bt_qg_parent(view, tree->root, address, level, generation, &parent);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	if (parent != 0) {
		return bt_qg_block_roots(view, parent, depth + 1, 0, out);
	}
	return bt_rootset_add(view->qgroups->env, out, root);
}

/* Linux's add_all_parents for a data reference: each leaf of tree that the
 * reference's root owns and that is not a shared parent of the extent, whose
 * file extent items of the reference's inode and offset name the extent,
 * until the reference's count is found. Out of line, as bt_qg_parent. */
static BT_NOINLINE enum btrfs_result
bt_qg_data_leaves(struct bt_qgroup_view *view, struct bt_root tree, const struct bt_qgroup_ref *ref,
    uint64_t extent, const struct bt_qgroup_ref *refs, size_t count, struct bt_rootset *leaves)
{
	const struct bt_disk_extent *file;
	const struct bt_disk_header *header;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = ref->inode,
		.type = BT_EXTENT_DATA,
		.offset = ref->offset >= BT_QGROUP_OFFSET_LIMIT ? 0 : ref->offset };
	uint64_t leaf;
	uint64_t matched = 0;
	size_t i;
	int skip;
	enum btrfs_result error;

	bt_cursor_init(&cursor, view->fs, tree);
	error = bt_cursor_seek(&cursor, key, 0);
	while (error == BTRFS_OK && matched < ref->count) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != ref->inode || record.key.type != BT_EXTENT_DATA) {
			break;
		}
		leaf = cursor.loaded[0].address;
		header = (const void *)cursor.blocks[0];
		file = (const void *)record.data;
		skip = bt_u64(header->owner) != ref->root ||
		    (record.size >= sizeof(file->header) && file->header.type == BT_EXTENT_INLINE);
		for (i = 0; i < count; i++) {
			skip |= refs[i].type == BT_SHARED_DATA_REF && refs[i].parent == leaf;
		}
		if (!skip && record.size < sizeof(*file)) {
			error = BTRFS_CORRUPT;
			break;
		}
		if (!skip && bt_u64(file->disk_bytenr) == extent &&
		    record.key.offset - bt_u64(file->offset) == ref->offset) {
			matched++;
			error = bt_rootset_add(view->qgroups->env, leaves, leaf);
		}
		if (error == BTRFS_OK) {
			error = bt_cursor_next(&cursor);
		}
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* The roots a data reference contributes: those of its leaves, or without one
 * the reference's root itself. */
static enum btrfs_result
bt_qg_data_ref(struct bt_qgroup_view *view, const struct bt_qgroup_ref *ref, uint64_t extent,
    const struct bt_qgroup_ref *refs, size_t count, struct bt_rootset *out)
{
	const struct btrfs_environment *env = view->qgroups->env;
	const struct bt_qgroup_tree *tree;
	struct bt_rootset leaves = { NULL, 0, 0 };
	size_t i;
	enum btrfs_result error;

	if (!bt_file_tree(ref->root)) {
		return BTRFS_OK;
	}
	error = bt_qg_tree(view, ref->root, &tree);
	if (error == BTRFS_OK && tree->found && !(view->current && tree->deleted)) {
		error = bt_qg_data_leaves(view, tree->root, ref, extent, refs, count, &leaves);
	}
	if (error == BTRFS_OK && leaves.count == 0) {
		error = bt_rootset_add(env, out, ref->root);
	}
	for (i = 0; error == BTRFS_OK && i < leaves.count; i++) {
		error = bt_qg_block_roots(view, leaves.ids[i], 0, 0, out);
	}
	bt_rootset_free(env, &leaves);
	return error;
}

/* The subvolume trees reaching a tree block through its references; parents
 * are one level up, so the walk ends within BT_MAX_LEVEL steps. Without an
 * extent item the block is unreferenced (optional) or corrupt. */
static enum btrfs_result
bt_qg_block_roots(struct bt_qgroup_view *view, uint64_t address, unsigned depth, int optional,
    struct bt_rootset *out)
{
	const struct btrfs_environment *env = view->qgroups->env;
	const struct bt_qgroup_memo *memo;
	struct bt_qgroup_ref *refs = NULL;
	struct bt_rootset set = { NULL, 0, 0 };
	struct bt_key key = { .objectid = address, .type = BT_METADATA_ITEM, .offset = 0 };
	size_t count = 0;
	size_t capacity = 0;
	size_t i;
	uint64_t generation = 0;
	uint8_t level = 0;
	int found = 0;
	enum btrfs_result error;

	memo = bt_qg_memo_find(view, address);
	if (memo != NULL) {
		for (i = 0, error = BTRFS_OK; error == BTRFS_OK && i < memo->count; i++) {
			error = bt_rootset_add(env, out, view->pool[memo->first + i]);
		}
		return error;
	}
	if (depth >= BT_MAX_LEVEL) {
		return BTRFS_CORRUPT;
	}
	error = bt_qg_refs(view, key, 1, &generation, &level, &refs, &count, &capacity, &found);
	if (error == BTRFS_OK && !found) {
		error = optional ? BTRFS_OK : BTRFS_CORRUPT;
	}
	for (i = 0; error == BTRFS_OK && found && i < count; i++) {
		if (refs[i].type == BT_TREE_BLOCK_REF) {
			error = bt_qg_tree_ref(
			    view, refs[i].root, address, level, generation, depth, &set);
		} else if (refs[i].type == BT_SHARED_BLOCK_REF) {
			error = bt_qg_block_roots(view, refs[i].parent, depth + 1, 0, &set);
		} else {
			error = BTRFS_CORRUPT;
		}
	}
	bt_qg_free(env, refs, capacity, sizeof(*refs));
	if (error == BTRFS_OK && found) {
		error = bt_qg_memo_add(view, address, &set);
	}
	for (i = 0; error == BTRFS_OK && i < set.count; i++) {
		error = bt_rootset_add(env, out, set.ids[i]);
	}
	bt_rootset_free(env, &set);
	return error;
}

/* The subvolume trees that reach an extent in the view, as btrfs_find_all_roots
 * finds them; none when the view has no such extent. */
static enum btrfs_result
bt_qg_extent_roots(struct bt_qgroup_view *view, struct bt_key extent, struct bt_rootset *out)
{
	const struct btrfs_environment *env = view->qgroups->env;
	struct bt_qgroup_ref *refs = NULL;
	size_t count = 0;
	size_t capacity = 0;
	size_t i;
	uint64_t generation = 0;
	uint8_t level = 0;
	int found = 0;
	enum btrfs_result error;

	if (extent.type == BT_METADATA_ITEM) {
		return bt_qg_block_roots(view, extent.objectid, 0, 1, out);
	}
	error = bt_qg_refs(view, extent, 0, &generation, &level, &refs, &count, &capacity, &found);
	for (i = 0; error == BTRFS_OK && found && i < count; i++) {
		if (refs[i].type == BT_EXTENT_DATA_REF) {
			error = bt_qg_data_ref(view, &refs[i], extent.objectid, refs, count, out);
		} else if (refs[i].type == BT_SHARED_DATA_REF) {
			error = bt_qg_block_roots(view, refs[i].parent, 0, 0, out);
		} else {
			error = BTRFS_CORRUPT;
		}
	}
	bt_qg_free(env, refs, capacity, sizeof(*refs));
	return error;
}

/* The first relation whose member is id. */
static size_t
bt_qg_relations_of(const struct bt_qgroups *qgroups, uint64_t id)
{
	size_t low = 0;
	size_t high = qgroups->relation_count;
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (qgroups->relations[middle].member < id) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return low;
}

/* Linux's qgroup_update_refcnt: each root's qgroup and every qgroup above it
 * count once per root. */
static void
bt_qg_count(struct bt_qgroups *qgroups, const struct bt_rootset *roots, int new_roots)
{
	struct bt_qgroup *group;
	size_t index;
	size_t head;
	size_t tail;
	size_t parent;
	size_t r;
	size_t i;

	for (i = 0; i < roots->count; i++) {
		index = bt_qg_find(qgroups, roots->ids[i]);
		if (index == SIZE_MAX) {
			continue;
		}
		qgroups->visit++;
		head = 0;
		tail = 0;
		qgroups->queue[tail++] = index;
		qgroups->groups[index].visit = qgroups->visit;
		while (head < tail) {
			index = qgroups->queue[head++];
			group = &qgroups->groups[index];
			if (group->extent != qgroups->extent) {
				group->extent = qgroups->extent;
				group->old_count = 0;
				group->new_count = 0;
				qgroups->touched[qgroups->touched_count++] = index;
			}
			if (new_roots) {
				group->new_count++;
			} else {
				group->old_count++;
			}
			for (r = bt_qg_relations_of(qgroups, group->id);
			    r < qgroups->relation_count &&
			    qgroups->relations[r].member == group->id;
			    r++) {
				parent = bt_qg_find(qgroups, qgroups->relations[r].parent);
				if (parent != SIZE_MAX &&
				    qgroups->groups[parent].visit != qgroups->visit) {
					qgroups->groups[parent].visit = qgroups->visit;
					qgroups->queue[tail++] = parent;
				}
			}
		}
	}
}

static int
bt_qg_change(uint64_t *value, uint64_t *compressed, uint64_t bytes, int add)
{
	if (!add && (*value < bytes || *compressed < bytes)) {
		return 0;
	}
	*value = add ? *value + bytes : *value - bytes;
	*compressed = add ? *compressed + bytes : *compressed - bytes;
	return 1;
}

/* Linux's qgroup_update_counters for one extent of bytes. A count that would
 * go below zero makes quotas inconsistent. */
static void
bt_qg_update(struct bt_qgroups *qgroups, uint64_t old_roots, uint64_t new_roots, uint64_t bytes)
{
	struct bt_qgroup *group;
	uint64_t old;
	uint64_t new;
	size_t i;
	int valid = 1;

	for (i = 0; i < qgroups->touched_count; i++) {
		group = &qgroups->groups[qgroups->touched[i]];
		old = group->old_count;
		new = group->new_count;
		if (old == 0 && new > 0) {
			valid &= bt_qg_change(
			    &group->referenced, &group->referenced_compressed, bytes, 1);
			group->dirty = 1;
		}
		if (old > 0 && new == 0) {
			valid &= bt_qg_change(
			    &group->referenced, &group->referenced_compressed, bytes, 0);
			group->dirty = 1;
		}
		/* Exclusive or none before, shared now. */
		if (old == old_roots && new < new_roots && old != 0) {
			valid &=
			    bt_qg_change(&group->exclusive, &group->exclusive_compressed, bytes, 0);
			group->dirty = 1;
		}
		/* Shared before, exclusive now. */
		if (old < old_roots && new == new_roots && new != 0) {
			valid &=
			    bt_qg_change(&group->exclusive, &group->exclusive_compressed, bytes, 1);
			group->dirty = 1;
		}
		/* Exclusive or none, before and now. */
		if (old == old_roots && new == new_roots && (old == 0) != (new == 0)) {
			valid &= bt_qg_change(
			    &group->exclusive, &group->exclusive_compressed, bytes, old == 0);
			group->dirty = 1;
		}
	}
	if (!valid) {
		bt_qg_inconsistent(qgroups);
	}
}

/* btrfs_qgroup_inherit for this transaction's snapshot, from counts at the
 * snapshot point, which are the base's: the copy references what the source
 * references, and each holds only its root node alone. A source in a higher
 * qgroup leaves quotas inconsistent, as Linux leaves them without an inherit
 * request. */
static void
bt_qg_inherit(struct bt_qgroups *qgroups, uint64_t node_size)
{
	struct bt_qgroup *source;
	struct bt_qgroup *target;
	size_t r;
	size_t index;

	index = bt_qg_find(qgroups, qgroups->snapshot_source);
	if (qgroups->snapshot_target == 0 || index == SIZE_MAX) {
		return;
	}
	source = &qgroups->groups[index];
	index = bt_qg_find(qgroups, qgroups->snapshot_target);
	if (index == SIZE_MAX) {
		return;
	}
	target = &qgroups->groups[index];
	target->referenced = source->referenced;
	target->referenced_compressed = source->referenced_compressed;
	target->exclusive = node_size;
	target->exclusive_compressed = node_size;
	source->exclusive = node_size;
	source->exclusive_compressed = node_size;
	target->limit_flags = source->limit_flags;
	target->max_referenced = source->max_referenced;
	target->max_exclusive = source->max_exclusive;
	target->reserved_referenced = source->reserved_referenced;
	target->reserved_exclusive = source->reserved_exclusive;
	target->dirty = 1;
	source->dirty = 1;
	r = bt_qg_relations_of(qgroups, source->id);
	if (r < qgroups->relation_count && qgroups->relations[r].member == source->id) {
		bt_qg_inconsistent(qgroups);
	}
}

/* Old roots at this transaction's snapshot point, the base with the copy
 * added: the copy itself belongs to the target alone, and everything the
 * source reaches below its root node the target reaches too. */
static enum btrfs_result
bt_qg_snapshot_roots(const struct bt_qgroups *qgroups, struct bt_key extent, struct bt_rootset *old)
{
	size_t i;
	int source = 0;

	if (qgroups->snapshot_target == 0) {
		return BTRFS_OK;
	}
	if (extent.type == BT_METADATA_ITEM && extent.objectid == qgroups->snapshot_copy) {
		old->count = 0;
		return bt_rootset_add(qgroups->env, old, qgroups->snapshot_target);
	}
	for (i = 0; i < old->count; i++) {
		source |= old->ids[i] == qgroups->snapshot_source;
	}
	if (!source ||
	    (extent.type == BT_METADATA_ITEM && extent.objectid == qgroups->snapshot_root)) {
		return BTRFS_OK;
	}
	return bt_rootset_add(qgroups->env, old, qgroups->snapshot_target);
}

static enum btrfs_result
bt_qg_account(struct btrfs_transaction *transaction, struct bt_qgroups *qgroups)
{
	const struct btrfs_environment *env = qgroups->env;
	const struct btrfs_fs *view = bt_mutation_view(transaction->mutation);
	struct bt_qgroup_view before;
	struct bt_qgroup_view after;
	struct bt_rootset old = { NULL, 0, 0 };
	struct bt_rootset new = { NULL, 0, 0 };
	struct bt_root extents;
	size_t kept = 0;
	size_t i;
	enum btrfs_result error;

	if (view == NULL) {
		return btrfs_transaction_failure(transaction);
	}
	bt_qg_sort(
	    qgroups->traced, qgroups->traced_count, sizeof(*qgroups->traced), bt_qg_key_less);
	for (i = 0; i < qgroups->traced_count; i++) {
		if (kept == 0 ||
		    bt_key_compare(qgroups->traced[kept - 1], qgroups->traced[i]) != 0) {
			qgroups->traced[kept++] = qgroups->traced[i];
		}
	}
	qgroups->traced_count = kept;
	error = bt_find_root(transaction->base, BT_EXTENT_TREE, &extents);
	if (error != BTRFS_OK) {
		return error;
	}
	qgroups->scratch_capacity = qgroups->count;
	qgroups->touched = env->allocate(env->context, qgroups->count * sizeof(*qgroups->touched));
	qgroups->queue = env->allocate(env->context, qgroups->count * sizeof(*qgroups->queue));
	if (qgroups->count != 0 && (qgroups->touched == NULL || qgroups->queue == NULL)) {
		return BTRFS_NO_MEMORY;
	}
	bt_qg_view_init(
	    &before, qgroups, transaction->base, transaction->base->root_tree, extents, 0);
	bt_qg_view_init(&after, qgroups, view, transaction->roots, transaction->extents.root, 1);
	bt_qg_inherit(qgroups, transaction->base->info.node_size);
	for (i = 0; error == BTRFS_OK && qgroups->accounting && i < qgroups->traced_count; i++) {
		old.count = 0;
		new.count = 0;
		error = bt_qg_extent_roots(&before, qgroups->traced[i], &old);
		if (error == BTRFS_OK) {
			error = bt_qg_snapshot_roots(qgroups, qgroups->traced[i], &old);
		}
		if (error == BTRFS_OK) {
			error = bt_qg_extent_roots(&after, qgroups->traced[i], &new);
		}
		if (error != BTRFS_OK || (old.count == 0 && new.count == 0)) {
			continue;
		}
		qgroups->extent++;
		qgroups->touched_count = 0;
		bt_qg_count(qgroups, &old, 0);
		bt_qg_count(qgroups, &new, 1);
		bt_qg_update(qgroups, old.count, new.count,
		    qgroups->traced[i].type == BT_METADATA_ITEM ? transaction->base->info.node_size
								: qgroups->traced[i].offset);
	}
	bt_rootset_free(env, &old);
	bt_rootset_free(env, &new);
	bt_qg_view_fini(&before);
	bt_qg_view_fini(&after);
	return error;
}

static enum btrfs_result
bt_qg_observe(void *context, struct bt_key extent)
{
	struct bt_qgroups *qgroups = context;
	enum btrfs_result error;

	if (!qgroups->accounting ||
	    (extent.type != BT_EXTENT_ITEM && extent.type != BT_METADATA_ITEM)) {
		return BTRFS_OK;
	}
	error = bt_qg_grow(qgroups->env, (void **)&qgroups->traced, &qgroups->traced_capacity,
	    qgroups->traced_count, sizeof(*qgroups->traced), SIZE_MAX / sizeof(*qgroups->traced));
	if (error == BTRFS_OK) {
		qgroups->traced[qgroups->traced_count++] = extent;
	}
	return error;
}

enum btrfs_result
bt_qgroup_begin(struct btrfs_transaction *transaction)
{
	const struct btrfs_environment *env = &transaction->base->env;
	struct bt_mutation_observer observer;
	struct bt_qgroups *qgroups;
	enum btrfs_result error;

	qgroups = env->allocate(env->context, sizeof(*qgroups));
	if (qgroups == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(qgroups, sizeof(*qgroups));
	qgroups->env = env;
	qgroups->accounting = 1;
	qgroups->generation = transaction->base->info.generation + 1;
	transaction->qgroups = qgroups;
	error = bt_qg_load(transaction, qgroups);
	if (error == BTRFS_OK) {
		bt_qg_sort(qgroups->relations, qgroups->relation_count, sizeof(*qgroups->relations),
		    bt_qg_relation_less);
		observer = (struct bt_mutation_observer){ qgroups, bt_qg_observe };
		bt_mutation_observe(transaction->mutation, &observer);
	}
	return error;
}

void
bt_qgroup_end(struct btrfs_transaction *transaction)
{
	struct bt_qgroups *qgroups = transaction->qgroups;
	const struct btrfs_environment *env;

	if (qgroups == NULL) {
		return;
	}
	env = qgroups->env;
	bt_qg_free(env, qgroups->groups, qgroups->capacity, sizeof(*qgroups->groups));
	bt_qg_free(
	    env, qgroups->relations, qgroups->relation_capacity, sizeof(*qgroups->relations));
	bt_qg_free(env, qgroups->traced, qgroups->traced_capacity, sizeof(*qgroups->traced));
	bt_qg_free(env, qgroups->touched, qgroups->scratch_capacity, sizeof(*qgroups->touched));
	bt_qg_free(env, qgroups->queue, qgroups->scratch_capacity, sizeof(*qgroups->queue));
	env->release(env->context, qgroups, sizeof(*qgroups));
	transaction->qgroups = NULL;
}

static enum btrfs_result
bt_qg_relation_items(struct btrfs_transaction *transaction, uint64_t member, uint64_t parent)
{
	struct bt_key key = { .objectid = member, .type = BT_QGROUP_RELATION, .offset = parent };
	enum btrfs_result error;

	error = bt_tx_edit(transaction, &transaction->quota.root, key, NULL, 0, BT_DELETE);
	if (error == BTRFS_OK || error == BTRFS_NOT_FOUND) {
		key = (struct bt_key){
			.objectid = parent, .type = BT_QGROUP_RELATION, .offset = member
		};
		error = bt_tx_edit(transaction, &transaction->quota.root, key, NULL, 0, BT_DELETE);
	}
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* btrfs_remove_qgroup for the qgroup of a fully dropped subvolume: its items
 * and relations go. Leaving a parent subtracts its exclusive bytes from the
 * parent and every qgroup above (quick_update_accounting) when they are all
 * it references, otherwise quotas become inconsistent; numbers left on a
 * consistent qgroup make them inconsistent too. */
static enum btrfs_result
bt_qg_remove(struct btrfs_transaction *transaction, struct bt_qgroups *qgroups, size_t index)
{
	struct bt_qgroup removed = qgroups->groups[index];
	struct bt_qgroup *group;
	struct bt_key key = { .objectid = 0, .type = BT_QGROUP_INFO, .offset = removed.id };
	uint64_t parent_id;
	size_t head;
	size_t tail;
	size_t parent;
	size_t r;
	enum btrfs_result error;

	error = bt_tx_edit(transaction, &transaction->quota.root, key, NULL, 0, BT_DELETE);
	if (error == BTRFS_OK || error == BTRFS_NOT_FOUND) {
		key.type = BT_QGROUP_LIMIT;
		error = bt_tx_edit(transaction, &transaction->quota.root, key, NULL, 0, BT_DELETE);
	}
	error = error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
	while (error == BTRFS_OK) {
		r = bt_qg_relations_of(qgroups, removed.id);
		if (r == qgroups->relation_count || qgroups->relations[r].member != removed.id) {
			break;
		}
		parent_id = qgroups->relations[r].parent;
		bt_move(&qgroups->relations[r], &qgroups->relations[r + 1],
		    (qgroups->relation_count - r - 1) * sizeof(*qgroups->relations));
		qgroups->relation_count--;
		error = bt_qg_relation_items(transaction, removed.id, parent_id);
		if (error != BTRFS_OK || removed.exclusive == 0) {
			continue;
		}
		if (removed.exclusive != removed.referenced) {
			bt_qg_inconsistent(qgroups);
			continue;
		}
		qgroups->visit++;
		head = 0;
		tail = 0;
		parent = bt_qg_find(qgroups, parent_id);
		if (parent != SIZE_MAX) {
			qgroups->queue[tail++] = parent;
			qgroups->groups[parent].visit = qgroups->visit;
		}
		while (head < tail) {
			group = &qgroups->groups[qgroups->queue[head++]];
			if (!bt_qg_change(&group->referenced, &group->referenced_compressed,
				removed.exclusive, 0) ||
			    !bt_qg_change(&group->exclusive, &group->exclusive_compressed,
				removed.exclusive, 0)) {
				bt_qg_inconsistent(qgroups);
			}
			group->dirty = 1;
			for (r = bt_qg_relations_of(qgroups, group->id);
			    r < qgroups->relation_count &&
			    qgroups->relations[r].member == group->id;
			    r++) {
				parent = bt_qg_find(qgroups, qgroups->relations[r].parent);
				if (parent != SIZE_MAX &&
				    qgroups->groups[parent].visit != qgroups->visit) {
					qgroups->groups[parent].visit = qgroups->visit;
					qgroups->queue[tail++] = parent;
				}
			}
		}
	}
	if (error == BTRFS_OK && (qgroups->flags & BT_QGROUP_STATUS_INCONSISTENT) == 0 &&
	    (removed.referenced != 0 || removed.exclusive != 0)) {
		bt_qg_inconsistent(qgroups);
	}
	if (error == BTRFS_OK) {
		bt_move(&qgroups->groups[index], &qgroups->groups[index + 1],
		    (qgroups->count - index - 1) * sizeof(*qgroups->groups));
		qgroups->count--;
	}
	return error;
}

static enum btrfs_result
bt_qg_store(struct btrfs_transaction *transaction, struct bt_qgroups *qgroups)
{
	struct bt_disk_qgroup_status *status = (void *)qgroups->status;
	struct bt_disk_qgroup_info info;
	struct bt_disk_qgroup_limit limit;
	struct bt_qgroup *group;
	struct bt_key key = { .objectid = 0, .type = BT_QGROUP_INFO, .offset = 0 };
	size_t i;
	enum btrfs_result error = BTRFS_OK;

	for (i = 0; error == BTRFS_OK && i < qgroups->count; i++) {
		group = &qgroups->groups[i];
		if (!group->dirty || !group->info) {
			continue;
		}
		bt_put64(&info.generation, qgroups->generation);
		bt_put64(&info.referenced, group->referenced);
		bt_put64(&info.referenced_compressed, group->referenced_compressed);
		bt_put64(&info.exclusive, group->exclusive);
		bt_put64(&info.exclusive_compressed, group->exclusive_compressed);
		key.offset = group->id;
		key.type = BT_QGROUP_INFO;
		error = bt_tx_edit(
		    transaction, &transaction->quota.root, key, &info, sizeof(info), BT_REPLACE);
		/* btrfs_run_qgroups writes a dirty qgroup's limit item as well. */
		if (error == BTRFS_OK && group->limit) {
			bt_put64(&limit.flags, group->limit_flags);
			bt_put64(&limit.max_referenced, group->max_referenced);
			bt_put64(&limit.max_exclusive, group->max_exclusive);
			bt_put64(&limit.reserved_referenced, group->reserved_referenced);
			bt_put64(&limit.reserved_exclusive, group->reserved_exclusive);
			key.type = BT_QGROUP_LIMIT;
			error = bt_tx_edit(transaction, &transaction->quota.root, key, &limit,
			    sizeof(limit), BT_REPLACE);
		}
		group->dirty = 0;
	}
	/* update_qgroup_status_item: every commit stamps the generation. */
	if (error == BTRFS_OK) {
		bt_put64(&status->generation, qgroups->generation);
		bt_put64(&status->flags, qgroups->flags);
		key = (struct bt_key){ .objectid = 0, .type = BT_QGROUP_STATUS, .offset = 0 };
		error = bt_tx_edit(transaction, &transaction->quota.root, key, qgroups->status,
		    qgroups->status_size, BT_REPLACE);
	}
	return error;
}

enum btrfs_result
bt_qgroup_commit(struct btrfs_transaction *transaction)
{
	struct bt_qgroups *qgroups = transaction->qgroups;
	size_t i;
	enum btrfs_result error = BTRFS_OK;

	if (qgroups == NULL) {
		return BTRFS_OK;
	}
	if (qgroups->accounting && (qgroups->traced_count != 0 || qgroups->snapshot_target != 0)) {
		error = bt_qg_account(transaction, qgroups);
	}
	if (error == BTRFS_OK && qgroups->queue == NULL && qgroups->count != 0) {
		qgroups->scratch_capacity = qgroups->count;
		qgroups->touched =
		    qgroups->env->allocate(qgroups->env->context, qgroups->count * sizeof(size_t));
		qgroups->queue =
		    qgroups->env->allocate(qgroups->env->context, qgroups->count * sizeof(size_t));
		error =
		    qgroups->touched == NULL || qgroups->queue == NULL ? BTRFS_NO_MEMORY : BTRFS_OK;
	}
	for (i = 0; error == BTRFS_OK && i < qgroups->count;) {
		if (qgroups->groups[i].dropped) {
			error = bt_qg_remove(transaction, qgroups, i);
		} else {
			i++;
		}
	}
	if (error == BTRFS_OK) {
		error = bt_qg_store(transaction, qgroups);
	}
	return error;
}

enum btrfs_result
bt_qgroup_create(struct btrfs_transaction *transaction, uint64_t subvolume)
{
	struct bt_qgroups *qgroups = transaction->qgroups;
	struct bt_disk_qgroup_info info;
	struct bt_disk_qgroup_limit limit;
	struct bt_key key = { .objectid = 0, .type = BT_QGROUP_INFO, .offset = subvolume };
	size_t index;
	enum btrfs_result error;

	if (qgroups == NULL) {
		return BTRFS_OK;
	}
	bt_zero(&info, sizeof(info));
	bt_zero(&limit, sizeof(limit));
	bt_put64(&info.generation, qgroups->generation);
	error = bt_qg_insert(qgroups, subvolume, &index);
	if (error == BTRFS_OK) {
		qgroups->groups[index].info = 1;
		qgroups->groups[index].limit = 1;
		error = bt_tx_edit(
		    transaction, &transaction->quota.root, key, &info, sizeof(info), BT_UPSERT);
	}
	if (error == BTRFS_OK) {
		key.type = BT_QGROUP_LIMIT;
		error = bt_tx_edit(
		    transaction, &transaction->quota.root, key, &limit, sizeof(limit), BT_UPSERT);
	}
	return error;
}

enum btrfs_result
bt_qgroup_reserve(struct btrfs_transaction *transaction, uint64_t tree, uint64_t bytes)
{
	const struct btrfs_environment *env;
	struct bt_qgroups *qgroups = transaction->qgroups;
	struct bt_qgroup *group;
	size_t *queue;
	size_t index;
	size_t head;
	size_t tail;
	size_t parent;
	size_t r;
	enum btrfs_result error = BTRFS_OK;

	if (qgroups == NULL || bytes == 0) {
		return BTRFS_OK;
	}
	index = bt_qg_find(qgroups, tree);
	if (index == SIZE_MAX) {
		return BTRFS_OK;
	}
	env = qgroups->env;
	queue = env->allocate(env->context, qgroups->count * sizeof(*queue));
	if (queue == NULL) {
		return BTRFS_NO_MEMORY;
	}
	/* qgroup_check_limits on the qgroup and every qgroup above it, then the
	 * reservation for all of them. */
	qgroups->visit++;
	head = 0;
	tail = 0;
	queue[tail++] = index;
	qgroups->groups[index].visit = qgroups->visit;
	while (head < tail) {
		group = &qgroups->groups[queue[head++]];
		if (((group->limit_flags & BT_QGROUP_LIMIT_MAX_REFERENCED) != 0 &&
			(group->reserved > UINT64_MAX - group->referenced - bytes ||
			    group->referenced + group->reserved + bytes > group->max_referenced)) ||
		    ((group->limit_flags & BT_QGROUP_LIMIT_MAX_EXCLUSIVE) != 0 &&
			(group->reserved > UINT64_MAX - group->exclusive - bytes ||
			    group->exclusive + group->reserved + bytes > group->max_exclusive))) {
			error = BTRFS_QUOTA_EXCEEDED;
		}
		for (r = bt_qg_relations_of(qgroups, group->id);
		    r < qgroups->relation_count && qgroups->relations[r].member == group->id; r++) {
			parent = bt_qg_find(qgroups, qgroups->relations[r].parent);
			if (parent != SIZE_MAX && qgroups->groups[parent].visit != qgroups->visit) {
				qgroups->groups[parent].visit = qgroups->visit;
				queue[tail++] = parent;
			}
		}
	}
	for (head = 0; error == BTRFS_OK && head < tail; head++) {
		qgroups->groups[queue[head]].reserved += bytes;
	}
	env->release(env->context, queue, qgroups->count * sizeof(*queue));
	return error;
}

/* Every block and data extent below block, read from the transaction's view;
 * the subtree is below BT_QGROUP_SUBTREE_LEVEL, so the recursion is shallow. */
static enum btrfs_result
bt_qg_trace_block(struct btrfs_transaction *transaction, struct bt_root block)
{
	const struct btrfs_fs *view = bt_mutation_view(transaction->mutation);
	const struct bt_disk_extent *file;
	const struct bt_disk_header *header;
	const struct bt_disk_pointer *pointers;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = {
		.objectid = block.address, .type = BT_METADATA_ITEM, .offset = block.level
	};
	uint32_t slot;
	enum btrfs_result error;

	if (view == NULL) {
		return btrfs_transaction_failure(transaction);
	}
	error = bt_qg_observe(transaction->qgroups, key);
	if (error != BTRFS_OK) {
		return error;
	}
	bt_cursor_init(&cursor, view, block);
	error = bt_cursor_seek(&cursor, (struct bt_key){ 0 }, 0);
	if (error == BTRFS_OK && block.level != 0) {
		header = (const void *)cursor.blocks[block.level];
		pointers = (const void *)(header + 1);
		for (slot = 0; error == BTRFS_OK && slot < bt_u32(header->count); slot++) {
			error = bt_qg_trace_block(transaction,
			    (struct bt_root){ bt_u64(pointers[slot].bytenr),
				bt_u64(pointers[slot].generation), block.owner,
				(uint8_t)(block.level - 1) });
		}
	}
	while (error == BTRFS_OK && block.level == 0) {
		(void)bt_cursor_record(&cursor, &record);
		file = (const void *)record.data;
		if (record.key.type == BT_EXTENT_DATA && record.size >= sizeof(*file) &&
		    file->header.type != BT_EXTENT_INLINE && bt_u64(file->disk_bytenr) != 0) {
			key = (struct bt_key){ .objectid = bt_u64(file->disk_bytenr),
				.type = BT_EXTENT_ITEM,
				.offset = bt_u64(file->disk_bytes) };
			error = bt_qg_observe(transaction->qgroups, key);
		}
		if (error == BTRFS_OK) {
			error = bt_cursor_next(&cursor);
		}
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

enum btrfs_result
bt_qgroup_trace_subtree(struct btrfs_transaction *transaction, struct bt_root block)
{
	struct bt_qgroups *qgroups = transaction->qgroups;

	if (qgroups == NULL || !qgroups->accounting) {
		return BTRFS_OK;
	}
	if (block.level >= BT_QGROUP_SUBTREE_LEVEL) {
		bt_qg_inconsistent(qgroups);
		return BTRFS_OK;
	}
	return bt_qg_trace_block(transaction, block);
}

enum btrfs_result
bt_qgroup_snapshot(struct btrfs_transaction *transaction, uint64_t source, uint64_t target,
    uint64_t source_root, uint64_t copy_root)
{
	struct bt_qgroups *qgroups = transaction->qgroups;

	if (qgroups == NULL) {
		return BTRFS_OK;
	}
	if (qgroups->snapshot_target != 0) {
		return BTRFS_UNSUPPORTED;
	}
	qgroups->snapshot_source = source;
	qgroups->snapshot_target = target;
	qgroups->snapshot_root = source_root;
	qgroups->snapshot_copy = copy_root;
	return BTRFS_OK;
}

enum btrfs_result
bt_qgroup_dropped(struct btrfs_transaction *transaction, uint64_t subvolume)
{
	struct bt_qgroups *qgroups = transaction->qgroups;
	size_t index;

	if (qgroups == NULL) {
		return BTRFS_OK;
	}
	index = bt_qg_find(qgroups, subvolume);
	if (index != SIZE_MAX) {
		qgroups->groups[index].dropped = 1;
	}
	return BTRFS_OK;
}

uint64_t
bt_qgroup_next_id(const struct btrfs_transaction *transaction)
{
	const struct bt_qgroups *qgroups = transaction->qgroups;
	uint64_t next = 0;
	size_t i;

	for (i = 0; qgroups != NULL && i < qgroups->count; i++) {
		if (bt_qg_level(qgroups->groups[i].id) == 0 && qgroups->groups[i].id >= next) {
			next = qgroups->groups[i].id + 1;
		}
	}
	return next;
}
