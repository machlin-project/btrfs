/* SPDX-License-Identifier: BSD-3-Clause */
#include "namespace.h"
#include "qgroup.h"

/* Largest single data write issued to the device. */
#define BT_DATA_WRITE (1024U * 1024U)
/* Bytes one rewrite holds in memory; longer ranges are written in pieces. */
#define BT_REWRITE_CHUNK (UINT64_C(8) * 1024 * 1024)
/* Linux compresses data in pieces of at most 128 KiB (BTRFS_MAX_UNCOMPRESSED),
 * each one extent. */
#define BT_COMPRESS_CHUNK (128U * 1024U)
#define BT_FILE_SIZE_LIMIT ((UINT64_C(1) << 63) - 1)
/* Tree nodes a queued reference change edits at commit besides checksum
 * leaves: the extent item's leaf and, for a freed extent, its free-space
 * entry; parents are within the commit's own allowance. */
#define BT_REF_EDIT_NODES 1U
#define BT_REF_FREE_NODES 1U

struct bt_file_item {
	struct bt_key key;
	struct bt_disk_extent extent;
	size_t size;
	uint64_t end;
};

void
bt_tx_release_data(struct btrfs_transaction *transaction)
{
	const struct btrfs_environment *env = &transaction->base->env;

	if (transaction->refs != NULL) {
		env->release(env->context, transaction->refs,
		    BT_TRANSACTION_REFERENCES * sizeof(*transaction->refs));
	}
	transaction->refs = NULL;
	transaction->ref_count = 0;
	transaction->ref_nodes = 0;
}

size_t
bt_tx_work(const struct btrfs_transaction *transaction)
{
	return bt_mutation_count(transaction->mutation) + transaction->ref_nodes;
}

size_t
bt_tx_ref_nodes(const struct btrfs_transaction *transaction, struct bt_key extent, int add)
{
	const struct btrfs_fs *fs = transaction->base;
	uint64_t sums;
	uint64_t per_leaf;

	if (add) {
		return BT_REF_EDIT_NODES;
	}
	/* Linux's per-item limit of sums per leaf; the range may straddle one
	 * more leaf. */
	sums = extent.offset / fs->info.sector_size;
	per_leaf =
	    (fs->info.node_size - sizeof(struct bt_disk_header) - 2 * sizeof(struct bt_disk_item)) /
	    bt_checksum_size(fs->info.checksum_type);
	return BT_REF_EDIT_NODES + BT_REF_FREE_NODES + (size_t)((sums + per_leaf - 1) / per_leaf) +
	    1;
}

enum btrfs_result
bt_tx_queue(struct btrfs_transaction *transaction, struct bt_key extent,
    const struct bt_backref *reference, int add)
{
	const struct btrfs_environment *env = &transaction->base->env;

	if (transaction->refs == NULL) {
		transaction->refs = env->allocate(
		    env->context, BT_TRANSACTION_REFERENCES * sizeof(*transaction->refs));
		if (transaction->refs == NULL) {
			return BTRFS_NO_MEMORY;
		}
	}
	if (transaction->ref_count == BT_TRANSACTION_REFERENCES) {
		return BTRFS_UNSUPPORTED;
	}
	transaction->refs[transaction->ref_count++] =
	    (struct bt_file_ref){ extent, *reference, add };
	transaction->ref_nodes += bt_tx_ref_nodes(transaction, extent, add);
	return BTRFS_OK;
}

static enum btrfs_result
bt_tx_free_data(struct btrfs_transaction *transaction, struct bt_key extent)
{
	enum btrfs_result error;

	error = bt_csum_delete(
	    transaction->mutation, &transaction->checksums.root, extent.objectid, extent.offset);
	if (error == BTRFS_OK) {
		error = bt_space_change_used(transaction->space, extent.objectid, extent.offset, 0);
	}
	return error;
}

enum btrfs_result
bt_tx_apply_refs(struct btrfs_transaction *transaction)
{
	struct bt_file_ref *ref;
	size_t i;
	int pass;
	int freed;
	enum btrfs_result error = BTRFS_OK;

	for (pass = 1; pass >= 0 && error == BTRFS_OK; pass--) {
		for (i = 0; error == BTRFS_OK && i < transaction->ref_count; i++) {
			ref = &transaction->refs[i];
			if (ref->add != pass) {
				continue;
			}
			if (pass) {
				error = bt_backref_add(transaction->mutation,
				    &transaction->extents.root, ref->extent, &ref->reference, 1);
				continue;
			}
			error = bt_backref_drop(transaction->mutation, &transaction->extents.root,
			    ref->extent, &ref->reference, 1, &freed);
			if (error == BTRFS_OK && freed) {
				error = bt_tx_free_data(transaction, ref->extent);
			}
		}
	}
	transaction->ref_count = 0;
	transaction->ref_nodes = 0;
	return error;
}

/* Private read view: metadata through the mutation overlay, new data from the
 * device (written when its extent was created), and the transaction's own
 * checksum and file tree roots. */
static enum btrfs_result
bt_view_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct btrfs_transaction *transaction = context;
	const struct btrfs_fs *mutation = bt_mutation_view(transaction->mutation);

	if (mutation == NULL) {
		return transaction->failure != BTRFS_OK ? transaction->failure : BTRFS_IO;
	}
	return mutation->env.read(mutation->env.context, offset, buffer, length);
}

static void *
bt_view_allocate(void *context, size_t size)
{
	struct btrfs_transaction *transaction = context;

	return transaction->base->env.allocate(transaction->base->env.context, size);
}

static void
bt_view_release(void *context, void *allocation, size_t size)
{
	struct btrfs_transaction *transaction = context;

	transaction->base->env.release(transaction->base->env.context, allocation, size);
}

static enum btrfs_result
bt_view_decompress(void *context, enum btrfs_compression codec, const void *input,
    size_t input_size, void *output, size_t capacity, size_t *produced)
{
	struct btrfs_transaction *transaction = context;
	const struct btrfs_environment *env = &transaction->base->env;

	return env->decompress == NULL
	    ? BTRFS_UNSUPPORTED
	    : env->decompress(env->context, codec, input, input_size, output, capacity, produced);
}

static enum btrfs_result
bt_tx_view(
    struct btrfs_transaction *transaction, const struct bt_owned_root *tree, struct btrfs_fs *view)
{
	const struct btrfs_fs *mutation = bt_mutation_view(transaction->mutation);

	if (mutation == NULL) {
		return transaction->failure != BTRFS_OK ? transaction->failure
							: BTRFS_INVALID_ARGUMENT;
	}
	*view = *mutation;
	view->env.context = transaction;
	view->env.read = bt_view_read;
	view->env.allocate = bt_view_allocate;
	view->env.release = bt_view_release;
	view->env.decompress = bt_view_decompress;
	view->chunk_count = transaction->fs.chunk_count;
	view->checksum_tree = transaction->checksums.root;
	view->selected_tree = tree->root;
	return BTRFS_OK;
}

/* Reads existing bytes as this transaction sees them; bytes past EOF read zero. */
static enum btrfs_result
bt_tx_read(struct btrfs_transaction *transaction, const struct bt_owned_root *tree, uint64_t inode,
    uint64_t offset, uint8_t *buffer, size_t length)
{
	struct btrfs_fs view;
	struct btrfs_inode file;
	struct btrfs_object_id id = { tree->root.owner, inode };
	size_t completed;
	enum btrfs_result error;

	bt_zero(buffer, length);
	error = bt_tx_view(transaction, tree, &view);
	if (error == BTRFS_OK) {
		error = btrfs_get_inode(&view, id, &file);
	}
	if (error == BTRFS_OK && offset < file.size) {
		error = btrfs_read(&view, &file, offset, buffer, length, &completed);
	}
	return error;
}

static enum btrfs_result
bt_tx_inode(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t inode,
    struct bt_disk_inode *item)
{
	struct bt_key key = { .objectid = inode, .type = BT_INODE_ITEM };
	size_t length;
	enum btrfs_result error;

	error =
	    bt_mutation_find(transaction->mutation, tree->root, key, item, sizeof(*item), &length);
	if (error != BTRFS_OK) {
		return error == BTRFS_RANGE ? BTRFS_CORRUPT : error;
	}
	if (length != sizeof(*item)) {
		return BTRFS_CORRUPT;
	}
	if ((bt_u32(item->mode) & BTRFS_MODE_TYPE) != BTRFS_MODE_REGULAR) {
		return (bt_u32(item->mode) & BTRFS_MODE_TYPE) == BTRFS_MODE_DIRECTORY
		    ? BTRFS_IS_DIRECTORY
		    : BTRFS_UNSUPPORTED;
	}
	if ((bt_u64(item->flags) & BT_INODE_IMMUTABLE) != 0) {
		return BTRFS_NOT_PERMITTED;
	}
	if (bt_u64(item->sequence) == UINT64_MAX) {
		return BTRFS_UNSUPPORTED;
	}
	/* Set-id bits and file capabilities need the caller's explicit decision. */
	return bt_tx_privileges_settled(transaction, tree, inode, item);
}

