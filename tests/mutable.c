/* SPDX-License-Identifier: BSD-3-Clause */
#include "encode.h"
#include "mutable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 4096U
#define KEYS 601U
#define VALUE_MAX 3900U
#define MERGE_KEYS 4000U
#define MERGE_VALUE 100U
#define MERGE_KEEP 10U
#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

struct fixture {
	struct btrfs_fs fs;
	struct bt_chunk chunk;
	struct bt_root root;
	uint8_t *medium;
	uint8_t held[SLOTS];
	size_t next;
	size_t reservations;
	size_t allocations;
	size_t live_bytes;
	size_t reads;
	size_t reserves;
	size_t fail_allocation;
	size_t fail_read;
	size_t fail_reserve;
};

struct model_value {
	uint8_t bytes[VALUE_MAX];
	size_t length;
	int present;
};

static void *
allocate(void *context, size_t size)
{
	struct fixture *fixture = context;
	void *result;

	fixture->allocations++;
	if (fixture->allocations == fixture->fail_allocation) {
		return NULL;
	}
	result = malloc(size);
	REQUIRE(result != NULL);
	fixture->live_bytes += size;
	return result;
}

static void
release(void *context, void *allocation, size_t size)
{
	struct fixture *fixture = context;

	REQUIRE(allocation != NULL && size <= fixture->live_bytes);
	fixture->live_bytes -= size;
	free(allocation);
}

static enum btrfs_result
read_medium(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct fixture *fixture = context;

	fixture->reads++;
	if (fixture->reads == fixture->fail_read) {
		return BTRFS_IO;
	}
	REQUIRE(
	    offset <= fixture->fs.env.size_bytes && length <= fixture->fs.env.size_bytes - offset);
	memcpy(buffer, fixture->medium + offset, length);
	return BTRFS_OK;
}

static enum btrfs_result
reserve(void *context, uint64_t owner, uint8_t level, uint64_t *logical)
{
	struct fixture *fixture = context;

	REQUIRE(bt_file_tree(owner) && level < BT_MAX_LEVEL);
	fixture->reserves++;
	if (fixture->reserves == fixture->fail_reserve || fixture->next == SLOTS) {
		return BTRFS_NO_SPACE;
	}
	REQUIRE(fixture->held[fixture->next] == 0);
	fixture->held[fixture->next] = 1;
	*logical = fixture->next++ * fixture->fs.info.node_size;
	fixture->reservations++;
	return BTRFS_OK;
}

static void
unreserve(void *context, uint64_t logical)
{
	struct fixture *fixture = context;
	size_t slot = (size_t)(logical / fixture->fs.info.node_size);

	REQUIRE(logical % fixture->fs.info.node_size == 0 && slot < SLOTS && fixture->held[slot]);
	fixture->held[slot] = 0;
	REQUIRE(fixture->reservations != 0);
	fixture->reservations--;
}

static void
initialize(struct fixture *fixture, uint32_t node_size, int dup)
{
	struct bt_disk_header *header;
	struct bt_le32 checksum;
	size_t size = (size_t)node_size * SLOTS;

	memset(fixture, 0, sizeof(*fixture));
	fixture->medium = calloc(1, size * (dup ? 2 : 1));
	REQUIRE(fixture->medium != NULL);
	fixture->fs.env = (struct btrfs_environment){ .context = fixture,
		.size_bytes = size * (dup ? 2 : 1),
		.read = read_medium,
		.allocate = allocate,
		.release = release };
	fixture->fs.device_size = fixture->fs.env.size_bytes;
	fixture->fs.info.node_size = node_size;
	fixture->fs.info.sector_size = 4096;
	fixture->fs.info.generation = 1;
	fixture->chunk = (struct bt_chunk){ .logical = node_size,
		.length = size - node_size,
		.type = BT_BLOCK_METADATA | (dup ? BT_BLOCK_DUP : 0),
		.physical = { node_size, size + node_size },
		.mirrors = dup ? 2 : 1,
		.confirmed = 1 };
	fixture->fs.chunks = &fixture->chunk;
	fixture->fs.chunk_count = 1;
	fixture->root = (struct bt_root){
		.address = node_size, .generation = 1, .owner = BTRFS_TOP_LEVEL_TREE
	};
	fixture->next = 2;
	header = (void *)(fixture->medium + node_size);
	bt_put64(&header->bytenr, node_size);
	bt_put64(&header->generation, 1);
	bt_put64(&header->owner, BTRFS_TOP_LEVEL_TREE);
	bt_put64(&header->flags, BT_HEADER_WRITTEN | BT_HEADER_MIXED_BACKREF);
	bt_put32(&checksum,
	    ~bt_crc32c(UINT32_MAX, (uint8_t *)header + BT_CSUM_SIZE, node_size - BT_CSUM_SIZE));
	memcpy(header->csum, &checksum, sizeof(checksum));
	if (dup) {
		memcpy(fixture->medium + size + node_size, header, node_size);
	}
}

