/* SPDX-License-Identifier: BSD-3-Clause */
#include "encode.h"
#include "space.h"
#include <btrfs/write.h>

#define BT_TRANSACTION_NODES 4096U
#define BT_INLINE_WRITE_LIMIT 2048U
#define BT_ACCOUNT_ORIGINAL 1U
#define BT_ACCOUNT_NEW 2U
#define BT_INODE_IMMUTABLE (UINT64_C(1) << 6)
#define BT_INODE_APPEND (UINT64_C(1) << 7)

struct bt_owned_root {
	struct bt_root root;
	struct bt_key key;
	struct bt_disk_root_full item;
	size_t size;
};

struct btrfs_transaction {
	const struct btrfs_fs *base;
	struct btrfs_write_environment io;
	struct bt_mutation *mutation;
	struct bt_space *space;
	struct bt_root roots;
	struct bt_root devices;
	struct bt_owned_root files;
	struct bt_owned_root extents;
	struct bt_disk_super original_super;
	struct bt_disk_super super;
	uint8_t accounted[BT_TRANSACTION_NODES];
	uint8_t *scratch;
	enum btrfs_result failure;
	int changed;
	int finished;
	int writing;
};

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
			if (bt_u64(root->item.legacy.flags) != 0 ||
			    root->item.legacy.drop_level != 0 ||
			    bt_u64(root->item.legacy.drop_progress.objectid) != 0) {
				error = BTRFS_UNSUPPORTED;
			}
		}
	}
	bt_cursor_fini(&cursor);
	return error;
}

