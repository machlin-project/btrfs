/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"
#include "../adapters/posix/image.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct patch {
	uint64_t offset;
	const uint8_t *data;
	size_t size;
};

struct fixture {
	struct btrfs_image image;
	struct patch patches[2];
	size_t count;
};

static enum btrfs_result
patched_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct fixture *fixture = context;
	enum btrfs_result error;
	uint64_t start;
	uint64_t end;
	size_t i;

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

static void
set_le(void *field, uint64_t value, size_t size)
{
	uint8_t *bytes = field;
	size_t i;

	for (i = 0; i < size; i++) {
		bytes[i] = (uint8_t)(value >> (8U * i));
	}
}

#define SET(object, field, value) set_le(&(object)->field, (value), sizeof((object)->field))

static void
checksum(void *block, size_t size)
{
	uint32_t crc;

	crc = ~bt_crc32c(UINT32_MAX, (uint8_t *)block + BT_CSUM_SIZE, size - BT_CSUM_SIZE);
	set_le(block, crc, sizeof(crc));
}

static void
expect(struct fixture *fixture, const char *name, enum btrfs_result wanted, const char *path)
{
	struct btrfs_environment environment = fixture->image.environment;
	struct btrfs_fs *fs = NULL;
	struct btrfs_inode inode;
	uint8_t buffer[32];
	size_t completed;
	uint64_t live = fixture->image.live_allocations;
	enum btrfs_result result;

	environment.context = fixture;
	environment.read = patched_read;
	environment.allocate = allocate;
	environment.release = release;
	environment.decompress = NULL;
	result = btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs);
	if (result == BTRFS_OK && path != NULL) {
		result = btrfs_image_lookup(fs, path, &inode);
		if (result == BTRFS_OK) {
			result = btrfs_read(fs, &inode, 0, buffer, sizeof(buffer), &completed);
		}
	}
	btrfs_unmount(fs);
	if (result != wanted) {
		fprintf(stderr, "%s: expected %s, got %s\n", name, btrfs_result_string(wanted),
		    btrfs_result_string(result));
		abort();
	}
	assert(fixture->image.live_allocations == live);
	printf("%s: PASS (%s)\n", name, btrfs_result_string(result));
}

static void
super_tests(struct fixture *fixture)
{
	struct bt_disk_super *original;
	struct bt_disk_super *copy;
	struct bt_disk_chunk *chunk;
	struct bt_disk_stripe *stripe;

	original = malloc(sizeof(*original));
	copy = malloc(sizeof(*copy));
	assert(original != NULL && copy != NULL);
	assert(fixture->image.environment.read(
		   &fixture->image, BT_SUPER_OFFSET, original, sizeof(*original)) == BTRFS_OK);
	fixture->patches[0] = (struct patch){ BT_SUPER_OFFSET, (const void *)copy, sizeof(*copy) };
	fixture->count = 1;
	*copy = *original;
	copy->magic[0] ^= 1;
	expect(fixture, "foreign magic", BTRFS_NOT_BTRFS, NULL);
	*copy = *original;
	copy->csum[0] ^= 1;
	expect(fixture, "super checksum", BTRFS_CORRUPT, NULL);
	*copy = *original;
	SET(copy, checksum_type, 1);
	checksum(copy, sizeof(*copy));
	expect(fixture, "unsupported checksum is not CRC32C", BTRFS_UNSUPPORTED, NULL);
	*copy = *original;
	SET(copy, incompat, bt_u64(copy->incompat) | (UINT64_C(1) << 63));
	checksum(copy, sizeof(*copy));
	expect(fixture, "unknown incompatible feature", BTRFS_UNSUPPORTED, NULL);
	*copy = *original;
	SET(copy, log_root, bt_u64(copy->root));
	checksum(copy, sizeof(*copy));
	expect(fixture, "pending log is never replayed", BTRFS_RECOVERY_REQUIRED, NULL);
	*copy = *original;
	SET(copy, devices, 2);
	checksum(copy, sizeof(*copy));
	expect(fixture, "missing device set", BTRFS_UNSUPPORTED, NULL);
	*copy = *original;
	SET(copy, node_size, 6144);
	checksum(copy, sizeof(*copy));
	expect(fixture, "invalid node geometry", BTRFS_CORRUPT, NULL);
	*copy = *original;
	SET(copy, system_array_size, BT_SYSTEM_ARRAY_SIZE + 1);
	checksum(copy, sizeof(*copy));
	expect(fixture, "system array overflow", BTRFS_CORRUPT, NULL);
	*copy = *original;
	chunk = (void *)(copy->system_array + sizeof(struct bt_disk_key));
	SET(chunk, stripes, 0);
	checksum(copy, sizeof(*copy));
	expect(fixture, "zero stripes", BTRFS_CORRUPT, NULL);
	*copy = *original;
	stripe = (void *)(chunk + 1);
	stripe[1].offset = stripe[0].offset;
	checksum(copy, sizeof(*copy));
	expect(fixture, "DUP copies cannot alias", BTRFS_CORRUPT, NULL);
	*copy = *original;
	SET(stripe, device, UINT64_MAX);
	checksum(copy, sizeof(*copy));
	expect(fixture, "stripe device identity", BTRFS_CORRUPT, NULL);
	*copy = *original;
	SET(chunk, length, UINT64_MAX);
	checksum(copy, sizeof(*copy));
	expect(fixture, "chunk arithmetic overflow", BTRFS_CORRUPT, NULL);
	*copy = *original;
	SET(copy, compat_ro, UINT64_MAX);
	checksum(copy, sizeof(*copy));
	expect(fixture, "unknown readonly features remain readonly", BTRFS_OK, NULL);
	fixture->count = 0;
	free(copy);
	free(original);
}