/* Decodes a file extent item of inode; *valid is 0 for another inode or type. */
static enum btrfs_result
bt_tx_decode_file_item(
    const struct bt_record *record, uint64_t inode, struct bt_file_item *item, int *valid)
{
	const struct bt_disk_extent *extent = (const void *)record->data;
	uint64_t length;

	*valid = 0;
	if (record->key.objectid != inode || record->key.type != BT_EXTENT_DATA) {
		return BTRFS_OK;
	}
	if (record->size < sizeof(extent->header) ||
	    (extent->header.type == BT_EXTENT_INLINE ? record->key.offset != 0 ||
			record->size - sizeof(extent->header) > BT_MAX_NODE_SIZE
						     : record->size != sizeof(*extent))) {
		return BTRFS_CORRUPT;
	}
	bt_zero(&item->extent, sizeof(item->extent));
	bt_copy(&item->extent, record->data,
	    record->size < sizeof(item->extent) ? record->size : sizeof(item->extent));
	item->key = record->key;
	item->size = record->size;
	length = extent->header.type == BT_EXTENT_INLINE ? bt_u64(extent->header.ram_bytes)
							 : bt_u64(extent->length);
	if (length == 0 || length > UINT64_MAX - record->key.offset ||
	    (extent->header.type != BT_EXTENT_INLINE && extent->header.type != BT_EXTENT_REGULAR &&
		extent->header.type != BT_EXTENT_PREALLOC)) {
		return BTRFS_CORRUPT;
	}
	item->end = record->key.offset + length;
	*valid = 1;
	return BTRFS_OK;
}

/* Finds the first file extent item of inode overlapping [start, end). */
static enum btrfs_result
bt_tx_file_item(struct btrfs_transaction *transaction, const struct bt_owned_root *tree,
    uint64_t inode, uint64_t start, uint64_t end, struct bt_file_item *item, int *found)
{
	const struct btrfs_fs *view = bt_mutation_view(transaction->mutation);
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = inode, .type = BT_EXTENT_DATA, .offset = start };
	enum btrfs_result error = BTRFS_OK;
	int valid;
	int pass;

	*found = 0;
	if (view == NULL) {
		return transaction->failure;
	}
	bt_cursor_init(&cursor, view, tree->root);
	for (pass = 1; pass >= 0 && !*found && error == BTRFS_OK; pass--) {
		error = bt_cursor_seek(&cursor, key, pass);
		if (error == BTRFS_NOT_FOUND) {
			error = BTRFS_OK;
			continue;
		}
		if (error != BTRFS_OK) {
			break;
		}
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != inode || record.key.type != BT_EXTENT_DATA ||
		    record.key.offset >= end) {
			continue;
		}
		error = bt_tx_decode_file_item(&record, inode, item, &valid);
		*found = error == BTRFS_OK && valid && item->end > start;
	}
	bt_cursor_fini(&cursor);
	return error;
}

/* Finds the last file extent item of inode, when it ends past start. */
static enum btrfs_result
bt_tx_last_file_item(struct btrfs_transaction *transaction, const struct bt_owned_root *tree,
    uint64_t inode, uint64_t start, struct bt_file_item *item, int *found)
{
	const struct btrfs_fs *view = bt_mutation_view(transaction->mutation);
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = inode, .type = BT_EXTENT_DATA, .offset = UINT64_MAX };
	enum btrfs_result error;
	int valid = 0;

	*found = 0;
	if (view == NULL) {
		return transaction->failure;
	}
	bt_cursor_init(&cursor, view, tree->root);
	error = bt_cursor_seek(&cursor, key, 1);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		error = bt_tx_decode_file_item(&record, inode, item, &valid);
	} else if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_OK;
	}
	*found = error == BTRFS_OK && valid && item->end > start;
	bt_cursor_fini(&cursor);
	return error;
}

static enum btrfs_result
bt_tx_reference(struct btrfs_transaction *transaction, const struct bt_owned_root *tree,
    const struct bt_file_item *item, uint64_t position, int add)
{
	struct bt_backref reference;
	struct bt_key extent = { .objectid = bt_u64(item->extent.disk_bytenr),
		.type = BT_EXTENT_ITEM,
		.offset = bt_u64(item->extent.disk_bytes) };

	if (item->extent.header.type == BT_EXTENT_INLINE || extent.objectid == 0) {
		return BTRFS_OK;
	}
	bt_zero(&reference, sizeof(reference));
	reference.data = 1;
	reference.root = tree->root.owner;
	reference.inode = item->key.objectid;
	reference.offset = position - bt_u64(item->extent.offset);
	return bt_tx_queue(transaction, extent, &reference, add);
}

/* Removes file coverage of [start, end) the way btrfs_drop_extents does:
 * covered items go, overlapping ones are trimmed, moved or split, and the
 * file references change accordingly. removed reports the dropped bytes the
 * inode counts: inline and allocated ones, not those of hole items. */
enum btrfs_result
bt_tx_drop_range(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t inode,
    uint64_t start, uint64_t end, uint64_t *removed)
{
	struct bt_file_item item;
	struct bt_disk_extent piece;
	struct bt_key key;
	uint64_t a;
	uint64_t b;
	uint64_t steps;
	int counted;
	int found = 1;
	enum btrfs_result error = BTRFS_OK;

	*removed = 0;
	for (steps = 0; error == BTRFS_OK && found; steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			return BTRFS_UNSUPPORTED;
		}
		error = bt_tx_file_item(transaction, tree, inode, start, end, &item, &found);
		if (error != BTRFS_OK || !found) {
			break;
		}
		a = item.key.offset;
		b = item.end;
		if (item.extent.header.type == BT_EXTENT_INLINE) {
			/* The caller rewrites an inline extent's whole range from offset 0. */
			if (start != 0 || end < b) {
				return BTRFS_UNSUPPORTED;
			}
			*removed += b - a;
			error = bt_tx_edit(transaction, &tree->root, item.key, NULL, 0, BT_DELETE);
			continue;
		}
		piece = item.extent;
		key = item.key;
		counted = bt_u64(item.extent.disk_bytenr) != 0;
		if (a >= start && b <= end) {
			*removed += counted ? b - a : 0;
			error = bt_tx_edit(transaction, &tree->root, item.key, NULL, 0, BT_DELETE);
			if (error == BTRFS_OK) {
				error = bt_tx_reference(transaction, tree, &item, a, 0);
			}
			continue;
		}
		if (a < start) {
			/* Keep the head [a, start); a tail past end becomes a second item. */
			bt_put64(&piece.length, start - a);
			*removed += counted ? (b < end ? b : end) - start : 0;
			error = bt_tx_edit(
			    transaction, &tree->root, item.key, &piece, sizeof(piece), BT_REPLACE);
			if (error == BTRFS_OK && b > end) {
				piece = item.extent;
				bt_put64(&piece.offset, bt_u64(item.extent.offset) + (end - a));
				bt_put64(&piece.length, b - end);
				key.offset = end;
				error = bt_tx_edit(transaction, &tree->root, key, &piece,
				    sizeof(piece), BT_INSERT);
				if (error == BTRFS_OK) {
					error = bt_tx_reference(transaction, tree, &item, a, 1);
				}
			}
			continue;
		}
		/* start <= a < end < b: the remainder moves to end; its reference key
		 * (file offset minus extent offset) is unchanged. */
		*removed += counted ? end - a : 0;
		bt_put64(&piece.offset, bt_u64(item.extent.offset) + (end - a));
		bt_put64(&piece.length, b - end);
		key.offset = end;
		error = bt_tx_edit(transaction, &tree->root, item.key, NULL, 0, BT_DELETE);
		if (error == BTRFS_OK) {
			error = bt_tx_edit(
			    transaction, &tree->root, key, &piece, sizeof(piece), BT_INSERT);
		}
	}
	return error;
}

/* Tree nodes removing a file extent item takes: its own edit and the
 * reference drop it queues, for an extent on disk. */
static size_t
bt_tx_item_nodes(const struct btrfs_transaction *transaction, const struct bt_file_item *item)
{
	struct bt_key extent = { .objectid = bt_u64(item->extent.disk_bytenr),
		.type = BT_EXTENT_ITEM,
		.offset = bt_u64(item->extent.disk_bytes) };

	if (item->extent.header.type == BT_EXTENT_INLINE || extent.objectid == 0) {
		return 1;
	}
	return 1 + bt_tx_ref_nodes(transaction, extent, 0);
}

/* Removes the file extent items of inode at and beyond start from the last
 * one down, as Linux's btrfs_truncate_inode_items does from the end of a file,
 * until the work of this call (bt_tx_work) reaches budget; at least one item
 * goes. An item across start is cut there. *reached becomes the lowest offset
 * from which every item is gone (start when *done); removed adds the bytes
 * the inode counts. */
enum btrfs_result
bt_tx_shrink(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t inode,
    uint64_t start, size_t budget, uint64_t *removed, uint64_t *reached, int *done)
{
	struct bt_file_item item;
	size_t first = bt_tx_work(transaction);
	uint64_t cut;
	uint64_t piece;
	uint64_t steps;
	int found;
	enum btrfs_result error = BTRFS_OK;

	*done = 0;
	for (steps = 0; error == BTRFS_OK; steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			return BTRFS_UNSUPPORTED;
		}
		error = bt_tx_last_file_item(transaction, tree, inode, start, &item, &found);
		if (error != BTRFS_OK) {
			break;
		}
		if (!found) {
			*reached = start;
			*done = 1;
			break;
		}
		if (steps != 0 &&
		    bt_tx_work(transaction) - first + bt_tx_item_nodes(transaction, &item) >
			budget) {
			break;
		}
		cut = item.key.offset > start ? item.key.offset : start;
		error = bt_tx_drop_range(transaction, tree, inode, cut, UINT64_MAX, &piece);
		if (error == BTRFS_OK) {
			*removed += piece;
			*reached = cut;
		}
	}
	return error;
}