static struct bt_key
key(unsigned index)
{
	return (struct bt_key){
		.objectid = 256 + index / 7, .offset = (index % 7) * 2, .type = BT_EXTENT_DATA
	};
}

static void
fill(struct model_value *value, unsigned index, unsigned revision, size_t length)
{
	size_t i;

	value->present = 1;
	value->length = length;
	for (i = 0; i < length; i++) {
		value->bytes[i] = (uint8_t)(index * 17 + revision * 31 + i * 7);
	}
}

static void
verify(const struct btrfs_fs *fs, struct bt_root root, const struct model_value *model)
{
	struct bt_cursor cursor;
	struct bt_record record;
	enum btrfs_result error;
	unsigned i;

	bt_cursor_init(&cursor, fs, root);
	error = bt_cursor_seek(&cursor, (struct bt_key){ 0 }, 0);
	for (i = 0; i < KEYS; i++) {
		if (!model[i].present) {
			continue;
		}
		REQUIRE(error == BTRFS_OK);
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		REQUIRE(bt_key_compare(record.key, key(i)) == 0);
		REQUIRE(record.size == model[i].length &&
		    memcmp(record.data, model[i].bytes, record.size) == 0);
		error = bt_cursor_next(&cursor);
	}
	REQUIRE(error == BTRFS_NOT_FOUND);
	bt_cursor_fini(&cursor);
}

/* Every node of the mutation has the packed layout edits keep, in place or
 * rebuilt: leaf data packed from the end in item order and every unused body
 * byte zero. */
static void
require_packed(struct bt_mutation *mutation)
{
	struct bt_mutated_block block;
	const struct bt_disk_header *header;
	const struct bt_disk_item *items;
	const uint8_t *body;
	size_t end;
	size_t used;
	size_t count;
	size_t i;
	size_t j;

	for (i = 0; i < bt_mutation_count(mutation); i++) {
		REQUIRE(bt_mutation_block(mutation, i, &block) == BTRFS_OK);
		if (block.discarded) {
			continue;
		}
		header = block.bytes;
		items = (const void *)(header + 1);
		body = (const uint8_t *)(header + 1);
		count = bt_u32(header->count);
		end = block.size - sizeof(*header);
		used = count * sizeof(struct bt_disk_pointer);
		if (header->level == 0) {
			for (j = 0; j < count; j++) {
				REQUIRE(bt_u32(items[j].offset) + bt_u32(items[j].size) == end);
				end = bt_u32(items[j].offset);
			}
			used = count * sizeof(*items);
		}
		REQUIRE(used <= end);
		for (j = used; j < end; j++) {
			REQUIRE(body[j] == 0);
		}
	}
}

/* Whether a cursor, borrowing or not, reads the record at key from the bytes of
 * one of the mutation's own nodes. */
static int
in_place(struct bt_mutation *mutation, struct bt_root root, struct bt_key wanted, int borrow)
{
	struct bt_mutated_block block;
	struct bt_cursor cursor;
	struct bt_record record;
	size_t i;
	int found = 0;

	bt_cursor_init(&cursor, bt_mutation_view(mutation), root);
	cursor.borrow = borrow;
	REQUIRE(bt_cursor_seek(&cursor, wanted, 0) == BTRFS_OK);
	REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
	REQUIRE(bt_key_compare(record.key, wanted) == 0);
	for (i = 0; i < bt_mutation_count(mutation); i++) {
		REQUIRE(bt_mutation_block(mutation, i, &block) == BTRFS_OK);
		found |= (uintptr_t)record.data - (uintptr_t)block.bytes < block.size;
	}
	bt_cursor_fini(&cursor);
	return found;
}

/* A cursor seeking near its previous position finds what a fresh seek finds,
 * for keys present, between present ones and beyond the last. */
