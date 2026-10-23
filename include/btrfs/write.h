/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_WRITE_H
#define MACHLIN_BTRFS_WRITE_H

#include <btrfs/btrfs.h>

struct btrfs_transaction;

/* A further device of a volume spanning several: positioned writes and a
 * barrier, as the write environment's own. */
struct btrfs_write_device {
	void *context;
	enum btrfs_result (*write)(void *context, uint64_t offset, const void *bytes, size_t size);
	enum btrfs_result (*flush)(void *context);
};

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
	/* compress-force: every piece is tried, NOCOMPRESS files' too. Without
	 * it Linux's heuristic decides whether a piece is tried, and a piece
	 * that does not compress marks its file NOCOMPRESS unless the file's
	 * property names the codec. */
	int compress_force;
	/* Writes to the volume's other devices when it spans several: one for
	 * each of the reader environment's devices, in the same order; begin
	 * keeps each entry, not the table. NULL and 0 for a volume of one
	 * device. */
	const struct btrfs_write_device *devices;
	size_t device_count;
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
	/* The copies on the environment's own device. */
	struct btrfs_super_copy copies[BTRFS_SUPER_COPIES];
	/* Copies inside the devices, on all of them. */
	unsigned present;
	unsigned selected;
	uint64_t generation;
	unsigned rewritten;
	/* For a volume spanning several devices, the copies on each of the
	 * environment's further devices, in its order, and the device of the
	 * selected copy: 0 for the environment's own, i + 1 for devices[i]. */
	struct btrfs_super_copy device_copies[BTRFS_MAX_DEVICES - 1][BTRFS_SUPER_COPIES];
	unsigned selected_device;
};

/* Explicit, exclusive superblock recovery; mount never selects a mirror. Chooses
 * the newest checksum-valid copy on any device of the volume, every one of which
 * the environment names (UNSUPPORTED otherwise), and requires equal-generation
 * copies to agree but for each device's own item. A device's copies must name one
 * device, and the devices distinct ones. It never selects a generation below
 * acknowledged (STALE), a tree log outside a primary copy (UNSUPPORTED) or a copy
 * of another filesystem (CORRUPT). A selected primary may name a pending tree
 * log, which Linux's fsync writes to the primaries alone: equal secondaries then
 * name none, the copies recovery rewrites name none either, so every primary
 * must already name it, and btrfs_replay_log replays the log. When copies
 * disagree, it first opens the selection's chunk, root, extent and device trees
 * and its allocation map on every device. A NULL writer only inspects and then
 * returns RECOVERY_REQUIRED. With a writer, which writes each further device
 * too, each disagreeing copy is rewritten from the selection with its device's
 * item, whose used bytes the selection's chunk tree gives, followed by a
 * barrier on every device; the selected copy itself is never written. The
 * report's status fields describe the copies as found; current describes them
 * after a successful call. */
enum btrfs_result btrfs_recover_supers(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t acknowledged,
    struct btrfs_recovery_report *report);

struct btrfs_replay_report {
	/* The committed generation whose tree log was replayed. */
	uint64_t generation;
	/* Subvolume logs replayed, and the items each pass applied. */
	uint64_t logs;
	uint64_t inodes;
	uint64_t names;
	uint64_t unlinked;
	uint64_t extents;
	/* Logged data extents the replay allocated in the extent tree. */
	uint64_t allocated;
	/* Inodes left without a name; orphan cleanup removes them. */
	uint64_t orphans;
};

/* Explicit, exclusive tree-log replay, as Linux performs when it mounts a
 * filesystem whose primary superblock names a log: what fsync made durable
 * after the last commit is applied in one transaction, whose commit names no
 * log. The superblock copies must otherwise agree with the primary. Log
 * blocks and the data extents they reference stay out of allocation until
 * that commit. NOT_FOUND when no log is pending; nothing is written unless
 * every log item replays. A NULL writer replays without committing, writes
 * nothing and then returns RECOVERY_REQUIRED. Read-only mounts never replay:
 * they keep returning RECOVERY_REQUIRED. now stamps the directories the
 * replay changes. Afterwards, orphan cleanup removes inodes the log left
 * without a name. */
