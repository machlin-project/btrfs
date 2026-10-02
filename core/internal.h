/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_INTERNAL_H
#define MACHLIN_BTRFS_INTERNAL_H

#include "disk.h"

struct bt_key {
	uint64_t objectid, offset;
	uint8_t type;
};

/* Wire decoding and key order sit on every tree search; defined here so each
 * translation unit inlines them. */
static inline uint16_t
bt_u16(struct bt_le16 value)
{
	return (uint16_t)((uint16_t)value.bytes[0] | (uint16_t)value.bytes[1] << 8);
}

static inline uint32_t
bt_u32(struct bt_le32 value)
{
	return (uint32_t)value.bytes[0] | (uint32_t)value.bytes[1] << 8 |
	    (uint32_t)value.bytes[2] << 16 | (uint32_t)value.bytes[3] << 24;
}

static inline uint64_t
bt_u64(struct bt_le64 value)
{
	uint64_t result = 0;
	unsigned i;

	for (i = 0; i < sizeof(value.bytes); i++) {
		result |= (uint64_t)value.bytes[i] << (i * 8U);
	}
	return result;
}

static inline struct bt_key
bt_key_decode(const struct bt_disk_key *key)
{
	struct bt_key result;

	result.objectid = bt_u64(key->objectid);
	result.type = key->type;
	result.offset = bt_u64(key->offset);
	return result;
}

static inline int
bt_key_compare(struct bt_key a, struct bt_key b)
{
	if (a.objectid != b.objectid) {
		return a.objectid < b.objectid ? -1 : 1;
	}
	if (a.type != b.type) {
		return a.type < b.type ? -1 : 1;
	}
	if (a.offset != b.offset) {
		return a.offset < b.offset ? -1 : 1;
	}
	return 0;
}

static inline int
bt_file_tree(uint64_t tree)
{
	return tree == BTRFS_TOP_LEVEL_TREE ||
	    (tree >= BTRFS_ROOT_INODE && tree <= BT_LAST_FREE_OBJECTID);
}

struct bt_root {
	uint64_t address, generation, owner;
	uint8_t level;
};

struct bt_chunk {
	uint64_t logical, length, type, physical[2];
	uint8_t mirrors, confirmed;
	/* A writer removes this unused block group at commit. */
	uint8_t removed;
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
	/* Nodes of this generation and later are not committed in this view (a
	 * transaction's private nodes) and never enter the shared node cache. */
	uint64_t cache_limit;
};

/* blocks[level] is the cursor's own buffer (owned[level]) or a node read in
 * place from the shared cache, pinned while pins[level] (handle + 1) is set. */
struct bt_cursor {
	const struct btrfs_fs *fs;
	struct bt_root root;
	const uint8_t *blocks[BT_MAX_LEVEL];
	uint8_t *owned[BT_MAX_LEVEL];
	size_t pins[BT_MAX_LEVEL];
	struct bt_root loaded[BT_MAX_LEVEL];
	uint32_t slots[BT_MAX_LEVEL];
	int valid;
};

struct bt_record {
	struct bt_key key;
	const uint8_t *data;
	size_t size;
};

/* The shared node cache (core/cache.c): get copies a stored node and its
 * header owner into buffer; put stores a verified node. pin returns a stored
 * node in place, unchanged and never replaced until bt_cache_unpin(handle). */
int bt_cache_get(struct btrfs_cache *cache, struct bt_root root, uint32_t node_size, void *buffer,
    uint64_t *owner);
void bt_cache_put(struct btrfs_cache *cache, struct bt_root root, uint32_t node_size,
    const void *node, uint64_t owner);
const uint8_t *bt_cache_pin(struct btrfs_cache *cache, struct bt_root root, uint32_t node_size,
    uint64_t *owner, size_t *handle);
void bt_cache_unpin(struct btrfs_cache *cache, size_t handle);

void bt_copy(void *destination, const void *source, size_t length);
void bt_move(void *destination, const void *source, size_t length);
void bt_zero(void *buffer, size_t length);
int bt_equal(const void *a, const void *b, size_t length);
uint32_t bt_crc32c(uint32_t seed, const void *buffer, size_t length);
int bt_name_valid(const void *name, size_t length);
int bt_xattr_name_valid(const void *name, size_t length);
enum btrfs_result bt_read_physical(
    const struct btrfs_fs *fs, uint64_t offset, void *buffer, size_t length);
uint64_t bt_super_offset(unsigned mirror);
int bt_super_present(uint64_t device_size, unsigned mirror);
enum btrfs_result bt_super_check(const struct bt_disk_super *super, uint64_t offset);
int bt_super_same(const struct bt_disk_super *a, const struct bt_disk_super *b);
enum btrfs_result bt_mount_super(const struct btrfs_environment *environment,
    const struct bt_disk_super *super, uint64_t offset, uint64_t tree, struct btrfs_fs **result);
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
