/* SPDX-License-Identifier: BSD-3-Clause */
/* Subvolume and snapshot creation, following Linux's create_subvol and
 * create_pending_snapshot. */
#include "namespace.h"
#include "qgroup.h"

/* Linux's placeholder inode inside a new root item. */
#define BT_ROOT_ITEM_INODE_SIZE 3U
#define BT_ROOT_ITEM_INODE_MODE (BTRFS_MODE_DIRECTORY | 0755U)
#define BT_PARENT_NAME ".."

/* A subvolume's UUID tree key: the UUID's two little-endian halves. */
static struct bt_key
bt_sv_uuid_key(const uint8_t *uuid, uint8_t type)
{
	struct bt_le64 high;
	struct bt_le64 low;

	bt_copy(&high, uuid, sizeof(high));
	bt_copy(&low, uuid + sizeof(high), sizeof(low));
	return (struct bt_key){ bt_u64(high), bt_u64(low), type };
}

/* Adds tree to the UUID tree item of uuid, as btrfs_uuid_tree_add: an id
 * already listed is not added twice. */
static enum btrfs_result
bt_sv_uuid_add(
    struct btrfs_transaction *transaction, const uint8_t *uuid, uint8_t type, uint64_t tree)
{
	struct bt_key key = bt_sv_uuid_key(uuid, type);
	struct bt_le64 *ids = (void *)transaction->item;
	size_t size = 0;
	size_t i;
	enum btrfs_result error;

	error = bt_mutation_find(transaction->mutation, transaction->uuids.root, key,
	    transaction->item, transaction->base->info.node_size, &size);
	if (error != BTRFS_OK && error != BTRFS_NOT_FOUND) {
		return error == BTRFS_RANGE ? BTRFS_CORRUPT : error;
	}
	if (size % sizeof(*ids) != 0) {
		return BTRFS_CORRUPT;
	}
	for (i = 0; error == BTRFS_OK && i < size / sizeof(*ids); i++) {
		if (bt_u64(ids[i]) == tree) {
			return BTRFS_OK;
		}
	}
	if (size + sizeof(*ids) > bt_ns_item_limit(transaction)) {
		return BTRFS_RANGE;
	}
	bt_put64(&ids[size / sizeof(*ids)], tree);
	return bt_tx_edit(transaction, &transaction->uuids.root, key, transaction->item,
	    size + sizeof(*ids), error == BTRFS_OK ? BT_REPLACE : BT_INSERT);
}

/* The ROOT_BACKREF of child and the ROOT_REF of parent: the directory, index
 * and name of child's entry in parent, as btrfs_add_root_ref. */
static enum btrfs_result
bt_sv_add_root_refs(struct btrfs_transaction *transaction, uint64_t parent, uint64_t child,
    uint64_t directory, uint64_t index, const void *name, size_t length)
{
	struct bt_disk_root_ref *ref = (void *)transaction->entry;
	struct bt_key key = { child, parent, BT_ROOT_BACKREF };
	enum btrfs_result error;

	bt_put64(&ref->directory, directory);
	bt_put64(&ref->index, index);
	bt_put16(&ref->name_length, (uint16_t)length);
	bt_copy(ref + 1, name, length);
	error = bt_tx_edit(
	    transaction, &transaction->roots, key, ref, sizeof(*ref) + length, BT_INSERT);
	if (error == BTRFS_OK) {
		key = (struct bt_key){ parent, child, BT_ROOT_REF };
		error = bt_tx_edit(
		    transaction, &transaction->roots, key, ref, sizeof(*ref) + length, BT_INSERT);
	}
	return error == BTRFS_EXISTS ? BTRFS_CORRUPT : error;
}

/* Checks a new entry for a subvolume in directory before any change and
 * takes its index. */
