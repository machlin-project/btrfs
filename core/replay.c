/* SPDX-License-Identifier: BSD-3-Clause */
/* Tree-log replay, as Linux's btrfs_recover_log_trees does when it mounts a
 * filesystem whose last fsync was not followed by a commit. One transaction
 * reads the log through a separate view and replays it in Linux's passes:
 * inode items (with the xattrs and directory entries the log deleted), then
 * directory index items, then everything else, after which link counts are
 * recomputed and inodes without names get orphan items. Log blocks and the
 * data extents the log references stay out of allocation throughout. */
#include "namespace.h"

/* Subvolume logs one replay handles. */
#define BT_REPLAY_TREES 256U
/* Inodes whose link counts one replay recomputes. */
#define BT_REPLAY_FIXUPS UINT64_C(1048576)
/* Nodes the log trees of one replay hold. */
#define BT_REPLAY_NODES UINT64_C(1048576)

enum bt_replay_stage { BT_REPLAY_INODES, BT_REPLAY_NAMES, BT_REPLAY_ALL };

struct bt_replay_fixup {
	size_t log;
	uint64_t inode;
};

struct bt_replay {
	struct btrfs_transaction *transaction;
	/* Reads the log trees: one generation beyond the base, outside the
	 * shared node cache. */
	struct btrfs_fs view;
	struct bt_root logs[BT_REPLAY_TREES];
	uint64_t subvolumes[BT_REPLAY_TREES];
	size_t log_count;
	struct bt_replay_fixup *fixups;
	size_t fixup_count;
	size_t fixup_capacity;
	/* The log item being replayed, a second log item looked up while it
	 * is, and a subvolume item kept across edits. */
	uint8_t *item;
	uint8_t *other;
	uint8_t *kept;
	/* One node per level for walking a log tree. */
	uint8_t *nodes;
	/* An inode whose log item has no link: its items are not replayed. */
	uint64_t ignored;
	struct btrfs_time now;
	struct btrfs_replay_report *report;
};

static const struct btrfs_fs *
bt_rp_fs(const struct bt_replay *rp)
{
	return rp->transaction->base;
}

static uint64_t
bt_rp_transid(const struct bt_replay *rp)
{
	return bt_rp_fs(rp)->info.generation + 1;
}

/* Looks up the log item of key exactly, into buffer. */
static enum btrfs_result
bt_rp_log_find(
    struct bt_replay *rp, size_t log, struct bt_key key, uint8_t *buffer, size_t *size, int *found)
{
	struct bt_cursor cursor;
	struct bt_record record;
	enum btrfs_result error;

	*found = 0;
	bt_cursor_init(&cursor, &rp->view, rp->logs[log]);
	error = bt_cursor_seek(&cursor, key, 0);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (bt_key_compare(record.key, key) == 0) {
			bt_copy(buffer, record.data, record.size);
			*size = record.size;
			*found = 1;
		}
	} else if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_OK;
	}
	bt_cursor_fini(&cursor);
	return error;
}

/* A subvolume item into buffer; found is 0 when the key is absent. */
static enum btrfs_result
bt_rp_find(struct bt_replay *rp, const struct bt_owned_root *tree, struct bt_key key,
    uint8_t *buffer, size_t *size, int *found)
{
	enum btrfs_result error;

	error = bt_mutation_find(
	    rp->transaction->mutation, tree->root, key, buffer, bt_rp_fs(rp)->info.node_size, size);
	*found = error == BTRFS_OK;
	if (error == BTRFS_NOT_FOUND) {
		return BTRFS_OK;
	}
	return error == BTRFS_RANGE ? BTRFS_CORRUPT : error;
}

/* Stores an inode item as given. While Linux replays a log, its inode
 * updates leave the subvolume root's change times alone. */
static enum btrfs_result
bt_rp_put_inode(struct bt_replay *rp, struct bt_owned_root *tree, uint64_t inode,
    const struct bt_disk_inode *item, int insert)
{
	struct bt_key key = { .objectid = inode, .type = BT_INODE_ITEM };
	enum btrfs_result error;

	error = bt_tx_edit(rp->transaction, &tree->root, key, item, sizeof(*item),
	    insert ? BT_INSERT : BT_REPLACE);
	if (error == BTRFS_OK) {
		rp->transaction->changed = 1;
	}
	return error;
}

/* btrfs_update_inode: the replaying transaction's id, every other field as
 * given. */
static enum btrfs_result
bt_rp_update_inode(
    struct bt_replay *rp, struct bt_owned_root *tree, uint64_t inode, struct bt_disk_inode *item)
{
	bt_put64(&item->transid, bt_rp_transid(rp));
	return bt_rp_put_inode(rp, tree, inode, item, 0);
}

/* The inode an existing directory entry or back reference names must exist. */
static enum btrfs_result
bt_rp_inode(struct bt_replay *rp, const struct bt_owned_root *tree, uint64_t inode,
    struct bt_disk_inode *item)
{
	enum btrfs_result error;

	error = bt_ns_inode(rp->transaction, tree, inode, item);
	return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
}

/* A change Linux makes with inode_inc_iversion and, when timed, sets the
 * change time (and with modified the modification time) to the current time. */
static enum btrfs_result
bt_rp_change_inode(struct bt_replay *rp, struct bt_owned_root *tree, uint64_t inode,
    struct bt_disk_inode *item, int timed, int modified)
{
	bt_put64(&item->sequence, bt_u64(item->sequence) + 1);
	if (timed) {
		bt_put64(&item->ctime.seconds, (uint64_t)rp->now.seconds);
		bt_put32(&item->ctime.nanoseconds, rp->now.nanoseconds);
	}
	if (timed && modified) {
		item->mtime = item->ctime;
	}
	return bt_rp_update_inode(rp, tree, inode, item);
}

/* A directory gains or loses a name: its size stays twice the sum of its
 * name lengths. btrfs_add_link leaves its times to the log during replay;
 * __btrfs_unlink_inode sets them to the current time. */
static enum btrfs_result
bt_rp_resize_directory(
    struct bt_replay *rp, struct bt_owned_root *tree, uint64_t directory, size_t length, int add)
{
	struct bt_disk_inode item;
	uint64_t change = 2 * (uint64_t)length;
	uint64_t size;
	enum btrfs_result error;

	error = bt_rp_inode(rp, tree, directory, &item);
	if (error != BTRFS_OK) {
		return error;
	}
	size = bt_u64(item.size);
	if (!bt_ns_is_directory(&item) || (add ? size > UINT64_MAX - change : size < change)) {
		return BTRFS_CORRUPT;
	}
	bt_put64(&item.size, add ? size + change : size - change);
	return bt_rp_change_inode(rp, tree, directory, &item, !add, 1);
}

/* Records an inode whose link count is recomputed after the replay of log. */
static enum btrfs_result
bt_rp_fixup(struct bt_replay *rp, size_t log, uint64_t inode)
{
	const struct btrfs_environment *env = &bt_rp_fs(rp)->env;
	struct bt_replay_fixup *grown;
	size_t capacity;
	size_t i;

	for (i = 0; i < rp->fixup_count; i++) {
		if (rp->fixups[i].log == log && rp->fixups[i].inode == inode) {
			return BTRFS_OK;
		}
	}
	if (rp->fixup_count == BT_REPLAY_FIXUPS) {
		return BTRFS_UNSUPPORTED;
	}
	if (rp->fixup_count == rp->fixup_capacity) {
		capacity = rp->fixup_capacity == 0 ? 64 : rp->fixup_capacity * 2;
		grown = env->allocate(env->context, capacity * sizeof(*grown));
		if (grown == NULL) {
			return BTRFS_NO_MEMORY;
		}
		if (rp->fixups != NULL) {
			bt_copy(grown, rp->fixups, rp->fixup_count * sizeof(*grown));
			env->release(
			    env->context, rp->fixups, rp->fixup_capacity * sizeof(*rp->fixups));
		}
		rp->fixups = grown;
		rp->fixup_capacity = capacity;
	}
	rp->fixups[rp->fixup_count++] = (struct bt_replay_fixup){ log, inode };
	return BTRFS_OK;
}

static uint32_t
bt_rp_count(const uint8_t *node)
{
	const struct bt_disk_header *header = (const void *)node;

	return bt_u32(header->count);
}

/* Item slot of a leaf bt_tree_read validated. */
static void
bt_rp_record(const uint8_t *leaf, uint32_t slot, struct bt_record *record)
{
	const uint8_t *body = leaf + sizeof(struct bt_disk_header);
	const struct bt_disk_item *item = (const struct bt_disk_item *)body + slot;

	record->key = bt_key_decode(&item->key);
	record->data = body + bt_u32(item->offset);
	record->size = bt_u32(item->size);
}

