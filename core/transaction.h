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
/* Staged data and queued file reference changes of one transaction. */
#define BT_TRANSACTION_DATA (UINT64_C(64) * 1024 * 1024)
#define BT_TRANSACTION_EXTENTS 4096U
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
};

/* A new data extent held in memory until publication writes it before the
 * first barrier. Its physical copies serve reads through the private view. */
struct bt_staged {
	uint64_t logical;
	uint64_t length;
	uint64_t physical[2];
	unsigned mirrors;
	uint8_t *bytes;
	int freed;
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
	struct bt_root roots;
	struct bt_root chunks;
	struct bt_owned_root devices;
	struct bt_root top;
	struct bt_owned_root trees[BT_TRANSACTION_TREES];
	size_t tree_count;
	struct bt_owned_root extents;
	struct bt_owned_root checksums;
	struct bt_owned_root free_space;
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
	struct bt_staged *staged;
	size_t staged_count;
	uint64_t staged_bytes;
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
	enum btrfs_result failure;
	int changed;
	int finished;
	int writing;
};

struct bt_directory_counter *bt_counters_directory(
    struct btrfs_counters *counters, uint64_t tree, uint64_t directory, int add);
enum btrfs_result bt_tx_tree(
    struct btrfs_transaction *transaction, uint64_t tree, struct bt_owned_root **result);
enum btrfs_result bt_tx_edit(struct btrfs_transaction *transaction, struct bt_root *root,
    struct bt_key key, const void *data, size_t size, enum bt_edit edit);
/* Applies queued file references; a data extent whose last reference goes
 * loses its checksums and its block-group space. */
enum btrfs_result bt_tx_apply_refs(struct btrfs_transaction *transaction);
enum btrfs_result bt_tx_write_staged(struct btrfs_transaction *transaction);
enum btrfs_result bt_tx_drop_range(struct btrfs_transaction *transaction,
    struct bt_owned_root *tree, uint64_t inode, uint64_t start, uint64_t end, uint64_t *removed);
void bt_tx_release_data(struct btrfs_transaction *transaction);
/* A write or truncation of a regular file needs a settled privilege decision
 * when it has set-id bits Linux removes or a file capability. */
enum btrfs_result bt_tx_privileges_settled(struct btrfs_transaction *transaction,
    struct bt_owned_root *tree, uint64_t inode, const struct bt_disk_inode *item);

#endif
