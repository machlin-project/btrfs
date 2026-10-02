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

/* a <= b, written so the compiler can evaluate it without branches. */
static inline int
bt_key_at_most(struct bt_key a, struct bt_key b)
{
	return (a.objectid < b.objectid) |
	    ((a.objectid == b.objectid) &
		((a.type < b.type) | ((a.type == b.type) & (a.offset <= b.offset))));
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

/* Copies of a block: SINGLE has one, DUP two on the same device. */
#define BT_MAX_MIRRORS 2U

struct bt_chunk {
	uint64_t logical, length, type, physical[BT_MAX_MIRRORS];
	uint8_t mirrors, confirmed;
	/* A writer removes this unused block group at commit. */
	uint8_t removed;
};

/* Bits of a CRC32C register. */
#define BT_CRC_BITS 32U
/* Lanes bt_crc32c_block checksums in parallel, and the shortest lane worth
 * joining them. */
#define BT_CRC_BLOCK_LANES 3U
#define BT_CRC_BLOCK_MINIMUM 256U

/* Joins lanes of one block's CRC: column i advances register bit i over one
 * lane of zero bytes. Zero lane: no join, a single chain. */
struct bt_crc_shift {
	size_t length;
	size_t lane;
	uint32_t columns[BT_CRC_BITS];
};

struct btrfs_fs {
	struct btrfs_environment env;
	struct btrfs_info info;
	struct bt_root root_tree, chunk_tree, checksum_tree, selected_tree;
	struct btrfs_inode root_inode;
	uint8_t metadata_uuid[BTRFS_UUID_SIZE], device_uuid[BTRFS_UUID_SIZE];
	uint64_t device_id, device_size;
	/* Joins the lanes of a node checksum (node_size - BT_CSUM_SIZE bytes). */
	struct bt_crc_shift node_crc;
	/* Sorted by logical address; chunk_capacity entries are allocated. */
	struct bt_chunk *chunks;
	size_t chunk_count;
	size_t chunk_capacity;
	/* Nodes of this generation and later are not committed in this view (a
	 * transaction's private nodes) and never enter the shared node cache. */
	uint64_t cache_limit;
	/* A transaction's private view finds its own nodes by logical address,
	 * with no device read or checksum; NOT_FOUND falls back to a read. The
	 * node stays valid until the transaction's next edit. */
	enum btrfs_result (*private_node)(void *context, struct bt_root root, const uint8_t **node);
	void *private_context;
	/* Nonzero for a transaction's reader view, whose readers finish before
	 * its next operation: every cursor reads its nodes in place. */
	int private_borrow;
	/* A transaction's reader view resolves the trees the transaction owns to
	 * their private roots: nonzero with *result OK or NOT_FOUND (a deleted
	 * subvolume); zero to look the root item up. */
	int (*private_root)(
	    void *context, uint64_t tree, struct bt_root *root, enum btrfs_result *result);
	void *private_root_context;
};

/* blocks[level] is the cursor's own buffer (owned[level]), a node read in
 * place from the shared cache, pinned while pins[level] (handle + 1) is set,
 * or, with borrow set, a transaction's own node read in place. */
struct bt_cursor {
	const struct btrfs_fs *fs;
	struct bt_root root;
	const uint8_t *blocks[BT_MAX_LEVEL];
	uint8_t *owned[BT_MAX_LEVEL];
	size_t pins[BT_MAX_LEVEL];
	struct bt_root loaded[BT_MAX_LEVEL];
	uint32_t slots[BT_MAX_LEVEL];
	int valid;
	/* Set by a transaction's own lookups and on its reader view, neither of
	 * which edits before bt_cursor_fini; other cursors copy its nodes. */
	int borrow;
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
/* hint is any address on the caller's stack: it spreads concurrent callers'
 * pin counters over separate cache lines. */
const uint8_t *bt_cache_pin(struct btrfs_cache *cache, struct bt_root root, uint32_t node_size,
    uint64_t *owner, size_t *handle, const void *hint);
void bt_cache_unpin(struct btrfs_cache *cache, size_t handle);

void bt_copy(void *destination, const void *source, size_t length);
void bt_move(void *destination, const void *source, size_t length);
void bt_zero(void *buffer, size_t length);
int bt_equal(const void *a, const void *b, size_t length);
uint32_t bt_crc32c(uint32_t seed, const void *buffer, size_t length);
/* Lanes bt_crc32c_sectors interleaves, and the sectors its callers checksum
 * per call (a stack array of results). */
#define BT_CRC_LANES 4U
#define BT_CRC_BATCH 64U
/* checksums[i] = Btrfs CRC32C (~bt_crc32c(UINT32_MAX, ...)) of sector i. */
void bt_crc32c_sectors(const void *data, size_t sector_size, size_t count, uint32_t *checksums);
/* Prepares bt_crc32c_block for blocks of length bytes. */
void bt_crc_shift_init(struct bt_crc_shift *shift, size_t length);
/* bt_crc32c of a block in parallel lanes when shift was prepared for its
 * length; otherwise a single chain. */
uint32_t bt_crc32c_block(
    const struct bt_crc_shift *shift, uint32_t seed, const void *buffer, size_t length);
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
/* Index of the first chunk at or after logical; chunk_count if none. */
size_t bt_chunk_position(const struct btrfs_fs *fs, uint64_t logical);
/* Index of the chunk holding logical; chunk_count if none. */
size_t bt_chunk_containing(const struct btrfs_fs *fs, uint64_t logical);
enum btrfs_result bt_chunk_add(
    struct btrfs_fs *fs, struct bt_key key, const void *data, size_t length, int bootstrap);
enum btrfs_result bt_map(const struct btrfs_fs *fs, uint64_t logical, size_t length, uint64_t kind,
    unsigned mirror, uint64_t *physical, unsigned *mirrors);
enum btrfs_result bt_tree_read(const struct btrfs_fs *fs, struct bt_root root, void *buffer);
enum btrfs_result bt_node_items(const struct btrfs_fs *fs, const uint8_t *block);
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