/* Writes new data to its extent now: no committed root references the space,
 * and the commit's first barrier makes it durable before any metadata naming
 * it. A failed write fails the transaction. */
static enum btrfs_result
bt_tx_write_data(
    struct btrfs_transaction *transaction, uint64_t logical, const uint8_t *bytes, uint64_t length)
{
	uint64_t physical;
	uint64_t done;
	size_t chunk = 0;
	unsigned mirrors = 1;
	unsigned mirror;
	enum btrfs_result error = BTRFS_OK;

	for (mirror = 0; error == BTRFS_OK && mirror < mirrors; mirror++) {
		for (done = 0; error == BTRFS_OK && done < length; done += chunk) {
			chunk =
			    length - done < BT_DATA_WRITE ? (size_t)(length - done) : BT_DATA_WRITE;
			error = bt_map(&transaction->fs, logical + done, chunk, BT_BLOCK_DATA,
			    mirror, &physical, &mirrors);
			if (error == BTRFS_OK) {
				error = transaction->io.write(
				    transaction->io.context, physical, bytes + done, chunk);
			}
		}
	}
	if (error != BTRFS_OK) {
		transaction->failure = error;
	}
	return error;
}

/* The codec new data of a file is compressed with, as Linux's
 * inode_need_compress and compress_type choose: none for NODATACOW, NODATASUM
 * or NOCOMPRESS files; else the file's compression property, else the mount's
 * codec when the file has COMPRESS or the mount compresses everything (zlib
 * when the mount names none). LZO is read only here: such files are written
 * uncompressed. Without a compress callback nothing is compressed. */
static enum btrfs_result
bt_tx_codec(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t ino,
    const struct bt_disk_inode *inode, enum btrfs_compression *codec)
{
	const struct bt_codec *property = NULL;
	uint64_t flags = bt_u64(inode->flags);
	enum btrfs_compression mount = transaction->io.compression;
	enum btrfs_result error;

	*codec = BTRFS_COMPRESSION_NONE;
	if (transaction->io.compress == NULL ||
	    (flags & (BT_INODE_NODATACOW | BT_INODE_NODATASUM_FLAG | BT_INODE_NOCOMPRESS)) != 0) {
		return BTRFS_OK;
	}
	error = bt_ns_inherited_codec(transaction, tree, ino, flags, &property);
	if (error != BTRFS_OK) {
		return error;
	}
	if (property != NULL) {
		*codec = property->codec;
	} else if ((flags & BT_INODE_COMPRESS) != 0 || mount != BTRFS_COMPRESSION_NONE) {
		*codec = mount != BTRFS_COMPRESSION_NONE ? mount : BTRFS_COMPRESSION_ZLIB;
	}
	if (*codec != BTRFS_COMPRESSION_ZLIB && *codec != BTRFS_COMPRESSION_ZSTD) {
		*codec = BTRFS_COMPRESSION_NONE;
	}
	return BTRFS_OK;
}

/* The extent item of a new data extent at logical of size bytes, with the
 * file's one data reference, as the extent's space is accounted used. */
static enum btrfs_result
bt_tx_extent_item(struct btrfs_transaction *transaction, const struct bt_owned_root *tree,
    uint64_t ino, uint64_t position, uint64_t logical, uint64_t size)
{
	struct {
		struct bt_disk_extent_item item;
		uint8_t type;
		struct bt_disk_data_ref reference;
	} wire;
	struct bt_key key = { .objectid = logical, .type = BT_EXTENT_ITEM, .offset = size };
	enum btrfs_result error;

	_Static_assert(sizeof(wire) ==
		sizeof(struct bt_disk_extent_item) + 1 + sizeof(struct bt_disk_data_ref),
	    "inline data reference layout");
	error = bt_space_change_used(transaction->space, logical, size, 1);
	if (error == BTRFS_OK) {
		bt_zero(&wire, sizeof(wire));
		bt_put64(&wire.item.refs, 1);
		bt_put64(&wire.item.generation, transaction->base->info.generation + 1);
		bt_put64(&wire.item.flags, BT_EXTENT_FLAG_DATA);
		wire.type = BT_EXTENT_DATA_REF;
		bt_put64(&wire.reference.root, tree->root.owner);
		bt_put64(&wire.reference.objectid, ino);
		bt_put64(&wire.reference.offset, position);
		bt_put32(&wire.reference.count, 1);
		error = bt_tx_edit(
		    transaction, &transaction->extents.root, key, &wire, sizeof(wire), BT_INSERT);
	}
	if (error == BTRFS_OK) {
		error = bt_mutation_note_extent(transaction->mutation, key);
	}
	return error;
}

/* One new extent at logical: its disk bytes (compressed or not), extent item
 * with the file's data reference, checksums of the stored bytes, and the file
 * extent item covering [position, position + length). */
static enum btrfs_result
bt_tx_extent(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    const struct bt_disk_inode *inode, uint64_t ino, uint64_t position, uint64_t length,
    uint64_t logical, const uint8_t *stored, uint64_t stored_size, enum btrfs_compression codec)
{
	struct bt_disk_extent file;
	struct bt_key key;
	uint64_t generation = transaction->base->info.generation + 1;
	enum btrfs_result error;

	error = bt_tx_write_data(transaction, logical, stored, stored_size);
	if (error == BTRFS_OK) {
		error = bt_tx_extent_item(transaction, tree, ino, position, logical, stored_size);
	}
	if (error == BTRFS_OK && !(bt_u64(inode->flags) & BT_INODE_NODATASUM_FLAG)) {
		error = bt_csum_insert(transaction->mutation, &transaction->checksums.root, logical,
		    stored, stored_size);
	}
	if (error == BTRFS_OK) {
		bt_zero(&file, sizeof(file));
		bt_put64(&file.header.generation, generation);
		bt_put64(&file.header.ram_bytes, length);
		file.header.compression = (uint8_t)codec;
		file.header.type = BT_EXTENT_REGULAR;
		bt_put64(&file.disk_bytenr, logical);
		bt_put64(&file.disk_bytes, stored_size);
		bt_put64(&file.length, length);
		key =
		    (struct bt_key){ .objectid = ino, .type = BT_EXTENT_DATA, .offset = position };
		error = bt_tx_edit(transaction, &tree->root, key, &file, sizeof(file), BT_INSERT);
	}
	if (error == BTRFS_OK && codec == BTRFS_COMPRESSION_ZSTD) {
		bt_ns_require_feature(transaction, BT_FEATURE_COMPRESS_ZSTD);
	}
	return error;
}

/* Compresses length bytes into compressed (BT_COMPRESS_CHUNK bytes): the
 * stream padded to whole sectors, kept only when it saves at least one sector,
 * as compress_file_range decides; *stored is 0 otherwise. */
static enum btrfs_result
bt_tx_compress(struct btrfs_transaction *transaction, enum btrfs_compression codec,
    const uint8_t *bytes, uint64_t length, uint8_t *compressed, uint64_t *stored)
{
	uint64_t sector = transaction->base->info.sector_size;
	size_t size = 0;
	enum btrfs_result error;

	*stored = 0;
	error = transaction->io.compress(transaction->io.context, codec, bytes, (size_t)length,
	    compressed, BT_COMPRESS_CHUNK, &size);
	if (error == BTRFS_RANGE || error == BTRFS_UNSUPPORTED) {
		return BTRFS_OK;
	}
	if (error != BTRFS_OK || size > BT_COMPRESS_CHUNK) {
		return error == BTRFS_OK ? BTRFS_CORRUPT : error;
	}
	if (size + (sector - size % sector) % sector + sector <= length) {
		*stored = size + (sector - size % sector) % sector;
		bt_zero(compressed + size, (size_t)(*stored - size));
	}
	return BTRFS_OK;
}

/* Writes [start, end) of the file from buffer as new extents. The range is
 * sector aligned and holds no file extent items. A file that compresses is
 * written in BT_COMPRESS_CHUNK pieces, each compressed when that saves a
 * sector; other data goes into extents of up to BT_DATA_EXTENT bytes. */
static enum btrfs_result
bt_tx_cow(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    const struct bt_disk_inode *inode, uint64_t ino, uint64_t start, uint64_t end,
    const uint8_t *buffer)
{
	const struct btrfs_environment *env = &transaction->base->env;
	enum btrfs_compression codec = BTRFS_COMPRESSION_NONE;
	uint8_t *compressed = NULL;
	uint64_t position;
	uint64_t piece_end;
	uint64_t logical;
	uint64_t stored = 0;
	uint64_t size;
	uint64_t want;
	enum btrfs_result error;

	error = bt_tx_codec(transaction, tree, ino, inode, &codec);
	if (error == BTRFS_OK && codec != BTRFS_COMPRESSION_NONE) {
		compressed = env->allocate(env->context, BT_COMPRESS_CHUNK);
		error = compressed == NULL ? BTRFS_NO_MEMORY : BTRFS_OK;
	}
	for (position = start; error == BTRFS_OK && position < end; position = piece_end) {
		piece_end = end;
		if (compressed != NULL) {
			piece_end =
			    end - position < BT_COMPRESS_CHUNK ? end : position + BT_COMPRESS_CHUNK;
			error = bt_tx_compress(transaction, codec, buffer + (position - start),
			    piece_end - position, compressed, &stored);
			if (error == BTRFS_OK && stored != 0) {
				error =
				    bt_space_reserve_exact(transaction->space, stored, &logical);
				if (error == BTRFS_OK) {
					error = bt_tx_extent(transaction, tree, inode, ino,
					    position, piece_end - position, logical, compressed,
					    stored, codec);
				}
				continue;
			}
		}
		for (; error == BTRFS_OK && position < piece_end; position += size) {
			want = piece_end - position < BT_DATA_EXTENT ? piece_end - position
								     : BT_DATA_EXTENT;
			error = bt_space_reserve_data(transaction->space, want, &logical, &size);
			if (error == BTRFS_OK) {
				error = bt_tx_extent(transaction, tree, inode, ino, position, size,
				    logical, buffer + (position - start), size,
				    BTRFS_COMPRESSION_NONE);
			}
		}
	}
	if (compressed != NULL) {
		env->release(env->context, compressed, BT_COMPRESS_CHUNK);
	}
	return error;
}

