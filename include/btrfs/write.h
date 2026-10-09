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
	/* Optional: compresses input into one stream of codec in output, as Linux
	 * stores a compressed extent, reporting its size; RANGE when it would not
	 * fit capacity. Without it no data is compressed. */
	enum btrfs_result (*compress)(void *context, enum btrfs_compression codec,
	    const void *input, size_t input_size, void *output, size_t capacity, size_t *size);
	/* The compress mount option's codec, or NONE: files ask for compression
	 * with their property or COMPRESS flag only. */
	enum btrfs_compression compression;
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

/* Current admission: any of Linux's four checksum algorithms, skinny metadata,
 * SINGLE/DUP, no free-space cache tree, quotas or mixed groups. At least two
 * superblock copies must exist and agree with the mounted primary; otherwise
 * begin returns RECOVERY_REQUIRED. Changes are confined to unshared top-level
 * tree paths. Unsupported layouts return before any media write. Native
 * adapters open read-write only on request, and run btrfs_recover_supers first
 * when admission returns RECOVERY_REQUIRED.
 *
 * Commit writes new metadata, barrier, secondary superblocks, barrier, primary,
 * barrier. Every crash point therefore retains a valid copy at the last
 * acknowledged generation or later; btrfs_recover_supers selects it. */
enum btrfs_result btrfs_transaction_begin(const struct btrfs_fs *base,
    const struct btrfs_write_environment *environment, struct btrfs_transaction **result);
/* Inode numbers and directory indexes that stay unique across the transactions
 * of one mount, as Linux keeps each root's highest inode number and each
 * cached directory's next index (index_cnt) in memory. Without counters a
 * transaction continues after the highest number in its base tree, so a number
 * whose inode or entry was removed and committed may be handed out again, as
 * Linux does after evicting the inode or at the next mount. Directory counters
 * live in a bounded table that forgets every directory when it fills, as
 * eviction would; trees beyond its tree capacity fall back to the base tree.
 * Counters advance as numbers are handed out, also in transactions that are
 * later aborted. Every transaction of a mounted volume uses the same counters,
 * one transaction at a time. */
struct btrfs_counters;

enum btrfs_result btrfs_counters_create(
    const struct btrfs_environment *environment, struct btrfs_counters **result);
void btrfs_counters_destroy(struct btrfs_counters *counters);
/* Attaches counters before the transaction's first namespace change. */
enum btrfs_result btrfs_transaction_use_counters(
    struct btrfs_transaction *transaction, struct btrfs_counters *counters);
/* The allocator state of the last committed generation, which a mounted
 * volume keeps across its transactions: a transaction begun on that
 * generation copies it instead of loading and verifying the extent,
 * free-space and device trees again, and a successful commit updates it; any
 * other base is verified as usual and saved. counts reports the full loads
 * (scans) and the reuses so far. One transaction at a time uses a map. */
struct btrfs_allocation_map;

enum btrfs_result btrfs_allocation_map_create(
    const struct btrfs_environment *environment, struct btrfs_allocation_map **result);
void btrfs_allocation_map_destroy(struct btrfs_allocation_map *map);
void btrfs_allocation_map_counts(
    const struct btrfs_allocation_map *map, uint64_t *scans, uint64_t *reuses);
enum btrfs_result btrfs_transaction_begin_mapped(const struct btrfs_fs *base,
    const struct btrfs_write_environment *environment, struct btrfs_allocation_map *map,
    struct btrfs_transaction **result);
/* Replace an existing, uncompressed inline regular file (including hardlinks).
 * No file creation, external extent conversion or implicit truncation of other
 * extents. Payloads are bounded by Btrfs's 2 KiB default inline-write policy. */
enum btrfs_result btrfs_transaction_write_inline(struct btrfs_transaction *transaction,
    struct btrfs_object_id id, const void *bytes, size_t size, struct btrfs_time modified);