static void
require_near(const struct btrfs_fs *fs, struct bt_root root)
{
	struct bt_cursor near;
	struct bt_cursor fresh;
	struct bt_record found;
	struct bt_record expected;
	struct bt_key wanted;
	enum btrfs_result result;
	unsigned i;

	bt_cursor_init(&near, fs, root);
	/* In order, each key and the absent one after it, then in a scattered
	 * order. */
	for (i = 0; i < 4 * KEYS; i++) {
		wanted = key(i < 2 * KEYS ? i / 2 : i * 7919U % (KEYS + KEYS / 8));
		wanted.offset += i < 2 * KEYS ? i % 2 : i % 3 == 0;
		bt_cursor_init(&fresh, fs, root);
		result = bt_cursor_seek(&fresh, wanted, 0);
		REQUIRE(bt_cursor_seek_near(&near, wanted) == result);
		if (result == BTRFS_OK) {
			REQUIRE(bt_cursor_record(&fresh, &expected) == BTRFS_OK &&
			    bt_cursor_record(&near, &found) == BTRFS_OK);
			REQUIRE(bt_key_compare(found.key, expected.key) == 0 &&
			    found.size == expected.size &&
			    memcmp(found.data, expected.data, found.size) == 0);
		}
		bt_cursor_fini(&fresh);
	}
	bt_cursor_fini(&near);
}

static struct bt_mutation *
begin(struct fixture *fixture, size_t limit)
{
	struct bt_mutation_allocator allocator = { fixture, reserve, unreserve, limit };
	struct bt_mutation *mutation;

	REQUIRE(bt_mutation_create(&fixture->fs, &allocator, &mutation) == BTRFS_OK);
	return mutation;
}

/* This publishes an isolated tree to the synthetic test medium. It is not a
 * filesystem commit: the production editor has no media-write interface. */
static void
publish_tree(struct fixture *fixture, struct bt_mutation *mutation)
{
	struct bt_mutated_block block;
	uint64_t physical;
	unsigned mirrors;
	unsigned mirror;
	size_t i;

	REQUIRE(bt_mutation_accept(mutation) == BTRFS_INVALID_ARGUMENT);
	REQUIRE(bt_mutation_seal(mutation) == BTRFS_OK);
	for (i = 0; i < bt_mutation_count(mutation); i++) {
		REQUIRE(bt_mutation_block(mutation, i, &block) == BTRFS_OK);
		if (block.discarded) {
			continue;
		}
		mirrors = 1;
		for (mirror = 0; mirror < mirrors; mirror++) {
			REQUIRE(bt_map(&fixture->fs, block.address, block.size, BT_BLOCK_METADATA,
				    mirror, &physical, &mirrors) == BTRFS_OK);
			memcpy(fixture->medium + physical, block.bytes, block.size);
		}
	}
	REQUIRE(bt_mutation_accept(mutation) == BTRFS_OK);
	REQUIRE(bt_mutation_accept(mutation) == BTRFS_INVALID_ARGUMENT);
	bt_mutation_destroy(mutation);
	fixture->fs.info.generation++;
	REQUIRE(fixture->live_bytes == 0);
}

