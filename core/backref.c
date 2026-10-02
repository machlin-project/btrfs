/* SPDX-License-Identifier: BSD-3-Clause */
#include "backref.h"
#include "encode.h"

#define BT_TREE_REF_SIZE (1U + sizeof(struct bt_le64))
#define BT_DATA_REF_SIZE (1U + sizeof(struct bt_disk_data_ref))
#define BT_SHARED_DATA_REF_SIZE                                                                    \
	(1U + sizeof(struct bt_le64) + sizeof(struct bt_disk_shared_data_ref))
#define BT_DATA_REF_PROBES 65536U

/* A copied extent item and the position of one inline reference in it. */
struct bt_extent_copy {
	const struct btrfs_fs *view;
	uint8_t *bytes;
	size_t size;
	size_t start;
	size_t position;
	int found;
};

uint64_t
bt_backref_data_hash(uint64_t root, uint64_t inode, uint64_t offset)
{
	struct bt_le64 value;
	uint32_t high = UINT32_MAX;
	uint32_t low = UINT32_MAX;

	bt_put64(&value, root);
	high = bt_crc32c(high, &value, sizeof(value));
	bt_put64(&value, inode);
	low = bt_crc32c(low, &value, sizeof(value));
	bt_put64(&value, offset);
	low = bt_crc32c(low, &value, sizeof(value));
	return ((uint64_t)high << 31) ^ (uint64_t)low;
}

static uint8_t
bt_ref_type(const struct bt_backref *reference)
{
	if (reference->data) {
		return reference->parent != 0 ? BT_SHARED_DATA_REF : BT_EXTENT_DATA_REF;
	}
	return reference->parent != 0 ? BT_SHARED_BLOCK_REF : BT_TREE_BLOCK_REF;
}

static size_t
bt_ref_size(uint8_t type)
{
	switch (type) {
	case BT_TREE_BLOCK_REF:
	case BT_SHARED_BLOCK_REF:
		return BT_TREE_REF_SIZE;
	case BT_EXTENT_DATA_REF:
		return BT_DATA_REF_SIZE;
	case BT_SHARED_DATA_REF:
		return BT_SHARED_DATA_REF_SIZE;
	default:
		return 0;
	}
}

static int
bt_ref_matches(const struct bt_disk_data_ref *wire, const struct bt_backref *reference)
{
	return bt_u64(wire->root) == reference->root &&
	    bt_u64(wire->objectid) == reference->inode && bt_u64(wire->offset) == reference->offset;
}

static enum btrfs_result
bt_extent_load(struct bt_mutation *mutation, struct bt_root extents, struct bt_key extent,
    struct bt_extent_copy *copy)
{
	const struct bt_disk_extent_item *item;
	uint64_t flags;
	enum btrfs_result error;

	error = bt_mutation_find(mutation, extents, extent, copy->bytes,
	    bt_mutation_view(mutation)->info.node_size, &copy->size);
	if (error != BTRFS_OK) {
		return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
	}
	if (copy->size < sizeof(*item)) {
		return BTRFS_CORRUPT;
	}
	item = (const void *)copy->bytes;
	flags = bt_u64(item->flags);
	copy->start = sizeof(*item);
	if (extent.type == BT_METADATA_ITEM
		? !(flags & BT_EXTENT_FLAG_TREE)
		: (flags & BT_EXTENT_FLAG_TREE) != 0 || !(flags & BT_EXTENT_FLAG_DATA)) {
		return BTRFS_CORRUPT;
	}
	return bt_u64(item->refs) == 0 ? BTRFS_CORRUPT : BTRFS_OK;
}

/* Linux orders inline references by ascending type and, within one type, by
 * descending root/parent or data-reference hash. Finds a match or the position
 * where the reference belongs. */
