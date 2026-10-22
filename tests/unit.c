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

	struct bt_device device;

	memset(&fs, 0, sizeof(fs));
	fs.env.allocate = table_allocate;
	fs.env.release = table_release;
	fs.info.sector_size = TABLE_SECTOR;
	fs.device_id = TABLE_DEVICE_ID;
	fs.device_size = (uint64_t)(TABLE_CHUNKS + 1) * TABLE_CHUNK_BYTES;
	memset(fs.device_uuid, 0x5a, BTRFS_UUID_SIZE);
	memset(&device, 0, sizeof(device));
	device.id = fs.device_id;
	device.size = fs.device_size;
	memcpy(device.uuid, fs.device_uuid, BTRFS_UUID_SIZE);
	fs.devices = &device;
	fs.device_count = 1;
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

/* Devices of the profile contract, and the chunk it maps. */
#define PROFILE_DEVICES 4U
#define PROFILE_DEVICE_BYTES (UINT64_C(1) << 32)
#define PROFILE_CHUNK_LOGICAL (UINT64_C(1) << 30)
#define PROFILE_STRIPE_BYTES (UINT64_C(16) << 20)
#define PROFILE_MAX_STRIPES 17U

struct profile_item {
	struct bt_disk_chunk chunk;
	struct bt_disk_stripe stripes[PROFILE_MAX_STRIPES];
};

/* A chunk item of stripes stripes, stripe i on device i % PROFILE_DEVICES at
 * a distinct offset; forged gives its second stripe another device UUID. */
static enum btrfs_result
profile_item(struct btrfs_fs *fs, uint64_t type, unsigned stripes, unsigned sub_stripes,
    uint64_t length, int forged)
{
	struct profile_item item;
	struct bt_key key = { BT_FIRST_CHUNK_OBJECTID, PROFILE_CHUNK_LOGICAL, BT_CHUNK_ITEM };
	unsigned i;

	memset(&item, 0, sizeof(item));
	bt_put64(&item.chunk.length, length);
	bt_put64(&item.chunk.owner, BT_EXTENT_TREE);
	bt_put64(&item.chunk.stripe_length, BT_STRIPE_LENGTH);
	bt_put64(&item.chunk.type, BT_BLOCK_DATA | type);
	bt_put32(&item.chunk.sector_size, TABLE_SECTOR);
	bt_put16(&item.chunk.stripes, (uint16_t)stripes);
	bt_put16(&item.chunk.sub_stripes, (uint16_t)sub_stripes);
	for (i = 0; i < stripes; i++) {
		bt_put64(&item.stripes[i].device, 1 + i % PROFILE_DEVICES);
		bt_put64(&item.stripes[i].offset, (1 + i) * PROFILE_STRIPE_BYTES * 2);
		memcpy(
		    item.stripes[i].uuid, fs->devices[i % PROFILE_DEVICES].uuid, BTRFS_UUID_SIZE);
	}
	item.stripes[1].uuid[0] ^= (uint8_t)(forged != 0);
	return bt_chunk_add(
	    fs, key, &item, sizeof(item.chunk) + stripes * sizeof(item.stripes[0]), 0);
}

static enum btrfs_result
profile_add(
    struct btrfs_fs *fs, uint64_t type, unsigned stripes, unsigned sub_stripes, uint64_t length)
{
	return profile_item(fs, type, stripes, sub_stripes, length, 0);
}

/* An independent layout model: walks the chunk's 64 KiB data units in order,
 * filling each row of stripes as Linux lays them out, and reports where unit
 * goes for copy mirror. */
static void
profile_model(uint64_t profile, unsigned stripes, uint64_t unit, unsigned mirror, unsigned *index,
    uint64_t *row)
{
	unsigned parity = profile == BT_BLOCK_RAID5 ? 1U : profile == BT_BLOCK_RAID6 ? 2U : 0U;
	unsigned columns = profile == BT_BLOCK_RAID10 ? stripes / 2 : stripes - parity;
	uint64_t walked = 0;
	uint64_t r;
	unsigned column;

	for (r = 0;; r++) {
		for (column = 0; column < columns; column++, walked++) {
			if (walked != unit) {
				continue;
			}
			*row = r;
			if (profile == BT_BLOCK_RAID10) {
				*index = column * 2 + mirror;
			} else if (parity != 0) {
				/* Row r's data begins at stripe r and its parity follows it. */
				*index = (unsigned)((r + column) % stripes);
			} else {
				*index = column;
			}
			return;
		}
	}
}

