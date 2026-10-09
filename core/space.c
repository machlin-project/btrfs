/* SPDX-License-Identifier: BSD-3-Clause */
#include "space.h"
#include "fst.h"

#define BT_SPACE_MAX_GAPS 131072U
/* New chunks: a tenth of the device, at most Linux's class limits. */
#define BT_CHUNK_ALIGN (UINT64_C(1) << 20)
#define BT_CHUNK_DATA_MAX (UINT64_C(1) << 30)
#define BT_CHUNK_METADATA_MAX (UINT64_C(256) << 20)
#define BT_CHUNK_SYSTEM_MAX (UINT64_C(32) << 20)
/* Linux's check_system_chunk: system space for one device item update
 * (btrfs_calc_metadata_size, nodes for BTRFS_MAX_LEVEL levels, twice) and one
 * chunk item insertion (btrfs_calc_insert_metadata_size), in nodes. */
#define BT_SYSTEM_RESERVE_NODES (BT_MAX_LEVEL * 2U + BT_MAX_LEVEL)

struct bt_gap {
	uint64_t start, end;
};

/* Free ranges of one chunk class in ascending logical order. Reservations
 * consume them from next onward and never return a range to the list. A
 * transaction's list taken from a kept map is borrowed (the map owns items)
 * until its first change copies it. */
struct bt_gaps {
	struct bt_gap *items;
	size_t count;
	size_t capacity;
	size_t next;
	int borrowed;
};

/* Allocation classes, indexing per-class state. */
enum bt_space_kind { BT_SPACE_METADATA, BT_SPACE_DATA, BT_SPACE_SYSTEM, BT_SPACE_KINDS };

/* A free range of a chunk as loading found it, for the kept map. A chunk
 * without free ranges has one entry with start == end. */
struct bt_space_loaded {
	size_t chunk;
	uint64_t start, end;
};

struct bt_space {
	struct btrfs_fs *fs;
	struct bt_gaps metadata;
	struct bt_gaps data;
	struct bt_gaps system;
	/* Unallocated physical ranges of the device, known after verification. */
	struct bt_gaps device;
	/* Bytes used per chunk; used_capacity entries (the chunk table's). Before
	 * a chunk is loaded its block-group item gives it. */
	uint64_t *used;
	size_t used_capacity;
	/* Chunks whose extents were verified and whose free ranges are in their
	 * class list (used_capacity entries). */
	uint8_t *loaded;
	/* Per class: chunks below the frontier are loaded or of another class,
	 * and the free bytes of chunks not loaded yet. */
	size_t frontier[BT_SPACE_KINDS];
	uint64_t unloaded_free[BT_SPACE_KINDS];
	/* Trees a load reads, and the bound on extent items all loads visit.
	 * Block-group items are in the extent tree unless the filesystem has a
	 * block-group tree. */
	struct bt_root extents;
	struct bt_root free_space;
	struct bt_root groups;
	int has_free_space;
	int has_group_tree;
	uint64_t item_limit;
	uint64_t items_scanned;
	struct bt_space_loaded *loads;
	size_t load_count;
	size_t load_capacity;
	/* Copies a new data or metadata chunk takes (DUP profiles). */
	unsigned data_copies;
	unsigned metadata_copies;
	/* Mixed groups (MIXED_GROUPS): data and metadata share DATA|METADATA
	 * groups and their one free list, the metadata class. */
	int mixed;
	size_t original_chunks;
	int growth;
	struct bt_space_change *changes;
	size_t change_count;
	size_t change_capacity;
	size_t node_limit;
	/* Metadata nodes the transaction must still be able to obtain: data
	 * growth leaves the device space that metadata growth would need. */
	uint64_t metadata_hold;
};

/* The allocator state of one committed generation, kept by the owner across
 * transactions: the chunk map, block-group totals, free ranges per class in
 * canonical form (sorted, merged within a chunk) and the device's free
 * ranges. */
struct btrfs_allocation_map {
	struct btrfs_environment env;
	int valid;
	uint64_t generation;
	uint8_t uuid[BTRFS_UUID_SIZE];
	struct bt_chunk *chunks;
	size_t chunk_count;
	/* Entries allocated for chunks, used and loaded. */
	size_t capacity;
	uint64_t *used;
	uint8_t *loaded;
	uint64_t unloaded_free[BT_SPACE_KINDS];
	uint64_t item_limit;
	struct bt_gaps metadata;
	struct bt_gaps data;
	struct bt_gaps system;
	struct bt_gaps device;
	int growth;
	uint64_t scans;
	uint64_t reuses;
};

static void bt_gaps_normalize(struct bt_gaps *list);
static void bt_gaps_release(const struct btrfs_environment *env, struct bt_gaps *list);
static uint64_t bt_space_excluded(const struct bt_chunk *chunk);

