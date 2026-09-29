/* SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 200809L
#include "../adapters/posix/image.h"
#include "internal.h"
#include <btrfs/write.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WRITE_LIMIT 1024U
#define SECOND_SUPER_OFFSET (UINT64_C(64) * 1024 * 1024)
#define DEVICE_SECTOR_SIZE 512U
#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

struct saved_write {
	uint64_t offset;
	size_t length;
	uint8_t *bytes;
};

struct device {
	struct btrfs_environment original;
	struct saved_write writes[WRITE_LIMIT];
	size_t count;
	size_t flushes;
	size_t durable;
	size_t fail_write;
	size_t fail_flush;
	size_t persisted[WRITE_LIMIT];
	int selective;
	int visible;
};

static enum btrfs_result
read_device(void *context, uint64_t offset, void *bytes, size_t size)
{
	struct device *device = context;
	const struct saved_write *write;
	uint64_t start;
	uint64_t end;
	size_t i;
	size_t length;
	enum btrfs_result error;

	error = device->original.read(device->original.context, offset, bytes, size);
	if (error == BTRFS_OK && device->visible) {
		for (i = 0; i < device->count; i++) {
			write = &device->writes[i];
			length = device->selective ? device->persisted[i]
			    : i < device->durable  ? write->length
						   : 0;
			if (length == 0) {
				continue;
			}
			start = offset > write->offset ? offset : write->offset;
			end = offset + size < write->offset + length ? offset + size
								     : write->offset + length;
			if (start < end) {
				memcpy((uint8_t *)bytes + (start - offset),
				    write->bytes + (start - write->offset), (size_t)(end - start));
			}
		}
	}
	return error;
}

static void *
allocate(void *context, size_t size)
{
	struct device *device = context;

	return device->original.allocate(device->original.context, size);
}

static void
release(void *context, void *bytes, size_t size)
{
	struct device *device = context;

	device->original.release(device->original.context, bytes, size);
}

static enum btrfs_result
write_device(void *context, uint64_t offset, const void *bytes, size_t size)
{
	struct device *device = context;
	struct saved_write *write;

	if (device->count + 1 == device->fail_write) {
		return BTRFS_IO;
	}
	REQUIRE(device->count < WRITE_LIMIT);
	REQUIRE(
	    offset <= device->original.size_bytes && size <= device->original.size_bytes - offset);
	write = &device->writes[device->count++];
	write->offset = offset;
	write->length = size;
	write->bytes = malloc(size);
	REQUIRE(write->bytes != NULL);
	memcpy(write->bytes, bytes, size);
	return BTRFS_OK;
}

static enum btrfs_result
flush_device(void *context)
{
	struct device *device = context;

	device->flushes++;
	if (device->flushes == device->fail_flush) {
		return BTRFS_IO;
	}
	device->durable = device->count;
	return BTRFS_OK;
}

static void
clear(struct device *device)
{
	size_t i;

	for (i = 0; i < device->count; i++) {
		free(device->writes[i].bytes);
	}
	device->count = 0;
	device->flushes = 0;
	device->durable = 0;
	device->visible = 0;
	device->selective = 0;
}

static void
check_contents(struct btrfs_environment *env, const void *expected, size_t size)
{
	struct btrfs_fs *fs;
	struct btrfs_inode inode;
	struct btrfs_inode link;
	uint8_t buffer[2048];
	size_t completed;

	REQUIRE(btrfs_mount(env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/greeting", &inode) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/hardlink", &link) == BTRFS_OK);
	REQUIRE(inode.id.inode == link.id.inode && inode.links == 2 && inode.size == size);
	REQUIRE(inode.uid == 1001 && inode.gid == 1002 && (inode.mode & 07777U) == 0640);
	REQUIRE(btrfs_read(fs, &inode, 0, buffer, sizeof(buffer), &completed) == BTRFS_OK);
	REQUIRE(completed == size && memcmp(buffer, expected, size) == 0);
	REQUIRE(btrfs_image_lookup(fs, "/snapshot/value", &inode) == BTRFS_OK);
	REQUIRE(btrfs_read(fs, &inode, 0, buffer, sizeof(buffer), &completed) == BTRFS_OK);
	REQUIRE(completed == sizeof("snapshot original\n") - 1 &&
	    memcmp(buffer, "snapshot original\n", completed) == 0);
	btrfs_unmount(fs);
}

/* Export changed byte ranges for the independent Linux oracle, which applies
 * each crash prefix to its private copy of the original Linux fixture. */
