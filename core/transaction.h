/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_TRANSACTION_H
#define MACHLIN_BTRFS_TRANSACTION_H

#include "backref.h"
#include "csum.h"
#include "fst.h"
#include "encode.h"
#include "space.h"
#include <btrfs/write.h>

#define BT_TRANSACTION_NODES 4096U
#define BT_TRANSACTION_TREES 16U
/* Queued file reference changes of one transaction. */
#define BT_TRANSACTION_REFERENCES 65536U
#define BT_INLINE_WRITE_LIMIT 2048U
#define BT_TRANSACTION_INDEXES 256U
#define BT_COUNTER_TREES 64U
/* Directory counter slots (a power of two) and the occupancy that empties the
 * table. */
#define BT_COUNTER_SLOTS 1024U
#define BT_COUNTER_LOAD 768U
#define BT_TRANSACTION_PRIVILEGED 64U
#define BT_ACCOUNT_ORIGINAL 1U
#define BT_ACCOUNT_NEW 2U
#define BT_INODE_NODATASUM_FLAG (UINT64_C(1) << 0)
#define BT_INODE_IMMUTABLE (UINT64_C(1) << 6)
#define BT_INODE_APPEND (UINT64_C(1) << 7)

struct bt_owned_root {
	struct bt_root root;
	struct bt_key key;
	struct bt_disk_root_full item;
	size_t size;
	/* A read-only subvolume opened as a snapshot source: its root item may
	 * change, its tree may not. */
	int read_only;
};

/* A file extent item reference change, applied after the CoW-derived changes
 * of the same commit with all additions before drops, as Linux runs its
 * delayed references. */
struct bt_file_ref {
	struct bt_key extent;
	struct bt_backref reference;
	int add;
};

struct bt_index_cache {
	uint64_t tree;
	uint64_t directory;
	uint64_t next;
};

struct bt_tree_counter {
	uint64_t tree;
	/* The next inode number; 0 until a transaction derived it. */
	uint64_t next;
};

/* A directory's next index; directory 0 marks a free slot, and known 0 a
 * directory forgotten since its counter was last derived. */
struct bt_directory_counter {
	uint64_t tree;
	uint64_t directory;
	uint64_t next;
	int known;
};

struct btrfs_counters {
	struct btrfs_environment environment;
	struct bt_tree_counter trees[BT_COUNTER_TREES];
	size_t tree_count;
	/* The next tree id; 0 until a transaction derived it. */
	uint64_t next_root;
	struct bt_directory_counter directories[BT_COUNTER_SLOTS];
	size_t directory_count;
	/* How often the directory table was emptied. */
	uint64_t forgotten;
};

struct btrfs_transaction {
	const struct btrfs_fs *base;
	/* The base with a private chunk map that growth may extend; every mapping
	 * and the mutation use it. */
	struct btrfs_fs fs;
	struct btrfs_write_environment io;
	struct bt_mutation *mutation;
	struct bt_space *space;
	/* The owner's allocation map, updated after a successful commit. */
	struct btrfs_allocation_map *map;
	struct bt_root roots;
	struct bt_root chunks;
	struct bt_owned_root devices;
	struct bt_root top;
	struct bt_owned_root trees[BT_TRANSACTION_TREES];
	size_t tree_count;
	struct bt_owned_root extents;
	struct bt_owned_root checksums;
	struct bt_owned_root free_space;
	/* Subvolume UUIDs, when the filesystem has the tree. */
	struct bt_owned_root uuids;
	int has_uuids;
	/* The next tree id this transaction hands out; 0 until derived. */
	uint64_t next_root;
	/* The deleted subvolume the cleaner works on. */
	struct bt_owned_root dropping;
	size_t free_space_applied;
	size_t chunks_published;
	int has_free_space;
	struct bt_disk_super original_super;
	struct bt_disk_super super;
	uint8_t accounted[BT_TRANSACTION_NODES];
	uint8_t *scratch;
	uint8_t *original;
	/* Namespace edits, allocated by the first one: the packed item being
	 * changed and the encoding of a new entry. */
	uint8_t *item;
	uint8_t *entry;
	struct bt_file_ref *refs;
	size_t ref_count;
	/* Inode numbers and directory indexes handed out in this transaction stay
	 * monotonic even when the highest one is removed again. */
	uint64_t next_objectid[BT_TRANSACTION_TREES];
	struct bt_index_cache indexes[BT_TRANSACTION_INDEXES];
	size_t index_count;
	/* Mount-wide counters continuing them across transactions, or NULL. */
	struct btrfs_counters *counters;
	/* Inodes whose set-id bits and file capability a privileged writer keeps. */
	struct btrfs_object_id privileged[BT_TRANSACTION_PRIVILEGED];
	size_t privileged_count;
	unsigned copies;
	/* btrfs_transaction_reader's view, rebuilt by each call. */
	struct btrfs_fs reader;
	enum btrfs_result failure;
	int changed;
	int finished;
	int writing;
};

