/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

static enum btrfs_result
bt_xattr_scan(const struct btrfs_fs *fs, const struct btrfs_inode *inode, const void *wanted,
    size_t wanted_length, void *buffer, size_t capacity, size_t *length)
{
	struct bt_cursor cursor;
	struct bt_key key;
	struct bt_record record;
	const struct bt_disk_dir *header;
	const uint8_t *name;
	const uint8_t *data;
	size_t offset;
	size_t size;
	size_t total = 0;
	uint64_t visited = 0;
	int found = 0;
	enum btrfs_result error;

	/* The stub directory of an unreferenced subvolume entry has no xattrs. */
	if (inode != NULL && inode->id.inode == BTRFS_EMPTY_SUBVOLUME_INODE) {
		return wanted == NULL ? BTRFS_OK : BTRFS_NOT_FOUND;
	}
	error = bt_inode_cursor(fs, inode, &cursor);
	if (error != BTRFS_OK) {
		return error;
	}
	key.objectid = inode->id.inode;
	key.type = BT_XATTR_ITEM;
	key.offset = wanted == NULL ? 0 : bt_crc32c(UINT32_MAX - 1U, wanted, wanted_length);
	error = bt_cursor_seek(&cursor, key, 0);
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != key.objectid || record.key.type != key.type ||
		    (wanted != NULL && record.key.offset != key.offset)) {
			error = BTRFS_NOT_FOUND;
			break;
		}
		if (++visited > BT_MAX_TREE_ITEMS) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		offset = 0;
		while (
		    (error = bt_dir_record(&record, &offset, &header, &name, &data)) == BTRFS_OK) {
			if (wanted != NULL) {
				if (bt_u16(header->name_length) != wanted_length ||
				    !bt_equal(name, wanted, wanted_length)) {
					continue;
				}
				if (found) {
					error = BTRFS_CORRUPT;
					break;
				}
				found = 1;
				size = bt_u16(header->data_length);
				if (buffer != NULL && size <= capacity) {
					bt_copy(buffer, data, size);
				}
				total = size;
			} else {
				size = bt_u16(header->name_length) + 1U;
				if (size > SIZE_MAX - total) {
					error = BTRFS_RANGE;
					break;
				}
				if (buffer != NULL && total <= capacity &&
				    size <= capacity - total) {
					bt_copy((uint8_t *)buffer + total, name, size - 1);
					((uint8_t *)buffer)[total + size - 1] = 0;
				}
				total += size;
			}
		}
		if (error != BTRFS_NOT_FOUND || wanted != NULL) {
			break;
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (error != BTRFS_NOT_FOUND) {
		return error;
	}
	if (wanted != NULL && !found) {
		return BTRFS_NOT_FOUND;
	}
	*length = total;
	return buffer != NULL && total > capacity ? BTRFS_RANGE : BTRFS_OK;
}

enum btrfs_result
btrfs_get_xattr(const struct btrfs_fs *fs, const struct btrfs_inode *inode, const void *name,
    size_t name_length, void *buffer, size_t capacity, size_t *length)
{
	if (length == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*length = 0;
	if (!bt_xattr_name_valid(name, name_length)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	return bt_xattr_scan(fs, inode, name, name_length, buffer, capacity, length);
}

enum btrfs_result
btrfs_list_xattrs(const struct btrfs_fs *fs, const struct btrfs_inode *inode, void *buffer,
    size_t capacity, size_t *length)
{
	enum btrfs_result error;
	size_t required = 0;

	if (length == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*length = 0;
	/* The sizing pass prevents partially publishing a list into a short buffer. */
	error = bt_xattr_scan(fs, inode, NULL, 0, NULL, 0, &required);
	if (error != BTRFS_OK) {
		return error;
	}
	*length = required;
	if (buffer == NULL) {
		return BTRFS_OK;
	}
	if (capacity < required) {
		return BTRFS_RANGE;
	}
	return bt_xattr_scan(fs, inode, NULL, 0, buffer, capacity, length);
}
