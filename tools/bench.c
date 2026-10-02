/* SPDX-License-Identifier: BSD-3-Clause */
/* Portable cost measurements on an image file: wall time per operation with
 * the backend reads, read bytes and allocations it took. Host page-cache
 * numbers measure this implementation's CPU and call costs, not a mounted
 * filesystem against Linux (see docs/PERFORMANCE.md). Build without
 * sanitizers for meaningful times. A writable copy of the image is required
 * for the write series, which modifies it. */
#define _POSIX_C_SOURCE 200809L
#include "../adapters/posix/image.h"
#include <btrfs/write.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct sample {
	struct timespec start;
	uint64_t reads;
	uint64_t bytes;
	uint64_t allocations;
};

static struct btrfs_image image;

static void
begin_sample(struct sample *sample)
{
	sample->reads = atomic_load(&image.reads);
	sample->bytes = atomic_load(&image.bytes_read);
	sample->allocations = atomic_load(&image.allocations);
	clock_gettime(CLOCK_MONOTONIC, &sample->start);
}

static void
end_sample(const struct sample *sample, const char *name, uint64_t operations, uint64_t bytes)
{
	struct timespec end;
	double seconds;

	clock_gettime(CLOCK_MONOTONIC, &end);
	seconds = (double)(end.tv_sec - sample->start.tv_sec) +
	    (double)(end.tv_nsec - sample->start.tv_nsec) / 1e9;
	printf("%-28s %10.2f us/op %9.1f MB/s %8.1f reads/op %9.1f KiB/op %8.1f allocs/op\n", name,
	    seconds * 1e6 / (double)operations, bytes == 0 ? 0.0 : (double)bytes / seconds / 1e6,
	    (double)(atomic_load(&image.reads) - sample->reads) / (double)operations,
	    (double)(atomic_load(&image.bytes_read) - sample->bytes) / 1024.0 / (double)operations,
	    (double)(atomic_load(&image.allocations) - sample->allocations) / (double)operations);
}

static int
lookup(struct btrfs_fs *fs, const char *path, struct btrfs_inode *inode)
{
	return btrfs_image_lookup(fs, path, inode) == BTRFS_OK;
}

static void
read_series(struct btrfs_fs *fs)
{
	struct btrfs_inode inode;
	struct btrfs_directory *stream;
	struct btrfs_dir_entry entry;
	struct sample sample;
	uint8_t *buffer;
	uint64_t cookie;
	uint64_t offset;
	size_t completed;
	unsigned round;
	unsigned i;
	char path[64];

	buffer = malloc(1024 * 1024);
	if (buffer == NULL || !lookup(fs, "/big", &inode)) {
		fprintf(stderr, "the image needs /big\n");
		exit(1);
	}
	begin_sample(&sample);
	for (round = 0; round < 64; round++) {
		for (offset = 0; offset < inode.size; offset += 1024 * 1024) {
			(void)btrfs_read(fs, &inode, offset, buffer, 1024 * 1024, &completed);
		}
	}
	end_sample(&sample, "read 1 MiB sequential", 64 * ((inode.size + 1048575) / 1048576),
	    64 * inode.size);
	begin_sample(&sample);
	for (i = 0; i < 20000; i++) {
		offset = ((uint64_t)i * 2654435761U) % (inode.size / 4096) * 4096;
		(void)btrfs_read(fs, &inode, offset, buffer, 4096, &completed);
	}
	end_sample(&sample, "read 4 KiB random", 20000, 20000 * 4096);
	begin_sample(&sample);
	for (i = 0; i < 20000; i++) {
		snprintf(path, sizeof(path), "/many/entry-%04u", (i * 7919U) % 700U);
		(void)lookup(fs, path, &inode);
	}
	end_sample(&sample, "lookup /many/entry-N", 20000, 0);
	if (!lookup(fs, "/many", &inode)) {
		exit(1);
	}
	begin_sample(&sample);
	for (round = 0; round < 200; round++) {
		if (btrfs_directory_open(fs, &inode, 0, &stream) != BTRFS_OK) {
			exit(1);
		}
		while (btrfs_directory_next(stream, &entry, &cookie) == BTRFS_OK) {
		}
		btrfs_directory_close(stream);
	}
	end_sample(&sample, "enumerate 700 entries", 200, 0);
	begin_sample(&sample);
	for (round = 0; round < 50; round++) {
		if (btrfs_directory_open(fs, &inode, 0, &stream) != BTRFS_OK) {
			exit(1);
		}
		while (btrfs_directory_next(stream, &entry, &cookie) == BTRFS_OK) {
			struct btrfs_inode child;

			(void)btrfs_get_inode(fs, entry.id, &child);
		}
		btrfs_directory_close(stream);
	}
	end_sample(&sample, "enumerate 700 with stat", 50, 0);
	free(buffer);
}