static enum btrfs_result
bt_extent_search(struct bt_extent_copy *copy, const struct bt_backref *reference)
{
	const struct bt_disk_data_ref *wire;
	struct bt_le64 value;
	uint64_t existing;
	uint64_t wanted = reference->parent != 0 ? reference->parent : reference->root;
	uint64_t hash = 0;
	uint8_t type = bt_ref_type(reference);
	uint8_t found;
	size_t size;

	if (type == BT_EXTENT_DATA_REF) {
		hash = bt_backref_data_hash(reference->root, reference->inode, reference->offset);
	}
	copy->found = 0;
	for (copy->position = copy->start; copy->position < copy->size; copy->position += size) {
		found = copy->bytes[copy->position];
		size = bt_ref_size(found);
		if (size == 0 || size > copy->size - copy->position) {
			return BTRFS_CORRUPT;
		}
		if (type < found) {
			return BTRFS_OK;
		}
		if (type > found) {
			continue;
		}
		if (type == BT_EXTENT_DATA_REF) {
			wire = (const void *)(copy->bytes + copy->position + 1);
			if (bt_ref_matches(wire, reference)) {
				copy->found = 1;
				return BTRFS_OK;
			}
			existing = bt_backref_data_hash(
			    bt_u64(wire->root), bt_u64(wire->objectid), bt_u64(wire->offset));
			if (existing < hash) {
				return BTRFS_OK;
			}
		} else {
			bt_copy(&value, copy->bytes + copy->position + 1, sizeof(value));
			existing = bt_u64(value);
			if (existing == wanted) {
				copy->found = 1;
				return BTRFS_OK;
			}
			if (existing < wanted) {
				return BTRFS_OK;
			}
		}
	}
	return BTRFS_OK;
}

static uint32_t
bt_inline_count(const struct bt_extent_copy *copy)
{
	const uint8_t *reference = copy->bytes + copy->position;
	const struct bt_disk_data_ref *data;
	const struct bt_disk_shared_data_ref *shared;

	if (reference[0] == BT_EXTENT_DATA_REF) {
		data = (const void *)(reference + 1);
		return bt_u32(data->count);
	}
	if (reference[0] == BT_SHARED_DATA_REF) {
		shared = (const void *)(reference + 1 + sizeof(struct bt_le64));
		return bt_u32(shared->count);
	}
	return 1;
}

static void
bt_inline_set_count(struct bt_extent_copy *copy, uint32_t count)
{
	uint8_t *reference = copy->bytes + copy->position;
	struct bt_disk_data_ref *data;
	struct bt_disk_shared_data_ref *shared;

	if (reference[0] == BT_EXTENT_DATA_REF) {
		data = (void *)(reference + 1);
		bt_put32(&data->count, count);
	} else if (reference[0] == BT_SHARED_DATA_REF) {
		shared = (void *)(reference + 1 + sizeof(struct bt_le64));
		bt_put32(&shared->count, count);
	}
}

static void
bt_inline_encode(uint8_t *out, const struct bt_backref *reference, uint32_t count)
{
	struct bt_disk_data_ref *data;
	struct bt_disk_shared_data_ref *shared;
	struct bt_le64 value;

	out[0] = bt_ref_type(reference);
	if (out[0] == BT_EXTENT_DATA_REF) {
		data = (void *)(out + 1);
		bt_put64(&data->root, reference->root);
		bt_put64(&data->objectid, reference->inode);
		bt_put64(&data->offset, reference->offset);
		bt_put32(&data->count, count);
		return;
	}
	bt_put64(&value, reference->parent != 0 ? reference->parent : reference->root);
	bt_copy(out + 1, &value, sizeof(value));
	if (out[0] == BT_SHARED_DATA_REF) {
		shared = (void *)(out + 1 + sizeof(value));
		bt_put32(&shared->count, count);
	}
}

static void
bt_extent_add_refs(struct bt_extent_copy *copy, int64_t change)
{
	struct bt_disk_extent_item *item = (void *)copy->bytes;

	bt_put64(&item->refs, (uint64_t)((int64_t)bt_u64(item->refs) + change));
}

