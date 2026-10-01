/* SPDX-License-Identifier: BSD-3-Clause */
#include "fst.h"
#include "encode.h"

#define BT_FST_RUN_LIMIT 1048576U

struct bt_run {
	uint64_t start, end;
};

struct bt_runs {
	const struct btrfs_environment *env;
	struct bt_run *items;
	size_t count;
	size_t capacity;
};

static enum btrfs_result
bt_runs_add(struct bt_runs *runs, uint64_t start, uint64_t end)
{
	struct bt_run *grown;
	size_t capacity;

	if (start == end) {
		return BTRFS_OK;
	}
	if (runs->count != 0 && runs->items[runs->count - 1].end == start) {
		runs->items[runs->count - 1].end = end;
		return BTRFS_OK;
	}
	if (runs->count == BT_FST_RUN_LIMIT) {
		return BTRFS_UNSUPPORTED;
	}
	if (runs->count == runs->capacity) {
		capacity = runs->capacity == 0 ? 64 : runs->capacity * 2;
		grown = runs->env->allocate(runs->env->context, capacity * sizeof(*grown));
		if (grown == NULL) {
			return BTRFS_NO_MEMORY;
		}
		if (runs->items != NULL) {
			bt_copy(grown, runs->items, runs->count * sizeof(*grown));
			runs->env->release(
			    runs->env->context, runs->items, runs->capacity * sizeof(*grown));
		}
		runs->items = grown;
		runs->capacity = capacity;
	}
	runs->items[runs->count++] = (struct bt_run){ start, end };
	return BTRFS_OK;
}

static void
bt_runs_release(struct bt_runs *runs)
{
	if (runs->items != NULL) {
		runs->env->release(
		    runs->env->context, runs->items, runs->capacity * sizeof(*runs->items));
	}
	runs->items = NULL;
	runs->count = runs->capacity = 0;
}

/* Free space by the extent tree: gaps between extents inside the group. */
static enum btrfs_result
bt_fst_expected(const struct btrfs_fs *fs, struct bt_root extents, const struct bt_chunk *chunk,
    struct bt_runs *runs)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = chunk->logical };
	uint64_t position = chunk->logical;
	uint64_t end = chunk->logical + chunk->length;
	uint64_t length;
	enum btrfs_result error;

	bt_cursor_init(&cursor, fs, extents);
	error = bt_cursor_seek(&cursor, key, 0);
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid >= end) {
			break;
		}
		if (record.key.type == BT_EXTENT_ITEM || record.key.type == BT_METADATA_ITEM) {
			length = record.key.type == BT_METADATA_ITEM ? fs->info.node_size
								     : record.key.offset;
			if (record.key.objectid < position || length > end - record.key.objectid) {
				error = BTRFS_CORRUPT;
				break;
			}
			error = bt_runs_add(runs, position, record.key.objectid);
			if (error != BTRFS_OK) {
				break;
			}
			position = record.key.objectid + length;
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (error == BTRFS_NOT_FOUND || error == BTRFS_OK) {
		error = bt_runs_add(runs, position, end);
	}
	return error;
}

static int
bt_bit(const uint8_t *bits, uint64_t index)
{
	return (bits[index / 8] >> (index % 8)) & 1;
}

/* Free space as recorded: extent items or bitmap runs, plus the info count. */
static enum btrfs_result
bt_fst_actual(const struct btrfs_fs *fs, struct bt_root tree, const struct bt_chunk *chunk,
    struct bt_runs *runs, uint32_t *count, uint32_t *flags, size_t *items)
{
	const struct bt_disk_free_space_info *info;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = {
		.objectid = chunk->logical, .type = BT_FREE_SPACE_INFO, .offset = chunk->length
	};
	uint64_t end = chunk->logical + chunk->length;
	uint64_t sector = fs->info.sector_size;
	uint64_t i;
	enum btrfs_result error;

