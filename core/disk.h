/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_DISK_H
#define MACHLIN_BTRFS_DISK_H

#include <btrfs/btrfs.h>

/* Byte-array fields have alignment one, independent of host ABI. */
struct bt_le16 {
	uint8_t bytes[2];
};

struct bt_le32 {
	uint8_t bytes[4];
};

struct bt_le64 {
	uint8_t bytes[8];
};

#define BT_SUPER_OFFSET UINT64_C(65536)
#define BT_SUPER_SIZE 4096U
#define BT_CSUM_SIZE 32U
#define BT_SYSTEM_ARRAY_SIZE 2048U
#define BT_MAX_LEVEL 8U
#define BT_MAX_NODE_SIZE 65536U
#define BT_MAX_CHUNKS 4096U
#define BT_MAX_COMPRESSED_SIZE (128U * 1024U)
#define BT_READ_WINDOW (1024U * 1024U)
#define BT_STRIPE_LENGTH UINT64_C(65536)
#define BT_MAX_TREE_ITEMS UINT64_C(1048576)
#define BT_MAGIC "_BHRfS_M"
#define BT_ROOT_TREE UINT64_C(1)
#define BT_EXTENT_TREE UINT64_C(2)
#define BT_CHUNK_TREE UINT64_C(3)
#define BT_CSUM_TREE UINT64_C(7)
#define BT_FIRST_CHUNK_OBJECTID UINT64_C(256)
#define BT_ROOT_DIR_OBJECTID UINT64_C(6)
#define BT_CSUM_OBJECTID (UINT64_MAX - UINT64_C(9))
#define BT_LAST_FREE_OBJECTID (UINT64_MAX - UINT64_C(255))
#define BT_INODE_NODATASUM UINT64_C(1)
#define BT_BLOCK_DATA UINT64_C(1)
#define BT_BLOCK_SYSTEM UINT64_C(2)
#define BT_BLOCK_METADATA UINT64_C(4)
#define BT_BLOCK_DUP UINT64_C(32)
#define BT_FEATURE_MIXED_BACKREF (UINT64_C(1) << 0)
#define BT_FEATURE_DEFAULT_SUBVOL (UINT64_C(1) << 1)
#define BT_FEATURE_MIXED_GROUPS (UINT64_C(1) << 2)
#define BT_FEATURE_COMPRESS_LZO (UINT64_C(1) << 3)
#define BT_FEATURE_COMPRESS_ZSTD (UINT64_C(1) << 4)
#define BT_FEATURE_BIG_METADATA (UINT64_C(1) << 5)
#define BT_FEATURE_EXTENDED_IREF (UINT64_C(1) << 6)
#define BT_FEATURE_SKINNY_METADATA (UINT64_C(1) << 8)
#define BT_FEATURE_NO_HOLES (UINT64_C(1) << 9)
#define BT_FEATURE_METADATA_UUID (UINT64_C(1) << 10)
#define BT_INCOMPAT_SUPPORTED                                                                      \
	(BT_FEATURE_MIXED_BACKREF | BT_FEATURE_DEFAULT_SUBVOL | BT_FEATURE_MIXED_GROUPS |          \
	    BT_FEATURE_COMPRESS_LZO | BT_FEATURE_COMPRESS_ZSTD | BT_FEATURE_BIG_METADATA |         \
	    BT_FEATURE_EXTENDED_IREF | BT_FEATURE_SKINNY_METADATA | BT_FEATURE_NO_HOLES |          \
	    BT_FEATURE_METADATA_UUID)
#define BT_SUPER_ERROR (UINT64_C(1) << 2)
#define BT_SUPER_SEEDING (UINT64_C(1) << 32)
#define BT_SUPER_METADUMP (UINT64_C(1) << 33)
#define BT_SUPER_METADUMP_V2 (UINT64_C(1) << 34)

enum bt_item_type {
	BT_INODE_ITEM = 1,
	BT_INODE_REF = 12,
	BT_INODE_EXTREF = 13,
	BT_XATTR_ITEM = 24,
	BT_DIR_ITEM = 84,
	BT_DIR_INDEX = 96,
	BT_EXTENT_DATA = 108,
	BT_EXTENT_CSUM = 128,
	BT_ROOT_ITEM = 132,
	BT_ROOT_BACKREF = 144,
	BT_CHUNK_ITEM = 228
};

enum bt_extent_type { BT_EXTENT_INLINE = 0, BT_EXTENT_REGULAR = 1, BT_EXTENT_PREALLOC = 2 };

struct bt_disk_key {
	struct bt_le64 objectid;
	uint8_t type;
	struct bt_le64 offset;
};