/* Any keyed backreference item after the extent item prevents new inline ones. */
static enum btrfs_result
bt_keyed_present(
    struct bt_mutation *mutation, struct bt_root extents, uint64_t bytenr, int *present)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = bytenr, .type = BT_METADATA_ITEM + 1 };
	enum btrfs_result error;

	*present = 0;
	bt_cursor_init(&cursor, bt_mutation_view(mutation), extents);
	error = bt_cursor_seek(&cursor, key, 0);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		*present = record.key.objectid == bytenr && record.key.type < BT_BLOCK_GROUP_ITEM;
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* Keyed data references start at their hash and probe upward on collision. */
static enum btrfs_result
bt_keyed_data(struct bt_mutation *mutation, struct bt_root extents, uint64_t bytenr,
    const struct bt_backref *reference, struct bt_key *key, struct bt_disk_data_ref *wire,
    int *found)
{
	size_t length;
	unsigned probe;
	enum btrfs_result error;

	*found = 0;
	key->objectid = bytenr;
	key->type = BT_EXTENT_DATA_REF;
	key->offset = bt_backref_data_hash(reference->root, reference->inode, reference->offset);
	for (probe = 0; probe < BT_DATA_REF_PROBES; probe++) {
		error = bt_mutation_find(mutation, extents, *key, wire, sizeof(*wire), &length);
		if (error == BTRFS_NOT_FOUND) {
			return BTRFS_OK;
		}
		if (error != BTRFS_OK) {
			return error == BTRFS_RANGE ? BTRFS_CORRUPT : error;
		}
		if (length != sizeof(*wire) || bt_u32(wire->count) == 0) {
			return BTRFS_CORRUPT;
		}
		if (bt_ref_matches(wire, reference)) {
			*found = 1;
			return BTRFS_OK;
		}
		if (key->offset == UINT64_MAX) {
			return BTRFS_UNSUPPORTED;
		}
		key->offset++;
	}
	return BTRFS_UNSUPPORTED;
}

static enum btrfs_result
bt_keyed_change(struct bt_mutation *mutation, struct bt_root *extents, uint64_t bytenr,
    const struct bt_backref *reference, int64_t change)
{
	struct bt_disk_data_ref wire;
	struct bt_disk_shared_data_ref shared;
	struct bt_key key = { .objectid = bytenr, .type = bt_ref_type(reference) };
	uint64_t count;
	size_t length;
	int found;
	enum btrfs_result error;

	if (key.type == BT_EXTENT_DATA_REF) {
		error = bt_keyed_data(mutation, *extents, bytenr, reference, &key, &wire, &found);
		if (error != BTRFS_OK) {
			return error;
		}
		if (!found) {
			if (change < 0) {
				return BTRFS_CORRUPT;
			}
			bt_put64(&wire.root, reference->root);
			bt_put64(&wire.objectid, reference->inode);
			bt_put64(&wire.offset, reference->offset);
			bt_put32(&wire.count, 0);
		}
		count = (uint64_t)((int64_t)bt_u32(wire.count) + change);
		if (count > UINT32_MAX) {
			return change < 0 ? BTRFS_CORRUPT : BTRFS_UNSUPPORTED;
		}
		bt_put32(&wire.count, (uint32_t)count);
		return bt_mutation_edit(mutation, extents, key, count == 0 ? NULL : &wire,
		    count == 0 ? 0 : sizeof(wire), count == 0 ? BT_DELETE : BT_UPSERT);
	}
	key.offset = reference->parent != 0 ? reference->parent : reference->root;
	error = bt_mutation_find(mutation, *extents, key, &shared, sizeof(shared), &length);
	found = error == BTRFS_OK;
	if (error != BTRFS_OK && error != BTRFS_NOT_FOUND) {
		return error == BTRFS_RANGE ? BTRFS_CORRUPT : error;
	}
	if (key.type != BT_SHARED_DATA_REF) {
		/* Tree references have no count: one item per referencing root/parent. */
		if (found ? length != 0 || change != -1 : change != 1) {
			return BTRFS_CORRUPT;
		}
		return bt_mutation_edit(
		    mutation, extents, key, NULL, 0, found ? BT_DELETE : BT_INSERT);
	}
	if (found && length != sizeof(shared)) {
		return BTRFS_CORRUPT;
	}
	count = (uint64_t)((int64_t)(found ? bt_u32(shared.count) : 0) + change);
	if (count > UINT32_MAX || (!found && change < 0)) {
		return BTRFS_CORRUPT;
	}
	bt_put32(&shared.count, (uint32_t)count);
	return bt_mutation_edit(mutation, extents, key, count == 0 ? NULL : &shared,
	    count == 0 ? 0 : sizeof(shared), count == 0 ? BT_DELETE : BT_UPSERT);
}

