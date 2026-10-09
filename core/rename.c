/* SPDX-License-Identifier: BSD-3-Clause */
/* Rename and exchange as Linux's renameat2 decides them: the VFS checks first
 * (a directory moved below itself, a replaced ancestor, one object under both
 * names, may_delete and may_create, a read-only root moved to another
 * parent), then btrfs_rename's and btrfs_rename_exchange's own, for names of
 * inodes and for subvolume entries, which may move between subvolumes. */
#include "namespace.h"

/* One name of a rename or exchange: a name of an inode in its directory's
 * tree, a subvolume entry, or a stub that a snapshot copied for one. */
struct bt_rn_side {
	struct btrfs_object_id parent;
	const void *name;
	size_t length;
	struct bt_owned_root *tree;
	struct bt_disk_inode directory;
	/* The named inode, or the subvolume's root directory; zero for a stub,
	 * which has no flags. */
	struct bt_disk_inode item;
	struct bt_entry entry;
	struct bt_sv_entry subvolume;
	/* What a new entry for this object names, as btrfs_add_link: an inode
	 * item, or the subvolume's root item key. */
	struct bt_key location;
	uint8_t type;
	int exists;
	int is_subvolume;
	int stub;
	int read_only;
};

/* Whether directory is ancestor or lies below it: the single INODE_REF of
 * each directory up to its tree's root directory, then the subvolume's
 * ROOT_BACKREF to the directory holding its entry, up to a subvolume without
 * an entry. A tree is left at most once, so the walk ends in the tree of
 * ancestor. */
static enum btrfs_result
bt_rn_inside(struct btrfs_transaction *transaction, struct btrfs_object_id directory,
    struct btrfs_object_id ancestor, int *inside)
{
	const struct bt_disk_root_ref *ref = (const void *)transaction->item;
	struct bt_owned_root view;
	struct bt_key key;
	struct bt_key next;
	struct btrfs_object_id current = directory;
	uint64_t steps;
	uint64_t walk;
	size_t size = 0;
	int found = 0;
	enum btrfs_result error = BTRFS_OK;

	*inside = 0;
	for (steps = 0; error == BTRFS_OK; steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			return BTRFS_CORRUPT;
		}
		error = bt_tx_tree_view(transaction, current.tree, &view);
		for (walk = 0; error == BTRFS_OK; walk++) {
			if (current.tree == ancestor.tree && current.inode == ancestor.inode) {
				*inside = 1;
				return BTRFS_OK;
			}
			if (current.inode == BTRFS_ROOT_INODE) {
				break;
			}
			if (walk == BT_MAX_TREE_ITEMS) {
				return BTRFS_CORRUPT;
			}
			key = (struct bt_key){ .objectid = current.inode, .type = BT_INODE_REF };
			error = bt_ns_neighbor(transaction, &view, key, 0, &next, &found);
			if (error == BTRFS_OK &&
			    (!found || next.objectid != current.inode ||
				next.type != BT_INODE_REF || next.offset == current.inode)) {
				error = BTRFS_CORRUPT;
			}
			if (error == BTRFS_OK) {
				current.inode = next.offset;
			}
		}
		if (error != BTRFS_OK || current.tree == ancestor.tree) {
			break;
		}
		/* The view now searches the root tree. */
		view.root = transaction->roots;
		key = (struct bt_key){ .objectid = current.tree, .type = BT_ROOT_BACKREF };
		error = bt_ns_neighbor(transaction, &view, key, 0, &next, &found);
		if (error != BTRFS_OK || !found || next.objectid != current.tree ||
		    next.type != BT_ROOT_BACKREF) {
			break;
		}
		error = bt_mutation_find(transaction->mutation, transaction->roots, next,
		    transaction->item, transaction->base->info.node_size, &size);
		if (error == BTRFS_OK &&
		    (next.offset == current.tree || size < sizeof(*ref) ||
			size != sizeof(*ref) + bt_u16(ref->name_length))) {
			error = BTRFS_CORRUPT;
		}
		if (error == BTRFS_OK) {
			current = (struct btrfs_object_id){ next.offset, bt_u64(ref->directory) };
		}
	}
	return error == BTRFS_NOT_FOUND || error == BTRFS_RANGE ? BTRFS_CORRUPT : error;
}

/* Opens side's directory and resolves its name; an optional name may be
 * missing (side->exists clear). A subvolume entry names its root item key
 * and root directory, read without opening the subvolume. */
