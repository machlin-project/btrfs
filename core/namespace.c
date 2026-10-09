/* SPDX-License-Identifier: BSD-3-Clause */
#include "namespace.h"

/* Linux's compression property: a value starting with a codec name (a level
 * may follow), or "no"/"none" to disable compression. */
static const struct bt_codec bt_ns_codecs[] = { { "zlib", 0, BTRFS_COMPRESSION_ZLIB },
	{ "lzo", BT_FEATURE_COMPRESS_LZO, BTRFS_COMPRESSION_LZO },
	{ "zstd", BT_FEATURE_COMPRESS_ZSTD, BTRFS_COMPRESSION_ZSTD } };

uint8_t
bt_ns_type(uint32_t mode)
{
	switch (mode & BTRFS_MODE_TYPE) {
	case BTRFS_MODE_REGULAR:
		return BTRFS_FT_REGULAR;
	case BTRFS_MODE_DIRECTORY:
		return BTRFS_FT_DIRECTORY;
	case BTRFS_MODE_CHARACTER:
		return BTRFS_FT_CHARACTER;
	case BTRFS_MODE_BLOCK:
		return BTRFS_FT_BLOCK;
	case BTRFS_MODE_FIFO:
		return BTRFS_FT_FIFO;
	case BTRFS_MODE_SOCKET:
		return BTRFS_FT_SOCKET;
	case BTRFS_MODE_SYMLINK:
		return BTRFS_FT_SYMLINK;
	default:
		return BTRFS_FT_UNKNOWN;
	}
}

int
bt_ns_is_directory(const struct bt_disk_inode *item)
{
	return (bt_u32(item->mode) & BTRFS_MODE_TYPE) == BTRFS_MODE_DIRECTORY;
}

/* Linux refuses changes to immutable inodes; append-only inodes keep their
 * names and attributes and only grow. */
int
bt_ns_immutable(const struct bt_disk_inode *item)
{
	return (bt_u64(item->flags) & BT_INODE_IMMUTABLE) != 0;
}

int
bt_ns_frozen(const struct bt_disk_inode *item)
{
	return (bt_u64(item->flags) & (BT_INODE_IMMUTABLE | BT_INODE_APPEND)) != 0;
}

size_t
bt_ns_length(const char *text)
{
	size_t length = 0;

	while (text[length] != '\0') {
		length++;
	}
	return length;
}

uint64_t
bt_ns_hash(const void *name, size_t length)
{
	return bt_crc32c(BT_NAME_HASH_SEED, name, length);
}

/* Linux's btrfs_extref_hash seeds CRC32C with the parent's number. */
uint64_t
bt_ns_extref_hash(uint64_t directory, const void *name, size_t length)
{
	return bt_crc32c((uint32_t)directory, name, length);
}

enum btrfs_result
bt_ns_name(const void *name, size_t length)
{
	const uint8_t *bytes = name;

	if (name != NULL && length > BTRFS_NAME_MAX) {
		return BTRFS_NAME_TOO_LONG;
	}
	if (!bt_name_valid(name, length) || (length == 1 && bytes[0] == '.') ||
	    (length == 2 && bytes[0] == '.' && bytes[1] == '.')) {
		return BTRFS_INVALID_ARGUMENT;
	}
	return BTRFS_OK;
}

size_t
bt_ns_item_limit(const struct btrfs_transaction *transaction)
{
	return transaction->base->info.node_size - sizeof(struct bt_disk_header) -
	    sizeof(struct bt_disk_item);
}

uint64_t
bt_ns_transid(const struct btrfs_transaction *transaction)
{
	return transaction->base->info.generation + 1;
}

/* The key of the first item at or after key (before: the last at or before
 * it); found is 0 when the tree holds none. */
enum btrfs_result
bt_ns_neighbor(struct btrfs_transaction *transaction, const struct bt_owned_root *tree,
    struct bt_key key, int before, struct bt_key *result, int *found)
{
	struct bt_cursor cursor;
	struct bt_record record;
	enum btrfs_result error;

	*found = 0;
	bt_cursor_init(&cursor, bt_mutation_view(transaction->mutation), tree->root);
	error = bt_cursor_seek(&cursor, key, before);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		*result = record.key;
		*found = 1;
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* Loads one packed item into the item buffer; an absent item is not an error. */
enum btrfs_result
bt_ns_load(struct btrfs_transaction *transaction, const struct bt_owned_root *tree,
    struct bt_key key, struct bt_packed *packed)
{
	enum btrfs_result error;

	packed->key = key;
	packed->bytes = transaction->item;
	error = bt_mutation_find(transaction->mutation, tree->root, key, packed->bytes,
	    transaction->base->info.node_size, &packed->size);
	packed->present = error == BTRFS_OK;
	if (error == BTRFS_NOT_FOUND) {
		packed->size = 0;
		return BTRFS_OK;
	}
	return error == BTRFS_RANGE ? BTRFS_CORRUPT : error;
}

/* Finds name among the entries of a packed DIR_ITEM, DIR_INDEX or XATTR_ITEM. */
enum btrfs_result
bt_ns_dir_find(const struct bt_packed *packed, const void *name, size_t length, size_t *offset,
    size_t *entry_size, const struct bt_disk_dir **header)
{
	struct bt_record record = { packed->key, packed->bytes, packed->size };
	const uint8_t *entry_name;
	const uint8_t *data;
	size_t position = 0;
	size_t start;
	enum btrfs_result error;

	for (;;) {
		start = position;
		error = bt_dir_record(&record, &position, header, &entry_name, &data);
		if (error != BTRFS_OK) {
			return error;
		}
		if (bt_u16((*header)->name_length) == length &&
		    bt_equal(entry_name, name, length)) {
			*offset = start;
			*entry_size = position - start;
			return BTRFS_OK;
		}
	}
}

/* Finds name among packed INODE_REF entries: index, name length and name. */
enum btrfs_result
bt_ns_ref_find(const struct bt_packed *packed, const void *name, size_t length, size_t *offset,
    uint64_t *index)
{
	const struct bt_disk_inode_ref *ref;
	size_t position = 0;
	size_t name_length;

	while (position < packed->size) {
		if (packed->size - position < sizeof(*ref)) {
			return BTRFS_CORRUPT;
		}
		ref = (const void *)(packed->bytes + position);
		name_length = bt_u16(ref->name_length);
		if (name_length > packed->size - position - sizeof(*ref) ||
		    !bt_name_valid(ref + 1, name_length)) {
			return BTRFS_CORRUPT;
		}
		if (name_length == length && bt_equal(ref + 1, name, length)) {
			*offset = position;
			*index = bt_u64(ref->index);
			return BTRFS_OK;
		}
		position += sizeof(*ref) + name_length;
	}
	return BTRFS_NOT_FOUND;
}

/* Finds directory's name among packed INODE_EXTREF entries: parent, index,
 * name length and name. */
enum btrfs_result
bt_ns_extref_find(const struct bt_packed *packed, uint64_t directory, const void *name,
    size_t length, size_t *offset, uint64_t *index)
{
	const struct bt_disk_inode_extref *ref;
	size_t position = 0;
	size_t name_length;

	while (position < packed->size) {
		if (packed->size - position < sizeof(*ref)) {
			return BTRFS_CORRUPT;
		}
		ref = (const void *)(packed->bytes + position);
		name_length = bt_u16(ref->name_length);
		if (name_length > packed->size - position - sizeof(*ref) ||
		    !bt_name_valid(ref + 1, name_length) ||
		    bt_ns_extref_hash(bt_u64(ref->parent), ref + 1, name_length) !=
			packed->key.offset) {
			return BTRFS_CORRUPT;
		}
		if (bt_u64(ref->parent) == directory && name_length == length &&
		    bt_equal(ref + 1, name, length)) {
			*offset = position;
			*index = bt_u64(ref->index);
			return BTRFS_OK;
		}
		position += sizeof(*ref) + name_length;
	}
	return BTRFS_NOT_FOUND;
}

/* Appends one entry to a packed item, creating the item when absent. Linux
 * extends a colliding item, so later names follow earlier ones. */
enum btrfs_result
bt_ns_append(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct bt_packed *packed, const uint8_t *entry, size_t entry_size)
{
	enum btrfs_result error;

	if (entry_size > bt_ns_item_limit(transaction) - packed->size) {
		return BTRFS_RANGE;
	}
	bt_copy(packed->bytes + packed->size, entry, entry_size);
	error = bt_tx_edit(transaction, &tree->root, packed->key, packed->bytes,
	    packed->size + entry_size, packed->present ? BT_REPLACE : BT_INSERT);
	if (error == BTRFS_OK) {
		packed->size += entry_size;
		packed->present = 1;
	}
	return error;
}

/* Removes [offset, offset + length) of a packed item, deleting the item when
 * nothing remains. */
enum btrfs_result
bt_ns_cut(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct bt_packed *packed, size_t offset, size_t length)
{
	enum btrfs_result error;

	if (length == packed->size) {
		error = bt_tx_edit(transaction, &tree->root, packed->key, NULL, 0, BT_DELETE);
	} else {
		bt_move(packed->bytes + offset, packed->bytes + offset + length,
		    packed->size - offset - length);
		error = bt_tx_edit(transaction, &tree->root, packed->key, packed->bytes,
		    packed->size - length, BT_REPLACE);
	}
	if (error == BTRFS_OK) {
		packed->size -= length;
		packed->present = packed->size != 0;
	}
	return error;
}

size_t
bt_ns_dir_entry(uint8_t *out, struct bt_key location, uint64_t transid, const void *name,
    size_t name_length, const void *data, size_t data_length, uint8_t type)
{
	struct bt_disk_dir *header = (void *)out;

	bt_zero(header, sizeof(*header));
	bt_key_encode(&header->location, location);
	bt_put64(&header->transid, transid);
	bt_put16(&header->data_length, (uint16_t)data_length);
	bt_put16(&header->name_length, (uint16_t)name_length);
	header->type = type;
	bt_copy(header + 1, name, name_length);
	if (data_length != 0) {
		bt_copy((uint8_t *)(header + 1) + name_length, data, data_length);
	}
	return sizeof(*header) + name_length + data_length;
}

enum btrfs_result
bt_ns_inode(struct btrfs_transaction *transaction, const struct bt_owned_root *tree, uint64_t inode,
    struct bt_disk_inode *item)
{
	struct bt_key key = { .objectid = inode, .type = BT_INODE_ITEM };
	size_t length;
	enum btrfs_result error;