/* Replaces [start, end) with buffer: drop old coverage, then write new extents.
 * Returns the change of the inode's allocated bytes. */
static enum btrfs_result
bt_tx_replace(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct bt_disk_inode *inode, uint64_t ino, uint64_t start, uint64_t end, const uint8_t *buffer)
{
	uint64_t removed;
	enum btrfs_result error;

	error = bt_tx_drop_range(transaction, tree, ino, start, end, &removed);
	if (error == BTRFS_OK) {
		error = bt_tx_cow(transaction, tree, inode, ino, start, end, buffer);
	}
	if (error == BTRFS_OK) {
		if (bt_u64(inode->nbytes) < removed) {
			return BTRFS_CORRUPT;
		}
		bt_put64(&inode->nbytes, bt_u64(inode->nbytes) - removed + (end - start));
	}
	return error;
}

/* Whether [position, end) of item may be written in place, as Linux's
 * run_delalloc_nocow decides: a preallocated extent, or a regular one of a
 * NODATACOW file, uncompressed, newer than the tree's last snapshot, referenced
 * only by this file's items for it, and without checksums in the range. */
static enum btrfs_result
bt_tx_nocow(struct btrfs_transaction *transaction, const struct bt_owned_root *tree,
    const struct bt_disk_inode *inode, uint64_t ino, const struct bt_file_item *item,
    uint64_t position, uint64_t end, int *in_place)
{
	const struct bt_disk_extent *extent = &item->extent;
	struct bt_backref reference;
	struct bt_key key = { bt_u64(extent->disk_bytenr), bt_u64(extent->disk_bytes),
		BT_EXTENT_ITEM };
	uint64_t refs = 0;
	uint64_t flags = 0;
	uint64_t count = 0;
	int checksummed = 0;
	enum btrfs_result error;

	*in_place = 0;
	if ((extent->header.type != BT_EXTENT_PREALLOC &&
		(extent->header.type != BT_EXTENT_REGULAR ||
		    (bt_u64(inode->flags) & BT_INODE_NODATACOW) == 0)) ||
	    key.objectid == 0 || extent->header.compression != 0 ||
	    extent->header.encryption != 0 || bt_u16(extent->header.encoding) != 0 ||
	    bt_u64(extent->header.generation) <= bt_u64(tree->item.legacy.last_snapshot)) {
		return BTRFS_OK;
	}
	bt_zero(&reference, sizeof(reference));
	reference.data = 1;
	reference.root = tree->root.owner;
	reference.inode = ino;
	reference.offset = item->key.offset - bt_u64(extent->offset);
	error =
	    bt_backref_info(transaction->mutation, transaction->extents.root, key, &refs, &flags);
	if (error == BTRFS_OK) {
		error = bt_backref_count(
		    transaction->mutation, transaction->extents.root, key, &reference, &count);
	}
	if (error != BTRFS_OK || refs != count) {
		return error;
	}
	error = bt_csum_exists(transaction->mutation, transaction->checksums.root,
	    key.objectid + bt_u64(extent->offset) + (position - item->key.offset), end - position,
	    &checksummed);
	*in_place = error == BTRFS_OK && !checksummed;
	return error;
}

/* Writes [position, end) into item's extent where it lies, as a NODATACOW
 * write does; a preallocated range becomes a regular one in place, splitting
 * the item as btrfs_mark_extent_written does, with checksums unless the file
 * is NODATASUM. */
static enum btrfs_result
bt_tx_write_in_place(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    const struct bt_disk_inode *inode, uint64_t ino, const struct bt_file_item *item,
    uint64_t position, uint64_t end, const uint8_t *bytes)
{
	struct bt_disk_extent piece;
	struct bt_key key;
	uint64_t offset = bt_u64(item->extent.offset);
	uint64_t logical =
	    bt_u64(item->extent.disk_bytenr) + offset + (position - item->key.offset);
	enum btrfs_result error;

	error = bt_tx_write_data(transaction, logical, bytes, end - position);
	if (error != BTRFS_OK || item->extent.header.type != BT_EXTENT_PREALLOC) {
		return error;
	}
	if (!(bt_u64(inode->flags) & BT_INODE_NODATASUM_FLAG)) {
		error = bt_csum_insert(transaction->mutation, &transaction->checksums.root, logical,
		    bytes, end - position);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_edit(transaction, &tree->root, item->key, NULL, 0, BT_DELETE);
	}
	/* The preallocated part before, the written part, the part after: each
	 * extra item is one more reference of the same key. */
	if (error == BTRFS_OK && position > item->key.offset) {
		piece = item->extent;
		bt_put64(&piece.length, position - item->key.offset);
		error = bt_tx_edit(
		    transaction, &tree->root, item->key, &piece, sizeof(piece), BT_INSERT);
		if (error == BTRFS_OK) {
			error = bt_tx_reference(transaction, tree, item, item->key.offset, 1);
		}
	}
	if (error == BTRFS_OK) {
		piece = item->extent;
		bt_put64(&piece.header.generation, transaction->base->info.generation + 1);
		piece.header.type = BT_EXTENT_REGULAR;
		bt_put64(&piece.offset, offset + (position - item->key.offset));
		bt_put64(&piece.length, end - position);
		key =
		    (struct bt_key){ .objectid = ino, .type = BT_EXTENT_DATA, .offset = position };
		error = bt_tx_edit(transaction, &tree->root, key, &piece, sizeof(piece), BT_INSERT);
	}
	if (error == BTRFS_OK && end < item->end) {
		piece = item->extent;
		bt_put64(&piece.offset, offset + (end - item->key.offset));
		bt_put64(&piece.length, item->end - end);
		key = (struct bt_key){ .objectid = ino, .type = BT_EXTENT_DATA, .offset = end };
		error = bt_tx_edit(transaction, &tree->root, key, &piece, sizeof(piece), BT_INSERT);
		if (error == BTRFS_OK) {
			error = bt_tx_reference(transaction, tree, item, item->key.offset, 1);
		}
	}
	return error;
}

/* Writes [start, end) from buffer: in place where bt_tx_nocow allows it,
 * otherwise as new extents replacing the old coverage. */
static enum btrfs_result
bt_tx_place(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct bt_disk_inode *inode, uint64_t ino, uint64_t start, uint64_t end, const uint8_t *buffer)
{
	struct bt_file_item item;
	uint64_t position;
	uint64_t next;
	int found = 0;
	int in_place = 0;
	enum btrfs_result error = BTRFS_OK;

	for (position = start; error == BTRFS_OK && position < end; position = next) {
		error = bt_tx_file_item(transaction, tree, ino, position, end, &item, &found);
		if (error != BTRFS_OK) {
			break;
		}
		in_place = 0;
		if (!found || item.key.offset > position) {
			next = found ? item.key.offset : end;
		} else if (item.extent.header.type == BT_EXTENT_INLINE) {
			/* An inline extent ends unaligned; its sector goes with it. */
			next = end;
		} else {
			next = item.end < end ? item.end : end;
			error = bt_tx_nocow(
			    transaction, tree, inode, ino, &item, position, next, &in_place);
		}
		if (error == BTRFS_OK) {
			error = in_place ? bt_tx_write_in_place(transaction, tree, inode, ino,
					       &item, position, next, buffer + (position - start))
					 : bt_tx_replace(transaction, tree, inode, ino, position,
					       next, buffer + (position - start));
		}
	}
	return error;
}

static enum btrfs_result
bt_tx_store_inode(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t ino,
    struct bt_disk_inode *inode, uint64_t size, struct btrfs_time modified)
{
	struct bt_key key = { .objectid = ino, .type = BT_INODE_ITEM };
	enum btrfs_result error;

	bt_put64(&inode->size, size);
	bt_put64(&inode->transid, transaction->base->info.generation + 1);
	bt_put64(&inode->sequence, bt_u64(inode->sequence) + 1);
	bt_put64(&inode->mtime.seconds, (uint64_t)modified.seconds);
	bt_put32(&inode->mtime.nanoseconds, modified.nanoseconds);
	inode->ctime = inode->mtime;
	error = bt_tx_edit(transaction, &tree->root, key, inode, sizeof(*inode), BT_REPLACE);
	if (error == BTRFS_OK) {
		transaction->changed = 1;
		bt_put64(&tree->item.ctransid, transaction->base->info.generation + 1);
		tree->item.ctime = inode->ctime;
	}
	return error;
}

/* Rewrites one or more whole sectors [start, end) holding the file's current
 * bytes, with bytes[offset, offset + size) replaced and everything at or past
 * limit zeroed. Inline extents are rewritten from offset 0. Pieces of at most
 * BT_REWRITE_CHUNK bytes keep the memory bound: each reads the bytes it keeps,
 * then replaces its range. */