static void
profile_check(struct btrfs_fs *fs, uint64_t profile, unsigned stripes, unsigned copies)
{
	struct bt_place place;
	uint64_t offset;
	uint64_t row;
	unsigned mirrors;
	unsigned mirror;
	unsigned index;
	int striped =
	    (profile & (BT_BLOCK_RAID0 | BT_BLOCK_RAID10 | BT_BLOCK_RAID5 | BT_BLOCK_RAID6)) != 0;
	/* A RAID5/6 copy above the first is a rebuild, not a place. */
	unsigned places = (profile & (BT_BLOCK_RAID5 | BT_BLOCK_RAID6)) != 0 ? 1U : copies;

	for (offset = 0; offset < 64 * BT_STRIPE_LENGTH; offset += 3 * TABLE_SECTOR) {
		if (places < copies) {
			assert(bt_map_place(fs, PROFILE_CHUNK_LOGICAL + offset, TABLE_SECTOR,
				   BT_BLOCK_DATA, places, &place, &mirrors) == BTRFS_UNSUPPORTED);
		}
		for (mirror = 0; mirror < places; mirror++) {
			assert(
			    bt_map_place(fs, PROFILE_CHUNK_LOGICAL + offset, 2 * BT_STRIPE_LENGTH,
				BT_BLOCK_DATA, mirror, &place, &mirrors) == BTRFS_OK);
			assert(mirrors == copies);
			if (!striped) {
				assert(place.device == mirror % PROFILE_DEVICES &&
				    place.physical ==
					(1 + mirror) * PROFILE_STRIPE_BYTES * 2 + offset &&
				    place.span == 2 * BT_STRIPE_LENGTH);
				continue;
			}
			profile_model(
			    profile, stripes, offset / BT_STRIPE_LENGTH, mirror, &index, &row);
			assert(place.device == index % PROFILE_DEVICES);
			assert(place.physical ==
			    (1 + index) * PROFILE_STRIPE_BYTES * 2 + row * BT_STRIPE_LENGTH +
				offset % BT_STRIPE_LENGTH);
			assert(place.span == BT_STRIPE_LENGTH - offset % BT_STRIPE_LENGTH);
		}
		assert(bt_map_place(fs, PROFILE_CHUNK_LOGICAL + offset, TABLE_SECTOR, BT_BLOCK_DATA,
			   copies, &place, &mirrors) == BTRFS_CORRUPT);
	}
	fs->chunk_count = 0;
}

/* Every profile Linux writes maps as an independent model of its layout
 * says, and chunk items Linux's btrfs_check_chunk_valid rejects, stripes on
 * unknown devices or overlapping on one, and more stripes than supported are
 * refused. */
