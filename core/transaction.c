/* SPDX-License-Identifier: BSD-3-Clause */
#include "transaction.h"

static enum btrfs_result
bt_tx_root(const struct btrfs_fs *fs, uint64_t owner, struct bt_owned_root *root)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = owner, .type = BT_ROOT_ITEM, .offset = UINT64_MAX };
	enum btrfs_result error;

	bt_cursor_init(&cursor, fs, fs->root_tree);
	error = bt_cursor_seek(&cursor, key, 1);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != owner || record.key.type != BT_ROOT_ITEM) {
			error = BTRFS_NOT_FOUND;
		} else if (record.size != sizeof(root->item)) {
			error = BTRFS_UNSUPPORTED;
		} else {
			bt_copy(&root->item, record.data, record.size);
			root->size = record.size;
			root->key = record.key;
			root->root = (struct bt_root){ bt_u64(root->item.legacy.bytenr),
				bt_u64(root->item.legacy.generation), owner,
				root->item.legacy.level };
			if (bt_u64(root->item.legacy.flags) == BT_ROOT_SUBVOL_READ_ONLY) {
				error = BTRFS_READ_ONLY;
			} else if (bt_u64(root->item.legacy.flags) != 0 ||
			    root->item.legacy.drop_level != 0 ||
			    bt_u64(root->item.legacy.drop_progress.objectid) != 0 ||
			    (bt_file_tree(owner) && bt_u32(root->item.legacy.refs) == 0)) {
				/* Dead or partially dropped trees need the cleaner's semantics. */
				error = BTRFS_UNSUPPORTED;
			}
		}
	}
	bt_cursor_fini(&cursor);
	return error;
}

/* Every superblock copy Linux maintains must be intact and agree with the
 * mounted primary. A disagreement is an unresolved earlier publication; a new
 * commit could otherwise overwrite blocks still referenced by a newer copy. */
static enum btrfs_result
bt_tx_copies(struct btrfs_transaction *transaction)
{
	struct bt_disk_super *copy = (void *)transaction->scratch;
	uint64_t offset;
	unsigned mirror;
	enum btrfs_result error;

	transaction->copies = 0;
	for (mirror = 0; mirror < BT_SUPER_MIRRORS; mirror++) {
		if (!bt_super_present(transaction->base->device_size, mirror)) {
			continue;
		}
		offset = bt_super_offset(mirror);
		error = bt_read_physical(transaction->base, offset, copy, BT_SUPER_SIZE);
		if (error != BTRFS_OK) {
			return error;
		}
		if (bt_super_check(copy, offset) != BTRFS_OK ||
		    !bt_super_same(copy, &transaction->original_super)) {
			return BTRFS_RECOVERY_REQUIRED;
		}
		transaction->copies++;
	}
	return BTRFS_OK;
}

enum btrfs_result
btrfs_transaction_begin(const struct btrfs_fs *base,
    const struct btrfs_write_environment *environment, struct btrfs_transaction **result)
{
	return btrfs_transaction_begin_mapped(base, environment, NULL, result);
}

enum btrfs_result
btrfs_transaction_begin_mapped(const struct btrfs_fs *base,
    const struct btrfs_write_environment *environment, struct btrfs_allocation_map *map,
    struct btrfs_transaction **result)
{
	struct btrfs_transaction *transaction;
	struct bt_mutation_allocator allocator;
	struct bt_root quota;
	int mapped;
	enum btrfs_result error;
	uint64_t unsupported = BT_FEATURE_MIXED_GROUPS | BT_FEATURE_METADATA_UUID;
	uint64_t free_space = BT_COMPAT_RO_FREE_SPACE_TREE | BT_COMPAT_RO_FREE_SPACE_TREE_VALID;

