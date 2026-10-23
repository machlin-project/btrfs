/* SPDX-License-Identifier: BSD-3-Clause */
/* Relocation of a block group, as Linux's balance relocates one
 * (btrfs_relocate_chunk): every extent in the group moves to other groups,
 * then the group, its chunk and device extents go. Linux stages the moves in
 * relocation trees and a data relocation inode and merges them into the file
 * trees; here each committed step moves extents in place of their originals,
 * so that every state it commits is an ordinary one with the same content.
 *
 * The group takes no allocation while its extents move (Linux's read-only
 * block group). A tree block of a tree that is never shared moves with the
 * CoW of its path. A block of a file tree moves once, however many trees
 * share it: its copy replaces it in every parent its references name and,
 * for a root, in its root item; a parent that is not this transaction's own
 * node moves the same way, up to the roots. The copy takes the extent item
 * with every reference, and the references of its children when it names
 * them by address (FULL_BACKREF). A data extent is copied with the
 * checksums it has, and every file extent item its references find points
 * at the copy in a moved leaf; its extent item keeps its generation. Sharing
 * between subvolumes stays exactly as it was. Moved blocks carry this
 * transaction's generation, so a root whose root node moved has its last
 * snapshot set to this transaction, as Linux's create_reloc_root sets it:
 * a later CoW then looks up the references of every moved block. */
#include "qgroup.h"
#include "transaction.h"

/* References one moved extent may have; a block more trees share, or a data
 * extent with more references, is UNSUPPORTED. */
#define BT_BALANCE_REFS 1024U
/* File extent items one data extent's move points at the copy. */
#define BT_BALANCE_ITEMS 4096U
/* Moved blocks one transaction remembers: a power of two above the nodes a
 * transaction changes. */
#define BT_BALANCE_COPIES (2U * BT_TRANSACTION_NODES)
/* Tree blocks a cleaner step visits before relocation begins. */
#define BT_BALANCE_DROP_BLOCKS 1024U
/* Data bytes one read and write of a data extent's copy move. */
#define BT_BALANCE_WINDOW (1024U * 1024U)
/* Sectors one window holds at most, at Linux's smallest sector size. */
#define BT_BALANCE_SECTORS (BT_BALANCE_WINDOW / 4096U)
/* Committed steps after which a relocation that still moves extents stops. */
#define BT_BALANCE_STEPS 1048576U
/* The kind bits of a block group's type; the rest is its profile. */
#define BT_BALANCE_KINDS (BT_BLOCK_DATA | BT_BLOCK_METADATA | BT_BLOCK_SYSTEM)
/* A data reference's offset (file offset minus extent offset) this large
 * wrapped below zero; its items are searched from the inode's first, as
 * Linux's find_data_references does. */
#define BT_BALANCE_OFFSET_LIMIT (UINT64_MAX << 32)

/* A block this transaction moved, by its original address. */
struct bt_bl_copy {
	uint64_t original;
	uint64_t address;
};

/* A file extent item naming the data extent being moved. */
struct bt_bl_item {
	uint64_t leaf;
	uint32_t slot;
};

struct bt_balance {
	struct btrfs_transaction *transaction;
	uint64_t start;
	uint64_t end;
	/* The generation this transaction commits. */
	uint64_t generation;
	struct bt_bl_copy *copies;
	/* References of the block being moved at each level, and of the data
	 * extent being moved (index BT_MAX_LEVEL). */
	struct bt_extent_ref *refs[BT_MAX_LEVEL + 1];
	uint8_t *nodes[BT_MAX_LEVEL];
	struct bt_bl_item *items;
	uint8_t *window;
	uint8_t *sums;
	uint8_t covered[BT_BALANCE_SECTORS];
	uint64_t blocks;
	uint64_t extents;
	uint64_t bytes;
};

static size_t
bt_bl_hash(uint64_t address)
{
	address ^= address >> 33;
	address *= UINT64_C(0xff51afd7ed558ccd);
	address ^= address >> 33;
	return (size_t)address & (BT_BALANCE_COPIES - 1U);
}

/* The copy of a block this transaction moved, 0 when it did not. */
static uint64_t
bt_bl_copy_of(const struct bt_balance *balance, uint64_t original)
{
	size_t i = bt_bl_hash(original);
	size_t probes;

	for (probes = 0; probes < BT_BALANCE_COPIES; probes++) {
		if (balance->copies[i].original == original) {
			return balance->copies[i].address;
		}
		if (balance->copies[i].original == 0) {
			return 0;
		}
		i = (i + 1) & (BT_BALANCE_COPIES - 1U);
	}
	return 0;
}

static enum btrfs_result
bt_bl_remember(struct bt_balance *balance, uint64_t original, uint64_t address)
{
	size_t i = bt_bl_hash(original);
	size_t probes;

	for (probes = 0; probes < BT_BALANCE_COPIES; probes++) {
		if (balance->copies[i].original == 0) {
			balance->copies[i] = (struct bt_bl_copy){ original, address };
			return BTRFS_OK;
		}
		i = (i + 1) & (BT_BALANCE_COPIES - 1U);
	}
	return BTRFS_UNSUPPORTED;
}

/* The generation of the tree block at address: this transaction's for its
 * own nodes, else its extent item's, which Linux and this writer give the
 * block's header when they allocate it. */
static enum btrfs_result
bt_bl_generation(struct bt_balance *balance, uint64_t address, uint8_t level, uint64_t *generation)
{
	struct btrfs_transaction *transaction = balance->transaction;
	const struct bt_disk_extent_item *item;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { address, level, BT_METADATA_ITEM };
	enum btrfs_result error;

	if (bt_mutation_owns(transaction->mutation, address)) {
		*generation = balance->generation;
		return BTRFS_OK;
	}
	bt_cursor_init(&cursor, bt_mutation_view(transaction->mutation), transaction->extents.root);
	error = bt_cursor_seek(&cursor, key, 0);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		item = (const void *)record.data;
		if (bt_key_compare(record.key, key) != 0 || record.size < sizeof(*item)) {
			error = BTRFS_CORRUPT;
		} else {
			*generation = bt_u64(item->generation);
		}
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
}

/* The owner a read of the tree block at address names: the chunk tree in
 * system chunks, where only its blocks live and which a read maps as such,
 * else whichever its header names. */
static uint64_t
bt_bl_owner(const struct bt_balance *balance, uint64_t address)
{
	const struct btrfs_fs *fs = &balance->transaction->fs;
	size_t chunk = bt_chunk_containing(fs, address);

	return chunk < fs->chunk_count && (fs->chunks[chunk].type & BT_BLOCK_SYSTEM) != 0
	    ? BT_CHUNK_TREE
	    : BT_OWNER_ANY;
}

/* Reads the tree block at address and level as this transaction sees it. */
static enum btrfs_result
bt_bl_read(struct bt_balance *balance, uint64_t address, uint8_t level, uint8_t *buffer)
{
	uint64_t generation = 0;
	enum btrfs_result error;

	error = bt_bl_generation(balance, address, level, &generation);
	if (error == BTRFS_OK) {
		error = bt_tree_read(bt_mutation_view(balance->transaction->mutation),
		    (struct bt_root){ address, generation, bt_bl_owner(balance, address), level },
		    buffer);
	}
	return error;
}