static enum btrfs_result
bt_sv_prepare_entry(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    uint64_t directory, const void *name, size_t length, uint64_t *index)
{
	struct bt_disk_inode item;
	enum btrfs_result error;

	if (!transaction->has_uuids) {
		/* Linux creates the UUID tree at mount before any subvolume. */
		return BTRFS_UNSUPPORTED;
	}
	error = bt_ns_parent(transaction, tree, directory, &item);
	if (error == BTRFS_OK) {
		error = bt_ns_absent(transaction, tree, directory, name, length);
	}
	if (error == BTRFS_OK && bt_ns_immutable(&item)) {
		error = BTRFS_NOT_PERMITTED;
	}
	if (error == BTRFS_OK) {
		error = bt_ns_room(transaction, tree, directory, name, length, 1, 0, NULL);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_index(transaction, tree, directory, index);
	}
	return error;
}

/* The directory entry, root references and parent directory update that
 * btrfs_add_link makes for a subvolume root. */
enum btrfs_result
bt_sv_link(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t directory,
    const void *name, size_t length, struct bt_key location, uint64_t index, struct btrfs_time time)
{
	enum btrfs_result error;

	error = bt_sv_add_root_refs(
	    transaction, tree->root.owner, location.objectid, directory, index, name, length);
	if (error == BTRFS_OK) {
		error = bt_ns_insert_entry(transaction, tree, directory, name, length, location,
		    BTRFS_FT_DIRECTORY, index);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_directory(transaction, tree, directory, length, 1, time);
	}
	return error;
}

static void
bt_sv_time(struct bt_disk_time *wire, struct btrfs_time time)
{
	bt_put64(&wire->seconds, (uint64_t)time.seconds);
	bt_put32(&wire->nanoseconds, time.nanoseconds);
}

/* The root directory of a new subvolume: inode 256 with one link and its ".."
 * INODE_REF, as btrfs_create_new_inode makes it for a subvolume. Subvolumes
 * do not inherit inode flags; they inherit the compression property of the
 * parent subvolume's root directory. */
static enum btrfs_result
bt_sv_root_directory(struct btrfs_transaction *transaction, struct bt_owned_root *parent,
    struct bt_owned_root *tree, const struct btrfs_new_inode *attributes)
{
	struct bt_disk_inode item;
	struct bt_disk_inode_ref *ref = (void *)transaction->entry;
	const struct bt_codec *codec = NULL;
	struct bt_disk_inode parent_root;
	struct bt_key key = { BTRFS_ROOT_INODE, 0, BT_INODE_ITEM };
	struct bt_key none = { 0, 0, 0 };
	uint64_t transid = bt_ns_transid(transaction);
	uint64_t directory = bt_u64(parent->item.legacy.root_dir);
	size_t size;
	enum btrfs_result error;

	error = bt_ns_inode(transaction, parent, directory, &parent_root);
	if (error == BTRFS_OK) {
		error = bt_ns_inherited_codec(transaction, parent, directory, 0, &codec);
	}
	if (error != BTRFS_OK) {
		return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
	}
	bt_zero(&item, sizeof(item));
	bt_put64(&item.generation, transid);
	bt_put64(&item.transid, transid);
	bt_put32(&item.links, 1);
	bt_put32(&item.uid, attributes->uid);
	bt_put32(&item.gid, attributes->gid);
	bt_put32(&item.mode, attributes->mode);
	bt_put64(&item.flags, codec != NULL ? BT_INODE_COMPRESS : 0);
	bt_sv_time(&item.atime, attributes->time);
	item.ctime = item.atime;
	item.mtime = item.atime;
	item.otime = item.atime;
	error = bt_tx_edit(transaction, &tree->root, key, &item, sizeof(item), BT_INSERT);
	if (error == BTRFS_OK) {
		bt_put64(&ref->index, 0);
		bt_put16(&ref->name_length, (uint16_t)bt_ns_length(BT_PARENT_NAME));
		bt_copy(ref + 1, BT_PARENT_NAME, bt_ns_length(BT_PARENT_NAME));
		key = (struct bt_key){ BTRFS_ROOT_INODE, BTRFS_ROOT_INODE, BT_INODE_REF };
		error = bt_tx_edit(transaction, &tree->root, key, ref,
		    sizeof(*ref) + bt_ns_length(BT_PARENT_NAME), BT_INSERT);
	}
	if (error == BTRFS_OK && codec != NULL) {
		size = bt_ns_length(BT_COMPRESSION_PROPERTY);
		key = (struct bt_key){ BTRFS_ROOT_INODE, bt_ns_hash(BT_COMPRESSION_PROPERTY, size),
			BT_XATTR_ITEM };
		size = bt_ns_dir_entry(transaction->entry, none, transid, BT_COMPRESSION_PROPERTY,
		    size, codec->name, bt_ns_length(codec->name), BTRFS_FT_XATTR);
		error =
		    bt_tx_edit(transaction, &tree->root, key, transaction->entry, size, BT_INSERT);
		bt_ns_require_feature(transaction, codec->feature);
	}
	return error;
}

