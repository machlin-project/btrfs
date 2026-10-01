/* SPDX-License-Identifier: BSD-3-Clause */
/* Dropping deleted subvolumes, following Linux's btrfs_drop_snapshot: a walk
 * from the root drops this tree's reference to every block. A block shared
 * with other trees is not entered: the tree's reference to it goes, after
 * its children were converted to parent references when this tree owns it
 * (the UPDATE_BACKREF stage), so the other owners keep valid references. An
 * unshared block is entered, its children handled, and then freed with the
 * file extents of a leaf. Progress is the key of the next child at a level:
 * the root item keeps it between transactions. */
#include "transaction.h"

#define BT_DROP_REFERENCE 0
#define BT_DROP_UPDATE_BACKREF 1

struct bt_drop {
	struct btrfs_transaction *transaction;
	struct bt_owned_root dead;
	uint8_t *nodes[BT_MAX_LEVEL];
	uint64_t addresses[BT_MAX_LEVEL];
	uint32_t slots[BT_MAX_LEVEL];
	uint64_t refs[BT_MAX_LEVEL];
	uint64_t flags[BT_MAX_LEVEL];
	int present[BT_MAX_LEVEL];
	int level;
	int stage;
	int shared_level;
	int update_ref;
	struct bt_key update_progress;
	size_t visited;
};

static const struct bt_disk_header *
bt_drop_header(const struct bt_drop *drop, int level)
{
	return (const void *)drop->nodes[level];
}

static uint32_t
bt_drop_count(const struct bt_drop *drop, int level)
{
	return bt_u32(bt_drop_header(drop, level)->count);
}

static struct bt_key
bt_drop_key(const struct bt_drop *drop, int level, uint32_t slot)
{
	const struct bt_disk_item *items = (const void *)(bt_drop_header(drop, level) + 1);
	const struct bt_disk_pointer *pointers = (const void *)(bt_drop_header(drop, level) + 1);

	return level == 0 ? bt_key_decode(&items[slot].key) : bt_key_decode(&pointers[slot].key);
}

static enum btrfs_result
bt_drop_info(struct bt_drop *drop, uint64_t address, int level, uint64_t *refs, uint64_t *flags)
{
	struct bt_key extent = { address, (uint64_t)level, BT_METADATA_ITEM };
	enum btrfs_result error;

	error = bt_backref_info(
	    drop->transaction->mutation, drop->transaction->extents.root, extent, refs, flags);
	if (error == BTRFS_OK && *refs == 0) {
		error = BTRFS_CORRUPT;
	}
	return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
}

/* Reads a block of the deleted tree; its committed copy is never changed. */
static enum btrfs_result
bt_drop_read(struct bt_drop *drop, int level, uint64_t address, uint64_t generation)
{
	struct bt_root root = { address, generation, drop->dead.root.owner, (uint8_t)level };
	enum btrfs_result error;

	error = bt_tree_read(drop->transaction->base, root, drop->nodes[level]);
	if (error == BTRFS_OK) {
		drop->addresses[level] = address;
		drop->slots[level] = 0;
		drop->present[level] = 1;
		drop->visited++;
	}
	return error;
}

/* Linux's find_next_key: the key after the path's slot at level or above. */
static int
bt_drop_next_key(const struct bt_drop *drop, int level, struct bt_key *key)
{
	for (; level < (int)BT_MAX_LEVEL && drop->present[level]; level++) {
		if (drop->slots[level] + 1 < bt_drop_count(drop, level)) {
			*key = bt_drop_key(drop, level, drop->slots[level] + 1);
			return 0;
		}
	}
	return 1;
}

/* Drops this tree's reference to the block at address: keyed by the tree, or
 * by parent when the referencing block uses parent references. last says
 * whether it must be the block's last reference. */
