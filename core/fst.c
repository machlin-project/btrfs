/* SPDX-License-Identifier: BSD-3-Clause */
#include "fst.h"
#include "encode.h"
#include "space.h"

#define BT_FST_RUN_LIMIT 1048576U
/* Linux's gap between the two thresholds, against thrashing. */
#define BT_FST_THRESHOLD_MARGIN 100U

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
bt_fst_verify_group(const struct btrfs_fs *fs, struct bt_root tree, const struct bt_chunk *chunk,
    const struct bt_fst_run *expected, size_t count)
{
	struct bt_runs actual = { &fs->env, NULL, 0, 0 };
	uint32_t extents = 0;
	uint32_t flags = 0;
	size_t items = 0;
	size_t i;
	enum btrfs_result error;

	error = bt_fst_actual(fs, tree, chunk, &actual, &extents, &flags, &items);
	/* Linux keeps extent items maximal, so their number equals the runs. */
	if (error == BTRFS_OK &&
	    (count != actual.count || extents != actual.count ||
		(!flags && items != actual.count))) {
		error = BTRFS_CORRUPT;
	}
	for (i = 0; error == BTRFS_OK && i < count; i++) {
		if (expected[i].start != actual.items[i].start ||
		    expected[i].end != actual.items[i].end) {
			error = BTRFS_CORRUPT;
		}
	}
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
	struct bt_key left = { 0 };
	struct bt_key right = { 0 };
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
		if (left.objectid == start && end == left.objectid + left.offset) {
			*delta = -1;
			return bt_mutation_edit(mutation, tree, left, NULL, 0, BT_DELETE);
		}
		/* Keep the existing item for the surviving range. An edge allocation
		 * usually changes only its key, without shifting any leaf contents. */
		*delta = 0;
		if (left.objectid < start) {
			key = (struct bt_key){ left.objectid, start - left.objectid,
				BT_FREE_SPACE_EXTENT };
			error = bt_mutation_rekey(mutation, tree, left, key, NULL, 0);
			if (error == BTRFS_OK && end < left.objectid + left.offset) {
				key = (struct bt_key){ end, left.objectid + left.offset - end,
					BT_FREE_SPACE_EXTENT };
				error = bt_mutation_edit(mutation, tree, key, NULL, 0, BT_INSERT);
				(*delta)++;
			}
		} else {
			key = (struct bt_key){ end, left.objectid + left.offset - end,
				BT_FREE_SPACE_EXTENT };
			error = bt_mutation_rekey(mutation, tree, left, key, NULL, 0);
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
	has_left = has_left && left.objectid + left.offset == start;
	has_right = has_right && right.objectid == end;
	*delta = 1 - has_left - has_right;
	if (has_left) {
		merged_start = left.objectid;
	}
	if (has_right) {
		merged_end = end + right.offset;
	}
	/* Two adjacent runs become one; otherwise extend the single neighbour
	 * directly. The count changes only when a run is added or removed. */
	if (has_left && has_right) {
		error = bt_mutation_edit(mutation, tree, right, NULL, 0, BT_DELETE);
	}
	if (error == BTRFS_OK) {
		key = (struct bt_key){ merged_start, merged_end - merged_start,
			BT_FREE_SPACE_EXTENT };
		error = has_left || has_right
		    ? bt_mutation_rekey(mutation, tree, has_left ? left : right, key, NULL, 0)
		    : bt_mutation_edit(mutation, tree, key, NULL, 0, BT_INSERT);
	}
	return error;
}

/* Locate and copy the bitmap in one descent. The cursor must be closed before
 * editing, because it can borrow the transaction's private nodes. */
static enum btrfs_result
bt_fst_bitmap_load(struct bt_mutation *mutation, struct bt_root tree, const struct bt_chunk *chunk,
    uint64_t position, struct bt_key *found, uint8_t *bits, size_t *length)
{
	const struct btrfs_fs *view = bt_mutation_view(mutation);
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { position, UINT64_MAX, BT_FREE_SPACE_BITMAP };
	uint64_t sector = view->info.sector_size;
	uint64_t end = chunk->logical + chunk->length;
	enum btrfs_result error;

	bt_cursor_init(&cursor, view, tree);
	error = bt_cursor_seek(&cursor, key, 1);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		*found = record.key;
		if (found->type != BT_FREE_SPACE_BITMAP || found->objectid < chunk->logical ||
		    found->objectid > position || found->objectid >= end || found->offset == 0 ||
		    found->offset > end - found->objectid || found->objectid % sector != 0 ||
		    found->offset % sector != 0 || position - found->objectid >= found->offset ||
		    record.size != (found->offset / sector + 7) / 8 ||
		    record.size > BT_FREE_SPACE_BITMAP_BYTES) {
			error = BTRFS_CORRUPT;
		} else {
			*length = record.size;
			bt_copy(bits, record.data, record.size);
		}
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
}

static enum btrfs_result
bt_fst_bitmap_bit(struct bt_mutation *mutation, struct bt_root tree, const struct bt_chunk *chunk,
    uint64_t position, int *bit)
{
	uint8_t bits[BT_FREE_SPACE_BITMAP_BYTES];
	struct bt_key found;
	size_t length;
	enum btrfs_result error;

	error = bt_fst_bitmap_load(mutation, tree, chunk, position, &found, bits, &length);
	if (error == BTRFS_OK) {
		*bit = bt_bit(bits,
		    (position - found.objectid) / bt_mutation_view(mutation)->info.sector_size);
	}
	return error;
}

/* Every sector in the range must change. Neighbours in the same bitmap come
 * from the copy already held; only neighbours across an item boundary need
 * another lookup. At most one bitmap is visited per sector in the range. */
static enum btrfs_result
bt_fst_bitmaps(struct bt_mutation *mutation, struct bt_root *tree, const struct bt_chunk *chunk,
    uint64_t start, uint64_t end, int allocate, int64_t *delta)
{
	uint8_t bits[BT_FREE_SPACE_BITMAP_BYTES];
	struct bt_key found;
	uint64_t sector = bt_mutation_view(mutation)->info.sector_size;
	uint64_t position;
	uint64_t next;
	uint64_t index;
	uint64_t last;
	size_t length;
	int before = 0;
	int after = 0;
	enum btrfs_result error;

	for (position = start; position < end; position = next) {
		error = bt_fst_bitmap_load(mutation, *tree, chunk, position, &found, bits, &length);
		if (error != BTRFS_OK) {
			return error;
		}
		next = found.objectid + found.offset < end ? found.objectid + found.offset : end;
		index = (position - found.objectid) / sector;
		last = (next - found.objectid) / sector;
		if (position == start && start > chunk->logical) {
			if (index != 0) {
				before = bt_bit(bits, index - 1);
			} else {
				error = bt_fst_bitmap_bit(
				    mutation, *tree, chunk, start - sector, &before);
			}
		}
		if (error == BTRFS_OK && next == end && end < chunk->logical + chunk->length) {
			if (last < found.offset / sector) {
				after = bt_bit(bits, last);
			} else {
				error = bt_fst_bitmap_bit(mutation, *tree, chunk, end, &after);
			}
		}
		if (error != BTRFS_OK) {
			return error;
		}
		for (; index < last; index++) {
			if (bt_bit(bits, index) == !allocate) {
				return BTRFS_CORRUPT;
			}
			bits[index / 8] ^= (uint8_t)(1U << (index % 8));
		}
		error = bt_mutation_edit(mutation, tree, found, bits, length, BT_REPLACE);
		if (error != BTRFS_OK) {
			return error;
		}
	}
	*delta = before && after ? (allocate ? 1 : -1)
	    : !before && !after	 ? (allocate ? -1 : 1)
				 : 0;
	return BTRFS_OK;
}

void
bt_fst_thresholds(uint64_t sector, uint64_t length, uint32_t *high, uint32_t *low)
{
	uint64_t range = sector * BT_FREE_SPACE_BITMAP_BYTES * 8;
	uint64_t bitmaps = length / range + (length % range != 0);
	uint64_t total = bitmaps * (sizeof(struct bt_disk_item) + BT_FREE_SPACE_BITMAP_BYTES);

	_Static_assert(sizeof(struct bt_disk_item) == 25, "Linux struct btrfs_item");
	*high = total / sizeof(struct bt_disk_item) > UINT32_MAX
	    ? UINT32_MAX
	    : (uint32_t)(total / sizeof(struct bt_disk_item));
	*low = *high > BT_FST_THRESHOLD_MARGIN ? *high - BT_FST_THRESHOLD_MARGIN : 0;
}

/* One bit per sector of the group, as convert_free_space_to_bitmaps and
 * convert_free_space_to_extents build it in memory. */
static uint8_t *
bt_fst_group_bits(const struct btrfs_fs *view, const struct bt_chunk *chunk, size_t *size)
{
	uint8_t *bits;

	*size = (size_t)((chunk->length / view->info.sector_size + 7) / 8);
	bits = view->env.allocate(view->env.context, *size);
	if (bits != NULL) {
		bt_zero(bits, *size);
	}
	return bits;
}

/* Replaces the group's free extent items with bitmap items of
 * BT_FREE_SPACE_BITMAP_BYTES * 8 sectors each, the last one shorter; the
 * extent count stays. expected is the count the info item records. */
static enum btrfs_result
bt_fst_to_bitmaps(struct bt_mutation *mutation, struct bt_root *tree, const struct bt_chunk *chunk,
    uint32_t expected)
{
	const struct btrfs_fs *view = bt_mutation_view(mutation);
	uint64_t sector = view->info.sector_size;
	uint64_t range = sector * BT_FREE_SPACE_BITMAP_BYTES * 8;
	uint64_t end = chunk->logical + chunk->length;
	uint64_t position;
	uint64_t piece;
	uint64_t index;
	uint64_t count = 0;
	struct bt_key key;
	struct bt_key found;
	uint8_t *bits;
	size_t size;
	int present = 1;
	enum btrfs_result error = BTRFS_OK;

	bits = bt_fst_group_bits(view, chunk, &size);
	if (bits == NULL) {
		return BTRFS_NO_MEMORY;
	}
	/* Each pass marks and removes the group's first free extent item. */
	while (error == BTRFS_OK) {
		key = (struct bt_key){ chunk->logical, 0, BT_FREE_SPACE_EXTENT };
		error = bt_fst_after(mutation, *tree, key, &found, &present);
		if (error != BTRFS_OK || !present || found.objectid >= end) {
			break;
		}
		if (count == expected || found.objectid < chunk->logical || found.offset == 0 ||
		    found.offset > end - found.objectid || found.objectid % sector != 0 ||
		    found.offset % sector != 0) {
			error = BTRFS_CORRUPT;
			break;
		}
		for (index = (found.objectid - chunk->logical) / sector;
		    index < (found.objectid + found.offset - chunk->logical) / sector; index++) {
			bits[index / 8] |= (uint8_t)(1U << (index % 8));
		}
		count++;
		error = bt_mutation_edit(mutation, tree, found, NULL, 0, BT_DELETE);
	}
	if (error == BTRFS_OK && count != expected) {
		error = BTRFS_CORRUPT;
	}
	for (position = chunk->logical; error == BTRFS_OK && position < end; position += piece) {
		piece = end - position < range ? end - position : range;
		key = (struct bt_key){ position, piece, BT_FREE_SPACE_BITMAP };
		error = bt_mutation_edit(mutation, tree, key,
		    bits + (position - chunk->logical) / sector / 8,
		    (size_t)((piece / sector + 7) / 8), BT_INSERT);
	}
	view->env.release(view->env.context, bits, size);
	return error;
}

/* Replaces the group's bitmap items, which must tile it in order, with one
 * free extent item per run of set bits; the runs must number expected. */
static enum btrfs_result
bt_fst_to_extents(struct bt_mutation *mutation, struct bt_root *tree, const struct bt_chunk *chunk,
    uint32_t expected)
{
	const struct btrfs_fs *view = bt_mutation_view(mutation);
	uint64_t sector = view->info.sector_size;
	uint64_t range = sector * BT_FREE_SPACE_BITMAP_BYTES * 8;
	uint64_t end = chunk->logical + chunk->length;
	uint64_t sectors = chunk->length / sector;
	uint64_t position = chunk->logical;
	uint64_t index;
	uint64_t first;
	uint64_t count = 0;
	struct bt_key key;
	struct bt_key found;
	uint8_t item[BT_FREE_SPACE_BITMAP_BYTES];
	uint8_t *bits;
	size_t size;
	size_t length = 0;
	int present;
	enum btrfs_result error = BTRFS_OK;

	bits = bt_fst_group_bits(view, chunk, &size);
	if (bits == NULL) {
		return BTRFS_NO_MEMORY;
	}
	while (error == BTRFS_OK && position < end) {
		key = (struct bt_key){ chunk->logical, 0, BT_FREE_SPACE_BITMAP };
		error = bt_fst_after(mutation, *tree, key, &found, &present);
		if (error == BTRFS_OK &&
		    (!present || found.objectid != position ||
			found.offset != (end - position < range ? end - position : range))) {
			error = BTRFS_CORRUPT;
		}
		if (error == BTRFS_OK) {
			error =
			    bt_mutation_find(mutation, *tree, found, item, sizeof(item), &length);
			if (error == BTRFS_RANGE ||
			    (error == BTRFS_OK && length != (found.offset / sector + 7) / 8)) {
				error = BTRFS_CORRUPT;
			}
		}
		if (error == BTRFS_OK) {
			bt_copy(bits + (position - chunk->logical) / sector / 8, item, length);
			error = bt_mutation_edit(mutation, tree, found, NULL, 0, BT_DELETE);
			position += found.offset;
		}
	}
	for (index = 0; error == BTRFS_OK && index < sectors;) {
		for (; index < sectors && !bt_bit(bits, index); index++) {
		}
		for (first = index; index < sectors && bt_bit(bits, index); index++) {
		}
		if (first == index) {
			break;
		}
		if (count == expected) {
			error = BTRFS_CORRUPT;
			break;
		}
		key = (struct bt_key){ chunk->logical + first * sector, (index - first) * sector,
			BT_FREE_SPACE_EXTENT };
		error = bt_mutation_edit(mutation, tree, key, NULL, 0, BT_INSERT);
		count++;
	}
	if (error == BTRFS_OK && count != expected) {
		error = BTRFS_CORRUPT;
	}
	view->env.release(view->env.context, bits, size);
	return error;
}

enum btrfs_result
bt_fst_remove_group(
    struct bt_mutation *mutation, struct bt_root *tree, const struct bt_chunk *chunk)
{
	const struct btrfs_fs *view = bt_mutation_view(mutation);
	struct bt_key key = { chunk->logical, 0, 0 };
	struct bt_key found;
	uint64_t end = chunk->logical + chunk->length;
	uint64_t limit = chunk->length / view->info.sector_size + 2;
	uint64_t count;
	int present;
	enum btrfs_result error = BTRFS_OK;

	/* The info item, then at most one free extent item per two sectors or one
	 * bitmap item per bitmap range. */
	for (count = 0; error == BTRFS_OK; count++) {
		found = (struct bt_key){ UINT64_MAX, UINT64_MAX, UINT8_MAX };
		error = bt_fst_after(mutation, *tree, key, &found, &present);
		if (error != BTRFS_OK || found.objectid >= end) {
			break;
		}
		if (count == limit ||
		    (found.type != BT_FREE_SPACE_INFO && found.type != BT_FREE_SPACE_EXTENT &&
			found.type != BT_FREE_SPACE_BITMAP)) {
			return BTRFS_CORRUPT;
		}
		error = bt_mutation_edit(mutation, tree, found, NULL, 0, BT_DELETE);
	}
	return error;
}

static enum btrfs_result
bt_fst_apply(struct bt_mutation *mutation, struct bt_root *tree, const struct bt_chunk *chunk,
    uint64_t start, uint64_t length, int allocate, struct bt_disk_free_space_info *info)
{
	uint64_t sector = bt_mutation_view(mutation)->info.sector_size;
	uint64_t end = chunk->logical + chunk->length;
	int64_t delta = 0;
	uint32_t count;
	uint32_t flags;
	uint32_t high;
	uint32_t low;
	enum btrfs_result error;

	if (length == 0 || start < chunk->logical || start >= end || length > end - start ||
	    start % sector != 0 || length % sector != 0) {
		return BTRFS_CORRUPT;
	}
	if (!(bt_u32(info->flags) & BT_FREE_SPACE_USING_BITMAPS)) {
		error =
		    bt_fst_extents(mutation, tree, chunk, start, start + length, allocate, &delta);
	} else {
		error =
		    bt_fst_bitmaps(mutation, tree, chunk, start, start + length, allocate, &delta);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	if ((int64_t)bt_u32(info->extent_count) + delta < 0 ||
	    (int64_t)bt_u32(info->extent_count) + delta > UINT32_MAX) {
		return BTRFS_CORRUPT;
	}
	count = (uint32_t)((int64_t)bt_u32(info->extent_count) + delta);
	flags = bt_u32(info->flags);
	/* Keep conversions bounded by Linux's thresholds, including during a
	 * batch: deferring them could exhaust nodes on a fragmented group. */
	bt_fst_thresholds(sector, chunk->length, &high, &low);
	if (delta != 0 && !(flags & BT_FREE_SPACE_USING_BITMAPS) && count > high) {
		error = bt_fst_to_bitmaps(mutation, tree, chunk, count);
		flags |= BT_FREE_SPACE_USING_BITMAPS;
	} else if (delta != 0 && (flags & BT_FREE_SPACE_USING_BITMAPS) && count < low) {
		error = bt_fst_to_extents(mutation, tree, chunk, count);
		flags &= ~BT_FREE_SPACE_USING_BITMAPS;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	bt_put32(&info->extent_count, count);
	bt_put32(&info->flags, flags);
	return BTRFS_OK;
}

enum btrfs_result
bt_fst_changes(struct bt_mutation *mutation, struct bt_root *tree, const struct bt_chunk *chunk,
    const struct bt_space_change *changes, size_t count)
{
	struct bt_disk_free_space_info original;
	struct bt_disk_free_space_info info;
	struct bt_key key = { chunk->logical, chunk->length, BT_FREE_SPACE_INFO };
	size_t i;
	enum btrfs_result error;

	if (bt_mutation_view(mutation) == NULL || count == 0 || count > BT_SPACE_MAX_CHANGES) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_fst_info(mutation, *tree, chunk, &info);
	if (error != BTRFS_OK) {
		return error;
	}
	if (bt_u32(info.flags) & ~BT_FREE_SPACE_USING_BITMAPS) {
		return BTRFS_UNSUPPORTED;
	}
	original = info;
	for (i = 0; i < count; i++) {
		error = bt_fst_apply(mutation, tree, chunk, changes[i].start, changes[i].length,
		    changes[i].allocate, &info);
		if (error != BTRFS_OK) {
			return error;
		}
	}
	return bt_equal(&original, &info, sizeof(info))
	    ? BTRFS_OK
	    : bt_mutation_edit(mutation, tree, key, &info, sizeof(info), BT_REPLACE);
}

enum btrfs_result
bt_fst_change(struct bt_mutation *mutation, struct bt_root *tree, const struct bt_chunk *chunk,
    uint64_t start, uint64_t length, int allocate)
{
	struct bt_space_change change = { start, length, 0, allocate };

	return bt_fst_changes(mutation, tree, chunk, &change, 1);
}
