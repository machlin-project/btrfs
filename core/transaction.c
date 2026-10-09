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
	const struct bt_disk_super *copy;
	uint64_t offset;
	unsigned mirror;
	int logged = transaction->base->log_root != 0;
	enum btrfs_result error;

	transaction->copies = 0;
	if (logged &&
	    (bt_u64(transaction->original_super.log_root) != transaction->base->log_root ||
		transaction->original_super.log_level != transaction->base->log_level)) {
		return BTRFS_STALE;
	}
	for (mirror = 0; mirror < BT_SUPER_MIRRORS; mirror++) {
		if (!bt_super_present(transaction->base->device_size, mirror)) {
			continue;
		}
		offset = bt_super_offset(mirror);
		/* Callers have just read the primary into original_super or compared
		 * it byte for byte with it. */
		copy = &transaction->original_super;
		if (mirror != 0) {
			copy = (const void *)transaction->scratch;
			error = bt_read_physical(
			    transaction->base, offset, transaction->scratch, BT_SUPER_SIZE);
			if (error != BTRFS_OK) {
				return error;
			}
		}
		if (bt_super_check(copy, offset) != BTRFS_OK ||
		    !(mirror != 0 && logged
			    ? bt_super_same_but_log(copy, &transaction->original_super)
			    : bt_super_same(copy, &transaction->original_super))) {
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
	uint64_t free_space = BT_COMPAT_RO_FREE_SPACE_TREE | BT_COMPAT_RO_FREE_SPACE_TREE_VALID;
	uint64_t maintained = free_space | BT_COMPAT_RO_BLOCK_GROUP_TREE;
	uint64_t readonly;

	if (result == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (base == NULL || environment == NULL || environment->write == NULL ||
	    environment->flush == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	/* The free-space and block-group trees are maintained; other read-only
	 * features are not. Linux keeps a block-group tree only beside a valid
	 * free-space tree and NO_HOLES. */
	readonly = base->info.readonly_features;
	if ((readonly & ~maintained) != 0 ||
	    ((readonly & free_space) != 0 && (readonly & free_space) != free_space) ||
	    ((readonly & BT_COMPAT_RO_BLOCK_GROUP_TREE) != 0 &&
		((readonly & free_space) != free_space ||
		    !(base->info.incompat_features & BT_FEATURE_NO_HOLES))) ||
	    ((base->info.incompat_features & BT_FEATURE_MIXED_GROUPS) != 0 &&
		base->info.node_size != base->info.sector_size) ||
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
	/* Room for the chunks this transaction may grow. */
	transaction->fs.chunk_capacity = base->chunk_count + BT_TRANSACTION_CHUNKS;
	transaction->fs.chunks = base->env.allocate(
	    base->env.context, transaction->fs.chunk_capacity * sizeof(*base->chunks));
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
	transaction->has_group_tree = (readonly & BT_COMPAT_RO_BLOCK_GROUP_TREE) != 0;
	if (error == BTRFS_OK && transaction->has_group_tree) {
		error = bt_tx_root(base, BT_BLOCK_GROUP_TREE, &transaction->groups);
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
		error = bt_space_from_map(&transaction->fs, map, transaction->extents.root,
		    transaction->has_free_space ? &transaction->free_space.root : NULL,
		    BT_TRANSACTION_NODES, &transaction->space);
		if (error == BTRFS_OK) {
			transaction->chunks_published =
			    bt_space_original_chunks(transaction->space);
		}
	} else if (error == BTRFS_OK) {
		/* A free-space tree that disagrees with the extent tree is not
		 * propagated. */
		error = bt_space_create(&transaction->fs, transaction->extents.root,
		    transaction->has_free_space ? &transaction->free_space.root : NULL,
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
bt_tx_release(struct btrfs_transaction *transaction, const struct bt_mutated_block *block,
    const struct bt_root *replacement, int *replaced)
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
	error = bt_backref_release_tree(transaction->mutation, &transaction->extents.root, extent,
	    block->original_owner == block->owner ? block->owner : 0, replacement, &refs, &flags,
	    &freed, replaced);
	if (error != BTRFS_OK) {
		return error;
	}
	if (freed) {
		return bt_space_change_used(transaction->space, block->original_address,
		    transaction->base->info.node_size, 0);
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
	struct bt_root replacement;
	size_t i;
	int replaced;
	enum btrfs_result error;

	for (i = 0; i < bt_mutation_count(transaction->mutation); i++) {
		error = bt_mutation_block(transaction->mutation, i, &block);
		if (error != BTRFS_OK) {
			return error;
		}
		if (block.original_address != 0 &&
		    !(transaction->accounted[i] & BT_ACCOUNT_ORIGINAL)) {
			replacement = (struct bt_root){ block.address,
				transaction->base->info.generation + 1, block.owner, block.level };
			error = bt_tx_release(transaction, &block,
			    !block.discarded && !(transaction->accounted[i] & BT_ACCOUNT_NEW)
				? &replacement
				: NULL,
			    &replaced);
			if (error == BTRFS_OK && replaced) {
				error = bt_space_change_used(transaction->space, block.address,
				    transaction->base->info.node_size, 1);
				transaction->accounted[i] |= BT_ACCOUNT_NEW;
			}
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
			error = bt_tx_edit(transaction, bt_tx_groups(transaction), key, &group,
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

/* The tree holding block-group items: their own tree, or the extent tree. */
struct bt_root *
bt_tx_groups(struct btrfs_transaction *transaction)
{
	return transaction->has_group_tree ? &transaction->groups.root : &transaction->extents.root;
}

/* Apply a snapshot of this round's log. Edits can allocate nodes and grow the
 * log, so no pointer into it survives an edit. The original ordered log is
 * retained for the allocation map; only the copied batch is normalized. */
static enum btrfs_result
bt_tx_free_space(struct btrfs_transaction *transaction)
{
	const struct btrfs_environment *env = &transaction->base->env;
	struct bt_space_change *changes;
	size_t end;
	size_t total;
	size_t count;
	size_t first;
	size_t next;
	enum btrfs_result error = BTRFS_OK;

	if (!transaction->has_free_space) {
		return BTRFS_OK;
	}
	/* Each pass consumes at least one entry of the bounded allocation log. */
	while (error == BTRFS_OK &&
	    transaction->free_space_applied < bt_space_change_count(transaction->space)) {
		end = bt_space_change_count(transaction->space);
		total = end - transaction->free_space_applied;
		changes = env->allocate(env->context, total * sizeof(*changes));
		if (changes == NULL) {
			return BTRFS_NO_MEMORY;
		}
		bt_copy(changes,
		    bt_space_change(transaction->space, transaction->free_space_applied),
		    total * sizeof(*changes));
		count = total;
		error = bt_space_coalesce(changes, &count);
		for (first = 0; error == BTRFS_OK && first < count; first = next) {
			for (next = first + 1;
			    next < count && changes[next].chunk == changes[first].chunk; next++) {
			}
			if (changes[first].chunk >= transaction->chunks_published) {
				error = bt_tx_publish_chunks(transaction);
				if (error != BTRFS_OK) {
					break;
				}
			}
			error = bt_fst_changes(transaction->mutation, &transaction->free_space.root,
			    &transaction->fs.chunks[changes[first].chunk], changes + first,
			    next - first);
		}
		env->release(env->context, changes, total * sizeof(*changes));
		transaction->free_space_applied = end;
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
			error = bt_tx_edit(transaction, bt_tx_groups(transaction), key, &item,
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
		if (error == BTRFS_OK && transaction->has_group_tree &&
		    transaction->groups.root.address !=
			bt_u64(transaction->groups.item.legacy.bytenr)) {
			error = bt_tx_update_root(transaction, &transaction->groups);
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
			/* A v1 free-space cache, which this writer does not maintain, is
			 * invalid once its generation differs from the superblock's:
			 * Linux then clears and rebuilds it. Zero means no v1 cache is
			 * in use (a free-space tree or none) and stays. */
			if (bt_u64(transaction->super.cache_generation) != 0) {
				bt_put64(&transaction->super.cache_generation, UINT64_MAX);
			}
			/* A commit names no tree log: a pending one was replayed. */
			bt_put64(&transaction->super.log_root, 0);
			transaction->super.log_level = 0;
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

/* Heap order by physical address: bounded and without allocation. */
static void
bt_tx_sift(struct bt_tx_write *writes, size_t root, size_t count)
{
	struct bt_tx_write swap;
	size_t child;

	for (;;) {
		child = 2 * root + 1;
		if (child >= count) {
			return;
		}
		if (child + 1 < count && writes[child + 1].physical > writes[child].physical) {
			child++;
		}
		if (writes[root].physical >= writes[child].physical) {
			return;
		}
		swap = writes[root];
		writes[root] = writes[child];
		writes[child] = swap;
		root = child;
	}
}

static void
bt_tx_sort(struct bt_tx_write *writes, size_t count)
{
	struct bt_tx_write swap;
	size_t i;

	for (i = count / 2; i != 0; i--) {
		bt_tx_sift(writes, i - 1, count);
	}
	for (i = count; i > 1; i--) {
		swap = writes[0];
		writes[0] = writes[i - 1];
		writes[i - 1] = swap;
		bt_tx_sift(writes, 0, i - 1);
	}
}

/* Lists every copy of every written node by physical address. Copies never
 * overlap: chunk stripes were checked to be disjoint when space was loaded. */
static enum btrfs_result
bt_tx_writes(struct btrfs_transaction *transaction, struct bt_tx_write *writes, size_t capacity,
    size_t *count)
{
	struct bt_mutated_block block;
	uint64_t physical;
	size_t i;
	unsigned mirrors;
	unsigned mirror;
	enum btrfs_result error;

	*count = 0;
	for (i = 0; i < bt_mutation_count(transaction->mutation); i++) {
		error = bt_mutation_block(transaction->mutation, i, &block);
		if (error != BTRFS_OK) {
			return error;
		}
		if (block.discarded) {
			continue;
		}
		mirrors = 1;
		for (mirror = 0; mirror < mirrors; mirror++) {
			error = bt_map(&transaction->fs, block.address, block.size,
			    block.owner == BT_CHUNK_TREE ? BT_BLOCK_SYSTEM : BT_BLOCK_METADATA,
			    mirror, &physical, &mirrors);
			if (error != BTRFS_OK) {
				return error;
			}
			if (*count == capacity) {
				return BTRFS_CORRUPT;
			}
			writes[(*count)++] = (struct bt_tx_write){ physical, block.bytes };
		}
	}
	bt_tx_sort(writes, *count);
	for (i = 1; i < *count; i++) {
		if (writes[i].physical - writes[i - 1].physical < transaction->fs.info.node_size) {
			return BTRFS_CORRUPT;
		}
	}
	return BTRFS_OK;
}

/* Number of nodes in the run of physically contiguous copies at first. */
static size_t
bt_tx_run(const struct btrfs_transaction *transaction, const struct bt_tx_write *writes,
    size_t count, size_t first)
{
	size_t node_size = transaction->fs.info.node_size;
	size_t limit = BT_WRITE_RUN / node_size;
	size_t end = first + 1;

	while (end < count && end - first < limit &&
	    writes[end].physical == writes[end - 1].physical + node_size) {
		end++;
	}
	return end - first;
}

/* Writes every node copy, contiguous copies merged into single writes. All
 * memory is allocated before the first write. */
static enum btrfs_result
bt_tx_write_nodes(struct btrfs_transaction *transaction)
{
	const struct btrfs_environment *env = &transaction->base->env;
	struct bt_tx_write *writes;
	uint8_t *staging = NULL;
	size_t node_size = transaction->fs.info.node_size;
	size_t capacity = BT_MAX_MIRRORS * bt_mutation_count(transaction->mutation);
	size_t count;
	size_t longest = 1;
	size_t length;
	size_t i;
	size_t j;
	enum btrfs_result error;

	if (capacity == 0) {
		return BTRFS_OK;
	}
	writes = env->allocate(env->context, capacity * sizeof(*writes));
	if (writes == NULL) {
		return BTRFS_NO_MEMORY;
	}
	error = bt_tx_writes(transaction, writes, capacity, &count);
	for (i = 0; error == BTRFS_OK && i < count; i += length) {
		length = bt_tx_run(transaction, writes, count, i);
		longest = length > longest ? length : longest;
	}
	if (error == BTRFS_OK && longest > 1) {
		staging = env->allocate(env->context, longest * node_size);
		error = staging == NULL ? BTRFS_NO_MEMORY : BTRFS_OK;
	}
	for (i = 0; error == BTRFS_OK && i < count; i += length) {
		length = bt_tx_run(transaction, writes, count, i);
		if (length == 1) {
			error = transaction->io.write(transaction->io.context, writes[i].physical,
			    writes[i].bytes, node_size);
			continue;
		}
		for (j = 0; j < length; j++) {
			bt_copy(staging + j * node_size, writes[i + j].bytes, node_size);
		}
		error = transaction->io.write(
		    transaction->io.context, writes[i].physical, staging, length * node_size);
	}
	if (staging != NULL) {
		env->release(env->context, staging, longest * node_size);
	}
	env->release(env->context, writes, capacity * sizeof(*writes));
	return error;
}

static enum btrfs_result
bt_tx_persist(struct btrfs_transaction *transaction)
{
	enum btrfs_result error;
	unsigned mirror;

	/* New data was written when its extents were created; it and the
	 * metadata naming it are durable at the first barrier. */
	error = bt_tx_write_nodes(transaction);
	if (error != BTRFS_OK) {
		return error;
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

/* The prepared root set and chunk map already describe the next generation.
 * Validate its selected root through the sealed private nodes, then detach
 * every private hook only after publication succeeds. No on-disk read can
 * substitute a different generation for the one this commit acknowledged. */
static enum btrfs_result
bt_tx_committed_view(struct btrfs_transaction *transaction, uint64_t tree, struct btrfs_fs **result)
{
	const struct btrfs_environment *env = &transaction->base->env;
	struct btrfs_fs *fs;
	struct btrfs_object_id id;
	size_t i;
	enum btrfs_result error;

	fs = env->allocate(env->context, sizeof(*fs));
	if (fs == NULL) {
		return BTRFS_NO_MEMORY;
	}
	*fs = *bt_mutation_view(transaction->mutation);
	fs->env = *env;
	fs->root_tree = transaction->roots;
	fs->chunk_tree = transaction->chunks;
	fs->checksum_tree = transaction->checksums.root;
	/* The base's selected-root shortcut names the previous generation. */
	bt_zero(&fs->selected_tree, sizeof(fs->selected_tree));
	fs->private_root = NULL;
	fs->private_root_context = NULL;
	fs->info.used_bytes = bt_u64(transaction->super.used_bytes);
	fs->info.incompat_features = bt_u64(transaction->super.incompat);
	fs->info.readonly_features = bt_u64(transaction->super.compat_ro);
	fs->chunk_count = 0;
	fs->chunk_capacity = transaction->fs.chunk_count;
	fs->chunks = env->allocate(env->context, fs->chunk_capacity * sizeof(*fs->chunks));
	if (fs->chunks == NULL) {
		btrfs_unmount(fs);
		return BTRFS_NO_MEMORY;
	}
	/* Growth appends in logical order; retired groups leave no mapping in the
	 * next view, just as when the mount reads the committed chunk tree. */
	for (i = 0; i < transaction->fs.chunk_count; i++) {
		if (!transaction->fs.chunks[i].removed) {
			fs->chunks[fs->chunk_count++] = transaction->fs.chunks[i];
		}
	}
	error = tree == 0 ? bt_default_tree(fs, &tree) : BTRFS_OK;
	if (error == BTRFS_OK) {
		error = bt_find_root(fs, tree, &fs->selected_tree);
	}
	if (error == BTRFS_OK) {
		fs->info.default_tree = tree;
		id = (struct btrfs_object_id){ tree, BTRFS_ROOT_INODE };
		error = btrfs_get_inode(fs, id, &fs->root_inode);
		if (error == BTRFS_OK &&
		    (fs->root_inode.mode & BTRFS_MODE_TYPE) != BTRFS_MODE_DIRECTORY) {
			error = BTRFS_CORRUPT;
		}
	}
	if (error != BTRFS_OK) {
		btrfs_unmount(fs);
		return error;
	}
	*result = fs;
	return BTRFS_OK;
}

static enum btrfs_result
bt_tx_commit(struct btrfs_transaction *transaction, uint64_t tree, struct btrfs_fs **view)
{
	struct btrfs_fs *prepared = NULL;
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
	if (error == BTRFS_OK && view != NULL) {
		error = bt_tx_committed_view(transaction, tree, &prepared);
	}
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
	if (error == BTRFS_OK && prepared != NULL) {
		/* The view owns its chunks and uses the base device/allocator. Private
		 * nodes become ordinary durable nodes, cached when the editor retires. */
		prepared->cache_limit = UINT64_MAX;
		prepared->private_node = NULL;
		prepared->private_context = NULL;
		prepared->private_borrow = 0;
		prepared->private_holders = NULL;
		prepared->private_root = NULL;
		prepared->private_root_context = NULL;
		*view = prepared;
	} else {
		btrfs_unmount(prepared);
	}
	transaction->failure = error;
	return error;
}

enum btrfs_result
btrfs_transaction_commit(struct btrfs_transaction *transaction)
{
	return bt_tx_commit(transaction, 0, NULL);
}

enum btrfs_result
btrfs_transaction_commit_view(
    struct btrfs_transaction *transaction, uint64_t tree, struct btrfs_fs **view)
{
	if (view == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*view = NULL;
	if (tree != 0 && !bt_file_tree(tree)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	return bt_tx_commit(transaction, tree, view);
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
		    transaction->fs.chunk_capacity * sizeof(*transaction->fs.chunks));
	}
	env->release(env->context, transaction, sizeof(*transaction));
}

enum btrfs_result
btrfs_transaction_failure(const struct btrfs_transaction *transaction)
{
	if (transaction == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (transaction->failure != BTRFS_OK) {
		return transaction->failure;
	}
	return transaction->finished ? BTRFS_READ_ONLY : BTRFS_OK;
}

static int
bt_tx_private_root(void *context, uint64_t tree, struct bt_root *root, enum btrfs_result *result)
{
	struct btrfs_transaction *transaction = context;
	size_t i;

	for (i = 0; i < transaction->tree_count; i++) {
		if (transaction->trees[i].root.owner == tree) {
			*root = transaction->trees[i].root;
			/* As bt_find_root reports a subvolume deleted in this view. */
			*result = bt_u32(transaction->trees[i].item.legacy.refs) == 0
			    ? BTRFS_NOT_FOUND
			    : BTRFS_OK;
			return 1;
		}
	}
	return 0;
}

const struct btrfs_fs *
btrfs_transaction_reader(struct btrfs_transaction *transaction)
{
	struct btrfs_fs *reader;
	struct bt_root root;
	enum btrfs_result result;

	if (btrfs_transaction_failure(transaction) != BTRFS_OK) {
		return NULL;
	}
	reader = &transaction->reader;
	/* The private view's nodes and chunk map, with the base environment:
	 * readers do not use the transaction's buffers. */
	*reader = *bt_mutation_view(transaction->mutation);
	reader->env = transaction->base->env;
	reader->private_borrow = 1;
	reader->private_root = bt_tx_private_root;
	reader->private_root_context = transaction;
	/* Chunks grown for data since the mutation last allocated a node. */
	reader->chunks = transaction->fs.chunks;
	reader->chunk_count = transaction->fs.chunk_count;
	reader->root_tree = transaction->roots;
	reader->checksum_tree = transaction->checksums.root;
	if (bt_tx_private_root(transaction, reader->selected_tree.owner, &root, &result)) {
		if (result != BTRFS_OK) {
			return NULL;
		}
		reader->selected_tree = root;
	}
	if (btrfs_get_inode(reader, reader->root_inode.id, &reader->root_inode) != BTRFS_OK) {
		return NULL;
	}
	return reader;
}

/* Tree nodes the commit itself may change beyond twice the transaction's
 * own: root items, block groups and free-space paths. */
#define BT_COMMIT_FIXED_NODES 64U
/* File trees and directory indexes one operation may add (a rename between
 * directories, a snapshot). */
#define BT_OPERATION_TREES 2U
#define BT_OPERATION_INDEXES 2U

/* Metadata that only operations releasing space may take, as Linux's global
 * block reserve: one releasing step, which may be the drop of a largest data
 * extent, and its commit, so that a full volume can always delete. */
static uint64_t
bt_tx_release_reserve(const struct btrfs_transaction *transaction)
{
	struct bt_key largest = { .type = BT_EXTENT_ITEM, .offset = BT_DATA_EXTENT };
	size_t step = bt_tx_ref_nodes(transaction, largest, 0) + 1;

	if (step < BTRFS_RELEASE_STEP_NODES) {
		step = BTRFS_RELEASE_STEP_NODES;
	}
	return 2 * (uint64_t)step + BT_COMMIT_FIXED_NODES;
}

/* The metadata nodes the transaction must still be able to obtain after an
 * operation of nodes: twice its work and the operation's, the commit's own,
 * and the reserve unless the operation releases space. */
static uint64_t
bt_tx_metadata_need(const struct btrfs_transaction *transaction, size_t nodes, int releasing)
{
	return 2 * ((uint64_t)bt_tx_work(transaction) + nodes) + BT_COMMIT_FIXED_NODES +
	    (releasing ? 0 : bt_tx_release_reserve(transaction));
}

static enum btrfs_result
bt_tx_room(struct btrfs_transaction *transaction, size_t nodes, int releasing)
{
	size_t work;

	if (btrfs_transaction_failure(transaction) != BTRFS_OK) {
		return btrfs_transaction_failure(transaction);
	}
	work = bt_tx_work(transaction);
	if (nodes > BT_TRANSACTION_NODES / 2 || work > BT_TRANSACTION_NODES / 2 - nodes ||
	    transaction->tree_count + BT_OPERATION_TREES > BT_TRANSACTION_TREES / 2 ||
	    transaction->index_count + BT_OPERATION_INDEXES > BT_TRANSACTION_INDEXES / 2 ||
	    transaction->privileged_count + 1 > BT_TRANSACTION_PRIVILEGED / 2 ||
	    transaction->ref_count > BT_TRANSACTION_REFERENCES / 2 ||
	    transaction->deferred_count + 1 > BT_TRANSACTION_DEFERRED / 2 ||
	    !bt_space_metadata_available(
		transaction->space, bt_tx_metadata_need(transaction, nodes, releasing))) {
		return BTRFS_NO_SPACE;
	}
	transaction->operation_nodes = nodes;
	bt_space_hold_metadata(
	    transaction->space, bt_tx_metadata_need(transaction, nodes, releasing));
	return BTRFS_OK;
}

enum btrfs_result
btrfs_transaction_room(struct btrfs_transaction *transaction, size_t nodes)
{
	return bt_tx_room(transaction, nodes, 0);
}

enum btrfs_result
btrfs_transaction_room_releasing(struct btrfs_transaction *transaction, size_t nodes)
{
	return bt_tx_room(transaction, nodes, 1);
}

void
bt_tx_hold_metadata(struct btrfs_transaction *transaction)
{
	bt_space_hold_metadata(
	    transaction->space, bt_tx_metadata_need(transaction, transaction->operation_nodes, 0));
}