struct bt_disk_header {
	uint8_t csum[BT_CSUM_SIZE];
	uint8_t fsid[BTRFS_UUID_SIZE];
	struct bt_le64 bytenr;
	struct bt_le64 flags;
	uint8_t chunk_uuid[BTRFS_UUID_SIZE];
	struct bt_le64 generation;
	struct bt_le64 owner;
	struct bt_le32 count;
	uint8_t level;
};

struct bt_disk_item {
	struct bt_disk_key key;
	struct bt_le32 offset;
	struct bt_le32 size;
};

struct bt_disk_pointer {
	struct bt_disk_key key;
	struct bt_le64 bytenr;
	struct bt_le64 generation;
};

struct bt_disk_device {
	struct bt_le64 id, total_bytes, used_bytes;
	struct bt_le32 io_align, io_width, sector_size;
	struct bt_le64 type, generation, start_offset;
	struct bt_le32 group;
	uint8_t seek_speed, bandwidth;
	uint8_t uuid[BTRFS_UUID_SIZE], fsid[BTRFS_UUID_SIZE];
};

struct bt_disk_stripe {
	struct bt_le64 device, offset;
	uint8_t uuid[BTRFS_UUID_SIZE];
};

struct bt_disk_chunk {
	struct bt_le64 length, owner, stripe_length, type;
	struct bt_le32 io_align, io_width, sector_size;
	struct bt_le16 stripes, sub_stripes;
};

struct bt_disk_super {
	uint8_t csum[BT_CSUM_SIZE], fsid[BTRFS_UUID_SIZE];
	struct bt_le64 bytenr, flags;
	uint8_t magic[8];
	struct bt_le64 generation, root, chunk_root, log_root, unused_log_transid;
	struct bt_le64 total_bytes, used_bytes, root_dir, devices;
	struct bt_le32 sector_size, node_size, unused_leaf_size, stripe_size, system_array_size;
	struct bt_le64 chunk_generation, compat, compat_ro, incompat;
	struct bt_le16 checksum_type;
	uint8_t root_level, chunk_level, log_level;
	struct bt_disk_device device;
	char label[BTRFS_LABEL_SIZE];
	struct bt_le64 cache_generation, uuid_generation;
	uint8_t metadata_uuid[BTRFS_UUID_SIZE];
	struct bt_le64 global_roots, reserved[27];
	uint8_t system_array[BT_SYSTEM_ARRAY_SIZE];
	uint8_t backup_roots[672], padding[565];
};

struct bt_disk_time {
	struct bt_le64 seconds;
	struct bt_le32 nanoseconds;
};

struct bt_disk_inode {
	struct bt_le64 generation, transid, size, nbytes, block_group;
	struct bt_le32 links, uid, gid, mode;
	struct bt_le64 device, flags, sequence, reserved[4];
	struct bt_disk_time atime, ctime, mtime, otime;
};

struct bt_disk_root {
	struct bt_disk_inode inode;
	struct bt_le64 generation, root_dir, bytenr, byte_limit, used_bytes, last_snapshot, flags;
	struct bt_le32 refs;
	struct bt_disk_key drop_progress;
	uint8_t drop_level, level;
};

struct bt_disk_dir {
	struct bt_disk_key location;
	struct bt_le64 transid;
	struct bt_le16 data_length, name_length;
	uint8_t type;
};

struct bt_disk_inode_ref {
	struct bt_le64 index;
	struct bt_le16 name_length;
};

struct bt_disk_root_ref {
	struct bt_le64 directory, index;
	struct bt_le16 name_length;
};

struct bt_disk_extent_header {
	struct bt_le64 generation, ram_bytes;
	uint8_t compression, encryption;
	struct bt_le16 encoding;
	uint8_t type;
};

struct bt_disk_extent {
	struct bt_disk_extent_header header;
	struct bt_le64 disk_bytenr, disk_bytes, offset, length;
};

_Static_assert(sizeof(struct bt_disk_super) == BT_SUPER_SIZE, "superblock layout");
_Static_assert(offsetof(struct bt_disk_super, system_array) == 811, "system array layout");
_Static_assert(sizeof(struct bt_disk_key) == 17, "key layout");
_Static_assert(sizeof(struct bt_disk_header) == 101, "tree header layout");
_Static_assert(sizeof(struct bt_disk_chunk) == 48, "chunk layout");
_Static_assert(sizeof(struct bt_disk_inode) == 160, "inode layout");
_Static_assert(sizeof(struct bt_disk_root) == 239, "legacy root layout");
_Static_assert(sizeof(struct bt_disk_extent_header) == 21, "inline extent layout");

#endif