static enum btrfs_result
bt_drop_block(struct bt_drop *drop, uint64_t address, int level, uint64_t parent, int last)
{
	struct btrfs_transaction *transaction = drop->transaction;
	struct bt_backref reference;
	struct bt_key extent = { address, (uint64_t)level, BT_METADATA_ITEM };
	int freed = 0;
	enum btrfs_result error;

	bt_zero(&reference, sizeof(reference));
	if (parent != 0) {
		reference.parent = parent;
	} else {
		reference.root = drop->dead.root.owner;
	}
	error = bt_backref_drop(
	    transaction->mutation, &transaction->extents.root, extent, &reference, 1, &freed);
	if (error == BTRFS_OK && freed != last) {
		error = BTRFS_CORRUPT;
	}
	return error == BTRFS_OK && freed ? bt_space_change_used(transaction->space, address,
						transaction->base->info.node_size, 0)
					  : error;
}

/* The parent a reference from the block at level uses: its address when it
 * uses parent references; otherwise it must belong to this tree. */
static enum btrfs_result
bt_drop_parent(const struct bt_drop *drop, int level, uint64_t *parent)
{
	*parent = 0;
	if ((drop->flags[level] & BT_EXTENT_FLAG_FULL_BACKREF) != 0) {
		*parent = drop->addresses[level];
		return BTRFS_OK;
	}
	return bt_u64(bt_drop_header(drop, level)->owner) == drop->dead.root.owner ? BTRFS_OK
										   : BTRFS_CORRUPT;
}

/* Queues the release of a leaf's file extent references, as btrfs_dec_ref:
 * keyed by the leaf's owner, or by the leaf when it uses parent references. */
static enum btrfs_result
bt_drop_file_references(struct bt_drop *drop, int full)
{
	const struct bt_disk_header *header = bt_drop_header(drop, 0);
	const struct bt_disk_item *items = (const void *)(header + 1);
	const struct bt_disk_extent *file;
	struct bt_backref reference;
	struct bt_key extent;
	struct bt_key key;
	uint32_t i;
	enum btrfs_result error = BTRFS_OK;

	for (i = 0; error == BTRFS_OK && i < bt_drop_count(drop, 0); i++) {
		key = bt_key_decode(&items[i].key);
		if (key.type != BT_EXTENT_DATA) {
			continue;
		}
		file = (const void *)((const uint8_t *)(header + 1) + bt_u32(items[i].offset));
		if (bt_u32(items[i].size) < sizeof(file->header)) {
			return BTRFS_CORRUPT;
		}
		if (file->header.type == BT_EXTENT_INLINE) {
			continue;
		}
		if (bt_u32(items[i].size) < sizeof(*file)) {
			return BTRFS_CORRUPT;
		}
		if (bt_u64(file->disk_bytenr) == 0) {
			continue;
		}
		extent = (struct bt_key){ bt_u64(file->disk_bytenr), bt_u64(file->disk_bytes),
			BT_EXTENT_ITEM };
		bt_zero(&reference, sizeof(reference));
		reference.data = 1;
		if (full) {
			reference.parent = drop->addresses[0];
		} else {
			reference.root = bt_u64(header->owner);
			reference.inode = key.objectid;
			reference.offset = key.offset - bt_u64(file->offset);
		}
		error = bt_tx_queue(drop->transaction, extent, &reference, 0);
	}
	return error;
}

