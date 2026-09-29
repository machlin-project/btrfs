/* SPDX-License-Identifier: BSD-3-Clause */
#include "space.h"

#define BT_SPACE_MAX_GAPS 131072U
#define BT_SUPER_MIRRORS 3U

struct bt_gap {
	uint64_t start, end;
};

struct bt_space {
	const struct btrfs_fs *fs;
	struct bt_gap *gaps;
	uint64_t *used;
	size_t count;
	size_t capacity;
	size_t next;
	size_t node_limit;
};

static uint64_t
bt_super_mirror(unsigned mirror)
{
	return mirror == 0 ? BT_SUPER_OFFSET : UINT64_C(16384) << (12 * mirror);
}

static enum btrfs_result
bt_space_gap(struct bt_space *space, uint64_t start, uint64_t end)
{
	struct bt_gap *gaps;
	size_t capacity;
	const struct btrfs_environment *env = &space->fs->env;

	if (start < end) {
		if (space->count == BT_SPACE_MAX_GAPS) {
			return BTRFS_UNSUPPORTED;
		}
		if (space->count == space->capacity) {
			capacity = space->capacity == 0 ? 256 : space->capacity * 2;
			gaps = env->allocate(env->context, capacity * sizeof(*gaps));
			if (gaps == NULL) {
				return BTRFS_NO_MEMORY;
			}
			if (space->gaps != NULL) {
				bt_copy(gaps, space->gaps, space->count * sizeof(*gaps));
				env->release(
				    env->context, space->gaps, space->capacity * sizeof(*gaps));
			}
			space->gaps = gaps;
			space->capacity = capacity;
		}
		space->gaps[space->count++] = (struct bt_gap){ start, end };
	}
	return BTRFS_OK;
}

/* Exclude every physical superblock's whole stripe from allocation, including
 * each DUP mapping. Extent trees do not describe these reserved stripes. */
