/* SPDX-License-Identifier: BSD-3-Clause */
#include "space.h"

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
 * consume them from next onward and never return a range to the list. */
struct bt_gaps {
	struct bt_gap *items;
	size_t count;
	size_t capacity;
	size_t next;
};

struct bt_space {
	struct btrfs_fs *fs;
	struct bt_gaps metadata;
	struct bt_gaps data;
	struct bt_gaps system;
	/* Unallocated physical ranges of the device, known after verification. */
	struct bt_gaps device;
	uint64_t *used;
	size_t original_chunks;
	int growth;
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

/* Exclude every physical superblock's whole stripe of one chunk from
 * allocation, including each DUP mapping. Extent trees do not describe these
 * reserved stripes. */
static enum btrfs_result
bt_space_exclude_chunk(struct bt_space *space, size_t chunk_index)
{
	const struct bt_chunk *chunk = &space->fs->chunks[chunk_index];
	struct bt_gaps *list = bt_space_class(space, chunk);
	uint64_t physical;
	uint64_t start;
	uint64_t end;
	uint64_t old_end;
	size_t i;
	unsigned mirror;
	unsigned copy;
	enum btrfs_result error;

	for (copy = 0; list != NULL && copy < chunk->mirrors; copy++) {
		for (mirror = 0; mirror < BT_SUPER_MIRRORS; mirror++) {
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
			for (i = 0; i < list->count; i++) {
				if (start < list->items[i].end && end > list->items[i].start) {
					old_end = list->items[i].end;
					if (start <= list->items[i].start) {
						list->items[i].start =
						    end < old_end ? end : old_end;
					} else {
						list->items[i].end = start;
						error = bt_space_gap(space, list, end, old_end);
						if (error != BTRFS_OK) {
							return error;
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
	struct bt_gaps saved = space->device;
	struct bt_chunk *chunk;
	struct bt_gap *copy;
	uint64_t type = 0;
	uint64_t logical = 0;
	uint64_t length;
	uint64_t limit = kind == BT_BLOCK_DATA ? BT_CHUNK_DATA_MAX
	    : kind == BT_BLOCK_SYSTEM	       ? BT_CHUNK_SYSTEM_MAX
					       : BT_CHUNK_METADATA_MAX;
	uint64_t physical[2];
	size_t i;
	unsigned stripes;
	unsigned stripe;
	int fits;
	enum btrfs_result error;

	if (!space->growth || fs->chunk_count == BT_MAX_CHUNKS) {
		return BTRFS_NO_SPACE;
	}
	/* The chunk item and device item updates need system space first. */
	if (kind != BT_BLOCK_SYSTEM) {
		error = bt_space_check_system(space);
		if (error != BTRFS_OK) {
			return error;
		}
	}
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
	/* Device gaps are copied so a failed attempt leaves them untouched. */
	copy = fs->env.allocate(fs->env.context, (saved.count + 1) * sizeof(*copy));
	if (copy == NULL) {
		return BTRFS_NO_MEMORY;
	}
	for (fits = 0; !fits && length >= minimum && length >= BT_CHUNK_ALIGN;
	    length = (length / 2) & ~(BT_CHUNK_ALIGN - 1)) {
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
	enum btrfs_result error;

	(void)level;
	for (;;) {
		while (list->next < list->count) {
			gap = &list->items[list->next];
			if (gap->start > UINT64_MAX - (size - 1)) {
				list->next++;
				continue;
			}
			start = (gap->start + size - 1) & ~(size - 1);
			if (start <= gap->end && size <= gap->end - start) {
				*logical = start;
				gap->start = start + size;
				return BTRFS_OK;
			}
			list->next++;
		}
		error = bt_space_grow(
		    space, owner == BT_CHUNK_TREE ? BT_BLOCK_SYSTEM : BT_BLOCK_METADATA, size);
		if (error != BTRFS_OK) {
			return error;
		}
	}
}

enum btrfs_result
bt_space_reserve_data(struct bt_space *space, uint64_t length, uint64_t *logical, uint64_t *size)
{
	struct bt_gap *gap;
	uint64_t sector = space->fs->info.sector_size;
	enum btrfs_result error;

	if (length == 0 || length % sector != 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	for (;;) {
		while (space->data.next < space->data.count) {
			gap = &space->data.items[space->data.next];
			if (gap->end - gap->start >= sector) {
				*logical = gap->start;
				*size =
				    gap->end - gap->start < length ? gap->end - gap->start : length;
				*size -= *size % sector;
				gap->start += *size;
				return BTRFS_OK;
			}
			space->data.next++;
		}
		error = bt_space_grow(space, BT_BLOCK_DATA, length);
		if (error != BTRFS_OK) {
			return error;
		}
	}
}

int
bt_space_data_available(const struct bt_space *space, uint64_t length)
{
	uint64_t available = 0;
	uint64_t unallocated = 0;
	unsigned copies = 1;
	size_t i;

	for (i = space->data.next; i < space->data.count && available < length; i++) {
		available += space->data.items[i].end - space->data.items[i].start;
	}
	if (available >= length) {
		return 1;
	}
	if (!space->growth) {
		return 0;
	}
	for (i = 0; i < space->fs->chunk_count; i++) {
		if ((space->fs->chunks[i].type & BT_BLOCK_DATA) != 0 &&
		    (space->fs->chunks[i].type & BT_BLOCK_DUP) != 0) {
			copies = 2;
		}
	}
	for (i = space->device.next; i < space->device.count; i++) {
		unallocated += space->device.items[i].end - space->device.items[i].start;
	}
	return unallocated / copies >= length - available;
}

enum btrfs_result
bt_space_reserve_exact(struct bt_space *space, uint64_t length, uint64_t *logical)
{
	struct bt_gap *gap;
	uint64_t sector = space->fs->info.sector_size;
	size_t i;
	enum btrfs_result error;

	if (length == 0 || length % sector != 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	for (;;) {
		for (i = space->data.next; i < space->data.count; i++) {
			gap = &space->data.items[i];
			if (gap->end - gap->start >= length) {
				*logical = gap->start;
				gap->start += length;
				return BTRFS_OK;
			}
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

enum btrfs_result
bt_space_create(
    struct btrfs_fs *fs, struct bt_root extent_root, size_t node_limit, struct bt_space **result)
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
	space->original_chunks = fs->chunk_count;
	space->used = fs->env.allocate(fs->env.context, BT_MAX_CHUNKS * sizeof(*space->used));
	error = space->used == NULL ? BTRFS_NO_MEMORY : bt_space_load(space, extent_root);
	for (i = 0; error == BTRFS_OK && i < fs->chunk_count; i++) {
		error = bt_space_exclude_chunk(space, i);
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

int
bt_space_unused(const struct bt_space *space, size_t chunk)
{
	const struct bt_chunk *group = &space->fs->chunks[chunk];
	const struct bt_gaps *list = bt_space_class((struct bt_space *)space, group);
	size_t i;

	if (chunk >= space->original_chunks || group->removed || space->used[chunk] != 0 ||
	    list == NULL) {
		return 0;
	}
	/* Gaps are never returned, so an untouched whole-group gap means this
	 * transaction neither allocated nor freed anything there. */
	for (i = list->next; i < list->count; i++) {
		if (list->items[i].start == group->logical &&
		    list->items[i].end == group->logical + group->length) {
			return 1;
		}
	}
	return 0;
}

void
bt_space_retire(struct bt_space *space, size_t chunk)
{
	struct bt_chunk *group = &space->fs->chunks[chunk];
	struct bt_gaps *list = bt_space_class(space, group);
	size_t i;

	for (i = list->next; i < list->count; i++) {
		if (list->items[i].start == group->logical) {
			list->items[i].start = list->items[i].end;
		}
	}
	group->removed = 1;
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
		env->release(env->context, space->used, BT_MAX_CHUNKS * sizeof(*space->used));
	}
	if (space->system.items != NULL) {
		env->release(env->context, space->system.items,
		    space->system.capacity * sizeof(*space->system.items));
	}
	if (space->device.items != NULL) {
		env->release(env->context, space->device.items,
		    space->device.capacity * sizeof(*space->device.items));
	}
	if (space->changes != NULL) {
		env->release(
		    env->context, space->changes, space->change_capacity * sizeof(*space->changes));
	}
	env->release(env->context, space, sizeof(*space));
}
