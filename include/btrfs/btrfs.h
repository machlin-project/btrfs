/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_H
#define MACHLIN_BTRFS_H

#include <stddef.h>
#include <stdint.h>

#define BTRFS_UUID_SIZE 16U
#define BTRFS_NAME_MAX 255U
#define BTRFS_LABEL_SIZE 256U
#define BTRFS_TOP_LEVEL_TREE UINT64_C(5)
#define BTRFS_ROOT_INODE UINT64_C(256)
/* A subvolume entry whose subvolume this tree does not reference (copied by
 * a snapshot, or deleted) resolves, as on Linux, to an empty read-only stub
 * directory with this inode number in the containing tree. */
#define BTRFS_EMPTY_SUBVOLUME_INODE UINT64_C(2)
#define BTRFS_COOKIE_END UINT64_MAX

enum btrfs_result {
	BTRFS_OK = 0,
	BTRFS_INVALID_ARGUMENT,
	BTRFS_NOT_BTRFS,
	BTRFS_UNSUPPORTED,
	BTRFS_CORRUPT,
	BTRFS_IO,
	BTRFS_NO_MEMORY,
	BTRFS_NOT_FOUND,
	BTRFS_NOT_DIRECTORY,
	BTRFS_IS_DIRECTORY,
	BTRFS_RANGE,
	BTRFS_READ_ONLY,
	BTRFS_RECOVERY_REQUIRED,
	BTRFS_STALE,
	BTRFS_EXISTS,
	BTRFS_NO_SPACE,
	BTRFS_NOT_EMPTY,
	BTRFS_CROSS_TREE,
	BTRFS_TOO_MANY_LINKS,
	BTRFS_NAME_TOO_LONG,
	BTRFS_NOT_PERMITTED
};

enum btrfs_compression {
	BTRFS_COMPRESSION_NONE = 0,
	BTRFS_COMPRESSION_ZLIB = 1,
	BTRFS_COMPRESSION_LZO = 2,
	BTRFS_COMPRESSION_ZSTD = 3
};

enum btrfs_mode {
	BTRFS_MODE_TYPE = 0170000,
	BTRFS_MODE_REGULAR = 0100000,
	BTRFS_MODE_DIRECTORY = 0040000,
	BTRFS_MODE_SYMLINK = 0120000,
	BTRFS_MODE_CHARACTER = 0020000,
	BTRFS_MODE_BLOCK = 0060000,
	BTRFS_MODE_FIFO = 0010000,
	BTRFS_MODE_SOCKET = 0140000
};

enum btrfs_file_type {
	BTRFS_FT_UNKNOWN = 0,
	BTRFS_FT_REGULAR = 1,
	BTRFS_FT_DIRECTORY = 2,
	BTRFS_FT_CHARACTER = 3,
	BTRFS_FT_BLOCK = 4,
	BTRFS_FT_FIFO = 5,
	BTRFS_FT_SOCKET = 6,
	BTRFS_FT_SYMLINK = 7,
	BTRFS_FT_XATTR = 8
};

struct btrfs_fs;
struct btrfs_directory;

/* The resource is immutable for the mount lifetime. read completes exactly
 * length bytes or fails. Callbacks must support concurrent independent reads.
 * The caller drains operations before unmount and retains context until then.
 * No callback in this interface can authorize a write. */
struct btrfs_cache;

struct btrfs_environment {
	void *context;
	uint64_t size_bytes;
	enum btrfs_result (*read)(void *context, uint64_t offset, void *buffer, size_t length);
	void *(*allocate)(void *context, size_t size);
	void (*release)(void *context, void *allocation, size_t size);
	/* Optional codec service. Produce exactly output_size bytes from one bounded
	 * stream; reject malformed/truncated input. Sector padding may follow it.
	 * The core checks stored data before invoking the codec. */
	enum btrfs_result (*decompress)(void *context, enum btrfs_compression codec,
	    const void *input, size_t input_size, void *output, size_t output_size);
	/* Optional verified tree-node cache shared by the owner's mounts of this
	 * device (btrfs_cache_create). */
	struct btrfs_cache *cache;
};

/* Mutual exclusion for a cache shared by threads; NULL callbacks mean one
 * thread at a time uses it. */
struct btrfs_cache_locks {
	void *context;
	void (*lock)(void *context);
	void (*unlock)(void *context);
};

/* A bounded cache of tree nodes that passed verification, keyed by address,
 * generation and level, which together name one immutable node: a block is
 * rewritten only after it is freed, and a reused address carries a newer
 * generation. A hit skips the device read and the checksum; the caller's
 * owner and level expectations are still checked. The cache serves one device
 * that changes only through its owner's commits, so it must be destroyed when
 * the device is changed otherwise (another writer, recovery to an older
 * generation, a rewound test device). bytes bounds the node storage. */