	*items = 0;
	bt_cursor_init(&cursor, fs, tree);
	error = bt_cursor_seek(&cursor, key, 0);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (bt_key_compare(record.key, key) != 0 || record.size != sizeof(*info)) {
			error = BTRFS_CORRUPT;
		} else {
			info = (const void *)record.data;
			*count = bt_u32(info->extent_count);
			*flags = bt_u32(info->flags);
			error = (*flags & ~BT_FREE_SPACE_USING_BITMAPS) != 0
			    ? BTRFS_UNSUPPORTED
			    : bt_cursor_next(&cursor);
		}
	}
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid >= end) {
			break;
		}
		if (record.key.type != (*flags ? BT_FREE_SPACE_BITMAP : BT_FREE_SPACE_EXTENT) ||
		    record.key.objectid < chunk->logical || record.key.offset == 0 ||
		    record.key.offset > end - record.key.objectid ||
		    record.key.objectid % sector != 0 || record.key.offset % sector != 0 ||
		    (runs->count != 0 && record.key.objectid < runs->items[runs->count - 1].end)) {
			error = BTRFS_CORRUPT;
			break;
		}
		(*items)++;
		if (!*flags) {
			if (record.size != 0) {
				error = BTRFS_CORRUPT;
				break;
			}
			error = bt_runs_add(
			    runs, record.key.objectid, record.key.objectid + record.key.offset);
		} else if (record.size != (record.key.offset / sector + 7) / 8 ||
		    record.size > BT_FREE_SPACE_BITMAP_BYTES) {
			error = BTRFS_CORRUPT;
		} else {
			for (i = 0; error == BTRFS_OK && i < record.key.offset / sector; i++) {
				if (bt_bit(record.data, i)) {
					error = bt_runs_add(runs, record.key.objectid + i * sector,
					    record.key.objectid + (i + 1) * sector);
				}
			}
		}
		if (error == BTRFS_OK) {
			error = bt_cursor_next(&cursor);
		}
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

enum btrfs_result
bt_fst_verify(const struct btrfs_fs *fs, struct bt_root tree, struct bt_root extents)
{
	struct bt_runs expected = { &fs->env, NULL, 0, 0 };
	struct bt_runs actual = { &fs->env, NULL, 0, 0 };
	uint32_t count = 0;
	uint32_t flags = 0;
	size_t items = 0;
	size_t chunk;
	enum btrfs_result error = BTRFS_OK;

	for (chunk = 0; error == BTRFS_OK && chunk < fs->chunk_count; chunk++) {
		expected.count = 0;
		actual.count = 0;
		error = bt_fst_expected(fs, extents, &fs->chunks[chunk], &expected);
		if (error == BTRFS_OK) {
			error = bt_fst_actual(
			    fs, tree, &fs->chunks[chunk], &actual, &count, &flags, &items);
		}
		/* Linux keeps extent items maximal, so their number equals the runs. */
		if (error == BTRFS_OK &&
		    (expected.count != actual.count || count != actual.count ||
			(!flags && items != actual.count) ||
			!bt_equal(expected.items, actual.items,
			    expected.count * sizeof(*expected.items)))) {
			error = BTRFS_CORRUPT;
		}
	}
	bt_runs_release(&expected);
	bt_runs_release(&actual);
	return error;
}

static enum btrfs_result
bt_fst_info(struct bt_mutation *mutation, struct bt_root tree, const struct bt_chunk *chunk,
    struct bt_disk_free_space_info *info)
{
	struct bt_key key = {
		.objectid = chunk->logical, .type = BT_FREE_SPACE_INFO, .offset = chunk->length
	};
	size_t length;
	enum btrfs_result error;

	error = bt_mutation_find(mutation, tree, key, info, sizeof(*info), &length);
	if (error == BTRFS_NOT_FOUND || error == BTRFS_RANGE ||
	    (error == BTRFS_OK && length != sizeof(*info))) {
		return BTRFS_CORRUPT;
	}
	return error;
}

/* The item of the given type with the greatest key at or below key. */
static enum btrfs_result
bt_fst_before(struct bt_mutation *mutation, struct bt_root tree, struct bt_key key,
    struct bt_key *found, int *present)
{
	struct bt_cursor cursor;
	struct bt_record record;
	enum btrfs_result error;