enum btrfs_result btrfs_replay_log(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, struct btrfs_time now,
    struct btrfs_replay_report *report);

struct btrfs_relocation_report {
	/* The committed generation recovery started from, and the relocation
	 * trees it found there. */
	uint64_t generation;
	uint64_t trees;
	/* Relocation trees merged into their file trees, and dropped. */
	uint64_t merged;
	uint64_t dropped;
	/* Orphan inodes of the data relocation tree removed. */
	uint64_t orphans;
	/* Transactions committed. */
	uint64_t commits;
};

/* Explicit, exclusive recovery of a balance a crash interrupted, as Linux's
 * btrfs_recover_relocation performs it at a read-write mount: relocation
 * trees still waiting for their merge are merged into their file trees, every
 * relocation tree is then dropped, and the data relocation tree's orphan
 * inodes are removed, in bounded transactions that each commit a state
 * Linux's recovery resumes from. Writable admission returns
 * RECOVERY_REQUIRED while relocation trees remain. NOT_FOUND when nothing is
 * left to recover; a NULL writer only inspects and returns RECOVERY_REQUIRED
 * when something is. A pending tree log must be replayed first
 * (btrfs_replay_log), which it may be beside relocation trees, as Linux's
 * mount replays it before its relocation recovery. With full quotas both
 * subtrees of every swap are accounted; simple quotas count each block and
 * extent for the owner it carries. A balance item stays: Linux resumes the
 * balance at its next read-write mount. */
enum btrfs_result btrfs_recover_relocation(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, struct btrfs_relocation_report *report);

/* What relocating a block group did: groups emptied and removed, tree
 * blocks and data extents moved, data bytes copied, and commits. */
struct btrfs_balance_report {
	uint64_t groups;
	uint64_t blocks;
	uint64_t extents;
	uint64_t bytes;
	uint64_t commits;
};

/* Relocates every extent of the block group at logical address group to
 * other groups and removes it, as Linux's balance relocates a chunk
 * (btrfs_relocate_chunk), in bounded steps that each commit. The group takes
 * no allocation while its extents move. A tree block moves once however
 * many subvolumes share it: its copy replaces it in every parent and root
 * item its references name, so sharing stays as it was; a data extent moves
 * with the checksums it has to a copy every file extent item naming it then
 * names. Every committed state holds the same files. Deleted subvolumes are
 * dropped first. The last group of its kind gets a new chunk of its profile
 * before it goes. Under simple quotas a moved tree block counts as new, as on
 * Linux, and moved data does not. A group whose v1 space cache would have to
 * go or a running quota rescan is UNSUPPORTED, and so is an extent with more
 * references than one transaction moves; NOT_FOUND names no group. Pending tree logs and
 * interrupted balances are recovered first (RECOVERY_REQUIRED). */
enum btrfs_result btrfs_relocate_group(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t group,
    struct btrfs_balance_report *report);

/* A balance's arguments for one kind of block group, as Linux's struct
 * btrfs_balance_args gives them: whether its groups are relocated; the usage
 * filter, only groups whose used bytes are below usage percent of their
 * length (0 selects empty groups, above 100 every group); the profile they
 * convert to, BTRFS_BALANCE_SINGLE or a BTRFS_GROUP_ profile, or
 * BTRFS_BALANCE_KEEP; and soft, which leaves groups that already have that
 * profile. */
struct btrfs_balance_kind {
	int enabled;
	unsigned usage;
	uint64_t convert;
	int soft;
};

#define BTRFS_BALANCE_KEEP UINT64_MAX
#define BTRFS_BALANCE_SINGLE UINT64_C(0)

struct btrfs_balance_args {
	struct btrfs_balance_kind data;
	struct btrfs_balance_kind metadata;
	struct btrfs_balance_kind system;
	/* Linux's --force: metadata or system groups may convert to a profile
	 * without a second copy or parity while the volume has such groups. */
	int force;
};