/* File data of regular files. Immutable files refuse writes and append-only
 * files accept data only at their end (NOT_PERMITTED); set-id files need a
 * privilege decision. Partially covered sectors are read from this
 * transaction's own view and rewritten; data is copied on write except into
 * preallocated and unshared NODATACOW extents, which are written in place as
 * Linux does. Old extents keep their references until commit drops them and
 * are never reused before the next transaction. New data reaches the device as
 * its extents are created; the commit's first barrier makes it durable before
 * any metadata names it. Checksums follow the inode's NODATASUM flag; files
 * compress as Linux decides. Inline files become regular extents. Writes past
 * an unaligned EOF clear the old EOF sector's tail; truncation clears the new
 * EOF sector's tail and drops coverage beyond it. Without NO_HOLES, a file
 * grown by a write or truncation has the new range covered by hole items. */
enum btrfs_result btrfs_transaction_write(struct btrfs_transaction *transaction,
    struct btrfs_object_id id, uint64_t offset, const void *bytes, size_t size,
    struct btrfs_time modified);
/* Tree nodes one step of a truncation, eviction or orphan cleanup changes,
 * counting those its dropped references change at commit (the step may
 * exceed it by one item's edit). Native writers commit between steps as the
 * room of a releasing operation requires. */
#define BTRFS_RELEASE_STEP_NODES 64U
/* Shrinking removes the items beyond the new EOF from the end of the file in
 * steps whose work reaches budget, as Linux's truncation does; a step that
 * leaves work stores the size it reached, a valid shorter file in every
 * committed state, and *done is 0 until the file has size bytes. Growing
 * completes in one step. */
enum btrfs_result btrfs_transaction_truncate(struct btrfs_transaction *transaction,
    struct btrfs_object_id id, uint64_t size, struct btrfs_time modified, size_t budget, int *done);

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
/* O_TMPFILE, as Linux's btrfs_tmpfile: a regular file inheriting from parent
 * like a created one, with no link and an orphan item, so that eviction or
 * orphan cleanup deletes it unless btrfs_transaction_link_tmpfile names it
 * first; that link removes the orphan item (btrfs_link). Linking an inode
 * without links that is not such a file is INVALID_ARGUMENT, and plain
 * btrfs_transaction_link of it NOT_FOUND. Adapters enforce Linux's
 * I_LINKABLE rule (O_EXCL tmpfiles never gain a name). */
enum btrfs_result btrfs_transaction_create_tmpfile(struct btrfs_transaction *transaction,
    struct btrfs_object_id parent, const struct btrfs_new_inode *attributes,
    struct btrfs_object_id *result);
enum btrfs_result btrfs_transaction_link_tmpfile(struct btrfs_transaction *transaction,
    struct btrfs_object_id id, struct btrfs_object_id parent, const void *name, size_t length,
    struct btrfs_time time);
/* Removes a name; directories must be empty. An inode losing its last name is
 * deleted with its data when that takes a small bounded amount of work; while
 * it is still open, or when its deletion needs more, an orphan item keeps it
 * until btrfs_transaction_evict, or orphan cleanup after a crash. The caller
 * evicts an unopened one that btrfs_transaction_take_deferred returns. */
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
/* renameat2 RENAME_WHITEOUT: the rename, then a whiteout under the old name,
 * a character device 0:0 without permission bits owned by uid and gid (the
 * caller's credentials). Two names of one inode change nothing. */
enum btrfs_result btrfs_transaction_rename_whiteout(struct btrfs_transaction *transaction,
    struct btrfs_object_id old_parent, const void *old_name, size_t old_length,
    struct btrfs_object_id new_parent, const void *new_name, size_t new_length, uint32_t uid,
    uint32_t gid, struct btrfs_time time, int target_open);
/* renameat2 RENAME_EXCHANGE: both names exist and each then names the other
 * inode, of any types, across directories of one tree; a directory cannot
 * move below itself (INVALID_ARGUMENT). Subvolume entries are refused
 * (CROSS_TREE). */
enum btrfs_result btrfs_transaction_exchange(struct btrfs_transaction *transaction,
    struct btrfs_object_id old_parent, const void *old_name, size_t old_length,
    struct btrfs_object_id new_parent, const void *new_name, size_t new_length,
    struct btrfs_time time);
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
/* Inode attributes changed together with the change time (ctime). mode holds
 * permission bits only. REMOVE_CAPABILITY removes security.capability in the
 * same transaction, as Linux's ATTR_KILL_PRIV after chown. Which set-id bits a
 * change keeps is the caller's policy: the core stores the mode it is given.
 * Immutable and append-only inodes refuse these changes (NOT_PERMITTED). */
