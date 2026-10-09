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
#define BT_SUPER_MIRRORS 3U
#define BT_SUPER_MIRROR_BASE UINT64_C(16384)
#define BT_SUPER_MIRROR_SHIFT 12U
#define BT_CSUM_SIZE 32U
/* Algorithms of the superblock's checksum_type; each checksum occupies the
 * first bytes of a BT_CSUM_SIZE field. */
#define BT_CHECKSUM_CRC32C 0U
#define BT_CHECKSUM_XXHASH 1U
#define BT_CHECKSUM_SHA256 2U
#define BT_CHECKSUM_BLAKE2 3U
#define BT_SYSTEM_ARRAY_SIZE 2048U
#define BT_MAX_LEVEL 8U
#define BT_MAX_NODE_SIZE 65536U
/* Chunks of one filesystem; the table holds the chunks actually present,
 * starting from BT_INITIAL_CHUNKS entries. */
#define BT_MAX_CHUNKS 1048576U
#define BT_INITIAL_CHUNKS 4U
#define BT_MAX_COMPRESSED_SIZE (128U * 1024U)
#define BT_READ_WINDOW (1024U * 1024U)
#define BT_STRIPE_LENGTH UINT64_C(65536)
#define BT_MAX_TREE_ITEMS UINT64_C(1048576)
#define BT_MAGIC "_BHRfS_M"
#define BT_ROOT_TREE UINT64_C(1)
#define BT_EXTENT_TREE UINT64_C(2)
#define BT_CHUNK_TREE UINT64_C(3)
#define BT_CSUM_TREE UINT64_C(7)
#define BT_QUOTA_TREE UINT64_C(8)
#define BT_FREE_SPACE_TREE UINT64_C(10)
/* Block-group items, when the BLOCK_GROUP_TREE feature moves them out of the
 * extent tree. */
#define BT_BLOCK_GROUP_TREE UINT64_C(11)
/* Root-tree items (objectid, 0, block group) naming a v1 space-cache inode. */
#define BT_FREE_SPACE_OBJECTID (UINT64_MAX - UINT64_C(10))
/* Root items (objectid, ROOT_ITEM, file tree) of the relocation trees Linux's
 * balance keeps until it merges them into their file trees. */
#define BT_TREE_RELOC (UINT64_MAX - UINT64_C(7))
#define BT_DATA_RELOC_TREE (UINT64_MAX - UINT64_C(8))
#define BT_DEV_TREE UINT64_C(4)
#define BT_UUID_TREE UINT64_C(9)
#define BT_FIRST_CHUNK_OBJECTID UINT64_C(256)
#define BT_DEV_ITEMS_OBJECTID UINT64_C(1)
/* Linux leaves the first MiB of every device unallocated. */
#define BT_DEVICE_RESERVED (UINT64_C(1) << 20)
#define BT_ROOT_DIR_OBJECTID UINT64_C(6)
#define BT_EMPTY_SUBVOLUME_MODE 0755U
#define BT_CSUM_OBJECTID (UINT64_MAX - UINT64_C(9))
#define BT_LAST_FREE_OBJECTID (UINT64_MAX - UINT64_C(255))
#define BT_ORPHAN_OBJECTID (UINT64_MAX - UINT64_C(4))
/* Owner of every tree-log block, and the objectid of each subvolume log's
 * root item in the log root tree. */
#define BT_TREE_LOG_OBJECTID (UINT64_MAX - UINT64_C(5))
#define BT_DIR_START_INDEX UINT64_C(2)
#define BT_INODE_NODATACOW (UINT64_C(1) << 1)
#define BT_INODE_NOCOMPRESS (UINT64_C(1) << 3)
/* The inode has preallocated extents (Linux's BTRFS_INODE_PREALLOC). */
#define BT_INODE_PREALLOC (UINT64_C(1) << 4)
#define BT_INODE_SYNC (UINT64_C(1) << 5)
#define BT_INODE_IMMUTABLE (UINT64_C(1) << 6)
#define BT_INODE_APPEND (UINT64_C(1) << 7)
#define BT_INODE_NODUMP (UINT64_C(1) << 8)
#define BT_INODE_NOATIME (UINT64_C(1) << 9)
#define BT_INODE_DIRSYNC (UINT64_C(1) << 10)
#define BT_INODE_COMPRESS (UINT64_C(1) << 11)
/* The upper half of an inode item's flags holds Linux's read-only flags:
 * BTRFS_INODE_RO_VERITY marks a file whose fs-verity metadata is complete. */