	if (result == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (base == NULL || environment == NULL || environment->write == NULL ||
	    environment->flush == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	/* The free-space tree is maintained; other read-only features are not. */
	if ((base->info.readonly_features & ~free_space) != 0 ||
	    ((base->info.readonly_features & free_space) != 0 &&
		(base->info.readonly_features & free_space) != free_space) ||
	    (base->info.incompat_features & unsupported) != 0 ||
	    !(base->info.incompat_features & BT_FEATURE_SKINNY_METADATA) ||
	    base->info.generation == UINT64_MAX) {
		return BTRFS_UNSUPPORTED;
	}
	error = bt_find_root(base, BT_QUOTA_TREE, &quota);
	if (error != BTRFS_NOT_FOUND) {
		return error == BTRFS_OK ? BTRFS_UNSUPPORTED : error;
	}
	transaction = base->env.allocate(base->env.context, sizeof(*transaction));
	if (transaction == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(transaction, sizeof(*transaction));
	transaction->base = base;
	transaction->io = *environment;
	transaction->map = map;
	/* The map holds this generation's verified allocator state, which only this
	 * owner's commits have changed since it was verified. */
	mapped = bt_space_map_fits(map, base);
	transaction->roots = base->root_tree;
	transaction->chunks = base->chunk_tree;
	transaction->fs = *base;
	transaction->fs.chunks =
	    base->env.allocate(base->env.context, BT_MAX_CHUNKS * sizeof(*base->chunks));
	if (transaction->fs.chunks != NULL) {
		bt_copy(transaction->fs.chunks, base->chunks,
		    base->chunk_count * sizeof(*base->chunks));
	}
	transaction->scratch = base->env.allocate(base->env.context, base->info.node_size);
	transaction->original = base->env.allocate(base->env.context, base->info.node_size);
	error = transaction->scratch == NULL || transaction->original == NULL ||
		transaction->fs.chunks == NULL
	    ? BTRFS_NO_MEMORY
	    : bt_read_physical(base, BT_SUPER_OFFSET, &transaction->original_super,
		  sizeof(transaction->original_super));
	if (error == BTRFS_OK &&
	    bt_u64(transaction->original_super.generation) != base->info.generation) {
		error = BTRFS_STALE;
	}
	if (error == BTRFS_OK) {
		error = bt_tx_copies(transaction);
	}
	/* A single copy cannot survive its own torn publication write. */
	if (error == BTRFS_OK && transaction->copies < 2) {
		error = BTRFS_UNSUPPORTED;
	}
	if (error == BTRFS_OK) {
		transaction->super = transaction->original_super;
		error = bt_tx_root(base, BT_EXTENT_TREE, &transaction->extents);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_root(base, BT_CSUM_TREE, &transaction->checksums);
	}
	transaction->has_free_space = (base->info.readonly_features & free_space) != 0;
	if (error == BTRFS_OK && transaction->has_free_space) {
		error = bt_tx_root(base, BT_FREE_SPACE_TREE, &transaction->free_space);
	}
	/* A free-space tree that disagrees with the extent tree is not propagated. */
	if (error == BTRFS_OK && transaction->has_free_space && !mapped) {
		error =
		    bt_fst_verify(base, transaction->free_space.root, transaction->extents.root);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_root(base, BT_UUID_TREE, &transaction->uuids);
		transaction->has_uuids = error == BTRFS_OK;
		error = error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
	}
	if (error == BTRFS_OK) {
		error = bt_find_root(base, BTRFS_TOP_LEVEL_TREE, &transaction->top);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_root(base, BT_DEV_TREE, &transaction->devices);
	}
	if (error == BTRFS_OK && mapped) {
		error = bt_space_from_map(
		    &transaction->fs, map, BT_TRANSACTION_NODES, &transaction->space);
		if (error == BTRFS_OK) {
			transaction->chunks_published =
			    bt_space_original_chunks(transaction->space);
		}
	} else if (error == BTRFS_OK) {
		error = bt_space_create(&transaction->fs, transaction->extents.root,
		    BT_TRANSACTION_NODES, &transaction->space);
		if (error == BTRFS_OK) {
			transaction->chunks_published =
			    bt_space_original_chunks(transaction->space);
			error = bt_space_devices(
			    transaction->space, transaction->chunks, transaction->devices.root);
		}
		/* Only a fully verified state is kept; a failed save only costs the
		 * next transaction a load. */
		if (error == BTRFS_OK && map != NULL) {
			(void)bt_space_map_save(transaction->space, map);
		}
	}
	if (error == BTRFS_OK) {
		bt_space_allocator(transaction->space, &allocator);
		error = bt_mutation_create(&transaction->fs, &allocator, &transaction->mutation);
	}
	if (error != BTRFS_OK) {
		btrfs_transaction_destroy(transaction);
		return error;
	}
	*result = transaction;
	return BTRFS_OK;
}

/* Opens a writable file tree once per transaction; its committed root item
 * remains the reference for sharing decisions until publication. */
enum btrfs_result
bt_tx_tree(struct btrfs_transaction *transaction, uint64_t tree, struct bt_owned_root **result)
{
	struct bt_owned_root *owned;
	size_t i;
	enum btrfs_result error;

	for (i = 0; i < transaction->tree_count; i++) {
		if (transaction->trees[i].root.owner == tree) {
			*result = &transaction->trees[i];
			return transaction->trees[i].read_only ? BTRFS_READ_ONLY : BTRFS_OK;
		}
	}
	if (!bt_file_tree(tree)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (transaction->tree_count == BT_TRANSACTION_TREES) {
		return BTRFS_UNSUPPORTED;
	}
	owned = &transaction->trees[transaction->tree_count];
	error = bt_tx_root(transaction->base, tree, owned);
	if (error == BTRFS_OK) {
		transaction->tree_count++;
		*result = owned;
	}
	return error;
}

enum btrfs_result
bt_tx_source(struct btrfs_transaction *transaction, uint64_t tree, struct bt_owned_root **result)
{
	struct bt_owned_root *owned;
	size_t i;
	enum btrfs_result error;

	for (i = 0; i < transaction->tree_count; i++) {
		if (transaction->trees[i].root.owner == tree) {
			*result = &transaction->trees[i];
			return BTRFS_OK;
		}
	}
	if (!bt_file_tree(tree)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (transaction->tree_count == BT_TRANSACTION_TREES) {
		return BTRFS_UNSUPPORTED;
	}
	owned = &transaction->trees[transaction->tree_count];
	error = bt_tx_root(transaction->base, tree, owned);
	owned->read_only = error == BTRFS_READ_ONLY;
	if (error == BTRFS_OK || error == BTRFS_READ_ONLY) {
		transaction->tree_count++;
		*result = owned;
		error = BTRFS_OK;
	}
	return error;
}

enum btrfs_result
bt_tx_root_item(struct btrfs_transaction *transaction, uint64_t tree, struct bt_owned_root *result)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { tree, UINT64_MAX, BT_ROOT_ITEM };
	enum btrfs_result error;

	bt_cursor_init(&cursor, bt_mutation_view(transaction->mutation), transaction->roots);
	error = bt_cursor_seek(&cursor, key, 1);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != tree || record.key.type != BT_ROOT_ITEM) {
			error = BTRFS_NOT_FOUND;
		} else if (record.size != sizeof(result->item)) {
			error = BTRFS_UNSUPPORTED;
		} else {
			bt_copy(&result->item, record.data, record.size);
			result->size = record.size;
			result->key = record.key;
			result->root = (struct bt_root){ bt_u64(result->item.legacy.bytenr),
				bt_u64(result->item.legacy.generation), tree,
				result->item.legacy.level };
		}
	}
	bt_cursor_fini(&cursor);
	return error;
}

enum btrfs_result
bt_tx_add_tree(struct btrfs_transaction *transaction, const struct bt_owned_root *owned,
    struct bt_owned_root **result)
{
	size_t i;

	for (i = 0; i < transaction->tree_count; i++) {
		if (transaction->trees[i].root.owner == owned->root.owner) {
			return BTRFS_CORRUPT;
		}
	}
	if (transaction->tree_count == BT_TRANSACTION_TREES) {
		return BTRFS_UNSUPPORTED;
	}
	transaction->trees[transaction->tree_count] = *owned;
	*result = &transaction->trees[transaction->tree_count++];
	return BTRFS_OK;
}

enum btrfs_result
bt_tx_root_id(struct btrfs_transaction *transaction, uint64_t *result)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { BT_LAST_FREE_OBJECTID, 0, 0 };
	uint64_t next = transaction->next_root;
	enum btrfs_result error = BTRFS_OK;

	if (next == 0 && transaction->counters != NULL) {
		next = transaction->counters->next_root;
	}
	if (next == 0) {
		next = BTRFS_ROOT_INODE;
		bt_cursor_init(
		    &cursor, bt_mutation_view(transaction->mutation), transaction->roots);
		error = bt_cursor_seek(&cursor, key, 1);
		if (error == BTRFS_OK) {
			(void)bt_cursor_record(&cursor, &record);
			if (record.key.objectid >= BTRFS_ROOT_INODE &&
			    record.key.objectid < BT_LAST_FREE_OBJECTID) {
				next = record.key.objectid + 1;
			}
		}
		bt_cursor_fini(&cursor);
		if (error == BTRFS_NOT_FOUND) {
			error = BTRFS_OK;
		}
	}
	if (error != BTRFS_OK) {
		return error;
	}
	if (next >= BT_ROOT_ID_LIMIT) {
		return BTRFS_NO_SPACE;
	}
	*result = next;
	transaction->next_root = next + 1;
	if (transaction->counters != NULL) {
		transaction->counters->next_root = next + 1;
	}
	return BTRFS_OK;
}

enum btrfs_result
bt_tx_edit(struct btrfs_transaction *transaction, struct bt_root *root, struct bt_key key,
    const void *data, size_t size, enum bt_edit edit)
{
	enum btrfs_result error;

	error = bt_mutation_edit(transaction->mutation, root, key, data, size, edit);
	if (error != BTRFS_OK) {
		transaction->failure = error;
	}
	return error;
}

enum btrfs_result
btrfs_transaction_write_inline(struct btrfs_transaction *transaction, struct btrfs_object_id id,
    const void *bytes, size_t size, struct btrfs_time modified)
{
	struct bt_disk_inode inode;
	struct bt_disk_extent_header *extent;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key inode_key = { .objectid = id.inode, .type = BT_INODE_ITEM };
	struct bt_key extent_key = { .objectid = id.inode, .type = BT_EXTENT_DATA };
	const struct btrfs_fs *view;
	struct bt_owned_root *tree;
	size_t length;
	enum btrfs_result error;

	if (transaction == NULL || (bytes == NULL && size != 0) ||
	    modified.nanoseconds >= 1000000000U) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (transaction->failure != BTRFS_OK || transaction->finished) {
		return transaction->failure == BTRFS_OK ? BTRFS_READ_ONLY : transaction->failure;
	}
	if (size > BT_INLINE_WRITE_LIMIT) {
		return BTRFS_UNSUPPORTED;
	}
	error = bt_tx_tree(transaction, id.tree, &tree);
	if (error != BTRFS_OK) {
		return error;
	}
	error = bt_mutation_find(
	    transaction->mutation, tree->root, inode_key, &inode, sizeof(inode), &length);
	if (error != BTRFS_OK) {
		return error;
	}
	if (length != sizeof(inode) ||
	    (bt_u32(inode.mode) & BTRFS_MODE_TYPE) != BTRFS_MODE_REGULAR ||
	    bt_u64(inode.sequence) == UINT64_MAX) {
		return BTRFS_UNSUPPORTED;
	}
	if ((bt_u64(inode.flags) & (BT_INODE_IMMUTABLE | BT_INODE_APPEND)) != 0) {
		return BTRFS_NOT_PERMITTED;
	}
	error = bt_tx_privileges_settled(transaction, tree, id.inode, &inode);
	if (error != BTRFS_OK) {
		return error;
	}
	/* Require exactly one existing, ordinary inline extent. This prevents a
	 * partial conversion from leaking external extents or changing snapshots. */
	view = bt_mutation_view(transaction->mutation);
	bt_cursor_init(&cursor, view, tree->root);
	error = bt_cursor_seek(&cursor, extent_key, 0);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		extent = (void *)record.data;
		if (bt_key_compare(record.key, extent_key) != 0 || record.size < sizeof(*extent) ||
		    extent->type != BT_EXTENT_INLINE ||
		    extent->compression != BTRFS_COMPRESSION_NONE || extent->encryption != 0 ||
		    bt_u16(extent->encoding) != 0 ||
		    record.size - sizeof(*extent) != bt_u64(inode.size)) {
			error = BTRFS_UNSUPPORTED;
		} else {
			error = bt_cursor_next(&cursor);
			if (error == BTRFS_OK) {
				(void)bt_cursor_record(&cursor, &record);
				error = record.key.objectid == id.inode &&
					record.key.type == BT_EXTENT_DATA
				    ? BTRFS_UNSUPPORTED
				    : BTRFS_OK;
			} else if (error == BTRFS_NOT_FOUND) {
				error = BTRFS_OK;
			}
		}
	}
	bt_cursor_fini(&cursor);
	if (error != BTRFS_OK) {
		return error;
	}
	extent = (void *)transaction->scratch;
	bt_zero(extent, sizeof(*extent));
	bt_put64(&extent->generation, transaction->base->info.generation + 1);
	bt_put64(&extent->ram_bytes, size);
	extent->type = BT_EXTENT_INLINE;
	bt_copy(extent + 1, bytes, size);
	error = size == 0 ? bt_tx_edit(transaction, &tree->root, extent_key, NULL, 0, BT_DELETE)
			  : bt_tx_edit(transaction, &tree->root, extent_key, extent,
				sizeof(*extent) + size, BT_REPLACE);
	if (error == BTRFS_OK) {
		bt_put64(&inode.transid, transaction->base->info.generation + 1);
		bt_put64(&inode.size, size);
		bt_put64(&inode.nbytes, size);
		bt_put64(&inode.sequence, bt_u64(inode.sequence) + 1);
		bt_put64(&inode.mtime.seconds, (uint64_t)modified.seconds);
		bt_put32(&inode.mtime.nanoseconds, modified.nanoseconds);
		inode.ctime = inode.mtime;
		/* Set-id inodes were settled before editing: the owning adapter
		 * decided to drop or keep their privileges in this transaction. */
		error = bt_tx_edit(
		    transaction, &tree->root, inode_key, &inode, sizeof(inode), BT_REPLACE);
	}
	if (error == BTRFS_OK) {
		transaction->changed = 1;
		bt_put64(&tree->item.ctransid, transaction->base->info.generation + 1);
		tree->item.ctime = inode.ctime;
	}
	return error;
}