#define BTRFS_ATTRIBUTE_MODE 1U
#define BTRFS_ATTRIBUTE_UID 2U
#define BTRFS_ATTRIBUTE_GID 4U
#define BTRFS_ATTRIBUTE_ACCESS_TIME 8U
#define BTRFS_ATTRIBUTE_MODIFY_TIME 16U
#define BTRFS_ATTRIBUTE_REMOVE_CAPABILITY 32U

struct btrfs_attributes {
	unsigned mask;
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	struct btrfs_time access_time;
	struct btrfs_time modify_time;
	struct btrfs_time time;
};
enum btrfs_result btrfs_transaction_set_attributes(struct btrfs_transaction *transaction,
    struct btrfs_object_id id, const struct btrfs_attributes *attributes);
/* Writing or truncating a regular file with S_ISUID, S_ISGID and group
 * execution, or security.capability needs the caller's decision first
 * (UNSUPPORTED otherwise). drop_privileges is Linux's file_remove_privs for a
 * writer without CAP_FSETID: it clears those bits and removes the capability
 * now. keep_privileges records a privileged writer for this transaction; at
 * most 64 inodes per transaction. */
enum btrfs_result btrfs_transaction_drop_privileges(
    struct btrfs_transaction *transaction, struct btrfs_object_id id, struct btrfs_time time);
enum btrfs_result btrfs_transaction_keep_privileges(
    struct btrfs_transaction *transaction, struct btrfs_object_id id);
/* Creates an empty subvolume named name in directory parent, as Linux's
 * create_subvol: a new tree whose root directory (inode 256) has the caller's
 * directory mode, owner and time, its root item, root references, a UUID
 * tree entry for uuid (the caller's random UUID) and the directory entry. The
 * root directory inherits the parent subvolume's compression property, not
 * inode flags. result receives the new tree id. */
enum btrfs_result btrfs_transaction_create_subvolume(struct btrfs_transaction *transaction,
    struct btrfs_object_id parent, const void *name, size_t length,
    const struct btrfs_new_inode *attributes, const uint8_t uuid[BTRFS_UUID_SIZE],
    uint64_t *result);
/* Snapshots subvolume source into directory parent, as Linux's
 * create_pending_snapshot: the snapshot's root node is a copy of the source's,
 * every block below it gains a reference, both root items record this
 * transaction as their last snapshot, and the snapshot's parent UUID names the
 * source. A read-only snapshot keeps the source's received UUID. A source
 * changed earlier in this transaction is refused (UNSUPPORTED): its new
 * blocks have no references yet. */
enum btrfs_result btrfs_transaction_snapshot(struct btrfs_transaction *transaction, uint64_t source,
    struct btrfs_object_id parent, const void *name, size_t length, int read_only,
    struct btrfs_time time, const uint8_t uuid[BTRFS_UUID_SIZE], uint64_t *result);
/* Deletes the subvolume named name in directory parent, as Linux's
 * btrfs_delete_subvolume: the entry and root references go, the root item
 * keeps refs 0 and the dead flag, an orphan item hands the tree to
 * btrfs_transaction_clean_subvolumes, and its UUID tree entries go. The
 * default subvolume is NOT_PERMITTED, a subvolume holding subvolumes is
 * NOT_EMPTY, a name that is not a subvolume entry INVALID_ARGUMENT, and a
 * subvolume this transaction opened UNSUPPORTED. A stub entry (copied by a
 * snapshot) is removed alone. Adapters keep mounted or busy subvolumes. */
enum btrfs_result btrfs_transaction_delete_subvolume(struct btrfs_transaction *transaction,
    struct btrfs_object_id parent, const void *name, size_t length, struct btrfs_time time);
/* Removes block groups that held nothing when the transaction began and that
 * it has not allocated from, as Linux's cleaner does with
 * btrfs_delete_unused_bgs (a group emptied in this transaction waits for the
 * next one). The last group of each type and profile stays, and so does a group
 * with a v1 space-cache inode. Their chunk and device extents, block group and
 * free-space items go at commit; their device space is free from the next
 * transaction on. removed counts them. */