static uint8_t *
record_patch(struct fixture *fixture, struct btrfs_fs *fs, struct btrfs_object_id id, uint8_t type,
    size_t *payload, size_t *size)
{
	struct bt_root root;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = id.inode, .type = type };
	const struct bt_disk_header *header;
	uint8_t *copy;
	uint64_t physical;
	unsigned mirrors;
	unsigned i;

	assert(bt_find_root(fs, id.tree, &root) == BTRFS_OK);
	bt_cursor_init(&cursor, fs, root);
	assert(bt_cursor_seek(&cursor, key, 0) == BTRFS_OK);
	assert(bt_cursor_record(&cursor, &record) == BTRFS_OK);
	assert(record.key.objectid == id.inode && record.key.type == type);
	copy = malloc(fs->info.node_size);
	assert(copy != NULL);
	memcpy(copy, cursor.blocks[0], fs->info.node_size);
	*payload = (size_t)(record.data - cursor.blocks[0]);
	*size = record.size;
	header = (const void *)copy;
	mirrors = 1;
	for (i = 0; i < mirrors; i++) {
		assert(bt_map(fs, bt_u64(header->bytenr), fs->info.node_size, BT_BLOCK_METADATA, i,
			   &physical, &mirrors) == BTRFS_OK);
		fixture->patches[i] = (struct patch){ physical, copy, fs->info.node_size };
	}
	fixture->count = mirrors;
	bt_cursor_fini(&cursor);
	return copy;
}