/* Linux's btrfs balance: every block group the arguments select when it
 * begins is relocated once (btrfs_relocate_group), in address order; a
 * mixed group takes the data arguments, which must equal the metadata ones
 * (INVALID_ARGUMENT). While a kind converts, new chunks of it take the
 * target profile and no group of it with another profile takes allocation,
 * so its groups end with the target profile. As Linux's btrfs_balance
 * refuses them (INVALID_ARGUMENT), a target needs its profile's fewest
 * devices, and metadata or system groups lose their redundancy only with
 * force. The first refusal ends the balance, the groups relocated before it
 * stay so, and report counts them all. */
enum btrfs_result btrfs_balance(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, const struct btrfs_balance_args *args,
    struct btrfs_balance_report *report);

struct btrfs_scrub_report {
	/* Copies read and verified: of tree blocks, and of data sectors with
	 * checksums. */
	uint64_t tree_copies;
	uint64_t data_copies;
	/* Data sectors without checksums (NODATASUM, preallocated), unverified. */
	uint64_t unchecked;
	/* Copies that did not verify, and that could not be read. */
	uint64_t corrupted;
	uint64_t unreadable;
	/* Copies rewritten from a copy that verifies. */
	uint64_t repaired;
	/* Tree blocks and data sectors no copy of which verifies. */
	uint64_t unrepairable;
	/* Superblock copies verified, and those that did not verify, which
	 * btrfs_recover_supers resolves. */
	uint64_t super_copies;
	uint64_t super_errors;
	/* RAID5/6 rows whose parity was checked, and sectors of their P and Q
	 * that did not equal the data's parity. */
	uint64_t parity_rows;
	uint64_t parity_errors;
};

/* Explicit, exclusive scrub, as Linux's btrfs scrub: every copy of every tree
 * block and of every checksummed data sector the extent tree names is read
 * from its device and verified as a read verifies it, and every superblock
 * copy is checked. With a writer (every device's, not on a degraded mount:
 * UNSUPPORTED), each copy that does not verify or cannot be read is rewritten
 * in place from a copy that verifies, followed by a barrier on each device
 * written; a NULL writer only inspects. RAID5/6 rebuilds from parity repair
 * a data stripe. Then, as Linux's scrub_raid56_parity_stripe, each RAID5/6
 * row holding extents is read again and its P and Q compared with the
 * data's parity at each sector offset where the extents of its data units
 * all verify; with a writer, those that differ are rewritten. CORRUPT when
 * some block or sector has no copy that verifies, a superblock copy fails,
 * or copies or parity fail without a writer; report counts them. */
enum btrfs_result btrfs_scrub(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, struct btrfs_scrub_report *report);

/* Linux's btrfs device add (btrfs_init_new_device): the device, read through
 * device and written through output, joins the volume the environment and
 * writer name (every device of it) with the next device id (*id) and uuid,
 * the caller's random UUID; its device item records its size in whole
 * sectors, and every superblock the volume's new device count and size. The
 * new device's superblock copies are written before any other device's
 * superblock names it. Afterwards the volume needs every device to mount,
 * the new one last in the environment's order. A device holding fewer than
 * two superblock copies, or one more than BTRFS_MAX_DEVICES, is UNSUPPORTED;
 * one whose superblock already names this volume INVALID_ARGUMENT. */
enum btrfs_result btrfs_device_add(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, const struct btrfs_device *device,
    const struct btrfs_write_device *output, const uint8_t uuid[BTRFS_UUID_SIZE], uint64_t *id);

/* Linux's btrfs device remove (btrfs_rm_device): every block group with a
 * stripe on the device named id is relocated, the highest-placed first and
 * none onto that device (btrfs_shrink_device), in committed steps report
 * counts; then the device's item goes and every other device's superblocks
 * name one device less, and the removed device's superblock copies lose their
 * magic (btrfs_scratch_superblocks). Removing the last device, or one a
 * profile in use needs (btrfs_check_raid_min_devices), is INVALID_ARGUMENT;
 * an unknown id NOT_FOUND. A crash leaves a volume of either device set:
 * superblock recovery with the remaining devices is UNSUPPORTED (a device is
 * missing) until the removal's last commit is durable on one of them, and
 * then succeeds. */