static void
profile_contract(void)
{
	struct bt_device devices[PROFILE_DEVICES];
	struct btrfs_fs fs;
	unsigned i;

	memset(&fs, 0, sizeof(fs));
	fs.env.allocate = table_allocate;
	fs.env.release = table_release;
	fs.info.sector_size = TABLE_SECTOR;
	for (i = 0; i < PROFILE_DEVICES; i++) {
		memset(&devices[i], 0, sizeof(devices[i]));
		devices[i].id = 1 + i;
		devices[i].size = PROFILE_DEVICE_BYTES;
		memset(devices[i].uuid, 0x40 + (int)i, BTRFS_UUID_SIZE);
	}
	fs.devices = devices;
	fs.device_count = PROFILE_DEVICES;
	fs.chunk_capacity = BT_INITIAL_CHUNKS;
	fs.chunks = malloc(fs.chunk_capacity * sizeof(*fs.chunks));
	assert(fs.chunks != NULL);
	assert(profile_add(&fs, 0, 1, 1, PROFILE_STRIPE_BYTES) == BTRFS_OK);
	profile_check(&fs, 0, 1, 1);
	assert(profile_add(&fs, BT_BLOCK_RAID1, 2, 1, PROFILE_STRIPE_BYTES) == BTRFS_OK);
	profile_check(&fs, BT_BLOCK_RAID1, 2, 2);
	assert(profile_add(&fs, BT_BLOCK_RAID1C3, 3, 1, PROFILE_STRIPE_BYTES) == BTRFS_OK);
	profile_check(&fs, BT_BLOCK_RAID1C3, 3, 3);
	assert(profile_add(&fs, BT_BLOCK_RAID1C4, 4, 1, PROFILE_STRIPE_BYTES) == BTRFS_OK);
	profile_check(&fs, BT_BLOCK_RAID1C4, 4, 4);
	for (i = 1; i <= PROFILE_DEVICES; i++) {
		assert(
		    profile_add(&fs, BT_BLOCK_RAID0, i, 1, i * PROFILE_STRIPE_BYTES) == BTRFS_OK);
		profile_check(&fs, BT_BLOCK_RAID0, i, 1);
	}
	assert(profile_add(&fs, BT_BLOCK_RAID10, 4, 2, 2 * PROFILE_STRIPE_BYTES) == BTRFS_OK);
	profile_check(&fs, BT_BLOCK_RAID10, 4, 2);
	assert(profile_add(&fs, BT_BLOCK_RAID5, 3, 1, 2 * PROFILE_STRIPE_BYTES) == BTRFS_OK);
	profile_check(&fs, BT_BLOCK_RAID5, 3, 2);
	assert(profile_add(&fs, BT_BLOCK_RAID6, 4, 1, 2 * PROFILE_STRIPE_BYTES) == BTRFS_OK);
	profile_check(&fs, BT_BLOCK_RAID6, 4, 4);
	/* Stripe counts and sub-stripes Linux's chunk checker rejects. */
	assert(profile_add(&fs, BT_BLOCK_RAID1, 3, 1, PROFILE_STRIPE_BYTES) == BTRFS_CORRUPT);
	assert(profile_add(&fs, BT_BLOCK_RAID1C3, 2, 1, PROFILE_STRIPE_BYTES) == BTRFS_CORRUPT);
	assert(profile_add(&fs, BT_BLOCK_RAID1C4, 3, 1, PROFILE_STRIPE_BYTES) == BTRFS_CORRUPT);
	assert(profile_add(&fs, BT_BLOCK_RAID10, 3, 2, PROFILE_STRIPE_BYTES) == BTRFS_CORRUPT);
	assert(profile_add(&fs, BT_BLOCK_RAID10, 4, 1, 2 * PROFILE_STRIPE_BYTES) == BTRFS_CORRUPT);
	assert(profile_add(&fs, BT_BLOCK_RAID5, 1, 1, PROFILE_STRIPE_BYTES) == BTRFS_CORRUPT);
	assert(profile_add(&fs, BT_BLOCK_RAID6, 2, 1, PROFILE_STRIPE_BYTES) == BTRFS_CORRUPT);
	assert(profile_add(&fs, 0, 2, 1, PROFILE_STRIPE_BYTES) == BTRFS_CORRUPT);
	assert(profile_add(&fs, BT_BLOCK_RAID1 | BT_BLOCK_RAID0, 2, 1, PROFILE_STRIPE_BYTES) ==
	    BTRFS_CORRUPT);
	/* A length its data stripes do not share evenly, a stripe past its
	 * device, more stripes than supported, an unknown profile. */
	assert(profile_add(&fs, BT_BLOCK_RAID0, 3, 1, PROFILE_STRIPE_BYTES + TABLE_SECTOR) ==
	    BTRFS_CORRUPT);
	assert(profile_add(&fs, BT_BLOCK_RAID1, 2, 1, PROFILE_DEVICE_BYTES) == BTRFS_CORRUPT);
	assert(profile_add(&fs, BT_BLOCK_RAID0, PROFILE_MAX_STRIPES, 1,
		   PROFILE_MAX_STRIPES * PROFILE_STRIPE_BYTES) == BTRFS_UNSUPPORTED);
	assert(
	    profile_add(&fs, UINT64_C(1) << 11, 1, 1, PROFILE_STRIPE_BYTES) == BTRFS_UNSUPPORTED);
	/* RAID0 over eight stripes puts two on each device, 128 MiB apart: apart
	 * when each holds 16 MiB, overlapping when each holds 160 MiB. */
	assert(
	    profile_add(&fs, BT_BLOCK_RAID0, 8, 1, 8 * 10 * PROFILE_STRIPE_BYTES) == BTRFS_CORRUPT);
	assert(profile_add(&fs, BT_BLOCK_RAID0, 8, 1, 8 * PROFILE_STRIPE_BYTES) == BTRFS_OK);
	fs.chunk_count = 0;
	devices[1].id = 99;
	assert(profile_add(&fs, BT_BLOCK_RAID1, 2, 1, PROFILE_STRIPE_BYTES) == BTRFS_CORRUPT);
	devices[1].id = 2;
	assert(profile_item(&fs, BT_BLOCK_RAID1, 2, 1, PROFILE_STRIPE_BYTES, 1) == BTRFS_CORRUPT);
	assert(fs.chunk_count == 0);
	free(fs.chunks);
}

/* RAID5 on five and RAID6 on six in-memory devices, four rows each. */
#define PARITY_ROWS 4U
#define PARITY_MAX_STRIPES 6U
#define PARITY_DEVICE_BYTES (PARITY_ROWS * BT_STRIPE_LENGTH)
#define PARITY_POLYNOMIAL 0x11dU

