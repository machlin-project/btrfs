/* SPDX-License-Identifier: BSD-3-Clause */
#include "encode.h"
#include "internal.h"
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
	xattr_name_contract();
	chunk_table_contract();
	btrfs_unmount(NULL);
	puts("wire encoding, CRC32C known vector, key order, names, chunk table and invalid API "
	     "inputs: PASS");
	return 0;
}