/* A log's file extent item, validated as the reader does. */
static enum btrfs_result
bt_rp_extent(const uint8_t *data, size_t size, struct bt_disk_extent *extent, int *inline_item)
{
	const struct bt_disk_extent_header *header = (const void *)data;

	if (size < sizeof(*header)) {
		return BTRFS_CORRUPT;
	}
	bt_zero(extent, sizeof(*extent));
	*inline_item = header->type == BT_EXTENT_INLINE;
	if (*inline_item) {
		bt_copy(&extent->header, header, sizeof(*header));
		return bt_u64(header->ram_bytes) == 0 ||
			bt_u64(header->ram_bytes) > BT_MAX_NODE_SIZE
		    ? BTRFS_CORRUPT
		    : BTRFS_OK;
	}
	if (size != sizeof(*extent) ||
	    (header->type != BT_EXTENT_REGULAR && header->type != BT_EXTENT_PREALLOC)) {
		return BTRFS_CORRUPT;
	}
	bt_copy(extent, data, sizeof(*extent));
	if (bt_u64(extent->length) == 0 ||
	    (bt_u64(extent->disk_bytenr) != 0 &&
		(bt_u64(extent->disk_bytes) == 0 ||
		    bt_u64(extent->disk_bytenr) > UINT64_MAX - bt_u64(extent->disk_bytes)))) {
		return BTRFS_CORRUPT;
	}
	return BTRFS_OK;
}

/* Every block of a log tree, and every data extent its leaves reference,
 * leaves allocation: none is in the extent tree, which reports their space
 * free. */
static enum btrfs_result
bt_rp_withhold(struct bt_replay *rp, struct bt_root root, uint64_t *visited)
{
	const struct btrfs_fs *fs = &rp->view;
	const struct bt_disk_pointer *pointers;
	struct bt_disk_extent extent;
	struct bt_record record;
	struct bt_root path[BT_MAX_LEVEL];
	uint32_t slots[BT_MAX_LEVEL];
	uint8_t *node;
	uint32_t count;
	uint32_t i;
	unsigned level = root.level;
	int inline_item;
	enum btrfs_result error;

	path[level] = root;
	slots[level] = 0;
	error = bt_tree_read(fs, root, rp->nodes + (size_t)level * fs->info.node_size);
	while (error == BTRFS_OK) {
		node = rp->nodes + (size_t)level * fs->info.node_size;
		count = bt_rp_count(node);
		if (slots[level] == 0) {
			if (++*visited > BT_REPLAY_NODES) {
				return BTRFS_UNSUPPORTED;
			}
			error = bt_space_withhold(
			    rp->transaction->space, path[level].address, fs->info.node_size);
			for (i = 0; error == BTRFS_OK && level == 0 && i < count; i++) {
				bt_rp_record(node, i, &record);
				if (record.key.type != BT_EXTENT_DATA) {
					continue;
				}
				error =
				    bt_rp_extent(record.data, record.size, &extent, &inline_item);
				if (error == BTRFS_OK && !inline_item &&
				    bt_u64(extent.disk_bytenr) != 0) {
					error = bt_space_withhold(rp->transaction->space,
					    bt_u64(extent.disk_bytenr), bt_u64(extent.disk_bytes));
				}
			}
		}
		if (error != BTRFS_OK) {
			break;
		}
		if (level != 0 && slots[level] < count) {
			pointers = (const void *)(node + sizeof(struct bt_disk_header));
			path[level - 1] =
			    (struct bt_root){ .address = bt_u64(pointers[slots[level]].bytenr),
				    .generation = bt_u64(pointers[slots[level]].generation),
				    .owner = BT_TREE_LOG_OBJECTID,
				    .level = (uint8_t)(level - 1) };
			slots[level]++;
			level--;
			slots[level] = 0;
			error = bt_tree_read(
			    fs, path[level], rp->nodes + (size_t)level * fs->info.node_size);
			continue;
		}
		/* Done with this node: back to its parent's next pointer. */
		if (level == root.level) {
			break;
		}
		level++;
	}
	return error;
}

static int
bt_rp_regular(const struct bt_disk_inode *item)
{
	return (bt_u32(item->mode) & BTRFS_MODE_TYPE) == BTRFS_MODE_REGULAR;
}

/* Copies a log item other than an inode item into the subvolume, as
 * overwrite_item does: an equal item stays, any other is replaced. */
static enum btrfs_result
bt_rp_overwrite(struct bt_replay *rp, struct bt_owned_root *tree, struct bt_key key,
    const uint8_t *data, size_t size)
{
	size_t existing;
	int found;
	enum btrfs_result error;

	error = bt_rp_find(rp, tree, key, rp->kept, &existing, &found);
	if (error != BTRFS_OK || (found && existing == size && bt_equal(rp->kept, data, size))) {
		return error;
	}
	error = bt_tx_edit(
	    rp->transaction, &tree->root, key, data, size, found ? BT_REPLACE : BT_INSERT);
	if (error == BTRFS_OK) {
		rp->transaction->changed = 1;
	}
	return error;
}

/* overwrite_item for an inode item: the byte count stays the subvolume's (the
 * extents replayed later adjust it), and an existing directory keeps its size,
 * which the names replayed later adjust; an inode logged with generation 0
 * only to exist changes no more than a regular file's size. */
static enum btrfs_result
bt_rp_overwrite_inode(struct bt_replay *rp, struct bt_owned_root *tree, uint64_t inode,
    const struct bt_disk_inode *logged)
{
	struct bt_disk_inode existing;
	struct bt_disk_inode item = *logged;
	int found;
	enum btrfs_result error;

	error = bt_ns_inode(rp->transaction, tree, inode, &existing);
	found = error == BTRFS_OK;
	if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_OK;
	}
	if (error != BTRFS_OK || (found && bt_equal(&existing, logged, sizeof(existing)))) {
		return error;
	}
	if (found && bt_u64(logged->generation) == 0) {
		if (bt_rp_regular(logged) && bt_rp_regular(&existing) &&
		    bt_u64(logged->size) != 0) {
			bt_put64(&existing.size, bt_u64(logged->size));
			return bt_rp_put_inode(rp, tree, inode, &existing, 0);
		}
		return BTRFS_OK;
	}
	bt_put64(&item.nbytes, found ? bt_u64(existing.nbytes) : 0);
	if (bt_ns_is_directory(&item)) {
		bt_put64(
		    &item.size, found && bt_ns_is_directory(&existing) ? bt_u64(existing.size) : 0);
	}
	if (bt_u64(item.generation) == 0) {
		bt_put64(&item.generation, bt_rp_transid(rp));
	}
	return bt_rp_put_inode(rp, tree, inode, &item, !found);
}

/* Before its extents are replayed, a regular file loses those beyond its
 * logged size: preallocations past EOF are logged again after it. */
static enum btrfs_result
bt_rp_truncate(struct bt_replay *rp, struct bt_owned_root *tree, uint64_t inode)
{
	struct bt_disk_inode item;
	uint64_t sector = bt_rp_fs(rp)->info.sector_size;
	uint64_t size;
	uint64_t removed = 0;
	enum btrfs_result error;

	error = bt_rp_inode(rp, tree, inode, &item);
	size = bt_u64(item.size);
	if (error == BTRFS_OK && size > UINT64_MAX - sector) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		error = bt_tx_drop_range(rp->transaction, tree, inode,
		    size + (sector - size % sector) % sector, UINT64_MAX, &removed);
	}
	if (error == BTRFS_OK && bt_u64(item.nbytes) < removed) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		bt_put64(&item.nbytes, bt_u64(item.nbytes) - removed);
		error = bt_rp_update_inode(rp, tree, inode, &item);
	}
	return error;
}

/* btrfs_unlink_inode: the DIR_ITEM entry, the back reference giving the
 * index, the DIR_INDEX item; the directory's size and times; one link less,
 * unless Linux raised the count first (keep). An inode reaching no link
 * stays until the link counts are recomputed. */
static enum btrfs_result
bt_rp_unlink(struct bt_replay *rp, struct bt_owned_root *tree, uint64_t directory, uint64_t inode,
    const uint8_t *name, size_t length, int keep)
{
	struct btrfs_transaction *transaction = rp->transaction;
	const struct bt_disk_dir *header;
	struct bt_disk_inode item;
	struct bt_packed packed;
	struct bt_key key = {
		.objectid = directory, .type = BT_DIR_ITEM, .offset = bt_ns_hash(name, length)
	};
	uint64_t index = 0;
	size_t offset = 0;
	size_t entry_size = 0;
	uint32_t links;
	enum btrfs_result error;

	error = bt_ns_load(transaction, tree, key, &packed);
	if (error == BTRFS_OK) {
		error = bt_ns_dir_find(&packed, name, length, &offset, &entry_size, &header);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_cut(transaction, tree, &packed, offset, entry_size);
	}
	if (error == BTRFS_OK) {
		key =
		    (struct bt_key){ .objectid = inode, .type = BT_INODE_REF, .offset = directory };
		error = bt_ns_load(transaction, tree, key, &packed);
		if (error == BTRFS_OK) {
			error = bt_ns_ref_find(&packed, name, length, &offset, &index);
		}
		if (error == BTRFS_OK) {
			error = bt_ns_cut(transaction, tree, &packed, offset,
			    sizeof(struct bt_disk_inode_ref) + length);
		} else if (error == BTRFS_NOT_FOUND) {
			key = (struct bt_key){ .objectid = inode,
				.type = BT_INODE_EXTREF,
				.offset = bt_ns_extref_hash(directory, name, length) };
			error = bt_ns_load(transaction, tree, key, &packed);
			if (error == BTRFS_OK) {
				error = bt_ns_extref_find(
				    &packed, directory, name, length, &offset, &index);
			}
			if (error == BTRFS_OK) {
				error = bt_ns_cut(transaction, tree, &packed, offset,
				    sizeof(struct bt_disk_inode_extref) + length);
			}
		}
	}
	if (error == BTRFS_OK) {
		key =
		    (struct bt_key){ .objectid = directory, .type = BT_DIR_INDEX, .offset = index };
		error = bt_tx_edit(transaction, &tree->root, key, NULL, 0, BT_DELETE);
	}
	if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		error = bt_rp_resize_directory(rp, tree, directory, length, 0);
	}
	if (error == BTRFS_OK) {
		error = bt_rp_inode(rp, tree, inode, &item);
	}
	if (error == BTRFS_OK) {
		links = bt_u32(item.links);
		if (!keep && links != 0) {
			bt_put32(&item.links, links - 1);
		}
		error = bt_rp_change_inode(rp, tree, inode, &item, 1, 0);
	}
	if (error == BTRFS_OK) {
		rp->report->unlinked++;
	}
	return error;
}

