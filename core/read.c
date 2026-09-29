/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

struct bt_read_session {
	struct bt_cursor checksums;
	uint8_t *window;
	size_t window_size;
};

static enum btrfs_result
bt_sector_checksum(struct bt_cursor *cursor, uint64_t logical, uint32_t *checksum)
{
	struct bt_key key = {
		.objectid = BT_CSUM_OBJECTID, .type = BT_EXTENT_CSUM, .offset = logical
	};
	struct bt_record record;
	struct bt_le32 disk;
	uint64_t index;
	enum btrfs_result error;

	error = bt_cursor_record(cursor, &record);
	if (error != BTRFS_OK || record.key.objectid != key.objectid ||
	    record.key.type != key.type || logical < record.key.offset ||
	    (logical - record.key.offset) / cursor->fs->info.sector_size >=
		record.size / sizeof(disk)) {
		error = bt_cursor_seek(cursor, key, 1);
	}
	if (error != BTRFS_OK) {
		return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
	}
	(void)bt_cursor_record(cursor, &record);
	if (record.key.objectid != key.objectid || record.key.type != key.type ||
	    record.key.offset % cursor->fs->info.sector_size != 0 ||
	    record.size % sizeof(disk) != 0) {
		return BTRFS_CORRUPT;
	}
	index = (logical - record.key.offset) / cursor->fs->info.sector_size;
	if (index >= record.size / sizeof(disk)) {
		return BTRFS_CORRUPT;
	}
	bt_copy(&disk, record.data + (size_t)index * sizeof(disk), sizeof(disk));
	*checksum = bt_u32(disk);
	return BTRFS_OK;
}

static enum btrfs_result
bt_verified_read(
    struct bt_cursor *cursor, uint64_t logical, void *buffer, size_t length, int checksummed)
{
	const struct btrfs_fs *fs = cursor->fs;
	uint8_t *out = buffer;
	uint64_t physical;
	size_t position;
	unsigned mirror;
	unsigned mirrors = 1;
	unsigned selected = 0;
	uint32_t checksum = 0;
	enum btrfs_result error = BTRFS_OK;

	if (logical % fs->info.sector_size != 0 || length % fs->info.sector_size != 0 ||
	    length > UINT64_MAX - logical) {
		return BTRFS_CORRUPT;
	}
	/* One contiguous device request for the range. Checksums are consumed from
	 * the retained checksum cursor; retry only a bad sector on the other copy. */
	for (mirror = 0; mirror < mirrors; mirror++) {
		error = bt_map(fs, logical, length, BT_BLOCK_DATA, mirror, &physical, &mirrors);
		if (error != BTRFS_OK) {
			return error;
		}
		error = bt_read_physical(fs, physical, buffer, length);
		if (error == BTRFS_OK) {
			selected = mirror;
			break;
		}
		if (error != BTRFS_IO) {
			return error;
		}
	}
	if (error != BTRFS_OK || !checksummed) {
		return error;
	}
	for (position = 0; position < length; position += fs->info.sector_size) {
		error = bt_sector_checksum(cursor, logical + position, &checksum);
		if (error != BTRFS_OK) {
			break;
		}
		if (~bt_crc32c(UINT32_MAX, out + position, fs->info.sector_size) == checksum) {
			continue;
		}
		error = BTRFS_CORRUPT;
		for (mirror = 0; mirror < mirrors; mirror++) {
			if (mirror == selected) {
				continue;
			}
			error = bt_map(fs, logical + position, fs->info.sector_size, BT_BLOCK_DATA,
			    mirror, &physical, &mirrors);
			if (error != BTRFS_OK) {
				break;
			}
			error =
			    bt_read_physical(fs, physical, out + position, fs->info.sector_size);
			if (error == BTRFS_OK &&
			    ~bt_crc32c(UINT32_MAX, out + position, fs->info.sector_size) !=
				checksum) {
				error = BTRFS_CORRUPT;
			}
			if (error == BTRFS_OK || (error != BTRFS_CORRUPT && error != BTRFS_IO)) {
				break;
			}
		}
		if (error != BTRFS_OK) {
			break;
		}
	}
	return error;
}

