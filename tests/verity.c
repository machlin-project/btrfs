/* SPDX-License-Identifier: BSD-3-Clause */
/* fs-verity reads on Linux's transactions-verity fixture (tests/prepare_linux.py)
 * when the data, the Merkle tree or the descriptor disagree: data without
 * checksums, tree items and descriptor fields are edited in copies of their
 * blocks, sealed with valid checksums, so only fs-verity can refuse them.
 * Reads that fail leave only verified bytes in the reported prefix. Every
 * allocation and I/O fault point of a verified read fails cleanly. */
#include "internal.h"
#include "../adapters/posix/image.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "large" holds two copies of the fixture's "big" input, byte (offset mod
 * 256), and a short tail; one tree block of hashes covers this many bytes. */
#define LARGE_PATTERN_BYTES (2U * 4194304U)
#define TREE_BLOCK_SPAN (4096U * 128U)
#define NOCOW_BYTES 65536U
#define READ_FILL 0xa5U
#define MIRRORS 2U

struct patch {
	uint64_t offset;
	uint8_t *data;
	size_t size;
};

struct fixture {
	struct btrfs_image image;
	struct patch patches[MIRRORS];
	size_t count;
	unsigned checksum;
	uint32_t node_size;
};

static enum btrfs_result
patched_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct fixture *fixture = context;
	uint64_t start;
	uint64_t end;
	size_t i;
	enum btrfs_result error;

	error = fixture->image.environment.read(&fixture->image, offset, buffer, length);
	if (error != BTRFS_OK) {
		return error;
	}
	for (i = 0; i < fixture->count; i++) {
		start = offset > fixture->patches[i].offset ? offset : fixture->patches[i].offset;
		end = offset + length < fixture->patches[i].offset + fixture->patches[i].size
		    ? offset + length
		    : fixture->patches[i].offset + fixture->patches[i].size;
		if (start < end) {
			memcpy((uint8_t *)buffer + (size_t)(start - offset),
			    fixture->patches[i].data + (size_t)(start - fixture->patches[i].offset),
			    (size_t)(end - start));
		}
	}
	return BTRFS_OK;
}

static void *
allocate(void *context, size_t size)
{
	struct fixture *fixture = context;

	return fixture->image.environment.allocate(&fixture->image, size);
}

static void
release(void *context, void *buffer, size_t size)
{
	struct fixture *fixture = context;

	fixture->image.environment.release(&fixture->image, buffer, size);
}