#define BT_INODE_RO_VERITY (UINT64_C(1) << 32)
#define BT_LINK_MAX 65535U
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
/* Simple quotas: data extents name their owning subvolume. */
#define BT_FEATURE_SIMPLE_QUOTA (UINT64_C(1) << 16)
#define BT_INCOMPAT_SUPPORTED                                                                      \
	(BT_FEATURE_MIXED_BACKREF | BT_FEATURE_DEFAULT_SUBVOL | BT_FEATURE_MIXED_GROUPS |          \
	    BT_FEATURE_COMPRESS_LZO | BT_FEATURE_COMPRESS_ZSTD | BT_FEATURE_BIG_METADATA |         \
	    BT_FEATURE_EXTENDED_IREF | BT_FEATURE_SKINNY_METADATA | BT_FEATURE_NO_HOLES |          \
	    BT_FEATURE_METADATA_UUID | BT_FEATURE_SIMPLE_QUOTA)
#define BT_SUPER_ERROR (UINT64_C(1) << 2)
#define BT_SUPER_SEEDING (UINT64_C(1) << 32)
#define BT_SUPER_METADUMP (UINT64_C(1) << 33)
#define BT_SUPER_METADUMP_V2 (UINT64_C(1) << 34)
#define BT_HEADER_WRITTEN UINT64_C(1)
#define BT_HEADER_RELOC (UINT64_C(1) << 1)
#define BT_HEADER_BACKREF_SHIFT 56U
#define BT_HEADER_MIXED_BACKREF (UINT64_C(1) << BT_HEADER_BACKREF_SHIFT)
#define BT_ROOT_SUBVOL_READ_ONLY UINT64_C(1)
/* A deleted subvolume's root item, until the cleaner drops it. */
#define BT_ROOT_SUBVOL_DEAD (UINT64_C(1) << 48)
/* Marks root items whose flags and byte limit are initialized. */
#define BT_INODE_ROOT_ITEM_INIT (UINT64_C(1) << 31)
/* Tree ids are qgroup ids of level zero: below 2^48. */
#define BT_ROOT_ID_LIMIT (UINT64_C(1) << 48)
#define BT_BACKUP_ROOTS 4U
#define BT_EXTENT_FLAG_DATA UINT64_C(1)
#define BT_EXTENT_FLAG_TREE UINT64_C(2)
#define BT_EXTENT_FLAG_FULL_BACKREF (UINT64_C(1) << 8)
#define BT_COMPAT_RO_FREE_SPACE_TREE (UINT64_C(1) << 0)
#define BT_COMPAT_RO_FREE_SPACE_TREE_VALID (UINT64_C(1) << 1)
/* Files with fs-verity metadata exist. */
#define BT_COMPAT_RO_VERITY (UINT64_C(1) << 2)
#define BT_COMPAT_RO_BLOCK_GROUP_TREE (UINT64_C(1) << 3)
#define BT_QGROUP_STATUS_VERSION UINT64_C(1)
#define BT_QGROUP_STATUS_ON (UINT64_C(1) << 0)
#define BT_QGROUP_STATUS_RESCAN (UINT64_C(1) << 1)
#define BT_QGROUP_STATUS_INCONSISTENT (UINT64_C(1) << 2)
#define BT_QGROUP_STATUS_SIMPLE (UINT64_C(1) << 3)
#define BT_QGROUP_LIMIT_MAX_REFERENCED (UINT64_C(1) << 0)
#define BT_QGROUP_LIMIT_MAX_EXCLUSIVE (UINT64_C(1) << 1)
/* A qgroup id: level in the top 16 bits, the subvolume (level 0) below. */
#define BT_QGROUP_LEVEL_SHIFT 48U
#define BT_FREE_SPACE_USING_BITMAPS UINT32_C(1)
#define BT_FREE_SPACE_BITMAP_BYTES 256U