/* Linux's walk_down_proc: 1 stops the descent at this level. */
static enum btrfs_result
bt_drop_down_proc(struct bt_drop *drop, int lookup, int *stop)
{
	int level = drop->level;
	enum btrfs_result error = BTRFS_OK;

	*stop = 0;
	if (drop->stage == BT_DROP_UPDATE_BACKREF &&
	    bt_u64(bt_drop_header(drop, level)->owner) != drop->dead.root.owner) {
		*stop = 1;
		return BTRFS_OK;
	}
	if (lookup &&
	    ((drop->stage == BT_DROP_REFERENCE && drop->refs[level] != 1) ||
		(drop->stage == BT_DROP_UPDATE_BACKREF &&
		    (drop->flags[level] & BT_EXTENT_FLAG_FULL_BACKREF) == 0))) {
		error = bt_drop_info(
		    drop, drop->addresses[level], level, &drop->refs[level], &drop->flags[level]);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	if (drop->stage == BT_DROP_REFERENCE) {
		*stop = drop->refs[level] > 1;
		return BTRFS_OK;
	}
	/* A shared block this tree owns: its children switch to parent
	 * references before this tree's go. */
	if ((drop->flags[level] & BT_EXTENT_FLAG_FULL_BACKREF) == 0) {
		error = bt_tx_children(
		    drop->transaction, drop->nodes[level], drop->addresses[level], 1, 0, 1);
		if (error == BTRFS_OK) {
			error = bt_tx_children(drop->transaction, drop->nodes[level],
			    drop->addresses[level], 0, drop->dead.root.owner, 0);
		}
		if (error == BTRFS_OK) {
			error = bt_backref_set_flags(drop->transaction->mutation,
			    &drop->transaction->extents.root,
			    (struct bt_key){
				drop->addresses[level], (uint64_t)level, BT_METADATA_ITEM },
			    BT_EXTENT_FLAG_FULL_BACKREF);
		}
		drop->flags[level] |= BT_EXTENT_FLAG_FULL_BACKREF;
	}
	return error;
}

/* Linux's do_walk_down: enters the child at the current slot (skip 0) or
 * drops this tree's reference to it (skip 1). */
static enum btrfs_result
bt_drop_down(struct bt_drop *drop, int *skip)
{
	const struct bt_disk_pointer *pointers =
	    (const void *)(bt_drop_header(drop, drop->level) + 1);
	int level = drop->level;
	uint64_t generation = bt_u64(pointers[drop->slots[level]].generation);
	uint64_t address = bt_u64(pointers[drop->slots[level]].bytenr);
	uint64_t snapshot = drop->dead.key.offset;
	uint64_t parent = 0;
	enum btrfs_result error;

	*skip = 0;
	if (drop->stage == BT_DROP_UPDATE_BACKREF && generation <= snapshot) {
		*skip = 1;
		return BTRFS_OK;
	}
	error =
	    bt_drop_info(drop, address, level - 1, &drop->refs[level - 1], &drop->flags[level - 1]);
	if (error != BTRFS_OK) {
		return error;
	}
	if (drop->stage == BT_DROP_REFERENCE) {
		if (drop->refs[level - 1] > 1) {
			/* Shared: a block this tree created after its snapshot
			 * point converts its children first. */
			if ((level == 1 &&
				(drop->flags[level - 1] & BT_EXTENT_FLAG_FULL_BACKREF) != 0) ||
			    !drop->update_ref || generation <= snapshot ||
			    bt_key_compare(bt_drop_key(drop, level, drop->slots[level]),
				drop->update_progress) < 0) {
				*skip = 1;
			} else {
				drop->stage = BT_DROP_UPDATE_BACKREF;
				drop->shared_level = level - 1;
			}
		}
	} else if (level == 1 && (drop->flags[level - 1] & BT_EXTENT_FLAG_FULL_BACKREF) != 0) {
		*skip = 1;
	}
	if (*skip) {
		drop->refs[level - 1] = 0;
		drop->flags[level - 1] = 0;
		if (drop->stage == BT_DROP_REFERENCE) {
			error = bt_drop_parent(drop, level, &parent);
			if (error == BTRFS_OK) {
				error = bt_drop_block(drop, address, level - 1, parent, 0);
			}
			drop->visited++;
		}
		return error;
	}
	error = bt_drop_read(drop, level - 1, address, generation);
	if (error == BTRFS_OK) {
		drop->level = level - 1;
	}
	return error;
}

/* Linux's walk_down_tree. */
static enum btrfs_result
bt_drop_walk_down(struct bt_drop *drop)
{
	int lookup = 1;
	int stop;
	int skip;
	enum btrfs_result error;

	while (drop->level >= 0) {
		error = bt_drop_down_proc(drop, lookup, &stop);
		if (error != BTRFS_OK) {
			return error;
		}
		if (stop || drop->level == 0 ||
		    drop->slots[drop->level] >= bt_drop_count(drop, drop->level)) {
			break;
		}
		error = bt_drop_down(drop, &skip);
		if (error != BTRFS_OK) {
			return error;
		}
		if (skip) {
			drop->slots[drop->level]++;
		}
		lookup = 1;
	}
	return BTRFS_OK;
}

/* Linux's walk_up_proc: done 1 restarts the descent at this level. */
static enum btrfs_result
bt_drop_up_proc(struct bt_drop *drop, int *restart)
{
	int level = drop->level;
	uint64_t parent = 0;
	enum btrfs_result error = BTRFS_OK;

	*restart = 0;
	if (drop->stage == BT_DROP_UPDATE_BACKREF) {
		if (level < drop->shared_level) {
			goto out;
		}
		if (bt_drop_next_key(drop, level + 1, &drop->update_progress) != 0) {
			drop->update_ref = 0;
		}
		drop->stage = BT_DROP_REFERENCE;
		drop->shared_level = -1;
		drop->slots[level] = 0;
		if (level > 0) {
			error = bt_drop_info(drop, drop->addresses[level], level,
			    &drop->refs[level], &drop->flags[level]);
			if (error == BTRFS_OK && drop->refs[level] == 1) {
				*restart = 1;
			}
			if (error != BTRFS_OK || *restart) {
				return error;
			}
		}
	}
	if (drop->refs[level] == 1 && level == 0) {
		error = bt_drop_file_references(
		    drop, (drop->flags[0] & BT_EXTENT_FLAG_FULL_BACKREF) != 0);
	}
	if (error == BTRFS_OK) {
		if (level == drop->dead.root.level) {
			parent = (drop->flags[level] & BT_EXTENT_FLAG_FULL_BACKREF) != 0
			    ? drop->addresses[level]
			    : 0;
			if (parent == 0 &&
			    bt_u64(bt_drop_header(drop, level)->owner) != drop->dead.root.owner) {
				error = BTRFS_CORRUPT;
			}
		} else {
			error = bt_drop_parent(drop, level + 1, &parent);
		}
	}
	if (error == BTRFS_OK) {
		error = bt_drop_block(
		    drop, drop->addresses[level], level, parent, drop->refs[level] == 1);
	}
out:
	drop->refs[level] = 0;
	drop->flags[level] = 0;
	return error;
}

/* Linux's walk_up_tree: done 1 when the root is gone. */
static enum btrfs_result
bt_drop_walk_up(struct bt_drop *drop, int *done)
{
	int level = drop->level;
	int restart;
	enum btrfs_result error;

	*done = 0;
	drop->slots[level] = bt_drop_count(drop, level);
	for (; level < (int)BT_MAX_LEVEL && drop->present[level]; level++) {
		drop->level = level;
		if (drop->slots[level] + 1 < bt_drop_count(drop, level)) {
			drop->slots[level]++;
			return BTRFS_OK;
		}
		error = bt_drop_up_proc(drop, &restart);
		if (error != BTRFS_OK || restart) {
			return error;
		}
		drop->present[level] = 0;
	}
	*done = 1;
	return BTRFS_OK;
}

/* Rebuilds the path from the root to the recorded progress, as
 * btrfs_drop_snapshot does when it resumes. */
static enum btrfs_result
bt_drop_resume(struct bt_drop *drop)
{
	struct bt_disk_root *item = &drop->dead.item.legacy;
	const struct bt_disk_pointer *pointers;
	struct bt_key progress = bt_key_decode(&item->drop_progress);
	int level = drop->dead.root.level;
	int bottom = item->drop_level;
	uint32_t slot;
	enum btrfs_result error;

	error = bt_drop_read(drop, level, drop->dead.root.address, drop->dead.root.generation);
	if (error != BTRFS_OK || progress.objectid == 0) {
		drop->level = level;
		return error;
	}
	if (bottom == 0 || bottom > level) {
		return BTRFS_CORRUPT;
	}
	drop->update_progress = progress;
	for (;; level--) {
		for (slot = 0; slot + 1 < bt_drop_count(drop, level) &&
		    bt_key_compare(bt_drop_key(drop, level, slot + 1), progress) <= 0;
		    slot++) {
		}
		drop->slots[level] = slot;
		error = bt_drop_info(
		    drop, drop->addresses[level], level, &drop->refs[level], &drop->flags[level]);
		if (error != BTRFS_OK || level == bottom) {
			break;
		}
		/* Levels above the progress were entered with one reference. */
		if (drop->refs[level] != 1) {
			return BTRFS_CORRUPT;
		}
		pointers = (const void *)(bt_drop_header(drop, level) + 1);
		error = bt_drop_read(drop, level - 1, bt_u64(pointers[slot].bytenr),
		    bt_u64(pointers[slot].generation));
		if (error != BTRFS_OK) {
			return error;
		}
	}
	if (error == BTRFS_OK &&
	    bt_key_compare(bt_drop_key(drop, bottom, drop->slots[bottom]), progress) != 0) {
		error = BTRFS_CORRUPT;
	}
	drop->level = bottom;
	return error;
}

/* The next deleted subvolume: the first orphan item of the root tree naming a
 * dead root. Stale orphan items (no root item, or a live one) go, as Linux's
 * btrfs_find_orphan_roots drops them. */
static enum btrfs_result
bt_drop_find(struct btrfs_transaction *transaction, struct bt_owned_root *dead, int *found)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { BT_ORPHAN_OBJECTID, 0, BT_ORPHAN_ITEM };
	struct bt_key orphan;
	enum btrfs_result error;

	*found = 0;
	for (;;) {
		bt_cursor_init(
		    &cursor, bt_mutation_view(transaction->mutation), transaction->roots);
		error = bt_cursor_seek(&cursor, key, 0);
		if (error == BTRFS_OK) {
			(void)bt_cursor_record(&cursor, &record);
			orphan = record.key;
			if (orphan.objectid != BT_ORPHAN_OBJECTID ||
			    orphan.type != BT_ORPHAN_ITEM) {
				error = BTRFS_NOT_FOUND;
			}
		}
		bt_cursor_fini(&cursor);
		if (error != BTRFS_OK) {
			return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
		}
		error = bt_file_tree(orphan.offset)
		    ? bt_tx_root_item(transaction, orphan.offset, dead)
		    : BTRFS_CORRUPT;
		if (error == BTRFS_OK && bt_u32(dead->item.legacy.refs) == 0) {
			*found = 1;
			return BTRFS_OK;
		}
		if (error != BTRFS_OK && error != BTRFS_NOT_FOUND) {
			return error;
		}
		error = bt_tx_edit(transaction, &transaction->roots, orphan, NULL, 0, BT_DELETE);
		if (error != BTRFS_OK) {
			return error;
		}
		transaction->changed = 1;
		key.offset = orphan.offset + 1;
	}
}