/* btrfs_add_link with a given index: the back reference when asked, the
 * DIR_ITEM and DIR_INDEX entries, then the directory's size and times. A name
 * already in the directory or the back references is EEXIST to Linux:
 * EXISTS, with nothing changed. */
static enum btrfs_result
bt_rp_add_link(struct bt_replay *rp, struct bt_owned_root *tree, uint64_t directory, uint64_t inode,
    const uint8_t *name, size_t length, int backref, uint64_t index)
{
	struct btrfs_transaction *transaction = rp->transaction;
	const struct bt_disk_dir *header;
	struct bt_disk_inode item;
	struct bt_packed packed;
	struct bt_key key = {
		.objectid = directory, .type = BT_DIR_ITEM, .offset = bt_ns_hash(name, length)
	};
	struct bt_key location = { .objectid = inode, .type = BT_INODE_ITEM };
	uint64_t existing;
	size_t offset;
	size_t entry_size;
	enum btrfs_result error;

	error = bt_rp_inode(rp, tree, inode, &item);
	if (error == BTRFS_OK) {
		error = bt_ns_load(transaction, tree, key, &packed);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_dir_find(&packed, name, length, &offset, &entry_size, &header);
		error = error == BTRFS_OK ? BTRFS_EXISTS : error;
	}
	if (error == BTRFS_NOT_FOUND && backref) {
		key =
		    (struct bt_key){ .objectid = inode, .type = BT_INODE_REF, .offset = directory };
		error = bt_ns_load(transaction, tree, key, &packed);
		if (error == BTRFS_OK) {
			error = bt_ns_ref_find(&packed, name, length, &offset, &existing);
		}
		if (error == BTRFS_NOT_FOUND) {
			key = (struct bt_key){ .objectid = inode,
				.type = BT_INODE_EXTREF,
				.offset = bt_ns_extref_hash(directory, name, length) };
			error = bt_ns_load(transaction, tree, key, &packed);
			if (error == BTRFS_OK) {
				error = bt_ns_extref_find(
				    &packed, directory, name, length, &offset, &existing);
			}
		}
		error = error == BTRFS_OK ? BTRFS_EXISTS : error;
		if (error == BTRFS_NOT_FOUND) {
			error =
			    bt_ns_add_ref(transaction, tree, directory, name, length, inode, index);
		}
	} else if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_OK;
	}
	if (error == BTRFS_OK) {
		/* Another name at this index is a conflict Linux cannot insert either. */
		key =
		    (struct bt_key){ .objectid = directory, .type = BT_DIR_INDEX, .offset = index };
		error = bt_ns_load(transaction, tree, key, &packed);
		if (error == BTRFS_OK && packed.present) {
			error = BTRFS_CORRUPT;
		}
	}
	if (error == BTRFS_OK) {
		error = bt_ns_insert_entry(transaction, tree, directory, name, length, location,
		    bt_ns_type(bt_u32(item.mode)), index);
	}
	if (error == BTRFS_OK) {
		error = bt_rp_resize_directory(rp, tree, directory, length, 1);
	}
	if (error == BTRFS_OK) {
		rp->report->names++;
	}
	return error;
}

/* drop_one_dir_item: an entry of the subvolume conflicting with the log goes,
 * and the inode it named has its link count recomputed; keep as for
 * bt_rp_unlink. */
static enum btrfs_result
bt_rp_drop_entry(struct bt_replay *rp, struct bt_owned_root *tree, size_t log, uint64_t directory,
    struct bt_key location, const uint8_t *name, size_t length, int keep)
{
	struct bt_disk_inode item;
	enum btrfs_result error;

	/* A subvolume's entry is never logged: Linux commits instead. */
	if (location.type != BT_INODE_ITEM) {
		return BTRFS_UNSUPPORTED;
	}
	error = bt_rp_inode(rp, tree, location.objectid, &item);
	if (error == BTRFS_OK) {
		error = bt_rp_fixup(rp, log, location.objectid);
	}
	return error == BTRFS_OK
	    ? bt_rp_unlink(rp, tree, directory, location.objectid, name, length, keep)
	    : error;
}

/* The single entry of a DIR_INDEX item: its location, type and name. */
static enum btrfs_result
bt_rp_index_entry(struct bt_key key, const uint8_t *data, size_t size, struct bt_key *location,
    uint8_t *type, uint8_t *name, size_t *length)
{
	struct bt_record record = { key, data, size };
	const struct bt_disk_dir *header;
	const uint8_t *entry_name;
	const uint8_t *value;
	size_t position = 0;
	enum btrfs_result error;

	error = bt_dir_record(&record, &position, &header, &entry_name, &value);
	if (error == BTRFS_NOT_FOUND || (error == BTRFS_OK && position != size)) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		*location = bt_key_decode(&header->location);
		*type = header->type;
		*length = bt_u16(header->name_length);
		bt_copy(name, entry_name, *length);
	}
	return error;
}

/* check_item_in_log: an index entry of a range the log owns stays when the
 * log has it, and is unlinked otherwise; with all, every entry goes. */
static enum btrfs_result
bt_rp_check_in_log(struct bt_replay *rp, struct bt_owned_root *tree, size_t log, int all,
    uint64_t directory, uint64_t index)
{
	struct bt_packed packed;
	struct bt_key key = { .objectid = directory, .type = BT_DIR_INDEX, .offset = index };
	struct bt_key location = { 0 };
	struct bt_key logged_location;
	uint8_t name[BTRFS_NAME_MAX];
	uint8_t logged_name[BTRFS_NAME_MAX];
	size_t length = 0;
	size_t logged_length;
	size_t size = 0;
	uint8_t type;
	int found = 0;
	enum btrfs_result error;

	error = bt_ns_load(rp->transaction, tree, key, &packed);
	if (error == BTRFS_OK && !packed.present) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		error = bt_rp_index_entry(
		    key, packed.bytes, packed.size, &location, &type, name, &length);
	}
	if (error == BTRFS_OK && !all) {
		error = bt_rp_log_find(rp, log, key, rp->other, &size, &found);
	}
	if (error == BTRFS_OK && found) {
		error = bt_rp_index_entry(
		    key, rp->other, size, &logged_location, &type, logged_name, &logged_length);
		if (error == BTRFS_OK && logged_length == length &&
		    bt_equal(logged_name, name, length)) {
			return BTRFS_OK;
		}
	}
	return error == BTRFS_OK
	    ? bt_rp_drop_entry(rp, tree, log, directory, location, name, length, 1)
	    : error;
}

/* Index entries from first to last of directory, through check_item_in_log. */
static enum btrfs_result
bt_rp_delete_range(struct bt_replay *rp, struct bt_owned_root *tree, size_t log, int all,
    uint64_t directory, uint64_t first, uint64_t last)
{
	struct bt_key key = { .objectid = directory, .type = BT_DIR_INDEX, .offset = first };
	struct bt_key found_key;
	uint64_t steps;
	int found;
	enum btrfs_result error = BTRFS_OK;

	for (steps = 0; error == BTRFS_OK; steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			return BTRFS_UNSUPPORTED;
		}
		error = bt_ns_neighbor(rp->transaction, tree, key, 0, &found_key, &found);
		if (error != BTRFS_OK || !found || found_key.objectid != directory ||
		    found_key.type != BT_DIR_INDEX || found_key.offset > last) {
			break;
		}
		error = bt_rp_check_in_log(rp, tree, log, all, directory, found_key.offset);
		if (found_key.offset == UINT64_MAX) {
			break;
		}
		key.offset = found_key.offset + 1;
	}
	return error;
}

/* replay_dir_deletes: in each index range the log owns for directory (with
 * all, the whole directory), the entries the log lacks are unlinked. */
