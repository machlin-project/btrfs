/* SPDX-License-Identifier: BSD-3-Clause */
#include "csum.h"
#include "encode.h"

struct bt_csum_item {
	struct bt_key key;
	uint64_t end;
	size_t size;
};

/* Bytes of one sector's checksum in an item. */
static size_t
bt_csum_bytes(const struct btrfs_fs *view)
{
	return bt_checksum_size(view->info.checksum_type);
}

/* Linux's per-item limit leaves room for two item headers in a leaf. */
static size_t
bt_csum_item_limit(const struct btrfs_fs *view)
{
	return (view->info.node_size - sizeof(struct bt_disk_header) -
		   2 * sizeof(struct bt_disk_item)) /
	    bt_csum_bytes(view) -
	    1;
}

static int
bt_csum_decode(const struct btrfs_fs *view, const struct bt_record *record, uint64_t end,
    struct bt_csum_item *item)
{
	uint64_t sectors = record->size / bt_csum_bytes(view);

	if (record->key.objectid != BT_CSUM_OBJECTID || record->key.type != BT_EXTENT_CSUM ||
	    record->key.offset >= end) {
		return 0;
	}
	item->key = record->key;
	item->size = record->size;
	item->end = record->key.offset + sectors * view->info.sector_size;
	return 1;
}

/* Finds the first item overlapping [logical, end): the predecessor of logical
 * when it reaches past logical, otherwise the first item at or after it. */
static enum btrfs_result
bt_csum_find(struct bt_mutation *mutation, struct bt_root root, uint64_t logical, uint64_t end,
    struct bt_csum_item *item, int *found)
{
	const struct btrfs_fs *view = bt_mutation_view(mutation);
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = {
		.objectid = BT_CSUM_OBJECTID, .type = BT_EXTENT_CSUM, .offset = logical
	};
	enum btrfs_result error;
	int pass;