static void
bt_drop_release(struct bt_drop *drop)
{
	const struct btrfs_environment *env = &drop->transaction->base->env;
	unsigned level;

	for (level = 0; level < BT_MAX_LEVEL; level++) {
		if (drop->nodes[level] != NULL) {
			env->release(env->context, drop->nodes[level],
			    drop->transaction->base->info.node_size);
		}
	}
	env->release(env->context, drop, sizeof(*drop));
}

/* Drops one deleted subvolume until it is gone or the budget is spent. */
static enum btrfs_result
bt_drop_tree(struct bt_drop *drop, size_t budget, int *finished)
{
	struct btrfs_transaction *transaction = drop->transaction;
	struct bt_disk_root *item = &drop->dead.item.legacy;
	struct bt_key orphan = { BT_ORPHAN_OBJECTID, drop->dead.root.owner, BT_ORPHAN_ITEM };
	size_t leaf_items = transaction->base->info.node_size / sizeof(struct bt_disk_item);
	int done = 0;
	enum btrfs_result error;

	*finished = 0;
	drop->stage = BT_DROP_REFERENCE;
	drop->shared_level = -1;
	drop->update_ref = 1;
	error = bt_drop_resume(drop);
	while (error == BTRFS_OK) {
		error = bt_drop_walk_down(drop);
		if (error == BTRFS_OK) {
			error = bt_drop_walk_up(drop, &done);
		}
		if (error != BTRFS_OK || done) {
			break;
		}
		if (drop->stage == BT_DROP_REFERENCE) {
			bt_key_encode(&item->drop_progress,
			    bt_drop_key(drop, drop->level, drop->slots[drop->level]));
			item->drop_level = (uint8_t)drop->level;
		}
		/* Stop at a recorded point with room for another leaf's file
		 * references in the transaction. */
		if (drop->visited >= budget ||
		    transaction->ref_count + leaf_items > BT_TRANSACTION_REFERENCES) {
			break;
		}
	}
	if (error != BTRFS_OK) {
		return error;
	}
	if (done) {
		*finished = 1;
		error = bt_tx_edit(
		    transaction, &transaction->roots, drop->dead.key, NULL, 0, BT_DELETE);
		if (error == BTRFS_OK) {
			error = bt_tx_edit(
			    transaction, &transaction->roots, orphan, NULL, 0, BT_DELETE);
		}
	} else {
		error = bt_tx_edit(transaction, &transaction->roots, drop->dead.key,
		    &drop->dead.item, drop->dead.size, BT_REPLACE);
	}
	transaction->changed = 1;
	return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
}

