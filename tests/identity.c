/* SPDX-License-Identifier: BSD-3-Clause */
#include <btrfs/identity.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* Objects of the mount root's tree numbered in one run; none allocates. */
#define ROOT_TREE_OBJECTS 1000000U
/* The first tree after the mount root's that the slot test assigns. */
#define FIRST_OTHER_TREE UINT64_C(1000)
/* The tree of the mount root, a subvolume (not FS_TREE). */
#define ROOT_TREE UINT64_C(257)
#define FS_TREE UINT64_C(5)
/* Object IDs below BTRFS_ROOT_INODE that a fs tree may hold (the free-ino
 * cache inode) or that a damaged directory entry may name. */
#define FREE_INO_OBJECTID (UINT64_MAX - 11U)
#define LOW_OBJECTID UINT64_C(5)

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

static uint64_t
get(struct btrfs_identity_table *table, uint64_t tree, uint64_t inode)
{
	struct btrfs_object_id object = { .tree = tree, .inode = inode };
	struct btrfs_object_id found;
	uint64_t number;
	uint64_t again;

	assert(btrfs_identity_get(table, object, &number) == BTRFS_OK);
	assert(btrfs_identity_get(table, object, &again) == BTRFS_OK && again == number);
	assert(btrfs_identity_lookup(table, number, &found) == BTRFS_OK);
	assert(found.tree == tree && found.inode == inode);
	return number;
}

static int
missing(const struct btrfs_identity_table *table, uint64_t number)
{
	struct btrfs_object_id found;

	return btrfs_identity_lookup(table, number, &found) == BTRFS_NOT_FOUND;
}

static uint64_t
direct(uint64_t slot, uint64_t inode)
{
	return (slot << BTRFS_NATIVE_SLOT_SHIFT) | inode;
}

/* Creation fails cleanly at each of its allocations. */
static void
creation(struct allocator *allocator, const struct btrfs_environment *environment)
{
	struct btrfs_object_id root = { .tree = ROOT_TREE, .inode = BTRFS_ROOT_INODE };
	struct btrfs_identity_table *table;
	size_t i;

	for (i = 1; i <= 3; i++) {
		allocator->calls = 0;
		allocator->fail = i;
		assert(btrfs_identity_create(environment, root, &table) == BTRFS_NO_MEMORY);
		assert(table == NULL && allocator->live == 0);
	}
	allocator->fail = 0;
}

/* The root is 2 alone; objects of its tree keep their inode numbers without
 * any allocation; numbers of no object are not found. */
static void
root_tree(struct allocator *allocator, const struct btrfs_environment *environment)
{
	struct btrfs_object_id root = { .tree = ROOT_TREE, .inode = BTRFS_ROOT_INODE };
	struct btrfs_object_id found;
	struct btrfs_identity_table *table;
	size_t calls;
	uint64_t inode;

	assert(btrfs_identity_create(environment, root, &table) == BTRFS_OK);
	assert(get(table, ROOT_TREE, BTRFS_ROOT_INODE) == BTRFS_NATIVE_ROOT_ID);
	assert(missing(table, BTRFS_ROOT_INODE));
	assert(missing(table, 0) && missing(table, 1) && missing(table, BTRFS_ROOT_INODE - 1U));
	calls = allocator->calls;
	for (inode = BTRFS_ROOT_INODE + 1U; inode < BTRFS_ROOT_INODE + ROOT_TREE_OBJECTS; inode++) {
		assert(get(table, ROOT_TREE, inode) == inode);
	}
	assert(get(table, ROOT_TREE, BTRFS_NATIVE_INODE_MASK) == BTRFS_NATIVE_INODE_MASK);
	assert(allocator->calls == calls);
	/* Lookup decodes a direct number that was never issued. */
	assert(btrfs_identity_lookup(table, BTRFS_NATIVE_INODE_MASK - 1U, &found) == BTRFS_OK);
	assert(found.tree == ROOT_TREE && found.inode == BTRFS_NATIVE_INODE_MASK - 1U);
	/* No other tree has a slot yet. */
	assert(missing(table, direct(1, BTRFS_ROOT_INODE)));
	assert(missing(table, BTRFS_NATIVE_INDIRECT_BASE));
	assert(missing(table, UINT64_MAX));
	btrfs_identity_destroy(table);
	assert(allocator->live == 0);
}

/* Inode numbers without a direct number get distinct indirect ones; the
 * indirect range is exhausted explicitly while direct numbers continue. */
