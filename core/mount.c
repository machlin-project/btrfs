/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

static int
bt_power_of_two(uint32_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

static enum btrfs_result
bt_super_decode(struct btrfs_fs *fs, const struct bt_disk_super *super, uint64_t offset)
{
	uint64_t flags;
	uint64_t incompat;
	enum btrfs_result error;

	error = bt_super_check(super, offset);
	if (error != BTRFS_OK) {
		return error;
	}
	flags = bt_u64(super->flags);
	incompat = bt_u64(super->incompat);
	if ((incompat & ~BT_INCOMPAT_SUPPORTED) != 0 || bt_u64(super->devices) != 1 ||
	    (flags & (BT_SUPER_SEEDING | BT_SUPER_METADUMP | BT_SUPER_METADUMP_V2)) != 0) {
		return BTRFS_UNSUPPORTED;
	}
	if ((flags & BT_SUPER_ERROR) != 0) {
		return BTRFS_CORRUPT;
	}
	if (bt_u64(super->log_root) != 0) {
		return BTRFS_RECOVERY_REQUIRED;
	}
	fs->info.generation = bt_u64(super->generation);
	fs->info.total_bytes = bt_u64(super->total_bytes);
	fs->info.used_bytes = bt_u64(super->used_bytes);
	fs->info.sector_size = bt_u32(super->sector_size);
	fs->info.node_size = bt_u32(super->node_size);
	fs->info.incompat_features = incompat;
	fs->info.readonly_features = bt_u64(super->compat_ro);
	fs->device_id = bt_u64(super->device.id);
	fs->device_size = bt_u64(super->device.total_bytes);
	if (!bt_power_of_two(fs->info.sector_size) || fs->info.sector_size < 4096 ||
	    fs->info.sector_size > BT_MAX_NODE_SIZE || !bt_power_of_two(fs->info.node_size) ||
	    fs->info.node_size < fs->info.sector_size || fs->info.node_size > BT_MAX_NODE_SIZE ||
	    fs->info.generation == 0 || fs->device_id == 0 ||
	    fs->device_size > fs->env.size_bytes ||
	    fs->device_size < BT_SUPER_OFFSET + BT_SUPER_SIZE ||
	    fs->info.total_bytes != fs->device_size || fs->info.used_bytes > fs->info.total_bytes ||
	    bt_u64(super->device.used_bytes) > fs->device_size ||
	    super->root_level >= BT_MAX_LEVEL || super->chunk_level >= BT_MAX_LEVEL ||
	    bt_u32(super->system_array_size) > BT_SYSTEM_ARRAY_SIZE ||
	    bt_u32(super->system_array_size) == 0) {
		return BTRFS_CORRUPT;
	}
	fs->info.checksum_type = bt_u16(super->checksum_type);
	bt_crc_shift_init(&fs->node_crc, fs->info.node_size - BT_CSUM_SIZE);
	bt_copy(fs->info.uuid, super->fsid, BTRFS_UUID_SIZE);
	bt_copy(fs->info.label, super->label, BTRFS_LABEL_SIZE);
	bt_copy(fs->device_uuid, super->device.uuid, BTRFS_UUID_SIZE);
	bt_copy(fs->metadata_uuid,
	    (incompat & BT_FEATURE_METADATA_UUID) != 0 ? super->metadata_uuid : super->fsid,
	    BTRFS_UUID_SIZE);
	if (!bt_equal(super->device.fsid, fs->metadata_uuid, BTRFS_UUID_SIZE)) {
		return BTRFS_CORRUPT;
	}
	fs->root_tree.address = bt_u64(super->root);
	fs->root_tree.generation = fs->info.generation;
	fs->root_tree.owner = BT_ROOT_TREE;
	fs->root_tree.level = super->root_level;
	fs->chunk_tree.address = bt_u64(super->chunk_root);
	fs->chunk_tree.generation = bt_u64(super->chunk_generation);
	fs->chunk_tree.owner = BT_CHUNK_TREE;
	fs->chunk_tree.level = super->chunk_level;
	return BTRFS_OK;
}

static enum btrfs_result
bt_bootstrap_chunks(struct btrfs_fs *fs, const struct bt_disk_super *super)
{
	const struct bt_disk_key *key;
	const struct bt_disk_chunk *chunk;
	size_t remaining = bt_u32(super->system_array_size);
	size_t offset = 0;
	size_t length;
	enum btrfs_result error;

	while (remaining != 0) {
		if (remaining < sizeof(*key) + sizeof(*chunk)) {
			return BTRFS_CORRUPT;
		}
		key = (const void *)(super->system_array + offset);
		chunk = (const void *)(key + 1);
		length = sizeof(*chunk) + bt_u16(chunk->stripes) * sizeof(struct bt_disk_stripe);
		if (length > remaining - sizeof(*key)) {
			return BTRFS_CORRUPT;
		}
		error = bt_chunk_add(fs, bt_key_decode(key), chunk, length, 1);
		if (error != BTRFS_OK) {
			return error;
		}
		remaining -= sizeof(*key) + length;
		offset += sizeof(*key) + length;
	}
	return BTRFS_OK;
}

static enum btrfs_result
bt_load_chunks(struct btrfs_fs *fs)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key first = { .objectid = BT_FIRST_CHUNK_OBJECTID, .type = BT_CHUNK_ITEM };
	enum btrfs_result error;
	size_t i;

	bt_cursor_init(&cursor, fs, fs->chunk_tree);
	error = bt_cursor_seek(&cursor, first, 0);
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != BT_FIRST_CHUNK_OBJECTID ||
		    record.key.type != BT_CHUNK_ITEM) {
			error = BTRFS_CORRUPT;
			break;
		}
		error = bt_chunk_add(fs, record.key, record.data, record.size, 0);
		if (error != BTRFS_OK) {
			break;
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (error != BTRFS_NOT_FOUND) {
		return error;
	}
	for (i = 0; i < fs->chunk_count; i++) {
		if (!fs->chunks[i].confirmed) {
			return BTRFS_CORRUPT;
		}
	}
	return BTRFS_OK;
}

