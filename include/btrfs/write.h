/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_WRITE_H
#define MACHLIN_BTRFS_WRITE_H

#include <btrfs/btrfs.h>

struct btrfs_transaction;

/* Separate from the immutable reader's environment: write authority is explicit.
 * The owner must hold exclusive resource access, drain readers before commit,
 * and retire the original btrfs_fs after a successful or uncertain commit.
 * A failed write/flush may have reached media; never reuse that writer or claim
 * rollback. flush must order and durably persist all preceding writes. */
struct btrfs_write_environment {
	void *context;
	enum btrfs_result (*write)(void *context, uint64_t offset, const void *bytes, size_t size);
	enum btrfs_result (*flush)(void *context);
};

#define BTRFS_SUPER_COPIES 3U

struct btrfs_super_copy {
	uint64_t offset;
	uint64_t generation;
	/* OK for a checksum-valid copy at its own offset; NOT_FOUND beyond the device. */
	enum btrfs_result status;
	/* Equal to the selected copy except for its offset and checksum. */
	int current;
};

struct btrfs_recovery_report {
	struct btrfs_super_copy copies[BTRFS_SUPER_COPIES];
	unsigned present;
	unsigned selected;
	uint64_t generation;
	unsigned rewritten;
};

/* Explicit, exclusive superblock recovery; mount never selects a mirror. Chooses
 * the newest checksum-valid copy and requires equal-generation copies to agree.
 * It never selects a generation below acknowledged (STALE), a pending tree log
 * (UNSUPPORTED) or a copy of another filesystem (CORRUPT). When copies disagree,
 * it first opens the selection's chunk, root, extent and device trees and its
 * allocation map. A NULL writer only inspects and then returns RECOVERY_REQUIRED.
 * With a writer, each disagreeing copy is rewritten from the selected copy, which
 * itself is never written, followed by one barrier. The report's status fields
 * describe the copies as found; current describes them after a successful call. */
enum btrfs_result btrfs_recover_supers(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t acknowledged,
    struct btrfs_recovery_report *report);

/* Current admission: CRC32C, skinny metadata, SINGLE/DUP, no free-space cache
 * tree, quotas or mixed groups. At least two superblock copies must exist and
 * agree with the mounted primary; otherwise begin returns RECOVERY_REQUIRED.
 * Changes are confined to unshared top-level tree paths. Unsupported layouts
 * return before any media write. Native adapters remain read-only until their
 * visibility and page-cache contracts are wired.
 *
 * Commit writes new metadata, barrier, secondary superblocks, barrier, primary,
 * barrier. Every crash point therefore retains a valid copy at the last
 * acknowledged generation or later; btrfs_recover_supers selects it. */
enum btrfs_result btrfs_transaction_begin(const struct btrfs_fs *base,
    const struct btrfs_write_environment *environment, struct btrfs_transaction **result);
/* Replace an existing, uncompressed inline regular file (including hardlinks).
 * No file creation, external extent conversion or implicit truncation of other
 * extents. Payloads are bounded by Btrfs's 2 KiB default inline-write policy. */
enum btrfs_result btrfs_transaction_write_inline(struct btrfs_transaction *transaction,
    struct btrfs_object_id id, const void *bytes, size_t size, struct btrfs_time modified);
/* Copy-on-write file data, for regular files without set-id bits, immutable or
 * append-only flags, on filesystems with NO_HOLES. Partially covered sectors
 * are read from this transaction's own view and rewritten; old extents keep
 * their references until commit drops them and are never reused before the
 * next transaction. New data stays in memory (at most 64 MiB per transaction)
 * and reaches media during commit before the first barrier. Checksums follow
 * the inode's NODATASUM flag. Inline files become regular extents. Writes past
 * an unaligned EOF clear the old EOF sector's tail; truncation clears the new
 * EOF sector's tail and drops coverage beyond it. */
enum btrfs_result btrfs_transaction_write(struct btrfs_transaction *transaction,
    struct btrfs_object_id id, uint64_t offset, const void *bytes, size_t size,
    struct btrfs_time modified);
enum btrfs_result btrfs_transaction_truncate(struct btrfs_transaction *transaction,
    struct btrfs_object_id id, uint64_t size, struct btrfs_time modified);

