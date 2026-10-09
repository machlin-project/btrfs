/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

static enum btrfs_result
bt_time_decode(struct bt_disk_time disk, struct btrfs_time *time)
{
	uint64_t seconds = bt_u64(disk.seconds);

	/* Avoid implementation-defined unsigned-to-signed conversion. */
	time->seconds =
	    seconds <= INT64_MAX ? (int64_t)seconds : -1 - (int64_t)(UINT64_MAX - seconds);
	time->nanoseconds = bt_u32(disk.nanoseconds);
	return time->nanoseconds < 1000000000U ? BTRFS_OK : BTRFS_CORRUPT;
}

enum btrfs_result
bt_inode_read(
    struct bt_cursor *cursor, struct btrfs_object_id id, int near, struct btrfs_inode *inode)
{
	const struct btrfs_fs *fs = cursor->fs;
	struct bt_record record;
	struct btrfs_inode decoded;
	const struct bt_disk_inode *disk;
	struct bt_key key = { .objectid = id.inode, .type = BT_INODE_ITEM };
	enum btrfs_result error;

	error = near ? bt_cursor_seek_near(cursor, key) : bt_cursor_seek(cursor, key, 0);
	if (error != BTRFS_OK) {
		return error;
	}
	(void)bt_cursor_record(cursor, &record);
	if (bt_key_compare(record.key, key) != 0) {
		return BTRFS_NOT_FOUND;
	}
	if (record.size != sizeof(*disk)) {
		return BTRFS_CORRUPT;
	}
	disk = (const void *)record.data;
	bt_zero(&decoded, sizeof(decoded));
	decoded.id = id;
	decoded.generation = bt_u64(disk->generation);
	decoded.size = bt_u64(disk->size);
	decoded.allocated_bytes = bt_u64(disk->nbytes);
	decoded.flags = bt_u64(disk->flags);
	decoded.device = bt_u64(disk->device);
	decoded.mode = bt_u32(disk->mode);
	decoded.uid = bt_u32(disk->uid);
	decoded.gid = bt_u32(disk->gid);
	decoded.links = bt_u32(disk->links);
	if (decoded.generation > fs->info.generation || decoded.size > INT64_MAX ||
	    bt_time_decode(disk->atime, &decoded.access_time) != BTRFS_OK ||
	    bt_time_decode(disk->mtime, &decoded.modify_time) != BTRFS_OK ||
	    bt_time_decode(disk->ctime, &decoded.change_time) != BTRFS_OK ||
	    bt_time_decode(disk->otime, &decoded.birth_time) != BTRFS_OK) {
		return BTRFS_CORRUPT;
	}
	*inode = decoded;
	return BTRFS_OK;
}

enum btrfs_result
btrfs_get_inode(const struct btrfs_fs *fs, struct btrfs_object_id id, struct btrfs_inode *inode)
{
	struct bt_root root;
	struct bt_cursor cursor;
	enum btrfs_result error;

	if (fs == NULL || inode == NULL || !bt_file_tree(id.tree) ||
	    (id.inode < BTRFS_ROOT_INODE && id.inode != BTRFS_EMPTY_SUBVOLUME_INODE)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_find_root(fs, id.tree, &root);
	if (error != BTRFS_OK) {
		return error;
	}
	if (id.inode == BTRFS_EMPTY_SUBVOLUME_INODE) {
		/* Linux's new_simple_dir: an empty directory, mode 0755, one link. */
		bt_zero(inode, sizeof(*inode));
		inode->id = id;
		inode->mode = BTRFS_MODE_DIRECTORY | BT_EMPTY_SUBVOLUME_MODE;
		inode->links = 1;
		return BTRFS_OK;
	}
	bt_cursor_init(&cursor, fs, root);
	error = bt_inode_read(&cursor, id, 0, inode);
	bt_cursor_fini(&cursor);
	return error;
}

enum btrfs_result
bt_inode_cursor(
    const struct btrfs_fs *fs, const struct btrfs_inode *inode, struct bt_cursor *cursor)
{
	struct bt_root root;
	enum btrfs_result error;

	if (fs == NULL || inode == NULL || !bt_file_tree(inode->id.tree) ||
	    inode->id.inode < BTRFS_ROOT_INODE) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_find_root(fs, inode->id.tree, &root);
	if (error == BTRFS_OK) {
		bt_cursor_init(cursor, fs, root);
	}
	return error;
}

unsigned
btrfs_inode_fsflags(const struct btrfs_inode *inode)
{
	static const struct {
		uint64_t inode;
		unsigned attribute;
	} map[] = { { BT_INODE_SYNC, BTRFS_FS_SYNC_FL },
		{ BT_INODE_IMMUTABLE, BTRFS_FS_IMMUTABLE_FL },
		{ BT_INODE_APPEND, BTRFS_FS_APPEND_FL }, { BT_INODE_NODUMP, BTRFS_FS_NODUMP_FL },
		{ BT_INODE_NOATIME, BTRFS_FS_NOATIME_FL },
		{ BT_INODE_DIRSYNC, BTRFS_FS_DIRSYNC_FL },
		{ BT_INODE_NODATACOW, BTRFS_FS_NOCOW_FL } };

	unsigned result = 0;
	size_t i;

	if (inode == NULL) {
		return 0;
	}
	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if ((inode->flags & map[i].inode) != 0) {
			result |= map[i].attribute;
		}
	}
	if ((inode->flags & BT_INODE_RO_VERITY) != 0) {
		result |= BTRFS_FS_VERITY_FL;
	}
	if ((inode->flags & BT_INODE_NOCOMPRESS) != 0) {
		result |= BTRFS_FS_NOCOMP_FL;
	} else if ((inode->flags & BT_INODE_COMPRESS) != 0) {
		result |= BTRFS_FS_COMPR_FL;
	}
	return result;
}
