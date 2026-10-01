/* SPDX-License-Identifier: BSD-3-Clause */
#include "transaction.h"

/* Largest single data write issued to the device during publication. */
#define BT_DATA_WRITE (1024U * 1024U)
/* Linux's limit on one uncompressed data extent. */
#define BT_DATA_EXTENT (UINT64_C(128) * 1024 * 1024)
#define BT_FILE_SIZE_LIMIT ((UINT64_C(1) << 63) - 1)

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
	size_t i;

	for (i = 0; i < transaction->staged_count; i++) {
		env->release(env->context, transaction->staged[i].bytes,
		    (size_t)transaction->staged[i].length);
	}
	if (transaction->staged != NULL) {
		env->release(env->context, transaction->staged,
		    BT_TRANSACTION_EXTENTS * sizeof(*transaction->staged));
	}
	if (transaction->refs != NULL) {
		env->release(env->context, transaction->refs,
		    BT_TRANSACTION_REFERENCES * sizeof(*transaction->refs));
	}
	transaction->staged = NULL;
	transaction->refs = NULL;
	transaction->staged_count = 0;
	transaction->ref_count = 0;
}

static enum btrfs_result
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
	return BTRFS_OK;
}

static enum btrfs_result
bt_tx_free_data(struct btrfs_transaction *transaction, struct bt_key extent)
{
	size_t i;
	enum btrfs_result error;

	error = bt_csum_delete(
	    transaction->mutation, &transaction->checksums.root, extent.objectid, extent.offset);
	if (error == BTRFS_OK) {
		error = bt_space_change_used(transaction->space, extent.objectid, extent.offset, 0);
	}
	for (i = 0; error == BTRFS_OK && i < transaction->staged_count; i++) {
		if (transaction->staged[i].logical == extent.objectid) {
			transaction->staged[i].freed = 1;
		}
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
	return error;
}

enum btrfs_result
bt_tx_write_staged(struct btrfs_transaction *transaction)
{
	const struct bt_staged *staged;
	uint64_t done;
	size_t length;
	size_t i;
	unsigned mirror;
	enum btrfs_result error = BTRFS_OK;

	for (i = 0; error == BTRFS_OK && i < transaction->staged_count; i++) {
		staged = &transaction->staged[i];
		for (mirror = 0; !staged->freed && error == BTRFS_OK && mirror < staged->mirrors;
		    mirror++) {
			for (done = 0; error == BTRFS_OK && done < staged->length; done += length) {
				length = staged->length - done < BT_DATA_WRITE
				    ? (size_t)(staged->length - done)
				    : BT_DATA_WRITE;
				error = transaction->io.write(transaction->io.context,
				    staged->physical[mirror] + done, staged->bytes + done, length);
			}
		}
	}
	return error;
}

/* Private read view: metadata through the mutation overlay, staged data by
 * physical address, and the transaction's own checksum and file tree roots. */
static enum btrfs_result
bt_view_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct btrfs_transaction *transaction = context;
	const struct btrfs_fs *mutation = bt_mutation_view(transaction->mutation);
	const struct bt_staged *staged;
	uint64_t start;
	uint64_t end;
	size_t i;
	unsigned mirror;
	enum btrfs_result error;

	if (mutation == NULL) {
		return transaction->failure != BTRFS_OK ? transaction->failure : BTRFS_IO;
	}
	error = mutation->env.read(mutation->env.context, offset, buffer, length);
	for (i = 0; error == BTRFS_OK && i < transaction->staged_count; i++) {
		staged = &transaction->staged[i];
		for (mirror = 0; mirror < staged->mirrors; mirror++) {
			start =
			    offset > staged->physical[mirror] ? offset : staged->physical[mirror];
			end = offset + length < staged->physical[mirror] + staged->length
			    ? offset + length
			    : staged->physical[mirror] + staged->length;
			if (start < end) {
				bt_copy((uint8_t *)buffer + (start - offset),
				    staged->bytes + (start - staged->physical[mirror]),
				    (size_t)(end - start));
			}
		}
	}
	return error;
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
    size_t input_size, void *output, size_t output_size)
{
	struct btrfs_transaction *transaction = context;
	const struct btrfs_environment *env = &transaction->base->env;

	return env->decompress == NULL
	    ? BTRFS_UNSUPPORTED
	    : env->decompress(env->context, codec, input, input_size, output, output_size);
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
	/* Set-id clearing needs the owner's credential policy; immutable and
	 * append-only inodes need their own authorization contracts. */
	if ((bt_u32(item->mode) & 06000U) != 0 || bt_u64(item->sequence) == UINT64_MAX ||
	    (bt_u64(item->flags) & (BT_INODE_IMMUTABLE | BT_INODE_APPEND)) != 0) {
		return BTRFS_UNSUPPORTED;
	}
	return BTRFS_OK;
}