static enum btrfs_result
bt_space_exclude_supers(struct bt_space *space)
{
	const struct bt_chunk *chunk;
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
		for (copy = 0; copy < chunk->mirrors; copy++) {
			for (mirror = 0; mirror < BT_SUPER_MIRRORS; mirror++) {
				physical = bt_super_mirror(mirror) & ~(BT_STRIPE_LENGTH - 1);
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
				for (i = 0; i < space->count; i++) {
					if (start < space->gaps[i].end &&
					    end > space->gaps[i].start) {
						old_end = space->gaps[i].end;
						if (start <= space->gaps[i].start) {
							space->gaps[i].start =
							    end < old_end ? end : old_end;
						} else {
							space->gaps[i].end = start;
							error = bt_space_gap(space, end, old_end);
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

static enum btrfs_result
bt_space_load(struct bt_space *space, struct bt_root root)
{
	const struct btrfs_fs *fs = space->fs;
	const struct bt_chunk *chunk;
	const struct bt_disk_extent_item *extent;
	const struct bt_disk_block_group *group;
	struct bt_cursor cursor;
	struct bt_record record;
	uint64_t position;
	uint64_t length;
	uint64_t flags;
	uint64_t used;
	size_t i;
	size_t scanned = 0;
	int group_found;
	enum btrfs_result error;

	bt_cursor_init(&cursor, fs, root);
	error = BTRFS_OK;
	for (i = 0; i < fs->chunk_count && error == BTRFS_OK; i++) {
		chunk = &fs->chunks[i];
		position = chunk->logical;
		used = 0;
		group_found = 0;
		error = bt_cursor_seek(&cursor, (struct bt_key){ .objectid = position }, 0);
		while (error == BTRFS_OK) {
			(void)bt_cursor_record(&cursor, &record);
			if (record.key.objectid >= chunk->logical + chunk->length) {
				break;
			}
			if (++scanned > BT_MAX_TREE_ITEMS) {
				error = BTRFS_UNSUPPORTED;
				break;
			}
			if (record.key.type == BT_EXTENT_ITEM ||
			    record.key.type == BT_METADATA_ITEM) {
				extent = (const void *)record.data;
				length = record.key.type == BT_METADATA_ITEM ? fs->info.node_size
									     : record.key.offset;
				flags = record.size >= sizeof(*extent) ? bt_u64(extent->flags) : 0;
				if (record.size < sizeof(*extent) || bt_u64(extent->refs) == 0 ||
				    bt_u64(extent->generation) > fs->info.generation ||
				    (flags != BT_EXTENT_FLAG_DATA && flags != BT_EXTENT_FLAG_TREE &&
					flags !=
					    (BT_EXTENT_FLAG_TREE | BT_EXTENT_FLAG_FULL_BACKREF)) ||
				    (record.key.type == BT_METADATA_ITEM &&
					(!(flags & BT_EXTENT_FLAG_TREE) ||
					    record.key.offset >= BT_MAX_LEVEL)) ||
				    length == 0 || length % fs->info.sector_size != 0 ||
				    record.key.objectid < position ||
				    record.key.objectid % fs->info.sector_size != 0 ||
				    length > chunk->logical + chunk->length - record.key.objectid ||
				    (flags == BT_EXTENT_FLAG_DATA
					    ? !(chunk->type & BT_BLOCK_DATA)
					    : !(chunk->type &
						  (BT_BLOCK_METADATA | BT_BLOCK_SYSTEM)))) {
					error = BTRFS_CORRUPT;
					break;
				}
				if (chunk->type & BT_BLOCK_METADATA) {
					error = bt_space_gap(space, position, record.key.objectid);
					if (error != BTRFS_OK) {
						break;
					}
				}
				position = record.key.objectid + length;
				used += length;
			} else if (record.key.type == BT_BLOCK_GROUP_ITEM) {
				group = (const void *)record.data;
				if (group_found || record.key.objectid != chunk->logical ||
				    record.key.offset != chunk->length ||
				    record.size != sizeof(*group) ||
				    bt_u64(group->flags) != chunk->type ||
				    bt_u64(group->chunk_objectid) != BT_FIRST_CHUNK_OBJECTID) {
					error = BTRFS_CORRUPT;
					break;
				}
				space->used[i] = bt_u64(group->used_bytes);
				group_found = 1;
			}
			error = bt_cursor_next(&cursor);
		}
		if (error == BTRFS_NOT_FOUND) {
			error = BTRFS_OK;
		}
		if (error == BTRFS_OK && (!group_found || used != space->used[i])) {
			error = BTRFS_CORRUPT;
		}
		if (error == BTRFS_OK && (chunk->type & BT_BLOCK_METADATA)) {
			error = bt_space_gap(space, position, chunk->logical + chunk->length);
		}
	}
	bt_cursor_fini(&cursor);
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
	while (space->next < space->count) {
		gap = &space->gaps[space->next];
		if (gap->start > UINT64_MAX - (size - 1)) {
			space->next++;
			continue;
		}
		start = (gap->start + size - 1) & ~(size - 1);
		if (start <= gap->end && size <= gap->end - start) {
			*logical = start;
			gap->start = start + size;
			return BTRFS_OK;
		}
		space->next++;
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

enum btrfs_result
bt_space_change_used(struct bt_space *space, uint64_t address, int allocate)
{
	const struct bt_chunk *chunk;
	size_t i;
	uint64_t size = space->fs->info.node_size;

	for (i = 0; i < space->fs->chunk_count; i++) {
		chunk = &space->fs->chunks[i];
		if (address >= chunk->logical && address < chunk->logical + chunk->length) {
			if (size > chunk->logical + chunk->length - address ||
			    (allocate ? size > chunk->length - space->used[i]
				      : size > space->used[i])) {
				return BTRFS_CORRUPT;
			}
			space->used[i] = allocate ? space->used[i] + size : space->used[i] - size;
			return BTRFS_OK;
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
	if (space->gaps != NULL) {
		env->release(env->context, space->gaps, space->capacity * sizeof(*space->gaps));
	}
	if (space->used != NULL) {
		env->release(
		    env->context, space->used, space->fs->chunk_count * sizeof(*space->used));
	}
	env->release(env->context, space, sizeof(*space));
}
