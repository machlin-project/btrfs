/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

enum btrfs_result
bt_dir_record(const struct bt_record *record, size_t *offset, const struct bt_disk_dir **header,
    const uint8_t **name, const uint8_t **data)
{
	const struct bt_disk_dir *disk;
	size_t name_size;
	size_t data_size;
	size_t remaining;

	if (*offset == record->size) {
		return BTRFS_NOT_FOUND;
	}
	if (*offset > record->size || record->size - *offset < sizeof(*disk)) {
		return BTRFS_CORRUPT;
	}
	disk = (const void *)(record->data + *offset);
	name_size = bt_u16(disk->name_length);
	data_size = bt_u16(disk->data_length);
	remaining = record->size - *offset - sizeof(*disk);
	if (name_size > remaining || data_size > remaining - name_size ||
	    (record->key.type == BT_XATTR_ITEM
		    ? (disk->type != BTRFS_FT_XATTR || !bt_xattr_name_valid(disk + 1, name_size))
		    : (disk->type == BTRFS_FT_UNKNOWN || disk->type > BTRFS_FT_SYMLINK ||
			  data_size != 0 || !bt_name_valid(disk + 1, name_size)))) {
		return BTRFS_CORRUPT;
	}
	if ((record->key.type == BT_DIR_ITEM || record->key.type == BT_XATTR_ITEM) &&
	    bt_crc32c(UINT32_MAX - 1U, disk + 1, name_size) != record->key.offset) {
		return BTRFS_CORRUPT;
	}
	*header = disk;
	*name = (const void *)(disk + 1);
	*data = *name + name_size;
	*offset += sizeof(*disk) + name_size + data_size;
	return BTRFS_OK;
}

/* Whether tree's ROOT_REF to child names this entry (directory and name), as
 * Linux's fixup_tree_root_location requires before entering a subvolume. */