static enum btrfs_result
bt_tx_rewrite(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct bt_disk_inode *inode, uint64_t ino, uint64_t start, uint64_t end, uint64_t offset,
    const void *bytes, size_t size, uint64_t limit)
{
	const struct btrfs_environment *env = &transaction->base->env;
	uint64_t piece;
	uint64_t piece_end;
	uint64_t from;
	uint64_t to;
	uint8_t *buffer;
	size_t capacity = (size_t)(end - start < BT_REWRITE_CHUNK ? end - start : BT_REWRITE_CHUNK);
	enum btrfs_result error = BTRFS_OK;

	buffer = env->allocate(env->context, capacity);
	if (buffer == NULL) {
		return BTRFS_NO_MEMORY;
	}
	for (piece = start; error == BTRFS_OK && piece < end; piece = piece_end) {
		piece_end = end - piece < BT_REWRITE_CHUNK ? end : piece + BT_REWRITE_CHUNK;
		bt_zero(buffer, (size_t)(piece_end - piece));
		/* The bytes this piece keeps: before the new bytes and after them. */
		to = offset < piece_end ? offset : piece_end;
		if (to > piece) {
			error =
			    bt_tx_read(transaction, tree, ino, piece, buffer, (size_t)(to - piece));
		}
		from = offset + size > piece ? offset + size : piece;
		if (error == BTRFS_OK && from < piece_end) {
			error = bt_tx_read(transaction, tree, ino, from, buffer + (from - piece),
			    (size_t)(piece_end - from));
		}
		from = offset > piece ? offset : piece;
		to = offset + size < piece_end ? offset + size : piece_end;
		if (error == BTRFS_OK && from < to) {
			bt_copy(buffer + (from - piece), (const uint8_t *)bytes + (from - offset),
			    (size_t)(to - from));
		}
		if (error == BTRFS_OK && limit < piece_end) {
			from = limit > piece ? limit : piece;
			bt_zero(buffer + (from - piece), (size_t)(piece_end - from));
		}
		if (error == BTRFS_OK) {
			error =
			    bt_tx_place(transaction, tree, inode, ino, piece, piece_end, buffer);
		}
	}
	env->release(env->context, buffer, capacity);
	return error;
}

/* Stores a file whose bytes fit its first sector as one inline extent, as
 * Linux's cow_file_range_inline does when a write reaches EOF: compressed when
 * the file compresses and its stream fits the inline limit, else as is when
 * the file fits the limit and does not fill the sector. *stored stays 0 when
 * neither applies or the file has data beyond its first sector. */
static enum btrfs_result
bt_tx_small(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct bt_disk_inode *inode, uint64_t ino, uint64_t offset, const void *bytes, size_t size,
    uint64_t old_size, uint64_t final, int *stored)
{
	const struct btrfs_environment *env = &transaction->base->env;
	struct bt_disk_extent_header *extent;
	struct bt_file_item item;
	struct bt_key key = { .objectid = ino, .type = BT_EXTENT_DATA, .offset = 0 };
	enum btrfs_compression codec = BTRFS_COMPRESSION_NONE;
	uint64_t sector = transaction->base->info.sector_size;
	uint64_t removed = 0;
	uint8_t *buffer;
	uint8_t *data;
	size_t data_size = 0;
	size_t length;
	int found = 0;
	enum btrfs_result error;

	*stored = 0;
	error = bt_tx_file_item(transaction, tree, ino, sector, UINT64_MAX, &item, &found);
	if (error != BTRFS_OK || found) {
		return error;
	}
	error = bt_tx_codec(transaction, tree, ino, inode, &codec);
	if (error != BTRFS_OK) {
		return error;
	}
	/* The file's bytes, then the extent item (header and data). */
	length = (size_t)sector + sizeof(*extent) + BT_INLINE_WRITE_LIMIT;
	buffer = env->allocate(env->context, length);
	if (buffer == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(buffer, length);
	extent = (void *)(buffer + sector);
	data = (uint8_t *)(extent + 1);
	if (old_size != 0) {
		error = bt_tx_read(transaction, tree, ino, 0, buffer, (size_t)old_size);
	}
	if (error == BTRFS_OK) {
		bt_copy(buffer + offset, bytes, size);
		if (codec != BTRFS_COMPRESSION_NONE) {
			error = transaction->io.compress(transaction->io.context, codec, buffer,
			    (size_t) final, data, BT_INLINE_WRITE_LIMIT, &data_size);
			if (error == BTRFS_RANGE || error == BTRFS_UNSUPPORTED ||
			    data_size >= final) {
				error = BTRFS_OK;
				codec = BTRFS_COMPRESSION_NONE;
			}
		}
	}
	if (error == BTRFS_OK && codec == BTRFS_COMPRESSION_NONE) {
		if (final > BT_INLINE_WRITE_LIMIT || final % sector == 0) {
			env->release(env->context, buffer, length);
			return BTRFS_OK;
		}
		data_size = (size_t) final;
		bt_copy(data, buffer, data_size);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_drop_range(transaction, tree, ino, 0, sector, &removed);
	}
	if (error == BTRFS_OK && bt_u64(inode->nbytes) < removed) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		bt_put64(&extent->generation, transaction->base->info.generation + 1);
		bt_put64(&extent->ram_bytes, final);
		extent->compression = (uint8_t)codec;
		extent->type = BT_EXTENT_INLINE;
		error = bt_tx_edit(
		    transaction, &tree->root, key, extent, sizeof(*extent) + data_size, BT_INSERT);
	}
	if (error == BTRFS_OK) {
		bt_put64(&inode->nbytes, bt_u64(inode->nbytes) - removed + final);
		if (codec == BTRFS_COMPRESSION_ZSTD) {
			bt_ns_require_feature(transaction, BT_FEATURE_COMPRESS_ZSTD);
		}
		*stored = 1;
	}
	env->release(env->context, buffer, length);
	return error;
}

/* Without NO_HOLES every sector below a file's size is covered by a file
 * extent item. Growing a file from old_size to size covers [old_size, size),
 * extended to sectors, as Linux's btrfs_cont_expand does: each gap and each
 * item other than a preallocated one becomes one hole item (a regular item
 * with disk_bytenr 0), which the inode's bytes do not count. */
static enum btrfs_result
bt_tx_expand(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct bt_disk_inode *inode, uint64_t ino, uint64_t old_size, uint64_t size)
{
	struct bt_file_item item;
	struct bt_disk_extent hole;
	struct bt_key key = { .objectid = ino, .type = BT_EXTENT_DATA };
	uint64_t sector = transaction->base->info.sector_size;
	uint64_t position = old_size + (sector - old_size % sector) % sector;
	uint64_t end = size + (sector - size % sector) % sector;
	uint64_t next;
	uint64_t removed;
	uint64_t steps;
	int found = 0;
	enum btrfs_result error = BTRFS_OK;

	if ((transaction->base->info.incompat_features & BT_FEATURE_NO_HOLES) != 0) {
		return BTRFS_OK;
	}
	for (steps = 0; error == BTRFS_OK && position < end; position = next, steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			return BTRFS_UNSUPPORTED;
		}
		error = bt_tx_file_item(transaction, tree, ino, position, end, &item, &found);
		if (error != BTRFS_OK) {
			break;
		}
		if (!found || item.key.offset > position) {
			next = found ? item.key.offset : end;
		} else {
			next = item.end < end ? item.end : end;
			if (item.extent.header.type == BT_EXTENT_PREALLOC) {
				continue;
			}
			error = bt_tx_drop_range(transaction, tree, ino, position, next, &removed);
			if (error == BTRFS_OK && bt_u64(inode->nbytes) < removed) {
				error = BTRFS_CORRUPT;
			}
			if (error == BTRFS_OK) {
				bt_put64(&inode->nbytes, bt_u64(inode->nbytes) - removed);
			}
		}
		if (error == BTRFS_OK) {
			bt_zero(&hole, sizeof(hole));
			bt_put64(&hole.header.generation, transaction->base->info.generation + 1);
			bt_put64(&hole.header.ram_bytes, next - position);
			hole.header.type = BT_EXTENT_REGULAR;
			bt_put64(&hole.length, next - position);
			key.offset = position;
			error = bt_tx_edit(
			    transaction, &tree->root, key, &hole, sizeof(hole), BT_INSERT);
		}
	}
	return error;
}

static enum btrfs_result
bt_tx_data_begin(struct btrfs_transaction *transaction, struct btrfs_object_id id,
    struct bt_owned_root **tree, struct bt_disk_inode *inode, uint64_t *inline_end)
{
	struct bt_file_item item;
	int found = 0;
	enum btrfs_result error;

	if (transaction->failure != BTRFS_OK || transaction->finished) {
		return transaction->failure == BTRFS_OK ? BTRFS_READ_ONLY : transaction->failure;
	}
	error = bt_tx_tree(transaction, id.tree, tree);
	if (error == BTRFS_OK) {
		error = bt_tx_inode(transaction, *tree, id.inode, inode);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_file_item(transaction, *tree, id.inode, 0, 1, &item, &found);
	}
	*inline_end = 0;
	if (error == BTRFS_OK && found && item.extent.header.type == BT_EXTENT_INLINE) {
		*inline_end = item.end;
	}
	return error;
}