static struct btrfs_fs *
patched_mount(struct fixture *fixture)
{
	struct btrfs_environment environment = fixture->image.environment;
	struct btrfs_fs *fs = NULL;

	environment.context = fixture;
	environment.read = patched_read;
	environment.allocate = allocate;
	environment.release = release;
	assert(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	return fs;
}

static void
clear_patches(struct fixture *fixture)
{
	free(fixture->patches[0].data);
	fixture->count = 0;
}

/* A copy of the leaf holding the item at or after key, patched over every
 * mirror; *item points at the item's bytes in the copy. */
static uint8_t *
patch_leaf(struct fixture *fixture, struct btrfs_fs *fs, uint64_t tree, struct bt_key key,
    uint8_t **item, size_t *size)
{
	struct bt_root root;
	struct bt_cursor cursor;
	struct bt_record record;
	const struct bt_disk_header *header;
	uint8_t *copy;
	uint64_t physical;
	unsigned mirrors = 1;
	unsigned i;

	assert(bt_find_root(fs, tree, &root) == BTRFS_OK);
	bt_cursor_init(&cursor, fs, root);
	assert(bt_cursor_seek(&cursor, key, 0) == BTRFS_OK);
	assert(bt_cursor_record(&cursor, &record) == BTRFS_OK);
	assert(record.key.objectid == key.objectid && record.key.type == key.type);
	copy = malloc(fs->info.node_size);
	assert(copy != NULL);
	memcpy(copy, cursor.blocks[0], fs->info.node_size);
	*item = copy + (record.data - cursor.blocks[0]);
	*size = record.size;
	header = (const void *)copy;
	for (i = 0; i < mirrors; i++) {
		assert(bt_map(fs, bt_u64(header->bytenr), fs->info.node_size, BT_BLOCK_METADATA, i,
			   &physical, &mirrors) == BTRFS_OK);
		assert(mirrors <= MIRRORS);
		fixture->patches[i] = (struct patch){ physical, copy, fs->info.node_size };
	}
	fixture->count = mirrors;
	bt_cursor_fini(&cursor);
	return copy;
}

static void
seal(struct fixture *fixture, uint8_t *leaf)
{
	bt_checksum(
	    fixture->checksum, leaf + BT_CSUM_SIZE, fixture->node_size - BT_CSUM_SIZE, leaf);
}

/* Reads length bytes of path from offset on the patched image: the result,
 * with the reported prefix checked against expected (when given) and every
 * byte beyond it untouched. */
static enum btrfs_result
read_path(struct fixture *fixture, const char *path, uint64_t offset, size_t length,
    const uint8_t *expected, size_t *completed)
{
	struct btrfs_fs *fs;
	struct btrfs_inode inode;
	uint8_t *buffer = malloc(length);
	uint64_t live = fixture->image.live_allocations;
	size_t i;
	enum btrfs_result result;

	fs = patched_mount(fixture);
	assert(buffer != NULL);
	memset(buffer, READ_FILL, length);
	assert(btrfs_image_lookup(fs, path, &inode) == BTRFS_OK);
	assert((inode.flags & BT_INODE_RO_VERITY) != 0);
	result = btrfs_read(fs, &inode, offset, buffer, length, completed);
	assert(*completed <= length);
	for (i = 0; i < length; i++) {
		if (i < *completed) {
			assert(expected == NULL || buffer[i] == expected[i]);
		} else {
			assert(buffer[i] == READ_FILL);
		}
	}
	free(buffer);
	btrfs_unmount(fs);
	assert(fixture->image.live_allocations == live);
	return result;
}

static void
expect_read(struct fixture *fixture, const char *name, const char *path, uint64_t offset,
    size_t length, const uint8_t *expected, enum btrfs_result wanted)
{
	size_t completed;
	enum btrfs_result result;

	result = read_path(fixture, path, offset, length, expected, &completed);
	if (result != wanted) {
		fprintf(stderr, "%s: expected %s, got %s\n", name, btrfs_result_string(wanted),
		    btrfs_result_string(result));
		abort();
	}
	assert(result != BTRFS_OK || completed == length);
	printf("%s: PASS (%s, %zu verified bytes)\n", name, btrfs_result_string(result), completed);
}

static uint8_t *
large_pattern(size_t length, uint64_t offset)
{
	uint8_t *bytes = malloc(length);
	size_t i;

	assert(bytes != NULL);
	for (i = 0; i < length; i++) {
		bytes[i] = (uint8_t)(offset + i);
	}
	return bytes;
}

/* Data the tree does not authenticate: a byte of the NODATACOW file, which
 * has no data checksums, changed on the device. */
static void
data_tests(struct fixture *fixture, struct btrfs_fs *fs)
{
	struct btrfs_inode inode;
	struct bt_root root;
	struct bt_cursor cursor;
	struct bt_record record;
	const struct bt_disk_extent *extent;
	struct bt_key key = { .type = BT_EXTENT_DATA };
	uint8_t *sector;
	uint8_t *expected;
	uint64_t physical;
	size_t completed;
	unsigned mirrors = 1;
	const size_t corrupted = 3U * fs->info.sector_size;

	assert(btrfs_image_lookup(fs, "verity/nocow/data", &inode) == BTRFS_OK);
	assert((inode.flags & BT_INODE_NODATASUM) != 0);
	expected = malloc(NOCOW_BYTES);
	assert(expected != NULL);
	assert(btrfs_read(fs, &inode, 0, expected, NOCOW_BYTES, &completed) == BTRFS_OK);
	assert(completed == NOCOW_BYTES);
	key.objectid = inode.id.inode;
	assert(bt_find_root(fs, inode.id.tree, &root) == BTRFS_OK);
	bt_cursor_init(&cursor, fs, root);
	assert(bt_cursor_seek(&cursor, key, 0) == BTRFS_OK);
	assert(bt_cursor_record(&cursor, &record) == BTRFS_OK);
	assert(record.key.objectid == inode.id.inode && record.size == sizeof(*extent));
	extent = (const void *)record.data;
	assert(bt_u64(extent->length) == NOCOW_BYTES && extent->header.compression == 0);
	assert(bt_map(fs, bt_u64(extent->disk_bytenr) + bt_u64(extent->offset) + corrupted,
		   fs->info.sector_size, BT_BLOCK_DATA, 0, &physical, &mirrors) == BTRFS_OK &&
	    mirrors == 1);
	bt_cursor_fini(&cursor);
	sector = malloc(fs->info.sector_size);
	assert(sector != NULL);
	memcpy(sector, expected + corrupted, fs->info.sector_size);
	sector[17] ^= 1;
	fixture->patches[0] = (struct patch){ physical, sector, fs->info.sector_size };
	fixture->count = 1;
	expect_read(fixture, "changed data without checksums", "verity/nocow/data", 0, NOCOW_BYTES,
	    expected, BTRFS_CORRUPT);
	assert(read_path(fixture, "verity/nocow/data", 0, NOCOW_BYTES, expected, &completed) ==
	    BTRFS_CORRUPT);
	assert(completed <= corrupted);
	expect_read(fixture, "data before the changed block", "verity/nocow/data", 0, corrupted,
	    expected, BTRFS_OK);
	expect_read(fixture, "data after the changed block", "verity/nocow/data",
	    corrupted + fs->info.sector_size, fs->info.sector_size,
	    expected + corrupted + fs->info.sector_size, BTRFS_OK);
	clear_patches(fixture);
	free(expected);
}

/* The Merkle tree: a hash of a level-0 block of "large" (stored after its one
 * level-1 block), and the root block of the three-level "sha512-1k". */
static void
tree_tests(struct fixture *fixture, struct btrfs_fs *fs)
{
	struct btrfs_inode inode;
	struct bt_key key = { .type = BT_VERITY_MERKLE_ITEM };
	uint8_t *leaf;
	uint8_t *item;
	uint8_t *expected;
	size_t size;
	const uint64_t tree_block = 4096;
	const uint64_t level_zero_block = 1;

	assert(btrfs_image_lookup(fs, "verity/large", &inode) == BTRFS_OK);
	key.objectid = inode.id.inode;
	/* The second level-0 block hashes the data from TREE_BLOCK_SPAN on. */
	key.offset = tree_block * (1 + level_zero_block);
	leaf = patch_leaf(fixture, fs, inode.id.tree, key, &item, &size);
	item[0] ^= 1;
	seal(fixture, leaf);
	expected = large_pattern(TREE_BLOCK_SPAN, 0);
	expect_read(fixture, "changed level-0 hash block", "verity/large", TREE_BLOCK_SPAN, 4096,
	    NULL, BTRFS_CORRUPT);
	expect_read(fixture, "data under an intact level-0 block", "verity/large", 0,
	    TREE_BLOCK_SPAN, expected, BTRFS_OK);
	clear_patches(fixture);
	free(expected);

	assert(btrfs_image_lookup(fs, "verity/sha512-1k", &inode) == BTRFS_OK);
	key.objectid = inode.id.inode;
	key.offset = 0;
	leaf = patch_leaf(fixture, fs, inode.id.tree, key, &item, &size);
	item[size - 1] ^= 0x80;
	seal(fixture, leaf);
	expect_read(fixture, "changed root tree block", "verity/sha512-1k", 1048576, 100, NULL,
	    BTRFS_CORRUPT);
	clear_patches(fixture);
}

/* A descriptor byte set to value, or with value toggled. */
struct descriptor_case {
	const char *name;
	size_t offset;
	uint8_t value;
	int toggle;
	enum btrfs_result wanted;
};

/* A 64-bit word of the size item set to value. */
struct size_case {
	const char *name;
	size_t offset;
	uint64_t value;
};

static void
patch_descriptor(struct fixture *fixture, struct btrfs_fs *fs, const char *path,
    const struct descriptor_case *change)
{
	struct btrfs_inode inode;
	struct bt_key key = { .type = BT_VERITY_DESC_ITEM, .offset = BT_VERITY_DESCRIPTOR_OFFSET };
	uint8_t *leaf;
	uint8_t *descriptor;
	size_t size;

	assert(btrfs_image_lookup(fs, path, &inode) == BTRFS_OK);
	key.objectid = inode.id.inode;
	leaf = patch_leaf(fixture, fs, inode.id.tree, key, &descriptor, &size);
	assert(size == sizeof(struct bt_disk_verity_descriptor));
	if (change->toggle) {
		descriptor[change->offset] ^= change->value;
	} else {
		descriptor[change->offset] = change->value;
	}
	seal(fixture, leaf);
	expect_read(fixture, change->name, path, 0, 100, NULL, change->wanted);
	clear_patches(fixture);
}

/* Fields of the descriptor of "tail" (256 bytes from VERITY_DESC offset 1,
 * after the 25-byte size item), and the salt of "sha512-salt". */
static void
descriptor_tests(struct fixture *fixture, struct btrfs_fs *fs)
{
	static const struct descriptor_case cases[] = {
		{ "descriptor version 2", offsetof(struct bt_disk_verity_descriptor, version), 2, 0,
		    BTRFS_UNSUPPORTED },
		{ "unknown hash algorithm",
		    offsetof(struct bt_disk_verity_descriptor, hash_algorithm), 3, 0,
		    BTRFS_UNSUPPORTED },
		{ "512-byte tree blocks", offsetof(struct bt_disk_verity_descriptor, log_blocksize),
		    9, 0, BTRFS_CORRUPT },
		{ "tree blocks above a sector",
		    offsetof(struct bt_disk_verity_descriptor, log_blocksize), 13, 0,
		    BTRFS_CORRUPT },
		{ "salt beyond its field", offsetof(struct bt_disk_verity_descriptor, salt_size),
		    33, 0, BTRFS_CORRUPT },
		{ "signature beyond the descriptor",
		    offsetof(struct bt_disk_verity_descriptor, sig_size), 1, 0, BTRFS_CORRUPT },
		{ "data size other than the inode's",
		    offsetof(struct bt_disk_verity_descriptor, data_size), 1, 1, BTRFS_CORRUPT },
		{ "reserved descriptor byte",
		    offsetof(struct bt_disk_verity_descriptor, reserved) + 143, 1, 0,
		    BTRFS_CORRUPT },
		{ "changed root hash", offsetof(struct bt_disk_verity_descriptor, root_hash) + 5,
		    0x10, 1, BTRFS_CORRUPT },
		/* Linux hashes salt_size bytes of the salt and reads no others. */
		{ "salt bytes beyond salt_size", offsetof(struct bt_disk_verity_descriptor, salt),
		    0x10, 1, BTRFS_OK },
	};
	static const struct descriptor_case salted = { "changed salt",
		offsetof(struct bt_disk_verity_descriptor, salt) + 31, 0x10, 1, BTRFS_CORRUPT };
	static const struct size_case sizes[] = {
		{ "descriptor size below the descriptor",
		    offsetof(struct bt_disk_verity_item, size),
		    sizeof(struct bt_disk_verity_descriptor) - 1 },
		{ "descriptor size beyond its items", offsetof(struct bt_disk_verity_item, size),
		    sizeof(struct bt_disk_verity_descriptor) + 1 },
		{ "descriptor size above the limit", offsetof(struct bt_disk_verity_item, size),
		    BT_VERITY_DESCRIPTOR_MAX + 1 },
		{ "reserved size item word", offsetof(struct bt_disk_verity_item, reserved), 1 },
	};
	struct btrfs_inode inode;
	struct bt_key key = { .type = BT_VERITY_DESC_ITEM, .offset = 0 };
	uint8_t *leaf;
	uint8_t *item;
	size_t size;
	size_t i;
	size_t j;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		patch_descriptor(fixture, fs, "verity/tail", &cases[i]);
	}
	patch_descriptor(fixture, fs, "verity/sha512-salt", &salted);
	assert(btrfs_image_lookup(fs, "verity/tail", &inode) == BTRFS_OK);
	key.objectid = inode.id.inode;
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		leaf = patch_leaf(fixture, fs, inode.id.tree, key, &item, &size);
		assert(size == sizeof(struct bt_disk_verity_item));
		for (j = 0; j < sizeof(uint64_t); j++) {
			item[sizes[i].offset + j] = (uint8_t)(sizes[i].value >> (8U * j));
		}
		seal(fixture, leaf);
		expect_read(fixture, sizes[i].name, "verity/tail", 0, 100, NULL, BTRFS_CORRUPT);
		clear_patches(fixture);
	}
}