enum btrfs_result btrfs_device_remove(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t id, struct btrfs_balance_report *report);

/* Linux's btrfs replace of the device named id by target, written through
 * output: as Linux's replace gives the target the device's id and UUID, the
 * target receives every stripe the device holds, at the same offsets, and
 * then its superblock copies, each behind a barrier, so that it holds the
 * device whole before it names itself one; the device's superblock copies
 * then lose their magic. Nothing else on the volume changes, and its trees
 * stay as they are. On a degraded mount without the device (every other
 * device present), each stripe comes from a stripe holding the same bytes
 * (RAID1, RAID1C3, RAID1C4, a RAID10 pair) or for RAID5/6 is rebuilt row by
 * row from the row's other stripes (UNSUPPORTED for single, DUP and RAID0),
 * and the superblocks from another device's with the device's item; a stripe
 * the device cannot read comes from there too. Superblock copies a stripe
 * covers are not copied with it. A target smaller than the device,
 * or already a device of the volume, is INVALID_ARGUMENT. Until the call
 * returns both may name the device; only one of them may then be given. The
 * operation is exclusive: no transaction runs meanwhile. */
enum btrfs_result btrfs_device_replace(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t id, const struct btrfs_device *target,
    const struct btrfs_write_device *output);

/* Linux's btrfs filesystem resize of the device named id to size bytes,
 * rounded down to whole sectors (btrfs_ioctl_resize): at least 256 MiB
 * (INVALID_ARGUMENT) and at most the device holds (RANGE). Growing records
 * the size in the device's items and the superblocks' total in one commit
 * (btrfs_grow_device); shrinking first relocates every group with a stripe
 * past the new end, the highest first, none of them going there
 * (btrfs_shrink_device), in committed steps report counts, then records the
 * size. The device's superblock copies follow its size. NOT_FOUND names no
 * device; a volume without room is refused for space with its groups
 * relocated so far. */
enum btrfs_result btrfs_device_resize(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t id, uint64_t size,
    struct btrfs_balance_report *report);

/* Admission: any of Linux's four checksum algorithms, skinny metadata, the
 * free-space and block-group trees, fs-verity, quotas and simple quotas, and
 * mixed groups whose nodes are one sector, on one device or on several: every
 * device of the volume present (not degraded), a writer device for each
 * further one (INVALID_ARGUMENT otherwise), and any profile; new chunks are
 * placed across the devices as Linux places them. In RAID5/6 chunks a
 * transaction allocates only in rows that held nothing when it began or that
 * it began itself, writes their data stripes only, never overwrites a
 * committed extent in place (a NODATACOW or preallocated range is copied),
 * and writes the rows' P and Q at commit with the metadata, before the first
 * barrier: no committed block's parity ever changes, so a torn write cannot
 * leave one unrecoverable (the write hole). Every device
 * must hold at least two superblock copies that agree with the mounted primary;
 * otherwise begin returns RECOVERY_REQUIRED. Unsupported layouts return
 * before any media write. Native adapters open read-write only on request,
 * and run btrfs_recover_supers first when admission returns
 * RECOVERY_REQUIRED.
 *
 * Commit writes new metadata (every copy on its device), a barrier on every
 * device, the secondary superblocks of every device, barriers, the primaries,
 * barriers; each device's superblocks carry its own device item. Every crash
 * point therefore retains a valid copy at the last acknowledged generation or
 * later; btrfs_recover_supers selects it. */
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
 * Linux does (in RAID5/6 chunks only those this transaction created). The
 * range's data space is checked first (NO_SPACE); as Linux's reserve_space,
 * a range written in place in every sector needs none, so a full volume
 * still takes writes into preallocated space. Old extents keep their references until commit drops
 * them and are never reused before the next transaction. New data reaches the device as its extents
 * are created; the commit's first barrier makes it durable before any metadata names it. Checksums
 * follow the inode's NODATASUM flag; files compress as Linux decides. Inline files become regular
 * extents. Writes past an unaligned EOF clear the old EOF sector's tail; truncation clears the new
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