static enum btrfs_result
bt_rn_resolve(struct btrfs_transaction *transaction, struct bt_rn_side *side, int optional)
{
	struct bt_owned_root view;
	enum btrfs_result error;

	error = bt_ns_begin(transaction, side->parent.tree, &side->tree);
	if (error == BTRFS_OK) {
		error = bt_ns_parent(transaction, side->tree, side->parent.inode, &side->directory);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_lookup(transaction, side->tree, side->parent.inode, side->name,
		    side->length, &side->entry);
	}
	side->exists = error == BTRFS_OK || error == BTRFS_CROSS_TREE;
	if (error == BTRFS_NOT_FOUND && optional) {
		return BTRFS_OK;
	}
	if (error == BTRFS_OK) {
		side->location =
		    (struct bt_key){ .objectid = side->entry.inode, .type = BT_INODE_ITEM };
		side->type = side->entry.type;
		return bt_ns_named_inode(transaction, side->tree, side->entry.inode, &side->item);
	}
	if (error != BTRFS_CROSS_TREE) {
		return error;
	}
	side->type = BTRFS_FT_DIRECTORY;
	error = bt_sv_entry(transaction, side->tree, side->parent.inode, side->name, side->length,
	    &side->subvolume);
	if (error != BTRFS_OK) {
		return error;
	}
	side->is_subvolume = side->subvolume.referenced;
	side->stub = !side->subvolume.referenced;
	if (side->stub) {
		return BTRFS_OK;
	}
	error = bt_tx_tree_view(transaction, side->subvolume.child, &view);
	error = error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
	if (error == BTRFS_OK && bt_u32(view.item.legacy.refs) == 0) {
		/* A deleted subvolume has no entry. */
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		side->location = view.key;
		side->read_only = (bt_u64(view.item.legacy.flags) & BT_ROOT_SUBVOL_READ_ONLY) != 0;
		error = bt_ns_named_inode(transaction, &view, BTRFS_ROOT_INODE, &side->item);
	}
	return error;
}

static int
bt_rn_directory(const struct bt_rn_side *side)
{
	return side->is_subvolume || side->stub || bt_ns_is_directory(&side->item);
}

/* Whether the two sides name one object. Every lookup of a stub makes a new
 * inode in Linux, so a stub is no other name's object. */
static int
bt_rn_same(const struct bt_rn_side *a, const struct bt_rn_side *b)
{
	if (a->is_subvolume && b->is_subvolume) {
		return a->subvolume.child == b->subvolume.child;
	}
	return !a->is_subvolume && !a->stub && !b->is_subvolume && !b->stub &&
	    a->parent.tree == b->parent.tree && a->entry.inode == b->entry.inode;
}

/* Whether directory is side's directory or lies below it; a stub is empty. */
static enum btrfs_result
bt_rn_contains(struct btrfs_transaction *transaction, const struct bt_rn_side *side,
    struct btrfs_object_id directory, int *contains)
{
	struct btrfs_object_id object;

	*contains = 0;
	if (!side->exists || side->stub || !bt_rn_directory(side)) {
		return BTRFS_OK;
	}
	object = side->is_subvolume
	    ? (struct btrfs_object_id){ side->subvolume.child, BTRFS_ROOT_INODE }
	    : (struct btrfs_object_id){ side->parent.tree, side->entry.inode };
	return bt_rn_inside(transaction, directory, object, contains);
}

/* Removes side's name: btrfs_unlink_subvol for a subvolume entry or a stub,
 * __btrfs_unlink_inode otherwise. */
static enum btrfs_result
bt_rn_remove(struct btrfs_transaction *transaction, struct bt_rn_side *side, struct btrfs_time time)
{
	if (side->is_subvolume || side->stub) {
		return bt_sv_unlink(transaction, side->tree, side->parent.inode, side->name,
		    side->length, &side->subvolume, time);
	}
	return bt_ns_remove_entry(transaction, side->tree, side->parent.inode, side->name,
	    side->length, &side->entry, time);
}

/* Names side's object in place's directory under place's name with index,
 * as btrfs_add_link does once an inode's back reference is in place: a
 * subvolume gains its root references there. */