static void
exercise(uint32_t node_size, int dup)
{
	struct fixture fixture;
	struct bt_root root;
	struct bt_root snapshot;
	struct bt_mutation *mutation;
	struct model_value *model;
	struct model_value *original;
	uint8_t *before;
	uint8_t value[VALUE_MAX];
	size_t length;
	size_t bytes;
	size_t allocations;
	size_t held;
	unsigned i;
	unsigned index;

	initialize(&fixture, node_size, dup);
	model = calloc(KEYS, sizeof(*model));
	original = calloc(KEYS, sizeof(*original));
	REQUIRE(model != NULL && original != NULL);
	root = fixture.root;
	mutation = begin(&fixture, SLOTS / 2);
	for (i = 0; i < KEYS; i++) {
		index = i * 137 % KEYS;
		fill(&model[index], index, 0, 1800);
		REQUIRE(bt_mutation_edit(mutation, &root, key(index), model[index].bytes,
			    model[index].length, BT_INSERT) == BTRFS_OK);
		if (i % 100 == 0) {
			verify(bt_mutation_view(mutation), root, model);
			require_packed(mutation);
		}
	}
	REQUIRE(node_size != 4096 || root.level >= 2);
	verify(bt_mutation_view(mutation), root, model);
	require_packed(mutation);
	require_near(bt_mutation_view(mutation), root);
	for (i = 0; i < KEYS; i += 7) {
		REQUIRE(bt_mutation_find(mutation, root, key(i), value, sizeof(value), &length) ==
		    BTRFS_OK);
		REQUIRE(length == model[i].length && memcmp(value, model[i].bytes, length) == 0);
		REQUIRE(
		    in_place(mutation, root, key(i), 1) && !in_place(mutation, root, key(i), 0));
	}
	verify(&fixture.fs, fixture.root, original);
	publish_tree(&fixture, mutation);
	verify(&fixture.fs, root, model);
	snapshot = root;
	memcpy(original, model, KEYS * sizeof(*model));
	bytes = fixture.next * node_size;
	before = malloc(bytes);
	REQUIRE(before != NULL);
	memcpy(before, fixture.medium, bytes);
	held = fixture.reservations;
	/* A snapshot initially shares all source blocks but owns its new blocks. */
	root.owner = 256;
	mutation = begin(&fixture, SLOTS / 2);
	for (i = 0; i < KEYS; i++) {
		index = i * 139 % KEYS;
		fill(&model[index], index, 1, index % 5 == 0 ? VALUE_MAX : index % 1700);
		REQUIRE(bt_mutation_edit(mutation, &root, key(index), model[index].bytes,
			    model[index].length, BT_REPLACE) == BTRFS_OK);
		if (i % 100 == 0) {
			require_packed(mutation);
		}
	}
	verify(bt_mutation_view(mutation), root, model);
	require_packed(mutation);
	require_near(bt_mutation_view(mutation), root);
	verify(&fixture.fs, snapshot, original);
	require_near(&fixture.fs, snapshot);
	REQUIRE(memcmp(before, fixture.medium, bytes) == 0);
	allocations = fixture.allocations;
	for (i = 0; i < 50; i++) {
		fill(&model[1], 1, i, 1);
		REQUIRE(bt_mutation_edit(mutation, &root, key(1), model[1].bytes, 1, BT_UPSERT) ==
		    BTRFS_OK);
	}
	REQUIRE(fixture.allocations == allocations);
	length = 0;
	REQUIRE(bt_mutation_find(mutation, root, key(0), NULL, 0, &length) == BTRFS_OK &&
	    length == VALUE_MAX);
	memset(value, 0xa5, sizeof(value));
	REQUIRE(bt_mutation_find(mutation, root, key(0), value, 1, &length) == BTRFS_RANGE &&
	    value[0] == 0xa5);
	REQUIRE(
	    bt_mutation_find(mutation, root, key(0), value, sizeof(value), &length) == BTRFS_OK);
	REQUIRE(memcmp(value, model[0].bytes, length) == 0);
	for (i = 0; i < KEYS; i++) {
		index = i * 137 % KEYS;
		REQUIRE(
		    bt_mutation_edit(mutation, &root, key(index), NULL, 0, BT_DELETE) == BTRFS_OK);
		model[index].present = 0;
		if (i % 100 == 0) {
			verify(bt_mutation_view(mutation), root, model);
			require_packed(mutation);
		}
	}
	REQUIRE(root.level == 0);
	verify(bt_mutation_view(mutation), root, model);
	require_packed(mutation);
	REQUIRE(bt_mutation_seal(mutation) == BTRFS_OK);
	REQUIRE(bt_mutation_edit(mutation, &root, key(0), NULL, 0, BT_INSERT) == BTRFS_READ_ONLY);
	bt_mutation_destroy(mutation);
	REQUIRE(fixture.reservations == held && fixture.live_bytes == 0);
	REQUIRE(memcmp(before, fixture.medium, bytes) == 0);
	verify(&fixture.fs, snapshot, original);
	free(before);
	free(original);
	free(model);
	free(fixture.medium);
}

static void
three_way_split(void)
{
	struct fixture fixture;
	struct bt_mutation *mutation;
	struct bt_root root;
	struct model_value *model;

	initialize(&fixture, 4096, 0);
	root = fixture.root;
	model = calloc(KEYS, sizeof(*model));
	REQUIRE(model != NULL);
	mutation = begin(&fixture, 32);
	fill(&model[0], 0, 0, 1800);
	fill(&model[2], 2, 0, 1800);
	fill(&model[1], 1, 0, VALUE_MAX);
	REQUIRE(bt_mutation_edit(mutation, &root, key(0), model[0].bytes, model[0].length,
		    BT_INSERT) == BTRFS_OK);
	REQUIRE(bt_mutation_edit(mutation, &root, key(2), model[2].bytes, model[2].length,
		    BT_INSERT) == BTRFS_OK);
	REQUIRE(bt_mutation_edit(mutation, &root, key(1), model[1].bytes, model[1].length,
		    BT_INSERT) == BTRFS_OK);
	REQUIRE(root.level == 1 && bt_mutation_count(mutation) == 4);
	verify(bt_mutation_view(mutation), root, model);
	bt_mutation_destroy(mutation);
	REQUIRE(fixture.live_bytes == 0 && fixture.reservations == 0);
	free(model);
	free(fixture.medium);
}