static void
tree_tests(struct fixture *fixture, struct btrfs_fs *fs)
{
	struct btrfs_inode root;
	struct btrfs_inode big;
	struct bt_disk_header *header;
	struct bt_disk_item *items;
	struct bt_disk_inode *inode;
	struct bt_disk_extent *extent;
	uint8_t *original;
	uint8_t *copy;
	uint8_t *sector;
	size_t payload;
	size_t size;
	size_t mirrors;
	uint64_t physical;
	unsigned copies;

	assert(btrfs_root(fs, &root) == BTRFS_OK);
	copy = record_patch(fixture, fs, root.id, BT_INODE_ITEM, &payload, &size);
	original = malloc(fs->info.node_size);
	assert(original != NULL);
	memcpy(original, copy, fs->info.node_size);
	mirrors = fixture->count;
	assert(mirrors == 2);
	copy[0] ^= 1;
	fixture->count = 1;
	expect(fixture, "metadata DUP checksum fallback", BTRFS_OK, NULL);
	fixture->count = mirrors;
	expect(fixture, "both metadata checksums corrupt", BTRFS_CORRUPT, NULL);
	memcpy(copy, original, fs->info.node_size);
	header = (void *)copy;
	SET(header, generation, fs->info.generation + 1);
	checksum(copy, fs->info.node_size);
	expect(fixture, "parent generation binding", BTRFS_CORRUPT, NULL);
	memcpy(copy, original, fs->info.node_size);
	SET(header, owner, BT_CHUNK_TREE);
	checksum(copy, fs->info.node_size);
	expect(fixture, "metadata tree provenance", BTRFS_CORRUPT, NULL);
	memcpy(copy, original, fs->info.node_size);
	SET(header, count, UINT32_MAX);
	checksum(copy, fs->info.node_size);
	expect(fixture, "untrusted item count", BTRFS_CORRUPT, NULL);
	memcpy(copy, original, fs->info.node_size);
	items = (void *)(header + 1);
	items[1].key = items[0].key;
	checksum(copy, fs->info.node_size);
	expect(fixture, "duplicate ordered keys", BTRFS_CORRUPT, NULL);
	memcpy(copy, original, fs->info.node_size);
	SET(&items[0], offset, 0);
	checksum(copy, fs->info.node_size);
	expect(fixture, "payload overlaps item table", BTRFS_CORRUPT, NULL);
	memcpy(copy, original, fs->info.node_size);
	SET(&items[0], size, UINT32_MAX);
	checksum(copy, fs->info.node_size);
	expect(fixture, "item arithmetic overflow", BTRFS_CORRUPT, NULL);
	memcpy(copy, original, fs->info.node_size);
	inode = (void *)(copy + payload);
	SET(&inode->mtime, nanoseconds, 1000000000U);
	checksum(copy, fs->info.node_size);
	expect(fixture, "invalid timestamp", BTRFS_CORRUPT, NULL);
	fixture->count = 0;
	free(copy);
	free(original);
	assert(btrfs_image_lookup(fs, "big", &big) == BTRFS_OK);
	copy = record_patch(fixture, fs, big.id, BT_EXTENT_DATA, &payload, &size);
	extent = (void *)(copy + payload);
	assert(size == sizeof(*extent) && extent->header.type == BT_EXTENT_REGULAR);
	assert(bt_map(fs, bt_u64(extent->disk_bytenr), fs->info.sector_size, BT_BLOCK_DATA, 0,
		   &physical, &copies) == BTRFS_OK);
	extent->header.compression = UINT8_MAX;
	checksum(copy, fs->info.node_size);
	expect(fixture, "unknown extent encoding", BTRFS_UNSUPPORTED, "big");
	fixture->count = 0;
	free(copy);
	sector = malloc(fs->info.sector_size);
	assert(sector != NULL);
	assert(fixture->image.environment.read(
		   &fixture->image, physical, sector, fs->info.sector_size) == BTRFS_OK);
	sector[0] ^= 1;
	fixture->patches[0] = (struct patch){ physical, sector, fs->info.sector_size };
	fixture->count = 1;
	expect(fixture, "file data checksum before publication", BTRFS_CORRUPT, "big");
	fixture->count = 0;
	free(sector);
}

static void
stream_tests(struct fixture *fixture)
{
	struct btrfs_fs *fs;
	struct btrfs_inode directory;
	struct btrfs_directory *stream;
	struct btrfs_dir_entry entry;
	uint64_t cookie = 0;
	uint64_t saved = 0;
	uint64_t reads;
	uint64_t allocations;
	size_t entries = 0;
	enum btrfs_result result;

	assert(btrfs_mount(&fixture->image.environment, 5, &fs) == BTRFS_OK);
	assert(btrfs_image_lookup(fs, "many", &directory) == BTRFS_OK);
	reads = fixture->image.reads;
	allocations = fixture->image.allocations;
	assert(btrfs_directory_open(fs, &directory, 0, &stream) == BTRFS_OK);
	while ((result = btrfs_directory_next(stream, &entry, &cookie)) == BTRFS_OK) {
		assert(cookie > saved);
		saved = cookie;
		entries++;
	}
	assert(result == BTRFS_NOT_FOUND && entries == 700);
	btrfs_directory_close(stream);
	reads = fixture->image.reads - reads;
	allocations = fixture->image.allocations - allocations;
	printf("700-entry stream: %" PRIu64 " I/O calls, %" PRIu64 " allocations\n", reads,
	    allocations);
	assert(reads <= 32 && allocations <= 8);
	assert(btrfs_directory_open(fs, &directory, saved, &stream) == BTRFS_OK);
	assert(btrfs_directory_next(stream, &entry, &cookie) == BTRFS_NOT_FOUND);
	btrfs_directory_close(stream);
	assert(btrfs_directory_open(fs, &directory, 350, &stream) == BTRFS_OK);
	assert(btrfs_directory_next(stream, &entry, &cookie) == BTRFS_OK && cookie == 351);
	btrfs_directory_close(stream);
	assert(btrfs_next_dir(fs, &directory, &cookie, &entry) == BTRFS_OK && cookie == 352);
	btrfs_unmount(fs);
	assert(fixture->image.live_allocations == 0 && fixture->image.live_bytes == 0);
	puts("streamed directory resume, EOF and structural budgets: PASS");
}