/* Copies a borrowed list before its first change. */
static enum btrfs_result
bt_gaps_own(const struct btrfs_environment *env, struct bt_gaps *list)
{
	struct bt_gap *items;
	size_t capacity = list->count == 0 ? 1 : list->count;

	if (!list->borrowed) {
		return BTRFS_OK;
	}
	items = env->allocate(env->context, capacity * sizeof(*items));
	if (items == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_copy(items, list->items, list->count * sizeof(*items));
	list->items = items;
	list->capacity = capacity;
	list->borrowed = 0;
	return BTRFS_OK;
}

/* Lends map's list to a transaction. */
static void
bt_gaps_borrow(struct bt_gaps *list, const struct bt_gaps *map)
{
	*list = *map;
	list->capacity = 0;
	list->borrowed = 1;
}

static struct bt_gaps *
bt_space_class(struct bt_space *space, const struct bt_chunk *chunk)
{
	if (chunk->type & BT_BLOCK_METADATA) {
		return &space->metadata;
	}
	if (chunk->type & BT_BLOCK_SYSTEM) {
		return &space->system;
	}
	return chunk->type & BT_BLOCK_DATA ? &space->data : NULL;
}

static enum btrfs_result
bt_space_gap(struct bt_space *space, struct bt_gaps *list, uint64_t start, uint64_t end)
{
	struct bt_gap *gaps;
	size_t capacity;
	const struct btrfs_environment *env = &space->fs->env;
	enum btrfs_result error;

	if (list != NULL && start < end) {
		error = bt_gaps_own(env, list);
		if (error != BTRFS_OK) {
			return error;
		}
		if (list->count == BT_SPACE_MAX_GAPS) {
			return BTRFS_UNSUPPORTED;
		}
		if (list->count == list->capacity) {
			capacity = list->capacity == 0 ? 256 : list->capacity * 2;
			gaps = env->allocate(env->context, capacity * sizeof(*gaps));
			if (gaps == NULL) {
				return BTRFS_NO_MEMORY;
			}
			if (list->items != NULL) {
				bt_copy(gaps, list->items, list->count * sizeof(*gaps));
				env->release(
				    env->context, list->items, list->capacity * sizeof(*gaps));
			}
			list->items = gaps;
			list->capacity = capacity;
		}
		list->items[list->count++] = (struct bt_gap){ start, end };
	}
	return BTRFS_OK;
}

/* Removes [start, end) from list's ranges, splitting one that holds it. */
static enum btrfs_result
bt_space_carve(struct bt_space *space, struct bt_gaps *list, uint64_t start, uint64_t end)
{
	uint64_t old_end;
	size_t i;
	enum btrfs_result error;

	error = bt_gaps_own(&space->fs->env, list);
	for (i = 0; error == BTRFS_OK && i < list->count; i++) {
		if (start < list->items[i].end && end > list->items[i].start) {
			old_end = list->items[i].end;
			if (start <= list->items[i].start) {
				list->items[i].start = end < old_end ? end : old_end;
			} else {
				list->items[i].end = start;
				error = bt_space_gap(space, list, end, old_end);
			}
		}
	}
	return error;
}

/* Exclude every physical superblock's whole stripe of one chunk from
 * allocation, including each DUP mapping. Extent trees do not describe these
 * reserved stripes. */
static enum btrfs_result
bt_space_exclude(struct bt_space *space, const struct bt_chunk *chunk, struct bt_gaps *list)
{
	uint64_t physical;
	uint64_t start;
	uint64_t end;
	unsigned mirror;
	unsigned copy;
	enum btrfs_result error = BTRFS_OK;

	for (copy = 0; list != NULL && error == BTRFS_OK && copy < chunk->mirrors; copy++) {
		for (mirror = 0; error == BTRFS_OK && mirror < BT_SUPER_MIRRORS; mirror++) {
			physical = bt_super_offset(mirror) & ~(BT_STRIPE_LENGTH - 1);
			if (physical + BT_STRIPE_LENGTH <= chunk->physical[copy] ||
			    physical >= chunk->physical[copy] + chunk->length) {
				continue;
			}
			start = chunk->logical +
			    (physical > chunk->physical[copy] ? physical - chunk->physical[copy]
							      : 0);
			end = physical + BT_STRIPE_LENGTH - chunk->physical[copy];
			end = chunk->logical + (end < chunk->length ? end : chunk->length);
			error = bt_space_carve(space, list, start, end);
		}
	}
	return error;
}

static enum btrfs_result
bt_space_exclude_chunk(struct bt_space *space, size_t chunk_index)
{
	const struct bt_chunk *chunk = &space->fs->chunks[chunk_index];

	return bt_space_exclude(space, chunk, bt_space_class(space, chunk));
}

static enum btrfs_result
bt_space_extent(struct bt_space *space, const struct bt_chunk *chunk,
    const struct bt_record *record, uint64_t position)
{
	const struct btrfs_fs *fs = space->fs;
	const struct bt_disk_extent_item *extent = (const void *)record->data;
	uint64_t length =
	    record->key.type == BT_METADATA_ITEM ? fs->info.node_size : record->key.offset;
	uint64_t flags = record->size >= sizeof(*extent) ? bt_u64(extent->flags) : 0;

	if (record->size < sizeof(*extent) || bt_u64(extent->refs) == 0 ||
	    bt_u64(extent->generation) > fs->info.generation ||
	    (flags != BT_EXTENT_FLAG_DATA && flags != BT_EXTENT_FLAG_TREE &&
		flags != (BT_EXTENT_FLAG_TREE | BT_EXTENT_FLAG_FULL_BACKREF)) ||
	    (record->key.type == BT_METADATA_ITEM &&
		(!(flags & BT_EXTENT_FLAG_TREE) || record->key.offset >= BT_MAX_LEVEL)) ||
	    length == 0 || length % fs->info.sector_size != 0 || record->key.objectid < position ||
	    record->key.objectid % fs->info.sector_size != 0 ||
	    length > chunk->logical + chunk->length - record->key.objectid ||
	    (flags == BT_EXTENT_FLAG_DATA
		    ? !(chunk->type & BT_BLOCK_DATA)
		    : !(chunk->type & (BT_BLOCK_METADATA | BT_BLOCK_SYSTEM)))) {
		return BTRFS_CORRUPT;
	}
	return BTRFS_OK;
}

/* The free list data comes from: the shared one of mixed groups. */
static struct bt_gaps *
bt_space_data_list(struct bt_space *space)
{
	return space->mixed ? &space->metadata : &space->data;
}

static enum bt_space_kind
bt_space_kind_of(struct bt_space *space, const struct bt_gaps *list)
{
	return list == &space->metadata ? BT_SPACE_METADATA
	    : list == &space->data	? BT_SPACE_DATA
					: BT_SPACE_SYSTEM;
}

/* Inserts count ranges, ascending and below every range from next on that
 * lies above them, keeping the list's remaining ranges sorted. */
static enum btrfs_result
bt_gaps_splice(const struct btrfs_environment *env, struct bt_gaps *list,
    const struct bt_gap *items, size_t count)
{
	struct bt_gap *grown;
	size_t capacity;
	size_t low = list->next;
	size_t high = list->count;
	size_t middle;
	enum btrfs_result error;

	if (count == 0) {
		return BTRFS_OK;
	}
	error = bt_gaps_own(env, list);
	if (error != BTRFS_OK) {
		return error;
	}
	if (count > BT_SPACE_MAX_GAPS - list->count) {
		return BTRFS_UNSUPPORTED;
	}
	if (list->count + count > list->capacity) {
		capacity = list->capacity == 0 ? 256 : list->capacity;
		while (capacity < list->count + count) {
			capacity *= 2;
		}
		grown = env->allocate(env->context, capacity * sizeof(*grown));
		if (grown == NULL) {
			return BTRFS_NO_MEMORY;
		}
		if (list->items != NULL) {
			bt_copy(grown, list->items, list->count * sizeof(*grown));
			env->release(env->context, list->items, list->capacity * sizeof(*grown));
		}
		list->items = grown;
		list->capacity = capacity;
	}
	while (low < high) {
		middle = low + (high - low) / 2;
		if (list->items[middle].start < items[0].start) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	bt_move(&list->items[low + count], &list->items[low],
	    (list->count - low) * sizeof(*list->items));
	bt_copy(&list->items[low], items, count * sizeof(*items));
	list->count += count;
	return BTRFS_OK;
}

static enum btrfs_result
bt_space_log_load(struct bt_space *space, size_t chunk, uint64_t start, uint64_t end)
{
	const struct btrfs_environment *env = &space->fs->env;
	struct bt_space_loaded *grown;
	size_t capacity;

	if (space->load_count == space->load_capacity) {
		if (space->load_capacity == BT_SPACE_MAX_GAPS) {
			return BTRFS_UNSUPPORTED;
		}
		capacity = space->load_capacity == 0 ? 64 : space->load_capacity * 2;
		grown = env->allocate(env->context, capacity * sizeof(*grown));
		if (grown == NULL) {
			return BTRFS_NO_MEMORY;
		}
		if (space->loads != NULL) {
			bt_copy(grown, space->loads, space->load_count * sizeof(*grown));
			env->release(
			    env->context, space->loads, space->load_capacity * sizeof(*grown));
		}
		space->loads = grown;
		space->load_capacity = capacity;
	}
	space->loads[space->load_count++] = (struct bt_space_loaded){ chunk, start, end };
	return BTRFS_OK;
}

/* Whether a record names an extent or block group, which must lie in a chunk. */
static int
bt_space_owned(const struct bt_record *record)
{
	return record->key.type == BT_EXTENT_ITEM || record->key.type == BT_METADATA_ITEM ||
	    record->key.type == BT_BLOCK_GROUP_ITEM;
}

/* Loads one chunk: an ordered pass over its extent records, starting where
 * the previous chunk ends so that no extent between chunks goes unseen. Its
 * extents must be aligned, disjoint and inside it and sum to its block-group
 * total; its free runs must equal its free-space items; superblock stripes
 * are then excluded and the remaining free ranges join the class list. */
static enum btrfs_result
bt_space_load_chunk(struct bt_space *space, size_t index)
{
	const struct btrfs_fs *fs = space->fs;
	const struct bt_chunk *chunk = &fs->chunks[index];
	const struct bt_disk_block_group *group;
	struct bt_gaps *list = bt_space_class(space, chunk);
	struct bt_gaps runs = { NULL, 0, 0, 0, 0 };
	struct bt_fst_run *expected = NULL;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key first = { 0 };
	uint64_t end = chunk->logical + chunk->length;
	uint64_t position = chunk->logical;
	uint64_t used = 0;
	uint64_t free = 0;
	enum bt_space_kind kind;
	size_t i;
	int group_found = 0;
	enum btrfs_result error;

	if (list == NULL) {
		return BTRFS_CORRUPT;
	}
	kind = bt_space_kind_of(space, list);
	/* bt_space_groups verified the item of a separate block-group tree. */
	group_found = space->has_group_tree;
	if (index > 0) {
		first.objectid = fs->chunks[index - 1].logical + fs->chunks[index - 1].length;
	}
	bt_cursor_init(&cursor, fs, space->extents);
	error = bt_cursor_seek(&cursor, first, 0);
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid >= end) {
			break;
		}
		if (++space->items_scanned > space->item_limit) {
			error = BTRFS_CORRUPT;
			break;
		}
		if (record.key.objectid < chunk->logical) {
			if (bt_space_owned(&record)) {
				error = BTRFS_CORRUPT;
				break;
			}
		} else if (record.key.type == BT_BLOCK_GROUP_ITEM) {
			group = (const void *)record.data;
			if (space->has_group_tree || group_found ||
			    record.key.objectid != chunk->logical ||
			    record.key.offset != chunk->length || record.size != sizeof(*group) ||
			    bt_u64(group->used_bytes) != space->used[index]) {
				error = BTRFS_CORRUPT;
				break;
			}
			group_found = 1;
		} else if (record.key.type == BT_EXTENT_ITEM ||
		    record.key.type == BT_METADATA_ITEM) {
			error = bt_space_extent(space, chunk, &record, position);
			if (error == BTRFS_OK) {
				error = bt_space_gap(space, &runs, position, record.key.objectid);
			}
			if (error != BTRFS_OK) {
				break;
			}
			position = record.key.objectid +
			    (record.key.type == BT_METADATA_ITEM ? fs->info.node_size
								 : record.key.offset);
			used += position - record.key.objectid;
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_OK;
	}
	if (error == BTRFS_OK) {
		error = bt_space_gap(space, &runs, position, end);
	}
	if (error == BTRFS_OK && (!group_found || used != space->used[index])) {
		error = BTRFS_CORRUPT;
	}
	/* A free-space tree that disagrees with the extent tree is not propagated. */
	if (error == BTRFS_OK && space->has_free_space) {
		expected = fs->env.allocate(fs->env.context, (runs.count + 1) * sizeof(*expected));
		error = expected == NULL ? BTRFS_NO_MEMORY : BTRFS_OK;
		for (i = 0; error == BTRFS_OK && i < runs.count; i++) {
			expected[i] = (struct bt_fst_run){ runs.items[i].start, runs.items[i].end };
		}
		if (error == BTRFS_OK) {
			error =
			    bt_fst_verify_group(fs, space->free_space, chunk, expected, runs.count);
		}
		if (expected != NULL) {
			fs->env.release(
			    fs->env.context, expected, (runs.count + 1) * sizeof(*expected));
		}
	}
	if (error == BTRFS_OK) {
		error = bt_space_exclude(space, chunk, &runs);
	}
	if (error == BTRFS_OK) {
		bt_gaps_normalize(&runs);
		error = bt_gaps_splice(&fs->env, list, runs.items, runs.count);
	}
	for (i = 0; error == BTRFS_OK && i < runs.count; i++) {
		free += runs.items[i].end - runs.items[i].start;
		error = bt_space_log_load(space, index, runs.items[i].start, runs.items[i].end);
	}
	if (error == BTRFS_OK && runs.count == 0) {
		error = bt_space_log_load(space, index, chunk->logical, chunk->logical);
	}
	bt_gaps_release(&fs->env, &runs);
	if (error != BTRFS_OK) {
		return error;
	}
	space->unloaded_free[kind] -=
	    free < space->unloaded_free[kind] ? free : space->unloaded_free[kind];
	space->loaded[index] = 1;
	return BTRFS_OK;
}

/* Whether an unloaded chunk has no free bytes outside its stripes: a load
 * would give it no free range, and it stays full until a change loads it. */
static int
bt_space_full(const struct bt_space *space, size_t index)
{
	const struct bt_chunk *chunk = &space->fs->chunks[index];

	return space->used[index] >= chunk->length - bt_space_excluded(chunk);
}

/* Loads the lowest unloaded chunk of list's class when it lies below limit:
 * allocation never skips free space of a lower chunk, so its choices do not
 * depend on which chunks earlier transactions loaded. Full chunks hold no
 * free space and are passed over without loading. */
static enum btrfs_result
bt_space_load_below(struct bt_space *space, struct bt_gaps *list, uint64_t limit, int *loaded)
{
	const struct btrfs_fs *fs = space->fs;
	size_t *frontier = &space->frontier[bt_space_kind_of(space, list)];
	enum btrfs_result error;

	*loaded = 0;
	while (*frontier < fs->chunk_count &&
	    (bt_space_class(space, &fs->chunks[*frontier]) != list || space->loaded[*frontier] ||
		fs->chunks[*frontier].removed || bt_space_full(space, *frontier))) {
		(*frontier)++;
	}
	if (*frontier == fs->chunk_count || fs->chunks[*frontier].logical >= limit) {
		return BTRFS_OK;
	}
	error = bt_space_load_chunk(space, *frontier);
	*loaded = error == BTRFS_OK;
	return error;
}

/* Bytes of a chunk's logical range that superblock stripes cover: the same
 * ranges bt_space_exclude removes, of every copy, merged. */
static uint64_t
bt_space_excluded(const struct bt_chunk *chunk)
{
	struct bt_gap ranges[2 * BT_SUPER_MIRRORS];
	struct bt_gap swap;
	uint64_t physical;
	uint64_t covered = 0;
	uint64_t position = 0;
	size_t count = 0;
	size_t i;
	size_t j;
	unsigned mirror;
	unsigned copy;

	for (copy = 0; copy < chunk->mirrors; copy++) {
		for (mirror = 0; mirror < BT_SUPER_MIRRORS; mirror++) {
			physical = bt_super_offset(mirror) & ~(BT_STRIPE_LENGTH - 1);
			if (physical + BT_STRIPE_LENGTH <= chunk->physical[copy] ||
			    physical >= chunk->physical[copy] + chunk->length) {
				continue;
			}
			ranges[count].start =
			    physical > chunk->physical[copy] ? physical - chunk->physical[copy] : 0;
			ranges[count].end = physical + BT_STRIPE_LENGTH - chunk->physical[copy];
			if (ranges[count].end > chunk->length) {
				ranges[count].end = chunk->length;
			}
			count++;
		}
	}
	for (i = 1; i < count; i++) {
		for (j = i; j > 0 && ranges[j - 1].start > ranges[j].start; j--) {
			swap = ranges[j];
			ranges[j] = ranges[j - 1];
			ranges[j - 1] = swap;
		}
	}
	for (i = 0; i < count; i++) {
		if (ranges[i].start < position) {
			ranges[i].start = position;
		}
		if (ranges[i].end > ranges[i].start) {
			covered += ranges[i].end - ranges[i].start;
			position = ranges[i].end;
		}
	}
	return covered;
}

/* Free bytes chunk index holds outside its superblock stripes, given its
 * block-group total. */
static enum btrfs_result
bt_space_estimate(struct bt_space *space, size_t index, uint64_t *free)
{
	const struct bt_chunk *chunk = &space->fs->chunks[index];
	uint64_t room = chunk->length - bt_space_excluded(chunk);

	*free = 0;
	if (space->used[index] > room) {
		return BTRFS_CORRUPT;
	}
	*free = room - space->used[index];
	return BTRFS_OK;
}

/* Reads every block group's item: its total, and the free bytes it holds
 * until loaded. A block-group tree holds exactly one item per chunk, in chunk
 * order, and nothing else. Extent items are bounded by the room metadata and
 * system chunks have for item headers. System chunks load at once. */
static enum btrfs_result
bt_space_groups(struct bt_space *space)
{
	const struct btrfs_fs *fs = space->fs;
	const struct bt_chunk *chunk;
	const struct bt_disk_block_group *group;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key;
	uint64_t free = 0;
	size_t i;
	enum btrfs_result error = BTRFS_OK;

	for (i = 0; i < fs->chunk_count; i++) {
		if (fs->chunks[i].type & (BT_BLOCK_METADATA | BT_BLOCK_SYSTEM)) {
			space->item_limit += fs->chunks[i].length / sizeof(struct bt_disk_item);
		}
	}
	bt_cursor_init(&cursor, fs, space->has_group_tree ? space->groups : space->extents);
	for (i = 0; error == BTRFS_OK && i < fs->chunk_count; i++) {
		chunk = &fs->chunks[i];
		key = (struct bt_key){ chunk->logical, chunk->length, BT_BLOCK_GROUP_ITEM };
		if (!space->has_group_tree) {
			error = bt_cursor_seek(&cursor, key, 0);
		} else if (i == 0) {
			error = bt_cursor_seek(&cursor, (struct bt_key){ 0 }, 0);
		} else {
			error = bt_cursor_next(&cursor);
		}
		if (error == BTRFS_OK) {
			(void)bt_cursor_record(&cursor, &record);
			group = (const void *)record.data;
			if (bt_key_compare(record.key, key) != 0 || record.size != sizeof(*group) ||
			    bt_u64(group->flags) != chunk->type ||
			    bt_u64(group->chunk_objectid) != BT_FIRST_CHUNK_OBJECTID ||
			    bt_u64(group->used_bytes) > chunk->length) {
				error = BTRFS_CORRUPT;
			} else {
				space->used[i] = bt_u64(group->used_bytes);
			}
		}
		error = error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
		if (error == BTRFS_OK) {
			error = bt_space_estimate(space, i, &free);
		}
		if (error == BTRFS_OK && bt_space_class(space, chunk) == NULL) {
			error = BTRFS_CORRUPT;
		}
		if (error == BTRFS_OK) {
			space->unloaded_free[bt_space_kind_of(
			    space, bt_space_class(space, chunk))] += free;
		}
	}
	if (error == BTRFS_OK && space->has_group_tree) {
		error = fs->chunk_count == 0 ? bt_cursor_seek(&cursor, (struct bt_key){ 0 }, 0)
					     : bt_cursor_next(&cursor);
		/* An item past the last chunk's names no chunk. */
		if (error == BTRFS_OK) {
			error = BTRFS_CORRUPT;
		} else if (error == BTRFS_NOT_FOUND) {
			error = BTRFS_OK;
		}
	}
	bt_cursor_fini(&cursor);
	bt_cursor_init(&cursor, fs, space->extents);
	/* Nothing an extent or block group names lies beyond the last chunk. */
	if (error == BTRFS_OK && fs->chunk_count != 0) {
		chunk = &fs->chunks[fs->chunk_count - 1];
		key = (struct bt_key){ chunk->logical + chunk->length, 0, 0 };
		error = bt_cursor_seek(&cursor, key, 0);
		while (error == BTRFS_OK) {
			(void)bt_cursor_record(&cursor, &record);
			if (bt_space_owned(&record) || ++space->items_scanned > space->item_limit) {
				error = BTRFS_CORRUPT;
				break;
			}
			error = bt_cursor_next(&cursor);
		}
		error = error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
	}
	bt_cursor_fini(&cursor);
	for (i = 0; error == BTRFS_OK && i < fs->chunk_count; i++) {
		if (bt_space_class(space, &fs->chunks[i]) == &space->system) {
			error = bt_space_load_chunk(space, i);
		}
	}
	return error;
}

enum btrfs_result
bt_space_load_all(struct bt_space *space)
{
	size_t i;
	enum btrfs_result error = BTRFS_OK;

	for (i = 0; error == BTRFS_OK && i < space->fs->chunk_count; i++) {
		if (!space->loaded[i]) {
			error = bt_space_load_chunk(space, i);
		}
	}
	return error;
}

/* Takes length bytes, aligned, from the first device gap that holds them. */
static int
bt_space_take(struct bt_gaps *device, uint64_t length, uint64_t *physical)
{
	struct bt_gap *gap;
	uint64_t start;
	size_t i;

	for (i = 0; i < device->count; i++) {
		gap = &device->items[i];
		start = (gap->start + BT_CHUNK_ALIGN - 1) & ~(BT_CHUNK_ALIGN - 1);
		if (start >= gap->start && start <= gap->end && length <= gap->end - start) {
			/* Alignment padding stays unallocated and is simply not reused. */
			*physical = start;
			gap->start = start + length;
			return 1;
		}
	}
	return 0;
}

/* Free metadata nodes, counting chunks not loaded yet. */
static uint64_t
bt_space_metadata_free(const struct bt_space *space, uint64_t enough)
{
	uint64_t node = space->fs->info.node_size;
	uint64_t available = 0;
	size_t i;

	for (i = space->metadata.next; i < space->metadata.count && available < enough; i++) {
		available += (space->metadata.items[i].end - space->metadata.items[i].start) / node;
	}
	return available + space->unloaded_free[BT_SPACE_METADATA] / node;
}

/* Units of BT_CHUNK_ALIGN bytes of unallocated device space that chunk
 * stripes can take, gap by gap as bt_space_take aligns them; a chunk takes
 * whole units, one run per stripe. */
static uint64_t
bt_space_device_units(const struct bt_space *space)
{
	const struct bt_gap *gap;
	uint64_t start;
	uint64_t units = 0;
	size_t i;

	for (i = space->device.next; i < space->device.count; i++) {
		gap = &space->device.items[i];
		start = (gap->start + BT_CHUNK_ALIGN - 1) & ~(BT_CHUNK_ALIGN - 1);
		if (start >= gap->start && start < gap->end) {
			units += (gap->end - start) / BT_CHUNK_ALIGN;
		}
	}
	return units;
}

/* Device units data growth leaves unallocated so that metadata can still
 * grow to the held nodes: the shortfall in whole units for each copy. */
static uint64_t
bt_space_holdback(const struct bt_space *space)
{
	uint64_t free = bt_space_metadata_free(space, space->metadata_hold);
	uint64_t bytes;

	if (free >= space->metadata_hold) {
		return 0;
	}
	bytes = (space->metadata_hold - free) * space->fs->info.node_size;
	return (bytes + BT_CHUNK_ALIGN - 1) / BT_CHUNK_ALIGN * space->metadata_copies;
}

/* Bytes new chunks with copies stripes can still take, leaving held units. */
static uint64_t
bt_space_growable(const struct bt_space *space, unsigned copies, uint64_t held)
{
	uint64_t units = bt_space_device_units(space);

	units = units > held ? units - held : 0;
	return units / copies * BT_CHUNK_ALIGN;
}

/* Free system space this transaction can still hand out. */
static uint64_t
bt_space_system_free(const struct bt_space *space)
{
	uint64_t free = 0;
	size_t i;

	for (i = space->system.next; i < space->system.count; i++) {
		free += space->system.items[i].end - space->system.items[i].start;
	}
	return free;
}

enum btrfs_result
bt_space_check_system(struct bt_space *space)
{
	enum btrfs_result error = BTRFS_OK;

	/* As Linux, a failed system chunk is ignored: the chunk tree may not need
	 * all of the reserve. */
	if (bt_space_system_free(space) <
	    (uint64_t)BT_SYSTEM_RESERVE_NODES * space->fs->info.node_size) {
		error = bt_space_grow(space, BT_BLOCK_SYSTEM, 0);
	}
	return error == BTRFS_NO_SPACE ? BTRFS_OK : error;
}

/* Creates a chunk of the given class from unallocated device space, in memory
 * only: the chunk map, block-group accounting and gaps grow now, and the
 * transaction publishes chunk, device-extent, device, block-group and
 * free-space items (and a system chunk's superblock array entry) before
 * writing. Profiles copy an existing chunk of the class. Undone with the whole
 * transaction. */
enum btrfs_result
bt_space_grow(struct bt_space *space, uint64_t kind, uint64_t minimum)
{
	struct btrfs_fs *fs = space->fs;
	struct bt_gaps saved;
	struct bt_chunk *chunk;
	struct bt_gap *copy;
	uint64_t type = 0;
	uint64_t logical = 0;
	uint64_t length;
	uint64_t limit;
	uint64_t physical[BT_MAX_MIRRORS];
	uint64_t allowed = UINT64_MAX;
	size_t i;
	unsigned stripes;
	unsigned stripe;
	int fits;
	enum btrfs_result error;

	if (!space->growth || fs->chunk_count >= fs->chunk_capacity ||
	    fs->chunk_count >= space->used_capacity || fs->chunk_count == BT_MAX_CHUNKS) {
		return BTRFS_NO_SPACE;
	}
	/* Mixed groups grow for data and metadata alike, sized as Linux sizes any
	 * type with the data bit (calc_chunk_size). */
	if (space->mixed && kind != BT_BLOCK_SYSTEM) {
		kind = BT_BLOCK_DATA | BT_BLOCK_METADATA;
	}
	limit = (kind & BT_BLOCK_DATA) != 0 ? BT_CHUNK_DATA_MAX
	    : kind == BT_BLOCK_SYSTEM	    ? BT_CHUNK_SYSTEM_MAX
					    : BT_CHUNK_METADATA_MAX;
	/* The chunk item and device item updates need system space first. */
	if (kind != BT_BLOCK_SYSTEM) {
		error = bt_space_check_system(space);
		if (error != BTRFS_OK) {
			return error;
		}
	}
	error = bt_gaps_own(&fs->env, &space->device);
	if (error != BTRFS_OK) {
		return error;
	}
	saved = space->device;
	for (i = 0; i < fs->chunk_count; i++) {
		if ((fs->chunks[i].type & (BT_BLOCK_DATA | BT_BLOCK_METADATA | BT_BLOCK_SYSTEM)) ==
			kind &&
		    !fs->chunks[i].removed && type == 0) {
			type = fs->chunks[i].type;
		}
		if (fs->chunks[i].logical + fs->chunks[i].length > logical) {
			logical = fs->chunks[i].logical + fs->chunks[i].length;
		}
	}
	if (type == 0 || logical > UINT64_MAX - limit - BT_CHUNK_ALIGN) {
		return BTRFS_NO_SPACE;
	}
	logical = (logical + BT_CHUNK_ALIGN - 1) & ~(BT_CHUNK_ALIGN - 1);
	stripes = type & BT_BLOCK_DUP ? 2 : 1;
	length = fs->device_size / 10 & ~(BT_CHUNK_ALIGN - 1);
	length = length > limit ? limit : length;
	length = length < BT_CHUNK_ALIGN ? BT_CHUNK_ALIGN : length;
	minimum = (minimum + BT_CHUNK_ALIGN - 1) & ~(BT_CHUNK_ALIGN - 1);
	length = length < minimum ? minimum : length;
	/* Data leaves the device space metadata would need to grow. */
	if (kind == BT_BLOCK_DATA) {
		allowed = bt_space_growable(space, stripes, bt_space_holdback(space));
	}
	/* Device gaps are copied so a failed attempt leaves them untouched. */
	copy = fs->env.allocate(fs->env.context, (saved.count + 1) * sizeof(*copy));
	if (copy == NULL) {
		return BTRFS_NO_MEMORY;
	}
	for (fits = 0; !fits && length >= minimum && length >= BT_CHUNK_ALIGN;
	    length = (length / 2) & ~(BT_CHUNK_ALIGN - 1)) {
		if (length > allowed) {
			continue;
		}
		bt_copy(copy, saved.items, saved.count * sizeof(*copy));
		space->device.items = copy;
		fits = 1;
		for (stripe = 0; fits && stripe < stripes; stripe++) {
			fits = bt_space_take(&space->device, length, &physical[stripe]);
		}
		if (fits) {
			break;
		}
	}
	space->device.items = saved.items;
	if (!fits) {
		fs->env.release(fs->env.context, copy, (saved.count + 1) * sizeof(*copy));
		return BTRFS_NO_SPACE;
	}
	bt_copy(saved.items, copy, saved.count * sizeof(*copy));
	fs->env.release(fs->env.context, copy, (saved.count + 1) * sizeof(*copy));
	chunk = &fs->chunks[fs->chunk_count];
	bt_zero(chunk, sizeof(*chunk));
	chunk->logical = logical;
	chunk->length = length;
	chunk->type = type;
	chunk->mirrors = (uint8_t)stripes;
	chunk->confirmed = 1;
	for (stripe = 0; stripe < stripes; stripe++) {
		chunk->physical[stripe] = physical[stripe];
	}
	space->used[fs->chunk_count] = 0;
	space->loaded[fs->chunk_count] = 1;
	fs->chunk_count++;
	error = bt_space_gap(space, bt_space_class(space, chunk), logical, logical + length);
	return error == BTRFS_OK ? bt_space_exclude_chunk(space, fs->chunk_count - 1) : error;
}

static enum btrfs_result
bt_space_reserve(void *context, uint64_t owner, uint8_t level, uint64_t *logical)
{
	struct bt_space *space = context;
	struct bt_gaps *list = owner == BT_CHUNK_TREE ? &space->system : &space->metadata;
	struct bt_gap *gap;
	uint64_t start;
	uint64_t size = space->fs->info.node_size;
	int loaded;
	enum btrfs_result error;

	(void)level;
	error = bt_gaps_own(&space->fs->env, list);
	if (error != BTRFS_OK) {
		return error;
	}
	for (;;) {
		error = bt_space_load_below(space, list,
		    list->next < list->count ? list->items[list->next].start : UINT64_MAX, &loaded);
		if (error != BTRFS_OK) {
			return error;
		}
		if (loaded) {
			continue;
		}
		if (list->next == list->count) {
			error = bt_space_grow(space,
			    owner == BT_CHUNK_TREE ? BT_BLOCK_SYSTEM : BT_BLOCK_METADATA, size);
			if (error != BTRFS_OK) {
				return error;
			}
			continue;
		}
		gap = &list->items[list->next];
		if (gap->start <= UINT64_MAX - (size - 1)) {
			start = (gap->start + size - 1) & ~(size - 1);
			if (start <= gap->end && size <= gap->end - start) {
				*logical = start;
				gap->start = start + size;
				return BTRFS_OK;
			}
		}
		list->next++;
	}
}

/* Takes the first free range of an owned list that holds length, every chunk
 * below it loaded; NOT_FOUND once every chunk of the class is loaded and no
 * range is long enough. Each pass loads a chunk or ends. */
static enum btrfs_result
bt_space_fit(struct bt_space *space, struct bt_gaps *list, uint64_t length, uint64_t *logical)
{
	struct bt_gap *gap;
	size_t i;
	int loaded = 0;
	enum btrfs_result error;

	for (;;) {
		for (i = list->next; !loaded; i++) {
			error = bt_space_load_below(space, list,
			    i < list->count ? list->items[i].start : UINT64_MAX, &loaded);
			if (error != BTRFS_OK) {
				return error;
			}
			if (loaded || i == list->count) {
				break;
			}
			gap = &list->items[i];
			if (gap->end - gap->start >= length) {
				*logical = gap->start;
				gap->start += length;
				return BTRFS_OK;
			}
		}
		if (!loaded) {
			return BTRFS_NOT_FOUND;
		}
		loaded = 0;
	}
}

enum btrfs_result
bt_space_reserve_data(struct bt_space *space, uint64_t length, uint64_t *logical, uint64_t *size)
{
	struct bt_gaps *list = bt_space_data_list(space);
	struct bt_gap *gap;
	uint64_t sector = space->fs->info.sector_size;
	int loaded;
	enum btrfs_result error;

	if (length == 0 || length % sector != 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_gaps_own(&space->fs->env, list);
	if (error != BTRFS_OK) {
		return error;
	}
	/* Metadata leaves node-sized holes in mixed groups, so data first takes a
	 * range that holds all of it, then a new group, as Linux's
	 * find_free_extent does before it splits a request. */
	if (space->mixed) {
		error = bt_space_fit(space, list, length, logical);
		if (error == BTRFS_NOT_FOUND) {
			error = bt_space_grow(space, BT_BLOCK_DATA, length);
			if (error == BTRFS_OK) {
				error = bt_space_fit(space, list, length, logical);
			} else if (error == BTRFS_NO_SPACE) {
				error = BTRFS_NOT_FOUND;
			}
		}
		if (error == BTRFS_OK) {
			*size = length;
			return BTRFS_OK;
		}
		if (error != BTRFS_NOT_FOUND) {
			return error;
		}
	}
	for (;;) {
		error = bt_space_load_below(space, list,
		    list->next < list->count ? list->items[list->next].start : UINT64_MAX, &loaded);
		if (error != BTRFS_OK) {
			return error;
		}
		if (loaded) {
			continue;
		}
		if (list->next == list->count) {
			/* One chunk for the whole range, or else the largest one that
			 * fits: the caller takes the range in pieces. */
			error = bt_space_grow(space, BT_BLOCK_DATA, length);
			if (error == BTRFS_NO_SPACE) {
				error = bt_space_grow(space, BT_BLOCK_DATA, sector);
			}
			if (error != BTRFS_OK) {
				return error;
			}
			continue;
		}
		gap = &list->items[list->next];
		if (gap->end - gap->start >= sector) {
			*logical = gap->start;
			*size = gap->end - gap->start < length ? gap->end - gap->start : length;
			*size -= *size % sector;
			gap->start += *size;
			return BTRFS_OK;
		}
		list->next++;
	}
}

void
bt_space_hold_metadata(struct bt_space *space, uint64_t nodes)
{
	space->metadata_hold = nodes;
}

int
bt_space_data_available(const struct bt_space *space, uint64_t length)
{
	const struct bt_gaps *list = space->mixed ? &space->metadata : &space->data;
	uint64_t node = space->fs->info.node_size;
	uint64_t available = 0;
	size_t i;

	/* In mixed groups data leaves the held metadata nodes free. */
	if (space->mixed) {
		if (space->metadata_hold > (UINT64_MAX - length) / node) {
			return 0;
		}
		length += space->metadata_hold * node;
	}
	for (i = list->next; i < list->count && available < length; i++) {
		available += list->items[i].end - list->items[i].start;
	}
	available += space->unloaded_free[space->mixed ? BT_SPACE_METADATA : BT_SPACE_DATA];
	if (available >= length) {
		return 1;
	}
	if (!space->growth) {
		return 0;
	}
	return bt_space_growable(space, space->data_copies,
		   space->mixed ? 0 : bt_space_holdback(space)) >= length - available;
}

int
bt_space_metadata_available(const struct bt_space *space, uint64_t nodes)
{
	uint64_t available = bt_space_metadata_free(space, nodes);

	if (available >= nodes) {
		return 1;
	}
	if (!space->growth) {
		return 0;
	}
	return bt_space_growable(space, space->metadata_copies, 0) / space->fs->info.node_size >=
	    nodes - available;
}

enum btrfs_result
bt_space_reserve_exact(struct bt_space *space, uint64_t length, uint64_t *logical)
{
	struct bt_gaps *list = bt_space_data_list(space);
	uint64_t sector = space->fs->info.sector_size;
	enum btrfs_result error;

	if (length == 0 || length % sector != 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_gaps_own(&space->fs->env, list);
	if (error != BTRFS_OK) {
		return error;
	}
	for (;;) {
		error = bt_space_fit(space, list, length, logical);
		if (error != BTRFS_NOT_FOUND) {
			return error;
		}
		error = bt_space_grow(space, BT_BLOCK_DATA, length);
		if (error != BTRFS_OK) {
			return error;
		}
	}
}

static void
bt_space_release(void *context, uint64_t address)
{
	/* All reservations belong to this context. Keeping released slots pinned
	 * prevents reuse by the same transaction; destroy releases them together. */
	(void)context;
	(void)address;
}

/* Allocates the per-chunk usage for every chunk the table can hold and
 * records the copies new chunks of each class take. */
static enum btrfs_result
bt_space_counts(struct bt_space *space)
{
	const struct btrfs_fs *fs = space->fs;
	size_t i;

	space->used_capacity = fs->chunk_capacity;
	space->used =
	    fs->env.allocate(fs->env.context, space->used_capacity * sizeof(*space->used));
	space->loaded = fs->env.allocate(fs->env.context, space->used_capacity);
	if (space->used == NULL || space->loaded == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(space->loaded, space->used_capacity);
	space->data_copies = 1;
	space->metadata_copies = 1;
	space->mixed = (fs->info.incompat_features & BT_FEATURE_MIXED_GROUPS) != 0;
	for (i = 0; i < fs->chunk_count; i++) {
		/* As Linux's read_one_block_group: a mixed group needs the feature. */
		if (!space->mixed && (fs->chunks[i].type & BT_BLOCK_DATA) != 0 &&
		    (fs->chunks[i].type & BT_BLOCK_METADATA) != 0) {
			return BTRFS_CORRUPT;
		}
		if ((fs->chunks[i].type & BT_BLOCK_DUP) != 0 &&
		    (fs->chunks[i].type & BT_BLOCK_DATA) != 0) {
			space->data_copies = 2;
		}
		if ((fs->chunks[i].type & BT_BLOCK_DUP) != 0 &&
		    (fs->chunks[i].type & BT_BLOCK_METADATA) != 0) {
			space->metadata_copies = 2;
		}
	}
	return BTRFS_OK;
}

/* Logical separation alone does not authorize writing: no two stripes of any
 * chunks may share physical space. Sorted stripes overlap only neighbours. */
static enum btrfs_result
bt_space_aliases(const struct btrfs_fs *fs)
{
	struct bt_gaps stripes = { NULL, 0, 0, 0, 0 };
	size_t i;
	unsigned stripe;
	enum btrfs_result error = BTRFS_OK;

	stripes.capacity = 2 * fs->chunk_count + 1;
	stripes.items =
	    fs->env.allocate(fs->env.context, stripes.capacity * sizeof(*stripes.items));
	if (stripes.items == NULL) {
		return BTRFS_NO_MEMORY;
	}
	for (i = 0; i < fs->chunk_count; i++) {
		for (stripe = 0; stripe < fs->chunks[i].mirrors; stripe++) {
			stripes.items[stripes.count++] =
			    (struct bt_gap){ fs->chunks[i].physical[stripe],
				    fs->chunks[i].physical[stripe] + fs->chunks[i].length };
		}
	}
	bt_gaps_normalize(&stripes);
	for (i = 1; i < stripes.count; i++) {
		if (stripes.items[i].start < stripes.items[i - 1].end) {
			error = BTRFS_CORRUPT;
		}
	}
	bt_gaps_release(&fs->env, &stripes);
	return error;
}

/* The block-group tree, when the filesystem has one, holds the block-group
 * items instead of the extent tree. */
static enum btrfs_result
bt_space_group_tree(struct bt_space *space)
{
	enum btrfs_result error;

	if ((space->fs->info.readonly_features & BT_COMPAT_RO_BLOCK_GROUP_TREE) == 0) {
		return BTRFS_OK;
	}
	error = bt_find_root(space->fs, BT_BLOCK_GROUP_TREE, &space->groups);
	space->has_group_tree = error == BTRFS_OK;
	return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
}

enum btrfs_result
bt_space_create(struct btrfs_fs *fs, struct bt_root extent_root, const struct bt_root *free_space,
    size_t node_limit, struct bt_space **result)
{
	struct bt_space *space;
	enum btrfs_result error;

	*result = NULL;
	error = bt_space_aliases(fs);
	if (error != BTRFS_OK) {
		return error;
	}
	space = fs->env.allocate(fs->env.context, sizeof(*space));
	if (space == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(space, sizeof(*space));
	space->fs = fs;
	space->node_limit = node_limit;
	space->original_chunks = fs->chunk_count;
	space->extents = extent_root;
	if (free_space != NULL) {
		space->free_space = *free_space;
		space->has_free_space = 1;
	}
	error = bt_space_group_tree(space);
	if (error == BTRFS_OK) {
		error = bt_space_counts(space);
	}
	if (error == BTRFS_OK) {
		error = bt_space_groups(space);
	}
	if (error != BTRFS_OK) {
		bt_space_destroy(space);
		return error;
	}
	*result = space;
	return BTRFS_OK;
}

static struct bt_chunk *
bt_space_chunk_at(struct btrfs_fs *fs, uint64_t logical)
{
	size_t low = 0;
	size_t high = fs->chunk_count;
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (fs->chunks[middle].logical < logical) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return low < fs->chunk_count && fs->chunks[low].logical == logical ? &fs->chunks[low]
									   : NULL;
}

enum btrfs_result
bt_space_devices(struct bt_space *space, struct bt_root chunk_tree, struct bt_root device_tree)
{
	struct btrfs_fs *fs = space->fs;
	const struct bt_disk_device *device;
	const struct bt_disk_dev_extent *extent;
	struct bt_chunk *chunk;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = {
		.objectid = BT_DEV_ITEMS_OBJECTID, .type = BT_DEV_ITEM, .offset = fs->device_id
	};
	uint8_t *seen;
	uint64_t total = 0;
	uint64_t used = 0;
	uint64_t position = BT_DEVICE_RESERVED;
	uint64_t sum = 0;
	size_t i;
	unsigned stripe;
	enum btrfs_result error;

	bt_cursor_init(&cursor, fs, chunk_tree);
	error = bt_cursor_seek(&cursor, key, 0);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		device = (const void *)record.data;
		if (bt_key_compare(record.key, key) != 0 || record.size != sizeof(*device) ||
		    bt_u64(device->id) != fs->device_id ||
		    !bt_equal(device->uuid, fs->device_uuid, BTRFS_UUID_SIZE)) {
			error = BTRFS_CORRUPT;
		} else {
			total = bt_u64(device->total_bytes);
			used = bt_u64(device->used_bytes);
			error = total != fs->device_size || used > total ? BTRFS_CORRUPT : BTRFS_OK;
		}
	}
	bt_cursor_fini(&cursor);
	if (error != BTRFS_OK) {
		return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
	}
	seen = fs->env.allocate(fs->env.context, fs->chunk_count);
	if (seen == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(seen, fs->chunk_count);
	/* Every device extent maps exactly one chunk stripe, in ascending order. */
	key = (struct bt_key){ .objectid = fs->device_id, .type = BT_DEV_EXTENT };
	bt_cursor_init(&cursor, fs, device_tree);
	error = bt_cursor_seek(&cursor, key, 0);
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != fs->device_id || record.key.type != BT_DEV_EXTENT) {
			break;
		}
		extent = (const void *)record.data;
		chunk = record.size == sizeof(*extent)
		    ? bt_space_chunk_at(fs, bt_u64(extent->chunk_offset))
		    : NULL;
		if (chunk == NULL || bt_u64(extent->chunk_tree) != BT_CHUNK_TREE ||
		    bt_u64(extent->chunk_objectid) != BT_FIRST_CHUNK_OBJECTID ||
		    bt_u64(extent->length) != chunk->length || record.key.offset < position ||
		    chunk->length > total - record.key.offset) {
			error = BTRFS_CORRUPT;
			break;
		}
		for (stripe = 0; stripe < chunk->mirrors; stripe++) {
			if (chunk->physical[stripe] == record.key.offset &&
			    !(seen[chunk - fs->chunks] & (1U << stripe))) {
				seen[chunk - fs->chunks] |= (uint8_t)(1U << stripe);
				break;
			}
		}
		if (stripe == chunk->mirrors) {
			error = BTRFS_CORRUPT;
			break;
		}
		error = bt_space_gap(space, &space->device, position, record.key.offset);
		position = record.key.offset + chunk->length;
		sum += chunk->length;
		if (error == BTRFS_OK) {
			error = bt_cursor_next(&cursor);
		}
	}
	bt_cursor_fini(&cursor);
	if (error == BTRFS_NOT_FOUND || error == BTRFS_OK) {
		error = bt_space_gap(space, &space->device, position, total);
	}
	for (i = 0; error == BTRFS_OK && i < fs->chunk_count; i++) {
		if (seen[i] != (1U << fs->chunks[i].mirrors) - 1) {
			error = BTRFS_CORRUPT;
		}
	}
	if (error == BTRFS_OK && sum != used) {
		error = BTRFS_CORRUPT;
	}
	fs->env.release(fs->env.context, seen, fs->chunk_count);
	space->growth = error == BTRFS_OK;
	return error;
}

size_t
bt_space_original_chunks(const struct bt_space *space)
{
	return space->original_chunks;
}

enum btrfs_result
bt_space_unused(struct bt_space *space, size_t chunk, int *unused)
{
	const struct bt_chunk *group = &space->fs->chunks[chunk];
	const struct bt_gaps *list = bt_space_class(space, group);
	size_t i;
	enum btrfs_result error;

	*unused = 0;
	if (chunk >= space->original_chunks || group->removed || space->used[chunk] != 0 ||
	    list == NULL) {
		return BTRFS_OK;
	}
	if (!space->loaded[chunk]) {
		error = bt_space_load_chunk(space, chunk);
		if (error != BTRFS_OK) {
			return error;
		}
	}
	/* Gaps are never returned, so an untouched whole-group gap means this
	 * transaction neither allocated nor freed anything there. */
	for (i = list->next; i < list->count; i++) {
		if (list->items[i].start == group->logical &&
		    list->items[i].end == group->logical + group->length) {
			*unused = 1;
		}
	}
	return BTRFS_OK;
}

enum btrfs_result
bt_space_retire(struct bt_space *space, size_t chunk)
{
	struct bt_chunk *group = &space->fs->chunks[chunk];
	struct bt_gaps *list = bt_space_class(space, group);
	size_t i;
	enum btrfs_result error;

	error = bt_gaps_own(&space->fs->env, list);
	if (error != BTRFS_OK) {
		return error;
	}
	for (i = list->next; i < list->count; i++) {
		if (list->items[i].start == group->logical) {
			list->items[i].start = list->items[i].end;
		}
	}
	group->removed = 1;
	return BTRFS_OK;
}

void
bt_space_allocator(struct bt_space *space, struct bt_mutation_allocator *allocator)
{
	*allocator = (struct bt_mutation_allocator){ space, bt_space_reserve, bt_space_release,
		space->node_limit };
}

/* Every allocation and release is logged in order for free-space maintenance. */
static enum btrfs_result
bt_space_log(struct bt_space *space, size_t chunk, uint64_t address, uint64_t size, int allocate)
{
	const struct btrfs_environment *env = &space->fs->env;
	struct bt_space_change *grown;
	size_t capacity;

	if (space->change_count == space->change_capacity) {
		if (space->change_capacity == BT_SPACE_MAX_CHANGES) {
			return BTRFS_UNSUPPORTED;
		}
		capacity = space->change_capacity == 0 ? 256 : space->change_capacity * 2;
		grown = env->allocate(env->context, capacity * sizeof(*grown));
		if (grown == NULL) {
			return BTRFS_NO_MEMORY;
		}
		if (space->changes != NULL) {
			bt_copy(grown, space->changes, space->change_count * sizeof(*grown));
			env->release(
			    env->context, space->changes, space->change_capacity * sizeof(*grown));
		}
		space->changes = grown;
		space->change_capacity = capacity;
	}
	space->changes[space->change_count++] =
	    (struct bt_space_change){ address, size, chunk, allocate };
	return BTRFS_OK;
}

size_t
bt_space_change_count(const struct bt_space *space)
{
	return space->change_count;
}

const struct bt_space_change *
bt_space_change(const struct bt_space *space, size_t index)
{
	return &space->changes[index];
}

static int
bt_space_change_after(const struct bt_space_change *a, const struct bt_space_change *b)
{
	return a->chunk != b->chunk ? a->chunk > b->chunk
	    : a->start != b->start  ? a->start > b->start
				    : a->length > b->length;
}

static void
bt_space_change_sift(struct bt_space_change *changes, size_t root, size_t count)
{
	struct bt_space_change swap;
	size_t child;

	for (;;) {
		child = root * 2 + 1;
		if (child >= count) {
			return;
		}
		if (child + 1 < count &&
		    bt_space_change_after(&changes[child + 1], &changes[child])) {
			child++;
		}
		if (!bt_space_change_after(&changes[child], &changes[root])) {
			return;
		}
		swap = changes[root];
		changes[root] = changes[child];
		changes[child] = swap;
		root = child;
	}
}

enum btrfs_result
bt_space_coalesce(struct bt_space_change *changes, size_t *count)
{
	struct bt_space_change change;
	struct bt_space_change previous = { 0 };
	struct bt_space_change *last;
	size_t i;
	size_t kept = 0;
	size_t total = *count;

	if (total > BT_SPACE_MAX_CHANGES) {
		return BTRFS_UNSUPPORTED;
	}
	/* Heap sort: O(n log n), constant extra storage, n bounded by the log. */
	for (i = total / 2; i != 0; i--) {
		bt_space_change_sift(changes, i - 1, total);
	}
	for (i = total; i > 1; i--) {
		change = changes[0];
		changes[0] = changes[i - 1];
		changes[i - 1] = change;
		bt_space_change_sift(changes, 0, i - 1);
	}
	for (i = 0; i < total; i++) {
		change = changes[i];
		if (change.length == 0 || change.length > UINT64_MAX - change.start ||
		    (i != 0 && change.chunk == previous.chunk &&
			change.start < previous.start + previous.length)) {
			return BTRFS_CORRUPT;
		}
		previous = change;
		if (i + 1 < total && change.chunk == changes[i + 1].chunk &&
		    change.start == changes[i + 1].start &&
		    change.length == changes[i + 1].length) {
			if (!!change.allocate == !!changes[i + 1].allocate) {
				return BTRFS_CORRUPT;
			}
			i++;
			continue;
		}
		last = kept == 0 ? NULL : &changes[kept - 1];
		if (last != NULL && last->chunk == change.chunk &&
		    !!last->allocate == !!change.allocate &&
		    last->start + last->length == change.start) {
			last->length += change.length;
		} else {
			changes[kept++] = change;
		}
	}
	*count = kept;
	return BTRFS_OK;
}

enum btrfs_result
bt_space_change_used(struct bt_space *space, uint64_t address, uint64_t size, int allocate)
{
	const struct bt_chunk *chunk;
	size_t i = bt_chunk_containing(space->fs, address);
	enum btrfs_result error;

	if (i == space->fs->chunk_count) {
		return BTRFS_CORRUPT;
	}
	/* A chunk's accounting changes only once its extents are verified. */
	if (!space->loaded[i]) {
		error = bt_space_load_chunk(space, i);
		if (error != BTRFS_OK) {
			return error;
		}
	}
	chunk = &space->fs->chunks[i];
	if (size > chunk->logical + chunk->length - address ||
	    (allocate ? size > chunk->length - space->used[i] : size > space->used[i])) {
		return BTRFS_CORRUPT;
	}
	error = bt_space_log(space, i, address, size, allocate);
	if (error == BTRFS_OK) {
		space->used[i] = allocate ? space->used[i] + size : space->used[i] - size;
	}
	return error;
}

enum btrfs_result
bt_space_withhold(struct bt_space *space, uint64_t logical, uint64_t length)
{
	const struct bt_chunk *chunk;
	struct bt_gaps *list;
	size_t i = bt_chunk_containing(space->fs, logical);
	enum btrfs_result error = BTRFS_OK;

	if (i == space->fs->chunk_count) {
		return BTRFS_CORRUPT;
	}
	chunk = &space->fs->chunks[i];
	list = bt_space_class(space, chunk);
	if (list == NULL || length == 0 || length > chunk->logical + chunk->length - logical) {
		return BTRFS_CORRUPT;
	}
	/* Only a loaded chunk tells free from used bytes, as Linux caches the
	 * block group before it excludes a logged extent. */
	if (!space->loaded[i]) {
		error = bt_space_load_chunk(space, i);
	}
	return error == BTRFS_OK ? bt_space_carve(space, list, logical, logical + length) : error;
}

uint64_t
bt_space_used(const struct bt_space *space, size_t chunk)
{
	return space->used[chunk];
}

void
bt_space_destroy(struct bt_space *space)
{
	const struct btrfs_environment *env;

	if (space == NULL) {
		return;
	}
	env = &space->fs->env;
	if (space->metadata.items != NULL && !space->metadata.borrowed) {
		env->release(env->context, space->metadata.items,
		    space->metadata.capacity * sizeof(*space->metadata.items));
	}
	if (space->data.items != NULL && !space->data.borrowed) {
		env->release(env->context, space->data.items,
		    space->data.capacity * sizeof(*space->data.items));
	}
	if (space->used != NULL) {
		env->release(
		    env->context, space->used, space->used_capacity * sizeof(*space->used));
	}
	if (space->system.items != NULL && !space->system.borrowed) {
		env->release(env->context, space->system.items,
		    space->system.capacity * sizeof(*space->system.items));
	}
	if (space->device.items != NULL && !space->device.borrowed) {
		env->release(env->context, space->device.items,
		    space->device.capacity * sizeof(*space->device.items));
	}
	if (space->changes != NULL) {
		env->release(
		    env->context, space->changes, space->change_capacity * sizeof(*space->changes));
	}
	if (space->loaded != NULL) {
		env->release(env->context, space->loaded, space->used_capacity);
	}
	if (space->loads != NULL) {
		env->release(
		    env->context, space->loads, space->load_capacity * sizeof(*space->loads));
	}
	env->release(env->context, space, sizeof(*space));
}

/* Gap lists in canonical form: sorted by start, without empty gaps. */
static void
bt_gaps_sift(struct bt_gap *items, size_t root, size_t count)
{
	struct bt_gap swap;
	size_t child;

	while ((child = 2 * root + 1) < count) {
		if (child + 1 < count && items[child + 1].start > items[child].start) {
			child++;
		}
		if (items[root].start >= items[child].start) {
			return;
		}
		swap = items[root];
		items[root] = items[child];
		items[child] = swap;
		root = child;
	}
}

static void
bt_gaps_normalize(struct bt_gaps *list)
{
	struct bt_gap swap;
	size_t i;
	size_t kept = 0;

	for (i = 0; i < list->count; i++) {
		if (list->items[i].start < list->items[i].end) {
			list->items[kept++] = list->items[i];
		}
	}
	list->count = kept;
	list->next = 0;
	for (i = kept / 2; i-- != 0;) {
		bt_gaps_sift(list->items, i, kept);
	}
	for (i = kept; i > 1; i--) {
		swap = list->items[0];
		list->items[0] = list->items[i - 1];
		list->items[i - 1] = swap;
		bt_gaps_sift(list->items, 0, i - 1);
	}
}

static void
bt_gaps_release(const struct btrfs_environment *env, struct bt_gaps *list)
{
	if (list->items != NULL) {
		env->release(env->context, list->items, list->capacity * sizeof(*list->items));
	}
	bt_zero(list, sizeof(*list));
}

static enum btrfs_result
bt_gaps_copy(
    const struct btrfs_environment *env, struct bt_gaps *target, const struct bt_gaps *source)
{
	bt_zero(target, sizeof(*target));
	if (source->count == 0) {
		return BTRFS_OK;
	}
	target->items = env->allocate(env->context, source->count * sizeof(*target->items));
	if (target->items == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_copy(target->items, source->items, source->count * sizeof(*target->items));
	target->count = target->capacity = source->count;
	return BTRFS_OK;
}

/* Makes room for one more gap at index, shifting the rest. */
static enum btrfs_result
bt_gaps_open(const struct btrfs_environment *env, struct bt_gaps *list, size_t index)
{
	struct bt_gap *grown;
	size_t capacity;

	if (list->count == BT_SPACE_MAX_GAPS) {
		return BTRFS_UNSUPPORTED;
	}
	if (list->count == list->capacity) {
		capacity = list->capacity == 0 ? 256 : list->capacity * 2;
		grown = env->allocate(env->context, capacity * sizeof(*grown));
		if (grown == NULL) {
			return BTRFS_NO_MEMORY;
		}
		if (list->items != NULL) {
			bt_copy(grown, list->items, list->count * sizeof(*grown));
			env->release(env->context, list->items, list->capacity * sizeof(*grown));
		}
		list->items = grown;
		list->capacity = capacity;
	}
	bt_move(&list->items[index + 1], &list->items[index],
	    (list->count - index) * sizeof(*list->items));
	list->count++;
	return BTRFS_OK;
}

/* The index of the first gap ending after position. */
static size_t
bt_gaps_find(const struct bt_gaps *list, uint64_t position)
{
	size_t low = 0;
	size_t high = list->count;
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (list->items[middle].end <= position) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return low;
}

/* Removes [start, end), which one gap must hold. */
static enum btrfs_result
bt_gaps_carve(
    const struct btrfs_environment *env, struct bt_gaps *list, uint64_t start, uint64_t end)
{
	size_t index = bt_gaps_find(list, start);
	struct bt_gap *gap;
	uint64_t tail;
	enum btrfs_result error;

	if (index == list->count || list->items[index].start > start ||
	    list->items[index].end < end) {
		return BTRFS_CORRUPT;
	}
	gap = &list->items[index];
	tail = gap->end;
	if (gap->start == start && tail == end) {
		bt_move(gap, gap + 1, (list->count - index - 1) * sizeof(*gap));
		list->count--;
		return BTRFS_OK;
	}
	if (gap->start == start) {
		gap->start = end;
		return BTRFS_OK;
	}
	gap->end = start;
	if (tail == end) {
		return BTRFS_OK;
	}
	error = bt_gaps_open(env, list, index + 1);
	if (error == BTRFS_OK) {
		list->items[index + 1] = (struct bt_gap){ end, tail };
	}
	return error;
}

/* Adds [start, end) of the chunk [low, high), merging with neighbours in the
 * same chunk only; overlap with a free range is corruption. */
static enum btrfs_result
bt_gaps_insert(const struct btrfs_environment *env, struct bt_gaps *list, uint64_t start,
    uint64_t end, uint64_t low, uint64_t high)
{
	size_t index = bt_gaps_find(list, start);
	int before;
	int after;
	enum btrfs_result error;

	if (index < list->count && list->items[index].start < end) {
		return BTRFS_CORRUPT;
	}
	before =
	    index > 0 && list->items[index - 1].end == start && list->items[index - 1].start >= low;
	after = index < list->count && list->items[index].start == end &&
	    list->items[index].end <= high;
	if (before && after) {
		list->items[index - 1].end = list->items[index].end;
		bt_move(&list->items[index], &list->items[index + 1],
		    (list->count - index - 1) * sizeof(*list->items));
		list->count--;
		return BTRFS_OK;
	}
	if (before) {
		list->items[index - 1].end = end;
		return BTRFS_OK;
	}
	if (after) {
		list->items[index].start = start;
		return BTRFS_OK;
	}
	error = bt_gaps_open(env, list, index);
	if (error == BTRFS_OK) {
		list->items[index] = (struct bt_gap){ start, end };
	}
	return error;
}

/* Removes every gap inside [low, high). */
static void
bt_gaps_drop(struct bt_gaps *list, uint64_t low, uint64_t high)
{
	size_t i;
	size_t kept = 0;

	for (i = 0; i < list->count; i++) {
		if (list->items[i].start < low || list->items[i].end > high) {
			list->items[kept++] = list->items[i];
		}
	}
	list->count = kept;
}

static int
bt_gaps_equal(const struct bt_gaps *a, const struct bt_gaps *b)
{
	return a->count == b->count && bt_equal(a->items, b->items, a->count * sizeof(*a->items));
}

enum btrfs_result
btrfs_allocation_map_create(
    const struct btrfs_environment *environment, struct btrfs_allocation_map **result)
{
	struct btrfs_allocation_map *map;

	if (environment == NULL || result == NULL || environment->allocate == NULL ||
	    environment->release == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*result = NULL;
	map = environment->allocate(environment->context, sizeof(*map));
	if (map == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(map, sizeof(*map));
	map->env = *environment;
	map->capacity = BT_INITIAL_CHUNKS;
	map->chunks =
	    environment->allocate(environment->context, map->capacity * sizeof(*map->chunks));
	map->used = environment->allocate(environment->context, map->capacity * sizeof(*map->used));
	map->loaded = environment->allocate(environment->context, map->capacity);
	if (map->chunks == NULL || map->used == NULL || map->loaded == NULL) {
		btrfs_allocation_map_destroy(map);
		return BTRFS_NO_MEMORY;
	}
	*result = map;
	return BTRFS_OK;
}

static void
bt_space_map_clear(struct btrfs_allocation_map *map)
{
	bt_gaps_release(&map->env, &map->metadata);
	bt_gaps_release(&map->env, &map->data);
	bt_gaps_release(&map->env, &map->system);
	bt_gaps_release(&map->env, &map->device);
	map->valid = 0;
	map->chunk_count = 0;
}

void
btrfs_allocation_map_destroy(struct btrfs_allocation_map *map)
{
	if (map == NULL) {
		return;
	}
	bt_space_map_clear(map);
	if (map->chunks != NULL) {
		map->env.release(
		    map->env.context, map->chunks, map->capacity * sizeof(*map->chunks));
	}
	if (map->used != NULL) {
		map->env.release(map->env.context, map->used, map->capacity * sizeof(*map->used));
	}
	if (map->loaded != NULL) {
		map->env.release(map->env.context, map->loaded, map->capacity);
	}
	map->env.release(map->env.context, map, sizeof(*map));
}

void
btrfs_allocation_map_counts(
    const struct btrfs_allocation_map *map, uint64_t *scans, uint64_t *reuses)
{
	*scans = map->scans;
	*reuses = map->reuses;
}

void
bt_space_map_invalidate(struct btrfs_allocation_map *map)
{
	bt_space_map_clear(map);
}

int
bt_space_map_fits(const struct btrfs_allocation_map *map, const struct btrfs_fs *fs)
{
	return map != NULL && map->valid && map->generation == fs->info.generation &&
	    bt_equal(map->uuid, fs->metadata_uuid, BTRFS_UUID_SIZE) &&
	    map->chunk_count == fs->chunk_count &&
	    bt_equal(map->chunks, fs->chunks, fs->chunk_count * sizeof(*fs->chunks));
}

/* Grows the map's chunk table to hold count chunks. */
static enum btrfs_result
bt_space_map_room(struct btrfs_allocation_map *map, size_t count)
{
	struct bt_chunk *chunks;
	uint64_t *used;
	uint8_t *loaded;
	size_t capacity = map->capacity;

	if (count <= capacity) {
		return BTRFS_OK;
	}
	while (capacity < count) {
		capacity *= 2;
	}
	chunks = map->env.allocate(map->env.context, capacity * sizeof(*chunks));
	used = map->env.allocate(map->env.context, capacity * sizeof(*used));
	loaded = map->env.allocate(map->env.context, capacity);
	if (chunks == NULL || used == NULL || loaded == NULL) {
		if (chunks != NULL) {
			map->env.release(map->env.context, chunks, capacity * sizeof(*chunks));
		}
		if (used != NULL) {
			map->env.release(map->env.context, used, capacity * sizeof(*used));
		}
		if (loaded != NULL) {
			map->env.release(map->env.context, loaded, capacity);
		}
		return BTRFS_NO_MEMORY;
	}
	bt_copy(chunks, map->chunks, map->chunk_count * sizeof(*chunks));
	bt_copy(used, map->used, map->chunk_count * sizeof(*used));
	bt_copy(loaded, map->loaded, map->chunk_count);
	map->env.release(map->env.context, map->chunks, map->capacity * sizeof(*map->chunks));
	map->env.release(map->env.context, map->used, map->capacity * sizeof(*map->used));
	map->env.release(map->env.context, map->loaded, map->capacity);
	map->chunks = chunks;
	map->used = used;
	map->loaded = loaded;
	map->capacity = capacity;
	return BTRFS_OK;
}

enum btrfs_result
bt_space_map_save(const struct bt_space *space, struct btrfs_allocation_map *map)
{
	const struct btrfs_fs *fs = space->fs;
	enum btrfs_result error;

	bt_space_map_clear(map);
	error = bt_space_map_room(map, fs->chunk_count);
	if (error != BTRFS_OK) {
		return error;
	}
	bt_copy(map->chunks, fs->chunks, fs->chunk_count * sizeof(*fs->chunks));
	bt_copy(map->used, space->used, fs->chunk_count * sizeof(*map->used));
	bt_copy(map->loaded, space->loaded, fs->chunk_count);
	bt_copy(map->unloaded_free, space->unloaded_free, sizeof(map->unloaded_free));
	map->item_limit = space->item_limit;
	map->chunk_count = fs->chunk_count;
	error = bt_gaps_copy(&map->env, &map->metadata, &space->metadata);
	if (error == BTRFS_OK) {
		error = bt_gaps_copy(&map->env, &map->data, &space->data);
	}
	if (error == BTRFS_OK) {
		error = bt_gaps_copy(&map->env, &map->system, &space->system);
	}
	if (error == BTRFS_OK) {
		error = bt_gaps_copy(&map->env, &map->device, &space->device);
	}
	if (error != BTRFS_OK) {
		bt_space_map_clear(map);
		return error;
	}
	map->growth = space->growth;
	map->generation = fs->info.generation;
	bt_copy(map->uuid, fs->metadata_uuid, BTRFS_UUID_SIZE);
	map->valid = 1;
	map->scans++;
	return BTRFS_OK;
}

enum btrfs_result
bt_space_from_map(struct btrfs_fs *fs, struct btrfs_allocation_map *map, struct bt_root extent_root,
    const struct bt_root *free_space, size_t node_limit, struct bt_space **result)
{
	struct bt_space *space;
	enum btrfs_result error;

	*result = NULL;
	space = fs->env.allocate(fs->env.context, sizeof(*space));
	if (space == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(space, sizeof(*space));
	space->fs = fs;
	space->node_limit = node_limit;
	space->original_chunks = fs->chunk_count;
	space->growth = map->growth;
	space->extents = extent_root;
	if (free_space != NULL) {
		space->free_space = *free_space;
		space->has_free_space = 1;
	}
	error = bt_space_group_tree(space);
	if (error == BTRFS_OK) {
		error = bt_space_counts(space);
	}
	if (error == BTRFS_OK) {
		bt_copy(space->used, map->used, fs->chunk_count * sizeof(*space->used));
		bt_copy(space->loaded, map->loaded, fs->chunk_count);
		bt_copy(space->unloaded_free, map->unloaded_free, sizeof(space->unloaded_free));
		space->item_limit = map->item_limit;
		/* Copied by the first change; untouched classes cost nothing. */
		bt_gaps_borrow(&space->metadata, &map->metadata);
		bt_gaps_borrow(&space->data, &map->data);
		bt_gaps_borrow(&space->system, &map->system);
		bt_gaps_borrow(&space->device, &map->device);
	}
	if (error != BTRFS_OK) {
		bt_space_destroy(space);
		return error;
	}
	map->reuses++;
	*result = space;
	return BTRFS_OK;
}

static struct bt_gaps *
bt_space_map_class(struct btrfs_allocation_map *map, const struct bt_chunk *chunk)
{
	if (chunk->type & BT_BLOCK_METADATA) {
		return &map->metadata;
	}
	if (chunk->type & BT_BLOCK_SYSTEM) {
		return &map->system;
	}
	return &map->data;
}

/* The device's free ranges: everything past the reserved first MiB that no
 * chunk stripe holds, as the device extents describe it. */
static enum btrfs_result
bt_space_map_devices(struct btrfs_allocation_map *map, uint64_t device_size)
{
	struct bt_gaps stripes = { NULL, 0, 0, 0, 0 };
	uint64_t position = BT_DEVICE_RESERVED;
	size_t i;
	unsigned stripe;
	enum btrfs_result error = BTRFS_OK;

	bt_gaps_release(&map->env, &map->device);
	/* Every stripe as a physical range, sorted; the device extents are the
	 * same ranges in the same order. At most two stripes per chunk. */
	stripes.capacity = 2 * map->chunk_count + 1;
	stripes.items =
	    map->env.allocate(map->env.context, stripes.capacity * sizeof(*stripes.items));
	if (stripes.items == NULL) {
		return BTRFS_NO_MEMORY;
	}
	for (i = 0; i < map->chunk_count; i++) {
		for (stripe = 0; stripe < map->chunks[i].mirrors; stripe++) {
			stripes.items[stripes.count++] =
			    (struct bt_gap){ map->chunks[i].physical[stripe],
				    map->chunks[i].physical[stripe] + map->chunks[i].length };
		}
	}
	bt_gaps_normalize(&stripes);
	for (i = 0; error == BTRFS_OK && i < stripes.count; i++) {
		if (stripes.items[i].start > position) {
			error = bt_gaps_open(&map->env, &map->device, map->device.count);
			if (error == BTRFS_OK) {
				map->device.items[map->device.count - 1] =
				    (struct bt_gap){ position, stripes.items[i].start };
			}
		}
		position = stripes.items[i].end;
	}
	bt_gaps_release(&map->env, &stripes);
	if (error == BTRFS_OK && position < device_size) {
		error = bt_gaps_open(&map->env, &map->device, map->device.count);
		if (error == BTRFS_OK) {
			map->device.items[map->device.count - 1] =
			    (struct bt_gap){ position, device_size };
		}
	}
	return error;
}

enum btrfs_result
bt_space_map_commit(
    const struct bt_space *space, struct btrfs_allocation_map *map, uint64_t generation)
{
	const struct btrfs_fs *fs = space->fs;
	const struct bt_space_change *change;
	const struct bt_chunk *chunk;
	struct bt_gaps *list;
	struct bt_space scratch;
	size_t i;
	size_t kept;
	enum btrfs_result error = BTRFS_OK;

	if (!map->valid || map->chunk_count != space->original_chunks) {
		return BTRFS_CORRUPT;
	}
	error = bt_space_map_room(map, fs->chunk_count);
	/* Chunks the transaction grew: free as a whole, minus superblock stripes,
	 * with the same exclusion a fresh scan applies. */
	for (i = map->chunk_count; error == BTRFS_OK && i < fs->chunk_count; i++) {
		map->chunks[i] = fs->chunks[i];
		map->used[i] = 0;
		map->loaded[i] = 1;
		map->chunk_count++;
		bt_zero(&scratch, sizeof(scratch));
		scratch.fs = (struct btrfs_fs *)fs;
		error = bt_space_gap(&scratch, bt_space_class(&scratch, &fs->chunks[i]),
		    fs->chunks[i].logical, fs->chunks[i].logical + fs->chunks[i].length);
		if (error == BTRFS_OK) {
			error = bt_space_exclude_chunk(&scratch, i);
		}
		list = bt_space_class(&scratch, &fs->chunks[i]);
		for (kept = 0; error == BTRFS_OK && kept < list->count; kept++) {
			if (list->items[kept].start < list->items[kept].end) {
				error = bt_gaps_insert(&map->env,
				    bt_space_map_class(map, &fs->chunks[i]),
				    list->items[kept].start, list->items[kept].end,
				    fs->chunks[i].logical,
				    fs->chunks[i].logical + fs->chunks[i].length);
			}
		}
		bt_gaps_release(&fs->env, &scratch.metadata);
		bt_gaps_release(&fs->env, &scratch.data);
		bt_gaps_release(&fs->env, &scratch.system);
	}
	/* Chunks this transaction loaded join with their free ranges as loaded;
	 * chunks the map already holds were loaded when it was saved. */
	for (i = 0; error == BTRFS_OK && i < space->load_count; i++) {
		chunk = &fs->chunks[space->loads[i].chunk];
		if (!map->loaded[space->loads[i].chunk] &&
		    space->loads[i].start < space->loads[i].end) {
			error = bt_gaps_insert(&map->env, bt_space_map_class(map, chunk),
			    space->loads[i].start, space->loads[i].end, chunk->logical,
			    chunk->logical + chunk->length);
		}
	}
	for (i = 0; error == BTRFS_OK && i < space->load_count; i++) {
		map->loaded[space->loads[i].chunk] = 1;
	}
	bt_copy(map->unloaded_free, space->unloaded_free, sizeof(map->unloaded_free));
	/* Every allocation and release in order. */
	for (i = 0; error == BTRFS_OK && i < space->change_count; i++) {
		change = &space->changes[i];
		chunk = &fs->chunks[change->chunk];
		list = bt_space_map_class(map, chunk);
		error = change->allocate
		    ? bt_gaps_carve(&map->env, list, change->start, change->start + change->length)
		    : bt_gaps_insert(&map->env, list, change->start, change->start + change->length,
			  chunk->logical, chunk->logical + chunk->length);
	}
	for (i = 0; error == BTRFS_OK && i < fs->chunk_count; i++) {
		map->used[i] = space->used[i];
	}
	/* Removed groups leave the map with all their free ranges. */
	for (i = kept = 0; error == BTRFS_OK && i < fs->chunk_count; i++) {
		chunk = &fs->chunks[i];
		if (chunk->removed) {
			bt_gaps_drop(bt_space_map_class(map, chunk), chunk->logical,
			    chunk->logical + chunk->length);
			continue;
		}
		map->chunks[kept] = *chunk;
		map->used[kept] = map->used[i];
		map->loaded[kept] = map->loaded[i];
		kept++;
	}
	if (error == BTRFS_OK) {
		map->chunk_count = kept;
		error = bt_space_map_devices(map, fs->device_size);
	}
	if (error != BTRFS_OK) {
		bt_space_map_clear(map);
		return error;
	}
	map->generation = generation;
	return BTRFS_OK;
}

enum btrfs_result
bt_space_map_check(const struct btrfs_allocation_map *map, struct bt_space *space)
{
	const struct btrfs_fs *fs = space->fs;
	size_t i;
	enum btrfs_result error = BTRFS_OK;

	if (!map->valid || map->chunk_count != fs->chunk_count ||
	    !bt_equal(map->chunks, fs->chunks, fs->chunk_count * sizeof(*fs->chunks))) {
		return BTRFS_CORRUPT;
	}
	/* A fresh space loads the chunks the map holds. */
	for (i = 0; error == BTRFS_OK && i < fs->chunk_count; i++) {
		if (map->loaded[i] && !space->loaded[i]) {
			error = bt_space_load_chunk(space, i);
		}
	}
	if (error != BTRFS_OK || !bt_equal(map->loaded, space->loaded, fs->chunk_count) ||
	    !bt_equal(map->unloaded_free, space->unloaded_free, sizeof(map->unloaded_free)) ||
	    !bt_equal(map->used, space->used, fs->chunk_count * sizeof(*map->used)) ||
	    !bt_gaps_equal(&map->metadata, &space->metadata) ||
	    !bt_gaps_equal(&map->data, &space->data) ||
	    !bt_gaps_equal(&map->system, &space->system) ||
	    !bt_gaps_equal(&map->device, &space->device)) {
		return BTRFS_CORRUPT;
	}
	return BTRFS_OK;
}