struct parity_device {
	uint8_t bytes[PARITY_DEVICE_BYTES];
};

static enum btrfs_result
parity_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct parity_device *device = context;

	if (offset > PARITY_DEVICE_BYTES || length > PARITY_DEVICE_BYTES - offset) {
		return BTRFS_IO;
	}
	memcpy(buffer, device->bytes + offset, length);
	return BTRFS_OK;
}

/* An independent GF(2^8): exponent and logarithm tables of g = 2 under
 * lib/raid6's polynomial, built by repeated doubling. */
struct parity_field {
	uint8_t exp[512];
	uint8_t log[256];
};

static void
parity_field_init(struct parity_field *field)
{
	unsigned value = 1;
	unsigned i;

	for (i = 0; i < 255; i++) {
		field->exp[i] = (uint8_t)value;
		field->exp[i + 255] = (uint8_t)value;
		field->log[value] = (uint8_t)i;
		value <<= 1;
		if (value & 0x100U) {
			value ^= PARITY_POLYNOMIAL;
		}
	}
}

static uint8_t
parity_times(const struct parity_field *field, uint8_t value, unsigned power)
{
	return value == 0 ? 0 : field->exp[field->log[value] + power % 255];
}

/* Writes a chunk's rows: data units in logical order at stripe (row + d) %
 * stripes, P after them, Q after P, as Linux's raid56 lays them out. */
static void
parity_fill(const struct parity_field *field, struct parity_device *devices, unsigned stripes,
    unsigned parity, uint8_t *data)
{
	unsigned count = stripes - parity;
	uint64_t row;
	size_t at;
	unsigned d;
	uint8_t p;
	uint8_t q;

	for (row = 0; row < PARITY_ROWS; row++) {
		for (at = 0; at < BT_STRIPE_LENGTH; at++) {
			p = 0;
			q = 0;
			for (d = 0; d < count; d++) {
				uint8_t byte = data[(row * count + d) * BT_STRIPE_LENGTH + at];

				devices[(row + d) % stripes].bytes[row * BT_STRIPE_LENGTH + at] =
				    byte;
				p ^= byte;
				q ^= parity_times(field, byte, d);
			}
			devices[(row + count) % stripes].bytes[row * BT_STRIPE_LENGTH + at] = p;
			if (parity == 2) {
				devices[(row + count + 1) % stripes]
				    .bytes[row * BT_STRIPE_LENGTH + at] = q;
			}
		}
	}
}

static enum btrfs_result
parity_add(struct btrfs_fs *fs, uint64_t profile, unsigned stripes, uint64_t length)
{
	struct profile_item item;
	struct bt_key key = { BT_FIRST_CHUNK_OBJECTID, PROFILE_CHUNK_LOGICAL, BT_CHUNK_ITEM };
	unsigned i;

	memset(&item, 0, sizeof(item));
	bt_put64(&item.chunk.length, length);
	bt_put64(&item.chunk.owner, BT_EXTENT_TREE);
	bt_put64(&item.chunk.stripe_length, BT_STRIPE_LENGTH);
	bt_put64(&item.chunk.type, BT_BLOCK_DATA | profile);
	bt_put32(&item.chunk.sector_size, TABLE_SECTOR);
	bt_put16(&item.chunk.stripes, (uint16_t)stripes);
	bt_put16(&item.chunk.sub_stripes, 1);
	for (i = 0; i < stripes; i++) {
		bt_put64(&item.stripes[i].device, fs->devices[i].id);
		memcpy(item.stripes[i].uuid, fs->devices[i].uuid, BTRFS_UUID_SIZE);
	}
	return bt_chunk_add(
	    fs, key, &item, sizeof(item.chunk) + stripes * sizeof(item.stripes[0]), 0);
}

/* Reads every sector as copy mirror and compares it with the data. */
static int
parity_reads(
    struct btrfs_fs *fs, unsigned mirror, const uint8_t *data, uint64_t length, uint64_t unit)
{
	uint8_t sector[TABLE_SECTOR];
	uint64_t offset;
	size_t span;
	unsigned mirrors;

	for (offset = unit * BT_STRIPE_LENGTH;
	    offset < (unit + 1) * BT_STRIPE_LENGTH && offset < length; offset += TABLE_SECTOR) {
		if (bt_read_copy(fs, PROFILE_CHUNK_LOGICAL + offset, sector, TABLE_SECTOR,
			BT_BLOCK_DATA, mirror, &span, &mirrors) != BTRFS_OK ||
		    span != TABLE_SECTOR || memcmp(sector, data + offset, TABLE_SECTOR) != 0) {
			return 0;
		}
	}
	return 1;
}