	*present = 0;
	bt_cursor_init(&cursor, bt_mutation_view(mutation), tree);
	error = bt_cursor_seek(&cursor, key, 1);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		*found = record.key;
		*present = record.key.type == key.type;
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

static enum btrfs_result
bt_fst_after(struct bt_mutation *mutation, struct bt_root tree, struct bt_key key,
    struct bt_key *found, int *present)
{
	struct bt_cursor cursor;
	struct bt_record record;
	enum btrfs_result error;

	*present = 0;
	bt_cursor_init(&cursor, bt_mutation_view(mutation), tree);
	error = bt_cursor_seek(&cursor, key, 0);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		*found = record.key;
		*present = record.key.type == key.type;
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

static enum btrfs_result
bt_fst_extents(struct bt_mutation *mutation, struct bt_root *tree, const struct bt_chunk *chunk,
    uint64_t start, uint64_t end, int allocate, int64_t *delta)
{
	struct bt_key key = { .objectid = start, .type = BT_FREE_SPACE_EXTENT, .offset = 0 };
	struct bt_key left;
	struct bt_key right;
	uint64_t chunk_end = chunk->logical + chunk->length;
	uint64_t merged_start = start;
	uint64_t merged_end = end;
	int has_left = 0;
	int has_right = 0;
	enum btrfs_result error;

	if (allocate) {
		key.offset = UINT64_MAX;
		error = bt_fst_before(mutation, *tree, key, &left, &has_left);
		if (error != BTRFS_OK) {
			return error;
		}
		if (!has_left || left.objectid < chunk->logical ||
		    left.offset < end - left.objectid || left.objectid > start) {
			return BTRFS_CORRUPT;
		}
		error = bt_mutation_edit(mutation, tree, left, NULL, 0, BT_DELETE);
		*delta = -1;
		if (error == BTRFS_OK && left.objectid < start) {
			key = (struct bt_key){ left.objectid, start - left.objectid,
				BT_FREE_SPACE_EXTENT };
			error = bt_mutation_edit(mutation, tree, key, NULL, 0, BT_INSERT);
			(*delta)++;
		}
		if (error == BTRFS_OK && end < left.objectid + left.offset) {
			key = (struct bt_key){ end, left.objectid + left.offset - end,
				BT_FREE_SPACE_EXTENT };
			error = bt_mutation_edit(mutation, tree, key, NULL, 0, BT_INSERT);
			(*delta)++;
		}
		return error;
	}
	error = bt_fst_before(mutation, *tree, key, &left, &has_left);
	if (error == BTRFS_OK) {
		error = bt_fst_after(mutation, *tree, key, &right, &has_right);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	has_left = has_left && left.objectid >= chunk->logical;
	has_right = has_right && right.objectid < chunk_end;
	if ((has_left && left.objectid + left.offset > start) ||
	    (has_right && right.objectid < end)) {
		return BTRFS_CORRUPT;
	}
	*delta = 1;
	if (has_left && left.objectid + left.offset == start) {
		merged_start = left.objectid;
		error = bt_mutation_edit(mutation, tree, left, NULL, 0, BT_DELETE);
		(*delta)--;
	}
	if (error == BTRFS_OK && has_right && right.objectid == end) {
		merged_end = end + right.offset;
		error = bt_mutation_edit(mutation, tree, right, NULL, 0, BT_DELETE);
		(*delta)--;
	}
	if (error == BTRFS_OK) {
		key = (struct bt_key){ merged_start, merged_end - merged_start,
			BT_FREE_SPACE_EXTENT };
		error = bt_mutation_edit(mutation, tree, key, NULL, 0, BT_INSERT);
	}
	return error;
}

/* Reads one sector's bit, or sets bits [sector_start, *end) of the bitmap item
 * holding sector_start to value, each having had the opposite value; *end is
 * clipped to that item. Bitmaps cover consecutive ranges of the group. */
static enum btrfs_result
bt_fst_bitmap(struct bt_mutation *mutation, struct bt_root *tree, const struct bt_chunk *chunk,
    uint64_t sector_start, uint64_t *end, int *bit, int write)
{
	const struct btrfs_fs *view = bt_mutation_view(mutation);
	uint8_t bits[BT_FREE_SPACE_BITMAP_BYTES];
	struct bt_key key = {
		.objectid = sector_start, .type = BT_FREE_SPACE_BITMAP, .offset = UINT64_MAX
	};
	struct bt_key found;
	uint64_t sector = view->info.sector_size;
	uint64_t index;
	uint64_t last;
	size_t length;
	int present;
	enum btrfs_result error;

	error = bt_fst_before(mutation, *tree, key, &found, &present);
	if (error != BTRFS_OK) {
		return error;
	}
	if (!present || found.objectid < chunk->logical ||
	    found.offset > sizeof(bits) * 8 * sector ||
	    sector_start - found.objectid >= found.offset) {
		return BTRFS_CORRUPT;
	}
	error = bt_mutation_find(mutation, *tree, found, bits, sizeof(bits), &length);
	if (error != BTRFS_OK) {
		return error == BTRFS_RANGE ? BTRFS_CORRUPT : error;
	}
	index = (sector_start - found.objectid) / sector;
	if (length * 8 < found.offset / sector) {
		return BTRFS_CORRUPT;
	}
	if (!write) {
		*bit = bt_bit(bits, index);
		return BTRFS_OK;
	}
	if (*end > found.objectid + found.offset) {
		*end = found.objectid + found.offset;
	}
	last = (*end - found.objectid) / sector;
	for (; index < last; index++) {
		if (bt_bit(bits, index) == *bit) {
			return BTRFS_CORRUPT;
		}
		bits[index / 8] ^= (uint8_t)(1U << (index % 8));
	}
	return bt_mutation_edit(mutation, tree, found, bits, length, BT_REPLACE);
}

enum btrfs_result
bt_fst_change(struct bt_mutation *mutation, struct bt_root *tree, const struct bt_chunk *chunk,
    uint64_t start, uint64_t length, int allocate)
{
	const struct btrfs_fs *view = bt_mutation_view(mutation);
	struct bt_disk_free_space_info info;
	struct bt_key key = {
		.objectid = chunk->logical, .type = BT_FREE_SPACE_INFO, .offset = chunk->length
	};
	uint64_t sector;
	uint64_t position;
	uint64_t next;
	int64_t delta = 0;
	int before = 0;
	int after = 0;
	int bit;
	enum btrfs_result error;

	if (view == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	sector = view->info.sector_size;
	if (length == 0 || start < chunk->logical ||
	    length > chunk->logical + chunk->length - start || start % sector != 0 ||
	    length % sector != 0) {
		return BTRFS_CORRUPT;
	}
	error = bt_fst_info(mutation, *tree, chunk, &info);
	if (error != BTRFS_OK) {
		return error;
	}
	if (!(bt_u32(info.flags) & BT_FREE_SPACE_USING_BITMAPS)) {
		error =
		    bt_fst_extents(mutation, tree, chunk, start, start + length, allocate, &delta);
	} else {
		/* Neighbouring sectors outside the range decide whether runs split or
		 * merge, exactly as the extent count tracks free runs. */
		if (start > chunk->logical) {
			error =
			    bt_fst_bitmap(mutation, tree, chunk, start - sector, NULL, &before, 0);
		}
		if (error == BTRFS_OK && start + length < chunk->logical + chunk->length) {
			error =
			    bt_fst_bitmap(mutation, tree, chunk, start + length, NULL, &after, 0);
		}
		for (position = start; error == BTRFS_OK && position < start + length;
		    position = next) {
			next = start + length;
			bit = !allocate;
			error = bt_fst_bitmap(mutation, tree, chunk, position, &next, &bit, 1);
		}
		delta = before && after ? (allocate ? 1 : -1)
		    : !before && !after ? (allocate ? -1 : 1)
					: 0;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	if ((int64_t)bt_u32(info.extent_count) + delta < 0) {
		return BTRFS_CORRUPT;
	}
	bt_put32(&info.extent_count, (uint32_t)((int64_t)bt_u32(info.extent_count) + delta));
	return bt_mutation_edit(mutation, tree, key, &info, sizeof(info), BT_REPLACE);
}