enum btrfs_result btrfs_transaction_remove_unused_groups(
    struct btrfs_transaction *transaction, size_t *removed);
/* Drops deleted subvolumes, as Linux's cleaner does with btrfs_drop_snapshot:
 * references of their blocks and file extents go, blocks other trees share
 * are first converted to parent references when the deleted tree owns them,
 * and unshared blocks and data are freed. At most budget tree blocks are
 * visited per call; the root item records the drop progress, so a later
 * transaction resumes there. A fully dropped subvolume loses its root item
 * and orphan item. dropped counts them; pending reports remaining work. */
enum btrfs_result btrfs_transaction_clean_subvolumes(
    struct btrfs_transaction *transaction, size_t budget, size_t *dropped, int *pending);
/* Deletes an orphaned inode after its last native reference closes, in steps
 * whose work reaches budget: its data from the end of the file, then its other
 * items and its orphan item. *done is 0 while work remains. */
enum btrfs_result btrfs_transaction_evict(
    struct btrfs_transaction *transaction, struct btrfs_object_id id, size_t budget, int *done);
/* An unopened inode whose deletion an unlink or rename of this transaction
 * left to btrfs_transaction_evict; NOT_FOUND when none remains. Each is taken
 * once; one left in a committed transaction is an orphan for the next mount's
 * cleanup. */
enum btrfs_result btrfs_transaction_take_deferred(
    struct btrfs_transaction *transaction, struct btrfs_object_id *id);
/* Deletes the orphaned unlinked inodes of a tree, as Linux does at mount, with
 * work up to budget; an orphan item of an inode that still has names, or of a
 * missing inode, is dropped. *pending reports that orphans remain. */
enum btrfs_result btrfs_transaction_clean_orphans(struct btrfs_transaction *transaction,
    uint64_t tree, size_t budget, size_t *cleaned, int *pending);
enum btrfs_result btrfs_transaction_commit(struct btrfs_transaction *transaction);
/* Commits and returns an independently owned, immutable view of the published
 * state, without mounting the device again. tree has btrfs_mount's meaning.
 * All view storage and root validation are prepared before publication; *view
 * stays NULL on failure or an unchanged transaction. Unmount a returned view
 * after its readers retire; it does not borrow the transaction's storage. */
enum btrfs_result btrfs_transaction_commit_view(
    struct btrfs_transaction *transaction, uint64_t tree, struct btrfs_fs **view);
void btrfs_transaction_destroy(struct btrfs_transaction *transaction);
/* OK while the transaction can still commit; otherwise the error that made it
 * unusable (an operation failed after its first change) or READ_ONLY after
 * commit. A refused operation leaves it usable. */
enum btrfs_result btrfs_transaction_failure(const struct btrfs_transaction *transaction);
/* The transaction's current state for readers: lookups, attributes,
 * directory streams, extended attributes and data reads see every operation
 * applied so far. Its nodes come from the transaction or, when unchanged, the
 * device and the node cache; it uses the base environment's allocator, so
 * readers may share it with each other but not with an operation in progress,
 * and must finish (closing their streams) before the next operation. The view
 * stays valid until the next call, commit or destroy; NULL when the
 * transaction is unusable or finished. */
const struct btrfs_fs *btrfs_transaction_reader(struct btrfs_transaction *transaction);
/* Whether one more operation that changes at most nodes tree nodes (its
 * caller's bound) fits this transaction while leaving its commit room: half
 * of each per-transaction limit (changed nodes and the nodes queued reference
 * changes will change, file trees, directory index, privilege and deferred
 * eviction slots, queued references) and free metadata space for twice that
 * work are kept for the commit's own accounting, and a reserve for operations
 * that release space. Data written afterwards leaves the device space this
 * metadata may need. NO_SPACE means the caller should commit first; for an
 * empty transaction it means the operation does not fit at all. */
enum btrfs_result btrfs_transaction_room(struct btrfs_transaction *transaction, size_t nodes);
/* The same for a step that releases space (an unlink, a truncation, eviction or
 * orphan cleanup step), which may take the reserve, as Linux's global block
 * reserve serves deletion on a full volume. */
enum btrfs_result btrfs_transaction_room_releasing(
    struct btrfs_transaction *transaction, size_t nodes);

#endif
