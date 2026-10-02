/* SPDX-License-Identifier: BSD-3-Clause */
#include "encode.h"
#include "internal.h"
#include "space.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Chunks the table test inserts, one GiB apart, and the device they map. */
#define TABLE_CHUNKS 1000U
#define TABLE_CHUNK_BYTES (UINT64_C(1) << 30)
#define TABLE_SECTOR 4096U
#define TABLE_DEVICE_ID 1U

struct table_item {
	struct bt_disk_chunk chunk;
	struct bt_disk_stripe stripe;
};

static void *
table_allocate(void *context, size_t size)
{
	(void)context;
	return malloc(size);
}

static void
table_release(void *context, void *allocation, size_t size)
{
	(void)context;
	(void)size;
	free(allocation);
}

static enum btrfs_result
table_add(struct btrfs_fs *fs, uint64_t logical, uint64_t length, uint64_t type, int bootstrap)
{
	struct table_item item;
	struct bt_key key = { BT_FIRST_CHUNK_OBJECTID, logical, BT_CHUNK_ITEM };

	memset(&item, 0, sizeof(item));
	bt_put64(&item.chunk.length, length);
	bt_put64(&item.chunk.owner, BT_EXTENT_TREE);
	bt_put64(&item.chunk.stripe_length, BT_STRIPE_LENGTH);
	bt_put64(&item.chunk.type, type);
	bt_put32(&item.chunk.sector_size, TABLE_SECTOR);
	bt_put16(&item.chunk.stripes, 1);
	bt_put64(&item.stripe.device, TABLE_DEVICE_ID);
	bt_put64(&item.stripe.offset, logical);
	memcpy(item.stripe.uuid, fs->device_uuid, BTRFS_UUID_SIZE);
	return bt_chunk_add(fs, key, &item, sizeof(item), bootstrap);
}

/* The chunk table grows from BT_INITIAL_CHUNKS, stays sorted whatever the
 * insertion order, finds the chunk holding an address, and rejects overlaps
 * and repeated confirmations. */
static void
chunk_table_contract(void)
{
	struct btrfs_fs fs;
	uint64_t physical;
	uint64_t address;
	unsigned mirrors;
	size_t i;
	size_t k;

	memset(&fs, 0, sizeof(fs));
	fs.env.allocate = table_allocate;
	fs.env.release = table_release;
	fs.info.sector_size = TABLE_SECTOR;
	fs.device_id = TABLE_DEVICE_ID;
	fs.device_size = (uint64_t)(TABLE_CHUNKS + 1) * TABLE_CHUNK_BYTES;
	memset(fs.device_uuid, 0x5a, BTRFS_UUID_SIZE);
	fs.chunk_capacity = BT_INITIAL_CHUNKS;
	fs.chunks = malloc(fs.chunk_capacity * sizeof(*fs.chunks));
	assert(fs.chunks != NULL);
	/* A bootstrap system chunk, confirmed once by the chunk tree. */
	assert(table_add(&fs, 0, TABLE_CHUNK_BYTES, BT_BLOCK_SYSTEM, 1) == BTRFS_OK);
	assert(table_add(&fs, 0, TABLE_CHUNK_BYTES, BT_BLOCK_SYSTEM, 0) == BTRFS_OK);
	assert(fs.chunks[0].confirmed);
	assert(table_add(&fs, 0, TABLE_CHUNK_BYTES, BT_BLOCK_SYSTEM, 0) == BTRFS_CORRUPT);
	/* 7919 is prime to TABLE_CHUNKS: every slot once, out of order. */
	for (i = 0; i < TABLE_CHUNKS; i++) {
		k = 1 + (i * 7919U) % TABLE_CHUNKS;
		assert(table_add(&fs, k * TABLE_CHUNK_BYTES, TABLE_CHUNK_BYTES, BT_BLOCK_DATA, 0) ==
		    BTRFS_OK);
	}
	assert(fs.chunk_count == TABLE_CHUNKS + 1 && fs.chunk_capacity >= fs.chunk_count);
	for (i = 1; i < fs.chunk_count; i++) {
		assert(fs.chunks[i - 1].logical + fs.chunks[i - 1].length <= fs.chunks[i].logical);
	}
	for (i = 0; i < TABLE_CHUNKS; i++) {
		address = (uint64_t)i * TABLE_CHUNK_BYTES + (i * TABLE_SECTOR) % TABLE_CHUNK_BYTES;
		assert(bt_chunk_containing(&fs, address) == i);
		assert(bt_map(&fs, address, TABLE_SECTOR, i == 0 ? BT_BLOCK_SYSTEM : BT_BLOCK_DATA,
			   0, &physical, &mirrors) == BTRFS_OK &&
		    physical == address && mirrors == 1);
	}
	assert(bt_chunk_containing(&fs, (uint64_t)(TABLE_CHUNKS + 1) * TABLE_CHUNK_BYTES) ==
	    fs.chunk_count);
	/* Overlapping either neighbour leaves the table unchanged. */
	assert(table_add(&fs, 5 * TABLE_CHUNK_BYTES + TABLE_SECTOR, TABLE_CHUNK_BYTES,
		   BT_BLOCK_DATA, 0) == BTRFS_CORRUPT);
	assert(table_add(&fs, 5 * TABLE_CHUNK_BYTES - TABLE_SECTOR, 2 * TABLE_SECTOR, BT_BLOCK_DATA,
		   0) == BTRFS_CORRUPT);
	assert(fs.chunk_count == TABLE_CHUNKS + 1);
	free(fs.chunks);
}

