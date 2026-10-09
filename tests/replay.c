/* SPDX-License-Identifier: BSD-3-Clause */
/* Tree-log replay of a Linux image under faults. The fixture is read through
 * a device that keeps every write in memory, so each run starts from it.
 * Every allocation and read fault point fails without a leak. Every write and barrier fault point,
 * and a power cut after every write (keeping either every acknowledged write or only those a
 * barrier made durable), leaves a device from which the adapters' sequence
 * (replay; superblock recovery when copies disagree or no log is pending;
 * open again) reaches the namespace of an uninterrupted replay. */
#include "../adapters/posix/image.h"
#include <btrfs/write.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Rounds of the adapters' replay and recovery sequence. */
#define OPEN_ROUNDS 2U
#define WALK_DEPTH 64U
#define WALK_BUFFER (1024U * 1024U)
#define FNV_OFFSET UINT64_C(14695981039346656037)
#define FNV_PRIME UINT64_C(1099511628211)

struct span {
	uint64_t offset;
	size_t size;
	uint8_t *data;
};

struct device {
	struct btrfs_image image;
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct span *spans;
	size_t count;
	size_t capacity;
	uint64_t writes;
	uint64_t flushes;
	uint64_t fail_write;
	uint64_t fail_flush;
	/* With cutting, writes after the cut-th are acknowledged and lost. */
	int cutting;
	uint64_t cut;
	/* Spans that a barrier before the cut made durable. */
	size_t durable;
};

static enum btrfs_result
device_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct device *device = context;
	const struct span *span;
	uint64_t start;
	uint64_t end;
	size_t i;
	enum btrfs_result error;

	error = device->image.environment.read(&device->image, offset, buffer, length);
	for (i = 0; error == BTRFS_OK && i < device->count; i++) {
		span = &device->spans[i];
		start = offset > span->offset ? offset : span->offset;
		end = offset + length < span->offset + span->size ? offset + length
								  : span->offset + span->size;
		if (start < end) {
			memcpy((uint8_t *)buffer + (start - offset),
			    span->data + (start - span->offset), (size_t)(end - start));
		}
	}
	return error;
}

static void *
device_allocate(void *context, size_t size)
{
	struct device *device = context;

	return device->image.environment.allocate(&device->image, size);
}

static void
device_release(void *context, void *buffer, size_t size)
{
	struct device *device = context;

	device->image.environment.release(&device->image, buffer, size);
}

static enum btrfs_result
device_write(void *context, uint64_t offset, const void *bytes, size_t size)
{
	struct device *device = context;
	struct span *span;

	device->writes++;
	if (device->writes == device->fail_write) {
		return BTRFS_IO;
	}
	if (device->cutting && device->writes > device->cut) {
		return BTRFS_OK;
	}
	if (device->count == device->capacity) {
		device->capacity = device->capacity == 0 ? 64 : device->capacity * 2;
		device->spans = realloc(device->spans, device->capacity * sizeof(*device->spans));
		assert(device->spans != NULL);
	}
	span = &device->spans[device->count++];
	span->offset = offset;
	span->size = size;
	span->data = malloc(size);
	assert(span->data != NULL);
	memcpy(span->data, bytes, size);
	return BTRFS_OK;
}

static enum btrfs_result
device_flush(void *context)
{
	struct device *device = context;

	device->flushes++;
	if (device->flushes == device->fail_flush) {
		return BTRFS_IO;
	}
	if (!device->cutting || device->writes <= device->cut) {
		device->durable = device->count;
	}
	return BTRFS_OK;
}

static void
device_drop(struct device *device, size_t keep)
{
	while (device->count > keep) {
		free(device->spans[--device->count].data);
	}
	device->durable = device->durable < keep ? device->durable : keep;
}

/* The fixture again, with no faults armed. */
static void
device_reset(struct device *device)
{
	device_drop(device, 0);
	device->writes = 0;
	device->flushes = 0;
	device->fail_write = 0;
	device->fail_flush = 0;
	device->cutting = 0;
	device->cut = 0;
	device->durable = 0;
	device->image.fail_read = 0;
	device->image.fail_allocate = 0;
}

static void
device_open(struct device *device, const char *path)
{
	memset(device, 0, sizeof(*device));
	assert(btrfs_image_open(path, &device->image) == 0);
	device->environment = device->image.environment;
	device->environment.context = device;
	device->environment.read = device_read;
	device->environment.allocate = device_allocate;
	device->environment.release = device_release;
	device->writer.context = device;
	device->writer.write = device_write;
	device->writer.flush = device_flush;
	device->writer.compression = BTRFS_COMPRESSION_NONE;
}