/* The first key of a node; 0 for an empty leaf, which only a root may be. */
static int
bt_bl_first(const uint8_t *node, struct bt_key *key)
{
	const struct bt_disk_header *header = (const void *)node;
	const struct bt_disk_item *items = (const void *)(header + 1);
	const struct bt_disk_pointer *pointers = (const void *)(header + 1);

	if (bt_u32(header->count) == 0) {
		return 0;
	}
	*key = header->level == 0 ? bt_key_decode(&items[0].key) : bt_key_decode(&pointers[0].key);
	return 1;
}

/* The slot of an internal node whose key is key and whose pointer names
 * child. */
static enum btrfs_result
bt_bl_slot(const uint8_t *node, struct bt_key key, uint64_t child, uint32_t *slot)
{
	const struct bt_disk_header *header = (const void *)node;
	const struct bt_disk_pointer *pointers = (const void *)(header + 1);
	uint32_t low = 0;
	uint32_t high = bt_u32(header->count);
	uint32_t middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (bt_key_compare(bt_key_decode(&pointers[middle].key), key) <= 0) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	if (header->level == 0 || low == 0 ||
	    bt_key_compare(bt_key_decode(&pointers[low - 1].key), key) != 0 ||
	    bt_u64(pointers[low - 1].bytenr) != child) {
		return BTRFS_CORRUPT;
	}
	*slot = low - 1;
	return BTRFS_OK;
}

/* A live file tree's current root item; a deleted tree, or one being
 * dropped, has no parents to redirect. */
static enum btrfs_result
bt_bl_tree(struct bt_balance *balance, uint64_t tree, struct bt_owned_root *owned)
{
	enum btrfs_result error;

	if (!bt_file_tree(tree)) {
		return BTRFS_CORRUPT;
	}
	error = bt_tx_root_item(balance->transaction, tree, owned);
	if (error == BTRFS_NOT_FOUND) {
		return BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK &&
	    (bt_u32(owned->item.legacy.refs) == 0 ||
		bt_u64(owned->item.legacy.drop_progress.objectid) != 0 ||
		owned->root.level >= BT_MAX_LEVEL)) {
		error = BTRFS_UNSUPPORTED;
	}
	return error;
}

/* Points tree's root item at the moved root node. */
static enum btrfs_result
bt_bl_root(
    struct bt_balance *balance, uint64_t tree, uint64_t original, uint64_t address, uint8_t level)
{
	struct btrfs_transaction *transaction = balance->transaction;
	struct bt_owned_root owned;
	size_t i;
	enum btrfs_result error;

	/* A tree the transaction opened would publish its own root item. */
	for (i = 0; i < transaction->tree_count; i++) {
		if (transaction->trees[i].root.owner == tree) {
			return BTRFS_UNSUPPORTED;
		}
	}
	error = bt_bl_tree(balance, tree, &owned);
	if (error == BTRFS_OK && (owned.root.address != original || owned.root.level != level)) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		bt_put64(&owned.item.legacy.bytenr, address);
		bt_put64(&owned.item.legacy.generation, balance->generation);
		bt_put64(&owned.item.generation_v2, balance->generation);
		bt_put64(&owned.item.legacy.last_snapshot, balance->generation);
		error = bt_tx_edit(transaction, &transaction->roots, owned.key, &owned.item,
		    owned.size, BT_REPLACE);
	}
	if (error == BTRFS_OK && tree == BTRFS_TOP_LEVEL_TREE) {
		transaction->top = (struct bt_root){ address, balance->generation, tree, level };
	}
	return error;
}

/* The node of tree's current tree that points at the block at address and
 * level, found as Linux's resolve_indirect_ref finds it: one level up on the
 * search path of the block's first key; 0 when the block is the root. Out of
 * line, so that no cursor stays on the stack of the recursive move. */
static BT_NOINLINE enum btrfs_result
bt_bl_keyed_parent(struct bt_balance *balance, uint64_t tree, uint64_t address, uint8_t level,
    const struct bt_key *first, uint64_t *parent, uint32_t *slot)
{
	struct bt_owned_root owned;
	struct bt_cursor cursor;
	enum btrfs_result error;

	*parent = 0;
	error = bt_bl_tree(balance, tree, &owned);
	if (error != BTRFS_OK || owned.root.address == address) {
		return error;
	}
	if (first == NULL || owned.root.level <= level) {
		return BTRFS_CORRUPT;
	}
	bt_cursor_init(&cursor, bt_mutation_view(balance->transaction->mutation), owned.root);
	error = bt_cursor_seek(&cursor, *first, 1);
	if (error == BTRFS_OK && cursor.loaded[level].address != address) {
		error = BTRFS_CORRUPT;
	} else if (error == BTRFS_OK) {
		*parent = cursor.loaded[level + 1].address;
		*slot = cursor.slots[level + 1];
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
}

static enum btrfs_result bt_bl_block(
    struct bt_balance *balance, uint64_t address, uint8_t level, uint64_t *copy);

/* Points slot of parent, at level, from original to the moved block, moving
 * the parent first unless it is this transaction's own node. */
static enum btrfs_result
bt_bl_link(struct bt_balance *balance, uint64_t parent, uint8_t level, uint32_t slot,
    uint64_t original, uint64_t address)
{
	struct bt_mutation *mutation = balance->transaction->mutation;
	const struct bt_disk_header *header;
	const struct bt_disk_pointer *pointers;
	uint8_t *bytes = NULL;
	uint64_t moved = parent;
	enum btrfs_result error = BTRFS_OK;

	if (!bt_mutation_owns(mutation, parent)) {
		error = bt_bl_block(balance, parent, level, &moved);
	}
	if (error == BTRFS_OK) {
		error = bt_mutation_bytes(mutation, moved, &bytes);
	}
	if (error == BTRFS_OK) {
		header = (const void *)bytes;
		pointers = (const void *)(header + 1);
		if (header->level != level || slot >= bt_u32(header->count) ||
		    bt_u64(pointers[slot].bytenr) != original) {
			error = BTRFS_CORRUPT;
		}
	}
	if (error == BTRFS_OK) {
		error =
		    bt_mutation_set_pointer(mutation, moved, slot, address, balance->generation);
	}
	return error;
}

/* Moves a file tree block, which nothing in this transaction moved yet, and
 * redirects every parent and root its references name; *copy is where it
 * went. A recursion moves parents, one level up at a time. */
static enum btrfs_result
bt_bl_block(struct bt_balance *balance, uint64_t address, uint8_t level, uint64_t *copy)
{
	struct btrfs_transaction *transaction = balance->transaction;
	struct bt_mutation *mutation = transaction->mutation;
	struct bt_extent_ref *refs;
	const struct bt_disk_header *header;
	struct bt_key key = { address, level, BT_METADATA_ITEM };
	struct bt_key first = { 0 };
	uint64_t flags = 0;
	uint64_t generation = 0;
	uint64_t parent = 0;
	uint64_t moved = 0;
	size_t count = 0;
	size_t index = 0;
	size_t i;
	uint32_t slot = 0;
	int has_first = 0;
	enum btrfs_result error;

	*copy = bt_bl_copy_of(balance, address);
	if (*copy != 0) {
		return BTRFS_OK;
	}
	if (level >= BT_MAX_LEVEL) {
		return BTRFS_CORRUPT;
	}
	refs = balance->refs[level];
	error = bt_backref_list(mutation, transaction->extents.root, key, &flags, &generation, refs,
	    BT_BALANCE_REFS, &count);
	if (error == BTRFS_RANGE) {
		error = BTRFS_UNSUPPORTED;
	} else if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		error = bt_tree_read(bt_mutation_view(mutation),
		    (struct bt_root){ address, generation, BT_OWNER_ANY, level },
		    balance->nodes[level]);
	}
	if (error == BTRFS_OK) {
		header = (const void *)balance->nodes[level];
		has_first = bt_bl_first(balance->nodes[level], &first);
		error = bt_file_tree(bt_u64(header->owner)) ? BTRFS_OK : BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		error = bt_mutation_relocate(mutation,
		    (struct bt_root){ address, generation, BT_OWNER_ANY, level }, &moved, &index);
	}
	if (error == BTRFS_OK) {
		error = index < BT_TRANSACTION_NODES ? bt_bl_remember(balance, address, moved)
						     : BTRFS_NO_SPACE;
	}
	/* Its extent item and space move below, not as a CoW's would. */
	if (error == BTRFS_OK) {
		transaction->accounted[index] = BT_ACCOUNT_ORIGINAL | BT_ACCOUNT_NEW;
	}
	/* Children it names by address follow it, added before dropped so that
	 * none is freed on the way. */
	if (error == BTRFS_OK && (flags & BT_EXTENT_FLAG_FULL_BACKREF) != 0) {
		error = bt_tx_children(transaction, balance->nodes[level], moved, 1, 0, 1);
		if (error == BTRFS_OK) {
			error =
			    bt_tx_children(transaction, balance->nodes[level], address, 1, 0, 0);
		}
	}
	for (i = 0; error == BTRFS_OK && i < count; i++) {
		if (refs[i].type == BT_TREE_BLOCK_REF) {
			error = bt_bl_keyed_parent(balance, refs[i].root, address, level,
			    has_first ? &first : NULL, &parent, &slot);
			if (error == BTRFS_OK && parent == 0) {
				error = bt_bl_root(balance, refs[i].root, address, moved, level);
			} else if (error == BTRFS_OK) {
				error =
				    bt_bl_link(balance, parent, level + 1, slot, address, moved);
			}
		} else if (refs[i].type == BT_SHARED_BLOCK_REF && has_first &&
		    level + 1 < BT_MAX_LEVEL) {
			error = bt_bl_read(
			    balance, refs[i].parent, level + 1, balance->nodes[level + 1]);
			if (error == BTRFS_OK) {
				error =
				    bt_bl_slot(balance->nodes[level + 1], first, address, &slot);
			}
			if (error == BTRFS_OK) {
				error = bt_bl_link(
				    balance, refs[i].parent, level + 1, slot, address, moved);
			}
		} else {
			error = BTRFS_CORRUPT;
		}
	}
	if (error == BTRFS_OK) {
		error = bt_backref_move(
		    mutation, &transaction->extents.root, key, moved, balance->generation);
	}
	if (error == BTRFS_OK) {
		error = bt_space_change_used(
		    transaction->space, address, transaction->base->info.node_size, 0);
	}
	if (error == BTRFS_OK) {
		error = bt_space_change_used(
		    transaction->space, moved, transaction->base->info.node_size, 1);
	}
	if (error == BTRFS_OK) {
		balance->blocks++;
		*copy = moved;
	}
	return error;
}

