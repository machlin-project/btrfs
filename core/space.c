/* SPDX-License-Identifier: BSD-3-Clause */
#include "space.h"

#define BT_SPACE_MAX_GAPS 131072U

struct bt_gap {
	uint64_t start, end;
};

/* Free ranges of one chunk class in ascending logical order. Reservations
 * consume them from next onward and never return a range to the list. */
struct bt_gaps {
	struct bt_gap *items;
	size_t count;
	size_t capacity;
	size_t next;
};

struct bt_space {
	const struct btrfs_fs *fs;
	struct bt_gaps metadata;
	struct bt_gaps data;
	uint64_t *used;
	struct bt_space_change *changes;
	size_t change_count;
	size_t change_capacity;
	size_t node_limit;
};

static struct bt_gaps *
bt_space_class(struct bt_space *space, const struct bt_chunk *chunk)
{
	if (chunk->type & BT_BLOCK_METADATA) {
		return &space->metadata;
	}
	return chunk->type & BT_BLOCK_DATA ? &space->data : NULL;
}

static enum btrfs_result
bt_space_gap(struct bt_space *space, struct bt_gaps *list, uint64_t start, uint64_t end)
{
	struct bt_gap *gaps;
	size_t capacity;
	const struct btrfs_environment *env = &space->fs->env;

	if (list != NULL && start < end) {
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

/* Exclude every physical superblock's whole stripe from allocation, including
 * each DUP mapping. Extent trees do not describe these reserved stripes. */
static enum btrfs_result
bt_space_exclude_supers(struct bt_space *space)
{
	const struct bt_chunk *chunk;
	struct bt_gaps *list;
	uint64_t physical;
	uint64_t start;
	uint64_t end;
	uint64_t old_end;
	size_t chunk_index;
	size_t i;
	unsigned mirror;
	unsigned copy;
	enum btrfs_result error;

	for (chunk_index = 0; chunk_index < space->fs->chunk_count; chunk_index++) {
		chunk = &space->fs->chunks[chunk_index];
		list = bt_space_class(space, chunk);
		for (copy = 0; list != NULL && copy < chunk->mirrors; copy++) {
			for (mirror = 0; mirror < BT_SUPER_MIRRORS; mirror++) {
				physical = bt_super_offset(mirror) & ~(BT_STRIPE_LENGTH - 1);
				if (physical + BT_STRIPE_LENGTH <= chunk->physical[copy] ||
				    physical >= chunk->physical[copy] + chunk->length) {
					continue;
				}
				start = chunk->logical +
				    (physical > chunk->physical[copy]
					    ? physical - chunk->physical[copy]
					    : 0);
				end = physical + BT_STRIPE_LENGTH - chunk->physical[copy];
				end = chunk->logical + (end < chunk->length ? end : chunk->length);
				for (i = 0; i < list->count; i++) {
					if (start < list->items[i].end &&
					    end > list->items[i].start) {
						old_end = list->items[i].end;
						if (start <= list->items[i].start) {
							list->items[i].start =
							    end < old_end ? end : old_end;
						} else {
							list->items[i].end = start;
							error =
							    bt_space_gap(space, list, end, old_end);
							if (error != BTRFS_OK) {
								return error;
							}
						}
					}
				}
			}
		}
	}
	return BTRFS_OK;
}

/* Finish one chunk: its block group must exist and match the extents found. */
static enum btrfs_result
bt_space_close(struct bt_space *space, size_t index, uint64_t position, uint64_t used, int group)
{
	const struct bt_chunk *chunk = &space->fs->chunks[index];

	if (!group || used != space->used[index]) {
		return BTRFS_CORRUPT;
	}
	return bt_space_gap(
	    space, bt_space_class(space, chunk), position, chunk->logical + chunk->length);
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

/* One ordered pass over the extent tree, merged with the sorted chunk map. Every
 * extent and block-group record must lie inside a chunk; each chunk's extents
 * must be disjoint and sum to its block-group total. Metadata gaps become
 * reservation candidates. Other record types are backreferences of an extent. */
static enum btrfs_result
bt_space_load(struct bt_space *space, struct bt_root root)
{
	const struct btrfs_fs *fs = space->fs;
	const struct bt_chunk *chunk;
	const struct bt_disk_block_group *group;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key first = { 0 };
	uint64_t position = fs->chunks[0].logical;
	uint64_t used = 0;
	size_t index = 0;
	size_t scanned = 0;
	int group_found = 0;
	enum btrfs_result error;

	bt_cursor_init(&cursor, fs, root);
	error = bt_cursor_seek(&cursor, first, 0);
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (++scanned > BT_MAX_TREE_ITEMS) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		if (record.key.type == BT_EXTENT_ITEM || record.key.type == BT_METADATA_ITEM ||
		    record.key.type == BT_BLOCK_GROUP_ITEM) {
			while (index < fs->chunk_count &&
			    record.key.objectid - fs->chunks[index].logical >=
				fs->chunks[index].length &&
			    record.key.objectid >= fs->chunks[index].logical) {
				error = bt_space_close(space, index, position, used, group_found);
				if (error != BTRFS_OK) {
					break;
				}
				index++;
				position = index < fs->chunk_count ? fs->chunks[index].logical : 0;
				used = 0;
				group_found = 0;
			}
			if (error != BTRFS_OK) {
				break;
			}
			if (index == fs->chunk_count ||
			    record.key.objectid < fs->chunks[index].logical) {
				error = BTRFS_CORRUPT;
				break;
			}
			chunk = &fs->chunks[index];
			if (record.key.type == BT_BLOCK_GROUP_ITEM) {
				group = (const void *)record.data;
				if (group_found || record.key.objectid != chunk->logical ||
				    record.key.offset != chunk->length ||
				    record.size != sizeof(*group) ||
				    bt_u64(group->flags) != chunk->type ||
				    bt_u64(group->chunk_objectid) != BT_FIRST_CHUNK_OBJECTID) {
					error = BTRFS_CORRUPT;
					break;
				}
				space->used[index] = bt_u64(group->used_bytes);
				group_found = 1;
			} else {
				error = bt_space_extent(space, chunk, &record, position);
				if (error == BTRFS_OK) {
					error = bt_space_gap(space, bt_space_class(space, chunk),
					    position, record.key.objectid);
				}
				if (error != BTRFS_OK) {
					break;
				}
				position = record.key.objectid +
				    (record.key.type == BT_METADATA_ITEM ? fs->info.node_size
									 : record.key.offset);
				used += position - record.key.objectid;
			}
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_OK;
	}
	for (; error == BTRFS_OK && index < fs->chunk_count; index++) {
		error = bt_space_close(space, index, position, used, group_found);
		position = index + 1 < fs->chunk_count ? fs->chunks[index + 1].logical : 0;
		used = 0;
		group_found = 0;
	}
	return error;
}

static enum btrfs_result
bt_space_reserve(void *context, uint64_t owner, uint8_t level, uint64_t *logical)
{
	struct bt_space *space = context;
	struct bt_gap *gap;
	uint64_t start;
	uint64_t size = space->fs->info.node_size;

	(void)level;
	if (owner == BT_CHUNK_TREE) {
		return BTRFS_UNSUPPORTED;
	}
	while (space->metadata.next < space->metadata.count) {
		gap = &space->metadata.items[space->metadata.next];
		if (gap->start > UINT64_MAX - (size - 1)) {
			space->metadata.next++;
			continue;
		}
		start = (gap->start + size - 1) & ~(size - 1);
		if (start <= gap->end && size <= gap->end - start) {
			*logical = start;
			gap->start = start + size;
			return BTRFS_OK;
		}
		space->metadata.next++;
	}
	return BTRFS_NO_SPACE;
}

enum btrfs_result
bt_space_reserve_data(struct bt_space *space, uint64_t length, uint64_t *logical, uint64_t *size)
{
	struct bt_gap *gap;
	uint64_t sector = space->fs->info.sector_size;

	if (length == 0 || length % sector != 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	while (space->data.next < space->data.count) {
		gap = &space->data.items[space->data.next];
		if (gap->end - gap->start >= sector) {
			*logical = gap->start;
			*size = gap->end - gap->start < length ? gap->end - gap->start : length;
			*size -= *size % sector;
			gap->start += *size;
			return BTRFS_OK;
		}
		space->data.next++;
	}
	return BTRFS_NO_SPACE;
}

static void
bt_space_release(void *context, uint64_t address)
{
	/* All reservations belong to this context. Keeping released slots pinned
	 * prevents reuse by the same transaction; destroy releases them together. */
	(void)context;
	(void)address;
}

enum btrfs_result
bt_space_create(const struct btrfs_fs *fs, struct bt_root extent_root, size_t node_limit,
    struct bt_space **result)
{
	struct bt_space *space;
	const struct bt_chunk *a;
	const struct bt_chunk *b;
	size_t i;
	size_t j;
	unsigned x;
	unsigned y;
	enum btrfs_result error;

	*result = NULL;
	/* Logical separation alone does not authorize writing: reject aliases
	 * across physical chunks and DUP stripes before producing any reservation. */
	for (i = 0; i < fs->chunk_count; i++) {
		a = &fs->chunks[i];
		for (j = i + 1; j < fs->chunk_count; j++) {
			b = &fs->chunks[j];
			for (x = 0; x < a->mirrors; x++) {
				for (y = 0; y < b->mirrors; y++) {
					if (a->physical[x] < b->physical[y] + b->length &&
					    b->physical[y] < a->physical[x] + a->length) {
						return BTRFS_CORRUPT;
					}
				}
			}
		}
	}
	space = fs->env.allocate(fs->env.context, sizeof(*space));
	if (space == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(space, sizeof(*space));
	space->fs = fs;
	space->node_limit = node_limit;
	space->used = fs->env.allocate(fs->env.context, fs->chunk_count * sizeof(*space->used));
	error = space->used == NULL ? BTRFS_NO_MEMORY : bt_space_load(space, extent_root);
	if (error == BTRFS_OK) {
		error = bt_space_exclude_supers(space);
	}
	if (error != BTRFS_OK) {
		bt_space_destroy(space);
		return error;
	}
	*result = space;
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
		if (space->change_capacity == BT_SPACE_MAX_GAPS) {
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

enum btrfs_result
bt_space_change_used(struct bt_space *space, uint64_t address, uint64_t size, int allocate)
{
	const struct bt_chunk *chunk;
	size_t i;
	enum btrfs_result error;

	for (i = 0; i < space->fs->chunk_count; i++) {
		chunk = &space->fs->chunks[i];
		if (address >= chunk->logical && address < chunk->logical + chunk->length) {
			if (size > chunk->logical + chunk->length - address ||
			    (allocate ? size > chunk->length - space->used[i]
				      : size > space->used[i])) {
				return BTRFS_CORRUPT;
			}
			error = bt_space_log(space, i, address, size, allocate);
			if (error == BTRFS_OK) {
				space->used[i] =
				    allocate ? space->used[i] + size : space->used[i] - size;
			}
			return error;
		}
	}
	return BTRFS_CORRUPT;
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
	if (space->metadata.items != NULL) {
		env->release(env->context, space->metadata.items,
		    space->metadata.capacity * sizeof(*space->metadata.items));
	}
	if (space->data.items != NULL) {
		env->release(env->context, space->data.items,
		    space->data.capacity * sizeof(*space->data.items));
	}
	if (space->used != NULL) {
		env->release(
		    env->context, space->used, space->fs->chunk_count * sizeof(*space->used));
	}
	if (space->changes != NULL) {
		env->release(
		    env->context, space->changes, space->change_capacity * sizeof(*space->changes));
	}
	env->release(env->context, space, sizeof(*space));
}
