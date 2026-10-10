/* SPDX-License-Identifier: BSD-3-Clause */
/* Recovery of a balance a crash interrupted, as Linux's btrfs_recover_relocation
 * performs it at a read-write mount. A relocation tree (root item TREE_RELOC,
 * offset its file tree) still waiting for its merge (refs above 0) is merged
 * into its file tree; one whose file tree is gone is marked garbage (refs 0,
 * mark_garbage_root). Every relocation tree with refs 0 is then dropped, as
 * clean_dirty_subvols does, and the data relocation tree's orphan inodes,
 * which hold the relocated copies, are removed as btrfs_orphan_cleanup does.
 * Each transaction does a bounded step and commits a state Linux's recovery
 * resumes from. A balance item stays: Linux resumes the balance at its next
 * read-write mount. */
#include "transaction.h"

/* Blocks a drop step visits, and work a merge or orphan step does, per
 * transaction. */
#define BT_RELOCATION_DROP_BLOCKS 1024U
#define BT_RELOCATION_MERGE_WORK 1024U
#define BT_RELOCATION_ORPHAN_WORK 1024U
/* Committed steps after which recovery that still makes progress gives up. */
#define BT_RELOCATION_STEPS 1048576U

/* The root of tree objectid in the root tree roots of fs, by its last root
 * item (that of a snapshot names its creation transaction), and its refs;
 * *found is zero without one. */