/* The root of a tree that is never shared, whose blocks move with the CoW of
 * their path; NULL for a tree this filesystem lacks. */
static enum btrfs_result
bt_bl_unshared_root(struct btrfs_transaction *transaction, uint64_t tree, struct bt_root **root)
{
	struct bt_owned_root *owned;
	enum btrfs_result error = BTRFS_OK;

	*root = NULL;
	switch (tree) {
	case BT_ROOT_TREE:
		*root = &transaction->roots;
		break;
	case BT_EXTENT_TREE:
		*root = &transaction->extents.root;
		break;
	case BT_CHUNK_TREE:
		*root = &transaction->chunks;
		break;
	case BT_DEV_TREE:
		*root = &transaction->devices.root;
		break;
	case BT_CSUM_TREE:
		*root = &transaction->checksums.root;
		break;
	case BT_FREE_SPACE_TREE:
		*root = transaction->has_free_space ? &transaction->free_space.root : NULL;
		break;
	case BT_BLOCK_GROUP_TREE:
		*root = transaction->has_group_tree ? &transaction->groups.root : NULL;
		break;
	case BT_QUOTA_TREE:
		*root = transaction->qgroups != NULL ? &transaction->quota.root : NULL;
		break;
	case BT_UUID_TREE:
		*root = transaction->has_uuids ? &transaction->uuids.root : NULL;
		break;
	case BT_DATA_RELOC_TREE:
		error = bt_tx_tree(transaction, tree, &owned);
		*root = error == BTRFS_OK ? &owned->root : NULL;
		break;
	default:
		break;
	}
	return error == BTRFS_OK && *root == NULL ? BTRFS_UNSUPPORTED : error;
}

/* The node at level on key's path from root, as a CoW of that path takes it:
 * at each node the last slot whose key is at most key, or the first. Nodes
 * above level are read into the buffers of their levels. */
static enum btrfs_result
bt_bl_path(struct bt_balance *balance, struct bt_root root, struct bt_key key, uint8_t level,
    uint64_t *address)
{
	const struct btrfs_fs *view = bt_mutation_view(balance->transaction->mutation);
	const struct bt_disk_header *header;
	const struct bt_disk_pointer *pointers;
	uint32_t slot;
	uint32_t count;
	uint8_t at;
	enum btrfs_result error = BTRFS_OK;

	for (at = root.level; error == BTRFS_OK && at > level; at--) {
		error = bt_tree_read(view, root, balance->nodes[at]);
		if (error != BTRFS_OK) {
			break;
		}
		header = (const void *)balance->nodes[at];
		pointers = (const void *)(header + 1);
		count = bt_u32(header->count);
		for (slot = 0; slot + 1 < count &&
		    bt_key_compare(bt_key_decode(&pointers[slot + 1].key), key) <= 0;
		    slot++) {
		}
		root.address = bt_u64(pointers[slot].bytenr);
		root.generation = bt_u64(pointers[slot].generation);
		root.level = at - 1;
	}
	*address = root.address;
	return error;
}

/* Moves a block of a tree that is never shared with the CoW of its path;
 * a path already copied in this transaction moved it before. node holds
 * the block. */
static enum btrfs_result
bt_bl_unshared(
    struct bt_balance *balance, uint64_t tree, uint64_t address, uint8_t level, const uint8_t *node)
{
	struct btrfs_transaction *transaction = balance->transaction;
	struct bt_root *root = NULL;
	struct bt_key first = { 0 };
	uint64_t found = 0;
	enum btrfs_result error;