	error =
	    bt_mutation_find(transaction->mutation, tree->root, key, item, sizeof(*item), &length);
	if (error == BTRFS_RANGE || (error == BTRFS_OK && length != sizeof(*item))) {
		return BTRFS_CORRUPT;
	}
	return error;
}

/* An inode named by a directory entry must exist. */
enum btrfs_result
bt_ns_named_inode(struct btrfs_transaction *transaction, const struct bt_owned_root *tree,
    uint64_t inode, struct bt_disk_inode *item)
{
	enum btrfs_result error;

	error = bt_ns_inode(transaction, tree, inode, item);
	return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
}

enum btrfs_result
bt_ns_parent(struct btrfs_transaction *transaction, const struct bt_owned_root *tree,
    uint64_t directory, struct bt_disk_inode *item)
{
	enum btrfs_result error;

	error = bt_ns_inode(transaction, tree, directory, item);
	if (error == BTRFS_OK && !bt_ns_is_directory(item)) {
		error = BTRFS_NOT_DIRECTORY;
	}
	return error;
}

/* Stores an inode changed now: transid, change counter and ctime advance. */
enum btrfs_result
bt_ns_store(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t inode,
    struct bt_disk_inode *item, struct btrfs_time time)
{
	struct bt_key key = { .objectid = inode, .type = BT_INODE_ITEM };
	enum btrfs_result error;