static void
fault_tests(struct fixture *fixture)
{
	struct btrfs_fs *fs = NULL;
	struct btrfs_inode inode;
	uint64_t calls;
	uint64_t i;
	uint64_t before;
	uint64_t live;
	uint64_t reads;
	uint8_t *buffer;
	size_t completed;
	size_t j;
	enum btrfs_result result;
	const size_t length = 4U * 1024U * 1024U;

	before = fixture->image.allocations;
	assert(btrfs_mount(&fixture->image.environment, 5, &fs) == BTRFS_OK);
	calls = fixture->image.allocations - before;
	btrfs_unmount(fs);
	for (i = 1; i <= calls; i++) {
		fixture->image.fail_allocate = fixture->image.allocations + i;
		fs = (void *)(uintptr_t)1;
		result = btrfs_mount(&fixture->image.environment, 5, &fs);
		assert(result == BTRFS_NO_MEMORY && fs == NULL);
		assert(fixture->image.live_allocations == 0 && fixture->image.live_bytes == 0);
	}
	fixture->image.fail_allocate = 0;
	before = fixture->image.reads;
	assert(btrfs_mount(&fixture->image.environment, 5, &fs) == BTRFS_OK);
	calls = fixture->image.reads - before;
	btrfs_unmount(fs);
	for (i = 1; i <= calls; i++) {
		fixture->image.fail_read = fixture->image.reads + i;
		result = btrfs_mount(&fixture->image.environment, 5, &fs);
		assert(result == BTRFS_OK || result == BTRFS_IO);
		btrfs_unmount(fs);
		assert(fixture->image.live_allocations == 0 && fixture->image.live_bytes == 0);
	}
	fixture->image.fail_read = 0;
	assert(btrfs_mount(&fixture->image.environment, 5, &fs) == BTRFS_OK);
	assert(btrfs_image_lookup(fs, "big", &inode) == BTRFS_OK);
	buffer = malloc(length);
	assert(buffer != NULL);
	before = fixture->image.allocations;
	reads = fixture->image.reads;
	assert(btrfs_read(fs, &inode, 0, buffer, length, &completed) == BTRFS_OK);
	assert(completed == length);
	calls = fixture->image.allocations - before;
	reads = fixture->image.reads - reads;
	for (j = 0; j < length; j++) {
		assert(buffer[j] == (uint8_t)j);
	}
	/* Structural budgets, not timing claims: I/O and allocations must scale
	 * with runs and tree nodes, not with 1024 individual data sectors. */
	printf(
	    "4 MiB verified read: %" PRIu64 " I/O calls, %" PRIu64 " allocations\n", reads, calls);
	assert(reads <= 64 && calls <= 16);
	live = fixture->image.live_allocations;
	for (i = 1; i <= calls; i++) {
		fixture->image.fail_allocate = fixture->image.allocations + i;
		result = btrfs_read(fs, &inode, 0, buffer, length, &completed);
		assert(result == BTRFS_NO_MEMORY);
		assert(fixture->image.live_allocations == live);
		for (j = 0; j < completed; j++) {
			assert(buffer[j] == (uint8_t)j);
		}
	}
	fixture->image.fail_allocate = 0;
	for (i = 1; i <= reads; i++) {
		memset(buffer, 0xaa, length);
		fixture->image.fail_read = fixture->image.reads + i;
		result = btrfs_read(fs, &inode, 0, buffer, length, &completed);
		assert(result == BTRFS_OK || result == BTRFS_IO);
		assert(fixture->image.live_allocations == live);
		for (j = 0; j < completed; j++) {
			assert(buffer[j] == (uint8_t)j);
		}
	}
	fixture->image.fail_read = 0;
	free(buffer);
	btrfs_unmount(fs);
	assert(fixture->image.live_allocations == 0 && fixture->image.live_bytes == 0);
	puts("exhaustive mount/read allocation and I/O fault points; range I/O budgets: PASS");
}

int
main(int argc, char **argv)
{
	struct fixture fixture = { 0 };
	struct btrfs_fs *fs;

	assert(argc == 2);
	assert(btrfs_image_open(argv[1], &fixture.image) == 0);
	super_tests(&fixture);
	assert(btrfs_mount(&fixture.image.environment, 5, &fs) == BTRFS_OK);
	tree_tests(&fixture, fs);
	btrfs_unmount(fs);
	fault_tests(&fixture);
	stream_tests(&fixture);
	btrfs_image_close(&fixture.image);
	puts("adversarial contracts: PASS");
	return 0;
}
