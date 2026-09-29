/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

enum btrfs_result
btrfs_parent(
    const struct btrfs_fs *fs, const struct btrfs_inode *directory, struct btrfs_inode *parent)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key;
	struct btrfs_object_id id = { 0 };
	const struct bt_disk_inode_ref *inode_ref;
	const struct bt_disk_root_ref *root_ref;
	size_t length;
	enum btrfs_result error;

	if (fs == NULL || directory == NULL || parent == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if ((directory->mode & BTRFS_MODE_TYPE) != BTRFS_MODE_DIRECTORY) {
		return BTRFS_NOT_DIRECTORY;
	}
	if (directory->id.inode == BTRFS_ROOT_INODE &&
	    (directory->id.tree == fs->info.default_tree ||
		directory->id.tree == BTRFS_TOP_LEVEL_TREE)) {
		*parent = *directory;
		return BTRFS_OK;
	}
	if (directory->id.inode == BTRFS_ROOT_INODE) {
		bt_cursor_init(&cursor, fs, fs->root_tree);
		key.objectid = directory->id.tree;
		key.type = BT_ROOT_BACKREF;
	} else {
		error = bt_inode_cursor(fs, directory, &cursor);
		if (error != BTRFS_OK) {
			return error;
		}
		key.objectid = directory->id.inode;
		key.type = BT_INODE_REF;
	}
	key.offset = 0;
	error = bt_cursor_seek(&cursor, key, 0);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != key.objectid || record.key.type != key.type) {
			error = BTRFS_CORRUPT;
		} else if (key.type == BT_ROOT_BACKREF) {
			root_ref = (const void *)record.data;
			if (record.size < sizeof(*root_ref)) {
				error = BTRFS_CORRUPT;
			} else {
				length = bt_u16(root_ref->name_length);
				if (length != record.size - sizeof(*root_ref) ||
				    !bt_name_valid(root_ref + 1, length) ||
				    !bt_file_tree(record.key.offset)) {
					error = BTRFS_CORRUPT;
				}
				id.tree = record.key.offset;
				id.inode = bt_u64(root_ref->directory);
			}
		} else {
			inode_ref = (const void *)record.data;
			if (record.size < sizeof(*inode_ref)) {
				error = BTRFS_CORRUPT;
			} else {
				length = bt_u16(inode_ref->name_length);
				if (length != record.size - sizeof(*inode_ref) ||
				    !bt_name_valid(inode_ref + 1, length)) {
					error = BTRFS_CORRUPT;
				}
				id.tree = directory->id.tree;
				id.inode = record.key.offset;
			}
		}
		if (error == BTRFS_OK) {
			error = bt_cursor_next(&cursor);
			if (error == BTRFS_NOT_FOUND) {
				error = BTRFS_OK;
			} else if (error == BTRFS_OK) {
				(void)bt_cursor_record(&cursor, &record);
				if (record.key.objectid == key.objectid &&
				    record.key.type == key.type) {
					error = BTRFS_CORRUPT;
				}
			}
		}
	}
	bt_cursor_fini(&cursor);
	if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		if (id.inode < BTRFS_ROOT_INODE ||
		    (id.tree == directory->id.tree && id.inode == directory->id.inode)) {
			return BTRFS_CORRUPT;
		}
		error = btrfs_get_inode(fs, id, parent);
		if (error == BTRFS_OK && (parent->mode & BTRFS_MODE_TYPE) != BTRFS_MODE_DIRECTORY) {
			error = BTRFS_CORRUPT;
		}
	}
	return error;
}