static void
xattr_name_contract(void)
{
	struct {
		struct bt_disk_dir header;
		uint8_t name[6];
	} payload = { .header = { .name_length = { { 6, 0 } }, .type = BTRFS_FT_XATTR },
		.name = { 'u', 's', 'e', 'r', '/', 'a' } };
	struct bt_record record = { .key = { .type = BT_XATTR_ITEM },
		.data = (const void *)&payload,
		.size = sizeof(payload) };
	const struct bt_disk_dir *header;
	const uint8_t *name;
	const uint8_t *data;
	size_t offset = 0;

	/* Xattr names are namespace keys, not pathname components. */
	record.key.offset = bt_crc32c(UINT32_MAX - 1U, payload.name, sizeof(payload.name));
	assert(bt_dir_record(&record, &offset, &header, &name, &data) == BTRFS_OK);
	assert(offset == sizeof(payload) && bt_equal(name, payload.name, sizeof(payload.name)));
	record.key.type = BT_DIR_ITEM;
	payload.header.type = BTRFS_FT_REGULAR;
	offset = 0;
	assert(bt_dir_record(&record, &offset, &header, &name, &data) == BTRFS_CORRUPT);
	assert(bt_xattr_name_valid("user/a", 6));
	assert(!bt_xattr_name_valid("user\0a", 6));
}

/* Compare compaction with a sector model, including exact alloc/free pairs,
 * arbitrary log order, separated ranges and adjacent different chunks. */