enum btrfs_result
bt_data_read(
    const struct btrfs_fs *fs, uint64_t logical, void *buffer, size_t length, int checksummed)
{
	struct bt_cursor cursor;
	enum btrfs_result error;

	bt_cursor_init(&cursor, fs, fs->checksum_tree);
	error = bt_verified_read(&cursor, logical, buffer, length, checksummed);
	bt_cursor_fini(&cursor);
	return error;
}

static enum btrfs_result
bt_extent_length(const struct btrfs_fs *fs, const struct bt_record *record,
    const struct btrfs_inode *inode, uint64_t *length)
{
	const struct bt_disk_extent_header *header = (const void *)record->data;
	const struct bt_disk_extent *extent = (const void *)record->data;
	uint64_t physical;
	uint64_t disk_bytes;
	uint64_t ram;
	uint64_t offset;

	if (record->size < sizeof(*header)) {
		return BTRFS_CORRUPT;
	}
	if (header->encryption != 0 || bt_u16(header->encoding) != 0 ||
	    header->compression > BTRFS_COMPRESSION_ZSTD) {
		return BTRFS_UNSUPPORTED;
	}
	if (bt_u64(header->generation) > fs->info.generation) {
		return BTRFS_CORRUPT;
	}
	ram = bt_u64(header->ram_bytes);
	if (header->type == BT_EXTENT_INLINE) {
		if (record->key.offset != 0 || ram == 0 || ram != inode->size ||
		    ram > BT_MAX_COMPRESSED_SIZE ||
		    (header->compression == BTRFS_COMPRESSION_NONE &&
			ram != record->size - sizeof(*header))) {
			return BTRFS_CORRUPT;
		}
		*length = ram;
		return BTRFS_OK;
	}
	if (header->type != BT_EXTENT_REGULAR && header->type != BT_EXTENT_PREALLOC) {
		return BTRFS_UNSUPPORTED;
	}
	if (record->size != sizeof(*extent)) {
		return BTRFS_CORRUPT;
	}
	*length = bt_u64(extent->length);
	physical = bt_u64(extent->disk_bytenr);
	disk_bytes = bt_u64(extent->disk_bytes);
	offset = bt_u64(extent->offset);
	if (*length == 0 || *length > UINT64_MAX - record->key.offset ||
	    *length % fs->info.sector_size != 0 || record->key.offset % fs->info.sector_size != 0 ||
	    offset % fs->info.sector_size != 0 || physical % fs->info.sector_size != 0 ||
	    disk_bytes % fs->info.sector_size != 0 || disk_bytes > UINT64_MAX - physical) {
		return BTRFS_CORRUPT;
	}
	if (physical == 0) {
		return disk_bytes == 0 && offset == 0 &&
			header->compression == BTRFS_COMPRESSION_NONE &&
			header->type == BT_EXTENT_REGULAR
		    ? BTRFS_OK
		    : BTRFS_CORRUPT;
	}
	if (offset > ram || *length > ram - offset || disk_bytes == 0 ||
	    (header->compression == BTRFS_COMPRESSION_NONE && ram != disk_bytes) ||
	    (header->type == BT_EXTENT_PREALLOC && header->compression != BTRFS_COMPRESSION_NONE) ||
	    (header->compression != BTRFS_COMPRESSION_NONE &&
		(ram > BT_MAX_COMPRESSED_SIZE || disk_bytes > BT_MAX_COMPRESSED_SIZE))) {
		return BTRFS_CORRUPT;
	}
	return BTRFS_OK;
}