static void
count_nodes(
    const struct btrfs_fs *fs, struct bt_root root, size_t *leaves, size_t *nodes, size_t *used)
{
	const struct bt_disk_header *header;
	const struct bt_disk_item *items;
	const struct bt_disk_pointer *pointers;
	struct bt_root child;
	uint8_t *block;
	uint32_t i;

	block = malloc(fs->info.node_size);
	REQUIRE(block != NULL);
	REQUIRE(bt_tree_read(fs, root, block) == BTRFS_OK);
	header = (const void *)block;
	items = (const void *)(header + 1);
	pointers = (const void *)(header + 1);
	if (root.level == 0) {
		(*leaves)++;
		for (i = 0; i < bt_u32(header->count); i++) {
			*used += sizeof(*items) + bt_u32(items[i].size);
		}
	} else {
		(*nodes)++;
		for (i = 0; i < bt_u32(header->count); i++) {
			child = root;
			child.address = bt_u64(pointers[i].bytenr);
			child.generation = bt_u64(pointers[i].generation);
			child.level = (uint8_t)(root.level - 1);
			count_nodes(fs, child, leaves, nodes, used);
		}
	}
	free(block);
}

/* Deleting most items merges underfull leaves and then underfull nodes, so the
 * tree shrinks instead of keeping one sparse leaf per original leaf. */
static void
rebalance(void)
{
	struct fixture fixture;
	struct bt_mutation *mutation;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	uint8_t value[MERGE_VALUE];
	size_t leaves = 0;
	size_t nodes = 0;
	size_t used = 0;
	size_t capacity;
	unsigned i;
	unsigned index;
	enum btrfs_result error;

	initialize(&fixture, 4096, 0);
	capacity = fixture.fs.info.node_size - sizeof(struct bt_disk_header);
	root = fixture.root;
	mutation = begin(&fixture, SLOTS / 2);
	for (i = 0; i < MERGE_KEYS; i++) {
		memset(value, (int)(i & 0xffU), sizeof(value));
		REQUIRE(bt_mutation_edit(
			    mutation, &root, key(i), value, sizeof(value), BT_INSERT) == BTRFS_OK);
	}
	publish_tree(&fixture, mutation);
	REQUIRE(root.level == 2);
	count_nodes(&fixture.fs, root, &leaves, &nodes, &used);
	REQUIRE(leaves >
	    6 * (MERGE_KEYS / MERGE_KEEP) * (sizeof(struct bt_disk_item) + MERGE_VALUE) / capacity);
	mutation = begin(&fixture, SLOTS / 2);
	for (i = 0; i < MERGE_KEYS; i++) {
		index = i * 1999 % MERGE_KEYS;
		if (index % MERGE_KEEP != 0) {
			REQUIRE(bt_mutation_edit(mutation, &root, key(index), NULL, 0, BT_DELETE) ==
			    BTRFS_OK);
		}
	}
	bt_cursor_init(&cursor, bt_mutation_view(mutation), root);
	error = bt_cursor_seek(&cursor, (struct bt_key){ 0 }, 0);
	for (i = 0; i < MERGE_KEYS; i += MERGE_KEEP) {
		REQUIRE(error == BTRFS_OK && bt_cursor_record(&cursor, &record) == BTRFS_OK);
		REQUIRE(bt_key_compare(record.key, key(i)) == 0 && record.size == MERGE_VALUE &&
		    record.data[0] == (uint8_t)(i & 0xffU));
		error = bt_cursor_next(&cursor);
	}
	REQUIRE(error == BTRFS_NOT_FOUND);
	bt_cursor_fini(&cursor);
	publish_tree(&fixture, mutation);
	leaves = nodes = used = 0;
	count_nodes(&fixture.fs, root, &leaves, &nodes, &used);
	/* No two adjacent edited leaves are both below a third, so each pair of
	 * leaves holds at least a third of a leaf. */
	REQUIRE(leaves <= 6 * used / capacity + 2);
	REQUIRE(root.level == 1 && nodes == 1);
	free(fixture.medium);
}