static enum btrfs_result
bt_rl_root(const struct btrfs_fs *fs, struct bt_root roots, uint64_t objectid, struct bt_root *root,
    uint32_t *refs, int *found)
{
	const struct bt_disk_root *item;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = objectid, .type = BT_ROOT_ITEM, .offset = UINT64_MAX };
	enum btrfs_result error;

	*found = 0;
	bt_cursor_init(&cursor, fs, roots);
	error = bt_cursor_seek(&cursor, key, 1);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid == objectid && record.key.type == BT_ROOT_ITEM) {
			if (record.size < sizeof(*item)) {
				error = BTRFS_CORRUPT;
			} else {
				item = (const void *)record.data;
				*root = (struct bt_root){ bt_u64(item->bytenr),
					bt_u64(item->generation), objectid, item->level };
				*refs = bt_u32(item->refs);
				*found = 1;
			}
		}
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* What remains to recover in the mounted state. */
static enum btrfs_result
bt_rl_pending(const struct btrfs_fs *fs, uint64_t *trees, int *orphans)
{
	struct bt_root data;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key reloc = { .objectid = BT_TREE_RELOC, .type = BT_ROOT_ITEM, .offset = 0 };
	struct bt_key orphan = {
		.objectid = BT_ORPHAN_OBJECTID, .type = BT_ORPHAN_ITEM, .offset = 0
	};
	uint64_t steps;
	uint32_t refs = 0;
	int found = 0;
	enum btrfs_result error;

	*trees = 0;
	*orphans = 0;
	bt_cursor_init(&cursor, fs, fs->root_tree);
	error = bt_cursor_seek(&cursor, reloc, 0);
	for (steps = 0; error == BTRFS_OK; steps++) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != BT_TREE_RELOC || record.key.type != BT_ROOT_ITEM) {
			break;
		}
		if (steps == BT_MAX_TREE_ITEMS) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		(*trees)++;
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (error != BTRFS_OK && error != BTRFS_NOT_FOUND) {
		return error;
	}
	error = bt_rl_root(fs, fs->root_tree, BT_DATA_RELOC_TREE, &data, &refs, &found);
	if (error != BTRFS_OK || !found) {
		return error;
	}
	bt_cursor_init(&cursor, fs, data);
	error = bt_cursor_seek(&cursor, orphan, 0);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		*orphans =
		    record.key.objectid == BT_ORPHAN_OBJECTID && record.key.type == BT_ORPHAN_ITEM;
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* One merge of a relocation tree into its file tree, as merge_reloc_root
 * performs it: a path down the relocation tree (copies of its nodes, read
 * through the transaction's view) and a buffer for the file tree's nodes. */
struct bt_rl_merge {
	struct btrfs_transaction *transaction;
	struct bt_owned_root *fs;
	struct bt_owned_root *reloc;
	uint64_t last_snapshot;
	size_t work;
	uint8_t *nodes[BT_MAX_LEVEL];
	uint32_t slots[BT_MAX_LEVEL];
	int present[BT_MAX_LEVEL];
	uint8_t *parent;
	struct bt_key next_key;
};

static uint32_t
bt_rl_count(const uint8_t *node)
{
	return bt_u32(((const struct bt_disk_header *)node)->count);
}

static uint8_t
bt_rl_level(const uint8_t *node)
{
	return ((const struct bt_disk_header *)node)->level;
}

static const struct bt_disk_pointer *
bt_rl_pointer(const uint8_t *node, uint32_t slot)
{
	return (const struct bt_disk_pointer *)(node + sizeof(struct bt_disk_header)) + slot;
}

static struct bt_key
bt_rl_key(const uint8_t *node, uint32_t slot)
{
	return bt_key_decode(&bt_rl_pointer(node, slot)->key);
}

/* The last slot of an internal node whose key is at most key (the first when
 * none is), as btrfs_bin_search then a step back finds it. */
static uint32_t
bt_rl_slot(const uint8_t *node, struct bt_key key)
{
	uint32_t low = 0;
	uint32_t high = bt_rl_count(node);
	uint32_t middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (bt_key_compare(bt_rl_key(node, middle), key) <= 0) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return low != 0 ? low - 1 : 0;
}

/* Reads an internal node of the transaction's view into buffer. */
static enum btrfs_result
bt_rl_read(struct bt_rl_merge *merge, struct bt_root root, uint8_t *buffer)
{
	enum btrfs_result error;

	error = bt_tree_read(bt_mutation_view(merge->transaction->mutation), root, buffer);
	if (error == BTRFS_OK && (bt_rl_level(buffer) == 0 || bt_rl_count(buffer) == 0)) {
		error = BTRFS_CORRUPT;
	}
	return error;
}

/* Loads the relocation tree's path from its root down to level along key, as
 * btrfs_search_slot with lowest_level does. */
static enum btrfs_result
bt_rl_load(struct bt_rl_merge *merge, struct bt_key key, int level)
{
	const struct bt_disk_pointer *pointer;
	struct bt_root root = merge->reloc->root;
	int at;
	enum btrfs_result error = BTRFS_OK;

	for (at = 0; at < (int)BT_MAX_LEVEL; at++) {
		merge->present[at] = 0;
	}
	if (level < 1 || level > root.level) {
		return BTRFS_CORRUPT;
	}
	for (at = root.level; error == BTRFS_OK; at--) {
		error = bt_rl_read(merge, root, merge->nodes[at]);
		if (error != BTRFS_OK) {
			break;
		}
		merge->present[at] = 1;
		merge->slots[at] = bt_rl_slot(merge->nodes[at], key);
		if (at == level) {
			break;
		}
		pointer = bt_rl_pointer(merge->nodes[at], merge->slots[at]);
		root = (struct bt_root){ bt_u64(pointer->bytenr), bt_u64(pointer->generation),
			BT_TREE_RELOC, (uint8_t)(at - 1) };
	}
	return error;
}

/* Linux's walk_down_reloc_tree: from level down, the lowest relocated block
 * (newer than the last snapshot); *done when none remains below level. */
static enum btrfs_result
bt_rl_walk_down(struct bt_rl_merge *merge, int *level, int *done)
{
	const struct bt_disk_pointer *pointer;
	uint32_t count;
	int i;
	enum btrfs_result error;

	*done = 0;
	for (i = *level; i > 0; i--) {
		count = bt_rl_count(merge->nodes[i]);
		while (merge->slots[i] < count &&
		    bt_u64(bt_rl_pointer(merge->nodes[i], merge->slots[i])->generation) <=
			merge->last_snapshot) {
			merge->slots[i]++;
		}
		if (merge->slots[i] >= count) {
			if (i == *level) {
				break;
			}
			*level = i + 1;
			return BTRFS_OK;
		}
		if (i == 1) {
			*level = 1;
			return BTRFS_OK;
		}
		pointer = bt_rl_pointer(merge->nodes[i], merge->slots[i]);
		error = bt_rl_read(merge,
		    (struct bt_root){ bt_u64(pointer->bytenr), bt_u64(pointer->generation),
			BT_TREE_RELOC, (uint8_t)(i - 1) },
		    merge->nodes[i - 1]);
		if (error != BTRFS_OK) {
			return error;
		}
		merge->present[i - 1] = 1;
		merge->slots[i - 1] = 0;
	}
	*done = 1;
	return BTRFS_OK;
}

/* Linux's walk_up_reloc_tree: the next relocated block at level or above;
 * *done when the path is exhausted. */
static void
bt_rl_walk_up(struct bt_rl_merge *merge, int *level, int *done)
{
	uint32_t count;
	int i;

	*done = 0;
	for (i = 0; i < *level; i++) {
		merge->present[i] = 0;
	}
	for (i = *level; i < (int)BT_MAX_LEVEL && merge->present[i]; i++) {
		count = bt_rl_count(merge->nodes[i]);
		while (merge->slots[i] + 1 < count) {
			merge->slots[i]++;
			if (bt_u64(bt_rl_pointer(merge->nodes[i], merge->slots[i])->generation) <=
			    merge->last_snapshot) {
				continue;
			}
			*level = i;
			return;
		}
		merge->present[i] = 0;
	}
	*done = 1;
}

/* Linux's find_next_key: 0 with the key after the path's slot at level or
 * above, 1 when there is none. */
static int
bt_rl_next_key(const struct bt_rl_merge *merge, int level, struct bt_key *key)
{
	for (; level < (int)BT_MAX_LEVEL && merge->present[level]; level++) {
		if (merge->slots[level] + 1 < bt_rl_count(merge->nodes[level])) {
			*key = bt_rl_key(merge->nodes[level], merge->slots[level] + 1);
			return 0;
		}
	}
	return 1;
}

/* One reference change of the tree block at address and level. */
static enum btrfs_result
bt_rl_reference(struct bt_rl_merge *merge, uint64_t address, uint8_t level, uint64_t parent,
    uint64_t root, int add)
{
	struct btrfs_transaction *transaction = merge->transaction;
	struct bt_backref reference;
	struct bt_key extent = { .objectid = address, .type = BT_METADATA_ITEM, .offset = level };
	int freed = 0;
	enum btrfs_result error;

	bt_zero(&reference, sizeof(reference));
	reference.parent = parent;
	reference.root = parent == 0 ? root : 0;
	error = add ? bt_backref_add(
			  transaction->mutation, &transaction->extents.root, extent, &reference, 1)
		    : bt_backref_drop(transaction->mutation, &transaction->extents.root, extent,
			  &reference, 1, &freed);
	/* A swap moves references; it never frees a block. */
	return error == BTRFS_OK && freed ? BTRFS_CORRUPT : error;
}

/* Linux's replace_path: from the file tree's root, along the key at the
 * relocation path's slot at lowest, the first pointer whose block is not
 * newer than the last snapshot and matches the relocation tree's pointer at
 * the same level and key is swapped with it. *swapped is that level, 0 when
 * nothing was swapped. next_key reports the file tree's next key on the way. */
static enum btrfs_result
bt_rl_replace(struct bt_rl_merge *merge, int lowest, int max_level, int *swapped)
{
	struct btrfs_transaction *transaction = merge->transaction;
	const struct bt_disk_pointer *pointer;
	struct bt_root root = merge->fs->root;
	struct bt_key key = bt_rl_key(merge->nodes[lowest], merge->slots[lowest]);
	struct bt_key parent_key;
	uint64_t old_address;
	uint64_t old_generation;
	uint64_t new_address;
	uint64_t new_generation;
	uint64_t address;
	uint64_t generation;
	uint64_t reloc_parent;
	uint32_t slot;
	int level;
	enum btrfs_result error;

	*swapped = 0;
	if (root.level < lowest) {
		return BTRFS_OK;
	}
	merge->next_key = (struct bt_key){ UINT64_MAX, UINT64_MAX, UINT8_MAX };
	error = bt_rl_read(merge, root, merge->parent);
	/* One node per level from the file tree's root down. */
	while (error == BTRFS_OK) {
		level = bt_rl_level(merge->parent);
		slot = bt_rl_slot(merge->parent, key);
		if (slot + 1 < bt_rl_count(merge->parent)) {
			merge->next_key = bt_rl_key(merge->parent, slot + 1);
		}
		pointer = bt_rl_pointer(merge->parent, slot);
		old_address = bt_u64(pointer->bytenr);
		old_generation = bt_u64(pointer->generation);
		parent_key = bt_rl_key(merge->parent, slot);
		new_address = 0;
		new_generation = 0;
		if (level <= max_level) {
			pointer = bt_rl_pointer(merge->nodes[level], merge->slots[level]);
			new_address = bt_u64(pointer->bytenr);
			new_generation = bt_u64(pointer->generation);
		}
		if (new_address != 0 && new_address == old_address) {
			*swapped = level;
			return BTRFS_OK;
		}
		if (new_address == 0 || old_generation > merge->last_snapshot ||
		    bt_key_compare(
			parent_key, bt_rl_key(merge->nodes[level], merge->slots[level])) != 0) {
			if (level <= lowest) {
				return BTRFS_OK;
			}
			error = bt_rl_read(merge,
			    (struct bt_root){
				old_address, old_generation, root.owner, (uint8_t)(level - 1) },
			    merge->parent);
			continue;
		}
		/* Swap the blocks in the file tree and the relocation tree, each on
		 * its own copied path, then move the references as Linux does. */
		error = bt_mutation_set_child(transaction->mutation, &merge->fs->root, parent_key,
		    (uint8_t)level, new_address, new_generation, &address, &generation);
		if (error == BTRFS_OK && (address != old_address || generation != old_generation)) {
			error = BTRFS_CORRUPT;
		}
		if (error == BTRFS_OK) {
			error = bt_mutation_set_child(transaction->mutation, &merge->reloc->root,
			    parent_key, (uint8_t)level, old_address, old_generation, &address,
			    &generation);
		}
		if (error == BTRFS_OK && (address != new_address || generation != new_generation)) {
			error = BTRFS_CORRUPT;
		}
		if (error == BTRFS_OK) {
			error = bt_tx_settle(transaction);
		}
		if (error == BTRFS_OK) {
			error = bt_rl_load(merge, parent_key, level);
		}
		if (error != BTRFS_OK) {
			break;
		}
		reloc_parent = bt_u64(((const struct bt_disk_header *)merge->nodes[level])->bytenr);
		error =
		    bt_rl_reference(merge, old_address, (uint8_t)(level - 1), reloc_parent, 0, 1);
		if (error == BTRFS_OK) {
			error = bt_rl_reference(
			    merge, new_address, (uint8_t)(level - 1), 0, root.owner, 1);
		}
		if (error == BTRFS_OK) {
			error = bt_rl_reference(
			    merge, new_address, (uint8_t)(level - 1), reloc_parent, 0, 0);
		}
		if (error == BTRFS_OK) {
			error = bt_rl_reference(
			    merge, old_address, (uint8_t)(level - 1), 0, root.owner, 0);
		}
		if (error == BTRFS_OK) {
			transaction->changed = 1;
			*swapped = level;
		}
		break;
	}
	return error;
}

/* Merge steps of one relocation tree until it is merged or the work budget is
 * spent, recording the progress in its root item as Linux does; *finished
 * when the whole tree was walked (insert_dirty_subvol then sets refs 0). */
static enum btrfs_result
bt_rl_merge_tree(struct bt_rl_merge *merge, int *finished)
{
	struct btrfs_transaction *transaction = merge->transaction;
	struct bt_disk_root_full *item = &merge->reloc->item;
	struct bt_key progress = bt_key_decode(&item->legacy.drop_progress);
	struct bt_key key;
	size_t start = bt_tx_work(transaction);
	uint64_t steps;
	int level = merge->reloc->root.level;
	int max_level;
	int swapped = 0;
	int done = 0;
	enum btrfs_result error = BTRFS_OK;

	*finished = 0;
	if (progress.objectid == 0) {
		error = bt_rl_load(merge, progress, level);
		merge->slots[level] = 0;
	} else {
		level = item->legacy.drop_level;
		error = bt_rl_load(merge, progress, level);
		if (error == BTRFS_OK &&
		    bt_key_compare(bt_rl_key(merge->nodes[level], merge->slots[level]), progress) !=
			0) {
			error = BTRFS_CORRUPT;
		}
	}
	bt_zero(&merge->next_key, sizeof(merge->next_key));
	for (steps = 0; error == BTRFS_OK; steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		max_level = level;
		error = bt_rl_walk_down(merge, &level, &done);
		if (error != BTRFS_OK || done) {
			break;
		}
		swapped = 0;
		if (bt_rl_next_key(merge, level, &key) != 0 ||
		    bt_key_compare(merge->next_key, key) < 0) {
			error = bt_rl_replace(merge, level, max_level, &swapped);
		}
		if (error != BTRFS_OK) {
			break;
		}
		if (swapped > 0) {
			level = swapped;
		}
		bt_rl_walk_up(merge, &level, &done);
		if (done) {
			break;
		}
		/* The next relocated block at level is where a later step goes on. */
		bt_key_encode(&item->legacy.drop_progress,
		    bt_rl_key(merge->nodes[level], merge->slots[level]));
		item->legacy.drop_level = (uint8_t)level;
		if (bt_tx_work(transaction) - start >= merge->work) {
			break;
		}
	}
	if (error == BTRFS_OK && done) {
		*finished = 1;
		bt_zero(&item->legacy.drop_progress, sizeof(item->legacy.drop_progress));
		item->legacy.drop_level = 0;
		bt_put32(&item->legacy.refs, 0);
	}
	return error;
}

/* Merges, in one bounded step, the first relocation tree still waiting for
 * its merge, or marks it garbage when its file tree is gone. *working is
 * zero when no relocation tree waits; *merged counts trees whose merge
 * finished. */
static enum btrfs_result
bt_rl_merge(struct btrfs_transaction *transaction, size_t work, uint64_t *merged, int *working)
{
	const struct btrfs_environment *env = &transaction->base->env;
	struct bt_owned_root *reloc = &transaction->relocation;
	struct bt_root fs_root;
	struct bt_rl_merge merge;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = BT_TREE_RELOC, .type = BT_ROOT_ITEM, .offset = 0 };
	uint64_t steps;
	size_t node_size = transaction->base->info.node_size;
	unsigned level;
	uint32_t refs = 0;
	int found = 0;
	int finished = 0;
	enum btrfs_result error;

	*merged = 0;
	*working = 0;
	bt_cursor_init(&cursor, bt_mutation_view(transaction->mutation), transaction->roots);
	error = bt_cursor_seek(&cursor, key, 0);
	for (steps = 0; error == BTRFS_OK && !found; steps++) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != BT_TREE_RELOC || record.key.type != BT_ROOT_ITEM) {
			break;
		}
		if (steps == BT_MAX_TREE_ITEMS || record.size != sizeof(reloc->item)) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		if (bt_u32(((const struct bt_disk_root *)record.data)->refs) != 0) {
			bt_zero(reloc, sizeof(*reloc));
			bt_copy(&reloc->item, record.data, record.size);
			reloc->size = record.size;
			reloc->key = record.key;
			reloc->root = (struct bt_root){ bt_u64(reloc->item.legacy.bytenr),
				bt_u64(reloc->item.legacy.generation), BT_TREE_RELOC,
				reloc->item.legacy.level };
			found = 1;
			break;
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_OK;
	}
	if (error != BTRFS_OK || !found) {
		return error;
	}
	*working = 1;
	transaction->changed = 1;
	if (reloc->root.level >= BT_MAX_LEVEL) {
		return BTRFS_CORRUPT;
	}
	/* mark_garbage_root: a relocation tree whose file tree is gone, or being
	 * deleted, is only dropped. */
	found = 0;
	error = bt_file_tree(reloc->key.offset)
	    ? bt_rl_root(bt_mutation_view(transaction->mutation), transaction->roots,
		  reloc->key.offset, &fs_root, &refs, &found)
	    : BTRFS_CORRUPT;
	if (error == BTRFS_OK && (!found || refs == 0)) {
		bt_zero(
		    &reloc->item.legacy.drop_progress, sizeof(reloc->item.legacy.drop_progress));
		reloc->item.legacy.drop_level = 0;
		bt_put32(&reloc->item.legacy.refs, 0);
		return bt_tx_edit(transaction, &transaction->roots, reloc->key, &reloc->item,
		    reloc->size, BT_REPLACE);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	bt_zero(&merge, sizeof(merge));
	merge.transaction = transaction;
	merge.reloc = reloc;
	merge.work = work;
	merge.last_snapshot = bt_u64(reloc->item.legacy.last_snapshot);
	transaction->relocating = 1;
	error = bt_tx_tree_merge(transaction, reloc->key.offset, &merge.fs);
	for (level = 0; error == BTRFS_OK && level <= BT_MAX_LEVEL; level++) {
		if (level == BT_MAX_LEVEL) {
			merge.parent = env->allocate(env->context, node_size);
			error = merge.parent == NULL ? BTRFS_NO_MEMORY : BTRFS_OK;
		} else {
			merge.nodes[level] = env->allocate(env->context, node_size);
			error = merge.nodes[level] == NULL ? BTRFS_NO_MEMORY : BTRFS_OK;
		}
	}
	if (error == BTRFS_OK) {
		error = bt_rl_merge_tree(&merge, &finished);
	}
	if (error == BTRFS_OK) {
		/* btrfs_update_reloc_root: the root item names the tree's root. */
		bt_put64(&reloc->item.legacy.bytenr, reloc->root.address);
		bt_put64(&reloc->item.legacy.generation, reloc->root.generation);
		bt_put64(&reloc->item.generation_v2, reloc->root.generation);
		reloc->item.legacy.level = reloc->root.level;
		error = bt_tx_edit(transaction, &transaction->roots, reloc->key, &reloc->item,
		    reloc->size, BT_REPLACE);
	}
	if (error == BTRFS_OK && finished) {
		*merged = 1;
	}
	for (level = 0; level < BT_MAX_LEVEL; level++) {
		if (merge.nodes[level] != NULL) {
			env->release(env->context, merge.nodes[level], node_size);
		}
	}
	if (merge.parent != NULL) {
		env->release(env->context, merge.parent, node_size);
	}
	if (error != BTRFS_OK) {
		transaction->failure = error;
	}
	return error;
}