/* Adds or removes the references a tree block's content holds: child blocks for
 * nodes, regular and preallocated data extents for leaves. full selects the
 * shared form naming this block as parent; otherwise references name root. */
enum btrfs_result
bt_tx_children(struct btrfs_transaction *transaction, const uint8_t *node, uint64_t address,
    int full, uint64_t root, int add)
{
	const struct bt_disk_header *header = (const void *)node;
	const struct bt_disk_item *items = (const void *)(header + 1);
	const struct bt_disk_pointer *pointers = (const void *)(header + 1);
	const struct bt_disk_extent *file;
	struct bt_backref reference;
	struct bt_key extent;
	struct bt_key key;
	uint32_t i;
	int freed = 0;
	enum btrfs_result error = BTRFS_OK;

	for (i = 0; error == BTRFS_OK && i < bt_u32(header->count); i++) {
		bt_zero(&reference, sizeof(reference));
		if (header->level != 0) {
			extent = (struct bt_key){ .objectid = bt_u64(pointers[i].bytenr),
				.type = BT_METADATA_ITEM,
				.offset = (uint64_t)header->level - 1 };
		} else {
			key = bt_key_decode(&items[i].key);
			file =
			    (const void *)((const uint8_t *)(header + 1) + bt_u32(items[i].offset));
			if (key.type != BT_EXTENT_DATA) {
				continue;
			}
			if (bt_u32(items[i].size) < sizeof(file->header)) {
				return BTRFS_CORRUPT;
			}
			if (file->header.type == BT_EXTENT_INLINE) {
				continue;
			}
			if (bt_u32(items[i].size) < sizeof(*file)) {
				return BTRFS_CORRUPT;
			}
			if (bt_u64(file->disk_bytenr) == 0) {
				continue;
			}
			extent = (struct bt_key){ .objectid = bt_u64(file->disk_bytenr),
				.type = BT_EXTENT_ITEM,
				.offset = bt_u64(file->disk_bytes) };
			reference.data = 1;
			reference.inode = key.objectid;
			reference.offset = key.offset - bt_u64(file->offset);
		}
		if (full) {
			reference.parent = address;
			reference.inode = 0;
			reference.offset = 0;
		} else {
			reference.root = root;
		}
		error = add ? bt_backref_add(transaction->mutation, &transaction->extents.root,
				  extent, &reference, 1)
			    : bt_backref_drop(transaction->mutation, &transaction->extents.root,
				  extent, &reference, 1, &freed);
		/* Converting one reference form into another never frees an extent. */
		if (error == BTRFS_OK && !add && freed) {
			error = BTRFS_CORRUPT;
		}
	}
	return error;
}

