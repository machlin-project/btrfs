/* SPDX-License-Identifier: BSD-3-Clause */
#include "../adapters/posix/image.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Entries of the fixture's "many" directory. */
#define MANY_ENTRIES 700U

struct worker {
	struct btrfs_fs *fs;
	unsigned index;
};

/* Reads the next entry of a stream kept open across every other operation, so
 * its pinned cache nodes must survive the other readers' evictions. */
static void
next_held(struct btrfs_fs *fs, const struct btrfs_inode *directory, struct btrfs_directory **stream,
    uint64_t *cookie)
{
	struct btrfs_dir_entry entry;
	struct btrfs_inode found;
	struct btrfs_inode child;
	uint64_t next = 0;
	enum btrfs_result error;

	error = btrfs_directory_next(*stream, &entry, &next);
	if (error == BTRFS_NOT_FOUND) {
		btrfs_directory_close(*stream);
		*cookie = 0;
		assert(btrfs_directory_open(fs, directory, 0, stream) == BTRFS_OK);
		error = btrfs_directory_next(*stream, &entry, &next);
	}
	assert(error == BTRFS_OK);
	/* DIR_INDEX keys of "many" are consecutive from 2. */
	assert(next == (*cookie < 2 ? 2 : *cookie) + 1);
	*cookie = next;
	assert(entry.name_length == strlen("entry-0000") &&
	    memcmp(entry.name, "entry-", strlen("entry-")) == 0);
	assert(btrfs_lookup(fs, directory, entry.name, entry.name_length, &found) == BTRFS_OK);
	assert(found.id.tree == entry.id.tree && found.id.inode == entry.id.inode);
	assert(btrfs_get_inode(fs, entry.id, &child) == BTRFS_OK);
	assert((child.mode & BTRFS_MODE_TYPE) == BTRFS_MODE_REGULAR);
	assert(btrfs_directory_inode(*stream, &entry, &found) == BTRFS_OK);
	assert(found.id.inode == child.id.inode && found.size == child.size &&
	    found.mode == child.mode && found.links == child.links);
}

/* "many/entry-NNNN" holds "N\n" inline; spreads readers over every leaf. */
static void
read_entry(struct btrfs_fs *fs, unsigned number)
{
	struct btrfs_inode inode;
	char path[32];
	char expected[16];
	char data[16];
	size_t completed;
	int length;

	snprintf(path, sizeof(path), "many/entry-%04u", number);
	length = snprintf(expected, sizeof(expected), "%u\n", number);
	assert(btrfs_image_lookup(fs, path, &inode) == BTRFS_OK);
	assert(inode.size == (uint64_t)length);
	assert(btrfs_read(fs, &inode, 0, data, sizeof(data), &completed) == BTRFS_OK);
	assert(completed == (size_t)length && memcmp(data, expected, completed) == 0);
}

static void *
read_worker(void *argument)
{
	struct worker *worker = argument;
	struct btrfs_inode inode;
	struct btrfs_inode directory;
	struct btrfs_directory *stream;
	struct btrfs_directory *held;
	struct btrfs_dir_entry entry;
	uint8_t *buffer;
	uint64_t offset;
	uint64_t cookie;
	uint64_t held_cookie = 2 + worker->index * MANY_ENTRIES / 8;
	size_t completed;
	size_t i;
	unsigned round;
	const size_t length = 65539;

	buffer = malloc(length);
	assert(buffer != NULL);
	assert(btrfs_image_lookup(worker->fs, "many", &directory) == BTRFS_OK);
	assert(btrfs_directory_open(worker->fs, &directory, held_cookie, &held) == BTRFS_OK);
	for (round = 0; round < 32; round++) {
		next_held(worker->fs, &directory, &held, &held_cookie);
		read_entry(worker->fs, (worker->index * 7919U + round * 104729U) % MANY_ENTRIES);
		assert(btrfs_image_lookup(worker->fs, "big", &inode) == BTRFS_OK);
		offset = (worker->index * UINT64_C(65537) + round * UINT64_C(32003)) %
		    (inode.size - length);
		assert(
		    btrfs_read(worker->fs, &inode, offset, buffer, length, &completed) == BTRFS_OK);
		assert(completed == length);
		for (i = 0; i < completed; i++) {
			assert(buffer[i] == (uint8_t)(offset + i));
		}
		assert(btrfs_image_lookup(worker->fs, "many", &directory) == BTRFS_OK);
		assert(
		    btrfs_directory_open(worker->fs, &directory, 2 + round, &stream) == BTRFS_OK);
		assert(btrfs_directory_next(stream, &entry, &cookie) == BTRFS_OK);
		assert(cookie == 3 + round);
		btrfs_directory_close(stream);
		next_held(worker->fs, &directory, &held, &held_cookie);
	}
	btrfs_directory_close(held);
	free(buffer);
	return NULL;
}

static void
cache_lock(void *context)
{
	assert(pthread_mutex_lock(context) == 0);
}

static void
cache_unlock(void *context)
{
	assert(pthread_mutex_unlock(context) == 0);
}

int
main(int argc, char **argv)
{
	struct btrfs_image image;
	struct btrfs_cache_locks locks;
	struct btrfs_cache *cache = NULL;
	struct btrfs_info info;
	struct btrfs_fs *fs;
	struct worker workers[8];
	pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	pthread_t threads[8];
	struct btrfs_cache_counts counts[3] = { { 0 } };
	size_t sizes[3] = { 0 };
	size_t i;
	int pass;

	assert(argc == 2);
	assert(btrfs_image_open(argv[1], &image) == 0);
	locks = (struct btrfs_cache_locks){ &mutex, cache_lock, cache_unlock };
	/* Without a node cache, then with one cache shared by all readers: small
	 * enough that readers evict each other's nodes, then a single set of eight
	 * nodes, which the readers' pinned nodes can fill completely. */
	for (pass = 0; pass < 3; pass++) {
		if (pass != 0) {
			assert(btrfs_cache_create(
				   &image.environment, &locks, sizes[pass], &cache) == BTRFS_OK);
		}
		image.environment.cache = cache;
		assert(btrfs_mount(&image.environment, 5, &fs) == BTRFS_OK);
		if (pass == 0) {
			btrfs_get_info(fs, &info);
			sizes[1] = 256 * 1024;
			sizes[2] = 8 * (size_t)info.node_size;
		}
		for (i = 0; i < 8; i++) {
			workers[i].fs = fs;
			workers[i].index = (unsigned)i;
			assert(pthread_create(&threads[i], NULL, read_worker, &workers[i]) == 0);
		}
		for (i = 0; i < 8; i++) {
			assert(pthread_join(threads[i], NULL) == 0);
		}
		btrfs_unmount(fs);
		if (cache != NULL) {
			btrfs_cache_counts(cache, &counts[pass]);
			assert(counts[pass].hits != 0 && counts[pass].misses != 0 &&
			    counts[pass].pinned == 0);
			btrfs_cache_destroy(cache);
			cache = NULL;
		}
	}
	assert(image.live_allocations == 0 && image.live_bytes == 0);
	btrfs_image_close(&image);
	printf("8 concurrent readers and held directory streams, shared immutable mount, balanced "
	       "allocations, also through a shared node cache (%llu hits, %llu misses) and a "
	       "pinned single set (%llu hits, %llu misses): PASS\n",
	    (unsigned long long)counts[1].hits, (unsigned long long)counts[1].misses,
	    (unsigned long long)counts[2].hits, (unsigned long long)counts[2].misses);
	return 0;
}