static void
space_batch_contract(void)
{
	enum { GROUPS = 8, SECTORS = 256, CHANGES = GROUPS * SECTORS * 2 };
	struct bt_space_change *changes = malloc(CHANGES * sizeof(*changes));
	struct bt_space_change swap;
	struct bt_space_change invalid[3];
	int expected[GROUPS * SECTORS];
	int actual[GROUPS * SECTORS];
	uint32_t seed = 17;
	size_t count;
	size_t group;
	size_t sector;
	size_t index;
	size_t i;
	size_t j;
	unsigned round;

	assert(changes != NULL);
	for (round = 0; round < 32; round++) {
		count = 0;
		memset(expected, 0, sizeof(expected));
		memset(actual, 0, sizeof(actual));
		for (group = 0; group < GROUPS; group++) {
			for (sector = 0; sector < SECTORS; sector++) {
				seed = seed * 1664525U + 1013904223U;
				index = group * SECTORS + sector;
				switch (seed >> 30) {
				case 0:
					break;
				case 1:
				case 2:
					expected[index] = (seed >> 30) == 1 ? 1 : -1;
					changes[count++] =
					    (struct bt_space_change){ index * TABLE_SECTOR,
						    TABLE_SECTOR, group, expected[index] == 1 };
					break;
				default:
					changes[count++] =
					    (struct bt_space_change){ index * TABLE_SECTOR,
						    TABLE_SECTOR, group, 1 };
					changes[count++] =
					    (struct bt_space_change){ index * TABLE_SECTOR,
						    TABLE_SECTOR, group, 0 };
					break;
				}
			}
		}
		for (i = count; i > 1; i--) {
			seed = seed * 1664525U + 1013904223U;
			j = seed % i;
			swap = changes[i - 1];
			changes[i - 1] = changes[j];
			changes[j] = swap;
		}
		assert(bt_space_coalesce(changes, &count) == BTRFS_OK);
		for (i = 0; i < count; i++) {
			assert(changes[i].chunk < GROUPS);
			index = changes[i].start / TABLE_SECTOR;
			assert(index / SECTORS == changes[i].chunk);
			assert((index + changes[i].length / TABLE_SECTOR - 1) / SECTORS ==
			    changes[i].chunk);
			if (i != 0) {
				assert(changes[i - 1].start + changes[i - 1].length <=
				    changes[i].start);
				assert(changes[i - 1].chunk != changes[i].chunk ||
				    changes[i - 1].allocate != changes[i].allocate ||
				    changes[i - 1].start + changes[i - 1].length <
					changes[i].start);
			}
			for (j = 0; j < changes[i].length / TABLE_SECTOR; j++) {
				actual[index + j] += changes[i].allocate ? 1 : -1;
			}
		}
		assert(memcmp(expected, actual, sizeof(expected)) == 0);
	}
	for (i = 0; i < CHANGES; i++) {
		changes[i] =
		    (struct bt_space_change){ (CHANGES - i) * TABLE_SECTOR, TABLE_SECTOR, 0, 1 };
	}
	count = CHANGES;
	assert(bt_space_coalesce(changes, &count) == BTRFS_OK && count == 1);
	assert(changes[0].start == TABLE_SECTOR && changes[0].length == CHANGES * TABLE_SECTOR);
	invalid[0] = (struct bt_space_change){ 4096, 4096, 0, 1 };
	invalid[1] = invalid[0];
	count = 2;
	assert(bt_space_coalesce(invalid, &count) == BTRFS_CORRUPT);
	invalid[1] = (struct bt_space_change){ 4096, 8192, 0, 0 };
	count = 2;
	assert(bt_space_coalesce(invalid, &count) == BTRFS_CORRUPT);
	invalid[0] = (struct bt_space_change){ UINT64_MAX, 4096, 0, 1 };
	count = 1;
	assert(bt_space_coalesce(invalid, &count) == BTRFS_CORRUPT);
	invalid[0].length = 0;
	assert(bt_space_coalesce(invalid, &count) == BTRFS_CORRUPT);
	count = 0;
	assert(bt_space_coalesce(NULL, &count) == BTRFS_OK && count == 0);
	count = BT_SPACE_MAX_CHANGES + 1;
	assert(bt_space_coalesce(NULL, &count) == BTRFS_UNSUPPORTED);
	free(changes);
}

struct cache_allocations {
	size_t live;
};

static void *
cache_allocate(void *context, size_t size)
{
	struct cache_allocations *allocations = context;
	void *bytes = malloc(size);

	if (bytes != NULL) {
		allocations->live++;
	}
	return bytes;
}

static void
cache_release(void *context, void *bytes, size_t size)
{
	struct cache_allocations *allocations = context;

	(void)size;
	assert(bytes != NULL && allocations->live != 0);
	allocations->live--;
	free(bytes);
}

/* Published buffers transfer exactly once; pinned entries survive eviction,
 * and buffers from another allocator remain their caller's responsibility. */