enum btrfs_result
btrfs_transaction_write(struct btrfs_transaction *transaction, struct btrfs_object_id id,
    uint64_t offset, const void *bytes, size_t size, struct btrfs_time modified)
{
	struct bt_owned_root *tree;
	struct bt_disk_inode inode;
	uint64_t sector;
	uint64_t old_size;
	uint64_t final;
	uint64_t inline_end;
	uint64_t start;
	uint64_t end;
	uint64_t tail;
	int stored = 0;
	enum btrfs_result error;

	if (transaction == NULL || (bytes == NULL && size != 0) ||
	    modified.nanoseconds >= 1000000000U) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (offset > BT_FILE_SIZE_LIMIT || size > BT_FILE_SIZE_LIMIT - offset) {
		return BTRFS_RANGE;
	}
	error = bt_tx_data_begin(transaction, id, &tree, &inode, &inline_end);
	/* An append-only file accepts data only at its end. */
	if (error == BTRFS_OK && (bt_u64(inode.flags) & BT_INODE_APPEND) != 0 &&
	    offset != bt_u64(inode.size)) {
		error = BTRFS_NOT_PERMITTED;
	}
	if (error != BTRFS_OK || size == 0) {
		return error;
	}
	sector = transaction->base->info.sector_size;
	old_size = bt_u64(inode.size);
	final = offset + size > old_size ? offset + size : old_size;
	/* The whole range's data space is checked before any change, as Linux
	 * reserves it at write time, leaving what metadata may still need;
	 * compression may need less. */
	bt_tx_hold_metadata(transaction);
	if (!bt_space_data_available(
		transaction->space, size + 2 * sector - (uint64_t)size % sector)) {
		return BTRFS_NO_SPACE;
	}
	/* btrfs_qgroup_reserve_data: the written sectors count against the
	 * subvolume's qgroups and every qgroup above them. */
	error = bt_qgroup_reserve(transaction, id.tree,
	    (offset + size + sector - 1) / sector * sector - (offset - offset % sector));
	if (error != BTRFS_OK) {
		return error;
	}
	/* A file that fits its first sector may become one inline extent. */
	if (final <= sector) {
		error = bt_tx_small(transaction, tree, &inode, id.inode, offset, bytes, size,
		    old_size, final, &stored);
		if (error == BTRFS_OK && stored) {
			error =
			    bt_tx_store_inode(transaction, tree, id.inode, &inode, final, modified);
		}
		if (error != BTRFS_OK || stored) {
			if (error != BTRFS_OK) {
				transaction->failure = error;
			}
			return error;
		}
	}
	start = offset - offset % sector;
	end = offset + size + (sector - (offset + size) % sector) % sector;
	/* An inline extent becomes part of the first regular extent when the write
	 * starts in its sector or the next one, as Linux's adjacent dirty ranges
	 * do; otherwise its sector is converted on its own below. */
	tail = inline_end + (sector - inline_end % sector) % sector;
	if (inline_end != 0 && start <= tail) {
		end = end > tail ? end : tail;
		start = 0;
	}
	/* Bytes between an unaligned EOF and a write beyond it must read as zero;
	 * a separate EOF sector is rewritten with its tail cleared. */
	tail = old_size - old_size % sector;
	if (old_size % sector != 0 && offset > old_size && tail < start) {
		error = bt_tx_rewrite(transaction, tree, &inode, id.inode, tail, tail + sector,
		    tail, NULL, 0, old_size);
	}
	if (error == BTRFS_OK && offset > old_size) {
		error = bt_tx_expand(transaction, tree, &inode, id.inode, old_size, start);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_rewrite(transaction, tree, &inode, id.inode, start, end, offset,
		    bytes, size, offset + size > old_size ? offset + size : old_size);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_store_inode(transaction, tree, id.inode, &inode,
		    offset + size > old_size ? offset + size : old_size, modified);
	}
	if (error != BTRFS_OK) {
		transaction->failure = error;
	}
	return error;
}

