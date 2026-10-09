/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_MUTABLE_H
#define MACHLIN_BTRFS_MUTABLE_H

#include "internal.h"

struct bt_mutation;

enum bt_edit { BT_INSERT, BT_REPLACE, BT_UPSERT, BT_DELETE };

/* The transaction allocator owns live-extent accounting. reserve must return a
 * fresh, node-aligned logical range of node_size bytes that is absent from every
 * committed/snapshot/pinned root. The editor never allocates by guessing a gap.
 * release rolls back a reservation, never frees a committed extent. Callbacks
 * must not edit these trees recursively. One caller owns a mutation context. */
struct bt_mutation_allocator {
	void *context;
	enum btrfs_result (*reserve)(
	    void *context, uint64_t owner, uint8_t level, uint64_t *logical);
	void (*release)(void *context, uint64_t logical);
	size_t node_limit;
};

/* Observes every extent whose references change, as Linux's qgroup extent
 * records do: the backref layer and extent creation report each one. The
 * callback records and must not edit trees. */
struct bt_mutation_observer {
	void *context;
	enum btrfs_result (*extent)(void *context, struct bt_key extent);
};

struct bt_mutated_block {
	uint64_t address;
	uint64_t original_address;
	uint64_t owner;
	uint64_t original_owner;
	uint64_t original_generation;
	uint64_t original_flags;
	uint8_t original_level;
	uint8_t level;
	int discarded;
	const void *bytes;
	size_t size;
};

/* Edits are private CoW operations. No function in this interface writes media
 * or publishes a filesystem root. The owning transaction must update extent
 * references, block groups, root items and durable superblocks before accept.
 * Any failed edit poisons the context: discard it; do not publish partial work.
 * The original reader and every untouched subtree remain immutable. */
enum btrfs_result bt_mutation_create(const struct btrfs_fs *base,
    const struct bt_mutation_allocator *allocator, struct bt_mutation **result);
enum btrfs_result bt_mutation_edit(struct bt_mutation *mutation, struct bt_root *root,
    struct bt_key key, const void *value, size_t length, enum bt_edit edit);
/* Replaces an existing key with an absent key and value. Equal keys replace
 * only the value. Same-size records within one packed leaf move in one edit;
 * other cases retain delete/insert semantics. value must not borrow a node. */
enum btrfs_result bt_mutation_rekey(struct bt_mutation *mutation, struct bt_root *root,
    struct bt_key old_key, struct bt_key new_key, const void *value, size_t length);
/* A new tree root owned by owner: with copy, a copy of source's node (a
 * snapshot's root, with the same items and child pointers); otherwise an
 * empty leaf whose header identifies this filesystem as source's does. */
enum btrfs_result bt_mutation_new_root(struct bt_mutation *mutation, struct bt_root source,
    uint64_t owner, int copy, struct bt_root *result);
enum btrfs_result bt_mutation_find(struct bt_mutation *mutation, struct bt_root root,
    struct bt_key key, void *value, size_t capacity, size_t *length);
const struct btrfs_fs *bt_mutation_view(const struct bt_mutation *mutation);
void bt_mutation_observe(struct bt_mutation *mutation, const struct bt_mutation_observer *observer);
/* Reports an extent (EXTENT_ITEM or METADATA_ITEM key) whose references
 * changed; a failed report poisons the mutation. */
enum btrfs_result bt_mutation_note_extent(struct bt_mutation *mutation, struct bt_key extent);
size_t bt_mutation_count(const struct bt_mutation *mutation);
/* Describes changed node index; its bytes carry their checksum once the
 * mutation is sealed. */
enum btrfs_result bt_mutation_block(
    struct bt_mutation *mutation, size_t index, struct bt_mutated_block *block);
enum btrfs_result bt_mutation_seal(struct bt_mutation *mutation);
/* Accept only after actual publication and its required persistence barrier. */
enum btrfs_result bt_mutation_accept(struct bt_mutation *mutation);
void bt_mutation_destroy(struct bt_mutation *mutation);

#endif