static void
cache_ownership_contract(void)
{
	struct cache_allocations allocations = { 0 };
	struct cache_allocations other = { 0 };
	struct btrfs_environment env = {
		.context = &allocations, .allocate = cache_allocate, .release = cache_release
	};
	struct btrfs_environment foreign = env;
	struct btrfs_cache *cache;
	struct bt_root root = { TABLE_SECTOR, 1, BTRFS_TOP_LEVEL_TREE, 0 };
	const uint8_t *pinned[8];
	uint8_t *bytes;
	uint8_t *original;
	uint64_t owner;
	size_t handles[8];
	size_t handle = 0;
	size_t i;
	size_t reused = 0;

	foreign.context = &other;
	assert(btrfs_cache_create(&env, NULL, 8 * TABLE_SECTOR, &cache) == BTRFS_OK);
	for (i = 0; i < 8; i++) {
		root.address = (i + 1) * TABLE_SECTOR;
		bytes = cache_allocate(&allocations, TABLE_SECTOR);
		assert(bytes != NULL);
		memset(bytes, (int)i, TABLE_SECTOR);
		original = bytes;
		bt_cache_take(cache, root, TABLE_SECTOR, &bytes, root.owner, &env);
		assert(bytes == NULL);
		pinned[i] = bt_cache_pin(cache, root, TABLE_SECTOR, &owner, &handles[i], &handle);
		assert(pinned[i] == original && owner == root.owner);
	}
	root.address += TABLE_SECTOR;
	bytes = cache_allocate(&allocations, TABLE_SECTOR);
	assert(bytes != NULL);
	bt_cache_take(cache, root, TABLE_SECTOR, &bytes, root.owner, &env);
	assert(bytes != NULL);
	cache_release(&allocations, bytes, TABLE_SECTOR);
	for (i = 0; i < 8; i++) {
		assert(pinned[i][0] == i && pinned[i][TABLE_SECTOR - 1] == i);
		bt_cache_unpin(cache, handles[i]);
	}
	bytes = cache_allocate(&other, TABLE_SECTOR);
	assert(bytes != NULL);
	memset(bytes, 0x79, TABLE_SECTOR);
	bt_cache_take(cache, root, TABLE_SECTOR, &bytes, root.owner, &foreign);
	assert(bytes != NULL);
	pinned[0] = bt_cache_pin(cache, root, TABLE_SECTOR, &owner, &handle, &handle);
	assert(pinned[0] != NULL && pinned[0] != bytes && pinned[0][0] == 0x79);
	bt_cache_unpin(cache, handle);
	cache_release(&other, bytes, TABLE_SECTOR);
	btrfs_cache_destroy(cache);
	assert(allocations.live == 0 && other.live == 0);
	assert(btrfs_cache_create(&env, NULL, 16 * TABLE_SECTOR, &cache) == BTRFS_OK);
	for (i = 0; i < 64; i++) {
		root.generation++;
		bytes = bt_cache_reuse(cache, TABLE_SECTOR, &env);
		reused += bytes != NULL;
		if (bytes == NULL) {
			bytes = cache_allocate(&allocations, TABLE_SECTOR);
		}
		assert(bytes != NULL);
		memset(bytes, (int)i, TABLE_SECTOR);
		original = bytes;
		bt_cache_take(cache, root, TABLE_SECTOR, &bytes, root.owner, &env);
		assert(bytes == NULL);
		pinned[0] = bt_cache_pin(cache, root, TABLE_SECTOR, &owner, &handle, &handle);
		assert(pinned[0] == original && pinned[0][0] == i);
		bt_cache_unpin(cache, handle);
	}
	assert(reused != 0 && bt_cache_reuse(cache, TABLE_SECTOR, &foreign) == NULL);
	btrfs_cache_destroy(cache);
	assert(allocations.live == 0 && other.live == 0);
}

int
main(void)
{
	struct btrfs_fs *fs = (void *)(uintptr_t)1;
	struct bt_key low = { .objectid = 1, .type = BT_DIR_ITEM, .offset = UINT64_MAX };
	struct bt_key high = { .objectid = 2, .type = BT_INODE_ITEM, .offset = 0 };
	struct bt_le64 wire = { { 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01 } };
	size_t completed = 999;

	assert(~bt_crc32c(UINT32_MAX, "123456789", 9) == UINT32_C(0xe3069283));
	assert(bt_u64(wire) == UINT64_C(0x0123456789abcdef));
	assert(bt_key_compare(low, high) < 0);
	assert(bt_key_compare(high, low) > 0);
	assert(bt_key_compare(high, high) == 0);
	assert(btrfs_mount(NULL, 0, &fs) == BTRFS_INVALID_ARGUMENT && fs == NULL);
	assert(btrfs_read(NULL, NULL, 0, NULL, 0, &completed) == BTRFS_INVALID_ARGUMENT);
	assert(completed == 0);
	assert(!bt_name_valid("a/b", 3));
	assert(!bt_name_valid("a\0b", 3));
	assert(bt_name_valid("\xff", 1));
	space_batch_contract();
	cache_ownership_contract();
	xattr_name_contract();
	chunk_table_contract();
	btrfs_unmount(NULL);
	puts("wire encoding, CRC32C known vector, key order, names, chunk table and invalid API "
	     "inputs: PASS");
	return 0;
}