/* FS_IOC_SETFLAGS as Linux's btrfs_fileattr_set: flags (BTRFS_FS_*_FL)
 * replace the inode's synchronous, immutable, append-only, no-dump, no-atime
 * and directory-sync flags. NOCOW sets NODATACOW (with NODATASUM on a
 * regular file) only on directories and empty regular files, and clears them
 * under the same rule. COMPR sets COMPRESS and the btrfs.compression property
 * to the mount's codec (zlib without one); NOCOMP sets NOCOMPRESS; without
 * either both flags and the property go. Flags another type cannot carry are
 * dropped, as Linux masks them. Unknown flags are UNSUPPORTED; COMPR with
 * NOCOMP or NOCOW, or NOCOW against compression flags, INVALID_ARGUMENT.
 * Changing IMMUTABLE or APPEND needs Linux's CAP_LINUX_IMMUTABLE and any
 * change the owner or a privileged caller: both are the caller's decision.
 * The change time and version change; immutable inodes accept the call. */
enum btrfs_result btrfs_transaction_set_fsflags(struct btrfs_transaction *transaction,
    struct btrfs_object_id id, unsigned flags, struct btrfs_time time);

/* Linux's fallocate modes (FALLOC_FL_*), with Linux's values. */
#define BTRFS_FALLOCATE_KEEP_SIZE 0x01U
#define BTRFS_FALLOCATE_PUNCH_HOLE 0x02U
#define BTRFS_FALLOCATE_ZERO_RANGE 0x10U

/* fallocate of a regular file as Linux's btrfs_fallocate. Mode 0 or
 * KEEP_SIZE preallocates the holes of [offset, offset + length) (and data
 * beyond EOF) as unwritten extents, which read as zeros and are written in
 * place later, and without KEEP_SIZE grows the file to offset + length.
 * PUNCH_HOLE (only with KEEP_SIZE) drops the range's coverage, zeroing the
 * partial sectors at its edges, and leaves hole items below EOF without
 * NO_HOLES. ZERO_RANGE (optionally with KEEP_SIZE) makes the range read as
 * zeros: written partial sectors are zeroed, the rest becomes unwritten
 * extents. Times change as Linux's file_modified changes them, except for a
 * punch entirely within a hole, which changes nothing. Other modes and
 * combinations are UNSUPPORTED; append-only files take only allocation
 * (NOT_PERMITTED otherwise); length 0 is INVALID_ARGUMENT; an end beyond the
 * file size limit is RANGE. The data space the operation may need is checked
 * before any change (NO_SPACE). */
enum btrfs_result btrfs_transaction_fallocate(struct btrfs_transaction *transaction,
    struct btrfs_object_id id, unsigned mode, uint64_t offset, uint64_t length,
    struct btrfs_time now);

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
/* Linux's quota rescan (btrfs quota rescan): every qgroup is counted again
 * from the extents of the committed extent tree, as btrfs_qgroup_rescan_worker
 * counts them. A new rescan zeroes every qgroup and records its progress in
 * the quota status item; it must be the transaction's first change, as Linux
 * commits before it begins (UNSUPPORTED otherwise). Each call examines about
 * budget extent-tree items from the recorded progress and later transactions
 * resume there; extents beyond the progress are left to the scan. *done is set
 * when the scan has covered the tree, which clears the inconsistent flag. A
 * filesystem without quotas is INVALID_ARGUMENT; a transaction that took a
 * snapshot is UNSUPPORTED, and so is a snapshot while a rescan runs. Anything
 * that makes quotas inconsistent cancels a running rescan, as on Linux. */