	error = bt_bl_unshared_root(transaction, tree, &root);
	if (error != BTRFS_OK) {
		return error;
	}
	if (root->address == address) {
		error = root->level == level ? BTRFS_OK : BTRFS_CORRUPT;
		if (error == BTRFS_OK) {
			error = bt_mutation_cow_path(transaction->mutation, root, first, level);
		}
		return error;
	}
	/* A tree this transaction shortened below the block's level dropped it. */
	if (root->level < level) {
		return bt_mutation_owns(transaction->mutation, root->address) ? BTRFS_OK
									      : BTRFS_CORRUPT;
	}
	if (!bt_bl_first(node, &first)) {
		return BTRFS_CORRUPT;
	}
	error = bt_bl_path(balance, *root, first, level, &found);
	if (error == BTRFS_OK && found != address) {
		return bt_mutation_owns(transaction->mutation, found) ? BTRFS_OK : BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		error = bt_mutation_cow_path(transaction->mutation, root, first, level);
	}
	if (error == BTRFS_OK) {
		balance->blocks++;
	}
	return error;
}

/* Moves the tree block at address and level, of whichever tree it is. */
static enum btrfs_result
bt_bl_tree_block(struct bt_balance *balance, uint64_t address, uint8_t level)
{
	const struct bt_disk_header *header;
	uint64_t copy = 0;
	uint64_t owner;
	enum btrfs_result error;

	if (level >= BT_MAX_LEVEL) {
		return BTRFS_CORRUPT;
	}
	error = bt_bl_read(balance, address, level, balance->nodes[level]);
	if (error != BTRFS_OK) {
		return error;
	}
	header = (const void *)balance->nodes[level];
	owner = bt_u64(header->owner);
	return bt_file_tree(owner)
	    ? bt_bl_block(balance, address, level, &copy)
	    : bt_bl_unshared(balance, owner, address, level, balance->nodes[level]);
}

/* Linux's find_data_references for one keyed data reference: the file
 * extent items of its inode in its tree's leaves the tree owns, apart from
 * shared parents of the extent, that name the extent at the reference's
 * offset, until its count is found. Out of line, as bt_bl_keyed_parent. */
static BT_NOINLINE enum btrfs_result
bt_bl_data_leaves(struct bt_balance *balance, const struct bt_extent_ref *ref, uint64_t extent,
    const struct bt_extent_ref *refs, size_t count, size_t *items)
{
	const struct bt_disk_extent *file;
	const struct bt_disk_header *header;
	struct bt_owned_root owned;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { ref->inode, ref->offset >= BT_BALANCE_OFFSET_LIMIT ? 0 : ref->offset,
		BT_EXTENT_DATA };
	uint64_t matched = 0;
	uint64_t steps;
	uint64_t leaf;
	size_t i;
	int skip;
	enum btrfs_result error;

	error = bt_bl_tree(balance, ref->root, &owned);
	if (error != BTRFS_OK) {
		return error;
	}
	bt_cursor_init(&cursor, bt_mutation_view(balance->transaction->mutation), owned.root);
	error = bt_cursor_seek(&cursor, key, 0);
	for (steps = 0; error == BTRFS_OK && matched < ref->count; steps++) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != ref->inode || record.key.type != BT_EXTENT_DATA) {
			break;
		}
		if (steps == BT_MAX_TREE_ITEMS) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		leaf = cursor.loaded[0].address;
		header = (const void *)cursor.blocks[0];
		file = (const void *)record.data;
		skip = bt_u64(header->owner) != ref->root ||
		    (record.size >= sizeof(file->header) && file->header.type == BT_EXTENT_INLINE);
		for (i = 0; i < count; i++) {
			skip |= refs[i].type == BT_SHARED_DATA_REF && refs[i].parent == leaf;
		}
		if (!skip && record.size != sizeof(*file)) {
			error = BTRFS_CORRUPT;
			break;
		}
		if (!skip && bt_u64(file->disk_bytenr) == extent &&
		    record.key.offset - bt_u64(file->offset) == ref->offset) {
			if (*items == BT_BALANCE_ITEMS) {
				error = BTRFS_UNSUPPORTED;
				break;
			}
			balance->items[(*items)++] = (struct bt_bl_item){ leaf, cursor.slots[0] };
			matched++;
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_OK;
	}
	return error == BTRFS_OK && matched != ref->count ? BTRFS_CORRUPT : error;
}

/* The file extent items of a shared parent leaf that name the extent; their
 * number is the reference's count. */
static enum btrfs_result
bt_bl_shared_leaf(struct bt_balance *balance, const struct bt_extent_ref *ref, struct bt_key extent,
    size_t *items)
{
	const struct bt_disk_header *header = (const void *)balance->nodes[0];
	const struct bt_disk_item *entries = (const void *)(header + 1);
	const struct bt_disk_extent *file;
	uint64_t matched = 0;
	uint32_t slot;
	enum btrfs_result error;

	error = bt_bl_read(balance, ref->parent, 0, balance->nodes[0]);
	if (error == BTRFS_OK && !bt_file_tree(bt_u64(header->owner))) {
		error = BTRFS_CORRUPT;
	}
	for (slot = 0; error == BTRFS_OK && slot < bt_u32(header->count); slot++) {
		file = (const void *)((const uint8_t *)(header + 1) + bt_u32(entries[slot].offset));
		if (bt_key_decode(&entries[slot].key).type != BT_EXTENT_DATA ||
		    bt_u32(entries[slot].size) != sizeof(*file) ||
		    bt_u64(file->disk_bytenr) != extent.objectid) {
			continue;
		}
		if (*items == BT_BALANCE_ITEMS) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		balance->items[(*items)++] = (struct bt_bl_item){ ref->parent, slot };
		matched++;
	}
	return error == BTRFS_OK && matched != ref->count ? BTRFS_CORRUPT : error;
}

/* The checksums of a window of a data extent, from the committed checksum
 * tree: covered marks the sectors that have one, sums holds them. Out of line,
 * as bt_bl_keyed_parent. */