/* Linux's reference update for a CoW of a committed block (update_ref_for_cow).
 * A block in a shareable tree may be shared when it is not the tree's root and
 * not newer than the tree's last snapshot. If the owner of a shared block CoWs
 * it, the old block's children switch to references naming it as parent; any
 * other tree adds references from its root for the copy. An unshared block with
 * parent-named children converts them back. The old block then loses this tree's
 * reference and is freed only with its last one. */
static enum btrfs_result
bt_tx_release(struct btrfs_transaction *transaction, const struct bt_mutated_block *block)
{
	struct bt_owned_root *tree = NULL;
	struct bt_backref reference;
	struct bt_root original = { block->original_address, block->original_generation,
		block->owner, block->original_level };
	struct bt_key extent = { .objectid = block->original_address,
		.type = BT_METADATA_ITEM,
		.offset = block->original_level };
	uint64_t refs;
	uint64_t flags;
	size_t i;
	int shareable = 0;
	int freed;
	enum btrfs_result error;

	if (block->original_flags >> BT_HEADER_BACKREF_SHIFT !=
	    BT_HEADER_MIXED_BACKREF >> BT_HEADER_BACKREF_SHIFT) {
		return BTRFS_UNSUPPORTED;
	}
	for (i = 0; i < transaction->tree_count; i++) {
		if (transaction->trees[i].root.owner == block->owner) {
			tree = &transaction->trees[i];
		}
	}
	if (tree != NULL) {
		shareable = block->original_address != bt_u64(tree->item.legacy.bytenr) &&
		    (block->original_generation <= bt_u64(tree->item.legacy.last_snapshot) ||
			(block->original_flags & BT_HEADER_RELOC) != 0);
	} else if (bt_file_tree(block->owner)) {
		return BTRFS_CORRUPT;
	}
	error = bt_backref_info(
	    transaction->mutation, transaction->extents.root, extent, &refs, &flags);
	if (error != BTRFS_OK) {
		return error;
	}
	if (refs > 1 && !shareable) {
		return BTRFS_CORRUPT;
	}
	if (refs > 1 || (flags & BT_EXTENT_FLAG_FULL_BACKREF) != 0) {
		error = bt_tree_read(transaction->base, original, transaction->original);
		if (error != BTRFS_OK) {
			return error;
		}
	}
	if (refs > 1 && block->original_owner == block->owner &&
	    !(flags & BT_EXTENT_FLAG_FULL_BACKREF)) {
		error = bt_tx_children(
		    transaction, transaction->original, block->original_address, 1, 0, 1);
		if (error == BTRFS_OK) {
			error = bt_backref_set_flags(transaction->mutation,
			    &transaction->extents.root, extent, BT_EXTENT_FLAG_FULL_BACKREF);
		}
	} else if (refs > 1) {
		error = bt_tx_children(transaction, transaction->original, block->original_address,
		    0, block->owner, 1);
	} else if (flags & BT_EXTENT_FLAG_FULL_BACKREF) {
		error = bt_tx_children(transaction, transaction->original, block->original_address,
		    0, block->owner, 1);
		if (error == BTRFS_OK) {
			error = bt_tx_children(
			    transaction, transaction->original, block->original_address, 1, 0, 0);
		}
	} else if (block->original_owner != block->owner) {
		/* A sole implicit reference always comes from the owning tree. */
		return BTRFS_CORRUPT;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	bt_zero(&reference, sizeof(reference));
	reference.root = block->owner;
	error = bt_backref_drop(
	    transaction->mutation, &transaction->extents.root, extent, &reference, 1, &freed);
	if (error == BTRFS_OK && freed != (refs == 1)) {
		error = BTRFS_CORRUPT;
	}
	return error == BTRFS_OK && freed
	    ? bt_space_change_used(
		  transaction->space, block->original_address, transaction->base->info.node_size, 0)
	    : error;
}

static enum btrfs_result
bt_tx_account(struct btrfs_transaction *transaction)
{
	struct {
		struct bt_disk_extent_item extent;
		struct bt_disk_inline_ref reference;
	} item;
	struct bt_mutated_block block;
	struct bt_key key;
	size_t i;
	enum btrfs_result error;

	for (i = 0; i < bt_mutation_count(transaction->mutation); i++) {
		error = bt_mutation_block(transaction->mutation, i, &block);
		if (error != BTRFS_OK) {
			return error;
		}
		if (block.original_address != 0 &&
		    !(transaction->accounted[i] & BT_ACCOUNT_ORIGINAL)) {
			error = bt_tx_release(transaction, &block);
			if (error != BTRFS_OK) {
				return error;
			}
			transaction->accounted[i] |= BT_ACCOUNT_ORIGINAL;
		}
		key = (struct bt_key){
			.objectid = block.address, .type = BT_METADATA_ITEM, .offset = block.level
		};
		if (!block.discarded && !(transaction->accounted[i] & BT_ACCOUNT_NEW)) {
			bt_put64(&item.extent.refs, 1);
			bt_put64(&item.extent.generation, transaction->base->info.generation + 1);
			bt_put64(&item.extent.flags, BT_EXTENT_FLAG_TREE);
			item.reference.type = BT_TREE_BLOCK_REF;
			bt_put64(&item.reference.offset, block.owner);
			error = bt_tx_edit(transaction, &transaction->extents.root, key, &item,
			    sizeof(item), BT_INSERT);
			if (error == BTRFS_OK) {
				error = bt_space_change_used(transaction->space, block.address,
				    transaction->base->info.node_size, 1);
			}
			if (error != BTRFS_OK) {
				return error;
			}
			transaction->accounted[i] |= BT_ACCOUNT_NEW;
		} else if (block.discarded && (transaction->accounted[i] & BT_ACCOUNT_NEW)) {
			error = bt_tx_edit(
			    transaction, &transaction->extents.root, key, NULL, 0, BT_DELETE);
			if (error == BTRFS_OK) {
				error = bt_space_change_used(transaction->space, block.address,
				    transaction->base->info.node_size, 0);
			}
			if (error != BTRFS_OK) {
				return error;
			}
			transaction->accounted[i] &= (uint8_t)~BT_ACCOUNT_NEW;
		}
	}
	return BTRFS_OK;
}

static enum btrfs_result
bt_tx_update_root(struct btrfs_transaction *transaction, struct bt_owned_root *root)
{
	struct bt_disk_root_full item = root->item;
	struct bt_mutated_block block;
	uint64_t used = bt_u64(item.legacy.used_bytes);
	uint64_t node_size = transaction->base->info.node_size;
	size_t i;
	enum btrfs_result error;

	for (i = 0; i < bt_mutation_count(transaction->mutation); i++) {
		error = bt_mutation_block(transaction->mutation, i, &block);
		if (error != BTRFS_OK) {
			return error;
		}
		if (block.owner == root->root.owner) {
			if (block.original_address != 0) {
				if (used < node_size) {
					return BTRFS_CORRUPT;
				}
				used -= node_size;
			}
			if (!block.discarded) {
				if (used > UINT64_MAX - node_size) {
					return BTRFS_CORRUPT;
				}
				used += node_size;
			}
		}
	}
	bt_put64(&item.legacy.bytenr, root->root.address);
	bt_put64(&item.legacy.generation, root->root.generation);
	bt_put64(&item.generation_v2, root->root.generation);
	bt_put64(&item.legacy.used_bytes, used);
	item.legacy.level = root->root.level;
	return bt_tx_edit(
	    transaction, &transaction->roots, root->key, &item, root->size, BT_REPLACE);
}

static void
bt_tx_backup(struct btrfs_transaction *transaction)
{
	struct bt_disk_root_backup *backup;
	struct bt_root top = transaction->top;
	unsigned oldest = 0;
	unsigned i;

	for (i = 0; i < transaction->tree_count; i++) {
		if (transaction->trees[i].root.owner == BTRFS_TOP_LEVEL_TREE) {
			top = transaction->trees[i].root;
		}
	}
	for (i = 1; i < BT_BACKUP_ROOTS; i++) {
		if (bt_u64(transaction->super.backup_roots[i].tree_generation) <
		    bt_u64(transaction->super.backup_roots[oldest].tree_generation)) {
			oldest = i;
		}
	}
	backup = &transaction->super.backup_roots[oldest];
	bt_zero(backup, sizeof(*backup));
	bt_put64(&backup->tree, transaction->roots.address);
	bt_put64(&backup->tree_generation, transaction->roots.generation);
	backup->tree_level = transaction->roots.level;
	bt_put64(&backup->extent, transaction->extents.root.address);
	bt_put64(&backup->extent_generation, transaction->extents.root.generation);
	backup->extent_level = transaction->extents.root.level;
	bt_put64(&backup->files, top.address);
	bt_put64(&backup->files_generation, top.generation);
	backup->files_level = top.level;
	bt_put64(&backup->chunk, transaction->chunks.address);
	bt_put64(&backup->chunk_generation, transaction->chunks.generation);
	backup->chunk_level = transaction->chunks.level;
	bt_put64(&backup->device, transaction->devices.root.address);
	bt_put64(&backup->device_generation, transaction->devices.root.generation);
	backup->device_level = transaction->devices.root.level;
	bt_put64(&backup->checksum, transaction->checksums.root.address);
	bt_put64(&backup->checksum_generation, transaction->checksums.root.generation);
	backup->checksum_level = transaction->checksums.root.level;
	backup->total_bytes = transaction->super.total_bytes;
	backup->used_bytes = transaction->super.used_bytes;
	bt_put64(&backup->devices, 1);
}

static enum btrfs_result
bt_tx_system_array_add(
    struct btrfs_transaction *transaction, struct bt_key key, const void *item, size_t size)
{
	struct bt_disk_key wire;
	uint32_t used = bt_u32(transaction->super.system_array_size);

	if (used > BT_SYSTEM_ARRAY_SIZE || sizeof(wire) + size > BT_SYSTEM_ARRAY_SIZE - used) {
		return BTRFS_NO_SPACE;
	}
	bt_key_encode(&wire, key);
	bt_copy(transaction->super.system_array + used, &wire, sizeof(wire));
	bt_copy(transaction->super.system_array + used + sizeof(wire), item, size);
	bt_put32(&transaction->super.system_array_size, used + (uint32_t)(sizeof(wire) + size));
	return BTRFS_OK;
}

/* Records chunks created by growth: chunk item and device item in the chunk
 * tree, device extents, the block group (its total follows each round) and,
 * with a free-space tree, its info and one free extent before any logged
 * allocation inside it is applied. */
static enum btrfs_result
bt_tx_publish_chunks(struct btrfs_transaction *transaction)
{
	struct {
		struct bt_disk_chunk chunk;
		struct bt_disk_stripe stripes[2];
	} item;
	struct bt_disk_dev_extent extent;
	struct bt_disk_device device;
	struct bt_disk_block_group group;
	struct bt_disk_free_space_info info;
	const struct bt_chunk *chunk;
	const struct btrfs_fs *base = transaction->base;
	struct bt_key key;
	size_t size;
	unsigned stripe;
	enum btrfs_result error = BTRFS_OK;

	for (; error == BTRFS_OK && transaction->chunks_published < transaction->fs.chunk_count;
	    transaction->chunks_published++) {
		chunk = &transaction->fs.chunks[transaction->chunks_published];
		bt_zero(&item, sizeof(item));
		bt_put64(&item.chunk.length, chunk->length);
		bt_put64(&item.chunk.owner, BT_EXTENT_TREE);
		bt_put64(&item.chunk.stripe_length, BT_STRIPE_LENGTH);
		bt_put64(&item.chunk.type, chunk->type);
		bt_put32(&item.chunk.io_align, BT_STRIPE_LENGTH);
		bt_put32(&item.chunk.io_width, BT_STRIPE_LENGTH);
		bt_put32(&item.chunk.sector_size, base->info.sector_size);
		bt_put16(&item.chunk.stripes, chunk->mirrors);
		bt_put16(&item.chunk.sub_stripes, 1);
		for (stripe = 0; stripe < chunk->mirrors; stripe++) {
			bt_put64(&item.stripes[stripe].device, base->device_id);
			bt_put64(&item.stripes[stripe].offset, chunk->physical[stripe]);
			bt_copy(item.stripes[stripe].uuid, base->device_uuid, BTRFS_UUID_SIZE);
		}
		key = (struct bt_key){ BT_FIRST_CHUNK_OBJECTID, chunk->logical, BT_CHUNK_ITEM };
		size = sizeof(item.chunk) + chunk->mirrors * sizeof(item.stripes[0]);
		error = bt_tx_edit(transaction, &transaction->chunks, key, &item, size, BT_INSERT);
		/* Chunk-tree blocks may live in a new system chunk, so the superblock's
		 * bootstrap array names it too (btrfs_add_system_chunk). */
		if (error == BTRFS_OK && (chunk->type & BT_BLOCK_SYSTEM) != 0) {
			error = bt_tx_system_array_add(transaction, key, &item, size);
		}
		for (stripe = 0; error == BTRFS_OK && stripe < chunk->mirrors; stripe++) {
			bt_zero(&extent, sizeof(extent));
			bt_put64(&extent.chunk_tree, BT_CHUNK_TREE);
			bt_put64(&extent.chunk_objectid, BT_FIRST_CHUNK_OBJECTID);
			bt_put64(&extent.chunk_offset, chunk->logical);
			bt_put64(&extent.length, chunk->length);
			key = (struct bt_key){ base->device_id, chunk->physical[stripe],
				BT_DEV_EXTENT };
			error = bt_tx_edit(transaction, &transaction->devices.root, key, &extent,
			    sizeof(extent), BT_INSERT);
		}
		key = (struct bt_key){ BT_DEV_ITEMS_OBJECTID, base->device_id, BT_DEV_ITEM };
		if (error == BTRFS_OK) {
			error = bt_mutation_find(transaction->mutation, transaction->chunks, key,
			    &device, sizeof(device), &size);
			error = error == BTRFS_OK && size != sizeof(device) ? BTRFS_CORRUPT : error;
		}
		if (error == BTRFS_OK) {
			bt_put64(&device.used_bytes,
			    bt_u64(device.used_bytes) + chunk->length * chunk->mirrors);
			transaction->super.device.used_bytes = device.used_bytes;
			error = bt_tx_edit(transaction, &transaction->chunks, key, &device,
			    sizeof(device), BT_REPLACE);
		}
		if (error == BTRFS_OK) {
			bt_zero(&group, sizeof(group));
			bt_put64(&group.chunk_objectid, BT_FIRST_CHUNK_OBJECTID);
			bt_put64(&group.flags, chunk->type);
			key = (struct bt_key){ chunk->logical, chunk->length, BT_BLOCK_GROUP_ITEM };
			error = bt_tx_edit(transaction, &transaction->extents.root, key, &group,
			    sizeof(group), BT_INSERT);
		}
		if (error == BTRFS_OK && transaction->has_free_space) {
			bt_put32(&info.extent_count, 1);
			bt_put32(&info.flags, 0);
			key = (struct bt_key){ chunk->logical, chunk->length, BT_FREE_SPACE_INFO };
			error = bt_tx_edit(transaction, &transaction->free_space.root, key, &info,
			    sizeof(info), BT_INSERT);
			key.type = BT_FREE_SPACE_EXTENT;
			if (error == BTRFS_OK) {
				error = bt_tx_edit(transaction, &transaction->free_space.root, key,
				    NULL, 0, BT_INSERT);
			}
		}
	}
	return error;
}

/* Applies the allocations and releases logged since the previous round. */
static enum btrfs_result
bt_tx_free_space(struct btrfs_transaction *transaction)
{
	const struct bt_space_change *change;
	enum btrfs_result error = BTRFS_OK;

	if (!transaction->has_free_space) {
		return BTRFS_OK;
	}
	for (; error == BTRFS_OK &&
	    transaction->free_space_applied < bt_space_change_count(transaction->space);
	    transaction->free_space_applied++) {
		change = bt_space_change(transaction->space, transaction->free_space_applied);
		if (change->chunk >= transaction->chunks_published) {
			error = bt_tx_publish_chunks(transaction);
			if (error != BTRFS_OK) {
				break;
			}
		}
		error = bt_fst_change(transaction->mutation, &transaction->free_space.root,
		    &transaction->fs.chunks[change->chunk], change->start, change->length,
		    change->allocate);
	}
	if (error == BTRFS_OK &&
	    transaction->free_space.root.address !=
		bt_u64(transaction->free_space.item.legacy.bytenr)) {
		error = bt_tx_update_root(transaction, &transaction->free_space);
	}
	return error;
}

static enum btrfs_result
bt_tx_prepare(struct btrfs_transaction *transaction)
{
	struct bt_disk_block_group item;
	struct bt_mutated_block block;
	const struct bt_chunk *chunk;
	struct bt_key key;
	uint64_t used;
	size_t i;
	size_t before;
	size_t round;
	int pending;
	enum btrfs_result error;

	for (i = 0; i < transaction->tree_count; i++) {
		error = bt_tx_update_root(transaction, &transaction->trees[i]);
		if (error != BTRFS_OK) {
			return error;
		}
	}
	/* CoW-derived references first, then file extent references, as Linux
	 * runs reference updates from CoW before the item edits that follow it. */
	error = bt_tx_account(transaction);
	if (error == BTRFS_OK) {
		error = bt_tx_apply_refs(transaction);
	}
	if (error == BTRFS_OK &&
	    transaction->checksums.root.address !=
		bt_u64(transaction->checksums.item.legacy.bytenr)) {
		error = bt_tx_update_root(transaction, &transaction->checksums);
	}
	if (error == BTRFS_OK && transaction->has_uuids &&
	    transaction->uuids.root.address != bt_u64(transaction->uuids.item.legacy.bytenr)) {
		error = bt_tx_update_root(transaction, &transaction->uuids);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_remove_groups(transaction);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	/* Extent references, block groups and root items may themselves CoW more
	 * blocks. Resolve this bounded fixed point before issuing any I/O. */
	for (round = 0; round < BT_TRANSACTION_NODES; round++) {
		error = bt_tx_account(transaction);
		if (error != BTRFS_OK) {
			return error;
		}
		before = bt_mutation_count(transaction->mutation);
		used = 0;
		for (i = 0; i < transaction->fs.chunk_count; i++) {
			error = bt_tx_publish_chunks(transaction);
			if (error != BTRFS_OK) {
				return error;
			}
			chunk = &transaction->fs.chunks[i];
			if (chunk->removed) {
				continue;
			}
			bt_put64(&item.used_bytes, bt_space_used(transaction->space, i));
			bt_put64(&item.chunk_objectid, BT_FIRST_CHUNK_OBJECTID);
			bt_put64(&item.flags, chunk->type);
			key = (struct bt_key){ .objectid = chunk->logical,
				.type = BT_BLOCK_GROUP_ITEM,
				.offset = chunk->length };
			error = bt_tx_edit(transaction, &transaction->extents.root, key, &item,
			    sizeof(item), BT_REPLACE);
			if (error != BTRFS_OK) {
				return error;
			}
			used += bt_space_used(transaction->space, i);
		}
		error = bt_tx_free_space(transaction);
		if (error == BTRFS_OK) {
			error = bt_tx_update_root(transaction, &transaction->extents);
		}
		if (error == BTRFS_OK &&
		    transaction->devices.root.address !=
			bt_u64(transaction->devices.item.legacy.bytenr)) {
			error = bt_tx_update_root(transaction, &transaction->devices);
		}
		if (error != BTRFS_OK) {
			return error;
		}
		pending = before != bt_mutation_count(transaction->mutation);
		for (i = 0; !pending && i < before; i++) {
			error = bt_mutation_block(transaction->mutation, i, &block);
			if (error != BTRFS_OK) {
				return error;
			}
			pending = (block.original_address != 0 &&
				      !(transaction->accounted[i] & BT_ACCOUNT_ORIGINAL)) ||
			    ((transaction->accounted[i] & BT_ACCOUNT_NEW) != 0) ==
				(block.discarded != 0);
		}
		if (!pending) {
			bt_put64(&transaction->super.used_bytes, used);
			bt_put64(
			    &transaction->super.generation, transaction->base->info.generation + 1);
			bt_put64(&transaction->super.root, transaction->roots.address);
			transaction->super.root_level = transaction->roots.level;
			bt_put64(&transaction->super.chunk_root, transaction->chunks.address);
			bt_put64(
			    &transaction->super.chunk_generation, transaction->chunks.generation);
			transaction->super.chunk_level = transaction->chunks.level;
			/* A v1 free-space cache is advisory and has not been maintained. */
			bt_put64(&transaction->super.cache_generation, UINT64_MAX);
			bt_tx_backup(transaction);
			return bt_mutation_seal(transaction->mutation);
		}
	}
	return BTRFS_NO_SPACE;
}

static enum btrfs_result
bt_tx_publish(struct btrfs_transaction *transaction, unsigned mirror)
{
	uint64_t offset = bt_super_offset(mirror);

	bt_super_seal(&transaction->super, offset);
	return transaction->io.write(
	    transaction->io.context, offset, &transaction->super, sizeof(transaction->super));
}

static enum btrfs_result
bt_tx_persist(struct btrfs_transaction *transaction)
{
	struct bt_mutated_block block;
	uint64_t physical;
	size_t i;
	unsigned mirrors;
	unsigned mirror;
	unsigned level;
	enum btrfs_result error;

	/* New data was written when its extents were created; it and the
	 * metadata naming it are durable at the first barrier. */
	for (level = 0; level < BT_MAX_LEVEL; level++) {
		for (i = 0; i < bt_mutation_count(transaction->mutation); i++) {
			error = bt_mutation_block(transaction->mutation, i, &block);
			if (error != BTRFS_OK) {
				return error;
			}
			if (block.discarded || block.level != level) {
				continue;
			}
			mirrors = 1;
			for (mirror = 0; mirror < mirrors; mirror++) {
				error = bt_map(&transaction->fs, block.address, block.size,
				    block.owner == BT_CHUNK_TREE ? BT_BLOCK_SYSTEM
								 : BT_BLOCK_METADATA,
				    mirror, &physical, &mirrors);
				if (error == BTRFS_OK) {
					error = transaction->io.write(transaction->io.context,
					    physical, block.bytes, block.size);
				}
				if (error != BTRFS_OK) {
					return error;
				}
			}
		}
	}
	error = transaction->io.flush(transaction->io.context);
	if (error != BTRFS_OK) {
		return error;
	}
	/* Secondary copies, barrier, primary, barrier. Until the second barrier the
	 * untouched primary holds the acknowledged generation; afterwards the durable
	 * secondaries hold the new one. A torn copy is resolved only by explicit
	 * recovery; the immutable reader never silently selects a mirror. */
	for (mirror = BT_SUPER_MIRRORS - 1; mirror != 0; mirror--) {
		if (bt_super_present(transaction->base->device_size, mirror)) {
			error = bt_tx_publish(transaction, mirror);
			if (error != BTRFS_OK) {
				return error;
			}
		}
	}
	error = transaction->io.flush(transaction->io.context);
	if (error == BTRFS_OK) {
		error = bt_tx_publish(transaction, 0);
	}
	return error == BTRFS_OK ? transaction->io.flush(transaction->io.context) : error;
}

enum btrfs_result
btrfs_transaction_commit(struct btrfs_transaction *transaction)
{
	enum btrfs_result error;

	if (transaction == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (transaction->finished || transaction->failure != BTRFS_OK) {
		return transaction->failure == BTRFS_OK ? BTRFS_READ_ONLY : transaction->failure;
	}
	transaction->finished = 1;
	if (!transaction->changed) {
		return BTRFS_OK;
	}
	error = bt_tx_prepare(transaction);
	if (error == BTRFS_OK) {
		error = bt_read_physical(
		    transaction->base, BT_SUPER_OFFSET, transaction->scratch, BT_SUPER_SIZE);
		if (error == BTRFS_OK &&
		    !bt_equal(transaction->scratch, &transaction->original_super, BT_SUPER_SIZE)) {
			error = BTRFS_STALE;
		}
		if (error == BTRFS_OK) {
			error = bt_tx_copies(transaction);
			error = error == BTRFS_RECOVERY_REQUIRED ? BTRFS_STALE : error;
		}
	}
	if (error == BTRFS_OK) {
		transaction->writing = 1;
		error = bt_tx_persist(transaction);
	}
	if (error == BTRFS_OK) {
		error = bt_mutation_accept(transaction->mutation);
	}
	/* The commit is durable; a map that cannot follow it is dropped and the
	 * next transaction loads and verifies again. */
	if (error == BTRFS_OK && transaction->map != NULL &&
	    bt_space_map_commit(transaction->space, transaction->map,
		transaction->base->info.generation + 1) != BTRFS_OK) {
		bt_space_map_invalidate(transaction->map);
	}
	transaction->failure = error;
	return error;
}

void
btrfs_transaction_destroy(struct btrfs_transaction *transaction)
{
	const struct btrfs_environment *env;

	if (transaction == NULL) {
		return;
	}
	env = &transaction->base->env;
	bt_tx_release_data(transaction);
	bt_mutation_destroy(transaction->mutation);
	bt_space_destroy(transaction->space);
	if (transaction->scratch != NULL) {
		env->release(env->context, transaction->scratch, transaction->base->info.node_size);
	}
	if (transaction->original != NULL) {
		env->release(
		    env->context, transaction->original, transaction->base->info.node_size);
	}
	if (transaction->item != NULL) {
		env->release(env->context, transaction->item, transaction->base->info.node_size);
	}
	if (transaction->entry != NULL) {
		env->release(env->context, transaction->entry, transaction->base->info.node_size);
	}
	if (transaction->fs.chunks != NULL) {
		env->release(env->context, transaction->fs.chunks,
		    BT_MAX_CHUNKS * sizeof(*transaction->fs.chunks));
	}
	env->release(env->context, transaction, sizeof(*transaction));
}