/* One committed step: merges, then drops, then cleans orphans. *changed is
 * zero when the step found nothing to do. */
static enum btrfs_result
bt_rl_step(struct btrfs_fs *fs, const struct btrfs_write_environment *writer, size_t merge_work,
    struct btrfs_relocation_report *report, int *changed)
{
	struct btrfs_transaction *transaction = NULL;
	struct bt_root quota;
	size_t dropped = 0;
	size_t cleaned = 0;
	uint64_t merged = 0;
	uint32_t refs = 0;
	int working = 0;
	int pending = 0;
	int live = 0;
	int quotas = 0;
	enum btrfs_result error;

	*changed = 0;
	/* Linux's merge records swapped subtrees for qgroups
	 * (btrfs_qgroup_add_swapped_blocks); that is not implemented. */
	error = bt_rl_root(fs, fs->root_tree, BT_QUOTA_TREE, &quota, &refs, &quotas);
	if (error != BTRFS_OK) {
		return error;
	}
	if (quotas) {
		return BTRFS_UNSUPPORTED;
	}
	fs->relocation = 1;
	error = btrfs_transaction_begin(fs, writer, &transaction);
	if (error == BTRFS_OK) {
		error = bt_rl_merge(transaction, merge_work, &merged, &working);
	}
	/* Relocation trees are dropped once none waits for its merge, as
	 * clean_dirty_subvols drops them after merge_reloc_roots. */
	if (error == BTRFS_OK && !working) {
		error = bt_drop_relocation(
		    transaction, BT_RELOCATION_DROP_BLOCKS, &dropped, &pending, &live);
	}
	/* The data relocation tree's copies go once no relocation tree needs
	 * them, as Linux cleans its orphans after the merge. */
	if (error == BTRFS_OK && !working && dropped == 0 && !pending && !live) {
		error = btrfs_transaction_clean_orphans(
		    transaction, BT_DATA_RELOC_TREE, BT_RELOCATION_ORPHAN_WORK, &cleaned, &pending);
	}
	if (error == BTRFS_OK) {
		*changed = transaction->changed;
		error = btrfs_transaction_commit(transaction);
	}
	if (error == BTRFS_OK) {
		report->merged += merged;
		report->dropped += dropped;
		report->orphans += cleaned;
		report->commits++;
	}
	btrfs_transaction_destroy(transaction);
	return error;
}