enum btrfs_result
btrfs_transaction_truncate(struct btrfs_transaction *transaction, struct btrfs_object_id id,
    uint64_t size, struct btrfs_time modified, size_t budget, int *done)
{
	struct bt_owned_root *tree;
	struct bt_disk_inode inode;
	uint64_t sector;
	uint64_t old_size;
	uint64_t inline_end;
	uint64_t edge;
	uint64_t removed = 0;
	uint64_t reached;
	enum btrfs_result error;

	if (transaction == NULL || done == NULL || budget == 0 ||
	    modified.nanoseconds >= 1000000000U) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*done = 0;
	if (size > BT_FILE_SIZE_LIMIT) {
		return BTRFS_RANGE;
	}
	error = bt_tx_data_begin(transaction, id, &tree, &inode, &inline_end);
	if (error == BTRFS_OK && (bt_u64(inode.flags) & BT_INODE_APPEND) != 0) {
		error = BTRFS_NOT_PERMITTED;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	sector = transaction->base->info.sector_size;
	old_size = bt_u64(inode.size);
	edge = size + (sector - size % sector) % sector;
	if (size < old_size && inline_end == 0) {
		/* Items past the new EOF's sector go from the end in bounded steps;
		 * each step stores the size reached, a valid shorter file. */
		reached = old_size;
		error = bt_tx_shrink(
		    transaction, tree, id.inode, edge, budget, &removed, &reached, done);
		if (error == BTRFS_OK) {
			error = bt_u64(inode.nbytes) < removed ? BTRFS_CORRUPT : BTRFS_OK;
			bt_put64(&inode.nbytes, bt_u64(inode.nbytes) - removed);
		}
		if (error == BTRFS_OK && !*done) {
			error = bt_tx_store_inode(transaction, tree, id.inode, &inode,
			    reached < old_size ? reached : old_size, modified);
		}
		if (error != BTRFS_OK) {
			transaction->failure = error;
		}
		if (error != BTRFS_OK || !*done) {
			return error;
		}
	}
	*done = 1;
	if (inline_end != 0) {
		/* Convert, then truncate the regular extent like any other. */
		edge = inline_end + (sector - inline_end % sector) % sector;
		error = bt_tx_rewrite(transaction, tree, &inode, id.inode, 0, edge, 0, NULL, 0,
		    old_size < size ? old_size : size);
	} else if (size % sector != 0 && size != old_size) {
		/* The sector holding the new or old EOF keeps only bytes below both. */
		edge = (size < old_size ? size : old_size);
		edge -= edge % sector;
		if (edge < old_size) {
			error = bt_tx_rewrite(transaction, tree, &inode, id.inode, edge,
			    edge + sector, edge, NULL, 0, size < old_size ? size : old_size);
		}
	} else if (size > old_size && old_size % sector != 0) {
		edge = old_size - old_size % sector;
		error = bt_tx_rewrite(transaction, tree, &inode, id.inode, edge, edge + sector,
		    edge, NULL, 0, old_size);
	}
	if (error == BTRFS_OK && size > old_size) {
		error = bt_tx_expand(transaction, tree, &inode, id.inode, old_size, size);
	}
	if (error == BTRFS_OK && size < old_size && inline_end != 0) {
		edge = size + (sector - size % sector) % sector;
		error = bt_tx_drop_range(transaction, tree, id.inode, edge, UINT64_MAX, &removed);
		if (error == BTRFS_OK) {
			error = bt_u64(inode.nbytes) < removed ? BTRFS_CORRUPT : BTRFS_OK;
			bt_put64(&inode.nbytes, bt_u64(inode.nbytes) - removed);
		}
	}
	if (error == BTRFS_OK) {
		error = bt_tx_store_inode(transaction, tree, id.inode, &inode, size, modified);
	}
	if (error != BTRFS_OK) {
		transaction->failure = error;
	}
	return error;
}

/* What btrfs_get_extent maps at a sector: nothing written (no item, or a hole
 * item), unwritten (preallocated) or written data. */
enum bt_mapping { BT_MAP_HOLE, BT_MAP_PREALLOC, BT_MAP_WRITTEN };

/* The mapping at the sector position and where it ends, as btrfs_get_extent
 * gives it with its extent maps cached: an item with data covering position
 * ends where it does (an inline extent at the end of its sector); a hole,
 * made of gaps and hole items, runs to the next item with data, or without
 * one to UINT64_MAX. */
static enum btrfs_result
bt_tx_mapping(struct btrfs_transaction *transaction, const struct bt_owned_root *tree, uint64_t ino,
    uint64_t position, enum bt_mapping *kind, uint64_t *end)
{
	struct bt_file_item item;
	uint64_t sector = transaction->base->info.sector_size;
	uint64_t cursor = position;
	uint64_t steps;
	int found = 0;
	int inline_item;
	enum btrfs_result error;

	*kind = BT_MAP_HOLE;
	*end = UINT64_MAX;
	for (steps = 0; steps < BT_MAX_TREE_ITEMS; steps++) {
		error = bt_tx_file_item(transaction, tree, ino, cursor, UINT64_MAX, &item, &found);
		if (error != BTRFS_OK || !found) {
			return error;
		}
		/* A gap runs to the item. */
		cursor = item.key.offset > cursor ? item.key.offset : cursor;
		inline_item = item.extent.header.type == BT_EXTENT_INLINE;
		if (!inline_item && bt_u64(item.extent.disk_bytenr) == 0) {
			cursor = item.end;
			continue;
		}
		if (cursor != position) {
			*end = cursor;
		} else if (inline_item) {
			*kind = BT_MAP_WRITTEN;
			*end = item.end + (sector - item.end % sector) % sector;
		} else {
			*kind = item.extent.header.type == BT_EXTENT_PREALLOC ? BT_MAP_PREALLOC
									      : BT_MAP_WRITTEN;
			*end = item.end;
		}
		return BTRFS_OK;
	}
	return BTRFS_UNSUPPORTED;
}

/* find_first_non_hole: when [*start, *start + *length) begins in a hole, the
 * range moves past it (*hole); *length 0 then means it was all hole. */
static enum btrfs_result
bt_tx_skip_hole(struct btrfs_transaction *transaction, const struct bt_owned_root *tree,
    uint64_t ino, uint64_t *start, uint64_t *length, int *hole)
{
	enum bt_mapping kind;
	uint64_t sector = transaction->base->info.sector_size;
	uint64_t end;
	enum btrfs_result error;

	*hole = 0;
	error = bt_tx_mapping(transaction, tree, ino, *start - *start % sector, &kind, &end);
	if (error == BTRFS_OK && kind == BT_MAP_HOLE) {
		*hole = 1;
		*length = end > *start + *length ? 0 : *start + *length - end;
		*start = end;
	}
	return error;
}

/* btrfs_truncate_block: an unaligned edge rewrites its sector with part of it
 * zeroed: [from, from + length), or with length 0 the rest of the sector
 * (front: the sector's start up to from). zeros holds a sector of zeros. */
static enum btrfs_result
bt_tx_zero_block(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct bt_disk_inode *inode, uint64_t ino, uint64_t from, uint64_t length, int front,
    const uint8_t *zeros)
{
	uint64_t sector = transaction->base->info.sector_size;
	uint64_t block = from - from % sector;
	uint64_t offset = from % sector;

	if (offset == 0 && length % sector == 0) {
		return BTRFS_OK;
	}
	if (front) {
		return bt_tx_rewrite(transaction, tree, inode, ino, block, block + sector, block,
		    zeros, (size_t)offset, bt_u64(inode->size));
	}
	if (length == 0 || length > sector - offset) {
		length = sector - offset;
	}
	return bt_tx_rewrite(transaction, tree, inode, ino, block, block + sector, from, zeros,
	    (size_t)length, bt_u64(inode->size));
}

/* An unwritten extent at logical for [position, position + size), as
 * insert_prealloc_file_extent stores it: the extent item with the file's data
 * reference and a PREALLOC file extent item; no data and no checksums. */
static enum btrfs_result
bt_tx_prealloc_extent(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    uint64_t ino, uint64_t position, uint64_t logical, uint64_t size)
{
	struct bt_disk_extent file;
	struct bt_key key = { .objectid = ino, .type = BT_EXTENT_DATA, .offset = position };
	enum btrfs_result error;

	error = bt_tx_extent_item(transaction, tree, ino, position, logical, size);
	if (error == BTRFS_OK) {
		bt_zero(&file, sizeof(file));
		bt_put64(&file.header.generation, transaction->base->info.generation + 1);
		bt_put64(&file.header.ram_bytes, size);
		file.header.type = BT_EXTENT_PREALLOC;
		bt_put64(&file.disk_bytenr, logical);
		bt_put64(&file.disk_bytes, size);
		bt_put64(&file.length, size);
		error = bt_tx_edit(transaction, &tree->root, key, &file, sizeof(file), BT_INSERT);
	}
	return error;
}

/* __btrfs_prealloc_file_range over the sectors [start, end): unwritten
 * extents of up to BT_PREALLOC_EXTENT bytes replace the coverage they take
 * and count in the inode's bytes; without KEEP_SIZE the size follows them,
 * up to limit. */
static enum btrfs_result
bt_tx_prealloc(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct bt_disk_inode *inode, uint64_t ino, uint64_t start, uint64_t end, unsigned mode,
    uint64_t limit)
{
	uint64_t position;
	uint64_t want;
	uint64_t logical;
	uint64_t size = 0;
	uint64_t removed = 0;
	enum btrfs_result error = BTRFS_OK;

	for (position = start; error == BTRFS_OK && position < end; position += size) {
		want = end - position < BT_PREALLOC_EXTENT ? end - position : BT_PREALLOC_EXTENT;
		error = bt_space_reserve_data(transaction->space, want, &logical, &size);
		if (error == BTRFS_OK) {
			error = bt_tx_drop_range(
			    transaction, tree, ino, position, position + size, &removed);
		}
		if (error == BTRFS_OK) {
			error =
			    bt_tx_prealloc_extent(transaction, tree, ino, position, logical, size);
		}
		if (error == BTRFS_OK && bt_u64(inode->nbytes) < removed) {
			error = BTRFS_CORRUPT;
		}
		if (error != BTRFS_OK) {
			break;
		}
		bt_put64(&inode->nbytes, bt_u64(inode->nbytes) - removed + size);
		bt_put64(&inode->flags, bt_u64(inode->flags) | BT_INODE_PREALLOC);
		if (!(mode & BTRFS_FALLOCATE_KEEP_SIZE) && limit > bt_u64(inode->size) &&
		    position + size > bt_u64(inode->size)) {
			bt_put64(&inode->size, position + size < limit ? position + size : limit);
		}
	}
	return error;
}

/* btrfs_fallocate_update_isize. */
static void
bt_tx_fallocate_size(struct bt_disk_inode *inode, unsigned mode, uint64_t end)
{
	if (!(mode & BTRFS_FALLOCATE_KEEP_SIZE) && end > bt_u64(inode->size)) {
		bt_put64(&inode->size, end);
	}
}

/* btrfs_zero_range, after btrfs_fallocate's own steps. */
static enum btrfs_result
bt_tx_zero_range(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct bt_disk_inode *inode, uint64_t ino, unsigned mode, uint64_t offset, uint64_t length,
    const uint8_t *zeros)
{
	enum bt_mapping kind;
	uint64_t sector = transaction->base->info.sector_size;
	uint64_t limit = offset + length;
	uint64_t alloc_start = offset - offset % sector;
	uint64_t alloc_end = limit + (sector - limit % sector) % sector;
	uint64_t end;
	enum btrfs_result error;

	error = bt_tx_mapping(transaction, tree, ino, alloc_start, &kind, &end);
	if (error == BTRFS_OK && kind == BT_MAP_PREALLOC) {
		/* What is already unwritten stays; only the rest is zeroed. */
		if (end >= limit) {
			bt_tx_fallocate_size(inode, mode, limit);
			return BTRFS_OK;
		}
		alloc_start = end;
		offset = alloc_start;
		length = limit - offset;
	}
	if (error == BTRFS_OK && offset / sector == (limit - 1) / sector) {
		error = bt_tx_mapping(transaction, tree, ino, alloc_start, &kind, &end);
		if (error != BTRFS_OK || kind == BT_MAP_PREALLOC) {
			bt_tx_fallocate_size(inode, mode, limit);
			return error;
		}
		if (length < sector && kind != BT_MAP_HOLE) {
			error = bt_tx_zero_block(
			    transaction, tree, inode, ino, offset, length, 0, zeros);
			bt_tx_fallocate_size(inode, mode, limit);
			return error;
		}
		alloc_start = offset - offset % sector;
		alloc_end = alloc_start + sector;
	} else if (error == BTRFS_OK) {
		alloc_start = offset + (sector - offset % sector) % sector;
		alloc_end = limit - limit % sector;
		/* An unaligned edge in a hole joins the allocation; written data
		 * there is zeroed in place of it; unwritten data reads zeros. */
		if (offset % sector != 0) {
			error = bt_tx_mapping(
			    transaction, tree, ino, offset - offset % sector, &kind, &end);
			if (error == BTRFS_OK && kind == BT_MAP_HOLE) {
				alloc_start = offset - offset % sector;
			} else if (error == BTRFS_OK && kind == BT_MAP_WRITTEN) {
				error = bt_tx_zero_block(
				    transaction, tree, inode, ino, offset, 0, 0, zeros);
			}
		}
		if (error == BTRFS_OK && limit % sector != 0) {
			error = bt_tx_mapping(
			    transaction, tree, ino, limit - limit % sector, &kind, &end);
			if (error == BTRFS_OK && kind == BT_MAP_HOLE) {
				alloc_end = limit + (sector - limit % sector);
			} else if (error == BTRFS_OK && kind == BT_MAP_WRITTEN) {
				error = bt_tx_zero_block(
				    transaction, tree, inode, ino, limit, 0, 1, zeros);
			}
		}
	}
	if (error == BTRFS_OK && alloc_start < alloc_end) {
		error = bt_tx_prealloc(
		    transaction, tree, inode, ino, alloc_start, alloc_end, mode, limit);
	}
	if (error == BTRFS_OK) {
		bt_tx_fallocate_size(inode, mode, limit);
	}
	return error;
}

/* btrfs_fallocate for allocation and zeroing: a range starting past EOF first
 * extends the file with holes (btrfs_cont_expand), one ending past it clears
 * the EOF sector's tail; then the holes of the range, and data beyond EOF,
 * become unwritten extents. */
static enum btrfs_result
bt_tx_allocate(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct bt_disk_inode *inode, uint64_t ino, unsigned mode, uint64_t offset, uint64_t length,
    const uint8_t *zeros)
{
	enum bt_mapping kind;
	uint64_t sector = transaction->base->info.sector_size;
	uint64_t size = bt_u64(inode->size);
	uint64_t limit = offset + length;
	uint64_t alloc_start = offset - offset % sector;
	uint64_t alloc_end = limit + (sector - limit % sector) % sector;
	uint64_t position;
	uint64_t next;
	uint64_t end;
	uint64_t steps;
	enum btrfs_result error = BTRFS_OK;

	if (alloc_start > size || limit > size) {
		error = bt_tx_zero_block(transaction, tree, inode, ino, size, 0, 0, zeros);
	}
	if (error == BTRFS_OK && alloc_start > size) {
		error = bt_tx_expand(transaction, tree, inode, ino, size, alloc_start);
	}
	if (error != BTRFS_OK || (mode & BTRFS_FALLOCATE_ZERO_RANGE) != 0) {
		return error == BTRFS_OK
		    ? bt_tx_zero_range(transaction, tree, inode, ino, mode, offset, length, zeros)
		    : error;
	}
	for (position = alloc_start, steps = 0; error == BTRFS_OK && position < alloc_end;
	    position = next, steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			return BTRFS_UNSUPPORTED;
		}
		error = bt_tx_mapping(transaction, tree, ino, position, &kind, &end);
		next = end < alloc_end ? end : alloc_end;
		next += (sector - next % sector) % sector;
		if (error == BTRFS_OK &&
		    (kind == BT_MAP_HOLE || (position >= size && kind != BT_MAP_PREALLOC))) {
			error = bt_tx_prealloc(
			    transaction, tree, inode, ino, position, next, mode, limit);
		}
	}
	if (error == BTRFS_OK) {
		bt_tx_fallocate_size(inode, mode, limit);
	}
	return error;
}