static void
failures(void)
{
	struct fixture fixture;
	struct bt_mutation_allocator allocator;
	struct bt_mutation *mutation;
	struct bt_root root;
	uint8_t value[VALUE_MAX] = { 0 };
	uint8_t original[4096];
	enum btrfs_result result;
	size_t totals[3] = { 0 };
	size_t point;
	unsigned kind;
	unsigned i;

	for (kind = 0; kind < 4; kind++) {
		for (point = 1; point <= (kind == 0 ? 1 : totals[kind - 1]); point++) {
			initialize(&fixture, 4096, 0);
			memcpy(original, fixture.medium + fixture.root.address, sizeof(original));
			fixture.fail_allocation = kind == 1 ? point : 0;
			fixture.fail_reserve = kind == 2 ? point : 0;
			fixture.fail_read = kind == 3 ? point : 0;
			allocator =
			    (struct bt_mutation_allocator){ &fixture, reserve, unreserve, 64 };
			root = fixture.root;
			mutation = NULL;
			result = bt_mutation_create(&fixture.fs, &allocator, &mutation);
			for (i = 0; result == BTRFS_OK && i < 8; i++) {
				result = bt_mutation_edit(
				    mutation, &root, key(i), value, sizeof(value), BT_INSERT);
			}
			if (kind == 0) {
				REQUIRE(result == BTRFS_OK);
				totals[0] = fixture.allocations;
				totals[1] = fixture.reserves;
				totals[2] = fixture.reads;
			} else {
				REQUIRE(result ==
				    (kind == 1		? BTRFS_NO_MEMORY
					    : kind == 2 ? BTRFS_NO_SPACE
							: BTRFS_IO));
				if (mutation != NULL) {
					REQUIRE(bt_mutation_view(mutation) == NULL);
					REQUIRE(bt_mutation_seal(mutation) == result);
					REQUIRE(bt_mutation_edit(mutation, &root, key(9), value, 1,
						    BT_INSERT) == result);
				}
			}
			bt_mutation_destroy(mutation);
			REQUIRE(fixture.live_bytes == 0 && fixture.reservations == 0);
			REQUIRE(memcmp(original, fixture.medium + fixture.root.address,
				    sizeof(original)) == 0);
			free(fixture.medium);
		}
	}
}

/* A node the editor left malformed is refused at seal, before any write. */
static void
malformed_seal(void)
{
	struct fixture fixture;
	struct bt_mutation *mutation;
	struct bt_mutated_block block;
	struct bt_disk_item *items;
	struct bt_root root;
	uint8_t value[8] = { 0 };
	uint8_t original[4096];
	unsigned corruption;
	unsigned i;

	for (corruption = 0; corruption < 2; corruption++) {
		initialize(&fixture, 4096, 0);
		memcpy(original, fixture.medium + fixture.root.address, sizeof(original));
		root = fixture.root;
		mutation = begin(&fixture, 8);
		for (i = 0; i < 3; i++) {
			REQUIRE(bt_mutation_edit(mutation, &root, key(i), value, sizeof(value),
				    BT_INSERT) == BTRFS_OK);
		}
		REQUIRE(root.level == 0 && bt_mutation_count(mutation) == 1);
		REQUIRE(bt_mutation_block(mutation, 0, &block) == BTRFS_OK);
		items = (void *)((uint8_t *)block.bytes + sizeof(struct bt_disk_header));
		if (corruption == 0) {
			items[2].key = items[1].key;
		} else {
			bt_put32(&items[1].offset, (uint32_t)block.size);
		}
		REQUIRE(bt_mutation_seal(mutation) == BTRFS_CORRUPT);
		REQUIRE(bt_mutation_seal(mutation) == BTRFS_CORRUPT);
		REQUIRE(bt_mutation_accept(mutation) == BTRFS_INVALID_ARGUMENT);
		REQUIRE(bt_mutation_view(mutation) == NULL);
		bt_mutation_destroy(mutation);
		REQUIRE(fixture.live_bytes == 0 && fixture.reservations == 0);
		REQUIRE(
		    memcmp(original, fixture.medium + fixture.root.address, sizeof(original)) == 0);
		free(fixture.medium);
	}
}

int
main(void)
{
	three_way_split();
	rebalance();
	exercise(4096, 0);
	exercise(16384, 1);
	exercise(65536, 0);
	failures();
	malformed_seal();
	puts("private CoW trees: model, split/grow/shrink/merge, snapshot isolation, abort/faults "
	     "PASS");
	return 0;
}