enum btrfs_result
btrfs_recover_relocation(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, struct btrfs_relocation_report *report)
{
	return bt_recover_relocation(environment, writer, BT_RELOCATION_MERGE_WORK, report);
}

enum btrfs_result
bt_recover_relocation(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, size_t merge_work,
    struct btrfs_relocation_report *report)
{
	struct btrfs_fs *fs = NULL;
	uint64_t trees = 0;
	uint64_t steps;
	int orphans = 0;
	int changed = 1;
	enum btrfs_result error = BTRFS_OK;

	if (environment == NULL || report == NULL || merge_work == 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	bt_zero(report, sizeof(*report));
	for (steps = 0; error == BTRFS_OK; steps++) {
		error = btrfs_mount(environment, BTRFS_TOP_LEVEL_TREE, &fs);
		if (error == BTRFS_OK) {
			error = bt_rl_pending(fs, &trees, &orphans);
		}
		if (error == BTRFS_OK && steps == 0) {
			report->generation = fs->info.generation;
			report->trees = trees;
		}
		if (error == BTRFS_OK && trees == 0 && !orphans) {
			btrfs_unmount(fs);
			return steps == 0 ? BTRFS_NOT_FOUND : BTRFS_OK;
		}
		if (error == BTRFS_OK && writer == NULL) {
			error = BTRFS_RECOVERY_REQUIRED;
		}
		/* A step that changes nothing while work remains would repeat. */
		if (error == BTRFS_OK && (!changed || steps == BT_RELOCATION_STEPS)) {
			error = BTRFS_CORRUPT;
		}
		if (error == BTRFS_OK) {
			error = bt_rl_step(fs, writer, merge_work, report, &changed);
		}
		btrfs_unmount(fs);
		fs = NULL;
	}
	return error;
}