enum btrfs_result
btrfs_transaction_create_subvolume(struct btrfs_transaction *transaction,
    struct btrfs_object_id parent, const void *name, size_t length,
    const struct btrfs_new_inode *attributes, const uint8_t uuid[BTRFS_UUID_SIZE], uint64_t *result)
{
	struct bt_owned_root *tree = NULL;
	struct bt_owned_root *created = NULL;
	struct bt_owned_root owned;
	struct bt_disk_root_full *item = &owned.item;
	struct bt_key location;
	uint64_t transid;
	uint64_t index = 0;
	uint64_t id = 0;
	enum btrfs_result error;

	if (attributes == NULL || uuid == NULL || result == NULL ||
	    attributes->time.nanoseconds >= 1000000000U ||
	    (attributes->mode & BTRFS_MODE_TYPE) != BTRFS_MODE_DIRECTORY ||
	    (attributes->mode & ~(uint32_t)(BTRFS_MODE_TYPE | 07777U)) != 0 ||
	    attributes->target != NULL || attributes->target_length != 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_ns_name(name, length);
	if (error == BTRFS_OK) {
		error = bt_ns_begin(transaction, parent.tree, &tree);
	}
	if (error == BTRFS_OK) {
		error = bt_sv_prepare_entry(transaction, tree, parent.inode, name, length, &index);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_root_id(transaction, &id);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	/* Linux's create_subvol: an empty leaf, its root item and UUID. */
	transid = bt_ns_transid(transaction);
	bt_zero(&owned, sizeof(owned));
	error = bt_mutation_new_root(transaction->mutation, transaction->roots, id, 0, &owned.root);
	if (error == BTRFS_OK) {
		error = bt_qgroup_create(transaction, id);
	}
	bt_put64(&item->legacy.inode.generation, 1);
	bt_put64(&item->legacy.inode.size, BT_ROOT_ITEM_INODE_SIZE);
	bt_put32(&item->legacy.inode.links, 1);
	bt_put64(&item->legacy.inode.nbytes, transaction->base->info.node_size);
	bt_put32(&item->legacy.inode.mode, BT_ROOT_ITEM_INODE_MODE);
	bt_put64(&item->legacy.inode.flags, BT_INODE_ROOT_ITEM_INIT);
	bt_put64(&item->legacy.bytenr, owned.root.address);
	bt_put64(&item->legacy.generation, transid);
	bt_put64(&item->generation_v2, transid);
	bt_put64(&item->legacy.root_dir, BTRFS_ROOT_INODE);
	bt_put32(&item->legacy.refs, 1);
	/* The new leaf's bytes join used_bytes at commit. */
	bt_put64(&item->legacy.used_bytes, 0);
	bt_copy(item->uuid, uuid, BTRFS_UUID_SIZE);
	bt_sv_time(&item->otime, attributes->time);
	item->ctime = item->otime;
	bt_put64(&item->ctransid, transid);
	bt_put64(&item->otransid, transid);
	owned.key = (struct bt_key){ id, 0, BT_ROOT_ITEM };
	owned.size = sizeof(owned.item);
	if (error == BTRFS_OK) {
		error = bt_tx_edit(
		    transaction, &transaction->roots, owned.key, item, owned.size, BT_INSERT);
		error = error == BTRFS_EXISTS ? BTRFS_CORRUPT : error;
	}
	if (error == BTRFS_OK) {
		error = bt_tx_add_tree(transaction, &owned, &created);
	}
	if (error == BTRFS_OK) {
		error = bt_sv_uuid_add(transaction, uuid, BT_UUID_SUBVOL, id);
	}
	if (error == BTRFS_OK) {
		error = bt_sv_root_directory(transaction, tree, created, attributes);
	}
	if (error == BTRFS_OK) {
		/* btrfs_add_link names a subvolume by its root key. */
		location = (struct bt_key){ id, 0, BT_ROOT_ITEM };
		error = bt_sv_link(transaction, tree, parent.inode, name, length, location, index,
		    attributes->time);
	}
	if (error == BTRFS_OK) {
		transaction->changed = 1;
		*result = id;
	}
	return bt_ns_poison(transaction, error);
}

/* Whether this transaction changed any block of tree. */
static enum btrfs_result
bt_sv_changed(struct btrfs_transaction *transaction, uint64_t tree, int *changed)
{
	struct bt_mutated_block block;
	size_t i;
	enum btrfs_result error;

	*changed = 0;
	for (i = 0; i < bt_mutation_count(transaction->mutation) && !*changed; i++) {
		error = bt_mutation_block(transaction->mutation, i, &block);
		if (error != BTRFS_OK) {
			return error;
		}
		*changed = block.owner == tree;
	}
	return BTRFS_OK;
}

enum btrfs_result
btrfs_transaction_snapshot(struct btrfs_transaction *transaction, uint64_t source,
    struct btrfs_object_id parent, const void *name, size_t length, int read_only,
    struct btrfs_time time, const uint8_t uuid[BTRFS_UUID_SIZE], uint64_t *result)
{
	static const uint8_t empty[BTRFS_UUID_SIZE];
	struct bt_owned_root *tree = NULL;
	struct bt_owned_root *origin = NULL;
	struct bt_owned_root *created = NULL;
	struct bt_owned_root owned;
	struct bt_disk_root_full *item = &owned.item;
	struct bt_key location;
	uint64_t transid;
	uint64_t used;
	uint64_t flags;
	uint64_t index = 0;
	uint64_t id = 0;
	int changed = 0;
	enum btrfs_result error;

	if (uuid == NULL || result == NULL || time.nanoseconds >= 1000000000U) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_ns_name(name, length);
	if (error == BTRFS_OK) {
		error = bt_ns_begin(transaction, parent.tree, &tree);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_source(transaction, source, &origin);
	}
	/* The copy shares only committed blocks: their references exist. */
	if (error == BTRFS_OK) {
		error = bt_sv_changed(transaction, source, &changed);
	}
	if (error == BTRFS_OK && changed) {
		error = BTRFS_UNSUPPORTED;
	}
	/* With quotas the snapshot is the transaction's first change: its
	 * accounting starts from the base with only the copy added. */
	if (error == BTRFS_OK && transaction->qgroups != NULL &&
	    (transaction->changed || bt_mutation_count(transaction->mutation) != 0)) {
		error = BTRFS_UNSUPPORTED;
	}
	if (error == BTRFS_OK &&
	    bt_u64(origin->item.legacy.used_bytes) < transaction->base->info.node_size) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		error = bt_sv_prepare_entry(transaction, tree, parent.inode, name, length, &index);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_root_id(transaction, &id);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	/* Linux's create_pending_snapshot: the source records this transaction
	 * as its last snapshot, and the copy starts from its root item. */
	transid = bt_ns_transid(transaction);
	bt_put64(&origin->item.legacy.last_snapshot, transid);
	bt_zero(&owned, sizeof(owned));
	owned.item = origin->item;
	error = bt_mutation_new_root(transaction->mutation, origin->root, id, 1, &owned.root);
	if (error == BTRFS_OK) {
		error = bt_qgroup_create(transaction, id);
	}
	if (error == BTRFS_OK) {
		error = bt_qgroup_snapshot(
		    transaction, source, id, origin->root.address, owned.root.address);
	}
	if (error == BTRFS_OK) {
		error = bt_tree_read(
		    bt_mutation_view(transaction->mutation), owned.root, transaction->original);
	}
	/* btrfs_inc_ref: every child of the copied root gains a reference from
	 * the snapshot. */
	if (error == BTRFS_OK) {
		error = bt_tx_children(
		    transaction, transaction->original, owned.root.address, 0, id, 1);
	}
	if ((bt_u64(item->legacy.inode.flags) & BT_INODE_ROOT_ITEM_INIT) == 0) {
		bt_put64(&item->legacy.inode.flags,
		    bt_u64(item->legacy.inode.flags) | BT_INODE_ROOT_ITEM_INIT);
		bt_put64(&item->legacy.flags, 0);
		bt_put64(&item->legacy.byte_limit, 0);
	}
	flags = bt_u64(item->legacy.flags) & ~BT_ROOT_SUBVOL_READ_ONLY;
	bt_put64(&item->legacy.flags, flags | (read_only ? BT_ROOT_SUBVOL_READ_ONLY : 0));
	bt_put64(&item->generation_v2, transid);
	bt_copy(item->uuid, uuid, BTRFS_UUID_SIZE);
	bt_copy(item->parent_uuid, origin->item.uuid, BTRFS_UUID_SIZE);
	if (!read_only) {
		bt_zero(item->received_uuid, BTRFS_UUID_SIZE);
		bt_zero(&item->stime, sizeof(item->stime));
		bt_zero(&item->rtime, sizeof(item->rtime));
		bt_put64(&item->stransid, 0);
		bt_put64(&item->rtransid, 0);
	}
	bt_sv_time(&item->otime, time);
	bt_put64(&item->otransid, transid);
	bt_put64(&item->legacy.bytenr, owned.root.address);
	bt_put64(&item->legacy.generation, transid);
	item->legacy.level = owned.root.level;
	/* btrfs_copy_root does not count the copy in used_bytes; the commit
	 * counts every new block of the tree. */
	used = bt_u64(item->legacy.used_bytes);
	bt_put64(&item->legacy.used_bytes, used - transaction->base->info.node_size);
	/* The snapshot's root item key records when it was taken. */
	owned.key = (struct bt_key){ id, transid, BT_ROOT_ITEM };
	owned.size = sizeof(owned.item);
	owned.read_only = 0;
	if (error == BTRFS_OK) {
		error = bt_tx_edit(
		    transaction, &transaction->roots, owned.key, item, owned.size, BT_INSERT);
		error = error == BTRFS_EXISTS ? BTRFS_CORRUPT : error;
	}
	if (error == BTRFS_OK) {
		error = bt_tx_add_tree(transaction, &owned, &created);
	}
	if (error == BTRFS_OK) {
		/* A read-only snapshot refuses edits for the rest of the
		 * transaction too. */
		created->read_only = read_only;
		location = (struct bt_key){ id, UINT64_MAX, BT_ROOT_ITEM };
		error = bt_sv_link(
		    transaction, tree, parent.inode, name, length, location, index, time);
	}
	if (error == BTRFS_OK) {
		error = bt_sv_uuid_add(transaction, uuid, BT_UUID_SUBVOL, id);
	}
	if (error == BTRFS_OK && !bt_equal(item->received_uuid, empty, BTRFS_UUID_SIZE)) {
		error =
		    bt_sv_uuid_add(transaction, item->received_uuid, BT_UUID_RECEIVED_SUBVOL, id);
	}
	if (error == BTRFS_OK) {
		transaction->changed = 1;
		*result = id;
	}
	return bt_ns_poison(transaction, error);
}

/* Removes tree from the UUID tree item of uuid, as btrfs_uuid_tree_remove; a
 * missing item or id is not an error. */
static enum btrfs_result
bt_sv_uuid_remove(
    struct btrfs_transaction *transaction, const uint8_t *uuid, uint8_t type, uint64_t tree)
{
	struct bt_key key = bt_sv_uuid_key(uuid, type);
	struct bt_le64 *ids = (void *)transaction->item;
	size_t size = 0;
	size_t count;
	size_t i;
	enum btrfs_result error;

	error = bt_mutation_find(transaction->mutation, transaction->uuids.root, key,
	    transaction->item, transaction->base->info.node_size, &size);
	if (error == BTRFS_NOT_FOUND) {
		return BTRFS_OK;
	}
	if (error != BTRFS_OK || size % sizeof(*ids) != 0) {
		return error == BTRFS_OK || error == BTRFS_RANGE ? BTRFS_CORRUPT : error;
	}
	count = size / sizeof(*ids);
	for (i = 0; i < count && bt_u64(ids[i]) != tree; i++) {
	}
	if (i == count) {
		return BTRFS_OK;
	}
	if (count == 1) {
		return bt_tx_edit(transaction, &transaction->uuids.root, key, NULL, 0, BT_DELETE);
	}
	bt_move(&ids[i], &ids[i + 1], (count - i - 1) * sizeof(*ids));
	return bt_tx_edit(transaction, &transaction->uuids.root, key, transaction->item,
	    size - sizeof(*ids), BT_REPLACE);
}

/* The index of name among directory's DIR_INDEX items, as
 * btrfs_search_dir_index_item finds a stub's index. */
static enum btrfs_result
bt_sv_stub_index(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    uint64_t directory, const void *name, size_t length, uint64_t *index)
{
	const struct bt_disk_dir *header;
	const uint8_t *entry_name;
	const uint8_t *data;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { directory, 0, BT_DIR_INDEX };
	size_t offset;
	uint64_t steps;
	enum btrfs_result error;

	bt_cursor_init(&cursor, bt_mutation_view(transaction->mutation), tree->root);
	error = bt_cursor_seek(&cursor, key, 0);
	for (steps = 0; error == BTRFS_OK; steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != directory || record.key.type != BT_DIR_INDEX) {
			error = BTRFS_NOT_FOUND;
			break;
		}
		offset = 0;
		error = bt_dir_record(&record, &offset, &header, &entry_name, &data);
		if (error == BTRFS_OK && bt_u16(header->name_length) == length &&
		    bt_equal(entry_name, name, length)) {
			*index = record.key.offset;
			break;
		}
		error = error == BTRFS_OK ? bt_cursor_next(&cursor) : error;
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
}

/* Resolves the subvolume entry name in directory, as btrfs_unlink_subvol. */
enum btrfs_result
bt_sv_entry(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t directory,
    const void *name, size_t length, struct bt_sv_entry *entry)
{
	const struct bt_disk_dir *header = NULL;
	const struct bt_disk_root_ref *ref;
	struct bt_packed packed;
	struct bt_key key = { directory, bt_ns_hash(name, length), BT_DIR_ITEM };
	struct bt_key location;
	size_t offset;
	size_t entry_size;
	size_t size = 0;
	enum btrfs_result error;

	error = bt_ns_load(transaction, tree, key, &packed);
	if (error == BTRFS_OK && !packed.present) {
		error = BTRFS_NOT_FOUND;
	}
	if (error == BTRFS_OK) {
		error = bt_ns_dir_find(&packed, name, length, &offset, &entry_size, &header);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	location = bt_key_decode(&header->location);
	if (location.type != BT_ROOT_ITEM) {
		/* Not a subvolume: rmdir or unlink removes it. */
		return BTRFS_INVALID_ARGUMENT;
	}
	if (!bt_file_tree(location.objectid) || location.objectid == tree->root.owner) {
		return BTRFS_CORRUPT;
	}
	entry->child = location.objectid;
	key = (struct bt_key){ tree->root.owner, entry->child, BT_ROOT_REF };
	error = bt_mutation_find(transaction->mutation, transaction->roots, key, transaction->item,
	    transaction->base->info.node_size, &size);
	ref = (const void *)transaction->item;
	entry->referenced = error == BTRFS_OK && size >= sizeof(*ref) &&
	    size == sizeof(*ref) + bt_u16(ref->name_length) &&
	    bt_u64(ref->directory) == directory && bt_u16(ref->name_length) == length &&
	    bt_equal(ref + 1, name, length);
	if (error != BTRFS_OK && error != BTRFS_NOT_FOUND) {
		return error == BTRFS_RANGE ? BTRFS_CORRUPT : error;
	}
	if (entry->referenced) {
		entry->index = bt_u64(ref->index);
		return BTRFS_OK;
	}
	return bt_sv_stub_index(transaction, tree, directory, name, length, &entry->index);
}

/* Linux's may_destroy_subvol: the default subvolume stays (NOT_PERMITTED), and
 * a subvolume holding subvolumes is NOT_EMPTY. */
static enum btrfs_result
bt_sv_may_destroy(struct btrfs_transaction *transaction, uint64_t child)
{
	static const char name[] = "default";
	const struct bt_disk_dir *header;
	struct bt_owned_root roots = { .root = transaction->roots };
	struct bt_packed packed;
	struct bt_key key = { BT_ROOT_DIR_OBJECTID, bt_ns_hash(name, sizeof(name) - 1),
		BT_DIR_ITEM };
	struct bt_key next;
	size_t offset;
	size_t entry_size;
	int found;
	enum btrfs_result error;

	error = bt_ns_load(transaction, &roots, key, &packed);
	if (error == BTRFS_OK && packed.present) {
		error =
		    bt_ns_dir_find(&packed, name, sizeof(name) - 1, &offset, &entry_size, &header);
		if (error == BTRFS_OK && bt_key_decode(&header->location).objectid == child) {
			return BTRFS_NOT_PERMITTED;
		}
		error = error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	key = (struct bt_key){ child, 0, BT_ROOT_REF };
	error = bt_ns_neighbor(transaction, &roots, key, 0, &next, &found);
	if (error == BTRFS_OK && found && next.objectid == child && next.type == BT_ROOT_REF) {
		error = BTRFS_NOT_EMPTY;
	}
	return error;
}

/* Removes the subvolume's entry from directory: its DIR_ITEM entry, DIR_INDEX
 * item and, when it is referenced, its root references. */
enum btrfs_result
bt_sv_unlink(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t directory,
    const void *name, size_t length, const struct bt_sv_entry *entry, struct btrfs_time time)
{
	const struct bt_disk_dir *header;
	struct bt_packed packed;
	struct bt_key key = { directory, bt_ns_hash(name, length), BT_DIR_ITEM };
	size_t offset = 0;
	size_t entry_size = 0;
	enum btrfs_result error;

	error = bt_ns_load(transaction, tree, key, &packed);
	if (error == BTRFS_OK) {
		error = bt_ns_dir_find(&packed, name, length, &offset, &entry_size, &header);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_cut(transaction, tree, &packed, offset, entry_size);
	}
	if (error == BTRFS_OK) {
		key = (struct bt_key){ directory, entry->index, BT_DIR_INDEX };
		error = bt_tx_edit(transaction, &tree->root, key, NULL, 0, BT_DELETE);
	}
	if (error == BTRFS_OK && entry->referenced) {
		key = (struct bt_key){ tree->root.owner, entry->child, BT_ROOT_REF };
		error = bt_tx_edit(transaction, &transaction->roots, key, NULL, 0, BT_DELETE);
		if (error == BTRFS_OK) {
			key = (struct bt_key){ entry->child, tree->root.owner, BT_ROOT_BACKREF };
			error =
			    bt_tx_edit(transaction, &transaction->roots, key, NULL, 0, BT_DELETE);
		}
	}
	if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		error = bt_ns_directory(transaction, tree, directory, length, 0, time);
	}
	return error;
}

enum btrfs_result
btrfs_transaction_delete_subvolume(struct btrfs_transaction *transaction,
    struct btrfs_object_id parent, const void *name, size_t length, struct btrfs_time time)
{
	static const uint8_t empty[BTRFS_UUID_SIZE];
	struct bt_owned_root *tree = NULL;
	struct bt_owned_root dead;
	struct bt_disk_inode directory;
	struct bt_sv_entry entry;
	struct bt_key orphan;
	size_t i;
	enum btrfs_result error;

	if (time.nanoseconds >= 1000000000U) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_ns_name(name, length);
	if (error == BTRFS_OK) {
		error = bt_ns_begin(transaction, parent.tree, &tree);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_parent(transaction, tree, parent.inode, &directory);
	}
	if (error == BTRFS_OK && bt_ns_frozen(&directory)) {
		error = BTRFS_NOT_PERMITTED;
	}
	if (error == BTRFS_OK) {
		error = bt_sv_entry(transaction, tree, parent.inode, name, length, &entry);
	}
	if (error == BTRFS_OK && !entry.referenced) {
		/* A stub goes alone; its subvolume belongs to another tree. */
		error = bt_sv_unlink(transaction, tree, parent.inode, name, length, &entry, time);
		if (error == BTRFS_OK) {
			transaction->changed = 1;
		}
		return bt_ns_poison(transaction, error);
	}
	if (error == BTRFS_OK) {
		error = bt_sv_may_destroy(transaction, entry.child);
	}
	/* A subvolume this transaction opened would have its root item rewritten
	 * at commit. */
	for (i = 0; error == BTRFS_OK && i < transaction->tree_count; i++) {
		if (transaction->trees[i].root.owner == entry.child) {
			error = BTRFS_UNSUPPORTED;
		}
	}
	bt_zero(&dead, sizeof(dead));
	if (error == BTRFS_OK) {
		error = bt_tx_root_item(transaction, entry.child, &dead);
	}
	if (error == BTRFS_OK && bt_u32(dead.item.legacy.refs) == 0) {
		error = BTRFS_CORRUPT;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	/* Linux's btrfs_delete_subvolume: the entry and root references go; the
	 * root item keeps refs 0, the dead flag and no drop progress, and an
	 * orphan item hands the tree to the cleaner. */
	error = bt_sv_unlink(transaction, tree, parent.inode, name, length, &entry, time);
	bt_put64(&dead.item.legacy.flags, bt_u64(dead.item.legacy.flags) | BT_ROOT_SUBVOL_DEAD);
	bt_zero(&dead.item.legacy.drop_progress, sizeof(dead.item.legacy.drop_progress));
	dead.item.legacy.drop_level = 0;
	bt_put32(&dead.item.legacy.refs, 0);
	if (error == BTRFS_OK) {
		error = bt_tx_edit(
		    transaction, &transaction->roots, dead.key, &dead.item, dead.size, BT_REPLACE);
	}
	if (error == BTRFS_OK) {
		orphan = (struct bt_key){ BT_ORPHAN_OBJECTID, entry.child, BT_ORPHAN_ITEM };
		error = bt_tx_edit(transaction, &transaction->roots, orphan, NULL, 0, BT_INSERT);
		error = error == BTRFS_EXISTS ? BTRFS_OK : error;
	}
	if (error == BTRFS_OK && transaction->has_uuids) {
		error = bt_sv_uuid_remove(transaction, dead.item.uuid, BT_UUID_SUBVOL, entry.child);
	}
	if (error == BTRFS_OK && transaction->has_uuids &&
	    !bt_equal(dead.item.received_uuid, empty, BTRFS_UUID_SIZE)) {
		error = bt_sv_uuid_remove(
		    transaction, dead.item.received_uuid, BT_UUID_RECEIVED_SUBVOL, entry.child);
	}
	if (error == BTRFS_OK) {
		transaction->changed = 1;
	}
	return bt_ns_poison(transaction, error);
}