static enum btrfs_result
bt_dir_root_ref(const struct btrfs_fs *fs, uint64_t tree, uint64_t child, uint64_t directory,
    const uint8_t *name, size_t length, int *referenced)
{
	const struct bt_disk_root_ref *ref;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { tree, child, BT_ROOT_REF };
	enum btrfs_result error;

	*referenced = 0;
	bt_cursor_init(&cursor, fs, fs->root_tree);
	error = bt_cursor_seek(&cursor, key, 0);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		ref = (const void *)record.data;
		if (bt_key_compare(record.key, key) != 0) {
			error = BTRFS_NOT_FOUND;
		} else if (record.size < sizeof(*ref) ||
		    record.size != sizeof(*ref) + bt_u16(ref->name_length)) {
			error = BTRFS_CORRUPT;
		} else {
			*referenced = bt_u64(ref->directory) == directory &&
			    bt_u16(ref->name_length) == length && bt_equal(ref + 1, name, length);
		}
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* The object a directory entry names. A subvolume entry enters the
 * subvolume only when tree's root reference names the entry; otherwise it is
 * the empty stub directory. */
static enum btrfs_result
bt_dir_resolve(const struct btrfs_fs *fs, uint64_t tree, uint64_t directory,
    const struct bt_disk_dir *header, const uint8_t *name, struct btrfs_object_id *id)
{
	int referenced;
	enum btrfs_result error;

	error = bt_dir_identity(tree, header, id);
	if (error != BTRFS_OK || id->tree == tree) {
		return error;
	}
	error = bt_dir_root_ref(
	    fs, tree, id->tree, directory, name, bt_u16(header->name_length), &referenced);
	if (error == BTRFS_OK && !referenced) {
		id->tree = tree;
		id->inode = BTRFS_EMPTY_SUBVOLUME_INODE;
	}
	return error;
}

enum btrfs_result
bt_dir_identity(uint64_t tree, const struct bt_disk_dir *header, struct btrfs_object_id *id)
{
	struct bt_key key = bt_key_decode(&header->location);

	if (key.type == BT_INODE_ITEM && key.objectid >= BTRFS_ROOT_INODE && key.offset == 0) {
		id->tree = tree;
		id->inode = key.objectid;
		return BTRFS_OK;
	}
	if (key.type == BT_ROOT_ITEM && bt_file_tree(key.objectid) &&
	    header->type == BTRFS_FT_DIRECTORY) {
		id->tree = key.objectid;
		id->inode = BTRFS_ROOT_INODE;
		return BTRFS_OK;
	}
	return BTRFS_CORRUPT;
}

static enum btrfs_result
bt_lookup_record(struct bt_cursor *cursor, uint64_t directory, const void *name, size_t length,
    int resolve, struct btrfs_object_id *id)
{
	struct bt_key key = { .objectid = directory, .type = BT_DIR_ITEM };
	struct bt_record record;
	const struct bt_disk_dir *header;
	const uint8_t *entry_name;
	const uint8_t *data;
	struct btrfs_object_id found = { 0 };
	size_t offset = 0;
	int matched = 0;
	enum btrfs_result error;

	key.offset = bt_crc32c(UINT32_MAX - 1U, name, length);
	error = bt_cursor_seek(cursor, key, 0);
	if (error != BTRFS_OK) {
		return error;
	}
	(void)bt_cursor_record(cursor, &record);
	if (bt_key_compare(record.key, key) != 0) {
		return BTRFS_NOT_FOUND;
	}
	while ((error = bt_dir_record(&record, &offset, &header, &entry_name, &data)) == BTRFS_OK) {
		(void)data;
		if (bt_u16(header->name_length) == length && bt_equal(entry_name, name, length)) {
			if (matched) {
				return BTRFS_CORRUPT;
			}
			error = resolve ? bt_dir_resolve(cursor->fs, cursor->root.owner, directory,
					      header, entry_name, &found)
					: bt_dir_identity(cursor->root.owner, header, &found);
			if (error != BTRFS_OK) {
				return error;
			}
			matched = 1;
		}
	}
	if (error != BTRFS_NOT_FOUND) {
		return error;
	}
	if (!matched) {
		return BTRFS_NOT_FOUND;
	}
	*id = found;
	return BTRFS_OK;
}

enum btrfs_result
bt_default_tree(const struct btrfs_fs *fs, uint64_t *tree)
{
	struct bt_cursor cursor;
	struct btrfs_object_id id;
	enum btrfs_result error;
	static const uint8_t name[] = "default";

	*tree = BTRFS_TOP_LEVEL_TREE;
	if ((fs->info.incompat_features & BT_FEATURE_DEFAULT_SUBVOL) == 0) {
		return BTRFS_OK;
	}
	bt_cursor_init(&cursor, fs, fs->root_tree);
	/* The root tree's default entry has no root reference. */
	error = bt_lookup_record(&cursor, BT_ROOT_DIR_OBJECTID, name, sizeof(name) - 1, 0, &id);
	bt_cursor_fini(&cursor);
	if (error == BTRFS_OK) {
		*tree = id.tree;
	}
	return error;
}

/* Linux's new_simple_dir gives a stub the owner and access time of the
 * directory it was looked up in; the other times are those of the lookup. */
static void
bt_dir_stub(uint32_t uid, uint32_t gid, struct btrfs_time access_time, struct btrfs_inode *inode)
{
	inode->uid = uid;
	inode->gid = gid;
	inode->access_time = access_time;
}

enum btrfs_result
btrfs_lookup(const struct btrfs_fs *fs, const struct btrfs_inode *directory, const void *name,
    size_t length, struct btrfs_inode *inode)
{
	struct bt_cursor cursor;
	struct btrfs_object_id id;
	enum btrfs_result error;

	if (fs == NULL || directory == NULL || inode == NULL || !bt_name_valid(name, length)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if ((directory->mode & BTRFS_MODE_TYPE) != BTRFS_MODE_DIRECTORY) {
		return BTRFS_NOT_DIRECTORY;
	}
	if (length == 1 && bt_equal(name, ".", 1)) {
		*inode = *directory;
		return BTRFS_OK;
	}
	if (length == 2 && bt_equal(name, "..", 2)) {
		return btrfs_parent(fs, directory, inode);
	}
	if (directory->id.inode == BTRFS_EMPTY_SUBVOLUME_INODE) {
		return BTRFS_NOT_FOUND;
	}
	error = bt_inode_cursor(fs, directory, &cursor);
	if (error != BTRFS_OK) {
		return error;
	}
	error = bt_lookup_record(&cursor, directory->id.inode, name, length, 1, &id);
	bt_cursor_fini(&cursor);
	if (error == BTRFS_OK) {
		error = btrfs_get_inode(fs, id, inode);
	}
	if (error == BTRFS_OK && id.inode == BTRFS_EMPTY_SUBVOLUME_INODE) {
		bt_dir_stub(directory->uid, directory->gid, directory->access_time, inode);
	}
	return error;
}

struct btrfs_directory {
	struct bt_cursor cursor;
	/* Reads the inodes of entries in the directory's own tree; set up on
	 * first use. */
	struct bt_cursor inodes;
	int inodes_ready;
	uint64_t inode;
	/* What the directory passes to the stubs among its entries. */
	uint32_t uid;
	uint32_t gid;
	struct btrfs_time access_time;
	int advance;
	int end;
};

enum btrfs_result
btrfs_directory_open(const struct btrfs_fs *fs, const struct btrfs_inode *directory,
    uint64_t cookie, struct btrfs_directory **result)
{
	struct btrfs_directory *stream;
	struct bt_root root;
	struct bt_key key;
	enum btrfs_result error;

	if (result == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (fs == NULL || directory == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if ((directory->mode & BTRFS_MODE_TYPE) != BTRFS_MODE_DIRECTORY) {
		return BTRFS_NOT_DIRECTORY;
	}
	stream = fs->env.allocate(fs->env.context, sizeof(*stream));
	if (stream == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(stream, sizeof(*stream));
	if (directory->id.inode == BTRFS_EMPTY_SUBVOLUME_INODE) {
		/* The stub directory has no entries. */
		error = bt_find_root(fs, directory->id.tree, &root);
		if (error == BTRFS_OK) {
			bt_cursor_init(&stream->cursor, fs, root);
			stream->end = 1;
			*result = stream;
			return BTRFS_OK;
		}
	} else {
		error = bt_inode_cursor(fs, directory, &stream->cursor);
	}
	if (error != BTRFS_OK) {
		fs->env.release(fs->env.context, stream, sizeof(*stream));
		return error;
	}
	stream->inode = directory->id.inode;
	stream->uid = directory->uid;
	stream->gid = directory->gid;
	stream->access_time = directory->access_time;
	key.objectid = directory->id.inode;
	key.type = BT_DIR_INDEX;
	key.offset = cookie;
	error =
	    cookie == BTRFS_COOKIE_END ? BTRFS_NOT_FOUND : bt_cursor_seek(&stream->cursor, key, 0);
	if (error != BTRFS_OK && error != BTRFS_NOT_FOUND) {
		btrfs_directory_close(stream);
		return error;
	}
	stream->end = error == BTRFS_NOT_FOUND;
	*result = stream;
	return BTRFS_OK;
}

enum btrfs_result
btrfs_directory_next(
    struct btrfs_directory *stream, struct btrfs_dir_entry *entry, uint64_t *next_cookie)
{
	struct bt_record record;
	const struct bt_disk_dir *header;
	const uint8_t *name;
	const uint8_t *data;
	struct btrfs_dir_entry decoded;
	size_t offset = 0;
	enum btrfs_result error;

	if (stream == NULL || entry == NULL || next_cookie == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (stream->end) {
		return BTRFS_NOT_FOUND;
	}
	if (stream->advance) {
		error = bt_cursor_next(&stream->cursor);
		if (error != BTRFS_OK) {
			stream->end = 1;
			return error;
		}
	}
	(void)bt_cursor_record(&stream->cursor, &record);
	if (record.key.objectid != stream->inode || record.key.type != BT_DIR_INDEX) {
		stream->end = 1;
		return BTRFS_NOT_FOUND;
	}
	error = bt_dir_record(&record, &offset, &header, &name, &data);
	if (error != BTRFS_OK || offset != record.size || record.key.offset < 2) {
		stream->end = 1;
		return error == BTRFS_OK || error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
	}
	error = bt_dir_resolve(
	    stream->cursor.fs, stream->cursor.root.owner, stream->inode, header, name, &decoded.id);
	if (error != BTRFS_OK) {
		stream->end = 1;
		return error;
	}
	decoded.name_length = bt_u16(header->name_length);
	decoded.type = header->type;
	bt_copy(decoded.name, name, decoded.name_length);
	*entry = decoded;
	*next_cookie = record.key.offset == UINT64_MAX ? BTRFS_COOKIE_END : record.key.offset + 1;
	stream->advance = 1;
	return BTRFS_OK;
}

void
btrfs_directory_close(struct btrfs_directory *stream)
{
	const struct btrfs_fs *fs;

	if (stream == NULL) {
		return;
	}
	fs = stream->cursor.fs;
	bt_cursor_fini(&stream->cursor);
	if (stream->inodes_ready) {
		bt_cursor_fini(&stream->inodes);
	}
	fs->env.release(fs->env.context, stream, sizeof(*stream));
}

enum btrfs_result
btrfs_directory_inode(
    struct btrfs_directory *stream, const struct btrfs_dir_entry *entry, struct btrfs_inode *inode)
{
	enum btrfs_result error;

	if (stream == NULL || entry == NULL || inode == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	/* A subvolume's root directory, or its empty stub, is in another tree. */
	if (entry->id.tree != stream->cursor.root.owner || entry->id.inode < BTRFS_ROOT_INODE) {
		error = btrfs_get_inode(stream->cursor.fs, entry->id, inode);
		if (error == BTRFS_OK && entry->id.inode == BTRFS_EMPTY_SUBVOLUME_INODE) {
			bt_dir_stub(stream->uid, stream->gid, stream->access_time, inode);
		}
		return error;
	}
	if (!stream->inodes_ready) {
		bt_cursor_init(&stream->inodes, stream->cursor.fs, stream->cursor.root);
		stream->inodes_ready = 1;
	}
	return bt_inode_read(&stream->inodes, entry->id, 1, inode);
}

enum btrfs_result
btrfs_next_dir(const struct btrfs_fs *fs, const struct btrfs_inode *directory, uint64_t *cookie,
    struct btrfs_dir_entry *entry)
{
	struct btrfs_directory *stream = NULL;
	enum btrfs_result error;

	if (cookie == NULL || entry == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = btrfs_directory_open(fs, directory, *cookie, &stream);
	if (error == BTRFS_OK) {
		error = btrfs_directory_next(stream, entry, cookie);
	}
	btrfs_directory_close(stream);
	return error;
}
