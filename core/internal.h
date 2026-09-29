/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_INTERNAL_H
#define MACHLIN_BTRFS_INTERNAL_H

#include "disk.h"

struct bt_key {
	uint64_t objectid, offset;
	uint8_t type;
};

struct bt_root {
	uint64_t address, generation, owner;
	uint8_t level;
};

struct bt_chunk {
	uint64_t logical, length, type, physical[2];
	uint8_t mirrors, confirmed;
};

struct btrfs_fs {
	struct btrfs_environment env;
	struct btrfs_info info;
	struct bt_root root_tree, chunk_tree, checksum_tree, selected_tree;
	struct btrfs_inode root_inode;
	uint8_t metadata_uuid[BTRFS_UUID_SIZE], device_uuid[BTRFS_UUID_SIZE];
	uint64_t device_id, device_size;
	struct bt_chunk *chunks;
	size_t chunk_count;
};

struct bt_cursor {
	const struct btrfs_fs *fs;
	struct bt_root root;
	uint8_t *blocks[BT_MAX_LEVEL];
	struct bt_root loaded[BT_MAX_LEVEL];
	uint32_t slots[BT_MAX_LEVEL];
	int valid;
};

struct bt_record {
	struct bt_key key;
	const uint8_t *data;
	size_t size;
};

uint16_t bt_u16(struct bt_le16 value);
uint32_t bt_u32(struct bt_le32 value);
uint64_t bt_u64(struct bt_le64 value);
void bt_copy(void *destination, const void *source, size_t length);
void bt_zero(void *buffer, size_t length);
int bt_equal(const void *a, const void *b, size_t length);
uint32_t bt_crc32c(uint32_t seed, const void *buffer, size_t length);
struct bt_key bt_key_decode(const struct bt_disk_key *key);
int bt_key_compare(struct bt_key a, struct bt_key b);
int bt_file_tree(uint64_t tree);
int bt_name_valid(const void *name, size_t length);
int bt_xattr_name_valid(const void *name, size_t length);
enum btrfs_result bt_read_physical(
    const struct btrfs_fs *fs, uint64_t offset, void *buffer, size_t length);
enum btrfs_result bt_chunk_add(
    struct btrfs_fs *fs, struct bt_key key, const void *data, size_t length, int bootstrap);
enum btrfs_result bt_map(const struct btrfs_fs *fs, uint64_t logical, size_t length, uint64_t kind,
    unsigned mirror, uint64_t *physical, unsigned *mirrors);
enum btrfs_result bt_tree_read(const struct btrfs_fs *fs, struct bt_root root, void *buffer);
void bt_cursor_init(struct bt_cursor *cursor, const struct btrfs_fs *fs, struct bt_root root);
void bt_cursor_fini(struct bt_cursor *cursor);
enum btrfs_result bt_cursor_seek(struct bt_cursor *cursor, struct bt_key key, int predecessor);
enum btrfs_result bt_cursor_next(struct bt_cursor *cursor);
enum btrfs_result bt_cursor_record(const struct bt_cursor *cursor, struct bt_record *record);
enum btrfs_result bt_find_root(const struct btrfs_fs *fs, uint64_t tree, struct bt_root *root);
enum btrfs_result bt_inode_cursor(
    const struct btrfs_fs *fs, const struct btrfs_inode *inode, struct bt_cursor *cursor);
enum btrfs_result bt_dir_record(const struct bt_record *record, size_t *offset,
    const struct bt_disk_dir **header, const uint8_t **name, const uint8_t **data);
enum btrfs_result bt_dir_identity(
    uint64_t tree, const struct bt_disk_dir *header, struct btrfs_object_id *id);
enum btrfs_result bt_default_tree(const struct btrfs_fs *fs, uint64_t *tree);
enum btrfs_result bt_data_read(
    const struct btrfs_fs *fs, uint64_t logical, void *buffer, size_t length, int checksummed);

#endif