struct bt_directory_counter *bt_counters_directory(
    struct btrfs_counters *counters, uint64_t tree, uint64_t directory, int add);
enum btrfs_result bt_tx_tree(
    struct btrfs_transaction *transaction, uint64_t tree, struct bt_owned_root **result);
/* Opens a subvolume as a snapshot source: a read-only one too, without
 * admitting edits of its tree. */
enum btrfs_result bt_tx_source(
    struct btrfs_transaction *transaction, uint64_t tree, struct bt_owned_root **result);
/* Registers a tree created in this transaction, whose root item is already
 * inserted under owned->key. */
enum btrfs_result bt_tx_add_tree(struct btrfs_transaction *transaction,
    const struct bt_owned_root *owned, struct bt_owned_root **result);
/* The current root item of tree, without admitting edits of the tree. */
enum btrfs_result bt_tx_root_item(
    struct btrfs_transaction *transaction, uint64_t tree, struct bt_owned_root *result);
/* The next free tree id: after the highest object of the root tree below
 * BTRFS_LAST_FREE_OBJECTID, and after every id the mount's counters handed out. */
enum btrfs_result bt_tx_root_id(struct btrfs_transaction *transaction, uint64_t *result);
/* Adds or removes the references a tree block's content holds: child blocks
 * for nodes, regular and preallocated data extents for leaves. full selects
 * the shared form naming address as parent; otherwise references name root. */
enum btrfs_result bt_tx_children(struct btrfs_transaction *transaction, const uint8_t *node,
    uint64_t address, int full, uint64_t root, int add);
enum btrfs_result bt_tx_edit(struct btrfs_transaction *transaction, struct bt_root *root,
    struct bt_key key, const void *data, size_t size, enum bt_edit edit);
/* Queues a file reference change for bt_tx_apply_refs. */
enum btrfs_result bt_tx_queue(struct btrfs_transaction *transaction, struct bt_key extent,
    const struct bt_backref *reference, int add);
/* Applies queued file references; a data extent whose last reference goes
 * loses its checksums and its block-group space. */
enum btrfs_result bt_tx_apply_refs(struct btrfs_transaction *transaction);
/* Deletes the items of every group marked removed (core/group.c). */
enum btrfs_result bt_tx_remove_groups(struct btrfs_transaction *transaction);
enum btrfs_result bt_tx_drop_range(struct btrfs_transaction *transaction,
    struct bt_owned_root *tree, uint64_t inode, uint64_t start, uint64_t end, uint64_t *removed);
void bt_tx_release_data(struct btrfs_transaction *transaction);
/* A write or truncation of a regular file needs a settled privilege decision
 * when it has set-id bits Linux removes or a file capability. */
enum btrfs_result bt_tx_privileges_settled(struct btrfs_transaction *transaction,
    struct bt_owned_root *tree, uint64_t inode, const struct bt_disk_inode *item);

#endif
