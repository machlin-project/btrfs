/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

static int
bt_power_of_two(uint32_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

static enum btrfs_result
bt_super_decode(struct btrfs_fs *fs, const struct bt_disk_super *super, uint64_t offset, int logged)
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
	if ((incompat & ~BT_INCOMPAT_SUPPORTED) != 0 || bt_u64(super->devices) == 0 ||
	    bt_u64(super->devices) > BT_MAX_DEVICES ||
	    (flags & (BT_SUPER_SEEDING | BT_SUPER_METADUMP | BT_SUPER_METADUMP_V2)) != 0) {
		return BTRFS_UNSUPPORTED;
	}
	if ((flags & BT_SUPER_ERROR) != 0) {
		return BTRFS_CORRUPT;
	}
	/* A pending tree log is admitted only by its replay. */
	if (bt_u64(super->log_root) != 0 && !logged) {
		return BTRFS_RECOVERY_REQUIRED;
	}
	if (bt_u64(super->log_root) != 0 && super->log_level >= BT_MAX_LEVEL) {
		return BTRFS_CORRUPT;
	}
	fs->log_root = bt_u64(super->log_root);
	fs->log_level = super->log_level;
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
	    (bt_u64(super->devices) == 1 && fs->info.total_bytes != fs->device_size) ||
	    fs->info.used_bytes > fs->info.total_bytes ||
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

/* Whether another device's superblock belongs to the same committed state
 * of the volume as the one it was opened from: everything but the device's
 * own item, offset and checksum. */
static int
bt_super_sibling(const struct bt_disk_super *super, const struct bt_disk_super *other)
{
	return bt_equal(other->fsid, super->fsid, BTRFS_UUID_SIZE) &&
	    bt_equal(other->metadata_uuid, super->metadata_uuid, BTRFS_UUID_SIZE) &&
	    bt_u64(other->flags) == bt_u64(super->flags) &&
	    bt_u64(other->root) == bt_u64(super->root) &&
	    bt_u64(other->chunk_root) == bt_u64(super->chunk_root) &&
	    bt_u64(other->chunk_generation) == bt_u64(super->chunk_generation) &&
	    bt_u64(other->total_bytes) == bt_u64(super->total_bytes) &&
	    bt_u64(other->devices) == bt_u64(super->devices) &&
	    bt_u32(other->sector_size) == bt_u32(super->sector_size) &&
	    bt_u32(other->node_size) == bt_u32(super->node_size) &&
	    bt_u64(other->incompat) == bt_u64(super->incompat) &&
	    bt_u64(other->compat_ro) == bt_u64(super->compat_ro) &&
	    bt_u16(other->checksum_type) == bt_u16(super->checksum_type) &&
	    other->root_level == super->root_level && other->chunk_level == super->chunk_level &&
	    bt_u32(other->system_array_size) == bt_u32(super->system_array_size) &&
	    bt_equal(other->system_array, super->system_array, BT_SYSTEM_ARRAY_SIZE);
}

/* The volume's other devices, each identified by its superblock: the same
 * fsid and committed state, its own device item. Every device the volume
 * names must be there, as Linux requires without -o degraded; a device whose
 * generation differs missed commits (STALE). */
static enum btrfs_result
bt_mount_devices(struct btrfs_fs *fs, const struct bt_disk_super *super)
{
	const struct btrfs_device *source;
	struct bt_disk_super *other;
	struct bt_device *device;
	uint64_t total = fs->device_size;
	size_t i;
	size_t j;
	enum btrfs_result error = BTRFS_OK;

	if (fs->env.device_count != bt_u64(super->devices) - 1) {
		return fs->env.device_count < bt_u64(super->devices) - 1 ? BTRFS_UNSUPPORTED
									 : BTRFS_INVALID_ARGUMENT;
	}
	fs->devices =
	    fs->env.allocate(fs->env.context, (fs->env.device_count + 1) * sizeof(*fs->devices));
	if (fs->devices == NULL) {
		return BTRFS_NO_MEMORY;
	}
	fs->devices[0] = (struct bt_device){ fs->device_id, fs->device_size, { 0 }, fs->env.context,
		fs->env.read, fs->env.size_bytes };
	bt_copy(fs->devices[0].uuid, fs->device_uuid, BTRFS_UUID_SIZE);
	fs->device_count = 1;
	if (fs->env.device_count == 0) {
		return BTRFS_OK;
	}
	other = fs->env.allocate(fs->env.context, sizeof(*other));
	if (other == NULL) {
		return BTRFS_NO_MEMORY;
	}
	for (i = 0; error == BTRFS_OK && i < fs->env.device_count; i++) {
		source = &fs->env.devices[i];
		if (source->read == NULL || source->size_bytes < BT_SUPER_OFFSET + BT_SUPER_SIZE) {
			error = BTRFS_INVALID_ARGUMENT;
			break;
		}
		error = source->read(source->context, BT_SUPER_OFFSET, other, sizeof(*other));
		if (error == BTRFS_OK) {
			error = bt_super_check(other, BT_SUPER_OFFSET);
		}
		if (error == BTRFS_OK && bt_u64(other->generation) != bt_u64(super->generation)) {
			error = BTRFS_STALE;
		}
		if (error == BTRFS_OK &&
		    (!bt_super_sibling(super, other) || bt_u64(other->device.id) == 0 ||
			!bt_equal(other->device.fsid, fs->metadata_uuid, BTRFS_UUID_SIZE) ||
			bt_u64(other->device.total_bytes) > source->size_bytes ||
			bt_u64(other->device.total_bytes) > UINT64_MAX - total)) {
			error = BTRFS_CORRUPT;
		}
		for (j = 0; error == BTRFS_OK && j < fs->device_count; j++) {
			if (fs->devices[j].id == bt_u64(other->device.id)) {
				error = BTRFS_CORRUPT;
			}
		}
		if (error == BTRFS_OK) {
			device = &fs->devices[fs->device_count++];
			*device = (struct bt_device){ bt_u64(other->device.id),
				bt_u64(other->device.total_bytes), { 0 }, source->context,
				source->read, source->size_bytes };
			bt_copy(device->uuid, other->device.uuid, BTRFS_UUID_SIZE);
			total += device->size;
		}
	}
	fs->env.release(fs->env.context, other, sizeof(*other));
	if (error == BTRFS_OK && total != fs->info.total_bytes) {
		error = BTRFS_CORRUPT;
	}
	return error;
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
    uint64_t offset, uint64_t tree, int logged, struct btrfs_fs **result)
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
	error = bt_super_decode(fs, super, offset, logged);
	if (error == BTRFS_OK) {
		fs->chunks = environment->allocate(
		    environment->context, BT_INITIAL_CHUNKS * sizeof(*fs->chunks));
		fs->chunk_capacity = BT_INITIAL_CHUNKS;
		if (fs->chunks == NULL) {
			error = BTRFS_NO_MEMORY;
		}
	}
	if (error == BTRFS_OK) {
		error = bt_mount_devices(fs, super);
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
		error = bt_mount_super(environment, super, BT_SUPER_OFFSET, tree, 0, result);
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
	if (fs->devices != NULL) {
		env.release(
		    env.context, fs->devices, (env.device_count + 1) * sizeof(*fs->devices));
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

_Static_assert(BTRFS_GROUP_DATA == BT_BLOCK_DATA && BTRFS_GROUP_SYSTEM == BT_BLOCK_SYSTEM &&
	BTRFS_GROUP_METADATA == BT_BLOCK_METADATA && BTRFS_GROUP_DUP == BT_BLOCK_DUP,
    "Linux's block group type flags");

enum btrfs_result
btrfs_get_groups(
    const struct btrfs_fs *fs, struct btrfs_group *groups, size_t capacity, size_t *count)
{
	const struct bt_disk_block_group *item;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	struct bt_key key;
	size_t i;
	enum btrfs_result error;

	if (fs == NULL || count == NULL || (groups == NULL && capacity != 0)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*count = fs->chunk_count;
	error = bt_find_root(fs,
	    (fs->info.readonly_features & BT_COMPAT_RO_BLOCK_GROUP_TREE) != 0 ? BT_BLOCK_GROUP_TREE
									      : BT_EXTENT_TREE,
	    &root);
	for (i = 0; error == BTRFS_OK && i < fs->chunk_count && i < capacity; i++) {
		key = (struct bt_key){ fs->chunks[i].logical, fs->chunks[i].length,
			BT_BLOCK_GROUP_ITEM };
		bt_cursor_init(&cursor, fs, root);
		error = bt_cursor_seek(&cursor, key, 0);
		if (error == BTRFS_OK) {
			(void)bt_cursor_record(&cursor, &record);
			item = (const void *)record.data;
			if (bt_key_compare(record.key, key) != 0 || record.size != sizeof(*item) ||
			    bt_u64(item->used_bytes) > fs->chunks[i].length) {
				error = BTRFS_CORRUPT;
			} else {
				groups[i] = (struct btrfs_group){ fs->chunks[i].logical,
					fs->chunks[i].length, fs->chunks[i].type,
					bt_u64(item->used_bytes) };
			}
		}
		bt_cursor_fini(&cursor);
	}
	if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_CORRUPT;
	}
	return error == BTRFS_OK && fs->chunk_count > capacity ? BTRFS_RANGE : error;
}

enum btrfs_result
btrfs_identify(const struct btrfs_environment *environment, struct btrfs_info *info)
{
	struct bt_disk_super *super;
	enum btrfs_result error;

	if (environment == NULL || environment->read == NULL || environment->allocate == NULL ||
	    environment->release == NULL || info == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	bt_zero(info, sizeof(*info));
	if (environment->size_bytes < BT_SUPER_OFFSET + BT_SUPER_SIZE) {
		return BTRFS_NOT_BTRFS;
	}
	super = environment->allocate(environment->context, sizeof(*super));
	if (super == NULL) {
		return BTRFS_NO_MEMORY;
	}
	error = environment->read(environment->context, BT_SUPER_OFFSET, super, sizeof(*super));
	if (error == BTRFS_OK) {
		error = bt_super_check(super, BT_SUPER_OFFSET);
	}
	if (error == BTRFS_OK) {
		bt_copy(info->uuid, super->fsid, BTRFS_UUID_SIZE);
		bt_copy(info->label, super->label, BTRFS_LABEL_SIZE);
		info->generation = bt_u64(super->generation);
		info->total_bytes = bt_u64(super->total_bytes);
		info->used_bytes = bt_u64(super->used_bytes);
		info->incompat_features = bt_u64(super->incompat);
		info->readonly_features = bt_u64(super->compat_ro);
		info->sector_size = bt_u32(super->sector_size);
		info->node_size = bt_u32(super->node_size);
		info->checksum_type = bt_u16(super->checksum_type);
	}
	environment->release(environment->context, super, sizeof(*super));
	return error;
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