static void
export_writes(const struct device *device, const char *directory)
{
	char path[4096];
	FILE *manifest;
	FILE *payload;
	size_t i;

	REQUIRE(snprintf(path, sizeof(path), "%s/writes.tsv", directory) < (int)sizeof(path));
	manifest = fopen(path, "wx");
	REQUIRE(manifest != NULL);
	for (i = 0; i < device->count; i++) {
		REQUIRE(snprintf(path, sizeof(path), "%s/write-%04zu.bin", directory, i) <
		    (int)sizeof(path));
		payload = fopen(path, "wx");
		REQUIRE(payload != NULL);
		REQUIRE(fwrite(device->writes[i].bytes, 1, device->writes[i].length, payload) ==
		    device->writes[i].length);
		REQUIRE(fclose(payload) == 0);
		REQUIRE(fprintf(manifest, "%llu\t%zu\twrite-%04zu.bin\n",
			    (unsigned long long)device->writes[i].offset, device->writes[i].length,
			    i) > 0);
	}
	REQUIRE(fclose(manifest) == 0);
}

static void
reordered_persistence(struct device *device, struct btrfs_environment *env, const char *original,
    size_t original_size, const char *replacement, size_t replacement_size)
{
	struct btrfs_fs *fs = NULL;
	size_t i;
	size_t metadata = device->count - 2;
	uint32_t seed = UINT32_C(0x142857);
	unsigned round;
	unsigned mirrors;

	REQUIRE(device->writes[metadata].offset == SECOND_SUPER_OFFSET);
	REQUIRE(device->writes[metadata + 1].offset == BT_SUPER_OFFSET);
	device->selective = 1;
	for (round = 0; round < 64; round++) {
		memset(device->persisted, 0, sizeof(device->persisted));
		for (i = 0; i < metadata; i++) {
			seed ^= seed << 13;
			seed ^= seed >> 17;
			seed ^= seed << 5;
			/* Volatile metadata may be missing, complete or sector-torn. No
			 * superblock may be issued before its persistence barrier succeeds. */
			device->persisted[i] =
			    (seed % (device->writes[i].length / DEVICE_SECTOR_SIZE + 1)) *
			    DEVICE_SECTOR_SIZE;
		}
		check_contents(env, original, original_size);
	}
	for (i = 0; i < metadata; i++) {
		device->persisted[i] = device->writes[i].length;
	}
	for (mirrors = 0; mirrors < 4; mirrors++) {
		device->persisted[metadata] = mirrors & 1 ? BT_SUPER_SIZE : 0;
		device->persisted[metadata + 1] = mirrors & 2 ? BT_SUPER_SIZE : 0;
		check_contents(env, mirrors & 2 ? replacement : original,
		    mirrors & 2 ? replacement_size : original_size);
	}
	/* A torn primary must fail explicitly. Automatic mirror recovery remains
	 * a separate feature; this is rejection coverage, not successful recovery. */
	device->persisted[metadata + 1] = DEVICE_SECTOR_SIZE;
	REQUIRE(btrfs_mount(env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_CORRUPT && fs == NULL);
	device->selective = 0;
}

int
main(int argc, char **argv)
{
	static const char original[] = "hello from Linux Btrfs\n";
	static const char replacement[] = "written by Machlin CoW transaction\n";
	struct btrfs_image image;
	struct device *device;
	struct btrfs_environment env;
	struct btrfs_write_environment writer;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct btrfs_inode inode;
	struct btrfs_time time = { 1700000000, 123456789 };
	enum btrfs_result result;
	size_t write_count;
	size_t flush_count;
	size_t totals[5] = { 1, 0, 0, 0, 0 };
	uint64_t allocation_start;
	uint64_t read_start;
	size_t point;
	size_t prefix;
	size_t saved;
	unsigned mode;

	REQUIRE(argc == 2 || argc == 3);
	REQUIRE(btrfs_image_open(argv[1], &image) == 0);
	device = calloc(1, sizeof(*device));
	REQUIRE(device != NULL);
	device->original = image.environment;
	env = image.environment;
	env.context = device;
	env.read = read_device;
	env.allocate = allocate;
	env.release = release;
	env.decompress = NULL;
	writer = (struct btrfs_write_environment){ device, write_device, flush_device };
	write_count = flush_count = 0;
	for (mode = 0; mode < 5; mode++) {
		for (point = 1; point <= totals[mode]; point++) {
			device->fail_write = mode == 1 ? point : 0;
			device->fail_flush = mode == 2 ? point : 0;
			REQUIRE(btrfs_mount(&env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
			REQUIRE(btrfs_image_lookup(fs, "/greeting", &inode) == BTRFS_OK);
			allocation_start = image.allocations;
			read_start = image.reads;
			image.fail_allocate = mode == 3 ? allocation_start + point : 0;
			image.fail_read = mode == 4 ? read_start + point : 0;
			result = btrfs_transaction_begin(fs, &writer, &transaction);
			if (result == BTRFS_OK) {
				result = btrfs_transaction_write_inline(transaction, inode.id,
				    replacement, sizeof(replacement) - 1, time);
			}
			if (result == BTRFS_OK) {
				result = btrfs_transaction_commit(transaction);
			}
			if (mode == 0 && result != BTRFS_OK) {
				fprintf(stderr, "commit: %s\n", btrfs_result_string(result));
			}
			REQUIRE(result ==
			    (mode == 0		? BTRFS_OK
				    : mode == 3 ? BTRFS_NO_MEMORY
						: BTRFS_IO));
			if (mode == 0) {
				totals[1] = device->count;
				totals[2] = device->flushes;
				totals[3] = (size_t)(image.allocations - allocation_start);
				totals[4] = (size_t)(image.reads - read_start);
			}
			saved = device->count;
			if (mode <= 2) {
				REQUIRE(btrfs_transaction_commit(transaction) ==
				    (mode == 0 ? BTRFS_READ_ONLY : BTRFS_IO));
			} else {
				REQUIRE(device->count == 0 && device->flushes == 0);
			}
			REQUIRE(device->count == saved);
			btrfs_transaction_destroy(transaction);
			btrfs_unmount(fs);
			image.fail_allocate = 0;
			image.fail_read = 0;
			REQUIRE(image.live_allocations == 0);
			check_contents(&env, original, sizeof(original) - 1);
			device->visible = 1;
			if (mode == 0) {
				write_count = device->count;
				flush_count = device->flushes;
				REQUIRE(write_count >= 4 && flush_count == 2);
				check_contents(&env, replacement, sizeof(replacement) - 1);
				if (argc == 3) {
					export_writes(device, argv[2]);
				}
				for (prefix = 0; prefix <= write_count; prefix++) {
					device->durable = prefix;
					check_contents(&env,
					    prefix == write_count ? replacement : original,
					    prefix == write_count ? sizeof(replacement) - 1
								  : sizeof(original) - 1);
				}
				reordered_persistence(device, &env, original, sizeof(original) - 1,
				    replacement, sizeof(replacement) - 1);
			} else {
				check_contents(&env, original, sizeof(original) - 1);
			}
			clear(device);
			REQUIRE(image.live_allocations == 0);
		}
	}
	/* Unsupported operation admission and abort do not issue media writes. */
	device->fail_write = device->fail_flush = 0;
	REQUIRE(btrfs_mount(&env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/greeting", &inode) == BTRFS_OK);
	REQUIRE(btrfs_transaction_begin(fs, &writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_write_inline(transaction,
		    (struct btrfs_object_id){ 256, inode.id.inode }, replacement,
		    sizeof(replacement) - 1, time) == BTRFS_UNSUPPORTED);
	REQUIRE(btrfs_transaction_write_inline(transaction, inode.id, replacement, 2049, time) ==
	    BTRFS_UNSUPPORTED);
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
	REQUIRE(device->count == 0 && device->flushes == 0);
	btrfs_transaction_destroy(transaction);
	REQUIRE(btrfs_transaction_begin(fs, &writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_write_inline(
		    transaction, inode.id, replacement, sizeof(replacement) - 1, time) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	REQUIRE(device->count == 0 && device->flushes == 0);
	REQUIRE(btrfs_transaction_begin(fs, &writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_write_inline(transaction, inode.id, NULL, 0, time) == BTRFS_OK);
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	device->visible = 1;
	check_contents(&env, "", 0);
	clear(device);
	REQUIRE(image.live_allocations == 0);
	btrfs_image_close(&image);
	free(device);
	printf("inline transactions: %zu writes, %zu barriers, %zu allocations, %zu reads; fault "
	       "sweeps and crash prefixes PASS\n",
	    write_count, flush_count, totals[3], totals[4]);
	return 0;
}