static enum btrfs_result
bt_rn_place(struct btrfs_transaction *transaction, const struct bt_rn_side *side,
    const struct bt_rn_side *place, uint64_t index, struct btrfs_time time)
{
	enum btrfs_result error;

	if (side->is_subvolume) {
		return bt_sv_link(transaction, place->tree, place->parent.inode, place->name,
		    place->length, side->location, index, time);
	}
	error = bt_ns_insert_entry(transaction, place->tree, place->parent.inode, place->name,
	    place->length, side->location, side->type, index);
	if (error == BTRFS_OK) {
		error = bt_ns_directory(
		    transaction, place->tree, place->parent.inode, place->length, 1, time);
	}
	return error;
}

/* An inode records the change; a subvolume's root directory keeps its times,
 * as Linux does not write it. */
static enum btrfs_result
bt_rn_touch(struct btrfs_transaction *transaction, struct bt_rn_side *side, struct btrfs_time time)
{
	enum btrfs_result error;

	if (side->is_subvolume) {
		return BTRFS_OK;
	}
	error = bt_ns_named_inode(transaction, side->tree, side->entry.inode, &side->item);
	if (error == BTRFS_OK) {
		error = bt_ns_store(transaction, side->tree, side->entry.inode, &side->item, time);
	}
	return error;
}

static int
bt_rn_moving(struct btrfs_object_id old_parent, struct btrfs_object_id new_parent)
{
	return old_parent.tree != new_parent.tree || old_parent.inode != new_parent.inode;
}

/* Linux's renameat2 without flags. A subvolume entry may move into another
 * subvolume's directory, other names may not (btrfs_rename's EXDEV). A
 * subvolume or a directory may replace an empty directory or a stub; nothing
 * replaces a subvolume (ENOTEMPTY), and a stub does not move. A read-only
 * subvolume stays in its directory: btrfs_permission refuses to write its
 * root for a new parent. *moved reports whether anything changed. */