/* Finds the first file extent item of inode overlapping [start, end). */
static enum btrfs_result
bt_tx_file_item(struct btrfs_transaction *transaction, const struct bt_owned_root *tree,
    uint64_t inode, uint64_t start, uint64_t end, struct bt_file_item *item, int *found)
{
	const struct btrfs_fs *view = bt_mutation_view(transaction->mutation);
	const struct bt_disk_extent *extent;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = inode, .type = BT_EXTENT_DATA, .offset = start };
	uint64_t length;
	enum btrfs_result error = BTRFS_OK;
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
		extent = (const void *)record.data;
		if (record.size < sizeof(extent->header) ||
		    (extent->header.type == BT_EXTENT_INLINE ? record.key.offset != 0 ||
				record.size - sizeof(extent->header) > BT_MAX_NODE_SIZE
							     : record.size != sizeof(*extent))) {
			error = BTRFS_CORRUPT;
			break;
		}
		bt_zero(&item->extent, sizeof(item->extent));
		bt_copy(&item->extent, record.data,
		    record.size < sizeof(item->extent) ? record.size : sizeof(item->extent));
		item->key = record.key;
		item->size = record.size;
		length = extent->header.type == BT_EXTENT_INLINE ? bt_u64(extent->header.ram_bytes)
								 : bt_u64(extent->length);
		if (length == 0 || length > UINT64_MAX - record.key.offset ||
		    (extent->header.type != BT_EXTENT_INLINE &&
			extent->header.type != BT_EXTENT_REGULAR &&
			extent->header.type != BT_EXTENT_PREALLOC)) {
			error = BTRFS_CORRUPT;
			break;
		}
		item->end = record.key.offset + length;
		*found = item->end > start;
	}
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
 * file references change accordingly. removed reports the dropped bytes. */
