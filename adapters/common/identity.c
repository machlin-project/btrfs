/* SPDX-License-Identifier: BSD-3-Clause */
#include <btrfs/identity.h>

/* Buckets of the indirect identities, chained by object and by number. */
#define ID_BUCKETS 1024U
/* Slots of a new table; the slot array doubles up to BTRFS_NATIVE_TREE_LIMIT. */
#define TREE_INITIAL_SLOTS 8U
/* The open-addressed tree index has more cells than slots, so every probe
 * sequence meets an empty cell. */
#define TREE_INDEX_FACTOR 2U

_Static_assert(
    ((uint64_t)BTRFS_NATIVE_TREE_LIMIT << BTRFS_NATIVE_SLOT_SHIFT) == BTRFS_NATIVE_INDIRECT_BASE,
    "direct numbers end where indirect numbers begin");

struct identity_entry {
	struct btrfs_object_id object;
	uint64_t number;
	struct identity_entry *object_next;
	struct identity_entry *number_next;
};

struct btrfs_identity_table {
	struct btrfs_environment environment;
	struct btrfs_object_id root;
	/* Tree of each slot; slot 0 is the mount root's tree. */
	uint64_t *trees;
	/* Tree index cells hold slot + 1, or zero when empty. */
	uint32_t *index;
	uint32_t slots;
	uint32_t capacity;
	uint64_t next;
	struct identity_entry *objects[ID_BUCKETS];
	struct identity_entry *numbers[ID_BUCKETS];
};

static uint64_t
mix(uint64_t value)
{
	value ^= value >> 30;
	value *= UINT64_C(0xbf58476d1ce4e5b9);
	value ^= value >> 27;
	value *= UINT64_C(0x94d049bb133111eb);
	return value ^ (value >> 31);
}

static int
same_object(struct btrfs_object_id left, struct btrfs_object_id right)
{
	return left.tree == right.tree && left.inode == right.inode;
}

static int
direct_inode(uint64_t inode)
{
	return inode >= BTRFS_ROOT_INODE && inode <= BTRFS_NATIVE_INODE_MASK;
}

/* The cell holding tree, or the empty cell where it belongs. cells is a power
 * of two larger than the slots in use, which bounds the probe. */
static uint32_t
tree_cell(const uint64_t *trees, const uint32_t *index, uint32_t cells, uint64_t tree)
{
	uint32_t mask = cells - 1U;
	uint32_t cell = (uint32_t)mix(tree) & mask;

	while (index[cell] != 0 && trees[index[cell] - 1U] != tree) {
		cell = (cell + 1U) & mask;
	}
	return cell;
}

/* Replaces the slot arrays with ones of the given capacity holding the same
 * slots; on failure the table is unchanged. */
static enum btrfs_result
resize_trees(struct btrfs_identity_table *table, uint32_t capacity)
{
	struct btrfs_environment *environment = &table->environment;
	uint32_t cells = capacity * TREE_INDEX_FACTOR;
	uint64_t *trees = environment->allocate(environment->context, capacity * sizeof(*trees));
	uint32_t *index = environment->allocate(environment->context, cells * sizeof(*index));
	uint32_t slot;
	uint32_t i;

	if (trees == NULL || index == NULL) {
		if (trees != NULL) {
			environment->release(
			    environment->context, trees, capacity * sizeof(*trees));
		}
		if (index != NULL) {
			environment->release(environment->context, index, cells * sizeof(*index));
		}
		return BTRFS_NO_MEMORY;
	}
	for (i = 0; i < cells; i++) {
		index[i] = 0;
	}
	for (slot = 0; slot < table->slots; slot++) {
		trees[slot] = table->trees[slot];
		index[tree_cell(trees, index, cells, trees[slot])] = slot + 1U;
	}
	if (table->trees != NULL) {
		environment->release(
		    environment->context, table->trees, table->capacity * sizeof(*table->trees));
		environment->release(environment->context, table->index,
		    table->capacity * TREE_INDEX_FACTOR * sizeof(*table->index));
	}
	table->trees = trees;
	table->index = index;
	table->capacity = capacity;
	return BTRFS_OK;
}

/* The slot of tree, assigning the next one when it has none. BTRFS_NOT_FOUND:
 * the slots are exhausted, and tree never gets one. */
static enum btrfs_result
tree_slot(struct btrfs_identity_table *table, uint64_t tree, uint32_t *slot)
{
	enum btrfs_result result;
	uint32_t cell;

	if (table->trees[0] == tree) {
		*slot = 0;
		return BTRFS_OK;
	}
	cell = tree_cell(table->trees, table->index, table->capacity * TREE_INDEX_FACTOR, tree);
	if (table->index[cell] != 0) {
		*slot = table->index[cell] - 1U;
		return BTRFS_OK;
	}
	if (table->slots == BTRFS_NATIVE_TREE_LIMIT) {
		return BTRFS_NOT_FOUND;
	}
	if (table->slots == table->capacity) {
		result = resize_trees(table, table->capacity * 2U);
		if (result != BTRFS_OK) {
			return result;
		}
		cell = tree_cell(
		    table->trees, table->index, table->capacity * TREE_INDEX_FACTOR, tree);
	}
	table->trees[table->slots] = tree;
	table->index[cell] = table->slots + 1U;
	*slot = table->slots++;
	return BTRFS_OK;
}