static enum btrfs_result
bt_rp_dir_deletes(
    struct bt_replay *rp, struct bt_owned_root *tree, size_t log, int all, uint64_t directory)
{
	const struct bt_disk_dir_log *range;
	struct bt_disk_inode item;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = directory, .type = BT_DIR_LOG_INDEX };
	uint64_t end;
	enum btrfs_result error;

	error = bt_ns_inode(rp->transaction, tree, directory, &item);
	if (error != BTRFS_OK) {
		/* The directory may come only from the log. */
		return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
	}
	if (all) {
		return bt_rp_delete_range(rp, tree, log, 1, directory, 0, UINT64_MAX);
	}
	bt_cursor_init(&cursor, &rp->view, rp->logs[log]);
	error = bt_cursor_seek(&cursor, key, 0);
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != directory || record.key.type != BT_DIR_LOG_INDEX) {
			break;
		}
		if (record.size != sizeof(*range)) {
			error = BTRFS_CORRUPT;
			break;
		}
		range = (const void *)record.data;
		end = bt_u64(range->end);
		if (end < record.key.offset) {
			error = BTRFS_CORRUPT;
			break;
		}
		error = bt_rp_delete_range(rp, tree, log, 0, directory, record.key.offset, end);
		if (error == BTRFS_OK) {
			error = bt_cursor_next(&cursor);
		}
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* replay_xattr_deletes: the subvolume's xattrs of inode that the log lacks. */
static enum btrfs_result
bt_rp_xattr_deletes(struct bt_replay *rp, struct bt_owned_root *tree, size_t log, uint64_t inode)
{
	const struct bt_disk_dir *header;
	const struct bt_disk_dir *other;
	const uint8_t *name;
	const uint8_t *value;
	struct bt_packed packed;
	struct bt_packed logged;
	struct bt_record record;
	struct bt_key key = { .objectid = inode, .type = BT_XATTR_ITEM };
	struct bt_key found_key;
	size_t position;
	size_t size = 0;
	size_t logged_size = 0;
	size_t length;
	size_t offset;
	size_t entry_size;
	uint64_t steps;
	int found;
	int present = 0;
	int deleted;
	enum btrfs_result error = BTRFS_OK;

	for (steps = 0; error == BTRFS_OK; steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			return BTRFS_UNSUPPORTED;
		}
		error = bt_ns_neighbor(rp->transaction, tree, key, 0, &found_key, &found);
		if (error != BTRFS_OK || !found || found_key.objectid != inode ||
		    found_key.type != BT_XATTR_ITEM) {
			break;
		}
		error = bt_rp_find(rp, tree, found_key, rp->kept, &size, &found);
		if (error == BTRFS_OK) {
			error =
			    bt_rp_log_find(rp, log, found_key, rp->other, &logged_size, &present);
		}
		logged =
		    (struct bt_packed){ found_key, rp->other, present ? logged_size : 0, present };
		record = (struct bt_record){ found_key, rp->kept, size };
		position = 0;
		deleted = 0;
		while (error == BTRFS_OK && !deleted) {
			error = bt_dir_record(&record, &position, &header, &name, &value);
			if (error != BTRFS_OK) {
				break;
			}
			length = bt_u16(header->name_length);
			error = bt_ns_dir_find(&logged, name, length, &offset, &entry_size, &other);
			if (error == BTRFS_NOT_FOUND) {
				error = bt_ns_load(rp->transaction, tree, found_key, &packed);
				if (error == BTRFS_OK) {
					error = bt_ns_dir_find(
					    &packed, name, length, &offset, &entry_size, &other);
				}
				if (error == BTRFS_OK) {
					error = bt_ns_cut(
					    rp->transaction, tree, &packed, offset, entry_size);
				}
				deleted = 1;
			}
		}
		if (error == BTRFS_NOT_FOUND) {
			error = BTRFS_OK;
		}
		if (error == BTRFS_OK && !deleted) {
			if (found_key.offset == UINT64_MAX) {
				break;
			}
			key.offset = found_key.offset + 1;
		}
	}
	return error;
}

/* backref_in_log: whether the log's back reference item of key holds name
 * (and, for INODE_EXTREF, parent). */
