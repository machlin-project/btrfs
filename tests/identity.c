/* SPDX-License-Identifier: BSD-3-Clause */
#include <btrfs/identity.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

struct allocator {
	size_t calls, live, fail;
};

static void *
allocate(void *context, size_t size)
{
	struct allocator *allocator = context;
	void *allocation;

	if (++allocator->calls == allocator->fail) {
		return NULL;
	}
	allocation = malloc(size);
	if (allocation != NULL) {
		allocator->live++;
	}
	return allocation;
}

static void
release(void *context, void *allocation, size_t size)
{
	struct allocator *allocator = context;

	(void)size;
	assert(allocator->live != 0);
	allocator->live--;
	free(allocation);
}

int
main(void)
{
	struct allocator allocator = { 0 };
	struct btrfs_environment environment = {
		.context = &allocator, .allocate = allocate, .release = release
	};
	struct btrfs_identity_table *table;
	struct btrfs_object_id object = { .tree = 5, .inode = 256 };
	struct btrfs_object_id found;
	uint64_t root_number;
	uint64_t number;
	uint64_t again;
	size_t i;

	for (i = 1; i <= 2; i++) {
		allocator.calls = 0;
		allocator.fail = i;
		assert(btrfs_identity_create(&environment, object, &table) == BTRFS_NO_MEMORY);
		assert(table == NULL && allocator.live == 0);
	}
	allocator.fail = 0;
	assert(btrfs_identity_create(&environment, object, &table) == BTRFS_OK);
	assert(btrfs_identity_get(table, object, &root_number) == BTRFS_OK);
	assert(root_number == BTRFS_NATIVE_ROOT_ID);
	for (i = 1; i < BTRFS_NATIVE_ID_LIMIT; i++) {
		object.tree = 255 + i;
		object.inode = BTRFS_ROOT_INODE;
		assert(btrfs_identity_get(table, object, &number) == BTRFS_OK);
		assert(number == root_number + i);
		assert(btrfs_identity_get(table, object, &again) == BTRFS_OK && again == number);
		assert(btrfs_identity_lookup(table, number, &found) == BTRFS_OK);
		assert(found.tree == object.tree && found.inode == object.inode);
	}
	object.tree++;
	assert(btrfs_identity_get(table, object, &number) == BTRFS_RANGE);
	assert(btrfs_identity_lookup(table, UINT64_MAX, &found) == BTRFS_NOT_FOUND);
	btrfs_identity_destroy(table);
	assert(allocator.live == 0);
	puts("native identity: 65536 distinct roots, stable reuse, exhaustion and allocation "
	     "rollback PASS");
	return 0;
}