	*found = 0;
	if (view == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	bt_cursor_init(&cursor, view, root);
	error = BTRFS_OK;
	for (pass = 1; pass >= 0 && !*found && error == BTRFS_OK; pass--) {
		error = bt_cursor_seek(&cursor, key, pass);
		if (error == BTRFS_OK) {
			(void)bt_cursor_record(&cursor, &record);
			if (record.key.objectid == BT_CSUM_OBJECTID &&
			    record.key.type == BT_EXTENT_CSUM &&
			    (record.size == 0 || record.size % bt_csum_bytes(view) != 0 ||
				record.key.offset % view->info.sector_size != 0)) {
				error = BTRFS_CORRUPT;
			} else if (bt_csum_decode(view, &record, end, item) &&
			    item->end > logical) {
				*found = 1;
			}
		} else if (error == BTRFS_NOT_FOUND) {
			error = BTRFS_OK;
		}
	}
	bt_cursor_fini(&cursor);
	return error;
}

enum btrfs_result
bt_csum_exists(struct bt_mutation *mutation, struct bt_root checksums, uint64_t logical,
    uint64_t length, int *exists)
{
	struct bt_csum_item item;

	if (length == 0 || length > UINT64_MAX - logical) {
		return BTRFS_INVALID_ARGUMENT;
	}
	return bt_csum_find(mutation, checksums, logical, logical + length, &item, exists);
}

/* Inserts the checksums of [logical, logical + length) in items of at most
 * Linux's per-item count: computed from data, or copied from sums. */
static enum btrfs_result
bt_csum_store(struct bt_mutation *mutation, struct bt_root *checksums, uint64_t logical,
    const uint8_t *data, const uint8_t *sums, uint64_t length)
{
	const struct btrfs_fs *view = bt_mutation_view(mutation);
	struct bt_csum_item existing;
	struct bt_key key = { .objectid = BT_CSUM_OBJECTID, .type = BT_EXTENT_CSUM };
	uint8_t *buffer;
	uint64_t done;
	uint64_t sector;
	size_t size;
	size_t limit;
	size_t count;
	size_t batch;
	size_t i;
	int found;
	enum btrfs_result error;

	if (view == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	sector = view->info.sector_size;
	size = bt_csum_bytes(view);
	limit = bt_checksum_batch(view);
	if (length == 0 || logical % sector != 0 || length % sector != 0 ||
	    length > UINT64_MAX - logical) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_csum_find(mutation, *checksums, logical, logical + length, &existing, &found);
	if (error != BTRFS_OK || found) {
		return error == BTRFS_OK ? BTRFS_CORRUPT : error;
	}
	buffer = view->env.allocate(view->env.context, view->info.node_size);
	if (buffer == NULL) {
		return BTRFS_NO_MEMORY;
	}
	for (done = 0; error == BTRFS_OK && done < length; done += count * sector) {
		count = (length - done) / sector < bt_csum_item_limit(view)
		    ? (size_t)((length - done) / sector)
		    : bt_csum_item_limit(view);
		if (sums != NULL) {
			bt_copy(buffer, sums + (size_t)(done / sector) * size, count * size);
		}
		for (i = 0; sums == NULL && i < count; i += batch) {
			batch = count - i < limit ? count - i : limit;
			bt_checksum_sectors(
			    view, data + done + i * sector, batch, buffer + i * size);
		}
		key.offset = logical + done;
		error = bt_mutation_edit(mutation, checksums, key, buffer, count * size, BT_INSERT);
	}
	view->env.release(view->env.context, buffer, view->info.node_size);
	return error;
}

enum btrfs_result
bt_csum_insert(struct bt_mutation *mutation, struct bt_root *checksums, uint64_t logical,
    const uint8_t *data, uint64_t length)
{
	return bt_csum_store(mutation, checksums, logical, data, NULL, length);
}

enum btrfs_result
bt_csum_insert_sums(struct bt_mutation *mutation, struct bt_root *checksums, uint64_t logical,
    const uint8_t *sums, uint64_t length)
{
	return bt_csum_store(mutation, checksums, logical, NULL, sums, length);
}

enum btrfs_result
bt_csum_delete(
    struct bt_mutation *mutation, struct bt_root *checksums, uint64_t logical, uint64_t length)
{
	const struct btrfs_fs *view = bt_mutation_view(mutation);
	struct bt_csum_item item;
	struct bt_key tail;
	uint8_t *buffer;
	uint64_t end;
	uint64_t first;
	uint64_t last;
	uint64_t sector;
	uint64_t steps;
	size_t size;
	int found = 1;
	enum btrfs_result error = BTRFS_OK;

	if (view == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	sector = view->info.sector_size;
	if (logical % sector != 0 || length % sector != 0 || length > UINT64_MAX - logical) {
		return BTRFS_INVALID_ARGUMENT;
	}
	end = logical + length;
	buffer = view->env.allocate(view->env.context, view->info.node_size);
	if (buffer == NULL) {
		return BTRFS_NO_MEMORY;
	}
	for (steps = 0; error == BTRFS_OK && found; steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		error = bt_csum_find(mutation, *checksums, logical, end, &item, &found);
		if (error != BTRFS_OK || !found) {
			break;
		}
		error = bt_mutation_find(
		    mutation, *checksums, item.key, buffer, view->info.node_size, &size);
		if (error != BTRFS_OK) {
			break;
		}
		first = item.key.offset > logical ? item.key.offset : logical;
		last = item.end < end ? item.end : end;
		tail = item.key;
		tail.offset = last;
		if (first == item.key.offset) {
			error = bt_mutation_edit(mutation, checksums, item.key, NULL, 0, BT_DELETE);
		} else {
			error = bt_mutation_edit(mutation, checksums, item.key, buffer,
			    (size_t)((first - item.key.offset) / sector) * bt_csum_bytes(view),
			    BT_REPLACE);
		}
		if (error == BTRFS_OK && last < item.end) {
			error = bt_mutation_edit(mutation, checksums, tail,
			    buffer +
				(size_t)((last - item.key.offset) / sector) * bt_csum_bytes(view),
			    (size_t)((item.end - last) / sector) * bt_csum_bytes(view), BT_INSERT);
		}
	}
	view->env.release(view->env.context, buffer, view->info.node_size);
	return error;
}