static enum btrfs_result
bt_compressed_read(struct bt_read_session *session, const struct bt_record *record,
    const struct btrfs_inode *inode, uint64_t within, void *output, size_t length)
{
	const struct btrfs_fs *fs = session->checksums.fs;
	const struct bt_disk_extent_header *header = (const void *)record->data;
	const struct bt_disk_extent *extent = (const void *)record->data;
	const uint8_t *input;
	uint8_t *stored = NULL;
	uint8_t *decoded;
	size_t stored_size;
	size_t decoded_size = (size_t)bt_u64(header->ram_bytes);
	uint64_t offset = within;
	enum btrfs_result error;

	if (fs->env.decompress == NULL) {
		return BTRFS_UNSUPPORTED;
	}
	if (header->type == BT_EXTENT_INLINE) {
		input = record->data + sizeof(*header);
		stored_size = record->size - sizeof(*header);
	} else {
		stored_size = (size_t)bt_u64(extent->disk_bytes);
		stored = fs->env.allocate(fs->env.context, stored_size);
		if (stored == NULL) {
			return BTRFS_NO_MEMORY;
		}
		error = bt_verified_read(&session->checksums, bt_u64(extent->disk_bytenr), stored,
		    stored_size, (inode->flags & BT_INODE_NODATASUM) == 0);
		if (error != BTRFS_OK) {
			fs->env.release(fs->env.context, stored, stored_size);
			return error;
		}
		input = stored;
		offset += bt_u64(extent->offset);
	}
	decoded = fs->env.allocate(fs->env.context, decoded_size);
	if (decoded == NULL) {
		error = BTRFS_NO_MEMORY;
	} else {
		error =
		    fs->env.decompress(fs->env.context, (enum btrfs_compression)header->compression,
			input, stored_size, decoded, decoded_size);
		if (error == BTRFS_OK) {
			bt_copy(output, decoded + offset, length);
		}
		fs->env.release(fs->env.context, decoded, decoded_size);
	}
	if (stored != NULL) {
		fs->env.release(fs->env.context, stored, stored_size);
	}
	return error;
}

static enum btrfs_result
bt_read_extent(struct bt_read_session *session, const struct bt_record *record,
    const struct btrfs_inode *inode, uint64_t within, void *output, size_t length,
    size_t *completed)
{
	const struct btrfs_fs *fs = session->checksums.fs;
	const struct bt_disk_extent *extent = (const void *)record->data;
	const struct bt_disk_extent_header *header = (const void *)record->data;
	uint64_t logical;
	size_t skip;
	size_t count;
	size_t aligned;
	enum btrfs_result error = BTRFS_OK;

	*completed = 0;
	if (header->compression != BTRFS_COMPRESSION_NONE) {
		error = bt_compressed_read(session, record, inode, within, output, length);
		if (error == BTRFS_OK) {
			*completed = length;
		}
		return error;
	}
	if (header->type == BT_EXTENT_INLINE) {
		bt_copy(output, record->data + sizeof(*header) + within, length);
		*completed = length;
		return BTRFS_OK;
	}
	if (header->type == BT_EXTENT_PREALLOC || bt_u64(extent->disk_bytenr) == 0) {
		bt_zero(output, length);
		*completed = length;
		return BTRFS_OK;
	}
	logical = bt_u64(extent->disk_bytenr) + bt_u64(extent->offset) + within;
	if (session->window == NULL) {
		session->window = fs->env.allocate(fs->env.context, session->window_size);
		if (session->window == NULL) {
			return BTRFS_NO_MEMORY;
		}
	}
	while (*completed < length) {
		skip = (size_t)(logical % fs->info.sector_size);
		count = session->window_size - skip;
		if (count > length - *completed) {
			count = length - *completed;
		}
		aligned = ((count + skip + fs->info.sector_size - 1) / fs->info.sector_size) *
		    fs->info.sector_size;
		error = bt_verified_read(&session->checksums, logical - skip, session->window,
		    aligned, (inode->flags & BT_INODE_NODATASUM) == 0);
		if (error != BTRFS_OK) {
			break;
		}
		bt_copy((uint8_t *)output + *completed, session->window + skip, count);
		logical += count;
		*completed += count;
	}
	return error;
}