enum bt_item_type {
	BT_INODE_ITEM = 1,
	BT_INODE_REF = 12,
	BT_INODE_EXTREF = 13,
	BT_XATTR_ITEM = 24,
	/* A file's fs-verity descriptor and Merkle tree bytes, keyed by byte
	 * offset (core/verity.c). */
	BT_VERITY_DESC_ITEM = 36,
	BT_VERITY_MERKLE_ITEM = 37,
	BT_ORPHAN_ITEM = 48,
	BT_DIR_LOG_ITEM = 60,
	BT_DIR_LOG_INDEX = 72,
	BT_DIR_ITEM = 84,
	BT_DIR_INDEX = 96,
	BT_EXTENT_DATA = 108,
	BT_EXTENT_CSUM = 128,
	BT_ROOT_ITEM = 132,
	BT_ROOT_BACKREF = 144,
	BT_ROOT_REF = 156,
	BT_EXTENT_ITEM = 168,
	BT_METADATA_ITEM = 169,
	/* A data extent's owning subvolume: its first inline item, which is no
	 * reference. */
	BT_EXTENT_OWNER_REF = 172,
	BT_TREE_BLOCK_REF = 176,
	BT_EXTENT_DATA_REF = 178,
	BT_SHARED_BLOCK_REF = 182,
	BT_SHARED_DATA_REF = 184,
	BT_BLOCK_GROUP_ITEM = 192,
	BT_FREE_SPACE_INFO = 198,
	BT_FREE_SPACE_EXTENT = 199,
	BT_FREE_SPACE_BITMAP = 200,
	BT_DEV_EXTENT = 204,
	BT_DEV_ITEM = 216,
	BT_CHUNK_ITEM = 228,
	BT_QGROUP_STATUS = 240,
	BT_QGROUP_INFO = 242,
	BT_QGROUP_LIMIT = 244,
	BT_QGROUP_RELATION = 246,
	BT_UUID_SUBVOL = 251,
	BT_UUID_RECEIVED_SUBVOL = 252
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

struct bt_disk_dev_extent {
	struct bt_le64 chunk_tree, chunk_objectid, chunk_offset, length;
	uint8_t chunk_tree_uuid[BTRFS_UUID_SIZE];
};

struct bt_disk_chunk {
	struct bt_le64 length, owner, stripe_length, type;
	struct bt_le32 io_align, io_width, sector_size;
	struct bt_le16 stripes, sub_stripes;
};

struct bt_disk_root_backup {
	struct bt_le64 tree, tree_generation, chunk, chunk_generation;
	struct bt_le64 extent, extent_generation, files, files_generation;
	struct bt_le64 device, device_generation, checksum, checksum_generation;
	struct bt_le64 total_bytes, used_bytes, devices, reserved64[4];
	uint8_t tree_level, chunk_level, extent_level, files_level, device_level, checksum_level;
	uint8_t reserved8[10];
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
	struct bt_disk_root_backup backup_roots[BT_BACKUP_ROOTS];
	uint8_t padding[565];
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

struct bt_disk_root_full {
	struct bt_disk_root legacy;
	struct bt_le64 generation_v2;
	uint8_t uuid[BTRFS_UUID_SIZE], parent_uuid[BTRFS_UUID_SIZE], received_uuid[BTRFS_UUID_SIZE];
	struct bt_le64 ctransid, otransid, stransid, rtransid;
	struct bt_disk_time ctime, otime, stime, rtime;
	struct bt_le64 reserved[8];
};

struct bt_disk_extent_item {
	struct bt_le64 refs, generation, flags;
};

struct bt_disk_inline_ref {
	uint8_t type;
	struct bt_le64 offset;
};

/* Follows the inline reference type byte, or forms a keyed item. */
struct bt_disk_data_ref {
	struct bt_le64 root, objectid, offset;
	struct bt_le32 count;
};

struct bt_disk_shared_data_ref {
	struct bt_le32 count;
};

/* Present after a non-skinny tree extent item, before its inline references. */
struct bt_disk_tree_block_info {
	struct bt_disk_key key;
	uint8_t level;
};

/* The quota tree's (0, QGROUP_STATUS, 0) item; enable_gen follows only in
 * newer items and only simple quotas use it. */
struct bt_disk_qgroup_status {
	struct bt_le64 version, generation, flags, rescan;
};

/* The status item of simple quotas: extents from enable_gen on count. */
struct bt_disk_qgroup_status_simple {
	struct bt_disk_qgroup_status status;
	struct bt_le64 enable_gen;
};

/* (inode, VERITY_DESC_ITEM, 0): the size of the fs-verity descriptor stored
 * in the VERITY_DESC items from offset 1 on. The reserved words would hold an
 * fscrypt IV; Linux reads neither them nor the encryption byte. */
struct bt_disk_verity_item {
	struct bt_le64 size;
	struct bt_le64 reserved[2];
	uint8_t encryption;
};

/* Linux's struct fsverity_descriptor; a builtin signature of sig_size bytes
 * may follow it. */
struct bt_disk_verity_descriptor {
	uint8_t version;
	uint8_t hash_algorithm;
	uint8_t log_blocksize;
	uint8_t salt_size;
	struct bt_le32 sig_size;
	struct bt_le64 data_size;
	uint8_t root_hash[64];
	uint8_t salt[32];
	uint8_t reserved[144];
};

#define BT_VERITY_VERSION 1U
#define BT_VERITY_HASH_SHA256 1U
#define BT_VERITY_HASH_SHA512 2U
#define BT_VERITY_SALT_MAX 32U
/* FS_VERITY_MAX_DESCRIPTOR_SIZE and FS_VERITY_MAX_LEVELS. */
#define BT_VERITY_DESCRIPTOR_MAX 16384U
#define BT_VERITY_LEVELS_MAX 8U
/* Merkle tree blocks are at least 1 KiB and at most a sector. */
#define BT_VERITY_LOG_BLOCK_MIN 10U
/* The descriptor's offset in VERITY_DESC items, after the size item. */
#define BT_VERITY_DESCRIPTOR_OFFSET 1U
/* Bytes Linux's write_key_bytes stores per verity item. */
#define BT_VERITY_ITEM_BYTES 2048U

/* (0, QGROUP_INFO, qgroup): the bytes a qgroup references and holds alone. */
struct bt_disk_qgroup_info {
	struct bt_le64 generation, referenced, referenced_compressed, exclusive,
	    exclusive_compressed;
};

/* (0, QGROUP_LIMIT, qgroup): limits on those numbers. */
struct bt_disk_qgroup_limit {
	struct bt_le64 flags, max_referenced, max_exclusive, reserved_referenced,
	    reserved_exclusive;
};

struct bt_disk_free_space_info {
	struct bt_le32 extent_count, flags;
};

struct bt_disk_block_group {
	struct bt_le64 used_bytes, chunk_objectid, flags;
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

/* An INODE_EXTREF entry; the item key's offset is btrfs_extref_hash. */
struct bt_disk_inode_extref {
	struct bt_le64 parent;
	struct bt_le64 index;
	struct bt_le16 name_length;
};

/* A DIR_LOG_INDEX item: the log is authoritative for directory index keys
 * from the item's key offset to end. */
struct bt_disk_dir_log {
	struct bt_le64 end;
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
_Static_assert(sizeof(struct bt_disk_root_backup) == 168, "backup root layout");
_Static_assert(offsetof(struct bt_disk_super, system_array) == 811, "system array layout");
_Static_assert(sizeof(struct bt_disk_key) == 17, "key layout");
_Static_assert(sizeof(struct bt_disk_header) == 101, "tree header layout");
_Static_assert(sizeof(struct bt_disk_inode_extref) == 18, "extended reference layout");
_Static_assert(sizeof(struct bt_disk_chunk) == 48, "chunk layout");
_Static_assert(sizeof(struct bt_disk_stripe) == 32, "stripe layout");
_Static_assert(sizeof(struct bt_disk_device) == 98, "device item layout");
_Static_assert(sizeof(struct bt_disk_dev_extent) == 48, "device extent layout");
_Static_assert(sizeof(struct bt_disk_inode) == 160, "inode layout");
_Static_assert(sizeof(struct bt_disk_root) == 239, "legacy root layout");
_Static_assert(sizeof(struct bt_disk_root_full) == 439, "root layout");
_Static_assert(sizeof(struct bt_disk_extent_item) == 24, "extent item layout");
_Static_assert(sizeof(struct bt_disk_inline_ref) == 9, "inline reference layout");
_Static_assert(sizeof(struct bt_disk_extent_header) == 21, "inline extent layout");
_Static_assert(sizeof(struct bt_disk_extent) == 53, "file extent layout");
_Static_assert(sizeof(struct bt_disk_data_ref) == 28, "data reference layout");
_Static_assert(sizeof(struct bt_disk_tree_block_info) == 18, "tree block info layout");
_Static_assert(sizeof(struct bt_disk_qgroup_status) == 32, "qgroup status layout");
_Static_assert(sizeof(struct bt_disk_qgroup_status_simple) == 40, "simple quota status layout");
_Static_assert(sizeof(struct bt_disk_qgroup_info) == 40, "qgroup info layout");
_Static_assert(sizeof(struct bt_disk_verity_item) == 25, "verity descriptor item layout");
_Static_assert(sizeof(struct bt_disk_verity_descriptor) == 256, "verity descriptor layout");
_Static_assert(sizeof(struct bt_disk_qgroup_limit) == 40, "qgroup limit layout");

#endif