static void
digest_tests(struct btrfs_fs *fs)
{
	struct btrfs_inode inode;
	uint8_t digest[BTRFS_VERITY_DIGEST_MAX];
	size_t length;
	unsigned algorithm;

	assert(btrfs_image_lookup(fs, "verity/sha512-salt", &inode) == BTRFS_OK);
	assert(btrfs_verity_digest(fs, &inode, &algorithm, digest, sizeof(digest), &length) ==
	    BTRFS_OK);
	assert(algorithm == BTRFS_VERITY_HASH_SHA512 && length == BT_SHA512_DIGEST);
	assert(btrfs_verity_digest(fs, &inode, &algorithm, digest, BT_SHA256_DIGEST, &length) ==
	    BTRFS_RANGE);
	assert(length == BT_SHA512_DIGEST);
	assert(btrfs_image_lookup(fs, "greeting", &inode) == BTRFS_OK);
	assert(btrfs_verity_digest(fs, &inode, &algorithm, digest, sizeof(digest), &length) ==
	    BTRFS_NOT_FOUND);
	assert(btrfs_image_lookup(fs, "verity", &inode) == BTRFS_OK);
	assert(btrfs_verity_digest(fs, &inode, &algorithm, digest, sizeof(digest), &length) ==
	    BTRFS_NOT_FOUND);
	puts("verity digests, short digest buffers and files without verity: PASS");
}