static enum btrfs_result
image_write_flush(void *context)
{
	(void)context;
	return BTRFS_OK;
}

static void
write_series(const char *path, struct btrfs_cache *cache)
{
	struct btrfs_write_environment writer;
	struct btrfs_new_inode attributes;
	struct btrfs_transaction *transaction;
	struct btrfs_object_id parent;
	struct btrfs_object_id id;
	struct btrfs_inode root;
	struct btrfs_fs *fs;
	struct sample sample;
	struct btrfs_time time = { 1800000000, 0 };
	uint8_t *data;
	char name[32];
	unsigned i;

	btrfs_image_close(&image);
	if (btrfs_image_open_writable(path, &image) != 0) {
		perror("open writable");
		exit(1);
	}
	btrfs_image_writer(&image, &writer);
	image.environment.cache = cache;
	/* Barriers cost the host's fsync, which this series does not measure. */
	writer.flush = image_write_flush;
	data = malloc(8 * 1024 * 1024);
	if (data == NULL) {
		exit(1);
	}
	memset(data, 0x5a, 8 * 1024 * 1024);
	memset(&attributes, 0, sizeof(attributes));
	attributes.mode = BTRFS_MODE_REGULAR | 0644;
	attributes.time = time;
	begin_sample(&sample);
	for (i = 0; i < 200; i++) {
		if (btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) != BTRFS_OK ||
		    btrfs_root(fs, &root) != BTRFS_OK ||
		    btrfs_transaction_begin(fs, &writer, &transaction) != BTRFS_OK) {
			exit(1);
		}
		parent = root.id;
		snprintf(name, sizeof(name), "bench-%04u", i);
		if (btrfs_transaction_create(
			transaction, parent, name, strlen(name), &attributes, &id) != BTRFS_OK ||
		    btrfs_transaction_commit(transaction) != BTRFS_OK) {
			exit(1);
		}
		btrfs_transaction_destroy(transaction);
		btrfs_unmount(fs);
	}
	end_sample(&sample, "mount+create+commit", 200, 0);
	begin_sample(&sample);
	for (i = 0; i < 4; i++) {
		if (btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) != BTRFS_OK ||
		    btrfs_root(fs, &root) != BTRFS_OK ||
		    btrfs_transaction_begin(fs, &writer, &transaction) != BTRFS_OK) {
			exit(1);
		}
		snprintf(name, sizeof(name), "stream-%u", i);
		if (btrfs_transaction_create(
			transaction, root.id, name, strlen(name), &attributes, &id) != BTRFS_OK ||
		    btrfs_transaction_write(transaction, id, 0, data, 8 * 1024 * 1024, time) !=
			BTRFS_OK ||
		    btrfs_transaction_commit(transaction) != BTRFS_OK) {
			exit(1);
		}
		btrfs_transaction_destroy(transaction);
		btrfs_unmount(fs);
	}
	end_sample(&sample, "write 8 MiB file", 4, 4 * 8 * 1024 * 1024);
	free(data);
}

int
main(int argc, char **argv)
{
	struct btrfs_cache *cache = NULL;
	struct btrfs_fs *fs;
	uint64_t hits;
	uint64_t misses;
	size_t cache_bytes = 0;
	int write = 0;
	int i;

	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--write") == 0) {
			write = 1;
		} else if (strcmp(argv[i], "--cache") == 0 && i + 1 < argc) {
			cache_bytes = (size_t)strtoull(argv[++i], NULL, 0) * 1024 * 1024;
		} else {
			argc = 0;
		}
	}
	if (argc < 2) {
		fprintf(stderr, "Usage: btrfs-bench IMAGE [--cache MiB] [--write]\n");
		return 2;
	}
	if (btrfs_image_open(argv[1], &image) != 0) {
		perror("open image");
		return 1;
	}
	if (cache_bytes != 0 &&
	    btrfs_cache_create(&image.environment, NULL, cache_bytes, &cache) != BTRFS_OK) {
		return 1;
	}
	image.environment.cache = cache;
	if (btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) != BTRFS_OK) {
		fprintf(stderr, "cannot mount %s\n", argv[1]);
		return 1;
	}
	read_series(fs);
	btrfs_unmount(fs);
	if (write) {
		write_series(argv[1], cache);
	}
	if (cache != NULL) {
		btrfs_cache_counts(cache, &hits, &misses);
		printf("node cache: %llu hits, %llu misses\n", (unsigned long long)hits,
		    (unsigned long long)misses);
		btrfs_cache_destroy(cache);
	}
	btrfs_image_close(&image);
	return 0;
}