/* Data bytes an allocation may take: the holes it fills and data beyond EOF
 * it replaces, and the EOF sector it may rewrite. */
static enum btrfs_result
bt_tx_allocate_need(struct btrfs_transaction *transaction, const struct bt_owned_root *tree,
    uint64_t ino, uint64_t size, uint64_t offset, uint64_t length, uint64_t *need)
{
	enum bt_mapping kind;
	uint64_t sector = transaction->base->info.sector_size;
	uint64_t limit = offset + length;
	uint64_t alloc_end = limit + (sector - limit % sector) % sector;
	uint64_t position;
	uint64_t next;
	uint64_t end;
	uint64_t steps;
	enum btrfs_result error = BTRFS_OK;

	*need = sector;
	for (position = offset - offset % sector, steps = 0;
	    error == BTRFS_OK && position < alloc_end; position = next, steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			return BTRFS_UNSUPPORTED;
		}
		error = bt_tx_mapping(transaction, tree, ino, position, &kind, &end);
		next = end < alloc_end ? end : alloc_end;
		next += (sector - next % sector) % sector;
		if (kind == BT_MAP_HOLE || (position >= size && kind != BT_MAP_PREALLOC)) {
			*need += next - position;
		}
	}
	return error;
}

/* btrfs_punch_hole, once [offset, offset + length) was found to start past
 * any hole at original. */
static enum btrfs_result
bt_tx_punch(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct bt_disk_inode *inode, uint64_t ino, uint64_t original, uint64_t offset, uint64_t length,
    const uint8_t *zeros)
{
	struct bt_disk_extent hole;
	struct bt_key key = { .objectid = ino, .type = BT_EXTENT_DATA };
	uint64_t sector = transaction->base->info.sector_size;
	uint64_t size = bt_u64(inode->size);
	uint64_t eof = size + (sector - size % sector) % sector;
	uint64_t lock_start = offset + (sector - offset % sector) % sector;
	uint64_t lock_end = (offset + length) - (offset + length) % sector;
	uint64_t tail_start;
	uint64_t tail_length;
	uint64_t removed = 0;
	int hole_found = 0;
	enum btrfs_result error = BTRFS_OK;

	/* Within one sector only that sector's part is zeroed. */
	if (offset / sector == (offset + length - 1) / sector && length < sector) {
		return offset < eof
		    ? bt_tx_zero_block(transaction, tree, inode, ino, offset, length, 0, zeros)
		    : BTRFS_OK;
	}
	if (offset < eof) {
		error = bt_tx_zero_block(transaction, tree, inode, ino, offset, 0, 0, zeros);
	}
	/* Unless the start already moved past a hole, look again after the
	 * zeroed sector. */
	if (error == BTRFS_OK && offset == original) {
		length = offset + length - lock_start;
		offset = lock_start;
		error = bt_tx_skip_hole(transaction, tree, ino, &offset, &length, &hole_found);
		if (error != BTRFS_OK || (hole_found && length == 0)) {
			return error;
		}
		lock_start = offset;
	}
	tail_start = lock_end;
	tail_length = error == BTRFS_OK ? offset + length - tail_start : 0;
	if (tail_length != 0) {
		error =
		    bt_tx_skip_hole(transaction, tree, ino, &tail_start, &tail_length, &hole_found);
		if (error == BTRFS_OK && !hole_found && tail_start + tail_length < eof) {
			error = bt_tx_zero_block(
			    transaction, tree, inode, ino, tail_start + tail_length, 0, 1, zeros);
		}
	}
	if (error != BTRFS_OK || lock_end <= lock_start) {
		return error;
	}
	error = bt_tx_drop_range(transaction, tree, ino, lock_start, lock_end, &removed);
	if (error == BTRFS_OK && bt_u64(inode->nbytes) < removed) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		bt_put64(&inode->nbytes, bt_u64(inode->nbytes) - removed);
	}
	/* fill_holes: without NO_HOLES, a punch below EOF leaves a hole item. */
	if (error == BTRFS_OK && lock_start < eof &&
	    (transaction->base->info.incompat_features & BT_FEATURE_NO_HOLES) == 0) {
		bt_zero(&hole, sizeof(hole));
		bt_put64(&hole.header.generation, transaction->base->info.generation + 1);
		bt_put64(&hole.header.ram_bytes, lock_end - lock_start);
		hole.header.type = BT_EXTENT_REGULAR;
		bt_put64(&hole.length, lock_end - lock_start);
		key.offset = lock_start;
		error = bt_tx_edit(transaction, &tree->root, key, &hole, sizeof(hole), BT_INSERT);
	}
	return error;
}

enum btrfs_result
btrfs_transaction_fallocate(struct btrfs_transaction *transaction, struct btrfs_object_id id,
    unsigned mode, uint64_t offset, uint64_t length, struct btrfs_time now)
{
	const struct btrfs_environment *env;
	struct bt_owned_root *tree;
	struct bt_disk_inode inode;
	uint8_t *zeros = NULL;
	uint64_t sector;
	uint64_t limit;
	uint64_t inline_end;
	uint64_t original = offset;
	uint64_t need = 0;
	int hole = 0;
	enum btrfs_result error;

	if (transaction == NULL || length == 0 || now.nanoseconds >= 1000000000U) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if ((mode &
		~(BTRFS_FALLOCATE_KEEP_SIZE | BTRFS_FALLOCATE_PUNCH_HOLE |
		    BTRFS_FALLOCATE_ZERO_RANGE)) != 0 ||
	    ((mode & BTRFS_FALLOCATE_PUNCH_HOLE) != 0 &&
		(mode & (BTRFS_FALLOCATE_KEEP_SIZE | BTRFS_FALLOCATE_ZERO_RANGE)) !=
		    BTRFS_FALLOCATE_KEEP_SIZE)) {
		return BTRFS_UNSUPPORTED;
	}
	if (offset > BT_FILE_SIZE_LIMIT || length > BT_FILE_SIZE_LIMIT - offset) {
		return BTRFS_RANGE;
	}
	error = bt_tx_data_begin(transaction, id, &tree, &inode, &inline_end);
	/* An append-only file may only gain space. */
	if (error == BTRFS_OK && (bt_u64(inode.flags) & BT_INODE_APPEND) != 0 &&
	    (mode & ~BTRFS_FALLOCATE_KEEP_SIZE) != 0) {
		error = BTRFS_NOT_PERMITTED;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	sector = transaction->base->info.sector_size;
	limit = offset + length;
	if ((mode & BTRFS_FALLOCATE_PUNCH_HOLE) != 0) {
		/* A punch entirely within a hole changes nothing, not even times. */
		error = bt_tx_skip_hole(transaction, tree, id.inode, &offset, &length, &hole);
		if (error != BTRFS_OK || (hole && length == 0)) {
			return error;
		}
		need = 2 * sector;
	} else if ((mode & BTRFS_FALLOCATE_ZERO_RANGE) != 0) {
		need = limit + (sector - limit % sector) % sector - (offset - offset % sector) +
		    3 * sector;
	} else {
		error = bt_tx_allocate_need(
		    transaction, tree, id.inode, bt_u64(inode.size), offset, length, &need);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	bt_tx_hold_metadata(transaction);
	if (!bt_space_data_available(transaction->space, need)) {
		return BTRFS_NO_SPACE;
	}
	/* Allocation and zeroing reserve their sectors as btrfs_fallocate does;
	 * a punch adds nothing. */
	if ((mode & BTRFS_FALLOCATE_PUNCH_HOLE) == 0) {
		error = bt_qgroup_reserve(transaction, id.tree, need);
		if (error != BTRFS_OK) {
			return error;
		}
	}
	env = &transaction->base->env;
	zeros = env->allocate(env->context, (size_t)sector);
	if (zeros == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(zeros, (size_t)sector);
	error = (mode & BTRFS_FALLOCATE_PUNCH_HOLE) != 0
	    ? bt_tx_punch(transaction, tree, &inode, id.inode, original, offset, length, zeros)
	    : bt_tx_allocate(transaction, tree, &inode, id.inode, mode, offset, length, zeros);
	env->release(env->context, zeros, (size_t)sector);
	/* file_modified, and each step after it: change and modification times. */
	if (error == BTRFS_OK) {
		error =
		    bt_tx_store_inode(transaction, tree, id.inode, &inode, bt_u64(inode.size), now);
	}
	if (error != BTRFS_OK) {
		transaction->failure = error;
	}
	return error;
}