/* Unaligned reads across verification windows and tree blocks. */
static void
range_tests(struct btrfs_fs *fs)
{
	static const uint64_t ranges[][2] = { { 0, 1 }, { 4095, 2 }, { 65530, 70000 },
		{ TREE_BLOCK_SPAN - 3, 4103 }, { LARGE_PATTERN_BYTES - 5000, 5123 },
		{ LARGE_PATTERN_BYTES + 100, 100 } };
	struct btrfs_inode inode;
	uint8_t *buffer;
	uint8_t *expected;
	size_t completed;
	size_t wanted;
	size_t i;
	size_t j;

	assert(btrfs_image_lookup(fs, "verity/large", &inode) == BTRFS_OK);
	for (i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
		buffer = malloc((size_t)ranges[i][1]);
		assert(buffer != NULL);
		expected = large_pattern((size_t)ranges[i][1], ranges[i][0]);
		assert(btrfs_read(fs, &inode, ranges[i][0], buffer, (size_t)ranges[i][1],
			   &completed) == BTRFS_OK);
		wanted = ranges[i][0] >= inode.size	       ? 0
		    : inode.size - ranges[i][0] < ranges[i][1] ? (size_t)(inode.size - ranges[i][0])
							       : (size_t)ranges[i][1];
		assert(completed == wanted);
		for (j = 0; j < completed && ranges[i][0] + j < LARGE_PATTERN_BYTES; j++) {
			assert(buffer[j] == expected[j]);
		}
		free(expected);
		free(buffer);
	}
	puts("unaligned verified reads across windows and tree blocks: PASS");
}