enum btrfs_result
btrfs_transaction_begin(const struct btrfs_fs *base,
    const struct btrfs_write_environment *environment, struct btrfs_transaction **result)
{
	struct btrfs_transaction *transaction;
	struct bt_mutation_allocator allocator;
	struct bt_root quota;
	enum btrfs_result error;
	uint64_t unsupported = BT_FEATURE_MIXED_GROUPS | BT_FEATURE_METADATA_UUID;

	if (result == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (base == NULL || environment == NULL || environment->write == NULL ||
	    environment->flush == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (base->info.readonly_features != 0 ||
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
	transaction->roots = base->root_tree;
	transaction->scratch = base->env.allocate(base->env.context, base->info.node_size);
	error = transaction->scratch == NULL
	    ? BTRFS_NO_MEMORY
	    : bt_read_physical(base, BT_SUPER_OFFSET, &transaction->original_super,
		  sizeof(transaction->original_super));
	if (error == BTRFS_OK &&
	    bt_u64(transaction->original_super.generation) != base->info.generation) {
		error = BTRFS_STALE;
	}
	if (error == BTRFS_OK) {
		transaction->super = transaction->original_super;
		error = bt_tx_root(base, BT_EXTENT_TREE, &transaction->extents);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_root(base, BTRFS_TOP_LEVEL_TREE, &transaction->files);
	}
	if (error == BTRFS_OK) {
		error = bt_find_root(base, BT_DEV_TREE, &transaction->devices);
	}
	if (error == BTRFS_OK) {
		error = bt_space_create(
		    base, transaction->extents.root, BT_TRANSACTION_NODES, &transaction->space);
	}
	if (error == BTRFS_OK) {
		bt_space_allocator(transaction->space, &allocator);
		error = bt_mutation_create(base, &allocator, &transaction->mutation);
	}
	if (error != BTRFS_OK) {
		btrfs_transaction_destroy(transaction);
		return error;
	}
	*result = transaction;
	return BTRFS_OK;
}

static enum btrfs_result
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
	size_t length;
	enum btrfs_result error;

	if (transaction == NULL || (bytes == NULL && size != 0) ||
	    modified.nanoseconds >= 1000000000U) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (transaction->failure != BTRFS_OK || transaction->finished) {
		return transaction->failure == BTRFS_OK ? BTRFS_READ_ONLY : transaction->failure;
	}
	if (id.tree != BTRFS_TOP_LEVEL_TREE || size > BT_INLINE_WRITE_LIMIT) {
		return BTRFS_UNSUPPORTED;
	}
	error = bt_mutation_find(transaction->mutation, transaction->files.root, inode_key, &inode,
	    sizeof(inode), &length);
	if (error != BTRFS_OK) {
		return error;
	}
	if (length != sizeof(inode) ||
	    (bt_u32(inode.mode) & BTRFS_MODE_TYPE) != BTRFS_MODE_REGULAR ||
	    (bt_u32(inode.mode) & 06000U) != 0 || bt_u64(inode.sequence) == UINT64_MAX ||
	    (bt_u64(inode.flags) & (BT_INODE_IMMUTABLE | BT_INODE_APPEND)) != 0) {
		return BTRFS_UNSUPPORTED;
	}
	/* Require exactly one existing, ordinary inline extent. This prevents a
	 * partial conversion from leaking external extents or changing snapshots. */
	view = bt_mutation_view(transaction->mutation);
	bt_cursor_init(&cursor, view, transaction->files.root);
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
	error = size == 0
	    ? bt_tx_edit(transaction, &transaction->files.root, extent_key, NULL, 0, BT_DELETE)
	    : bt_tx_edit(transaction, &transaction->files.root, extent_key, extent,
		  sizeof(*extent) + size, BT_REPLACE);
	if (error == BTRFS_OK) {
		bt_put64(&inode.transid, transaction->base->info.generation + 1);
		bt_put64(&inode.size, size);
		bt_put64(&inode.nbytes, size);
		bt_put64(&inode.sequence, bt_u64(inode.sequence) + 1);
		bt_put64(&inode.mtime.seconds, (uint64_t)modified.seconds);
		bt_put32(&inode.mtime.nanoseconds, modified.nanoseconds);
		inode.ctime = inode.mtime;
		/* Set-id inodes were rejected before editing; credential policy belongs
		 * to the owning adapter, which must authorize the operation separately. */
		error = bt_tx_edit(transaction, &transaction->files.root, inode_key, &inode,
		    sizeof(inode), BT_REPLACE);
	}
	if (error == BTRFS_OK) {
		transaction->changed = 1;
		bt_put64(&transaction->files.item.ctransid, transaction->base->info.generation + 1);
		transaction->files.item.ctime = inode.ctime;
	}
	return error;
}

static enum btrfs_result
bt_tx_drop_original(struct btrfs_transaction *transaction, const struct bt_mutated_block *block)
{
	struct {
		struct bt_disk_extent_item extent;
		struct bt_disk_inline_ref reference;
	} item;
	struct bt_key key = { .objectid = block->original_address,
		.type = BT_METADATA_ITEM,
		.offset = block->original_level };
	size_t size;
	enum btrfs_result error;

	error = bt_mutation_find(
	    transaction->mutation, transaction->extents.root, key, &item, sizeof(item), &size);
	if (error != BTRFS_OK) {
		return error;
	}
	/* Explicitly reject shared/full-backreference paths. Removing a single
	 * owner from those requires delayed child/data reference accounting. */
	if (size != sizeof(item) || bt_u64(item.extent.refs) != 1 ||
	    bt_u64(item.extent.flags) != BT_EXTENT_FLAG_TREE ||
	    bt_u64(item.extent.generation) != block->original_generation ||
	    item.reference.type != BT_TREE_BLOCK_REF ||
	    bt_u64(item.reference.offset) != block->owner ||
	    block->original_owner != block->owner) {
		return BTRFS_UNSUPPORTED;
	}
	error = bt_tx_edit(transaction, &transaction->extents.root, key, NULL, 0, BT_DELETE);
	return error == BTRFS_OK
	    ? bt_space_change_used(transaction->space, block->original_address, 0)
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
			error = bt_tx_drop_original(transaction, &block);
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
				error = bt_space_change_used(transaction->space, block.address, 1);
			}
			if (error != BTRFS_OK) {
				return error;
			}
			transaction->accounted[i] |= BT_ACCOUNT_NEW;
		} else if (block.discarded && (transaction->accounted[i] & BT_ACCOUNT_NEW)) {
			error = bt_tx_edit(
			    transaction, &transaction->extents.root, key, NULL, 0, BT_DELETE);
			if (error == BTRFS_OK) {
				error = bt_space_change_used(transaction->space, block.address, 0);
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
	const struct btrfs_fs *base = transaction->base;
	unsigned oldest = 0;
	unsigned i;

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
	bt_put64(&backup->files, transaction->files.root.address);
	bt_put64(&backup->files_generation, transaction->files.root.generation);
	backup->files_level = transaction->files.root.level;
	bt_put64(&backup->chunk, base->chunk_tree.address);
	bt_put64(&backup->chunk_generation, base->chunk_tree.generation);
	backup->chunk_level = base->chunk_tree.level;
	bt_put64(&backup->device, transaction->devices.address);
	bt_put64(&backup->device_generation, transaction->devices.generation);
	backup->device_level = transaction->devices.level;
	bt_put64(&backup->checksum, base->checksum_tree.address);
	bt_put64(&backup->checksum_generation, base->checksum_tree.generation);
	backup->checksum_level = base->checksum_tree.level;
	backup->total_bytes = transaction->super.total_bytes;
	backup->used_bytes = transaction->super.used_bytes;
	bt_put64(&backup->devices, 1);
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

	error = bt_tx_update_root(transaction, &transaction->files);
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
		for (i = 0; i < transaction->base->chunk_count; i++) {
			chunk = &transaction->base->chunks[i];
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
		error = bt_tx_update_root(transaction, &transaction->extents);
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
			/* A v1 free-space cache is advisory and has not been maintained. */
			bt_put64(&transaction->super.cache_generation, UINT64_MAX);
			bt_tx_backup(transaction);
			return bt_mutation_seal(transaction->mutation);
		}
	}
	return BTRFS_NO_SPACE;
}

static enum btrfs_result
bt_tx_persist(struct btrfs_transaction *transaction)
{
	struct bt_mutated_block block;
	struct bt_disk_super *super = &transaction->super;
	struct bt_le32 checksum;
	uint64_t physical;
	uint64_t offset;
	size_t i;
	unsigned mirrors;
	unsigned mirror;
	unsigned level;
	enum btrfs_result error;

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
				error = bt_map(transaction->base, block.address, block.size,
				    BT_BLOCK_METADATA, mirror, &physical, &mirrors);
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
	/* Secondary mirrors first; the primary root is the last publication write.
	 * A failed or torn publication is uncertain and requires a fresh recovery
	 * decision; the immutable reader never silently rolls back to a mirror. */
	for (mirror = 3; mirror != 0; mirror--) {
		offset = mirror == 1 ? BT_SUPER_OFFSET : UINT64_C(16384) << (12 * (mirror - 1));
		if (offset > transaction->base->device_size ||
		    BT_SUPER_SIZE > transaction->base->device_size - offset) {
			continue;
		}
		bt_put64(&super->bytenr, offset);
		bt_zero(super->csum, sizeof(super->csum));
		bt_put32(&checksum,
		    ~bt_crc32c(UINT32_MAX, (const uint8_t *)super + BT_CSUM_SIZE,
			sizeof(*super) - BT_CSUM_SIZE));
		bt_copy(super->csum, &checksum, sizeof(checksum));
		error =
		    transaction->io.write(transaction->io.context, offset, super, sizeof(*super));
		if (error != BTRFS_OK) {
			return error;
		}
	}
	return transaction->io.flush(transaction->io.context);
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
	}
	if (error == BTRFS_OK) {
		transaction->writing = 1;
		error = bt_tx_persist(transaction);
	}
	if (error == BTRFS_OK) {
		error = bt_mutation_accept(transaction->mutation);
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
	bt_mutation_destroy(transaction->mutation);
	bt_space_destroy(transaction->space);
	if (transaction->scratch != NULL) {
		env->release(env->context, transaction->scratch, transaction->base->info.node_size);
	}
	env->release(env->context, transaction, sizeof(*transaction));
}