static enum btrfs_result
bt_rn_rename(struct btrfs_transaction *transaction, struct btrfs_object_id old_parent,
    const void *old_name, size_t old_length, struct btrfs_object_id new_parent,
    const void *new_name, size_t new_length, struct btrfs_time time, int target_open, int *moved)
{
	struct bt_rn_side source;
	struct bt_rn_side target;
	struct bt_removed removed;
	uint64_t index = 0;
	uint64_t inode;
	int moving = bt_rn_moving(old_parent, new_parent);
	int contains = 0;
	enum btrfs_result error;

	*moved = 0;
	if (time.nanoseconds >= BT_NANOSECONDS) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_ns_name(old_name, old_length);
	if (error == BTRFS_OK) {
		error = bt_ns_name(new_name, new_length);
	}
	if (error == BTRFS_OK && new_parent.inode == BTRFS_EMPTY_SUBVOLUME_INODE) {
		/* A stub directory takes no names: btrfs_rename's EPERM. */
		error = BTRFS_NOT_PERMITTED;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	source =
	    (struct bt_rn_side){ .parent = old_parent, .name = old_name, .length = old_length };
	target =
	    (struct bt_rn_side){ .parent = new_parent, .name = new_name, .length = new_length };
	error = bt_rn_resolve(transaction, &source, 0);
	if (error == BTRFS_OK) {
		error = bt_rn_resolve(transaction, &target, 1);
	}
	/* The VFS: the old name may not move below itself, and the new one may
	 * not name an ancestor of the old one. */
	if (error == BTRFS_OK && moving) {
		error = bt_rn_contains(transaction, &source, new_parent, &contains);
		error = error == BTRFS_OK && contains ? BTRFS_INVALID_ARGUMENT : error;
	}
	if (error == BTRFS_OK && moving) {
		error = bt_rn_contains(transaction, &target, old_parent, &contains);
		error = error == BTRFS_OK && contains ? BTRFS_NOT_EMPTY : error;
	}
	if (error == BTRFS_OK && target.exists && bt_rn_same(&source, &target)) {
		/* One object under both names: POSIX rename does nothing. */
		return BTRFS_OK;
	}
	/* may_delete for the old name and a replaced one, may_create for a new
	 * name, then write access to a directory moving to a new parent. */
	if (error == BTRFS_OK &&
	    (bt_ns_frozen(&source.directory) || bt_ns_frozen(&source.item) ||
		bt_ns_immutable(&target.directory) || bt_ns_frozen(&target.item) ||
		(target.exists && (bt_u64(target.directory.flags) & BT_INODE_APPEND) != 0))) {
		error = BTRFS_NOT_PERMITTED;
	}
	if (error == BTRFS_OK && target.exists &&
	    bt_rn_directory(&target) != bt_rn_directory(&source)) {
		error = bt_rn_directory(&source) ? BTRFS_NOT_DIRECTORY : BTRFS_IS_DIRECTORY;
	}
	if (error == BTRFS_OK && moving && source.read_only) {
		error = BTRFS_READ_ONLY;
	}
	/* btrfs_rename's own refusals. */
	if (error == BTRFS_OK && source.stub) {
		error = BTRFS_NOT_EMPTY;
	}
	if (error == BTRFS_OK && !source.is_subvolume && old_parent.tree != new_parent.tree) {
		error = BTRFS_CROSS_TREE;
	}
	if (error == BTRFS_OK && target.exists &&
	    (target.is_subvolume ||
		(!target.stub && bt_rn_directory(&target) && bt_u64(target.item.size) != 0))) {
		error = BTRFS_NOT_EMPTY;
	}
	/* A replaced name leaves room for the new one in its DIR_ITEM; the old
	 * name leaves the items it shares with the new one. */
	inode = source.is_subvolume ? 0 : source.entry.inode;
	if (error == BTRFS_OK) {
		removed = (struct bt_removed){ old_parent.inode, old_name, old_length,
			!source.is_subvolume && source.entry.extended };
		error = bt_ns_room(transaction, target.tree, new_parent.inode, new_name, new_length,
		    !target.exists, inode, old_parent.tree == new_parent.tree ? &removed : NULL);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_index(transaction, target.tree, new_parent.inode, &index);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	/* btrfs_rename's order: the old name, the replaced name and its link
	 * (or a stub), then the new name with the index taken above. */
	error = bt_rn_remove(transaction, &source, time);
	if (error == BTRFS_OK && target.exists) {
		error = bt_rn_remove(transaction, &target, time);
	}
	if (error == BTRFS_OK && target.exists && !target.stub) {
		error =
		    bt_ns_release(transaction, target.tree, target.entry.inode, target_open, time);
	}
	if (error == BTRFS_OK && inode != 0) {
		error = bt_ns_add_ref(
		    transaction, target.tree, new_parent.inode, new_name, new_length, inode, index);
	}
	if (error == BTRFS_OK) {
		error = bt_rn_place(transaction, &source, &target, index, time);
	}
	if (error == BTRFS_OK) {
		error = bt_rn_touch(transaction, &source, time);
	}
	if (error == BTRFS_OK) {
		transaction->changed = 1;
		*moved = 1;
	}
	return bt_ns_poison(transaction, error);
}

enum btrfs_result
btrfs_transaction_rename(struct btrfs_transaction *transaction, struct btrfs_object_id old_parent,
    const void *old_name, size_t old_length, struct btrfs_object_id new_parent,
    const void *new_name, size_t new_length, struct btrfs_time time, int target_open)
{
	int moved = 0;

	if (transaction == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	return bt_rn_rename(transaction, old_parent, old_name, old_length, new_parent, new_name,
	    new_length, time, target_open, &moved);
}

/* RENAME_WHITEOUT: the rename, then a whiteout (a character device 0:0
 * without permission bits, owned by uid and gid) under the old name, created
 * as btrfs_rename creates it after the move. */
enum btrfs_result
btrfs_transaction_rename_whiteout(struct btrfs_transaction *transaction,
    struct btrfs_object_id old_parent, const void *old_name, size_t old_length,
    struct btrfs_object_id new_parent, const void *new_name, size_t new_length, uint32_t uid,
    uint32_t gid, struct btrfs_time time, int target_open)
{
	struct btrfs_new_inode whiteout;
	struct btrfs_object_id id;
	int moved = 0;
	enum btrfs_result error;

	if (transaction == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_rn_rename(transaction, old_parent, old_name, old_length, new_parent, new_name,
	    new_length, time, target_open, &moved);
	if (error != BTRFS_OK || !moved) {
		return error;
	}
	bt_zero(&whiteout, sizeof(whiteout));
	whiteout.mode = BTRFS_MODE_CHARACTER;
	whiteout.uid = uid;
	whiteout.gid = gid;
	whiteout.time = time;
	error =
	    btrfs_transaction_create(transaction, old_parent, old_name, old_length, &whiteout, &id);
	return bt_ns_poison(transaction, error);
}

/* RENAME_EXCHANGE as btrfs_rename_exchange does it: each name now names the
 * other object. Both indexes are taken first (the source's in the new
 * directory), an inode's back reference inserted while its old one still
 * exists, both old names removed, then both entries inserted under the
 * swapped indexes. Subvolume entries may be exchanged across subvolumes, other
 * names only within one; neither may move below itself, and a stub does not
 * move (btrfs_rename_exchange would abort its transaction). */
enum btrfs_result
btrfs_transaction_exchange(struct btrfs_transaction *transaction, struct btrfs_object_id old_parent,
    const void *old_name, size_t old_length, struct btrfs_object_id new_parent,
    const void *new_name, size_t new_length, struct btrfs_time time)
{
	struct bt_rn_side source;
	struct bt_rn_side target;
	uint64_t source_index = 0;
	uint64_t target_index = 0;
	int moving = bt_rn_moving(old_parent, new_parent);
	int contains = 0;
	enum btrfs_result error;

	if (transaction == NULL || time.nanoseconds >= BT_NANOSECONDS) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_ns_name(old_name, old_length);
	if (error == BTRFS_OK) {
		error = bt_ns_name(new_name, new_length);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	source =
	    (struct bt_rn_side){ .parent = old_parent, .name = old_name, .length = old_length };
	target =
	    (struct bt_rn_side){ .parent = new_parent, .name = new_name, .length = new_length };
	error = bt_rn_resolve(transaction, &source, 0);
	if (error == BTRFS_OK) {
		error = bt_rn_resolve(transaction, &target, 0);
	}
	/* The VFS: neither name may move below itself. */
	if (error == BTRFS_OK && moving) {
		error = bt_rn_contains(transaction, &source, new_parent, &contains);
	}
	if (error == BTRFS_OK && moving && !contains) {
		error = bt_rn_contains(transaction, &target, old_parent, &contains);
	}
	if (error == BTRFS_OK && contains) {
		error = BTRFS_INVALID_ARGUMENT;
	}
	if (error == BTRFS_OK && bt_rn_same(&source, &target)) {
		return BTRFS_OK;
	}
	/* may_delete for both names in both directories, then write access to
	 * directories moving to a new parent. */
	if (error == BTRFS_OK &&
	    (bt_ns_frozen(&source.directory) || bt_ns_frozen(&target.directory) ||
		bt_ns_frozen(&source.item) || bt_ns_frozen(&target.item))) {
		error = BTRFS_NOT_PERMITTED;
	}
	if (error == BTRFS_OK && moving && (source.read_only || target.read_only)) {
		error = BTRFS_READ_ONLY;
	}
	/* btrfs_rename_exchange's own refusals. */
	if (error == BTRFS_OK && old_parent.tree != new_parent.tree &&
	    (!source.is_subvolume || !target.is_subvolume)) {
		error = BTRFS_CROSS_TREE;
	}
	if (error == BTRFS_OK && (source.stub || target.stub)) {
		error = BTRFS_NOT_EMPTY;
	}
	if (error == BTRFS_OK && !source.is_subvolume) {
		error = bt_ns_room(transaction, target.tree, new_parent.inode, new_name, new_length,
		    0, source.entry.inode, NULL);
	}
	if (error == BTRFS_OK && !target.is_subvolume) {
		error = bt_ns_room(transaction, source.tree, old_parent.inode, old_name, old_length,
		    0, target.entry.inode, NULL);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_index(transaction, target.tree, new_parent.inode, &source_index);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_index(transaction, source.tree, old_parent.inode, &target_index);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	if (!source.is_subvolume) {
		error = bt_ns_add_ref(transaction, target.tree, new_parent.inode, new_name,
		    new_length, source.entry.inode, source_index);
	}
	if (error == BTRFS_OK && !target.is_subvolume) {
		error = bt_ns_add_ref(transaction, source.tree, old_parent.inode, old_name,
		    old_length, target.entry.inode, target_index);
	}
	if (error == BTRFS_OK) {
		error = bt_rn_remove(transaction, &source, time);
	}
	if (error == BTRFS_OK) {
		error = bt_rn_remove(transaction, &target, time);
	}
	if (error == BTRFS_OK) {
		error = bt_rn_place(transaction, &source, &target, source_index, time);
	}
	if (error == BTRFS_OK) {
		error = bt_rn_place(transaction, &target, &source, target_index, time);
	}
	if (error == BTRFS_OK) {
		error = bt_rn_touch(transaction, &source, time);
	}
	if (error == BTRFS_OK) {
		error = bt_rn_touch(transaction, &target, time);
	}
	if (error == BTRFS_OK) {
		transaction->changed = 1;
	}
	return bt_ns_poison(transaction, error);
}