static void
indirect(struct allocator *allocator, const struct btrfs_environment *environment)
{
	struct btrfs_object_id root = { .tree = ROOT_TREE, .inode = BTRFS_ROOT_INODE };
	struct btrfs_object_id object = { .tree = FS_TREE, .inode = 0 };
	struct btrfs_identity_table *table;
	uint64_t number;
	uint64_t first;
	uint64_t i;

	assert(btrfs_identity_create(environment, root, &table) == BTRFS_OK);
	first = get(table, ROOT_TREE, BTRFS_NATIVE_INODE_MASK + 1U);
	assert(first == BTRFS_NATIVE_INDIRECT_BASE);
	assert(get(table, ROOT_TREE, LOW_OBJECTID) == first + 1U);
	assert(get(table, ROOT_TREE, FREE_INO_OBJECTID) == first + 2U);
	/* Another tree: its slot is 1, its low object indirect. */
	assert(get(table, FS_TREE, BTRFS_ROOT_INODE) == direct(1, BTRFS_ROOT_INODE));
	assert(get(table, FS_TREE, LOW_OBJECTID) == first + 3U);
	assert(get(table, ROOT_TREE, BTRFS_NATIVE_INODE_MASK + 1U) == first);
	/* An indirect entry that fails to allocate consumes no number. */
	allocator->calls = 0;
	allocator->fail = 1;
	object.inode = UINT64_MAX;
	assert(btrfs_identity_get(table, object, &number) == BTRFS_NO_MEMORY);
	allocator->fail = 0;
	assert(get(table, FS_TREE, UINT64_MAX) == first + 4U);
	for (i = 5; i < BTRFS_NATIVE_INDIRECT_LIMIT; i++) {
		assert(get(table, FS_TREE, UINT64_MAX - i) == first + i);
	}
	object.inode = UINT64_MAX - BTRFS_NATIVE_INDIRECT_LIMIT;
	assert(btrfs_identity_get(table, object, &number) == BTRFS_RANGE);
	assert(missing(table, first + BTRFS_NATIVE_INDIRECT_LIMIT));
	assert(get(table, FS_TREE, BTRFS_ROOT_INODE + 1U) == direct(1, BTRFS_ROOT_INODE + 1U));
	assert(get(table, ROOT_TREE, LOW_OBJECTID) == first + 1U);
	btrfs_identity_destroy(table);
	assert(allocator->live == 0);
}

/* Trees get slots in order; growth that fails changes nothing; after the
 * last slot a new tree's objects are indirect. */
static void
slots(struct allocator *allocator, const struct btrfs_environment *environment)
{
	struct btrfs_object_id root = { .tree = ROOT_TREE, .inode = BTRFS_ROOT_INODE };
	struct btrfs_object_id object = { .inode = BTRFS_ROOT_INODE };
	struct btrfs_identity_table *table;
	uint64_t number;
	uint64_t slot;
	size_t failure;
	size_t live;

	assert(btrfs_identity_create(environment, root, &table) == BTRFS_OK);
	for (slot = 1; slot < BTRFS_NATIVE_TREE_LIMIT; slot++) {
		object.tree = FIRST_OTHER_TREE + slot;
		/* Each growth fails at either array before it succeeds. */
		for (failure = 1; (slot & (slot - 1U)) == 0 && slot >= 8U && failure <= 2U;
		    failure++) {
			live = allocator->live;
			allocator->calls = 0;
			allocator->fail = failure;
			assert(btrfs_identity_get(table, object, &number) == BTRFS_NO_MEMORY);
			assert(allocator->live == live);
			assert(missing(table, direct(slot, BTRFS_ROOT_INODE)));
			allocator->fail = 0;
		}
		assert(get(table, object.tree, BTRFS_ROOT_INODE) == direct(slot, BTRFS_ROOT_INODE));
	}
	for (slot = 1; slot < BTRFS_NATIVE_TREE_LIMIT; slot++) {
		assert(get(table, FIRST_OTHER_TREE + slot, BTRFS_NATIVE_INODE_MASK) ==
		    direct(slot, BTRFS_NATIVE_INODE_MASK));
	}
	assert(get(table, ROOT_TREE, BTRFS_ROOT_INODE + 1U) == BTRFS_ROOT_INODE + 1U);
	number = get(table, FIRST_OTHER_TREE, BTRFS_ROOT_INODE);
	assert(number == BTRFS_NATIVE_INDIRECT_BASE);
	assert(get(table, FIRST_OTHER_TREE, BTRFS_ROOT_INODE + 1U) == number + 1U);
	btrfs_identity_destroy(table);
	assert(allocator->live == 0);
}

int
main(void)
{
	struct allocator allocator = { 0 };
	struct btrfs_environment environment = {
		.context = &allocator, .allocate = allocate, .release = release
	};

	creation(&allocator, &environment);
	root_tree(&allocator, &environment);
	indirect(&allocator, &environment);
	slots(&allocator, &environment);
	puts("native identity: direct root-tree numbers without allocation, 32768 tree slots, "
	     "indirect exhaustion and allocation rollback PASS");
	return 0;
}