static size_t
object_hash(struct btrfs_object_id object)
{
	return (size_t)(mix(object.inode ^ mix(object.tree)) % ID_BUCKETS);
}

static enum btrfs_result
indirect_get(struct btrfs_identity_table *table, struct btrfs_object_id object, uint64_t *number)
{
	struct identity_entry *entry;
	size_t bucket = object_hash(object);

	for (entry = table->objects[bucket]; entry != NULL; entry = entry->object_next) {
		if (same_object(entry->object, object)) {
			*number = entry->number;
			return BTRFS_OK;
		}
	}
	if (table->next - BTRFS_NATIVE_INDIRECT_BASE == BTRFS_NATIVE_INDIRECT_LIMIT) {
		return BTRFS_RANGE;
	}
	entry = table->environment.allocate(table->environment.context, sizeof(*entry));
	if (entry == NULL) {
		return BTRFS_NO_MEMORY;
	}
	entry->object = object;
	entry->number = table->next++;
	entry->object_next = table->objects[bucket];
	entry->number_next = table->numbers[entry->number % ID_BUCKETS];
	table->objects[bucket] = entry;
	table->numbers[entry->number % ID_BUCKETS] = entry;
	*number = entry->number;
	return BTRFS_OK;
}

enum btrfs_result
btrfs_identity_get(
    struct btrfs_identity_table *table, struct btrfs_object_id object, uint64_t *number)
{
	enum btrfs_result result;
	uint32_t slot;

	if (table == NULL || number == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (same_object(object, table->root)) {
		*number = BTRFS_NATIVE_ROOT_ID;
		return BTRFS_OK;
	}
	result = tree_slot(table, object.tree, &slot);
	if (result == BTRFS_OK && direct_inode(object.inode)) {
		*number = ((uint64_t)slot << BTRFS_NATIVE_SLOT_SHIFT) | object.inode;
		return BTRFS_OK;
	}
	if (result != BTRFS_OK && result != BTRFS_NOT_FOUND) {
		return result;
	}
	return indirect_get(table, object, number);
}

enum btrfs_result
btrfs_identity_lookup(
    const struct btrfs_identity_table *table, uint64_t number, struct btrfs_object_id *object)
{
	const struct identity_entry *entry;
	struct btrfs_object_id found;
	uint64_t slot;

	if (table == NULL || object == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (number == BTRFS_NATIVE_ROOT_ID) {
		*object = table->root;
		return BTRFS_OK;
	}
	if (number >= BTRFS_NATIVE_INDIRECT_BASE) {
		for (entry = table->numbers[number % ID_BUCKETS]; entry != NULL;
		    entry = entry->number_next) {
			if (entry->number == number) {
				*object = entry->object;
				return BTRFS_OK;
			}
		}
		return BTRFS_NOT_FOUND;
	}
	slot = number >> BTRFS_NATIVE_SLOT_SHIFT;
	found.inode = number & BTRFS_NATIVE_INODE_MASK;
	if (slot >= table->slots || !direct_inode(found.inode)) {
		return BTRFS_NOT_FOUND;
	}
	found.tree = table->trees[slot];
	/* The root has only its own number. */
	if (same_object(found, table->root)) {
		return BTRFS_NOT_FOUND;
	}
	*object = found;
	return BTRFS_OK;
}

enum btrfs_result
btrfs_identity_create(const struct btrfs_environment *environment, struct btrfs_object_id root,
    struct btrfs_identity_table **result)
{
	struct btrfs_identity_table *table;
	size_t i;

	if (result == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (environment == NULL || environment->allocate == NULL || environment->release == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	table = environment->allocate(environment->context, sizeof(*table));
	if (table == NULL) {
		return BTRFS_NO_MEMORY;
	}
	table->environment = *environment;
	table->root = root;
	table->trees = NULL;
	table->index = NULL;
	table->slots = 0;
	table->capacity = 0;
	table->next = BTRFS_NATIVE_INDIRECT_BASE;
	for (i = 0; i < ID_BUCKETS; i++) {
		table->objects[i] = NULL;
		table->numbers[i] = NULL;
	}
	if (resize_trees(table, TREE_INITIAL_SLOTS) != BTRFS_OK) {
		environment->release(environment->context, table, sizeof(*table));
		return BTRFS_NO_MEMORY;
	}
	table->trees[0] = root.tree;
	table->index[tree_cell(
	    table->trees, table->index, table->capacity * TREE_INDEX_FACTOR, root.tree)] = 1U;
	table->slots = 1;
	*result = table;
	return BTRFS_OK;
}

void
btrfs_identity_destroy(struct btrfs_identity_table *table)
{
	struct identity_entry *entry;
	struct identity_entry *next;
	struct btrfs_environment environment;
	size_t i;

	if (table == NULL) {
		return;
	}
	environment = table->environment;
	for (i = 0; i < ID_BUCKETS; i++) {
		for (entry = table->objects[i]; entry != NULL; entry = next) {
			next = entry->object_next;
			environment.release(environment.context, entry, sizeof(*entry));
		}
	}
	environment.release(
	    environment.context, table->trees, table->capacity * sizeof(*table->trees));
	environment.release(environment.context, table->index,
	    table->capacity * TREE_INDEX_FACTOR * sizeof(*table->index));
	environment.release(environment.context, table, sizeof(*table));
}