/* Every allocation and I/O fault point of a verified read of "sha512-1k",
 * whose tree has three levels. */
static void
fault_tests(struct fixture *fixture, struct btrfs_fs *fs)
{
	struct btrfs_inode inode;
	uint8_t *buffer;
	uint8_t *expected;
	uint64_t before;
	uint64_t reads;
	uint64_t calls;
	uint64_t live;
	uint64_t i;
	size_t completed;
	size_t j;
	enum btrfs_result result;
	const size_t length = 300000;

	assert(btrfs_image_lookup(fs, "verity/sha512-1k", &inode) == BTRFS_OK);
	buffer = malloc(length);
	expected = large_pattern(length, 0);
	assert(buffer != NULL);
	before = fixture->image.allocations;
	reads = fixture->image.reads;
	assert(btrfs_read(fs, &inode, 0, buffer, length, &completed) == BTRFS_OK);
	assert(completed == length && memcmp(buffer, expected, length) == 0);
	calls = fixture->image.allocations - before;
	reads = fixture->image.reads - reads;
	live = fixture->image.live_allocations;
	for (i = 1; i <= calls; i++) {
		fixture->image.fail_allocate = fixture->image.allocations + i;
		result = btrfs_read(fs, &inode, 0, buffer, length, &completed);
		assert(result == BTRFS_NO_MEMORY);
		assert(fixture->image.live_allocations == live);
		for (j = 0; j < completed; j++) {
			assert(buffer[j] == expected[j]);
		}
	}
	fixture->image.fail_allocate = 0;
	for (i = 1; i <= reads; i++) {
		memset(buffer, READ_FILL, length);
		fixture->image.fail_read = fixture->image.reads + i;
		result = btrfs_read(fs, &inode, 0, buffer, length, &completed);
		assert(result == BTRFS_OK || result == BTRFS_IO);
		assert(fixture->image.live_allocations == live);
		for (j = 0; j < completed; j++) {
			assert(buffer[j] == expected[j]);
		}
	}
	fixture->image.fail_read = 0;
	free(expected);
	free(buffer);
	printf("verified read: %" PRIu64 " allocation and %" PRIu64
	       " I/O fault points fail cleanly: PASS\n",
	    calls, reads);
}

int
main(int argc, char **argv)
{
	struct fixture fixture = { 0 };
	struct btrfs_fs *fs;
	struct btrfs_info info;

	assert(argc == 2);
	assert(btrfs_image_open(argv[1], &fixture.image) == 0);
	assert(btrfs_mount(&fixture.image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	btrfs_get_info(fs, &info);
	assert((info.readonly_features & BT_COMPAT_RO_VERITY) != 0);
	fixture.checksum = info.checksum_type;
	fixture.node_size = info.node_size;
	data_tests(&fixture, fs);
	tree_tests(&fixture, fs);
	descriptor_tests(&fixture, fs);
	digest_tests(fs);
	range_tests(fs);
	fault_tests(&fixture, fs);
	btrfs_unmount(fs);
	btrfs_image_close(&fixture.image);
	assert(fixture.image.live_allocations == 0 && fixture.image.live_bytes == 0);
	puts("verity contracts: PASS");
	return 0;
}