enum btrfs_result
btrfs_read(const struct btrfs_fs *fs, const struct btrfs_inode *inode, uint64_t offset,
    void *buffer, size_t length, size_t *completed)
{
	struct bt_cursor cursor;
	struct bt_read_session session;
	struct bt_record record;
	struct bt_key key;
	uint64_t extent_length;
	uint64_t end;
	uint64_t position;
	uint64_t previous_end = 0;
	uint64_t visited = 0;
	size_t count;
	size_t copied;
	enum btrfs_result error;

	if (completed == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*completed = 0;
	if (fs == NULL || inode == NULL || (buffer == NULL && length != 0)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if ((inode->mode & BTRFS_MODE_TYPE) == BTRFS_MODE_DIRECTORY) {
		return BTRFS_IS_DIRECTORY;
	}
	if ((inode->mode & BTRFS_MODE_TYPE) != BTRFS_MODE_REGULAR &&
	    (inode->mode & BTRFS_MODE_TYPE) != BTRFS_MODE_SYMLINK) {
		return BTRFS_UNSUPPORTED;
	}
	if (offset >= inode->size || length == 0) {
		return BTRFS_OK;
	}
	if (length > inode->size - offset) {
		length = (size_t)(inode->size - offset);
	}
	error = bt_inode_cursor(fs, inode, &cursor);
	if (error != BTRFS_OK) {
		return error;
	}
	bt_zero(&session, sizeof(session));
	bt_cursor_init(&session.checksums, fs, fs->checksum_tree);
	session.window_size = length >= BT_READ_WINDOW - fs->info.sector_size
	    ? BT_READ_WINDOW
	    : ((length + fs->info.sector_size - 1) / fs->info.sector_size + 1) *
		fs->info.sector_size;
	key.objectid = inode->id.inode;
	key.type = BT_EXTENT_DATA;
	key.offset = offset;
	error = bt_cursor_seek(&cursor, key, 1);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
	}
	if (error == BTRFS_NOT_FOUND ||
	    (error == BTRFS_OK &&
		(record.key.objectid != key.objectid || record.key.type != key.type))) {
		error = bt_cursor_seek(&cursor, key, 0);
	}
	while (*completed < length && (error == BTRFS_OK || error == BTRFS_NOT_FOUND)) {
		position = offset + *completed;
		if (error == BTRFS_OK) {
			(void)bt_cursor_record(&cursor, &record);
		}
		if (error == BTRFS_NOT_FOUND || record.key.objectid != key.objectid ||
		    record.key.type != key.type) {
			bt_zero((uint8_t *)buffer + *completed, length - *completed);
			*completed = length;
			error = BTRFS_OK;
			break;
		}
		if (++visited > BT_MAX_TREE_ITEMS) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		error = bt_extent_length(fs, &record, inode, &extent_length);
		if (error != BTRFS_OK || record.key.offset < previous_end) {
			error = error == BTRFS_OK ? BTRFS_CORRUPT : error;
			break;
		}
		end = record.key.offset + extent_length;
		previous_end = end;
		if (position < record.key.offset) {
			count = length - *completed;
			if (count > record.key.offset - position) {
				count = (size_t)(record.key.offset - position);
			}
			bt_zero((uint8_t *)buffer + *completed, count);
			*completed += count;
			position += count;
		}
		if (*completed < length && position < end) {
			count = length - *completed;
			if (count > end - position) {
				count = (size_t)(end - position);
			}
			error =
			    bt_read_extent(&session, &record, inode, position - record.key.offset,
				(uint8_t *)buffer + *completed, count, &copied);
			*completed += copied;
			if (error != BTRFS_OK) {
				break;
			}
		}
		if (*completed == length) {
			error = BTRFS_OK;
			break;
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	bt_cursor_fini(&session.checksums);
	if (session.window != NULL) {
		fs->env.release(fs->env.context, session.window, session.window_size);
	}
	return error;
}