static enum btrfs_result
bt_extent_begin(struct bt_mutation *mutation, struct bt_extent_copy *copy)
{
	const struct btrfs_fs *view = bt_mutation_view(mutation);

	bt_zero(copy, sizeof(*copy));
	if (view == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	copy->view = view;
	copy->bytes = view->env.allocate(view->env.context, view->info.node_size);
	return copy->bytes == NULL ? BTRFS_NO_MEMORY : BTRFS_OK;
}

/* The view pointer is kept: a failed edit poisons the mutation's view. */
static void
bt_extent_end(struct bt_extent_copy *copy)
{
	if (copy->bytes != NULL) {
		copy->view->env.release(
		    copy->view->env.context, copy->bytes, copy->view->info.node_size);
	}
}

static int
bt_reference_valid(struct bt_key extent, const struct bt_backref *reference, uint32_t count)
{
	return count != 0 && (extent.type == BT_METADATA_ITEM || extent.type == BT_EXTENT_ITEM) &&
	    (extent.type == BT_EXTENT_ITEM) == (reference->data != 0) &&
	    (reference->data || count == 1) && (reference->parent != 0 || reference->root != 0);
}

enum btrfs_result
bt_backref_info(struct bt_mutation *mutation, struct bt_root extents, struct bt_key extent,
    uint64_t *refs, uint64_t *flags)
{
	const struct bt_disk_extent_item *item;
	struct bt_extent_copy copy;
	enum btrfs_result error;

	error = bt_extent_begin(mutation, &copy);
	if (error == BTRFS_OK) {
		error = bt_extent_load(mutation, extents, extent, &copy);
	}
	if (error == BTRFS_OK) {
		item = (const void *)copy.bytes;
		*refs = bt_u64(item->refs);
		*flags = bt_u64(item->flags);
	}
	bt_extent_end(&copy);
	return error;
}

enum btrfs_result
bt_backref_count(struct bt_mutation *mutation, struct bt_root extents, struct bt_key extent,
    const struct bt_backref *reference, uint64_t *count)
{
	struct bt_extent_copy copy;
	struct bt_disk_data_ref wire;
	struct bt_disk_shared_data_ref shared;
	struct bt_key key;
	size_t length;
	int found = 0;
	enum btrfs_result error;

	*count = 0;
	error = bt_extent_begin(mutation, &copy);
	if (error == BTRFS_OK) {
		error = bt_extent_load(mutation, extents, extent, &copy);
	}
	if (error == BTRFS_OK) {
		error = bt_extent_search(&copy, reference);
	}
	if (error == BTRFS_OK && copy.found) {
		*count = bt_inline_count(&copy);
	} else if (error == BTRFS_OK && bt_ref_type(reference) == BT_EXTENT_DATA_REF) {
		error = bt_keyed_data(
		    mutation, extents, extent.objectid, reference, &key, &wire, &found);
		*count = error == BTRFS_OK && found ? bt_u32(wire.count) : 0;
	} else if (error == BTRFS_OK) {
		key = (struct bt_key){ extent.objectid,
			reference->parent != 0 ? reference->parent : reference->root,
			bt_ref_type(reference) };
		error = bt_mutation_find(mutation, extents, key, &shared, sizeof(shared), &length);
		if (error == BTRFS_OK) {
			*count = key.type == BT_SHARED_DATA_REF && length == sizeof(shared)
			    ? bt_u32(shared.count)
			    : 1;
		}
		error = error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
	}
	bt_extent_end(&copy);
	return error == BTRFS_RANGE ? BTRFS_CORRUPT : error;
}

enum btrfs_result
bt_backref_add(struct bt_mutation *mutation, struct bt_root *extents, struct bt_key extent,
    const struct bt_backref *reference, uint32_t count)
{
	struct bt_extent_copy copy;
	uint64_t total;
	size_t size = bt_ref_size(bt_ref_type(reference));
	size_t limit;
	int keyed = 0;
	enum btrfs_result error;

	if (!bt_reference_valid(extent, reference, count)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_extent_begin(mutation, &copy);
	if (error == BTRFS_OK) {
		error = bt_extent_load(mutation, *extents, extent, &copy);
	}
	if (error == BTRFS_OK) {
		error = bt_extent_search(&copy, reference);
	}
	if (error == BTRFS_OK && copy.found) {
		total = (uint64_t)bt_inline_count(&copy) + count;
		if (!reference->data || total > UINT32_MAX) {
			error = reference->data ? BTRFS_UNSUPPORTED : BTRFS_CORRUPT;
		} else {
			bt_inline_set_count(&copy, (uint32_t)total);
		}
	} else if (error == BTRFS_OK) {
		limit =
		    ((bt_mutation_view(mutation)->info.node_size - sizeof(struct bt_disk_header)) >>
			4) -
		    sizeof(struct bt_disk_item);
		error = bt_keyed_present(mutation, *extents, extent.objectid, &keyed);
		if (error == BTRFS_OK && (keyed || copy.size + size >= limit)) {
			keyed = 1;
			error =
			    bt_keyed_change(mutation, extents, extent.objectid, reference, count);
		} else if (error == BTRFS_OK) {
			bt_move(copy.bytes + copy.position + size, copy.bytes + copy.position,
			    copy.size - copy.position);
			bt_inline_encode(copy.bytes + copy.position, reference, count);
			copy.size += size;
		}
	}
	if (error == BTRFS_OK) {
		bt_extent_add_refs(&copy, count);
		error =
		    bt_mutation_edit(mutation, extents, extent, copy.bytes, copy.size, BT_REPLACE);
	}
	bt_extent_end(&copy);
	return error;
}

/* Apply a drop to an item already loaded by either the general reference
 * operation or the CoW accounting pass. */
static enum btrfs_result
bt_extent_drop(struct bt_mutation *mutation, struct bt_root *extents, struct bt_key extent,
    const struct bt_backref *reference, uint32_t count, struct bt_extent_copy *copy, int *freed)
{
	const struct bt_disk_extent_item *item;
	uint32_t remaining;
	size_t size = bt_ref_size(bt_ref_type(reference));
	enum btrfs_result error = BTRFS_OK;

	if (error == BTRFS_OK &&
	    bt_u64(((const struct bt_disk_extent_item *)copy->bytes)->refs) < count) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		error = bt_extent_search(copy, reference);
	}
	if (error == BTRFS_OK && copy->found) {
		remaining = bt_inline_count(copy);
		if (remaining < count) {
			error = BTRFS_CORRUPT;
		} else if (remaining == count) {
			bt_move(copy->bytes + copy->position, copy->bytes + copy->position + size,
			    copy->size - copy->position - size);
			copy->size -= size;
		} else {
			bt_inline_set_count(copy, remaining - count);
		}
	} else if (error == BTRFS_OK) {
		error =
		    bt_keyed_change(mutation, extents, extent.objectid, reference, -(int64_t)count);
	}
	if (error == BTRFS_OK) {
		bt_extent_add_refs(copy, -(int64_t)count);
		item = (const void *)copy->bytes;
		if (bt_u64(item->refs) != 0) {
			error = bt_mutation_edit(
			    mutation, extents, extent, copy->bytes, copy->size, BT_REPLACE);
		} else if (copy->size != copy->start) {
			/* A zero count with references still listed is inconsistent. */
			error = BTRFS_CORRUPT;
		} else {
			error = bt_mutation_edit(mutation, extents, extent, NULL, 0, BT_DELETE);
			*freed = error == BTRFS_OK;
		}
	}
	return error;
}

enum btrfs_result
bt_backref_drop(struct bt_mutation *mutation, struct bt_root *extents, struct bt_key extent,
    const struct bt_backref *reference, uint32_t count, int *freed)
{
	struct bt_extent_copy copy;
	enum btrfs_result error;

	*freed = 0;
	if (!bt_reference_valid(extent, reference, count)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_extent_begin(mutation, &copy);
	if (error == BTRFS_OK) {
		error = bt_extent_load(mutation, *extents, extent, &copy);
	}
	if (error == BTRFS_OK) {
		error = bt_extent_drop(mutation, extents, extent, reference, count, &copy, freed);
	}
	bt_extent_end(&copy);
	return error;
}

enum btrfs_result
bt_backref_release_tree(struct bt_mutation *mutation, struct bt_root *extents, struct bt_key extent,
    uint64_t owner, const struct bt_root *replacement, uint64_t *refs, uint64_t *flags, int *freed,
    int *replaced)
{
	struct bt_disk_extent_item *item;
	struct bt_extent_copy copy;
	struct bt_backref reference = { .root = owner };
	struct bt_key target;
	enum btrfs_result error;

	*freed = 0;
	*replaced = 0;
	if (extent.type != BT_METADATA_ITEM) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_extent_begin(mutation, &copy);
	if (error == BTRFS_OK) {
		error = bt_extent_load(mutation, *extents, extent, &copy);
	}
	if (error == BTRFS_OK) {
		item = (void *)copy.bytes;
		*refs = bt_u64(item->refs);
		*flags = bt_u64(item->flags);
		if (owner != 0 && *refs == 1 && !(*flags & BT_EXTENT_FLAG_FULL_BACKREF)) {
			error = bt_extent_search(&copy, &reference);
			if (error == BTRFS_OK && replacement != NULL && copy.found &&
			    copy.size == copy.start + BT_TREE_REF_SIZE) {
				target = (struct bt_key){ .objectid = replacement->address,
					.type = BT_METADATA_ITEM,
					.offset = replacement->level };
				bt_put64(&item->generation, replacement->generation);
				bt_put64(&item->flags, BT_EXTENT_FLAG_TREE);
				error = bt_mutation_rekey(
				    mutation, extents, extent, target, copy.bytes, copy.size);
				*freed = *replaced = error == BTRFS_OK;
			} else if (error == BTRFS_OK) {
				error = bt_extent_drop(
				    mutation, extents, extent, &reference, 1, &copy, freed);
			}
		}
	}
	bt_extent_end(&copy);
	return error;
}

enum btrfs_result
bt_backref_set_flags(
    struct bt_mutation *mutation, struct bt_root *extents, struct bt_key extent, uint64_t flags)
{
	struct bt_disk_extent_item *item;
	struct bt_extent_copy copy;
	enum btrfs_result error;

	error = bt_extent_begin(mutation, &copy);
	if (error == BTRFS_OK) {
		error = bt_extent_load(mutation, *extents, extent, &copy);
	}
	if (error == BTRFS_OK) {
		item = (void *)copy.bytes;
		bt_put64(&item->flags, bt_u64(item->flags) | flags);
		error =
		    bt_mutation_edit(mutation, extents, extent, copy.bytes, copy.size, BT_REPLACE);
	}
	bt_extent_end(&copy);
	return error;
}