/* A new inode's attributes: mode includes the file type; credentials and
 * set-id inheritance are the caller's authorization decision. device is the
 * Linux kernel's dev_t (MAJOR << 20 | MINOR) of a character or block device.
 * The symlink target becomes an inline extent. */
struct btrfs_new_inode {
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	uint64_t device;
	struct btrfs_time time;
	const void *target;
	size_t target_length;
};

#define BTRFS_XATTR_CREATE 1
#define BTRFS_XATTR_REPLACE 2

/* Namespace operations update the inode reference, DIR_ITEM (packed with any
 * name-hash collisions), DIR_INDEX, link count, directory size and times of
 * every affected inode in one transaction, as Linux does. Names are raw bytes
 * without '/' or NUL; "." and ".." are refused, and names longer than
 * BTRFS_NAME_MAX or symlink targets beyond one inline extent or PATH_MAX are
 * NAME_TOO_LONG. Inode numbers and directory indexes continue from the tree's
 * highest and never repeat within one transaction; a transaction indexes a
 * bounded number of directories (UNSUPPORTED beyond it, before any change).
 * A name that would overflow its packed DIR_ITEM is RANGE (Linux's EOVERFLOW).
 * Entries naming subvolumes and operations across trees are refused
 * (CROSS_TREE). Linux's btrfs flag and compression-property inheritance applies
 * to new inodes. A failure after the first change poisons the transaction. */
enum btrfs_result btrfs_transaction_create(struct btrfs_transaction *transaction,
    struct btrfs_object_id parent, const void *name, size_t length,
    const struct btrfs_new_inode *attributes, struct btrfs_object_id *result);
enum btrfs_result btrfs_transaction_link(struct btrfs_transaction *transaction,
    struct btrfs_object_id id, struct btrfs_object_id parent, const void *name, size_t length,
    struct btrfs_time time);
/* Removes a name; directories must be empty. An inode losing its last name is
 * deleted with its data, unless it is still open: then an orphan item keeps it
 * until btrfs_transaction_evict, or orphan cleanup after a crash. */
enum btrfs_result btrfs_transaction_unlink(struct btrfs_transaction *transaction,
    struct btrfs_object_id parent, const void *name, size_t length, struct btrfs_time time,
    int open);
/* Atomic rename, replacing an existing target of a compatible type (an empty
 * directory for a directory); target_open keeps a replaced last name as an
 * orphan. A directory cannot move below itself. */
enum btrfs_result btrfs_transaction_rename(struct btrfs_transaction *transaction,
    struct btrfs_object_id old_parent, const void *old_name, size_t old_length,
    struct btrfs_object_id new_parent, const void *new_name, size_t new_length,
    struct btrfs_time time, int target_open);
/* Raw xattrs with Linux's CREATE/REPLACE semantics; name and value must fit one
 * leaf item (NO_SPACE). btrfs.compression is Linux's property: its value must
 * name a codec or be "no"/"none" on an inode with data checksums, an empty
 * value removes it, it updates the inode's compression flags (recording the
 * codec's incompat feature) and it is ignored on objects other than regular
 * files and directories. Other btrfs. names are invalid. */
enum btrfs_result btrfs_transaction_set_xattr(struct btrfs_transaction *transaction,
    struct btrfs_object_id id, const void *name, size_t name_length, const void *value,
    size_t value_length, int flags, struct btrfs_time time);
enum btrfs_result btrfs_transaction_remove_xattr(struct btrfs_transaction *transaction,
    struct btrfs_object_id id, const void *name, size_t name_length, struct btrfs_time time);
/* Deletes an orphaned inode after its last native reference closes. */
enum btrfs_result btrfs_transaction_evict(
    struct btrfs_transaction *transaction, struct btrfs_object_id id);
/* Deletes every orphaned unlinked inode of a tree, as Linux does at mount; an
 * orphan item of an inode that still has names, or of a missing inode, is
 * dropped. */
enum btrfs_result btrfs_transaction_clean_orphans(
    struct btrfs_transaction *transaction, uint64_t tree, size_t *cleaned);
enum btrfs_result btrfs_transaction_commit(struct btrfs_transaction *transaction);
void btrfs_transaction_destroy(struct btrfs_transaction *transaction);

#endif