enum btrfs_result btrfs_transaction_quota_rescan(
    struct btrfs_transaction *transaction, size_t budget, int *done);
/* Whether a quota rescan is in progress, begun here or by Linux. */
int btrfs_transaction_quota_rescanning(const struct btrfs_transaction *transaction);
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
 * work up to budget; an orphan item of an inode that still has names goes
 * with the fs-verity items of an enable that never finished, and that of a
 * missing inode is dropped. *pending reports that orphans remain. */
enum btrfs_result btrfs_transaction_clean_orphans(struct btrfs_transaction *transaction,
    uint64_t tree, size_t budget, size_t *cleaned, int *pending);

/* FS_IOC_ENABLE_VERITY's arguments (struct fsverity_enable_arg): a hash
 * algorithm (BTRFS_VERITY_HASH_*), a Merkle tree block size (a power of two
 * from 1 KiB up to the sector size), a salt of at most 32 bytes and an
 * optional builtin signature, stored and not verified. */
struct btrfs_verity_parameters {
	unsigned algorithm;
	uint32_t block_size;
	const void *salt;
	size_t salt_size;
	const void *signature;
	size_t signature_size;
};
/* An fs-verity enable in progress: the tree blocks it is filling. */
struct btrfs_verity_build;
/* Begins enabling fs-verity on a regular file, refusing as Linux's
 * fsverity_ioctl_enable does: INVALID_ARGUMENT for a block size that is not
 * a power of two, RANGE for a salt or signature too large (EMSGSIZE),
 * NOT_PERMITTED for an immutable file, READ_ONLY in a read-only subvolume,
 * NOT_PERMITTED for an append-only file, IS_DIRECTORY or INVALID_ARGUMENT for
 * other types, INVALID_ARGUMENT for another algorithm or block size, RANGE for
 * a tree deeper than eight levels or of more than 2^23 blocks (EFBIG), and
 * EXISTS for a file with fs-verity. A builtin signature is stored, not
 * verified: the caller checks it against its keyring first, as Linux's
 * fsverity_verify_signature does (ENOKEY with an empty keyring). Then, as
 * btrfs_begin_enable_verity, an orphan item marks the enable until it finishes; a mount's orphan
 * cleanup drops what an enable left. The caller authorizes write access, holds the file open as the
 * ioctl's descriptor does, keeps its data unchanged (Linux's deny_write_access) and refuses a
 * second enable of the file (EBUSY) until finish or abort, which may come in later transactions of
 * the mount. The build's storage comes from, and must not outlive, the base environment's
 * allocator. */
enum btrfs_result btrfs_transaction_verity_begin(struct btrfs_transaction *transaction,
    struct btrfs_object_id id, const struct btrfs_verity_parameters *parameters,
    struct btrfs_verity_build **build);
/* Drops fs-verity items an earlier enable left, then hashes up to budget data
 * blocks and stores each Merkle tree block they complete in VERITY_MERKLE
 * items, as btrfs_write_merkle_tree_block does; *done once all data is
 * hashed. Each item counts against qgroup limits. */
enum btrfs_result btrfs_transaction_verity_step(struct btrfs_transaction *transaction,
    struct btrfs_verity_build *build, size_t budget, int *done);
/* Stores the remaining tree blocks and the descriptor, then marks the inode
 * RO_VERITY, deletes its orphan item and sets the VERITY read-only feature, as
 * btrfs_end_enable_verity does. STALE when the file changed size or identity
 * since begin; the build then needs btrfs_transaction_verity_abort. */
enum btrfs_result btrfs_transaction_verity_finish(
    struct btrfs_transaction *transaction, struct btrfs_verity_build *build);
/* Linux's rollback_verity: deletes the fs-verity items stored so far with
 * work up to budget and then the orphan item; *done once both are gone. */
enum btrfs_result btrfs_transaction_verity_abort(struct btrfs_transaction *transaction,
    struct btrfs_verity_build *build, size_t budget, int *done);
void btrfs_verity_build_free(struct btrfs_verity_build *build);
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