static void
fold(uint64_t *hash, const void *bytes, size_t size)
{
	const uint8_t *byte = bytes;
	size_t i;

	for (i = 0; i < size; i++) {
		*hash = (*hash ^ byte[i]) * FNV_PRIME;
	}
}

static void
fold_u64(uint64_t *hash, uint64_t value)
{
	fold(hash, &value, sizeof(value));
}

/* The attributes, xattrs and bytes of inode, then a directory's entries. */
static void
walk(struct btrfs_fs *fs, const struct btrfs_inode *inode, unsigned depth, uint8_t *buffer,
    uint8_t *value, uint64_t *hash)
{
	struct btrfs_directory *stream;
	struct btrfs_dir_entry entry;
	struct btrfs_inode child;
	uint64_t cookie = 0;
	uint64_t offset = 0;
	size_t names;
	size_t length;
	size_t position;
	size_t name_length;
	enum btrfs_result error;

	assert(depth < WALK_DEPTH);
	fold_u64(hash, inode->id.tree);
	fold_u64(hash, inode->id.inode);
	fold_u64(hash, inode->mode);
	fold_u64(hash, inode->uid);
	fold_u64(hash, inode->gid);
	fold_u64(hash, inode->links);
	fold_u64(hash, inode->size);
	fold_u64(hash, (uint64_t)inode->modify_time.seconds);
	assert(btrfs_list_xattrs(fs, inode, buffer, WALK_BUFFER, &names) == BTRFS_OK);
	for (position = 0; position < names; position += name_length + 1) {
		name_length = strnlen((const char *)buffer + position, names - position);
		assert(btrfs_get_xattr(fs, inode, buffer + position, name_length, value,
			   WALK_BUFFER, &length) == BTRFS_OK);
		fold(hash, buffer + position, name_length + 1);
		fold(hash, value, length);
	}
	if ((inode->mode & BTRFS_MODE_TYPE) != BTRFS_MODE_DIRECTORY) {
		while (offset < inode->size) {
			assert(btrfs_read(fs, inode, offset, buffer, WALK_BUFFER, &length) ==
			    BTRFS_OK);
			assert(length != 0);
			fold(hash, buffer, length);
			offset += length;
		}
		return;
	}
	assert(btrfs_directory_open(fs, inode, cookie, &stream) == BTRFS_OK);
	while ((error = btrfs_directory_next(stream, &entry, &cookie)) == BTRFS_OK) {
		fold(hash, entry.name, entry.name_length);
		assert(btrfs_directory_inode(stream, &entry, &child) == BTRFS_OK);
		walk(fs, &child, depth + 1, buffer, value, hash);
	}
	assert(error == BTRFS_NOT_FOUND);
	btrfs_directory_close(stream);
}