/* RAID5 and RAID6 rebuilds against an independent model of the parity:
 * every copy reads every data unit while all devices are there, and with the
 * unit's device lost, the copy that does without it (P; Q without P; P and
 * Q without a second data stripe) still does. */
static void
parity_contract(void)
{
	static struct parity_device memory[PARITY_MAX_STRIPES];
	static uint8_t data[PARITY_ROWS * PARITY_MAX_STRIPES * BT_STRIPE_LENGTH];
	static const unsigned shapes[][2] = { { 5, 1 }, { 6, 2 } };
	struct bt_device devices[PARITY_MAX_STRIPES];
	struct parity_field field;
	struct btrfs_fs fs;
	uint64_t profile;
	uint64_t length;
	uint64_t unit;
	uint32_t state = 0x2545f491U;
	unsigned stripes;
	unsigned parity;
	unsigned count;
	unsigned shape;
	unsigned target;
	unsigned other;
	unsigned mirror;
	unsigned copies;
	unsigned i;

	parity_field_init(&field);
	for (i = 0; i < sizeof(data); i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		data[i] = (uint8_t)state;
	}
	for (shape = 0; shape < 2; shape++) {
		stripes = shapes[shape][0];
		parity = shapes[shape][1];
		count = stripes - parity;
		profile = parity == 1 ? BT_BLOCK_RAID5 : BT_BLOCK_RAID6;
		length = (uint64_t)PARITY_ROWS * count * BT_STRIPE_LENGTH;
		parity_fill(&field, memory, stripes, parity, data);
		memset(&fs, 0, sizeof(fs));
		fs.env.allocate = table_allocate;
		fs.env.release = table_release;
		fs.info.sector_size = TABLE_SECTOR;
		for (i = 0; i < stripes; i++) {
			memset(&devices[i], 0, sizeof(devices[i]));
			devices[i].id = 1 + i;
			devices[i].size = PARITY_DEVICE_BYTES;
			devices[i].readable = PARITY_DEVICE_BYTES;
			devices[i].context = &memory[i];
			devices[i].read = parity_read;
			memset(devices[i].uuid, 0x60 + (int)i, BTRFS_UUID_SIZE);
		}
		fs.devices = devices;
		fs.device_count = stripes;
		fs.device_capacity = stripes;
		/* Device 0 reads through the environment, as the mounted device. */
		fs.env.context = &memory[0];
		fs.env.read = parity_read;
		fs.env.size_bytes = PARITY_DEVICE_BYTES;
		fs.chunk_capacity = BT_INITIAL_CHUNKS;
		fs.chunks = malloc(fs.chunk_capacity * sizeof(*fs.chunks));
		assert(fs.chunks != NULL);
		assert(parity_add(&fs, profile, stripes, length) == BTRFS_OK);
		copies = fs.chunks[0].mirrors;
		assert(copies == (parity == 1 ? 2U : 3U + count - 1U));
		for (unit = 0; unit < PARITY_ROWS * count; unit++) {
			/* Unit's stripe, as the layout puts it. */
			target = (unsigned)(((unit / count) + unit % count) % stripes);
			for (mirror = 0; mirror < copies; mirror++) {
				assert(parity_reads(&fs, mirror, data, length, unit));
			}
			/* Device 0 is the device the volume is mounted from: lose the
			 * others' reads alone. */
			devices[target].read = NULL;
			assert(target == 0 || !parity_reads(&fs, 0, data, length, unit));
			assert(parity_reads(&fs, 1, data, length, unit));
			if (parity == 2) {
				/* Without P too: Q rebuilds it. */
				i = (unsigned)((unit / count + count) % stripes);
				devices[i].read = NULL;
				assert(parity_reads(&fs, 2, data, length, unit));
				devices[i].read = parity_read;
				/* Without each other data stripe too: P and Q. */
				for (other = 0; other + 1 < count; other++) {
					i = other < unit % count ? other : other + 1;
					i = (unsigned)((unit / count + i) % stripes);
					devices[i].read = NULL;
					assert(parity_reads(&fs, 3 + other, data, length, unit));
					devices[i].read = parity_read;
				}
			}
			devices[target].read = parity_read;
		}
		free(fs.chunks);
	}
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
	profile_contract();
	parity_contract();
	btrfs_unmount(NULL);
	puts("wire encoding, CRC32C known vector, key order, names, chunk table, chunk profiles, "
	     "RAID5/6 rebuilds and invalid API inputs: PASS");
	return 0;
}