	bt_put64(&item->transid, bt_ns_transid(transaction));
	bt_put64(&item->sequence, bt_u64(item->sequence) + 1);
	bt_put64(&item->ctime.seconds, (uint64_t)time.seconds);
	bt_put32(&item->ctime.nanoseconds, time.nanoseconds);
	error = bt_tx_edit(transaction, &tree->root, key, item, sizeof(*item), BT_REPLACE);
	if (error == BTRFS_OK) {
		transaction->changed = 1;
		bt_put64(&tree->item.ctransid, bt_ns_transid(transaction));
		tree->item.ctime = item->ctime;
	}
	return error;
}

/* A directory gains or loses a name: Linux keeps its size at twice the sum
 * of its name lengths and updates its modification and change times. */
enum btrfs_result
bt_ns_directory(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    uint64_t directory, size_t length, int add, struct btrfs_time time)
{
	struct bt_disk_inode item;
	uint64_t size;
	uint64_t change = 2 * (uint64_t)length;
	enum btrfs_result error;

	error = bt_ns_named_inode(transaction, tree, directory, &item);
	if (error != BTRFS_OK) {
		return error;
	}
	if (!bt_ns_is_directory(&item)) {
		return BTRFS_CORRUPT;
	}
	size = bt_u64(item.size);
	if (add ? size > UINT64_MAX - change : size < change) {
		return BTRFS_CORRUPT;
	}
	bt_put64(&item.size, add ? size + change : size - change);
	bt_put64(&item.mtime.seconds, (uint64_t)time.seconds);
	bt_put32(&item.mtime.nanoseconds, time.nanoseconds);
	return bt_ns_store(transaction, tree, directory, &item, time);
}

enum btrfs_result
btrfs_counters_create(const struct btrfs_environment *environment, struct btrfs_counters **result)
{
	struct btrfs_counters *counters;

	if (environment == NULL || result == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	counters = environment->allocate(environment->context, sizeof(*counters));
	if (counters == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(counters, sizeof(*counters));
	counters->environment = *environment;
	*result = counters;
	return BTRFS_OK;
}

void
btrfs_counters_destroy(struct btrfs_counters *counters)
{
	if (counters != NULL) {
		counters->environment.release(
		    counters->environment.context, counters, sizeof(*counters));
	}
}

enum btrfs_result
btrfs_transaction_use_counters(
    struct btrfs_transaction *transaction, struct btrfs_counters *counters)
{
	if (transaction == NULL || counters == NULL || transaction->counters != NULL ||
	    transaction->changed || transaction->tree_count != 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	transaction->counters = counters;
	return BTRFS_OK;
}

/* The tree's counter, added when there is room; NULL without counters or
 * room. */
static struct bt_tree_counter *
bt_ns_tree_counter(struct btrfs_transaction *transaction, uint64_t tree)
{
	struct btrfs_counters *counters = transaction->counters;
	size_t i;

	if (counters == NULL) {
		return NULL;
	}
	for (i = 0; i < counters->tree_count; i++) {
		if (counters->trees[i].tree == tree) {
			return &counters->trees[i];
		}
	}
	if (counters->tree_count == BT_COUNTER_TREES) {
		return NULL;
	}
	counters->trees[counters->tree_count].tree = tree;
	counters->trees[counters->tree_count].next = 0;
	return &counters->trees[counters->tree_count++];
}

/* The directory's counter slot. add claims a free slot for an absent
 * directory, emptying a table at its load bound first; otherwise an absent
 * directory has no slot (NULL). */
struct bt_directory_counter *
bt_counters_directory(struct btrfs_counters *counters, uint64_t tree, uint64_t directory, int add)
{
	struct bt_directory_counter *slot;
	size_t index;
	size_t probes;

	if (counters == NULL) {
		return NULL;
	}
	if (add && counters->directory_count == BT_COUNTER_LOAD) {
		bt_zero(counters->directories, sizeof(counters->directories));
		counters->directory_count = 0;
		counters->forgotten++;
	}
	index = (size_t)((tree * UINT64_C(0x9e3779b97f4a7c15)) ^
	    (directory * UINT64_C(0xc2b2ae3d27d4eb4f)));
	for (probes = 0; probes < BT_COUNTER_SLOTS; probes++) {
		slot = &counters->directories[(index + probes) & (BT_COUNTER_SLOTS - 1U)];
		if (slot->directory == directory && slot->tree == tree) {
			return slot;
		}
		if (slot->directory == 0) {
			if (!add) {
				return NULL;
			}
			slot->tree = tree;
			slot->directory = directory;
			slot->known = 0;
			counters->directory_count++;
			return slot;
		}
	}
	return NULL;
}

/* Linux continues inode numbers after the tree's highest object; a mount's
 * counters continue after the highest number they handed out. */
static enum btrfs_result
bt_ns_objectid(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t *result)
{
	struct bt_key key = { BT_LAST_FREE_OBJECTID, UINT64_MAX, UINT8_MAX };
	struct bt_key last;
	struct bt_tree_counter *counter;
	uint64_t *next = &transaction->next_objectid[tree - transaction->trees];
	int found;
	enum btrfs_result error;

	counter = bt_ns_tree_counter(transaction, tree->root.owner);
	if (*next == 0 && counter != NULL) {
		*next = counter->next;
	}
	if (*next == 0) {
		error = bt_ns_neighbor(transaction, tree, key, 1, &last, &found);
		if (error != BTRFS_OK) {
			return error;
		}
		*next = found && last.objectid >= BTRFS_ROOT_INODE ? last.objectid + 1
								   : BTRFS_ROOT_INODE;
	}
	if (*next >= BT_LAST_FREE_OBJECTID) {
		return BTRFS_NO_SPACE;
	}
	*result = (*next)++;
	if (counter != NULL) {
		counter->next = *next;
	}
	return BTRFS_OK;
}

/* Directory indexes continue after the highest DIR_INDEX, starting at 2, and
 * are never handed out twice in one transaction; a mount's counters keep
 * them monotonic across transactions while the directory stays remembered. */
enum btrfs_result
bt_ns_index(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t directory,
    uint64_t *result)
{
	struct bt_index_cache *cache = NULL;
	struct bt_directory_counter *counter;
	struct bt_key key = { directory, UINT64_MAX, BT_DIR_INDEX };
	struct bt_key last;
	size_t i;
	int found;
	enum btrfs_result error;

	for (i = 0; i < transaction->index_count && cache == NULL; i++) {
		if (transaction->indexes[i].tree == tree->root.owner &&
		    transaction->indexes[i].directory == directory) {
			cache = &transaction->indexes[i];
		}
	}
	if (cache == NULL) {
		if (transaction->index_count == BT_TRANSACTION_INDEXES) {
			return BTRFS_UNSUPPORTED;
		}
		counter =
		    bt_counters_directory(transaction->counters, tree->root.owner, directory, 0);
		if (counter != NULL && counter->known) {
			last.offset = counter->next;
		} else {
			error = bt_ns_neighbor(transaction, tree, key, 1, &last, &found);
			if (error != BTRFS_OK) {
				return error;
			}
			/* 0 marks an exhausted index space. */
			last.offset =
			    found && last.objectid == directory && last.type == BT_DIR_INDEX
			    ? (last.offset == UINT64_MAX ? 0 : last.offset + 1)
			    : BT_DIR_START_INDEX;
		}
		cache = &transaction->indexes[transaction->index_count++];
		cache->tree = tree->root.owner;
		cache->directory = directory;
		cache->next = last.offset;
	}
	if (cache->next == 0) {
		return BTRFS_RANGE;
	}
	*result = cache->next;
	cache->next = cache->next == UINT64_MAX ? 0 : cache->next + 1;
	counter = bt_counters_directory(transaction->counters, tree->root.owner, directory, 1);
	if (counter != NULL) {
		counter->next = cache->next;
		counter->known = 1;
	}
	return BTRFS_OK;
}

/* A new directory reuses no forgotten counter of an earlier inode with its
 * number. */
void
bt_ns_new_directory(struct btrfs_transaction *transaction, uint64_t tree, uint64_t directory)
{
	struct bt_directory_counter *counter;

	counter = bt_counters_directory(transaction->counters, tree, directory, 0);
	if (counter != NULL) {
		counter->known = 0;
	}
}

static int
bt_ns_extended(const struct btrfs_transaction *transaction)
{
	return (transaction->base->info.incompat_features & BT_FEATURE_EXTENDED_IREF) != 0;
}

/* Resolves name in directory and cross-checks its inode reference and index. */
enum btrfs_result
bt_ns_lookup(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t directory,
    const void *name, size_t length, struct bt_entry *entry)
{
	const struct bt_disk_dir *header;
	struct bt_packed packed;
	struct bt_key key = { directory, bt_ns_hash(name, length), BT_DIR_ITEM };
	struct bt_key location;
	size_t offset;
	size_t entry_size = 0;
	enum btrfs_result error;

	error = bt_ns_load(transaction, tree, key, &packed);
	if (error != BTRFS_OK || !packed.present) {
		return error == BTRFS_OK ? BTRFS_NOT_FOUND : error;
	}
	error = bt_ns_dir_find(&packed, name, length, &offset, &entry_size, &header);
	if (error != BTRFS_OK) {
		return error;
	}
	location = bt_key_decode(&header->location);
	entry->type = header->type;
	if (location.type == BT_ROOT_ITEM) {
		return BTRFS_CROSS_TREE;
	}
	if (location.type != BT_INODE_ITEM || location.offset != 0 ||
	    location.objectid < BTRFS_ROOT_INODE || location.objectid > BT_LAST_FREE_OBJECTID) {
		return BTRFS_CORRUPT;
	}
	entry->inode = location.objectid;
	entry->extended = 0;
	key = (struct bt_key){ entry->inode, directory, BT_INODE_REF };
	error = bt_ns_load(transaction, tree, key, &packed);
	if (error == BTRFS_OK) {
		error = bt_ns_ref_find(&packed, name, length, &offset, &entry->index);
	}
	if (error == BTRFS_NOT_FOUND) {
		/* A name beyond a full INODE_REF item. */
		entry->extended = 1;
		key = (struct bt_key){ entry->inode, bt_ns_extref_hash(directory, name, length),
			BT_INODE_EXTREF };
		error = bt_ns_load(transaction, tree, key, &packed);
		if (error == BTRFS_OK) {
			error = bt_ns_extref_find(
			    &packed, directory, name, length, &offset, &entry->index);
		}
	}
	if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_CORRUPT;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	key = (struct bt_key){ directory, entry->index, BT_DIR_INDEX };
	error = bt_ns_load(transaction, tree, key, &packed);
	if (error == BTRFS_OK) {
		error = bt_ns_dir_find(&packed, name, length, &offset, &entry_size, &header);
	}
	if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK &&
	    (offset != 0 || entry_size != packed.size ||
		bt_key_compare(bt_key_decode(&header->location), location) != 0 ||
		header->type != entry->type)) {
		error = BTRFS_CORRUPT;
	}
	return error;
}

/* Turns a lookup into a check that the name is free. */
enum btrfs_result
bt_ns_absent(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t directory,
    const void *name, size_t length)
{
	struct bt_entry entry;
	enum btrfs_result error;

	error = bt_ns_lookup(transaction, tree, directory, name, length, &entry);
	if (error == BTRFS_OK || error == BTRFS_CROSS_TREE) {
		return BTRFS_EXISTS;
	}
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* The bytes removed shares with the item of key: its entry leaves the item
 * before the new name is added. */
static size_t
bt_ns_freed(const struct bt_removed *removed, struct bt_key key, uint64_t inode)
{
	if (removed == NULL) {
		return 0;
	}
	switch (key.type) {
	case BT_DIR_ITEM:
		return removed->directory == key.objectid &&
			bt_ns_hash(removed->name, removed->length) == key.offset
		    ? sizeof(struct bt_disk_dir) + removed->length
		    : 0;
	case BT_INODE_REF:
		return !removed->extended && removed->directory == key.offset &&
			inode == key.objectid
		    ? sizeof(struct bt_disk_inode_ref) + removed->length
		    : 0;
	default:
		return removed->extended && inode == key.objectid &&
			bt_ns_extref_hash(removed->directory, removed->name, removed->length) ==
			    key.offset
		    ? sizeof(struct bt_disk_inode_extref) + removed->length
		    : 0;
	}
}

/* Where inode's back reference for a new name goes, as btrfs_insert_inode_ref
 * decides: the INODE_REF item while it has room, else an INODE_EXTREF item
 * when the filesystem has extended references (a full one is EOVERFLOW:
 * RANGE), else EMLINK. */
static enum btrfs_result
bt_ns_ref_place(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    uint64_t directory, const void *name, size_t length, uint64_t inode,
    const struct bt_removed *removed, struct bt_packed *packed)
{
	struct bt_key key = { inode, directory, BT_INODE_REF };
	size_t limit = bt_ns_item_limit(transaction);
	enum btrfs_result error;

	error = bt_ns_load(transaction, tree, key, packed);
	if (error != BTRFS_OK ||
	    packed->size - bt_ns_freed(removed, key, inode) + sizeof(struct bt_disk_inode_ref) +
		    length <=
		limit) {
		return error;
	}
	if (!bt_ns_extended(transaction)) {
		return BTRFS_TOO_MANY_LINKS;
	}
	key = (struct bt_key){ inode, bt_ns_extref_hash(directory, name, length), BT_INODE_EXTREF };
	error = bt_ns_load(transaction, tree, key, packed);
	if (error == BTRFS_OK &&
	    packed->size - bt_ns_freed(removed, key, inode) + sizeof(struct bt_disk_inode_extref) +
		    length >
		limit) {
		error = BTRFS_RANGE;
	}
	return error;
}

/* Checks before any change that the new name fits its packed DIR_ITEM (Linux
 * reports EOVERFLOW: RANGE) and, for inode != 0, a back reference item. */
enum btrfs_result
bt_ns_room(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t directory,
    const void *name, size_t length, int item, uint64_t inode, const struct bt_removed *removed)
{
	struct bt_packed packed;
	struct bt_key key = { directory, bt_ns_hash(name, length), BT_DIR_ITEM };
	enum btrfs_result error = BTRFS_OK;

	if (item) {
		error = bt_ns_load(transaction, tree, key, &packed);
		if (error == BTRFS_OK &&
		    packed.size - bt_ns_freed(removed, key, 0) + sizeof(struct bt_disk_dir) +
			    length >
			bt_ns_item_limit(transaction)) {
			error = BTRFS_RANGE;
		}
	}
	if (error == BTRFS_OK && inode != 0) {
		error = bt_ns_ref_place(
		    transaction, tree, directory, name, length, inode, removed, &packed);
	}
	return error;
}

/* Adds inode's back reference for name in directory where bt_ns_ref_place
 * puts it. */
enum btrfs_result
bt_ns_add_ref(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t directory,
    const void *name, size_t length, uint64_t inode, uint64_t index)
{
	struct bt_disk_inode_ref *ref = (void *)transaction->entry;
	struct bt_disk_inode_extref *extref = (void *)transaction->entry;
	struct bt_packed packed;
	uint64_t existing;
	size_t offset;
	size_t size;
	enum btrfs_result error;

	error = bt_ns_ref_place(transaction, tree, directory, name, length, inode, NULL, &packed);
	if (error != BTRFS_OK) {
		return error;
	}
	if (packed.key.type == BT_INODE_REF) {
		error = bt_ns_ref_find(&packed, name, length, &offset, &existing);
		bt_put64(&ref->index, index);
		bt_put16(&ref->name_length, (uint16_t)length);
		bt_copy(ref + 1, name, length);
		size = sizeof(*ref) + length;
	} else {
		error = bt_ns_extref_find(&packed, directory, name, length, &offset, &existing);
		bt_put64(&extref->parent, directory);
		bt_put64(&extref->index, index);
		bt_put16(&extref->name_length, (uint16_t)length);
		bt_copy(extref + 1, name, length);
		size = sizeof(*extref) + length;
	}
	if (error != BTRFS_NOT_FOUND) {
		return error == BTRFS_OK ? BTRFS_CORRUPT : error;
	}
	return bt_ns_append(transaction, tree, &packed, transaction->entry, size);
}

/* Inserts name's DIR_ITEM entry (appended to a colliding item) and its
 * DIR_INDEX item, both naming location. */
enum btrfs_result
bt_ns_insert_entry(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    uint64_t directory, const void *name, size_t length, struct bt_key location, uint8_t type,
    uint64_t index)
{
	struct bt_packed packed;
	struct bt_key key = { directory, bt_ns_hash(name, length), BT_DIR_ITEM };
	size_t entry_size;
	enum btrfs_result error;

	entry_size = bt_ns_dir_entry(
	    transaction->entry, location, bt_ns_transid(transaction), name, length, NULL, 0, type);
	error = bt_ns_load(transaction, tree, key, &packed);
	if (error == BTRFS_OK) {
		error = bt_ns_append(transaction, tree, &packed, transaction->entry, entry_size);
	}
	if (error == BTRFS_OK) {
		key = (struct bt_key){ directory, index, BT_DIR_INDEX };
		error = bt_tx_edit(
		    transaction, &tree->root, key, transaction->entry, entry_size, BT_INSERT);
		if (error == BTRFS_EXISTS) {
			error = BTRFS_CORRUPT;
		}
	}
	return error;
}

/* Adds name -> inode with a preallocated index: the back reference, DIR_ITEM
 * and DIR_INDEX, then the directory's size and times, as btrfs_add_link does. */
static enum btrfs_result
bt_ns_add_entry(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    uint64_t directory, const void *name, size_t length, uint64_t inode, uint8_t type,
    uint64_t index, struct btrfs_time time)
{
	struct bt_key location = { .objectid = inode, .type = BT_INODE_ITEM };
	enum btrfs_result error;

	error = bt_ns_add_ref(transaction, tree, directory, name, length, inode, index);
	if (error == BTRFS_OK) {
		error = bt_ns_insert_entry(
		    transaction, tree, directory, name, length, location, type, index);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_directory(transaction, tree, directory, length, 1, time);
	}
	return error;
}

/* Removes a resolved name: its DIR_ITEM entry, DIR_INDEX and back reference,
 * then the directory's size and times. */
enum btrfs_result
bt_ns_remove_entry(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    uint64_t directory, const void *name, size_t length, const struct bt_entry *entry,
    struct btrfs_time time)
{
	const struct bt_disk_dir *header;
	struct bt_packed packed;
	struct bt_key key = { directory, bt_ns_hash(name, length), BT_DIR_ITEM };
	uint64_t index;
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
	if (error == BTRFS_OK && !entry->extended) {
		key = (struct bt_key){ entry->inode, directory, BT_INODE_REF };
		error = bt_ns_load(transaction, tree, key, &packed);
		if (error == BTRFS_OK) {
			error = bt_ns_ref_find(&packed, name, length, &offset, &index);
		}
		if (error == BTRFS_OK) {
			error = bt_ns_cut(transaction, tree, &packed, offset,
			    sizeof(struct bt_disk_inode_ref) + length);
		}
	} else if (error == BTRFS_OK) {
		key = (struct bt_key){ entry->inode, bt_ns_extref_hash(directory, name, length),
			BT_INODE_EXTREF };
		error = bt_ns_load(transaction, tree, key, &packed);
		if (error == BTRFS_OK) {
			error =
			    bt_ns_extref_find(&packed, directory, name, length, &offset, &index);
		}
		if (error == BTRFS_OK) {
			error = bt_ns_cut(transaction, tree, &packed, offset,
			    sizeof(struct bt_disk_inode_extref) + length);
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

/* Tree nodes an unlink spends deleting its inode before leaving the rest to
 * eviction steps: one step's. */
#define BT_RELEASE_INLINE_NODES BTRFS_RELEASE_STEP_NODES

/* Deletes an inode without names in steps whose work reaches budget, as
 * Linux's eviction truncates: its file extents from the last one down with
 * their references, the size and bytes reached stored in its item; then its
 * other items, the inode item last. */
static enum btrfs_result
bt_ns_evict_inode(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t inode,
    struct bt_disk_inode *item, size_t budget, int *done)
{
	struct bt_key first = { .objectid = inode, .type = BT_INODE_ITEM + 1U };
	struct bt_key self = { .objectid = inode, .type = BT_INODE_ITEM };
	struct bt_key key;
	size_t start = bt_tx_work(transaction);
	uint64_t removed = 0;
	uint64_t reached = bt_u64(item->size);
	uint64_t steps;
	int shrunk;
	int found = 1;
	enum btrfs_result error;

	*done = 0;
	error = bt_tx_shrink(transaction, tree, inode, 0, budget, &removed, &reached, &shrunk);
	if (error == BTRFS_OK && bt_u64(item->nbytes) < removed) {
		error = BTRFS_CORRUPT;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	bt_put64(&item->nbytes, bt_u64(item->nbytes) - removed);
	if (reached < bt_u64(item->size)) {
		bt_put64(&item->size, reached);
	}
	for (steps = 0; error == BTRFS_OK && shrunk; steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			return BTRFS_UNSUPPORTED;
		}
		if (bt_tx_work(transaction) - start >= budget) {
			break;
		}
		error = bt_ns_neighbor(transaction, tree, first, 0, &key, &found);
		if (error != BTRFS_OK || !found || key.objectid != inode) {
			*done = error == BTRFS_OK;
			break;
		}
		/* Names were removed first; a remaining one is corruption. */
		if (key.type == BT_INODE_REF || key.type == BT_INODE_EXTREF ||
		    key.type == BT_DIR_ITEM || key.type == BT_DIR_INDEX) {
			return BTRFS_CORRUPT;
		}
		error = bt_tx_edit(transaction, &tree->root, key, NULL, 0, BT_DELETE);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	transaction->changed = 1;
	if (*done) {
		return bt_tx_edit(transaction, &tree->root, self, NULL, 0, BT_DELETE);
	}
	/* Work remains: the item records how far the deletion got. */
	bt_put64(&item->transid, bt_ns_transid(transaction));
	return bt_tx_edit(transaction, &tree->root, self, item, sizeof(*item), BT_REPLACE);
}

/* An inode loses one name. Without a name left it is deleted now when that
 * fits a small budget; otherwise, or while still open, an orphan item keeps
 * it, and an unopened one is left to btrfs_transaction_evict's steps. */
enum btrfs_result
bt_ns_release(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t inode,
    int open, struct btrfs_time time)
{
	struct bt_disk_inode item;
	struct bt_key orphan = { BT_ORPHAN_OBJECTID, inode, BT_ORPHAN_ITEM };
	uint32_t links;
	int done = 0;
	enum btrfs_result error;

	error = bt_ns_named_inode(transaction, tree, inode, &item);
	if (error != BTRFS_OK) {
		return error;
	}
	links = bt_u32(item.links);
	if (links == 0) {
		return BTRFS_CORRUPT;
	}
	bt_put32(&item.links, links - 1);
	if (links == 1 && !open) {
		error = bt_ns_evict_inode(
		    transaction, tree, inode, &item, BT_RELEASE_INLINE_NODES, &done);
		if (error != BTRFS_OK || done) {
			return error;
		}
		if (transaction->deferred_count == BT_TRANSACTION_DEFERRED) {
			return BTRFS_NO_SPACE;
		}
		transaction->deferred[transaction->deferred_count++] =
		    (struct btrfs_object_id){ tree->root.owner, inode };
	}
	error = bt_ns_store(transaction, tree, inode, &item, time);
	if (error == BTRFS_OK && links == 1) {
		error = bt_tx_edit(transaction, &tree->root, orphan, NULL, 0, BT_INSERT);
		if (error == BTRFS_EXISTS) {
			error = BTRFS_CORRUPT;
		}
	}
	return error;
}

static const struct bt_codec *
bt_ns_codec(const void *value, size_t length)
{
	size_t name;
	size_t i;

	for (i = 0; i < sizeof(bt_ns_codecs) / sizeof(bt_ns_codecs[0]); i++) {
		name = bt_ns_length(bt_ns_codecs[i].name);
		if (length >= name && bt_equal(value, bt_ns_codecs[i].name, name)) {
			return &bt_ns_codecs[i];
		}
	}
	return NULL;
}

static int
bt_ns_no_compression(const void *value, size_t length)
{
	return (length == 2 && bt_equal(value, "no", 2)) ||
	    (length == 4 && bt_equal(value, "none", 4));
}

/* Compression applies to regular files and directories (for inheritance);
 * the property is ignored elsewhere and invalid without data checksums. */
static int
bt_ns_compressible_type(const struct bt_disk_inode *item)
{
	uint32_t type = bt_u32(item->mode) & BTRFS_MODE_TYPE;

	return type == BTRFS_MODE_REGULAR || type == BTRFS_MODE_DIRECTORY;
}

int
bt_ns_can_compress(uint64_t flags)
{
	return (flags & (BT_INODE_NODATACOW | BT_INODE_NODATASUM)) == 0;
}

/* Applying a codec property records the codec's incompat feature, as Linux's
 * btrfs_set_fs_incompat does. */
void
bt_ns_require_feature(struct btrfs_transaction *transaction, uint64_t feature)
{
	uint64_t incompat = bt_u64(transaction->super.incompat);

	if (feature != 0 && (incompat & feature) == 0) {
		bt_put64(&transaction->super.incompat, incompat | feature);
		transaction->changed = 1;
	}
}

/* The codec a directory's valid compression property passes to new regular
 * files and directories, as btrfs_inode_inherit_props does. */
enum btrfs_result
bt_ns_inherited_codec(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    uint64_t directory, uint64_t flags, const struct bt_codec **codec)
{
	const struct bt_disk_dir *header;
	struct bt_packed packed;
	size_t length = bt_ns_length(BT_COMPRESSION_PROPERTY);
	struct bt_key key = { directory, bt_ns_hash(BT_COMPRESSION_PROPERTY, length),
		BT_XATTR_ITEM };
	size_t offset;
	size_t entry_size;
	enum btrfs_result error;

	*codec = NULL;
	error = bt_ns_load(transaction, tree, key, &packed);
	if (error == BTRFS_OK && packed.present) {
		error = bt_ns_dir_find(
		    &packed, BT_COMPRESSION_PROPERTY, length, &offset, &entry_size, &header);
		if (error == BTRFS_OK && bt_ns_can_compress(flags)) {
			*codec = bt_ns_codec(
			    (const uint8_t *)(header + 1) + length, bt_u16(header->data_length));
		}
		if (error == BTRFS_NOT_FOUND) {
			error = BTRFS_OK;
		}
	}
	return error;
}

static uint64_t
bt_ns_inherit(uint64_t parent, uint32_t mode)
{
	uint64_t flags = 0;

	if ((parent & BT_INODE_NOCOMPRESS) != 0) {
		flags |= BT_INODE_NOCOMPRESS;
	} else if ((parent & BT_INODE_COMPRESS) != 0) {
		flags |= BT_INODE_COMPRESS;
	}
	if ((parent & BT_INODE_NODATACOW) != 0) {
		flags |= BT_INODE_NODATACOW;
		if ((mode & BTRFS_MODE_TYPE) == BTRFS_MODE_REGULAR) {
			flags |= BT_INODE_NODATASUM;
		}
	}
	return flags;
}

/* The namespace buffers, allocated before the first change that needs them. */
static enum btrfs_result
bt_ns_buffers(struct btrfs_transaction *transaction)
{
	const struct btrfs_environment *env = &transaction->base->env;

	if (transaction->item == NULL) {
		transaction->item = env->allocate(env->context, transaction->base->info.node_size);
	}
	if (transaction->entry == NULL) {
		transaction->entry = env->allocate(env->context, transaction->base->info.node_size);
	}
	return transaction->item == NULL || transaction->entry == NULL ? BTRFS_NO_MEMORY : BTRFS_OK;
}

/* Opens the operation's tree. */
enum btrfs_result
bt_ns_begin(struct btrfs_transaction *transaction, uint64_t tree_id, struct bt_owned_root **tree)
{
	enum btrfs_result error;

	if (transaction == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (transaction->failure != BTRFS_OK || transaction->finished) {
		return transaction->failure == BTRFS_OK ? BTRFS_READ_ONLY : transaction->failure;
	}
	error = bt_ns_buffers(transaction);
	return error == BTRFS_OK ? bt_tx_tree(transaction, tree_id, tree) : error;
}

/* Records a failure after the first change: the private trees may hold part
 * of the operation, so the transaction can no longer commit. */
enum btrfs_result
bt_ns_poison(struct btrfs_transaction *transaction, enum btrfs_result error)
{
	if (error != BTRFS_OK) {
		transaction->failure = error;
	}
	return error;
}

/* Validates a new inode's attributes: a known type, permission bits only,
 * and a target exactly for symlinks. */
static enum btrfs_result
bt_ns_check_new(const struct btrfs_new_inode *attributes, uint8_t *type)
{
	if (attributes == NULL || attributes->time.nanoseconds >= BT_NANOSECONDS) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*type = bt_ns_type(attributes->mode);
	if (*type == BTRFS_FT_UNKNOWN ||
	    (attributes->mode & ~(uint32_t)(BTRFS_MODE_TYPE | BT_MODE_PERMISSIONS)) != 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (*type == BTRFS_FT_SYMLINK
		? attributes->target == NULL || attributes->target_length == 0
		: attributes->target != NULL || attributes->target_length != 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	return BTRFS_OK;
}

/* A new inode decided before any change: its number, inherited flags and
 * compression property. */
struct bt_new_inode {
	uint64_t inode;
	uint64_t flags;
	const struct bt_codec *codec;
};

/* Linux's btrfs_create_new_inode decisions: the parent's inheritable flags
 * and compression property, and the next inode number. */
static enum btrfs_result
bt_ns_plan_inode(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t parent,
    const struct bt_disk_inode *directory, const struct btrfs_new_inode *attributes,
    struct bt_new_inode *plan)
{
	uint8_t type = bt_ns_type(attributes->mode);
	enum btrfs_result error = BTRFS_OK;

	plan->codec = NULL;
	plan->inode = 0;
	plan->flags = bt_ns_inherit(bt_u64(directory->flags), attributes->mode);
	if (type == BTRFS_FT_REGULAR || type == BTRFS_FT_DIRECTORY) {
		error = bt_ns_inherited_codec(
		    transaction, tree, parent, bt_u64(directory->flags), &plan->codec);
	}
	if (plan->codec != NULL && !bt_ns_can_compress(plan->flags)) {
		plan->codec = NULL;
	}
	if (plan->codec != NULL) {
		plan->flags = (plan->flags & ~BT_INODE_NOCOMPRESS) | BT_INODE_COMPRESS;
	}
	if (error == BTRFS_OK) {
		error = bt_ns_objectid(transaction, tree, &plan->inode);
	}
	return error;
}

/* Writes the planned inode as btrfs_create_new_inode does: links links, all
 * four times now, the generation of this transaction, and a symlink's target
 * as an inline extent. */
static enum btrfs_result
bt_ns_make_inode(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    const struct btrfs_new_inode *attributes, uint32_t links, const struct bt_new_inode *plan)
{
	struct bt_disk_inode item;
	struct bt_disk_extent_header *extent;
	struct bt_key key;
	struct bt_key none = { 0, 0, 0 };
	uint64_t transid = bt_ns_transid(transaction);
	uint64_t inode = plan->inode;
	size_t size;
	uint8_t type = bt_ns_type(attributes->mode);
	enum btrfs_result error;

	bt_zero(&item, sizeof(item));
	bt_put64(&item.generation, transid);
	bt_put64(&item.transid, transid);
	bt_put64(&item.size, attributes->target_length);
	bt_put64(&item.nbytes, attributes->target_length);
	bt_put32(&item.links, links);
	bt_put32(&item.uid, attributes->uid);
	bt_put32(&item.gid, attributes->gid);
	bt_put32(&item.mode, attributes->mode);
	if (type == BTRFS_FT_CHARACTER || type == BTRFS_FT_BLOCK) {
		bt_put64(&item.device, attributes->device);
	}
	bt_put64(&item.flags, plan->flags);
	bt_put64(&item.atime.seconds, (uint64_t)attributes->time.seconds);
	bt_put32(&item.atime.nanoseconds, attributes->time.nanoseconds);
	item.ctime = item.atime;
	item.mtime = item.atime;
	item.otime = item.atime;
	key = (struct bt_key){ .objectid = inode, .type = BT_INODE_ITEM };
	error = bt_tx_edit(transaction, &tree->root, key, &item, sizeof(item), BT_INSERT);
	if (error == BTRFS_OK && type == BTRFS_FT_SYMLINK) {
		extent = (void *)transaction->entry;
		bt_zero(extent, sizeof(*extent));
		bt_put64(&extent->generation, transid);
		bt_put64(&extent->ram_bytes, attributes->target_length);
		extent->type = BT_EXTENT_INLINE;
		bt_copy(extent + 1, attributes->target, attributes->target_length);
		key = (struct bt_key){ .objectid = inode, .type = BT_EXTENT_DATA };
		error = bt_tx_edit(transaction, &tree->root, key, extent,
		    sizeof(*extent) + attributes->target_length, BT_INSERT);
	}
	if (error == BTRFS_OK && plan->codec != NULL) {
		size = bt_ns_length(BT_COMPRESSION_PROPERTY);
		key = (struct bt_key){ inode, bt_ns_hash(BT_COMPRESSION_PROPERTY, size),
			BT_XATTR_ITEM };
		size = bt_ns_dir_entry(transaction->entry, none, transid, BT_COMPRESSION_PROPERTY,
		    size, plan->codec->name, bt_ns_length(plan->codec->name), BTRFS_FT_XATTR);
		error =
		    bt_tx_edit(transaction, &tree->root, key, transaction->entry, size, BT_INSERT);
		bt_ns_require_feature(transaction, plan->codec->feature);
	}
	return error;
}

enum btrfs_result
btrfs_transaction_create(struct btrfs_transaction *transaction, struct btrfs_object_id parent,
    const void *name, size_t length, const struct btrfs_new_inode *attributes,
    struct btrfs_object_id *result)
{
	struct bt_owned_root *tree = NULL;
	struct bt_disk_inode directory;
	struct bt_new_inode plan;
	uint64_t index = 0;
	uint8_t type = BTRFS_FT_UNKNOWN;
	enum btrfs_result error;

	if (result == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_ns_check_new(attributes, &type);
	if (error != BTRFS_OK) {
		return error;
	}
	error = bt_ns_name(name, length);
	if (error == BTRFS_OK) {
		error = bt_ns_begin(transaction, parent.tree, &tree);
	}
	if (error == BTRFS_OK && type == BTRFS_FT_SYMLINK &&
	    (attributes->target_length > BT_SYMLINK_LIMIT ||
		attributes->target_length >
		    bt_ns_item_limit(transaction) - sizeof(struct bt_disk_extent_header))) {
		error = BTRFS_NAME_TOO_LONG;
	}
	if (error == BTRFS_OK) {
		error = bt_ns_parent(transaction, tree, parent.inode, &directory);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_absent(transaction, tree, parent.inode, name, length);
	}
	if (error == BTRFS_OK && bt_ns_immutable(&directory)) {
		error = BTRFS_NOT_PERMITTED;
	}
	if (error == BTRFS_OK) {
		error = bt_ns_room(transaction, tree, parent.inode, name, length, 1, 0, NULL);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_index(transaction, tree, parent.inode, &index);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_plan_inode(
		    transaction, tree, parent.inode, &directory, attributes, &plan);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	error = bt_ns_make_inode(transaction, tree, attributes, 1, &plan);
	if (error == BTRFS_OK) {
		error = bt_ns_add_entry(transaction, tree, parent.inode, name, length, plan.inode,
		    type, index, attributes->time);
	}
	if (error == BTRFS_OK) {
		if (type == BTRFS_FT_DIRECTORY) {
			bt_ns_new_directory(transaction, tree->root.owner, plan.inode);
		}
		transaction->changed = 1;
		result->tree = parent.tree;
		result->inode = plan.inode;
	}
	return bt_ns_poison(transaction, error);
}

/* Linux's btrfs_tmpfile: a regular file without a name, inheriting from
 * parent as a created one does, with no link and an orphan item; the parent
 * directory does not change. */
enum btrfs_result
btrfs_transaction_create_tmpfile(struct btrfs_transaction *transaction,
    struct btrfs_object_id parent, const struct btrfs_new_inode *attributes,
    struct btrfs_object_id *result)
{
	struct bt_owned_root *tree = NULL;
	struct bt_disk_inode directory;
	struct bt_new_inode plan;
	struct bt_key orphan = { BT_ORPHAN_OBJECTID, 0, BT_ORPHAN_ITEM };
	uint8_t type = BTRFS_FT_UNKNOWN;
	enum btrfs_result error;

	if (result == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_ns_check_new(attributes, &type);
	if (error == BTRFS_OK && type != BTRFS_FT_REGULAR) {
		error = BTRFS_INVALID_ARGUMENT;
	}
	if (error == BTRFS_OK) {
		error = bt_ns_begin(transaction, parent.tree, &tree);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_parent(transaction, tree, parent.inode, &directory);
	}
	if (error == BTRFS_OK && bt_ns_immutable(&directory)) {
		error = BTRFS_NOT_PERMITTED;
	}
	if (error == BTRFS_OK) {
		error = bt_ns_plan_inode(
		    transaction, tree, parent.inode, &directory, attributes, &plan);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	error = bt_ns_make_inode(transaction, tree, attributes, 0, &plan);
	if (error == BTRFS_OK) {
		orphan.offset = plan.inode;
		error = bt_tx_edit(transaction, &tree->root, orphan, NULL, 0, BT_INSERT);
		error = error == BTRFS_EXISTS ? BTRFS_CORRUPT : error;
	}
	if (error == BTRFS_OK) {
		transaction->changed = 1;
		result->tree = parent.tree;
		result->inode = plan.inode;
	}
	return bt_ns_poison(transaction, error);
}

/* A new name for an inode, as btrfs_link adds it. A file created with
 * O_TMPFILE (tmpfile) has no link and an orphan item; its first name removes
 * the orphan item. Other inodes without links cannot gain a name. */
static enum btrfs_result
bt_ns_link(struct btrfs_transaction *transaction, struct btrfs_object_id id,
    struct btrfs_object_id parent, const void *name, size_t length, struct btrfs_time time,
    int tmpfile)
{
	struct bt_owned_root *tree = NULL;
	struct bt_owned_root view;
	struct bt_disk_inode directory;
	struct bt_disk_inode item;
	struct bt_key orphan = { BT_ORPHAN_OBJECTID, id.inode, BT_ORPHAN_ITEM };
	uint8_t marker[1];
	size_t size;
	uint64_t index = 0;
	enum btrfs_result error;

	if (time.nanoseconds >= BT_NANOSECONDS) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_ns_name(name, length);
	if (error == BTRFS_OK) {
		error = bt_ns_begin(transaction, parent.tree, &tree);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_parent(transaction, tree, parent.inode, &directory);
	}
	/* An inode of another subvolume is read without opening its tree. */
	if (error == BTRFS_OK && id.tree != parent.tree) {
		error = bt_tx_tree_view(transaction, id.tree, &view);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_inode(
		    transaction, id.tree != parent.tree ? &view : tree, id.inode, &item);
	}
	/* vfs_link's order: may_create (a free name, a mutable directory), an
	 * inode that may change, no directory, a name only for an inode that
	 * has one or a tmpfile's first; then btrfs_link's EXDEV and EMLINK. */
	if (error == BTRFS_OK) {
		error = bt_ns_absent(transaction, tree, parent.inode, name, length);
	}
	if (error == BTRFS_OK && (bt_ns_immutable(&directory) || bt_ns_frozen(&item))) {
		error = BTRFS_NOT_PERMITTED;
	}
	if (error == BTRFS_OK) {
		if (bt_ns_is_directory(&item)) {
			error = BTRFS_IS_DIRECTORY;
		} else if (tmpfile != (bt_u32(item.links) == 0)) {
			/* An unlinked inode kept open cannot gain a name; only a
			 * tmpfile's first name goes through the tmpfile path. */
			error = tmpfile ? BTRFS_INVALID_ARGUMENT : BTRFS_NOT_FOUND;
		} else if (id.tree != parent.tree) {
			error = BTRFS_CROSS_TREE;
		} else if (bt_u32(item.links) >= BT_LINK_MAX) {
			error = BTRFS_TOO_MANY_LINKS;
		}
	}
	if (error == BTRFS_OK && tmpfile) {
		error = bt_mutation_find(
		    transaction->mutation, tree->root, orphan, marker, sizeof(marker), &size);
		error = error == BTRFS_NOT_FOUND     ? BTRFS_INVALID_ARGUMENT
		    : error == BTRFS_OK && size != 0 ? BTRFS_CORRUPT
						     : error;
	}
	if (error == BTRFS_OK) {
		error =
		    bt_ns_room(transaction, tree, parent.inode, name, length, 1, id.inode, NULL);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_index(transaction, tree, parent.inode, &index);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	error = bt_ns_add_entry(transaction, tree, parent.inode, name, length, id.inode,
	    bt_ns_type(bt_u32(item.mode)), index, time);
	if (error == BTRFS_OK) {
		error = bt_ns_named_inode(transaction, tree, id.inode, &item);
	}
	if (error == BTRFS_OK) {
		bt_put32(&item.links, bt_u32(item.links) + 1);
		error = bt_ns_store(transaction, tree, id.inode, &item, time);
	}
	if (error == BTRFS_OK && tmpfile) {
		error = bt_tx_edit(transaction, &tree->root, orphan, NULL, 0, BT_DELETE);
	}
	return bt_ns_poison(transaction, error);
}

enum btrfs_result
btrfs_transaction_link(struct btrfs_transaction *transaction, struct btrfs_object_id id,
    struct btrfs_object_id parent, const void *name, size_t length, struct btrfs_time time)
{
	return bt_ns_link(transaction, id, parent, name, length, time, 0);
}

enum btrfs_result
btrfs_transaction_link_tmpfile(struct btrfs_transaction *transaction, struct btrfs_object_id id,
    struct btrfs_object_id parent, const void *name, size_t length, struct btrfs_time time)
{
	return bt_ns_link(transaction, id, parent, name, length, time, 1);
}

enum btrfs_result
btrfs_transaction_unlink(struct btrfs_transaction *transaction, struct btrfs_object_id parent,
    const void *name, size_t length, struct btrfs_time time, int open)
{
	struct bt_owned_root *tree = NULL;
	struct bt_disk_inode directory;
	struct bt_disk_inode item;
	struct bt_entry entry;
	enum btrfs_result error;

	if (time.nanoseconds >= BT_NANOSECONDS) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_ns_name(name, length);
	if (error == BTRFS_OK) {
		error = bt_ns_begin(transaction, parent.tree, &tree);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_parent(transaction, tree, parent.inode, &directory);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_lookup(transaction, tree, parent.inode, name, length, &entry);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_named_inode(transaction, tree, entry.inode, &item);
	}
	if (error == BTRFS_OK && (bt_ns_frozen(&directory) || bt_ns_frozen(&item))) {
		error = BTRFS_NOT_PERMITTED;
	}
	if (error == BTRFS_OK && bt_ns_is_directory(&item) && bt_u64(item.size) != 0) {
		error = BTRFS_NOT_EMPTY;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	error = bt_ns_remove_entry(transaction, tree, parent.inode, name, length, &entry, time);
	if (error == BTRFS_OK) {
		error = bt_ns_release(transaction, tree, entry.inode, open, time);
	}
	return bt_ns_poison(transaction, error);
}

/* The btrfs. namespace holds Linux's properties; only btrfs.compression exists. */
static enum btrfs_result
bt_ns_property(const void *name, size_t name_length, int *property)
{
	size_t prefix = bt_ns_length(BT_PROPERTY_PREFIX);
	size_t length = bt_ns_length(BT_COMPRESSION_PROPERTY);

	*property = name_length >= prefix && bt_equal(name, BT_PROPERTY_PREFIX, prefix);
	if (*property &&
	    (name_length != length || !bt_equal(name, BT_COMPRESSION_PROPERTY, length))) {
		return BTRFS_INVALID_ARGUMENT;
	}
	return BTRFS_OK;
}

/* Linux validates a compression value before anything else (a codec or
 * "no"/"none" on an inode with data checksums), then ignores the property on
 * objects other than regular files and directories. */
static enum btrfs_result
bt_ns_check_property(const struct bt_disk_inode *item, const void *value, size_t value_length,
    const struct bt_codec **codec, int *ignore)
{
	*codec = NULL;
	if (value_length != 0) {
		*codec = bt_ns_codec(value, value_length);
		if (!bt_ns_can_compress(bt_u64(item->flags)) ||
		    (*codec == NULL && !bt_ns_no_compression(value, value_length))) {
			return BTRFS_INVALID_ARGUMENT;
		}
	}
	*ignore = !bt_ns_compressible_type(item);
	return BTRFS_OK;
}

/* Sets the compression flags a property value means; clear removes both. */
static void
bt_ns_apply_property(struct btrfs_transaction *transaction, struct bt_disk_inode *item,
    const struct bt_codec *codec, int clear)
{
	uint64_t flags = bt_u64(item->flags);

	if (clear) {
		flags &= ~(BT_INODE_COMPRESS | BT_INODE_NOCOMPRESS);
	} else if (codec != NULL) {
		flags = (flags & ~BT_INODE_NOCOMPRESS) | BT_INODE_COMPRESS;
		bt_ns_require_feature(transaction, codec->feature);
	} else {
		flags = (flags & ~BT_INODE_COMPRESS) | BT_INODE_NOCOMPRESS;
	}
	bt_put64(&item->flags, flags);
}

/* Sets (value) or removes (remove) name among inode's xattrs; clear removes
 * the name without requiring it. Refusals change nothing; a failure after the
 * first change poisons the transaction. */
static enum btrfs_result
bt_ns_edit_xattr(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t inode,
    const void *name, size_t name_length, const void *value, size_t value_length, int flags,
    int remove, int clear)
{
	const struct bt_disk_dir *header;
	struct bt_packed packed;
	struct bt_key location = { 0, 0, 0 };
	size_t limit = bt_ns_item_limit(transaction);
	size_t offset = 0;
	size_t entry_size = 0;
	int found = 0;
	enum btrfs_result error;

	/* Linux limits a name and value to one item's payload. */
	if (!remove && name_length + value_length > limit - sizeof(*header)) {
		return BTRFS_NO_SPACE;
	}
	error = bt_ns_load(transaction, tree,
	    (struct bt_key){ inode, bt_ns_hash(name, name_length), BT_XATTR_ITEM }, &packed);
	if (error == BTRFS_OK && packed.present) {
		error = bt_ns_dir_find(&packed, name, name_length, &offset, &entry_size, &header);
		found = error == BTRFS_OK;
		if (error == BTRFS_NOT_FOUND) {
			error = BTRFS_OK;
		}
	}
	if (error == BTRFS_OK && !found && (remove || (flags & BTRFS_XATTR_REPLACE) != 0)) {
		error = BTRFS_NOT_FOUND;
	}
	if (error == BTRFS_OK && found && (flags & BTRFS_XATTR_CREATE) != 0) {
		error = BTRFS_EXISTS;
	}
	/* Colliding names share the item; the new entry must fit beside them. */
	if (error == BTRFS_OK && !clear && !remove &&
	    packed.size - (found ? entry_size : 0) + sizeof(*header) + name_length + value_length >
		limit) {
		error = BTRFS_NO_SPACE;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	if (found) {
		error = bt_ns_cut(transaction, tree, &packed, offset, entry_size);
	}
	if (error == BTRFS_OK && !remove && !clear) {
		entry_size =
		    bt_ns_dir_entry(transaction->entry, location, bt_ns_transid(transaction), name,
			name_length, value, value_length, BTRFS_FT_XATTR);
		error = bt_ns_append(transaction, tree, &packed, transaction->entry, entry_size);
	}
	return error == BTRFS_OK ? BTRFS_OK : bt_ns_poison(transaction, error);
}

/* Sets (value) or removes (value NULL with remove) one xattr. An empty
 * compression property removes the property without requiring it to exist. */
static enum btrfs_result
bt_ns_xattr(struct btrfs_transaction *transaction, struct btrfs_object_id id, const void *name,
    size_t name_length, const void *value, size_t value_length, int flags, int remove,
    struct btrfs_time time)
{
	const struct bt_codec *codec = NULL;
	struct bt_owned_root *tree = NULL;
	struct bt_disk_inode item;
	int property = 0;
	int ignore = 0;
	int clear;
	enum btrfs_result error;

	if ((value == NULL && value_length != 0) || time.nanoseconds >= BT_NANOSECONDS ||
	    (flags & ~(BTRFS_XATTR_CREATE | BTRFS_XATTR_REPLACE)) != 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (name != NULL && name_length > BTRFS_NAME_MAX) {
		return BTRFS_RANGE;
	}
	if (!bt_xattr_name_valid(name, name_length)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_ns_property(name, name_length, &property);
	if (error == BTRFS_OK) {
		error = bt_ns_begin(transaction, id.tree, &tree);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_inode(transaction, tree, id.inode, &item);
	}
	if (error == BTRFS_OK && bt_ns_frozen(&item)) {
		error = BTRFS_NOT_PERMITTED;
	}
	if (error == BTRFS_OK && property) {
		error =
		    bt_ns_check_property(&item, value, remove ? 0 : value_length, &codec, &ignore);
	}
	if (error != BTRFS_OK || ignore) {
		return error;
	}
	clear = property && (remove || value_length == 0);
	error = bt_ns_edit_xattr(transaction, tree, id.inode, name, name_length, value,
	    value_length, flags, remove, clear);
	if (error != BTRFS_OK) {
		return error;
	}
	if (property) {
		bt_ns_apply_property(transaction, &item, codec, clear);
	}
	return bt_ns_poison(transaction, bt_ns_store(transaction, tree, id.inode, &item, time));
}

/* Linux's btrfs_mask_fsflags_for_type. */
static unsigned
bt_ns_mask_fsflags(const struct bt_disk_inode *item, unsigned flags)
{
	uint32_t type = bt_u32(item->mode) & BTRFS_MODE_TYPE;

	if (type == BTRFS_MODE_DIRECTORY) {
		return flags;
	}
	if (type == BTRFS_MODE_REGULAR) {
		return flags & ~BTRFS_FS_DIRSYNC_FL;
	}
	return flags & (BTRFS_FS_NODUMP_FL | BTRFS_FS_NOATIME_FL);
}

/* Linux's check_fsflags. */
static enum btrfs_result
bt_ns_check_fsflags(unsigned old_flags, unsigned flags)
{
	const unsigned known = BTRFS_FS_IMMUTABLE_FL | BTRFS_FS_APPEND_FL | BTRFS_FS_NOATIME_FL |
	    BTRFS_FS_NODUMP_FL | BTRFS_FS_SYNC_FL | BTRFS_FS_DIRSYNC_FL | BTRFS_FS_NOCOMP_FL |
	    BTRFS_FS_COMPR_FL | BTRFS_FS_NOCOW_FL;

	if ((flags & ~known) != 0) {
		return BTRFS_UNSUPPORTED;
	}
	if (((flags & BTRFS_FS_NOCOMP_FL) && (flags & BTRFS_FS_COMPR_FL)) ||
	    ((flags & BTRFS_FS_COMPR_FL) && (flags & BTRFS_FS_NOCOW_FL)) ||
	    ((old_flags & BTRFS_FS_NOCOW_FL) &&
		(flags & (BTRFS_FS_COMPR_FL | BTRFS_FS_NOCOMP_FL))) ||
	    ((flags & BTRFS_FS_NOCOW_FL) &&
		(old_flags & (BTRFS_FS_COMPR_FL | BTRFS_FS_NOCOMP_FL)))) {
		return BTRFS_INVALID_ARGUMENT;
	}
	return BTRFS_OK;
}

static uint64_t
bt_ns_flag(uint64_t flags, uint64_t bit, unsigned set)
{
	return set != 0 ? flags | bit : flags & ~bit;
}

enum btrfs_result
btrfs_transaction_set_fsflags(struct btrfs_transaction *transaction, struct btrfs_object_id id,
    unsigned fsflags, struct btrfs_time time)
{
	const struct bt_codec *codec = NULL;
	struct bt_owned_root *tree = NULL;
	struct btrfs_inode current;
	struct bt_disk_inode item;
	uint64_t flags;
	uint32_t type;
	size_t i;
	enum btrfs_result error;

	if (transaction == NULL || time.nanoseconds >= BT_NANOSECONDS) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_ns_begin(transaction, id.tree, &tree);
	if (error == BTRFS_OK) {
		error = bt_ns_inode(transaction, tree, id.inode, &item);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	type = bt_u32(item.mode) & BTRFS_MODE_TYPE;
	fsflags = bt_ns_mask_fsflags(&item, fsflags);
	bt_zero(&current, sizeof(current));
	current.flags = bt_u64(item.flags);
	error = bt_ns_check_fsflags(btrfs_inode_fsflags(&current), fsflags);
	if (error != BTRFS_OK) {
		return error;
	}
	flags = bt_u64(item.flags);
	flags = bt_ns_flag(flags, BT_INODE_SYNC, fsflags & BTRFS_FS_SYNC_FL);
	flags = bt_ns_flag(flags, BT_INODE_IMMUTABLE, fsflags & BTRFS_FS_IMMUTABLE_FL);
	flags = bt_ns_flag(flags, BT_INODE_APPEND, fsflags & BTRFS_FS_APPEND_FL);
	flags = bt_ns_flag(flags, BT_INODE_NODUMP, fsflags & BTRFS_FS_NODUMP_FL);
	flags = bt_ns_flag(flags, BT_INODE_NOATIME, fsflags & BTRFS_FS_NOATIME_FL);
	flags = bt_ns_flag(flags, BT_INODE_DIRSYNC, fsflags & BTRFS_FS_DIRSYNC_FL);
	/* A regular file changes copy-on-write only while it has no extents. */
	if (type != BTRFS_MODE_REGULAR) {
		flags = bt_ns_flag(flags, BT_INODE_NODATACOW, fsflags & BTRFS_FS_NOCOW_FL);
	} else if (bt_u64(item.size) == 0) {
		flags = bt_ns_flag(flags, BT_INODE_NODATACOW | BT_INODE_NODATASUM_FLAG,
		    fsflags & BTRFS_FS_NOCOW_FL);
	}
	if ((fsflags & BTRFS_FS_NOCOMP_FL) != 0) {
		flags = (flags & ~BT_INODE_COMPRESS) | BT_INODE_NOCOMPRESS;
	} else if ((fsflags & BTRFS_FS_COMPR_FL) != 0) {
		flags = (flags & ~BT_INODE_NOCOMPRESS) | BT_INODE_COMPRESS;
		/* The mount's codec, zlib without one, as the property records it. */
		codec = &bt_ns_codecs[0];
		for (i = 0; i < sizeof(bt_ns_codecs) / sizeof(bt_ns_codecs[0]); i++) {
			if (bt_ns_codecs[i].codec == transaction->io.compression) {
				codec = &bt_ns_codecs[i];
			}
		}
		/* prop_compression_validate, on the flags before the change. */
		if (!bt_ns_can_compress(bt_u64(item.flags))) {
			return BTRFS_INVALID_ARGUMENT;
		}
	} else {
		flags &= ~(BT_INODE_COMPRESS | BT_INODE_NOCOMPRESS);
	}
	error = bt_ns_edit_xattr(transaction, tree, id.inode, BT_COMPRESSION_PROPERTY,
	    bt_ns_length(BT_COMPRESSION_PROPERTY), codec != NULL ? codec->name : NULL,
	    codec != NULL ? bt_ns_length(codec->name) : 0, 0, 0, codec == NULL);
	if (error != BTRFS_OK) {
		return error;
	}
	if (codec != NULL) {
		bt_ns_require_feature(transaction, codec->feature);
	}
	bt_put64(&item.flags, flags);
	return bt_ns_poison(transaction, bt_ns_store(transaction, tree, id.inode, &item, time));
}

enum btrfs_result
btrfs_transaction_set_xattr(struct btrfs_transaction *transaction, struct btrfs_object_id id,
    const void *name, size_t name_length, const void *value, size_t value_length, int flags,
    struct btrfs_time time)
{
	return bt_ns_xattr(transaction, id, name, name_length, value, value_length, flags, 0, time);
}

enum btrfs_result
btrfs_transaction_remove_xattr(struct btrfs_transaction *transaction, struct btrfs_object_id id,
    const void *name, size_t name_length, struct btrfs_time time)
{
	return bt_ns_xattr(transaction, id, name, name_length, NULL, 0, 0, 1, time);
}

enum btrfs_result
btrfs_transaction_evict(
    struct btrfs_transaction *transaction, struct btrfs_object_id id, size_t budget, int *done)
{
	struct bt_owned_root *tree = NULL;
	struct bt_disk_inode item;
	struct bt_packed packed;
	struct bt_key orphan = { BT_ORPHAN_OBJECTID, id.inode, BT_ORPHAN_ITEM };
	enum btrfs_result error;

	if (done == NULL || budget == 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*done = 0;
	error = bt_ns_begin(transaction, id.tree, &tree);
	if (error == BTRFS_OK) {
		error = bt_ns_inode(transaction, tree, id.inode, &item);
	}
	if (error == BTRFS_OK && bt_u32(item.links) != 0) {
		error = BTRFS_INVALID_ARGUMENT;
	}
	if (error == BTRFS_OK) {
		error = bt_ns_load(transaction, tree, orphan, &packed);
	}
	if (error == BTRFS_OK && !packed.present) {
		error = BTRFS_CORRUPT;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	error = bt_ns_evict_inode(transaction, tree, id.inode, &item, budget, done);
	if (error == BTRFS_OK && *done) {
		error = bt_tx_edit(transaction, &tree->root, orphan, NULL, 0, BT_DELETE);
	}
	return bt_ns_poison(transaction, error);
}

enum btrfs_result
btrfs_transaction_take_deferred(struct btrfs_transaction *transaction, struct btrfs_object_id *id)
{
	if (transaction == NULL || id == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (transaction->deferred_count == 0) {
		return BTRFS_NOT_FOUND;
	}
	*id = transaction->deferred[--transaction->deferred_count];
	return BTRFS_OK;
}

enum btrfs_result
btrfs_transaction_clean_orphans(struct btrfs_transaction *transaction, uint64_t tree_id,
    size_t budget, size_t *cleaned, int *pending)
{
	struct bt_owned_root *tree = NULL;
	struct bt_disk_inode item;
	struct bt_key first = { BT_ORPHAN_OBJECTID, 0, BT_ORPHAN_ITEM };
	struct bt_key key;
	size_t start;
	uint64_t steps;
	int found;
	int done;
	int edited = 0;
	enum btrfs_result error;

	if (transaction == NULL || cleaned == NULL || pending == NULL || budget == 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*cleaned = 0;
	*pending = 0;
	start = bt_tx_work(transaction);
	error = bt_ns_begin(transaction, tree_id, &tree);
	for (steps = 0; error == BTRFS_OK; steps++) {
		if (steps == BT_MAX_TREE_ITEMS) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		error = bt_ns_neighbor(transaction, tree, first, 0, &key, &found);
		if (error != BTRFS_OK || !found || key.objectid != BT_ORPHAN_OBJECTID ||
		    key.type != BT_ORPHAN_ITEM) {
			break;
		}
		if (bt_tx_work(transaction) - start >= budget) {
			*pending = 1;
			break;
		}
		if (key.offset < BTRFS_ROOT_INODE || key.offset > BT_LAST_FREE_OBJECTID) {
			error = BTRFS_CORRUPT;
			break;
		}
		/* As btrfs_orphan_cleanup: an inode without links is deleted; the
		 * orphan item of a missing or still linked inode only goes away. */
		done = 1;
		error = bt_ns_inode(transaction, tree, key.offset, &item);
		if (error == BTRFS_OK && bt_u32(item.links) == 0) {
			edited = 1;
			error = bt_ns_evict_inode(transaction, tree, key.offset, &item,
			    budget - (bt_tx_work(transaction) - start), &done);
			if (error == BTRFS_OK && done) {
				(*cleaned)++;
			}
		} else if (error == BTRFS_NOT_FOUND) {
			error = BTRFS_OK;
		}
		if (error == BTRFS_OK && !done) {
			*pending = 1;
			break;
		}
		if (error == BTRFS_OK) {
			edited = 1;
			error = bt_tx_edit(transaction, &tree->root, key, NULL, 0, BT_DELETE);
			transaction->changed = 1;
		}
	}
	if (edited) {
		(void)bt_ns_poison(transaction, error);
	}
	return error;
}

/* Linux's should_remove_suid: S_ISUID, and S_ISGID when group execution is
 * set (S_ISGID alone marks mandatory locking and stays). */
static uint32_t
bt_ns_set_id_bits(uint32_t mode)
{
	uint32_t bits = mode & BT_MODE_SET_UID;

	if ((mode & (BT_MODE_SET_GID | BT_MODE_GROUP_EXECUTE)) ==
	    (BT_MODE_SET_GID | BT_MODE_GROUP_EXECUTE)) {
		bits |= BT_MODE_SET_GID;
	}
	return bits;
}

/* Loads the inode's xattr item holding security.capability; found reports
 * the entry at offset. */
static enum btrfs_result
bt_ns_capability(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t inode,
    struct bt_packed *packed, size_t *offset, size_t *entry_size, int *found)
{
	const struct bt_disk_dir *header;
	size_t length = bt_ns_length(BT_CAPABILITY_XATTR);
	struct bt_key key = { inode, bt_ns_hash(BT_CAPABILITY_XATTR, length), BT_XATTR_ITEM };
	enum btrfs_result error;

	*found = 0;
	error = bt_ns_load(transaction, tree, key, packed);
	if (error == BTRFS_OK && packed->present) {
		error = bt_ns_dir_find(
		    packed, BT_CAPABILITY_XATTR, length, offset, entry_size, &header);
		*found = error == BTRFS_OK;
		if (error == BTRFS_NOT_FOUND) {
			error = BTRFS_OK;
		}
	}
	return error;
}

static int
bt_ns_privileged(const struct btrfs_transaction *transaction, uint64_t tree, uint64_t inode)
{
	size_t i;

	for (i = 0; i < transaction->privileged_count; i++) {
		if (transaction->privileged[i].tree == tree &&
		    transaction->privileged[i].inode == inode) {
			return 1;
		}
	}
	return 0;
}

enum btrfs_result
bt_tx_privileges_settled(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    uint64_t inode, const struct bt_disk_inode *item)
{
	struct bt_packed packed;
	size_t offset;
	size_t entry_size;
	int capability = 0;
	enum btrfs_result error;

	if (bt_ns_privileged(transaction, tree->root.owner, inode)) {
		return BTRFS_OK;
	}
	error = bt_ns_buffers(transaction);
	if (error == BTRFS_OK) {
		error = bt_ns_capability(
		    transaction, tree, inode, &packed, &offset, &entry_size, &capability);
	}
	if (error == BTRFS_OK && (bt_ns_set_id_bits(bt_u32(item->mode)) != 0 || capability)) {
		error = BTRFS_UNSUPPORTED;
	}
	return error;
}

static enum btrfs_result
bt_ns_regular(struct btrfs_transaction *transaction, struct btrfs_object_id id,
    struct bt_owned_root **tree, struct bt_disk_inode *item)
{
	enum btrfs_result error;

	error = bt_ns_begin(transaction, id.tree, tree);
	if (error == BTRFS_OK) {
		error = bt_ns_inode(transaction, *tree, id.inode, item);
	}
	if (error == BTRFS_OK && (bt_u32(item->mode) & BTRFS_MODE_TYPE) != BTRFS_MODE_REGULAR) {
		error = bt_ns_is_directory(item) ? BTRFS_IS_DIRECTORY : BTRFS_INVALID_ARGUMENT;
	}
	return error;
}

enum btrfs_result
btrfs_transaction_drop_privileges(
    struct btrfs_transaction *transaction, struct btrfs_object_id id, struct btrfs_time time)
{
	struct bt_owned_root *tree = NULL;
	struct bt_disk_inode item;
	struct bt_packed packed;
	uint32_t bits;
	size_t offset = 0;
	size_t entry_size = 0;
	int capability = 0;
	enum btrfs_result error;

	if (time.nanoseconds >= BT_NANOSECONDS) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_ns_regular(transaction, id, &tree, &item);
	if (error == BTRFS_OK && bt_ns_immutable(&item)) {
		error = BTRFS_NOT_PERMITTED;
	}
	if (error == BTRFS_OK) {
		error = bt_ns_capability(
		    transaction, tree, id.inode, &packed, &offset, &entry_size, &capability);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	bits = bt_ns_set_id_bits(bt_u32(item.mode));
	if (bits == 0 && !capability) {
		return BTRFS_OK;
	}
	if (capability) {
		error = bt_ns_cut(transaction, tree, &packed, offset, entry_size);
	}
	if (error == BTRFS_OK) {
		bt_put32(&item.mode, bt_u32(item.mode) & ~bits);
		error = bt_ns_store(transaction, tree, id.inode, &item, time);
	}
	return bt_ns_poison(transaction, error);
}

enum btrfs_result
btrfs_transaction_keep_privileges(struct btrfs_transaction *transaction, struct btrfs_object_id id)
{
	struct bt_owned_root *tree = NULL;
	struct bt_disk_inode item;
	enum btrfs_result error;

	error = bt_ns_regular(transaction, id, &tree, &item);
	if (error != BTRFS_OK || bt_ns_privileged(transaction, id.tree, id.inode)) {
		return error;
	}
	if (transaction->privileged_count == BT_TRANSACTION_PRIVILEGED) {
		return BTRFS_UNSUPPORTED;
	}
	transaction->privileged[transaction->privileged_count++] = id;
	return BTRFS_OK;
}

static int
bt_ns_time_valid(struct btrfs_time time)
{
	return time.nanoseconds < BT_NANOSECONDS;
}

enum btrfs_result
btrfs_transaction_set_attributes(struct btrfs_transaction *transaction, struct btrfs_object_id id,
    const struct btrfs_attributes *attributes)
{
	struct bt_owned_root *tree = NULL;
	struct bt_disk_inode item;
	struct bt_packed packed;
	size_t offset = 0;
	size_t entry_size = 0;
	int capability = 0;
	enum btrfs_result error;

	if (attributes == NULL || (attributes->mask & ~BT_ATTRIBUTE_MASK) != 0 ||
	    (attributes->mode & ~BT_MODE_PERMISSIONS) != 0 || !bt_ns_time_valid(attributes->time) ||
	    !bt_ns_time_valid(attributes->access_time) ||
	    !bt_ns_time_valid(attributes->modify_time)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_ns_begin(transaction, id.tree, &tree);
	if (error == BTRFS_OK) {
		error = bt_ns_inode(transaction, tree, id.inode, &item);
	}
	/* Linux's notify_change refuses these attributes on immutable and
	 * append-only inodes. */
	if (error == BTRFS_OK && bt_ns_frozen(&item)) {
		error = BTRFS_NOT_PERMITTED;
	}
	if (error == BTRFS_OK && (attributes->mask & BTRFS_ATTRIBUTE_REMOVE_CAPABILITY) != 0) {
		error = bt_ns_capability(
		    transaction, tree, id.inode, &packed, &offset, &entry_size, &capability);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	if (capability) {
		error = bt_ns_cut(transaction, tree, &packed, offset, entry_size);
	}
	if ((attributes->mask & BTRFS_ATTRIBUTE_MODE) != 0) {
		bt_put32(&item.mode, (bt_u32(item.mode) & BTRFS_MODE_TYPE) | attributes->mode);
	}
	if ((attributes->mask & BTRFS_ATTRIBUTE_UID) != 0) {
		bt_put32(&item.uid, attributes->uid);
	}
	if ((attributes->mask & BTRFS_ATTRIBUTE_GID) != 0) {
		bt_put32(&item.gid, attributes->gid);
	}
	if ((attributes->mask & BTRFS_ATTRIBUTE_ACCESS_TIME) != 0) {
		bt_put64(&item.atime.seconds, (uint64_t)attributes->access_time.seconds);
		bt_put32(&item.atime.nanoseconds, attributes->access_time.nanoseconds);
	}
	if ((attributes->mask & BTRFS_ATTRIBUTE_MODIFY_TIME) != 0) {
		bt_put64(&item.mtime.seconds, (uint64_t)attributes->modify_time.seconds);
		bt_put32(&item.mtime.nanoseconds, attributes->modify_time.nanoseconds);
	}
	if (error == BTRFS_OK) {
		error = bt_ns_store(transaction, tree, id.inode, &item, attributes->time);
	}
	return bt_ns_poison(transaction, error);
}