static enum btrfs_result
bt_rp_backref_in_log(struct bt_replay *rp, size_t log, struct bt_key key, uint64_t parent,
    const uint8_t *name, size_t length, int *found)
{
	struct bt_packed packed;
	uint64_t index;
	size_t offset;
	size_t size;
	int present;
	enum btrfs_result error;

	*found = 0;
	error = bt_rp_log_find(rp, log, key, rp->other, &size, &present);
	if (error != BTRFS_OK || !present) {
		return error;
	}
	packed = (struct bt_packed){ key, rp->other, size, 1 };
	error = key.type == BT_INODE_EXTREF
	    ? bt_ns_extref_find(&packed, parent, name, length, &offset, &index)
	    : bt_ns_ref_find(&packed, name, length, &offset, &index);
	*found = error == BTRFS_OK;
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* An inode item of the log, in the first pass: the xattrs and (for a
 * directory) the entries the log deleted go, the item is copied, a regular
 * file loses extents beyond its size, and its links are recomputed later. */
static BT_NOINLINE enum btrfs_result
bt_rp_inode_item(struct bt_replay *rp, struct bt_owned_root *tree, size_t log, struct bt_key key,
    const uint8_t *data, size_t size)
{
	struct bt_disk_inode logged;
	enum btrfs_result error;

	if (size != sizeof(logged) || key.offset != 0) {
		return BTRFS_CORRUPT;
	}
	bt_copy(&logged, data, sizeof(logged));
	error = bt_rp_xattr_deletes(rp, tree, log, key.objectid);
	if (error == BTRFS_OK && bt_ns_is_directory(&logged)) {
		error = bt_rp_dir_deletes(rp, tree, log, 0, key.objectid);
	}
	if (error == BTRFS_OK) {
		error = bt_rp_overwrite_inode(rp, tree, key.objectid, &logged);
	}
	if (error == BTRFS_OK && bt_rp_regular(&logged)) {
		error = bt_rp_truncate(rp, tree, key.objectid);
	}
	if (error == BTRFS_OK) {
		error = bt_rp_fixup(rp, log, key.objectid);
	}
	if (error == BTRFS_OK) {
		rp->report->inodes++;
	}
	return error;
}

/* A subvolume entry for name in directory (DIR_ITEM, or the DIR_INDEX item
 * at index when that holds name): whether it matches the logged location and
 * type; a conflicting one goes when the logged inode exists. */
static enum btrfs_result
bt_rp_conflict(struct bt_replay *rp, struct bt_owned_root *tree, size_t log, struct bt_key key,
    const uint8_t *name, size_t length, struct bt_key location, uint8_t type, int exists,
    int *matches)
{
	const struct bt_disk_dir *header = NULL;
	struct bt_packed packed;
	struct bt_key found;
	size_t offset;
	size_t entry_size;
	enum btrfs_result error;

	*matches = 0;
	error = bt_ns_load(rp->transaction, tree, key, &packed);
	if (error == BTRFS_OK) {
		error = bt_ns_dir_find(&packed, name, length, &offset, &entry_size, &header);
	}
	if (error == BTRFS_NOT_FOUND) {
		return BTRFS_OK;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	found = bt_key_decode(&header->location);
	if (bt_key_compare(found, location) == 0 && header->type == type) {
		*matches = 1;
		return BTRFS_OK;
	}
	return exists ? bt_rp_drop_entry(rp, tree, log, key.objectid, found, name, length, 0)
		      : BTRFS_OK;
}

/* replay_one_dir_item: a DIR_INDEX entry of the log. Conflicting entries go;
 * a name whose back reference is in the log waits for it; otherwise the name
 * is added when its inode exists, and a non-directory has its links
 * recomputed. */
static BT_NOINLINE enum btrfs_result
bt_rp_dir_index(struct bt_replay *rp, struct bt_owned_root *tree, size_t log, struct bt_key key,
    const uint8_t *data, size_t size)
{
	struct bt_disk_inode item;
	struct bt_key location;
	struct bt_key search;
	uint8_t name[BTRFS_NAME_MAX];
	size_t length;
	uint8_t type;
	int exists = 0;
	int dir_matches = 0;
	int index_matches = 0;
	int found = 0;
	enum btrfs_result error;

	error = bt_rp_index_entry(key, data, size, &location, &type, name, &length);
	if (error == BTRFS_OK && location.type != BT_INODE_ITEM) {
		error = BTRFS_UNSUPPORTED;
	}
	if (error == BTRFS_OK) {
		error = bt_rp_inode(rp, tree, key.objectid, &item);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_inode(rp->transaction, tree, location.objectid, &item);
		exists = error == BTRFS_OK;
		error = error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
	}
	if (error == BTRFS_OK) {
		search = (struct bt_key){ .objectid = key.objectid,
			.type = BT_DIR_ITEM,
			.offset = bt_ns_hash(name, length) };
		error = bt_rp_conflict(
		    rp, tree, log, search, name, length, location, type, exists, &dir_matches);
	}
	if (error == BTRFS_OK) {
		error = bt_rp_conflict(
		    rp, tree, log, key, name, length, location, type, exists, &index_matches);
	}
	if (error != BTRFS_OK || (dir_matches && index_matches)) {
		return error;
	}
	search = (struct bt_key){
		.objectid = location.objectid, .type = BT_INODE_REF, .offset = key.objectid
	};
	error = bt_rp_backref_in_log(rp, log, search, 0, name, length, &found);
	if (error == BTRFS_OK && !found) {
		/* Linux looks for the extended reference at the directory's number as
		 * the key offset, not the name's hash. */
		search.type = BT_INODE_EXTREF;
		error = bt_rp_backref_in_log(rp, log, search, key.objectid, name, length, &found);
	}
	if (error != BTRFS_OK || found || !exists) {
		return error;
	}
	error =
	    bt_rp_add_link(rp, tree, key.objectid, location.objectid, name, length, 1, key.offset);
	if (error == BTRFS_EXISTS) {
		return BTRFS_OK;
	}
	return error == BTRFS_OK && type != BTRFS_FT_DIRECTORY
	    ? bt_rp_fixup(rp, log, location.objectid)
	    : error;
}

/* inode_in_dir: directory holds name at index and by name, both naming inode. */
static enum btrfs_result
bt_rp_inode_in_dir(struct bt_replay *rp, struct bt_owned_root *tree, uint64_t directory,
    uint64_t inode, uint64_t index, const uint8_t *name, size_t length, int *in)
{
	const struct bt_disk_dir *header = NULL;
	struct bt_packed packed;
	struct bt_key key = { .objectid = directory, .type = BT_DIR_INDEX, .offset = index };
	size_t offset;
	size_t entry_size;
	enum btrfs_result error;

	*in = 0;
	error = bt_ns_load(rp->transaction, tree, key, &packed);
	if (error == BTRFS_OK) {
		error = bt_ns_dir_find(&packed, name, length, &offset, &entry_size, &header);
	}
	if (error == BTRFS_OK && bt_u64(header->location.objectid) == inode) {
		key = (struct bt_key){ .objectid = directory,
			.type = BT_DIR_ITEM,
			.offset = bt_ns_hash(name, length) };
		error = bt_ns_load(rp->transaction, tree, key, &packed);
		if (error == BTRFS_OK) {
			error =
			    bt_ns_dir_find(&packed, name, length, &offset, &entry_size, &header);
		}
		*in = error == BTRFS_OK && bt_u64(header->location.objectid) == inode;
	}
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* One back reference entry: its parent, index and name, or NOT_FOUND past the
 * item's end. */
static enum btrfs_result
bt_rp_ref_entry(int extended, uint64_t parent, const uint8_t *data, size_t size, size_t *position,
    uint64_t *entry_parent, uint64_t *index, uint8_t *name, size_t *length)
{
	const struct bt_disk_inode_ref *ref;
	const struct bt_disk_inode_extref *extref;
	size_t header = extended ? sizeof(*extref) : sizeof(*ref);

	if (*position == size) {
		return BTRFS_NOT_FOUND;
	}
	if (*position > size || size - *position < header) {
		return BTRFS_CORRUPT;
	}
	if (extended) {
		extref = (const void *)(data + *position);
		*entry_parent = bt_u64(extref->parent);
		*index = bt_u64(extref->index);
		*length = bt_u16(extref->name_length);
	} else {
		ref = (const void *)(data + *position);
		*entry_parent = parent;
		*index = bt_u64(ref->index);
		*length = bt_u16(ref->name_length);
	}
	if (*length > size - *position - header ||
	    !bt_name_valid(data + *position + header, *length)) {
		return BTRFS_CORRUPT;
	}
	bt_copy(name, data + *position + header, *length);
	*position += header + *length;
	return BTRFS_OK;
}

/* Drops the entry for name in the subvolume item of key, when it holds one. */
static enum btrfs_result
bt_rp_drop_named(struct bt_replay *rp, struct bt_owned_root *tree, size_t log, struct bt_key key,
    const uint8_t *name, size_t length)
{
	const struct bt_disk_dir *header = NULL;
	struct bt_packed packed;
	struct bt_key location;
	size_t offset;
	size_t entry_size;
	enum btrfs_result error;

	error = bt_ns_load(rp->transaction, tree, key, &packed);
	if (error == BTRFS_OK) {
		error = bt_ns_dir_find(&packed, name, length, &offset, &entry_size, &header);
	}
	if (error != BTRFS_OK) {
		return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
	}
	location = bt_key_decode(&header->location);
	return bt_rp_drop_entry(rp, tree, log, key.objectid, location, name, length, 0);
}

/* __add_inode_ref: before a logged name is linked, the subvolume's other
 * names of inode in the same reference item that the log lacks are unlinked,
 * and an entry holding the logged index or name goes. *skip reports a back
 * reference of a subvolume's root directory, which is left as it is. */
static enum btrfs_result
bt_rp_ref_conflicts(struct bt_replay *rp, struct bt_owned_root *tree, size_t log,
    uint64_t directory, uint64_t inode, uint64_t index, const uint8_t *name, size_t length,
    int *skip)
{
	struct bt_packed packed;
	struct bt_key key;
	uint8_t victim[BTRFS_NAME_MAX];
	uint64_t entry_parent;
	uint64_t entry_index;
	uint64_t steps;
	size_t victim_length;
	size_t position;
	size_t offset;
	size_t size;
	int extended;
	int found;
	int in_log;
	int again = 1;
	enum btrfs_result error = BTRFS_OK;

	*skip = 0;
	for (steps = 0; error == BTRFS_OK && again; steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			return BTRFS_UNSUPPORTED;
		}
		again = 0;
		for (extended = 0; error == BTRFS_OK && !again && extended < 2; extended++) {
			key = extended ? (struct bt_key){ .objectid = inode,
				.type = BT_INODE_EXTREF,
				.offset = bt_ns_extref_hash(directory, name, length) }
				       : (struct bt_key){ .objectid = inode,
						 .type = BT_INODE_REF,
						 .offset = directory };
			error = bt_rp_find(rp, tree, key, rp->kept, &size, &found);
			if (error != BTRFS_OK || !found) {
				continue;
			}
			if (!extended && inode == directory) {
				*skip = 1;
				return BTRFS_OK;
			}
			packed = (struct bt_packed){ key, rp->kept, size, 1 };
			/* Linux examines an extended item only when it holds this name. */
			if (extended) {
				error = bt_ns_extref_find(
				    &packed, directory, name, length, &offset, &entry_index);
				if (error == BTRFS_NOT_FOUND) {
					error = BTRFS_OK;
					continue;
				}
			}
			position = 0;
			while (error == BTRFS_OK && !again) {
				error = bt_rp_ref_entry(extended, directory, rp->kept, size,
				    &position, &entry_parent, &entry_index, victim, &victim_length);
				if (error != BTRFS_OK || entry_parent != directory) {
					continue;
				}
				if (extended) {
					key.offset =
					    bt_ns_extref_hash(directory, victim, victim_length);
				}
				error = bt_rp_backref_in_log(
				    rp, log, key, directory, victim, victim_length, &in_log);
				if (error == BTRFS_OK && !in_log) {
					error = bt_rp_unlink(
					    rp, tree, directory, inode, victim, victim_length, 1);
					again = 1;
				}
			}
			if (error == BTRFS_NOT_FOUND) {
				error = BTRFS_OK;
			}
		}
	}
	if (error != BTRFS_OK) {
		return error;
	}
	/* A conflicting index, then a conflicting name. */
	key = (struct bt_key){ .objectid = directory, .type = BT_DIR_INDEX, .offset = index };
	error = bt_ns_load(rp->transaction, tree, key, &packed);
	if (error == BTRFS_OK && packed.present) {
		error = bt_rp_drop_named(rp, tree, log, key, name, length);
	}
	if (error == BTRFS_OK) {
		key = (struct bt_key){ .objectid = directory,
			.type = BT_DIR_ITEM,
			.offset = bt_ns_hash(name, length) };
		error = bt_rp_drop_named(rp, tree, log, key, name, length);
	}
	return error;
}

/* unlink_old_inode_refs: names of the subvolume's item of key that the logged
 * item lacks are unlinked before the logged item replaces it. A name whose
 * directory is missing stops the item (*skip): Linux then leaves it as it is,
 * for the link count fixup. */
static enum btrfs_result
bt_rp_unlink_old_refs(struct bt_replay *rp, struct bt_owned_root *tree, struct bt_key key,
    const uint8_t *data, size_t size, int *skip)
{
	struct bt_disk_inode directory;
	struct bt_packed logged = { key, (uint8_t *)data, size, 1 };
	uint8_t name[BTRFS_NAME_MAX];
	uint64_t parent;
	uint64_t index;
	uint64_t steps;
	size_t position;
	size_t length;
	size_t offset;
	size_t existing;
	int extended = key.type == BT_INODE_EXTREF;
	int found = 1;
	int again = 1;
	enum btrfs_result error = BTRFS_OK;

	*skip = 0;
	for (steps = 0; error == BTRFS_OK && again && found; steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			return BTRFS_UNSUPPORTED;
		}
		again = 0;
		error = bt_rp_find(rp, tree, key, rp->kept, &existing, &found);
		position = 0;
		while (error == BTRFS_OK && found && !again) {
			error = bt_rp_ref_entry(extended, key.offset, rp->kept, existing, &position,
			    &parent, &index, name, &length);
			if (error != BTRFS_OK) {
				break;
			}
			error = extended
			    ? bt_ns_extref_find(&logged, parent, name, length, &offset, &index)
			    : bt_ns_ref_find(&logged, name, length, &offset, &index);
			if (error == BTRFS_NOT_FOUND) {
				error = bt_ns_inode(rp->transaction, tree, parent, &directory);
				if (error == BTRFS_NOT_FOUND) {
					*skip = 1;
					return BTRFS_OK;
				}
				if (error == BTRFS_OK) {
					error = bt_rp_unlink(
					    rp, tree, parent, key.objectid, name, length, 0);
					again = 1;
				}
			}
		}
		if (error == BTRFS_NOT_FOUND) {
			error = BTRFS_OK;
			again = 0;
		}
	}
	return error;
}

/* add_inode_ref: each logged name of an inode back reference item is linked
 * unless its directory already holds it, after the conflicts it has go; then
 * the subvolume's other names in the item go and the logged item replaces
 * it. A missing directory skips the item. */
static BT_NOINLINE enum btrfs_result
bt_rp_inode_ref(struct bt_replay *rp, struct bt_owned_root *tree, size_t log, struct bt_key key,
    const uint8_t *data, size_t size)
{
	struct bt_disk_inode directory;
	struct bt_disk_inode item;
	uint8_t name[BTRFS_NAME_MAX];
	uint64_t parent;
	uint64_t index;
	size_t position = 0;
	size_t length;
	int extended = key.type == BT_INODE_EXTREF;
	int in = 0;
	int skip = 0;
	enum btrfs_result error = BTRFS_OK;

	if (size == 0) {
		return BTRFS_CORRUPT;
	}
	/* Linux looks up an INODE_REF item's directory before its inode. */
	if (!extended) {
		error = bt_ns_inode(rp->transaction, tree, key.offset, &directory);
		if (error != BTRFS_OK) {
			return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
		}
	}
	error = bt_rp_inode(rp, tree, key.objectid, &item);
	while (error == BTRFS_OK) {
		error = bt_rp_ref_entry(
		    extended, key.offset, data, size, &position, &parent, &index, name, &length);
		if (error != BTRFS_OK) {
			break;
		}
		error = bt_ns_inode(rp->transaction, tree, parent, &directory);
		if (error == BTRFS_NOT_FOUND) {
			return BTRFS_OK;
		}
		if (error == BTRFS_OK) {
			error = bt_rp_inode_in_dir(
			    rp, tree, parent, key.objectid, index, name, length, &in);
		}
		if (error != BTRFS_OK || in) {
			continue;
		}
		error = bt_rp_ref_conflicts(
		    rp, tree, log, parent, key.objectid, index, name, length, &skip);
		if (error == BTRFS_OK && skip) {
			return BTRFS_OK;
		}
		if (error == BTRFS_OK) {
			error =
			    bt_rp_add_link(rp, tree, parent, key.objectid, name, length, 0, index);
			error = error == BTRFS_EXISTS ? BTRFS_CORRUPT : error;
		}
		if (error == BTRFS_OK) {
			error = bt_rp_inode(rp, tree, key.objectid, &item);
		}
		if (error == BTRFS_OK) {
			error = bt_rp_update_inode(rp, tree, key.objectid, &item);
		}
	}
	if (error == BTRFS_NOT_FOUND) {
		error = bt_rp_unlink_old_refs(rp, tree, key, data, size, &skip);
	}
	return error == BTRFS_OK && !skip ? bt_rp_overwrite(rp, tree, key, data, size) : error;
}

/* The extent an extent tree record describes: its extent or metadata item
 * gives its end; a back reference or block group item gives none. */
static int
bt_rp_extent_end(const struct btrfs_fs *view, const struct bt_record *record, uint64_t *end)
{
	uint64_t length;

	if (record->key.type != BT_EXTENT_ITEM && record->key.type != BT_METADATA_ITEM) {
		return 0;
	}
	length = record->key.type == BT_EXTENT_ITEM ? record->key.offset : view->info.node_size;
	*end =
	    length > UINT64_MAX - record->key.objectid ? UINT64_MAX : record->key.objectid + length;
	return 1;
}

/* Whether no extent of the extent tree overlaps [start, end): the last one
 * starting below start ends by it, and none starts inside. Back reference and
 * block group items are passed over, each scan within BT_MAX_TREE_ITEMS. */
static enum btrfs_result
bt_rp_unallocated(struct bt_replay *rp, uint64_t start, uint64_t end)
{
	const struct btrfs_fs *view = bt_mutation_view(rp->transaction->mutation);
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = start - 1, .type = UINT8_MAX, .offset = UINT64_MAX };
	struct bt_key below;
	uint64_t extent_end;
	uint64_t steps;
	enum btrfs_result error = BTRFS_OK;

	if (view == NULL) {
		return rp->transaction->failure;
	}
	bt_cursor_init(&cursor, view, rp->transaction->extents.root);
	for (steps = 0; error == BTRFS_OK; steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		error = bt_cursor_seek(&cursor, key, 1);
		if (error != BTRFS_OK) {
			break;
		}
		(void)bt_cursor_record(&cursor, &record);
		if (bt_key_compare(record.key, key) > 0) {
			/* Nothing at or below key. */
			break;
		}
		if (bt_rp_extent_end(view, &record, &extent_end)) {
			error = extent_end > start ? BTRFS_CORRUPT : BTRFS_OK;
			break;
		}
		/* Next, the highest key the record's own extent item can have. */
		below = (struct bt_key){ .objectid = record.key.objectid,
			.type = BT_METADATA_ITEM,
			.offset = UINT64_MAX };
		if (bt_key_compare(below, record.key) >= 0) {
			if (record.key.objectid == 0) {
				break;
			}
			below = (struct bt_key){ .objectid = record.key.objectid - 1,
				.type = UINT8_MAX,
				.offset = UINT64_MAX };
		}
		key = below;
	}
	if (error == BTRFS_OK) {
		key = (struct bt_key){ .objectid = start };
		error = bt_cursor_seek(&cursor, key, 0);
	}
	for (steps = 0; error == BTRFS_OK; steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid >= end) {
			break;
		}
		if (bt_rp_extent_end(view, &record, &extent_end)) {
			error = BTRFS_CORRUPT;
			break;
		}
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* A logged file extent's data reference: an extent item already in the
 * extent tree gains it at commit; one the uncommitted transaction allocated
 * is allocated now, as btrfs_alloc_logged_file_extent does. */
static enum btrfs_result
bt_rp_reference(struct bt_replay *rp, struct bt_owned_root *tree,
    const struct bt_disk_extent *extent, uint64_t inode, uint64_t position)
{
	struct btrfs_transaction *transaction = rp->transaction;

	struct {
		struct bt_disk_extent_item item;
		uint8_t type;
		struct bt_disk_data_ref reference;
	} wire;
	struct bt_backref reference;
	struct bt_key key = { .objectid = bt_u64(extent->disk_bytenr),
		.type = BT_EXTENT_ITEM,
		.offset = bt_u64(extent->disk_bytes) };
	size_t size;
	size_t chunk = bt_chunk_containing(bt_rp_fs(rp), key.objectid);
	uint64_t sector = bt_rp_fs(rp)->info.sector_size;
	int found;
	enum btrfs_result error;

	bt_zero(&reference, sizeof(reference));
	reference.data = 1;
	reference.root = tree->root.owner;
	reference.inode = inode;
	reference.offset = position;
	error = bt_mutation_find(transaction->mutation, transaction->extents.root, key, rp->kept,
	    bt_rp_fs(rp)->info.node_size, &size);
	found = error == BTRFS_OK;
	if (error == BTRFS_NOT_FOUND || error == BTRFS_RANGE) {
		error = found ? BTRFS_CORRUPT : BTRFS_OK;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	if (found) {
		return bt_tx_queue(transaction, key, &reference, 1);
	}
	if (chunk == bt_rp_fs(rp)->chunk_count ||
	    !(bt_rp_fs(rp)->chunks[chunk].type & BT_BLOCK_DATA) || key.objectid % sector != 0 ||
	    key.offset % sector != 0) {
		return BTRFS_CORRUPT;
	}
	error = bt_rp_unallocated(rp, key.objectid, key.objectid + key.offset);
	if (error == BTRFS_OK) {
		error = bt_space_change_used(transaction->space, key.objectid, key.offset, 1);
	}
	if (error == BTRFS_OK) {
		bt_zero(&wire, sizeof(wire));
		bt_put64(&wire.item.refs, 1);
		bt_put64(&wire.item.generation, bt_rp_transid(rp));
		bt_put64(&wire.item.flags, BT_EXTENT_FLAG_DATA);
		wire.type = BT_EXTENT_DATA_REF;
		bt_put64(&wire.reference.root, tree->root.owner);
		bt_put64(&wire.reference.objectid, inode);
		bt_put64(&wire.reference.offset, position);
		bt_put32(&wire.reference.count, 1);
		error = bt_tx_edit(
		    transaction, &transaction->extents.root, key, &wire, sizeof(wire), BT_INSERT);
	}
	if (error == BTRFS_OK) {
		error = bt_mutation_note_extent(transaction->mutation, key);
	}
	if (error == BTRFS_OK) {
		rp->report->allocated++;
	}
	return error;
}

/* The log's checksums of [start, end) replace any in the checksum tree. */
static enum btrfs_result
bt_rp_csums(struct bt_replay *rp, size_t log, uint64_t start, uint64_t end)
{
	const struct btrfs_fs *fs = bt_rp_fs(rp);
	struct btrfs_transaction *transaction = rp->transaction;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = {
		.objectid = BT_CSUM_OBJECTID, .type = BT_EXTENT_CSUM, .offset = start
	};
	uint64_t sector = fs->info.sector_size;
	uint64_t size = bt_checksum_size(fs->info.checksum_type);
	uint64_t item_end;
	uint64_t first;
	uint64_t last;
	enum btrfs_result error;
	int pass;

	if (start % sector != 0 || end % sector != 0) {
		return BTRFS_CORRUPT;
	}
	bt_cursor_init(&cursor, &rp->view, rp->logs[log]);
	error = BTRFS_NOT_FOUND;
	/* The item reaching start from below, else the first after it. */
	for (pass = 1; pass >= 0 && error == BTRFS_NOT_FOUND; pass--) {
		error = bt_cursor_seek(&cursor, key, pass);
		if (error == BTRFS_OK) {
			(void)bt_cursor_record(&cursor, &record);
			if (pass &&
			    (record.key.objectid != BT_CSUM_OBJECTID ||
				record.key.type != BT_EXTENT_CSUM)) {
				error = BTRFS_NOT_FOUND;
			}
		}
	}
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != BT_CSUM_OBJECTID || record.key.type != BT_EXTENT_CSUM ||
		    record.key.offset >= end) {
			break;
		}
		if (record.size == 0 || record.size % size != 0 ||
		    record.key.offset % sector != 0 ||
		    record.size / size > (UINT64_MAX - record.key.offset) / sector) {
			error = BTRFS_CORRUPT;
			break;
		}
		item_end = record.key.offset + record.size / size * sector;
		first = record.key.offset > start ? record.key.offset : start;
		last = item_end < end ? item_end : end;
		if (first < last) {
			bt_copy(rp->other,
			    record.data + (first - record.key.offset) / sector * size,
			    (size_t)((last - first) / sector * size));
			error = bt_csum_delete(transaction->mutation, &transaction->checksums.root,
			    first, last - first);
			if (error == BTRFS_OK) {
				error = bt_csum_insert_sums(transaction->mutation,
				    &transaction->checksums.root, first, rp->other, last - first);
			}
		}
		if (error == BTRFS_OK) {
			error = bt_cursor_next(&cursor);
		}
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* replay_one_extent: a logged file extent item replaces the file's coverage
 * of its range; a data extent gains the reference and the log's checksums;
 * the inode's byte count follows. An equal item already present stays. */
static BT_NOINLINE enum btrfs_result
bt_rp_file_extent(struct bt_replay *rp, struct bt_owned_root *tree, size_t log, struct bt_key key,
    const uint8_t *data, size_t size)
{
	struct btrfs_transaction *transaction = rp->transaction;
	struct bt_disk_extent extent;
	struct bt_disk_inode item;
	uint64_t sector = bt_rp_fs(rp)->info.sector_size;
	uint64_t bytes;
	uint64_t end;
	uint64_t removed = 0;
	uint64_t csum_start;
	size_t existing;
	int inline_item;
	int found;
	enum btrfs_result error;

	error = bt_rp_extent(data, size, &extent, &inline_item);
	if (error != BTRFS_OK) {
		return error;
	}
	if (inline_item) {
		if (key.offset != 0) {
			return BTRFS_CORRUPT;
		}
		bytes = bt_u64(extent.header.ram_bytes);
		end = bytes + (sector - bytes % sector) % sector;
	} else {
		bytes = bt_u64(extent.disk_bytenr) == 0 ? 0 : bt_u64(extent.length);
		if (bt_u64(extent.length) > UINT64_MAX - key.offset) {
			return BTRFS_CORRUPT;
		}
		end = key.offset + bt_u64(extent.length);
	}
	error = bt_rp_inode(rp, tree, key.objectid, &item);
	if (error == BTRFS_OK && !inline_item) {
		error = bt_rp_find(rp, tree, key, rp->kept, &existing, &found);
		if (error == BTRFS_OK && found && existing == sizeof(extent) &&
		    bt_equal(rp->kept, data, sizeof(extent))) {
			return BTRFS_OK;
		}
	}
	if (error == BTRFS_OK) {
		error =
		    bt_tx_drop_range(transaction, tree, key.objectid, key.offset, end, &removed);
	}
	if (error == BTRFS_OK && inline_item) {
		error = bt_rp_overwrite(rp, tree, key, data, size);
	} else if (error == BTRFS_OK &&
	    (bt_u64(extent.disk_bytenr) != 0 ||
		!(bt_rp_fs(rp)->info.incompat_features & BT_FEATURE_NO_HOLES))) {
		error = bt_tx_edit(transaction, &tree->root, key, data, sizeof(extent), BT_INSERT);
		if (error == BTRFS_OK && bt_u64(extent.disk_bytenr) != 0) {
			error = bt_rp_reference(
			    rp, tree, &extent, key.objectid, key.offset - bt_u64(extent.offset));
		}
		if (error == BTRFS_OK && bt_u64(extent.disk_bytenr) != 0) {
			csum_start = bt_u64(extent.disk_bytenr) +
			    (extent.header.compression != BTRFS_COMPRESSION_NONE
				    ? 0
				    : bt_u64(extent.offset));
			error = bt_rp_csums(rp, log, csum_start,
			    extent.header.compression != BTRFS_COMPRESSION_NONE
				? bt_u64(extent.disk_bytenr) + bt_u64(extent.disk_bytes)
				: csum_start + bt_u64(extent.length));
		}
	}
	if (error == BTRFS_OK) {
		error = bt_rp_inode(rp, tree, key.objectid, &item);
	}
	if (error == BTRFS_OK &&
	    (bt_u64(item.nbytes) > UINT64_MAX - bytes || bt_u64(item.nbytes) + bytes < removed)) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		transaction->changed = 1;
		bt_put64(&item.nbytes, bt_u64(item.nbytes) + bytes - removed);
		error = bt_rp_update_inode(rp, tree, key.objectid, &item);
	}
	if (error == BTRFS_OK) {
		rp->report->extents++;
	}
	return error;
}

/* Names of inode in its INODE_REF and INODE_EXTREF items. */
static enum btrfs_result
bt_rp_count_names(struct bt_replay *rp, struct bt_owned_root *tree, uint64_t inode, uint32_t *count)
{
	struct bt_key key = { .objectid = inode, .type = BT_INODE_REF };
	struct bt_key found_key;
	uint8_t name[BTRFS_NAME_MAX];
	uint64_t parent;
	uint64_t index;
	uint64_t steps;
	size_t position;
	size_t length;
	size_t size;
	int found;
	int extended;
	enum btrfs_result error = BTRFS_OK;

	*count = 0;
	for (extended = 0; error == BTRFS_OK && extended < 2; extended++) {
		key = (struct bt_key){ .objectid = inode,
			.type = extended ? BT_INODE_EXTREF : BT_INODE_REF };
		for (steps = 0; error == BTRFS_OK; steps++) {
			if (steps == BT_MAX_TREE_ITEMS) {
				return BTRFS_UNSUPPORTED;
			}
			error = bt_ns_neighbor(rp->transaction, tree, key, 0, &found_key, &found);
			if (error != BTRFS_OK || !found || found_key.objectid != inode ||
			    found_key.type != key.type) {
				break;
			}
			error = bt_rp_find(rp, tree, found_key, rp->kept, &size, &found);
			position = 0;
			while (error == BTRFS_OK) {
				error = bt_rp_ref_entry(extended, found_key.offset, rp->kept, size,
				    &position, &parent, &index, name, &length);
				if (error == BTRFS_OK && *count == UINT32_MAX) {
					error = BTRFS_CORRUPT;
				}
				if (error == BTRFS_OK) {
					(*count)++;
				}
			}
			error = error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
			if (found_key.offset == UINT64_MAX) {
				break;
			}
			key.offset = found_key.offset + 1;
		}
	}
	return error;
}

/* fixup_inode_link_counts: each inode the replay touched gets the links its
 * names give it, highest number first; one left without names loses its
 * directory entries and gets an orphan item for the cleanup that follows. */
static BT_NOINLINE enum btrfs_result
bt_rp_fixups(struct bt_replay *rp, struct bt_owned_root *tree, size_t log)
{
	struct bt_disk_inode item;
	struct bt_key orphan = { .objectid = BT_ORPHAN_OBJECTID, .type = BT_ORPHAN_ITEM };
	uint64_t inode;
	uint64_t steps;
	uint32_t count = 0;
	size_t best;
	size_t i;
	enum btrfs_result error = BTRFS_OK;

	for (steps = 0; error == BTRFS_OK; steps++) {
		if (steps == BT_REPLAY_FIXUPS) {
			return BTRFS_UNSUPPORTED;
		}
		best = rp->fixup_count;
		for (i = 0; i < rp->fixup_count; i++) {
			if (rp->fixups[i].log == log &&
			    (best == rp->fixup_count ||
				rp->fixups[i].inode > rp->fixups[best].inode)) {
				best = i;
			}
		}
		if (best == rp->fixup_count) {
			break;
		}
		inode = rp->fixups[best].inode;
		rp->fixups[best] = rp->fixups[--rp->fixup_count];
		error = bt_rp_inode(rp, tree, inode, &item);
		if (error == BTRFS_OK) {
			error = bt_rp_count_names(rp, tree, inode, &count);
		}
		/* Linux updated each such inode when it listed it for the fixup. */
		if (error == BTRFS_OK) {
			bt_put32(&item.links, count);
			error = bt_rp_update_inode(rp, tree, inode, &item);
		}
		if (error != BTRFS_OK || count != 0) {
			continue;
		}
		if (bt_ns_is_directory(&item)) {
			error = bt_rp_dir_deletes(rp, tree, log, 1, inode);
		}
		if (error == BTRFS_OK) {
			orphan.offset = inode;
			error =
			    bt_tx_edit(rp->transaction, &tree->root, orphan, NULL, 0, BT_INSERT);
			error = error == BTRFS_EXISTS ? BTRFS_OK : error;
		}
		if (error == BTRFS_OK) {
			rp->report->orphans++;
		}
	}
	return error;
}

/* One pass over every log, in key order; the last pass ends each log with
 * its link count fixups. A log of a deleted subvolume is skipped. */
static BT_NOINLINE enum btrfs_result
bt_rp_pass(struct bt_replay *rp, enum bt_replay_stage stage)
{
	const struct bt_disk_inode *inode;
	struct bt_owned_root *tree;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key;
	struct bt_key first = { 0 };
	size_t log;
	enum btrfs_result error = BTRFS_OK;

	for (log = 0; error == BTRFS_OK && log < rp->log_count; log++) {
		error = bt_ns_begin(rp->transaction, rp->subvolumes[log], &tree);
		if (error == BTRFS_NOT_FOUND) {
			error = BTRFS_OK;
			continue;
		}
		rp->ignored = 0;
		bt_cursor_init(&cursor, &rp->view, rp->logs[log]);
		error = error == BTRFS_OK ? bt_cursor_seek(&cursor, first, 0) : error;
		while (error == BTRFS_OK) {
			(void)bt_cursor_record(&cursor, &record);
			key = record.key;
			bt_copy(rp->item, record.data, record.size);
			/* A tmpfile logged without a link is never replayed. */
			if (key.type == BT_INODE_ITEM) {
				if (record.size != sizeof(*inode)) {
					error = BTRFS_CORRUPT;
					break;
				}
				inode = (const void *)rp->item;
				rp->ignored = bt_u32(inode->links) == 0 ? key.objectid : 0;
			}
			if (rp->ignored == 0 || key.objectid != rp->ignored) {
				if (stage == BT_REPLAY_INODES && key.type == BT_INODE_ITEM) {
					error = bt_rp_inode_item(
					    rp, tree, log, key, rp->item, record.size);
				} else if (stage == BT_REPLAY_NAMES && key.type == BT_DIR_INDEX) {
					error = bt_rp_dir_index(
					    rp, tree, log, key, rp->item, record.size);
				} else if (stage == BT_REPLAY_ALL && key.type == BT_XATTR_ITEM) {
					error =
					    bt_rp_overwrite(rp, tree, key, rp->item, record.size);
				} else if (stage == BT_REPLAY_ALL &&
				    (key.type == BT_INODE_REF || key.type == BT_INODE_EXTREF)) {
					error = bt_rp_inode_ref(
					    rp, tree, log, key, rp->item, record.size);
				} else if (stage == BT_REPLAY_ALL && key.type == BT_EXTENT_DATA) {
					error = bt_rp_file_extent(
					    rp, tree, log, key, rp->item, record.size);
				}
			}
			if (error == BTRFS_OK) {
				error = bt_cursor_next(&cursor);
			}
		}
		bt_cursor_fini(&cursor);
		error = error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
		if (error == BTRFS_OK && stage == BT_REPLAY_ALL) {
			error = bt_rp_fixups(rp, tree, log);
			rp->report->logs++;
		}
	}
	return error;
}

/* Lists the subvolume logs of the log root tree and withholds every log
 * block and logged data extent from allocation. */
static BT_NOINLINE enum btrfs_result
bt_rp_open(struct bt_replay *rp)
{
	const struct btrfs_fs *base = bt_rp_fs(rp);
	const struct bt_disk_root *disk;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root logs = { .address = base->log_root,
		.generation = base->info.generation + 1,
		.owner = BT_TREE_LOG_OBJECTID,
		.level = base->log_level };
	struct bt_key first = { 0 };
	uint64_t visited = 0;
	size_t i;
	enum btrfs_result error;

	rp->view = *base;
	rp->view.info.generation = logs.generation;
	rp->view.cache_limit = logs.generation;
	rp->view.private_node = NULL;
	rp->view.private_root = NULL;
	rp->view.private_borrow = 0;
	bt_cursor_init(&cursor, &rp->view, logs);
	error = bt_cursor_seek(&cursor, first, 0);
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		disk = (const void *)record.data;
		if (record.key.objectid != BT_TREE_LOG_OBJECTID ||
		    record.key.type != BT_ROOT_ITEM || !bt_file_tree(record.key.offset) ||
		    record.size < sizeof(*disk) || disk->level >= BT_MAX_LEVEL ||
		    bt_u64(disk->generation) == 0 || bt_u64(disk->generation) > logs.generation) {
			error = BTRFS_CORRUPT;
			break;
		}
		if (rp->log_count == BT_REPLAY_TREES) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		rp->subvolumes[rp->log_count] = record.key.offset;
		rp->logs[rp->log_count++] = (struct bt_root){ .address = bt_u64(disk->bytenr),
			.generation = bt_u64(disk->generation),
			.owner = BT_TREE_LOG_OBJECTID,
			.level = disk->level };
		error = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	error = error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
	if (error == BTRFS_OK) {
		error = bt_rp_withhold(rp, logs, &visited);
	}
	for (i = 0; error == BTRFS_OK && i < rp->log_count; i++) {
		error = bt_rp_withhold(rp, rp->logs[i], &visited);
	}
	return error;
}

/* The writer of an inspection: nothing is committed, so nothing is written. */
static enum btrfs_result
bt_rp_refuse_write(void *context, uint64_t offset, const void *bytes, size_t size)
{
	(void)context;
	(void)offset;
	(void)bytes;
	(void)size;
	return BTRFS_READ_ONLY;
}

static enum btrfs_result
bt_rp_refuse_flush(void *context)
{
	(void)context;
	return BTRFS_READ_ONLY;
}

/* Releases storage the replay allocated, if it did. */
static void
bt_rp_release(const struct btrfs_environment *environment, void *bytes, size_t size)
{
	if (bytes != NULL) {
		environment->release(environment->context, bytes, size);
	}
}

enum btrfs_result
btrfs_replay_log(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, struct btrfs_time now,
    struct btrfs_replay_report *report)
{
	const struct btrfs_write_environment inspection = { .write = bt_rp_refuse_write,
		.flush = bt_rp_refuse_flush,
		.compression = BTRFS_COMPRESSION_NONE };
	struct bt_replay *rp = NULL;
	struct bt_disk_super *super;
	struct btrfs_fs *fs = NULL;
	struct btrfs_transaction *transaction = NULL;
	size_t node_size = 0;
	enum bt_replay_stage stage;
	enum btrfs_result error;

	if (environment == NULL || environment->read == NULL || environment->allocate == NULL ||
	    environment->release == NULL || report == NULL || now.nanoseconds >= BT_NANOSECONDS) {
		return BTRFS_INVALID_ARGUMENT;
	}
	bt_zero(report, sizeof(*report));
	if (environment->size_bytes < BT_SUPER_OFFSET + BT_SUPER_SIZE) {
		return BTRFS_NOT_BTRFS;
	}
	super = environment->allocate(environment->context, sizeof(*super));
	if (super == NULL) {
		return BTRFS_NO_MEMORY;
	}
	error = environment->read(environment->context, BT_SUPER_OFFSET, super, sizeof(*super));
	if (error == BTRFS_OK) {
		error = bt_super_check(super, BT_SUPER_OFFSET);
	}
	if (error == BTRFS_OK && bt_u64(super->log_root) == 0) {
		error = BTRFS_NOT_FOUND;
	}
	if (error == BTRFS_OK) {
		report->generation = bt_u64(super->generation);
		error = bt_mount_super(
		    environment, super, BT_SUPER_OFFSET, BTRFS_TOP_LEVEL_TREE, 1, &fs);
	}
	environment->release(environment->context, super, sizeof(*super));
	if (error == BTRFS_OK) {
		error = btrfs_transaction_begin(
		    fs, writer != NULL ? writer : &inspection, &transaction);
	}
	if (error == BTRFS_OK) {
		rp = environment->allocate(environment->context, sizeof(*rp));
		error = rp == NULL ? BTRFS_NO_MEMORY : BTRFS_OK;
	}
	if (error == BTRFS_OK) {
		bt_zero(rp, sizeof(*rp));
		node_size = fs->info.node_size;
		rp->transaction = transaction;
		rp->now = now;
		rp->report = report;
		rp->item = environment->allocate(environment->context, node_size);
		rp->other = environment->allocate(environment->context, node_size);
		rp->kept = environment->allocate(environment->context, node_size);
		rp->nodes = environment->allocate(environment->context, BT_MAX_LEVEL * node_size);
		if (rp->item == NULL || rp->other == NULL || rp->kept == NULL ||
		    rp->nodes == NULL) {
			error = BTRFS_NO_MEMORY;
		}
	}
	if (error == BTRFS_OK) {
		error = bt_rp_open(rp);
	}
	for (stage = BT_REPLAY_INODES; error == BTRFS_OK && stage <= BT_REPLAY_ALL; stage++) {
		error = bt_rp_pass(rp, stage);
	}
	/* Even an empty log is committed away: the next superblock names none. */
	if (error == BTRFS_OK && writer == NULL) {
		error = BTRFS_RECOVERY_REQUIRED;
	} else if (error == BTRFS_OK) {
		transaction->changed = 1;
		error = btrfs_transaction_commit(transaction);
	}
	if (rp != NULL) {
		bt_rp_release(environment, rp->item, node_size);
		bt_rp_release(environment, rp->other, node_size);
		bt_rp_release(environment, rp->kept, node_size);
		bt_rp_release(environment, rp->nodes, BT_MAX_LEVEL * node_size);
		bt_rp_release(environment, rp->fixups, rp->fixup_capacity * sizeof(*rp->fixups));
		environment->release(environment->context, rp, sizeof(*rp));
	}
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	return error;
}