static enum btrfs_result
bt_tx_drop_range(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t inode,
    uint64_t start, uint64_t end, uint64_t *removed)
{
	struct bt_file_item item;
	struct bt_disk_extent piece;
	struct bt_key key;
	uint64_t a;
	uint64_t b;
	uint64_t steps;
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
		if (a >= start && b <= end) {
			*removed += b - a;
			error = bt_tx_edit(transaction, &tree->root, item.key, NULL, 0, BT_DELETE);
			if (error == BTRFS_OK) {
				error = bt_tx_reference(transaction, tree, &item, a, 0);
			}
			continue;
		}
		if (a < start) {
			/* Keep the head [a, start); a tail past end becomes a second item. */
			bt_put64(&piece.length, start - a);
			*removed += (b < end ? b : end) - start;
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
		*removed += end - a;
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

static enum btrfs_result
bt_tx_stage(
    struct btrfs_transaction *transaction, uint64_t logical, const uint8_t *bytes, uint64_t length)
{
	const struct btrfs_environment *env = &transaction->base->env;
	struct bt_staged *staged;
	unsigned mirror;
	enum btrfs_result error = BTRFS_OK;

	if (transaction->staged == NULL) {
		transaction->staged = env->allocate(
		    env->context, BT_TRANSACTION_EXTENTS * sizeof(*transaction->staged));
		if (transaction->staged == NULL) {
			return BTRFS_NO_MEMORY;
		}
	}
	if (transaction->staged_count == BT_TRANSACTION_EXTENTS ||
	    length > BT_TRANSACTION_DATA - transaction->staged_bytes) {
		return BTRFS_UNSUPPORTED;
	}
	staged = &transaction->staged[transaction->staged_count];
	bt_zero(staged, sizeof(*staged));
	staged->logical = logical;
	staged->length = length;
	staged->mirrors = 1;
	for (mirror = 0; error == BTRFS_OK && mirror < staged->mirrors; mirror++) {
		error = bt_map(transaction->base, logical, (size_t)length, BT_BLOCK_DATA, mirror,
		    &staged->physical[mirror], &staged->mirrors);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	staged->bytes = env->allocate(env->context, (size_t)length);
	if (staged->bytes == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_copy(staged->bytes, bytes, (size_t)length);
	transaction->staged_count++;
	transaction->staged_bytes += length;
	return BTRFS_OK;
}

/* Writes [start, end) of the file from buffer as new extents. The range is
 * sector aligned and holds no file extent items. */
static enum btrfs_result
bt_tx_cow(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    const struct bt_disk_inode *inode, uint64_t ino, uint64_t start, uint64_t end,
    const uint8_t *buffer)
{
	struct {
		struct bt_disk_extent_item item;
		uint8_t type;
		struct bt_disk_data_ref reference;
	} wire;
	struct bt_disk_extent file;
	struct bt_key key;
	uint64_t generation = transaction->base->info.generation + 1;
	uint64_t position;
	uint64_t logical;
	uint64_t size;
	uint64_t want;
	enum btrfs_result error = BTRFS_OK;

	_Static_assert(sizeof(wire) ==
		sizeof(struct bt_disk_extent_item) + 1 + sizeof(struct bt_disk_data_ref),
	    "inline data reference layout");
	for (position = start; error == BTRFS_OK && position < end; position += size) {
		want = end - position < BT_DATA_EXTENT ? end - position : BT_DATA_EXTENT;
		error = bt_space_reserve_data(transaction->space, want, &logical, &size);
		if (error == BTRFS_OK) {
			error =
			    bt_tx_stage(transaction, logical, buffer + (position - start), size);
		}
		if (error == BTRFS_OK) {
			error = bt_space_change_used(transaction->space, logical, size, 1);
		}
		if (error == BTRFS_OK) {
			bt_zero(&wire, sizeof(wire));
			bt_put64(&wire.item.refs, 1);
			bt_put64(&wire.item.generation, generation);
			bt_put64(&wire.item.flags, BT_EXTENT_FLAG_DATA);
			wire.type = BT_EXTENT_DATA_REF;
			bt_put64(&wire.reference.root, tree->root.owner);
			bt_put64(&wire.reference.objectid, ino);
			bt_put64(&wire.reference.offset, position);
			bt_put32(&wire.reference.count, 1);
			key = (struct bt_key){
				.objectid = logical, .type = BT_EXTENT_ITEM, .offset = size
			};
			error = bt_tx_edit(transaction, &transaction->extents.root, key, &wire,
			    sizeof(wire), BT_INSERT);
		}
		if (error == BTRFS_OK && !(bt_u64(inode->flags) & BT_INODE_NODATASUM_FLAG)) {
			error = bt_csum_insert(transaction->mutation, &transaction->checksums.root,
			    logical, buffer + (position - start), size);
		}
		if (error == BTRFS_OK) {
			bt_zero(&file, sizeof(file));
			bt_put64(&file.header.generation, generation);
			bt_put64(&file.header.ram_bytes, size);
			file.header.type = BT_EXTENT_REGULAR;
			bt_put64(&file.disk_bytenr, logical);
			bt_put64(&file.disk_bytes, size);
			bt_put64(&file.length, size);
			key = (struct bt_key){
				.objectid = ino, .type = BT_EXTENT_DATA, .offset = position
			};
			error = bt_tx_edit(
			    transaction, &tree->root, key, &file, sizeof(file), BT_INSERT);
		}
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
 * limit zeroed. Inline extents are rewritten from offset 0. */
static enum btrfs_result
bt_tx_rewrite(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct bt_disk_inode *inode, uint64_t ino, uint64_t start, uint64_t end, uint64_t offset,
    const void *bytes, size_t size, uint64_t limit)
{
	const struct btrfs_environment *env = &transaction->base->env;
	uint64_t sector = transaction->base->info.sector_size;
	uint8_t *buffer;
	size_t length;
	enum btrfs_result error = BTRFS_OK;

	if (end - start > BT_TRANSACTION_DATA) {
		return BTRFS_UNSUPPORTED;
	}
	length = (size_t)(end - start);
	buffer = env->allocate(env->context, length);
	if (buffer == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(buffer, length);
	/* Only partially replaced edge sectors need their current bytes. */
	if (offset > start) {
		error = bt_tx_read(transaction, tree, ino, start, buffer,
		    (size_t)(offset - start < sector ? sector : offset - start));
	}
	if (error == BTRFS_OK && offset + size < end) {
		error = bt_tx_read(transaction, tree, ino, end - sector, buffer + (length - sector),
		    (size_t)sector);
	}
	if (error == BTRFS_OK) {
		if (size != 0) {
			bt_copy(buffer + (offset - start), bytes, size);
		}
		if (limit < end) {
			bt_zero(buffer + (limit > start ? limit - start : 0),
			    (size_t)(end - (limit > start ? limit : start)));
		}
		error = bt_tx_replace(transaction, tree, inode, ino, start, end, buffer);
	}
	env->release(env->context, buffer, length);
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
	/* Holes are implicit only with NO_HOLES; explicit hole items are not written. */
	if (!(transaction->base->info.incompat_features & BT_FEATURE_NO_HOLES)) {
		return BTRFS_UNSUPPORTED;
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
	uint64_t inline_end;
	uint64_t start;
	uint64_t end;
	uint64_t tail;
	enum btrfs_result error;

	if (transaction == NULL || (bytes == NULL && size != 0) ||
	    modified.nanoseconds >= 1000000000U) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (offset > BT_FILE_SIZE_LIMIT || size > BT_FILE_SIZE_LIMIT - offset) {
		return BTRFS_RANGE;
	}
	error = bt_tx_data_begin(transaction, id, &tree, &inode, &inline_end);
	if (error != BTRFS_OK || size == 0) {
		return error;
	}
	sector = transaction->base->info.sector_size;
	old_size = bt_u64(inode.size);
	start = offset - offset % sector;
	end = offset + size + (sector - (offset + size) % sector) % sector;
	/* An inline extent becomes part of the first regular extent. */
	if (inline_end != 0) {
		tail = inline_end + (sector - inline_end % sector) % sector;
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
    uint64_t size, struct btrfs_time modified)
{
	struct bt_owned_root *tree;
	struct bt_disk_inode inode;
	uint64_t sector;
	uint64_t old_size;
	uint64_t inline_end;
	uint64_t edge;
	uint64_t removed;
	enum btrfs_result error;

	if (transaction == NULL || modified.nanoseconds >= 1000000000U) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (size > BT_FILE_SIZE_LIMIT) {
		return BTRFS_RANGE;
	}
	error = bt_tx_data_begin(transaction, id, &tree, &inode, &inline_end);
	if (error != BTRFS_OK) {
		return error;
	}
	sector = transaction->base->info.sector_size;
	old_size = bt_u64(inode.size);
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
	if (error == BTRFS_OK && size < old_size) {
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