enum btrfs_result
bt_mount_super(const struct btrfs_environment *environment, const struct bt_disk_super *super,
    uint64_t offset, uint64_t tree, struct btrfs_fs **result)
{
	struct btrfs_fs *fs;
	struct btrfs_object_id id;
	enum btrfs_result error;

	*result = NULL;
	fs = environment->allocate(environment->context, sizeof(*fs));
	if (fs == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(fs, sizeof(*fs));
	fs->env = *environment;
	fs->cache_limit = UINT64_MAX;
	error = bt_super_decode(fs, super, offset);
	if (error == BTRFS_OK) {
		fs->chunks = environment->allocate(
		    environment->context, BT_INITIAL_CHUNKS * sizeof(*fs->chunks));
		fs->chunk_capacity = BT_INITIAL_CHUNKS;
		if (fs->chunks == NULL) {
			error = BTRFS_NO_MEMORY;
		}
	}
	if (error == BTRFS_OK) {
		error = bt_bootstrap_chunks(fs, super);
	}
	if (error == BTRFS_OK) {
		error = bt_load_chunks(fs);
	}
	if (error == BTRFS_OK) {
		error = bt_find_root(fs, BT_CSUM_TREE, &fs->checksum_tree);
	}
	if (error == BTRFS_OK && tree == 0) {
		error = bt_default_tree(fs, &tree);
	}
	if (error == BTRFS_OK) {
		error = bt_find_root(fs, tree, &fs->selected_tree);
	}
	if (error == BTRFS_OK) {
		fs->info.default_tree = tree;
		id.tree = tree;
		id.inode = BTRFS_ROOT_INODE;
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

/* Mount admits only the primary copy. Selecting a mirror is an explicit recovery
 * decision because an older copy may silently roll back acknowledged data. */
enum btrfs_result
btrfs_mount(const struct btrfs_environment *environment, uint64_t tree, struct btrfs_fs **result)
{
	struct bt_disk_super *super;
	enum btrfs_result error;

	if (result == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (environment == NULL || environment->read == NULL || environment->allocate == NULL ||
	    environment->release == NULL || (tree != 0 && !bt_file_tree(tree))) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (environment->size_bytes < BT_SUPER_OFFSET + BT_SUPER_SIZE) {
		return BTRFS_NOT_BTRFS;
	}
	super = environment->allocate(environment->context, sizeof(*super));
	if (super == NULL) {
		return BTRFS_NO_MEMORY;
	}
	error = environment->read(environment->context, BT_SUPER_OFFSET, super, sizeof(*super));
	if (error == BTRFS_OK) {
		error = bt_mount_super(environment, super, BT_SUPER_OFFSET, tree, result);
	}
	environment->release(environment->context, super, sizeof(*super));
	return error;
}

void
btrfs_unmount(struct btrfs_fs *fs)
{
	struct btrfs_environment env;

	if (fs == NULL) {
		return;
	}
	env = fs->env;
	if (fs->chunks != NULL) {
		env.release(env.context, fs->chunks, fs->chunk_capacity * sizeof(*fs->chunks));
	}
	env.release(env.context, fs, sizeof(*fs));
}

void
btrfs_get_info(const struct btrfs_fs *fs, struct btrfs_info *info)
{
	if (fs != NULL && info != NULL) {
		*info = fs->info;
	}
}

enum btrfs_result
btrfs_root(const struct btrfs_fs *fs, struct btrfs_inode *inode)
{
	if (fs == NULL || inode == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*inode = fs->root_inode;
	return BTRFS_OK;
}