static uint64_t
namespace_digest(struct device *device)
{
	struct btrfs_fs *fs = NULL;
	struct btrfs_inode root;
	uint64_t hash = FNV_OFFSET;
	uint8_t *buffer = malloc(WALK_BUFFER);
	uint8_t *value = malloc(WALK_BUFFER);

	assert(buffer != NULL && value != NULL);
	assert(btrfs_mount(&device->environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	assert(btrfs_root(fs, &root) == BTRFS_OK);
	walk(fs, &root, 0, buffer, value, &hash);
	btrfs_unmount(fs);
	free(buffer);
	free(value);
	return hash;
}

/* Whether a writable open would admit the device now. */
static enum btrfs_result
open_writable(struct device *device)
{
	struct btrfs_transaction *transaction = NULL;
	struct btrfs_fs *fs = NULL;
	enum btrfs_result error;

	error = btrfs_mount(&device->environment, BTRFS_TOP_LEVEL_TREE, &fs);
	if (error == BTRFS_OK) {
		error = btrfs_transaction_begin(fs, &device->writer, &transaction);
	}
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	return error;
}

static enum btrfs_result
replay(struct device *device, const struct btrfs_write_environment *writer,
    struct btrfs_replay_report *report)
{
	const struct btrfs_time now = { 1791600000, 0 };

	return btrfs_replay_log(&device->environment, writer, now, report);
}

/* The adapters' writable open, without faults. */
static void
settle(struct device *device, uint64_t expected, const char *what, uint64_t point)
{
	struct btrfs_replay_report report;
	struct btrfs_recovery_report recovery;
	unsigned round;
	enum btrfs_result error;

	error = open_writable(device);
	for (round = 0; error == BTRFS_RECOVERY_REQUIRED && round < OPEN_ROUNDS; round++) {
		error = replay(device, &device->writer, &report);
		if (error == BTRFS_NOT_FOUND || error == BTRFS_RECOVERY_REQUIRED) {
			error = btrfs_recover_supers(
			    &device->environment, &device->writer, 0, &recovery);
		}
		if (error == BTRFS_OK) {
			error = open_writable(device);
		}
	}
	if (error != BTRFS_OK || namespace_digest(device) != expected) {
		fprintf(stderr, "%s %" PRIu64 ": settled to %s, not the replayed namespace\n", what,
		    point, btrfs_result_string(error));
		abort();
	}
	assert(device->image.live_allocations == 0 && device->image.live_bytes == 0);
}

int
main(int argc, char **argv)
{
	struct btrfs_replay_report report;
	struct btrfs_info info;
	struct btrfs_info replayed;
	struct btrfs_fs *fs = NULL;
	struct device device;
	uint64_t expected;
	uint64_t allocations;
	uint64_t reads;
	uint64_t writes;
	uint64_t flushes;
	uint64_t i;
	unsigned durable;
	enum btrfs_result error;

	assert(argc == 2);
	device_open(&device, argv[1]);

	/* A probe recognizes the volume that no read-only mount admits. */
	assert(
	    btrfs_mount(&device.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_RECOVERY_REQUIRED);
	assert(btrfs_identify(&device.environment, &info) == BTRFS_OK);
	assert(strcmp(info.label, "machlin-btrfs") == 0 && info.generation != 0 &&
	    info.default_tree == 0);

	/* An inspection replays everything and writes nothing. */
	assert(replay(&device, NULL, &report) == BTRFS_RECOVERY_REQUIRED);
	assert(report.logs != 0 && device.count == 0);
	assert(open_writable(&device) == BTRFS_RECOVERY_REQUIRED);

	allocations = device.image.allocations;
	reads = device.image.reads;
	assert(replay(&device, &device.writer, &report) == BTRFS_OK);
	allocations = device.image.allocations - allocations;
	reads = device.image.reads - reads;
	writes = device.writes;
	flushes = device.flushes;
	assert(device.image.live_allocations == 0 && device.image.live_bytes == 0);
	assert(replay(&device, &device.writer, &report) == BTRFS_NOT_FOUND);
	assert(open_writable(&device) == BTRFS_OK);
	assert(btrfs_identify(&device.environment, &replayed) == BTRFS_OK);
	assert(replayed.generation == info.generation + 1 &&
	    memcmp(replayed.uuid, info.uuid, sizeof(info.uuid)) == 0);
	expected = namespace_digest(&device);
	printf("replay: %" PRIu64 " allocations, %" PRIu64 " reads, %" PRIu64 " writes, %" PRIu64
	       " barriers\n",
	    allocations, reads, writes, flushes);

	for (i = 1; i <= allocations; i++) {
		device_reset(&device);
		device.image.fail_allocate = device.image.allocations + i;
		error = replay(&device, &device.writer, &report);
		assert(error == BTRFS_NO_MEMORY);
		assert(device.image.live_allocations == 0 && device.image.live_bytes == 0);
		device.image.fail_allocate = 0;
		settle(&device, expected, "allocation fault", i);
	}
	puts("every allocation fault point: PASS");
	for (i = 1; i <= reads; i++) {
		device_reset(&device);
		device.image.fail_read = device.image.reads + i;
		error = replay(&device, &device.writer, &report);
		/* DUP metadata has a second copy to read. */
		assert(error == BTRFS_OK || error == BTRFS_IO);
		assert(device.image.live_allocations == 0 && device.image.live_bytes == 0);
		device.image.fail_read = 0;
		settle(&device, expected, "read fault", i);
	}
	puts("every read fault point: PASS");
	for (i = 1; i <= writes; i++) {
		device_reset(&device);
		device.fail_write = i;
		assert(replay(&device, &device.writer, &report) == BTRFS_IO);
		assert(device.image.live_allocations == 0 && device.image.live_bytes == 0);
		device.fail_write = 0;
		settle(&device, expected, "write fault", i);
	}
	for (i = 1; i <= flushes; i++) {
		device_reset(&device);
		device.fail_flush = i;
		assert(replay(&device, &device.writer, &report) == BTRFS_IO);
		assert(device.image.live_allocations == 0 && device.image.live_bytes == 0);
		device.fail_flush = 0;
		settle(&device, expected, "barrier fault", i);
	}
	puts("every write and barrier fault point: PASS");
	for (durable = 0; durable < 2; durable++) {
		for (i = 0; i <= writes; i++) {
			device_reset(&device);
			device.cutting = 1;
			device.cut = i;
			assert(replay(&device, &device.writer, &report) == BTRFS_OK);
			if (durable) {
				device_drop(&device, device.durable);
			}
			device.cutting = 0;
			settle(&device, expected, durable ? "power cut (durable)" : "power cut", i);
		}
	}
	puts("power cut after every write: PASS");
	device_reset(&device);
	free(device.spans);
	btrfs_image_close(&device.image);
	puts("tree-log replay faults: PASS");
	return 0;
}