enum btrfs_result btrfs_cache_create(const struct btrfs_environment *environment,
    const struct btrfs_cache_locks *locks, size_t bytes, struct btrfs_cache **result);
void btrfs_cache_destroy(struct btrfs_cache *cache);
void btrfs_cache_counts(const struct btrfs_cache *cache, uint64_t *hits, uint64_t *misses);

struct btrfs_object_id {
	uint64_t tree;
	uint64_t inode;
};

struct btrfs_time {
	int64_t seconds;
	uint32_t nanoseconds;
};

/* Linux inode flags (btrfs_inode.flags) that native policy maps. */
#define BTRFS_INODE_FLAG_IMMUTABLE (UINT64_C(1) << 6)
#define BTRFS_INODE_FLAG_APPEND (UINT64_C(1) << 7)
#define BTRFS_INODE_FLAG_NODUMP (UINT64_C(1) << 8)

struct btrfs_inode {
	struct btrfs_object_id id;
	uint64_t generation;
	uint64_t size;
	uint64_t allocated_bytes;
	uint64_t flags;
	uint64_t device;
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	uint32_t links;
	struct btrfs_time access_time;
	struct btrfs_time modify_time;
	struct btrfs_time change_time;
	struct btrfs_time birth_time;
};

struct btrfs_info {
	uint8_t uuid[BTRFS_UUID_SIZE];
	char label[BTRFS_LABEL_SIZE];
	uint64_t generation;
	uint64_t total_bytes;
	uint64_t used_bytes;
	uint64_t incompat_features;
	uint64_t readonly_features;
	uint64_t default_tree;
	uint32_t sector_size;
	uint32_t node_size;
};

struct btrfs_dir_entry {
	struct btrfs_object_id id;
	uint16_t name_length;
	uint8_t type;
	uint8_t name[BTRFS_NAME_MAX];
};

/* tree == 0 selects the on-disk default subvolume; 5 selects the top level.
 * Mount validates the root inode, and publishes *result only on success. */
enum btrfs_result btrfs_mount(
    const struct btrfs_environment *environment, uint64_t tree, struct btrfs_fs **result);
void btrfs_unmount(struct btrfs_fs *fs);
void btrfs_get_info(const struct btrfs_fs *fs, struct btrfs_info *info);
enum btrfs_result btrfs_root(const struct btrfs_fs *fs, struct btrfs_inode *inode);
enum btrfs_result btrfs_get_inode(
    const struct btrfs_fs *fs, struct btrfs_object_id id, struct btrfs_inode *inode);
enum btrfs_result btrfs_parent(
    const struct btrfs_fs *fs, const struct btrfs_inode *directory, struct btrfs_inode *parent);
enum btrfs_result btrfs_lookup(const struct btrfs_fs *fs, const struct btrfs_inode *directory,
    const void *name, size_t length, struct btrfs_inode *inode);
/* Cookies are next DIR_INDEX keys, not array positions. Start at zero; end is
 * BTRFS_NOT_FOUND. A failed call leaves the cookie unchanged. No dot entries. */
enum btrfs_result btrfs_next_dir(const struct btrfs_fs *fs, const struct btrfs_inode *directory,
    uint64_t *cookie, struct btrfs_dir_entry *entry);
/* Streamed enumeration retains its tree path. A stream has one owner, and the
 * filesystem must outlive it. next_cookie becomes valid only on success. */
enum btrfs_result btrfs_directory_open(const struct btrfs_fs *fs,
    const struct btrfs_inode *directory, uint64_t cookie, struct btrfs_directory **stream);
enum btrfs_result btrfs_directory_next(
    struct btrfs_directory *stream, struct btrfs_dir_entry *entry, uint64_t *next_cookie);
void btrfs_directory_close(struct btrfs_directory *stream);
/* completed is always initialized, including on error. Only the reported prefix
 * is valid. Regular extents verify complete sectors before copying any bytes. */
enum btrfs_result btrfs_read(const struct btrfs_fs *fs, const struct btrfs_inode *inode,
    uint64_t offset, void *buffer, size_t length, size_t *completed);
/* A NULL buffer queries length. RANGE reports required size without partial data.
 * list_xattrs returns packed NUL-terminated raw Linux names. No policy translation. */
enum btrfs_result btrfs_get_xattr(const struct btrfs_fs *fs, const struct btrfs_inode *inode,
    const void *name, size_t name_length, void *buffer, size_t capacity, size_t *length);
enum btrfs_result btrfs_list_xattrs(const struct btrfs_fs *fs, const struct btrfs_inode *inode,
    void *buffer, size_t capacity, size_t *length);
const char *btrfs_result_string(enum btrfs_result result);
uint32_t btrfs_mode_for_type(uint8_t type);

#endif