enum btrfs_result
btrfs_transaction_clean_subvolumes(
    struct btrfs_transaction *transaction, size_t budget, size_t *dropped, int *pending)
{
	const struct btrfs_environment *env;
	struct bt_drop *drop = NULL;
	size_t spent = 0;
	unsigned level;
	int found = 0;
	int finished = 1;
	enum btrfs_result error = BTRFS_OK;

	if (transaction == NULL || dropped == NULL || pending == NULL || budget == 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*dropped = 0;
	*pending = 0;
	if (transaction->failure != BTRFS_OK || transaction->finished) {
		return transaction->failure == BTRFS_OK ? BTRFS_READ_ONLY : transaction->failure;
	}
	env = &transaction->base->env;
	while (error == BTRFS_OK && finished && spent < budget) {
		error = bt_drop_find(transaction, &transaction->dropping, &found);
		if (error != BTRFS_OK || !found) {
			break;
		}
		drop = env->allocate(env->context, sizeof(*drop));
		error = drop == NULL ? BTRFS_NO_MEMORY : BTRFS_OK;
		if (error == BTRFS_OK) {
			bt_zero(drop, sizeof(*drop));
			drop->transaction = transaction;
			drop->dead = transaction->dropping;
		}
		for (level = 0; error == BTRFS_OK && level <= drop->dead.root.level; level++) {
			drop->nodes[level] =
			    env->allocate(env->context, transaction->base->info.node_size);
			error = drop->nodes[level] == NULL ? BTRFS_NO_MEMORY : BTRFS_OK;
		}
		if (error == BTRFS_OK && drop->dead.root.level >= BT_MAX_LEVEL) {
			error = BTRFS_CORRUPT;
		}
		if (error == BTRFS_OK) {
			error = bt_drop_tree(drop, budget - spent, &finished);
			spent += drop->visited;
			*dropped += finished;
		}
		if (drop != NULL) {
			bt_drop_release(drop);
			drop = NULL;
		}
	}
	if (error == BTRFS_OK) {
		*pending = !finished;
		if (finished) {
			error = bt_drop_find(transaction, &transaction->dropping, &found);
			*pending = error == BTRFS_OK && found;
		}
	}
	if (error != BTRFS_OK) {
		transaction->failure = error;
	}
	return error;
}
