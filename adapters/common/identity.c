/* SPDX-License-Identifier: BSD-3-Clause */
#include <btrfs/identity.h>

#define ID_BUCKETS 1024U

struct identity_entry {
	struct btrfs_object_id object;
	uint64_t number;
	struct identity_entry *object_next;
	struct identity_entry *number_next;
};

struct btrfs_identity_table {
	struct btrfs_environment environment;
	uint64_t next;
	struct identity_entry *objects[ID_BUCKETS];
	struct identity_entry *numbers[ID_BUCKETS];
};

static size_t
object_hash(struct btrfs_object_id object)
{
	uint64_t value = object.inode ^ (object.tree * UINT64_C(0x9e3779b97f4a7c15));

	value ^= value >> 32;
	return (size_t)(value % ID_BUCKETS);
}

enum btrfs_result
btrfs_identity_get(
    struct btrfs_identity_table *table, struct btrfs_object_id object, uint64_t *number)
{
	struct identity_entry *entry;
	size_t bucket;

	if (table == NULL || number == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	bucket = object_hash(object);
	for (entry = table->objects[bucket]; entry != NULL; entry = entry->object_next) {
		if (entry->object.tree == object.tree && entry->object.inode == object.inode) {
			*number = entry->number;
			return BTRFS_OK;
		}
	}
	if (table->next - BTRFS_NATIVE_ROOT_ID == BTRFS_NATIVE_ID_LIMIT) {
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
btrfs_identity_lookup(
    const struct btrfs_identity_table *table, uint64_t number, struct btrfs_object_id *object)
{
	const struct identity_entry *entry;

	if (table == NULL || object == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	for (entry = table->numbers[number % ID_BUCKETS]; entry != NULL;
	    entry = entry->number_next) {
		if (entry->number == number) {
			*object = entry->object;
			return BTRFS_OK;
		}
	}
	return BTRFS_NOT_FOUND;
}

enum btrfs_result
btrfs_identity_create(const struct btrfs_environment *environment, struct btrfs_object_id root,
    struct btrfs_identity_table **result)
{
	struct btrfs_identity_table *table;
	enum btrfs_result error;
	uint64_t number;
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
	table->next = BTRFS_NATIVE_ROOT_ID;
	for (i = 0; i < ID_BUCKETS; i++) {
		table->objects[i] = NULL;
		table->numbers[i] = NULL;
	}
	error = btrfs_identity_get(table, root, &number);
	if (error != BTRFS_OK) {
		btrfs_identity_destroy(table);
		return error;
	}
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
	environment.release(environment.context, table, sizeof(*table));
}