static BT_NOINLINE enum btrfs_result
bt_bl_sums(struct bt_balance *balance, uint64_t logical, uint64_t length)
{
	const struct btrfs_fs *base = balance->transaction->base;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { BT_CSUM_OBJECTID, logical, BT_EXTENT_CSUM };
	uint64_t sector = base->info.sector_size;
	uint64_t start;
	uint64_t end;
	uint64_t position;
	uint64_t steps;
	size_t size = bt_checksum_size(base->info.checksum_type);
	enum btrfs_result error;

	bt_zero(balance->covered, sizeof(balance->covered));
	bt_cursor_init(&cursor, base, base->checksum_tree);
	error = bt_cursor_seek(&cursor, key, 1);
	for (steps = 0; error == BTRFS_OK; steps++) {
		(void)bt_cursor_record(&cursor, &record);
		if (steps == BT_MAX_TREE_ITEMS) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		if (record.key.objectid != BT_CSUM_OBJECTID || record.key.type != BT_EXTENT_CSUM) {
			if (record.key.objectid > BT_CSUM_OBJECTID ||
			    (record.key.objectid == BT_CSUM_OBJECTID &&
				record.key.type > BT_EXTENT_CSUM)) {
				break;
			}
			error = bt_cursor_next(&cursor);
			continue;
		}
		if (record.key.offset >= logical + length) {
			break;
		}
		if (record.key.offset % sector != 0 || record.size % size != 0 ||
		    record.size / size > (UINT64_MAX - record.key.offset) / sector) {
			error = BTRFS_CORRUPT;
			break;
		}
		start = record.key.offset > logical ? record.key.offset : logical;
		end = record.key.offset + record.size / size * sector;
		end = end < logical + length ? end : logical + length;
		for (position = start; position < end; position += sector) {
			balance->covered[(position - logical) / sector] = 1;
			bt_copy(balance->sums + (position - logical) / sector * size,
			    record.data + (position - record.key.offset) / sector * size, size);
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* Copies a data extent's bytes, verified where they have checksums, and
 * those checksums to the copy at to; the original's checksums go. */
static enum btrfs_result
bt_bl_copy_data(struct bt_balance *balance, uint64_t from, uint64_t to, uint64_t length)
{
	struct btrfs_transaction *transaction = balance->transaction;
	const struct btrfs_fs *base = transaction->base;
	uint64_t sector = base->info.sector_size;
	uint64_t position;
	uint64_t window;
	size_t size = bt_checksum_size(base->info.checksum_type);
	size_t sectors;
	size_t i;
	size_t j;
	enum btrfs_result error = BTRFS_OK;

	for (position = 0; error == BTRFS_OK && position < length; position += window) {
		window =
		    length - position < BT_BALANCE_WINDOW ? length - position : BT_BALANCE_WINDOW;
		sectors = (size_t)(window / sector);
		error = bt_bl_sums(balance, from + position, window);
		for (i = 0; error == BTRFS_OK && i < sectors; i = j) {
			for (j = i + 1; j < sectors && balance->covered[j] == balance->covered[i];
			    j++) {
			}
			error = bt_data_read(base, from + position + i * sector,
			    balance->window + i * sector, (j - i) * sector, balance->covered[i]);
		}
		if (error == BTRFS_OK) {
			error =
			    bt_tx_write_data(transaction, to + position, balance->window, window);
		}
		for (i = 0; error == BTRFS_OK && i < sectors; i = j) {
			for (j = i + 1; j < sectors && balance->covered[j] == balance->covered[i];
			    j++) {
			}
			if (balance->covered[i]) {
				error = bt_csum_insert_sums(transaction->mutation,
				    &transaction->checksums.root, to + position + i * sector,
				    balance->sums + i * size, (j - i) * sector);
			}
		}
	}
	if (error == BTRFS_OK) {
		error = bt_csum_delete(
		    transaction->mutation, &transaction->checksums.root, from, length);
	}
	return error;
}

/* Moves a data extent: its bytes and checksums to a copy, every file extent
 * item naming it to the copy, and its extent item with its references. */
static enum btrfs_result
bt_bl_data(struct bt_balance *balance, struct bt_key extent)
{
	struct btrfs_transaction *transaction = balance->transaction;
	struct bt_mutation *mutation = transaction->mutation;
	struct bt_extent_ref *refs = balance->refs[BT_MAX_LEVEL];
	struct bt_disk_extent *file;
	const struct bt_disk_header *header;
	const struct bt_disk_item *entries;
	uint8_t *bytes = NULL;
	uint64_t flags = 0;
	uint64_t generation = 0;
	uint64_t moved = 0;
	uint64_t leaf;
	size_t count = 0;
	size_t items = 0;
	size_t i;
	enum btrfs_result error;

	error = bt_backref_list(mutation, transaction->extents.root, extent, &flags, &generation,
	    refs, BT_BALANCE_REFS, &count);
	if (error == BTRFS_RANGE) {
		error = BTRFS_UNSUPPORTED;
	} else if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK &&
	    (extent.offset == 0 || extent.objectid % transaction->base->info.sector_size != 0 ||
		extent.offset % transaction->base->info.sector_size != 0 ||
		extent.offset > balance->end - extent.objectid)) {
		error = BTRFS_CORRUPT;
	}
	/* Every item is found before any leaf moves, as moves change the
	 * leaves the references name. */
	for (i = 0; error == BTRFS_OK && i < count; i++) {
		/* The data of a v1 space cache inode of the root tree, which
		 * Linux deletes with its group, stays where it is here. */
		if (refs[i].type == BT_EXTENT_DATA_REF && refs[i].root == BT_ROOT_TREE) {
			error = BTRFS_UNSUPPORTED;
		} else if (refs[i].type == BT_EXTENT_DATA_REF) {
			error = bt_bl_data_leaves(
			    balance, &refs[i], extent.objectid, refs, count, &items);
		} else if (refs[i].type == BT_SHARED_DATA_REF) {
			error = bt_bl_shared_leaf(balance, &refs[i], extent, &items);
		} else {
			error = BTRFS_CORRUPT;
		}
	}
	if (error == BTRFS_OK) {
		error = bt_space_reserve_exact(transaction->space, extent.offset, &moved);
	}
	if (error == BTRFS_OK) {
		error = bt_bl_copy_data(balance, extent.objectid, moved, extent.offset);
	}
	for (i = 0; error == BTRFS_OK && i < items; i++) {
		leaf = balance->items[i].leaf;
		if (!bt_mutation_owns(mutation, leaf)) {
			error = bt_bl_block(balance, leaf, 0, &leaf);
		}
		if (error == BTRFS_OK) {
			error = bt_mutation_bytes(mutation, leaf, &bytes);
		}
		if (error != BTRFS_OK) {
			break;
		}
		header = (const void *)bytes;
		entries = (const void *)(header + 1);
		if (header->level != 0 || balance->items[i].slot >= bt_u32(header->count) ||
		    bt_u32(entries[balance->items[i].slot].size) != sizeof(*file)) {
			error = BTRFS_CORRUPT;
			break;
		}
		file = (void *)(bytes + sizeof(*header) +
		    bt_u32(entries[balance->items[i].slot].offset));
		if (bt_u64(file->disk_bytenr) != extent.objectid ||
		    bt_u64(file->disk_bytes) != extent.offset) {
			error = BTRFS_CORRUPT;
			break;
		}
		bt_put64(&file->disk_bytenr, moved);
	}
	if (error == BTRFS_OK) {
		error = bt_backref_move(
		    mutation, &transaction->extents.root, extent, moved, generation);
	}
	if (error == BTRFS_OK) {
		error = bt_space_change_used(transaction->space, extent.objectid, extent.offset, 0);
	}
	if (error == BTRFS_OK) {
		error = bt_space_change_used(transaction->space, moved, extent.offset, 1);
	}
	if (error == BTRFS_OK) {
		balance->extents++;
		balance->bytes += extent.offset;
	}
	return error;
}

/* The first extent item at or after position within the group, as this
 * transaction sees it; *found is zero when none remains. */
static enum btrfs_result
bt_bl_next(struct bt_balance *balance, uint64_t position, struct bt_key *extent, int *found)
{
	struct btrfs_transaction *transaction = balance->transaction;
	struct bt_cursor cursor;
	struct bt_record record;
	uint64_t steps;
	enum btrfs_result error;

	*found = 0;
	bt_cursor_init(&cursor, bt_mutation_view(transaction->mutation), transaction->extents.root);
	error = bt_cursor_seek(&cursor, (struct bt_key){ position, 0, 0 }, 0);
	for (steps = 0; error == BTRFS_OK; steps++) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid >= balance->end) {
			break;
		}
		if (steps == BT_MAX_TREE_ITEMS) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		if (record.key.type == BT_EXTENT_ITEM || record.key.type == BT_METADATA_ITEM) {
			*extent = record.key;
			*found = 1;
			break;
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* Whether the root tree names a v1 space-cache inode for the group, which
 * Linux deletes with it (btrfs_remove_free_space_inode); this writer does
 * not delete one. */
static enum btrfs_result
bt_bl_cached(struct btrfs_transaction *transaction, uint64_t logical)
{
	struct bt_key key = { BT_FREE_SPACE_OBJECTID, logical, 0 };
	uint8_t header[64];
	size_t length;
	enum btrfs_result error;

	error = bt_mutation_find(
	    transaction->mutation, transaction->roots, key, header, sizeof(header), &length);
	if (error == BTRFS_NOT_FOUND) {
		return BTRFS_OK;
	}
	return error == BTRFS_OK || error == BTRFS_RANGE ? BTRFS_UNSUPPORTED : error;
}

/* Takes the emptied group out, as btrfs_remove_chunk: its items go at
 * commit. When it is the last of its kind with its profile, or with the
 * profile a conversion gives the kind, a new chunk of them takes its place
 * first, as Linux's balance allocates one before it relocates the last. */
static enum btrfs_result
bt_bl_remove(struct btrfs_transaction *transaction, size_t chunk, uint64_t profile)
{
	const struct bt_chunk *group = &transaction->fs.chunks[chunk];
	uint64_t kind = group->type & BT_BALANCE_KINDS;
	uint64_t type = profile == BTRFS_BALANCE_KEEP ? group->type : kind | profile;
	size_t i;
	int sibling = 0;
	enum btrfs_result error = BTRFS_OK;

	if (bt_space_used(transaction->space, chunk) != 0) {
		return BTRFS_CORRUPT;
	}
	for (i = 0; i < transaction->fs.chunk_count; i++) {
		sibling |= i != chunk && !transaction->fs.chunks[i].removed &&
		    transaction->fs.chunks[i].type == type;
	}
	if (!sibling) {
		error = bt_space_grow(transaction->space, kind, 0);
	}
	if (error == BTRFS_OK) {
		error = bt_space_check_system(transaction->space);
	}
	if (error == BTRFS_OK) {
		error = bt_space_retire(transaction->space, chunk);
	}
	return error;
}

static void
bt_bl_free(const struct btrfs_environment *env, struct bt_balance *balance, size_t node_size)
{
	size_t i;

	if (balance->copies != NULL) {
		env->release(
		    env->context, balance->copies, BT_BALANCE_COPIES * sizeof(*balance->copies));
	}
	for (i = 0; i <= BT_MAX_LEVEL; i++) {
		if (balance->refs[i] != NULL) {
			env->release(env->context, balance->refs[i],
			    BT_BALANCE_REFS * sizeof(*balance->refs[i]));
		}
		if (i < BT_MAX_LEVEL && balance->nodes[i] != NULL) {
			env->release(env->context, balance->nodes[i], node_size);
		}
	}
	if (balance->items != NULL) {
		env->release(
		    env->context, balance->items, BT_BALANCE_ITEMS * sizeof(*balance->items));
	}
	if (balance->window != NULL) {
		env->release(env->context, balance->window, BT_BALANCE_WINDOW);
	}
	if (balance->sums != NULL) {
		env->release(env->context, balance->sums, BT_BALANCE_SECTORS * BT_CSUM_SIZE);
	}
}

static enum btrfs_result
bt_bl_allocate(const struct btrfs_environment *env, struct bt_balance *balance, size_t node_size)
{
	size_t i;

	balance->copies = env->allocate(env->context, BT_BALANCE_COPIES * sizeof(*balance->copies));
	if (balance->copies == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(balance->copies, BT_BALANCE_COPIES * sizeof(*balance->copies));
	for (i = 0; i <= BT_MAX_LEVEL; i++) {
		balance->refs[i] =
		    env->allocate(env->context, BT_BALANCE_REFS * sizeof(*balance->refs[i]));
		if (balance->refs[i] == NULL) {
			return BTRFS_NO_MEMORY;
		}
		if (i < BT_MAX_LEVEL) {
			balance->nodes[i] = env->allocate(env->context, node_size);
			if (balance->nodes[i] == NULL) {
				return BTRFS_NO_MEMORY;
			}
		}
	}
	balance->items = env->allocate(env->context, BT_BALANCE_ITEMS * sizeof(*balance->items));
	balance->window = env->allocate(env->context, BT_BALANCE_WINDOW);
	balance->sums = env->allocate(env->context, BT_BALANCE_SECTORS * BT_CSUM_SIZE);
	return balance->items == NULL || balance->window == NULL || balance->sums == NULL
	    ? BTRFS_NO_MEMORY
	    : BTRFS_OK;
}

/* A conversion of the group's kind to profile, as Linux's balance converts:
 * new chunks of the kind take the profile, and no group of the kind with
 * another one takes allocation. */
static enum btrfs_result
bt_bl_convert(struct btrfs_transaction *transaction, size_t chunk, uint64_t profile)
{
	const struct btrfs_fs *fs = &transaction->fs;
	uint64_t kind = fs->chunks[chunk].type & BT_BALANCE_KINDS;
	size_t count = fs->chunk_count;
	size_t i;
	enum btrfs_result error;

	error = bt_space_set_profile(transaction->space, kind, profile);
	for (i = 0; error == BTRFS_OK && i < count; i++) {
		if (i != chunk && !fs->chunks[i].removed &&
		    (fs->chunks[i].type & BT_BALANCE_KINDS) == kind &&
		    (fs->chunks[i].type & ~BT_BALANCE_KINDS) != profile) {
			error = bt_space_readonly(transaction->space, i);
		}
	}
	return error;
}

/* One committed step: extents of the group move until the work reaches
 * work (one extent when single), or the emptied group goes (*done); with a
 * profile other than BTRFS_BALANCE_KEEP, its kind converts to it. On the
 * device named limited (none for 0) no new chunk takes a stripe ending past
 * limit. *moved counts the extents moved. */
static enum btrfs_result
bt_bl_step(struct btrfs_fs *fs, const struct btrfs_write_environment *writer, uint64_t group,
    size_t work, int single, uint64_t profile, uint64_t limited, uint64_t limit,
    struct btrfs_balance_report *report, int *done, uint64_t *moved)
{
	size_t device;
	struct btrfs_transaction *transaction = NULL;
	struct bt_balance balance;
	struct bt_key extent = { 0 };
	uint64_t position;
	size_t chunk = 0;
	int found = 1;
	enum btrfs_result error;

	*done = 0;
	*moved = 0;
	bt_zero(&balance, sizeof(balance));
	error = btrfs_transaction_begin(fs, writer, &transaction);
	if (error == BTRFS_OK) {
		transaction->balancing = 1;
		chunk = bt_chunk_containing(&transaction->fs, group);
		if (chunk == transaction->fs.chunk_count ||
		    transaction->fs.chunks[chunk].logical != group ||
		    transaction->fs.chunks[chunk].removed) {
			error = BTRFS_NOT_FOUND;
		}
	}
	if (error == BTRFS_OK && limited != 0) {
		device = bt_tx_device_index(transaction, limited);
		if (device == transaction->fs.device_count) {
			error = BTRFS_NOT_FOUND;
		} else {
			bt_space_limit_device(transaction->space, device, limit);
		}
	}
	/* A running rescan counts extents behind its progress. */
	if (error == BTRFS_OK && transaction->qgroups != NULL &&
	    bt_qgroup_rescanning(transaction)) {
		error = BTRFS_UNSUPPORTED;
	}
	if (error == BTRFS_OK) {
		error = bt_bl_cached(transaction, group);
	}
	if (error == BTRFS_OK) {
		error = bt_space_readonly(transaction->space, chunk);
	}
	if (error == BTRFS_OK && profile != BTRFS_BALANCE_KEEP) {
		error = bt_bl_convert(transaction, chunk, profile);
	}
	if (error == BTRFS_OK) {
		balance.transaction = transaction;
		balance.start = group;
		balance.end = group + transaction->fs.chunks[chunk].length;
		balance.generation = fs->info.generation + 1;
		error = bt_bl_allocate(&fs->env, &balance, fs->info.node_size);
	}
	for (position = group; error == BTRFS_OK && found;) {
		error = bt_bl_next(&balance, position, &extent, &found);
		if (error != BTRFS_OK || !found) {
			break;
		}
		if (extent.type == BT_METADATA_ITEM) {
			error = extent.offset < BT_MAX_LEVEL
			    ? bt_bl_tree_block(&balance, extent.objectid, (uint8_t)extent.offset)
			    : BTRFS_CORRUPT;
			position = extent.objectid + fs->info.node_size;
		} else {
			error = bt_bl_data(&balance, extent);
			position = extent.objectid + extent.offset;
		}
		if (error == BTRFS_OK) {
			(*moved)++;
		}
		if (error == BTRFS_OK && (single || bt_tx_work(transaction) >= work)) {
			break;
		}
	}
	if (error == BTRFS_OK && *moved == 0) {
		error = bt_bl_remove(transaction, chunk, profile);
		*done = error == BTRFS_OK;
	}
	if (error == BTRFS_OK) {
		transaction->changed = 1;
		error = btrfs_transaction_commit(transaction);
	}
	if (error == BTRFS_OK) {
		report->groups += (uint64_t)*done;
		report->blocks += balance.blocks;
		report->extents += balance.extents;
		report->bytes += balance.bytes;
		report->commits++;
	}
	bt_bl_free(&fs->env, &balance, fs->info.node_size);
	btrfs_transaction_destroy(transaction);
	return error;
}

/* Drops deleted subvolumes first: a tree being dropped has no parents to
 * redirect. */
static enum btrfs_result
bt_bl_clean(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, struct btrfs_balance_report *report)
{
	struct btrfs_transaction *transaction;
	struct btrfs_fs *fs;
	uint64_t steps;
	size_t dropped;
	int pending = 1;
	enum btrfs_result error = BTRFS_OK;

	for (steps = 0; error == BTRFS_OK && pending; steps++) {
		fs = NULL;
		transaction = NULL;
		dropped = 0;
		pending = 0;
		error = steps == BT_BALANCE_STEPS ? BTRFS_UNSUPPORTED : BTRFS_OK;
		if (error == BTRFS_OK) {
			error = btrfs_mount(environment, BTRFS_TOP_LEVEL_TREE, &fs);
		}
		if (error == BTRFS_OK) {
			error = btrfs_transaction_begin(fs, writer, &transaction);
		}
		if (error == BTRFS_OK) {
			error = btrfs_transaction_clean_subvolumes(
			    transaction, BT_BALANCE_DROP_BLOCKS, &dropped, &pending);
		}
		if (error == BTRFS_OK && transaction->changed) {
			error = btrfs_transaction_commit(transaction);
			report->commits += error == BTRFS_OK;
		} else if (error == BTRFS_OK && pending) {
			error = BTRFS_CORRUPT;
		}
		btrfs_transaction_destroy(transaction);
		btrfs_unmount(fs);
	}
	return error;
}

/* Relocates one group in committed steps, its kind converting to profile
 * unless it is BTRFS_BALANCE_KEEP, no new chunk taking a stripe past limit on
 * the device named limited (none for 0); report accumulates. */
static enum btrfs_result
bt_bl_relocate(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t group, size_t work, uint64_t profile,
    uint64_t limited, uint64_t limit, struct btrfs_balance_report *report)
{
	struct btrfs_fs *fs;
	uint64_t steps;
	uint64_t moved = 0;
	int done = 0;
	enum btrfs_result error = BTRFS_OK;

	for (steps = 0; error == BTRFS_OK && !done; steps++) {
		fs = NULL;
		error = steps == BT_BALANCE_STEPS ? BTRFS_UNSUPPORTED : BTRFS_OK;
		if (error == BTRFS_OK) {
			error = btrfs_mount(environment, BTRFS_TOP_LEVEL_TREE, &fs);
		}
		if (error == BTRFS_OK) {
			error = bt_bl_step(fs, writer, group, work, 0, profile, limited, limit,
			    report, &done, &moved);
			/* A step too large for one transaction is retried one extent at a
			 * time. */
			if (error != BTRFS_OK && error != BTRFS_NOT_FOUND &&
			    error != BTRFS_UNSUPPORTED && error != BTRFS_CORRUPT &&
			    error != BTRFS_IO) {
				error = bt_bl_step(fs, writer, group, work, 1, profile, limited,
				    limit, report, &done, &moved);
			}
		}
		btrfs_unmount(fs);
	}
	return error;
}

enum btrfs_result
bt_relocate_group(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t group, size_t work,
    struct btrfs_balance_report *report)
{
	enum btrfs_result error;

	if (environment == NULL || writer == NULL || report == NULL || work == 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	bt_zero(report, sizeof(*report));
	error = bt_bl_clean(environment, writer, report);
	return error == BTRFS_OK
	    ? bt_bl_relocate(environment, writer, group, work, BTRFS_BALANCE_KEEP, 0, 0, report)
	    : error;
}

/* The group with a stripe on the device named id ending past limit whose
 * stripe there lies highest; NOT_FOUND when none has. */
static enum btrfs_result
bt_bl_device_group(const struct btrfs_fs *fs, uint64_t id, uint64_t limit, uint64_t *group)
{
	const struct bt_chunk *chunk;
	uint64_t highest = 0;
	size_t device;
	size_t i;
	unsigned stripe;
	int found = 0;

	for (device = 0; device < fs->device_count && fs->devices[device].id != id; device++) {
	}
	if (device == fs->device_count) {
		return BTRFS_NOT_FOUND;
	}
	for (i = 0; i < fs->chunk_count; i++) {
		chunk = &fs->chunks[i];
		for (stripe = 0; stripe < chunk->stripes; stripe++) {
			if (chunk->devices[stripe] == device &&
			    chunk->physical[stripe] + bt_chunk_stripe_length(chunk) > limit &&
			    (!found || chunk->physical[stripe] > highest)) {
				highest = chunk->physical[stripe];
				*group = chunk->logical;
				found = 1;
			}
		}
	}
	return found ? BTRFS_OK : BTRFS_NOT_FOUND;
}

enum btrfs_result
bt_relocate_device(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t id, uint64_t limit, size_t work,
    struct btrfs_balance_report *report)
{
	struct btrfs_fs *fs;
	uint64_t group = 0;
	uint64_t groups;
	uint64_t bound = 0;
	enum btrfs_result error;

	error = bt_bl_clean(environment, writer, report);
	/* Each relocation takes one group off the range and puts none there,
	 * so at most as many as the volume held at first. */
	for (groups = 0; error == BTRFS_OK; groups++) {
		fs = NULL;
		error = btrfs_mount(environment, BTRFS_TOP_LEVEL_TREE, &fs);
		if (error == BTRFS_OK) {
			bound = groups == 0 ? fs->chunk_count : bound;
			error = bt_bl_device_group(fs, id, limit, &group);
		}
		btrfs_unmount(fs);
		if (error == BTRFS_NOT_FOUND) {
			return BTRFS_OK;
		}
		if (error == BTRFS_OK && groups == bound) {
			error = BTRFS_CORRUPT;
		}
		if (error == BTRFS_OK) {
			error = bt_bl_relocate(environment, writer, group, work, BTRFS_BALANCE_KEEP,
			    id, limit, report);
		}
	}
	return error;
}

/* The arguments of a group's kind: a mixed group's are its data's. */
static const struct btrfs_balance_kind *
bt_bl_kind(const struct btrfs_balance_args *args, uint64_t type)
{
	if (type & BT_BLOCK_SYSTEM) {
		return &args->system;
	}
	return type & BT_BLOCK_DATA ? &args->data : &args->metadata;
}

/* Linux's chunk_usage_filter and chunk_soft_convert_filter: whether the
 * arguments of its kind select a group. */
static int
bt_bl_selects(const struct btrfs_balance_kind *kind, const struct btrfs_group *group)
{
	uint64_t threshold = kind->usage == 0 ? 1
	    : kind->usage > 100
	    ? group->length
	    : group->length / 100 * kind->usage + group->length % 100 * kind->usage / 100;

	if (!kind->enabled || group->used >= threshold) {
		return 0;
	}
	return !(kind->soft && kind->convert != BTRFS_BALANCE_KEEP &&
	    (group->flags & ~BT_BALANCE_KINDS) == kind->convert);
}

/* Linux's validate_convert_profile: a target is one profile, one this
 * writer allocates (RAID5 and RAID6 are UNSUPPORTED), on no fewer devices
 * than it needs (INVALID_ARGUMENT). */
static enum btrfs_result
bt_bl_target(const struct btrfs_balance_kind *kind, size_t devices, int *redundant)
{
	unsigned fewest;

	*redundant = 1;
	if (!kind->enabled || kind->convert == BTRFS_BALANCE_KEEP) {
		return BTRFS_OK;
	}
	if (bt_space_profile(kind->convert, &fewest, redundant) != BTRFS_OK) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if ((kind->convert & (BT_BLOCK_RAID5 | BT_BLOCK_RAID6)) != 0) {
		return BTRFS_UNSUPPORTED;
	}
	return fewest > devices ? BTRFS_INVALID_ARGUMENT : BTRFS_OK;
}

/* Linux's btrfs_balance argument checks against the mounted volume: valid
 * targets, and metadata or system redundancy reduced only with force while
 * some group of the kind has it. */
static enum btrfs_result
bt_bl_arguments(const struct btrfs_fs *fs, const struct btrfs_balance_args *args)
{
	int redundant[3];
	int present[3] = { 0, 0, 0 };
	uint64_t kinds[3] = { BT_BLOCK_DATA, BT_BLOCK_METADATA, BT_BLOCK_SYSTEM };
	const struct btrfs_balance_kind *targets[3] = { &args->data, &args->metadata,
		&args->system };
	unsigned fewest;
	int has;
	size_t i;
	size_t k;
	enum btrfs_result error = BTRFS_OK;

	for (k = 0; error == BTRFS_OK && k < 3; k++) {
		error = bt_bl_target(targets[k], fs->device_count, &redundant[k]);
	}
	for (i = 0; error == BTRFS_OK && i < fs->chunk_count; i++) {
		for (k = 1; k < 3; k++) {
			if ((fs->chunks[i].type & kinds[k]) != 0 &&
			    bt_space_profile(fs->chunks[i].type & BT_BLOCK_PROFILES, &fewest,
				&has) == BTRFS_OK) {
				present[k] |= has;
			}
		}
	}
	for (k = 1; error == BTRFS_OK && k < 3; k++) {
		if (present[k] && !redundant[k] && !args->force) {
			error = BTRFS_INVALID_ARGUMENT;
		}
	}
	return error;
}

enum btrfs_result
bt_balance(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, const struct btrfs_balance_args *args,
    size_t work, struct btrfs_balance_report *report)
{
	const struct btrfs_balance_kind *kind;
	struct btrfs_group *groups = NULL;
	struct btrfs_fs *fs = NULL;
	struct btrfs_info info;
	size_t capacity = 0;
	size_t count = 0;
	size_t i;
	enum btrfs_result error;

	if (environment == NULL || writer == NULL || args == NULL || report == NULL || work == 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	bt_zero(report, sizeof(*report));
	error = btrfs_mount(environment, BTRFS_TOP_LEVEL_TREE, &fs);
	if (error == BTRFS_OK) {
		error = bt_bl_arguments(fs, args);
	}
	btrfs_unmount(fs);
	fs = NULL;
	if (error == BTRFS_OK) {
		error = bt_bl_clean(environment, writer, report);
	}
	if (error == BTRFS_OK) {
		error = btrfs_mount(environment, BTRFS_TOP_LEVEL_TREE, &fs);
	}
	/* Mixed groups hold both kinds: Linux requires the same arguments. */
	if (error == BTRFS_OK) {
		btrfs_get_info(fs, &info);
		if ((info.incompat_features & BT_FEATURE_MIXED_GROUPS) != 0 &&
		    (args->data.enabled != args->metadata.enabled ||
			args->data.usage != args->metadata.usage ||
			args->data.convert != args->metadata.convert ||
			args->data.soft != args->metadata.soft)) {
			error = BTRFS_INVALID_ARGUMENT;
		}
	}
	if (error == BTRFS_OK) {
		capacity = fs->chunk_count;
		groups = environment->allocate(environment->context, capacity * sizeof(*groups));
		error = groups == NULL ? BTRFS_NO_MEMORY : BTRFS_OK;
	}
	if (error == BTRFS_OK) {
		error = btrfs_get_groups(fs, groups, capacity, &count);
	}
	btrfs_unmount(fs);
	/* The groups selected when the balance began, each once: chunks it
	 * creates are not revisited. */
	for (i = 0; error == BTRFS_OK && i < count; i++) {
		kind = bt_bl_kind(args, groups[i].flags);
		if (bt_bl_selects(kind, &groups[i])) {
			error = bt_bl_relocate(environment, writer, groups[i].logical, work,
			    kind->convert, 0, 0, report);
		}
	}
	if (groups != NULL) {
		environment->release(environment->context, groups, capacity * sizeof(*groups));
	}
	return error;
}

enum btrfs_result
btrfs_balance(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, const struct btrfs_balance_args *args,
    struct btrfs_balance_report *report)
{
	return bt_balance(environment, writer, args, BT_BALANCE_WORK, report);
}

enum btrfs_result
btrfs_relocate_group(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t group,
    struct btrfs_balance_report *report)
{
	return bt_relocate_group(environment, writer, group, BT_BALANCE_WORK, report);
}
