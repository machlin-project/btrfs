/* SPDX-License-Identifier: BSD-3-Clause */
/* Decompressed extents kept by the shared cache: reads through it equal reads
 * without a cache for sequential and random ranges of the compressed fixtures'
 * files, a cache without locks keeps none, refused storage only skips keeping,
 * concurrent readers share it, a read that verifies checksums uses only a
 * verified decompression, and the least recently used one is replaced. */
#include "../adapters/posix/image.h"
#include "internal.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

/* The fixtures' compressed file and the cache the readers share. */
#define FILE_PATH "/big"
#define CACHE_BYTES (4U * 1024U * 1024U)
#define SMALL_READ 4096U
#define MAX_READ (300U * 1024U)
#define RANDOM_READS 3000U
#define READERS 8U
#define READER_READS 1500U
/* Distinct keys beyond the cache's sixteen decompressed extents. */
#define EXTENT_KEYS 17U

struct reader {
	struct btrfs_fs *fs;
	const struct btrfs_inode *inode;
	const uint8_t *expected;
	uint32_t seed;
};

static struct btrfs_environment image_environment;

static uint32_t
next_random(uint32_t *state)
{
	*state = *state * UINT32_C(1103515245) + UINT32_C(12345);
	return *state >> 8;
}

/* A random range, compared with the uncached contents. */
static void
read_random(struct btrfs_fs *fs, const struct btrfs_inode *inode, const uint8_t *expected,
    uint8_t *buffer, uint32_t *state)
{
	uint64_t offset = next_random(state) % inode->size;
	size_t length = 1 + next_random(state) % MAX_READ;
	size_t completed = 0;

	if (length > inode->size - offset) {
		length = (size_t)(inode->size - offset);
	}
	REQUIRE(btrfs_read(fs, inode, offset, buffer, length, &completed) == BTRFS_OK);
	REQUIRE(completed == length && memcmp(buffer, expected + offset, length) == 0);
}

static void *
read_worker(void *argument)
{
	struct reader *reader = argument;
	uint8_t *buffer = malloc(MAX_READ);
	unsigned i;

	REQUIRE(buffer != NULL);
	for (i = 0; i < READER_READS; i++) {
		read_random(reader->fs, reader->inode, reader->expected, buffer, &reader->seed);
	}
	free(buffer);
	return NULL;
}

static void
cache_lock(void *context)
{
	REQUIRE(pthread_mutex_lock(context) == 0);
}

static void
cache_unlock(void *context)
{
	REQUIRE(pthread_mutex_unlock(context) == 0);
}

/* Storage for decompressed extents is refused; nodes are still stored. */
static void *
refuse_extents(void *context, size_t size)
{
	return size == BT_MAX_COMPRESSED_SIZE ? NULL : image_environment.allocate(context, size);
}

static void
mount_with(struct btrfs_image *image, struct btrfs_cache *cache, struct btrfs_fs **fs,
    struct btrfs_inode *inode)
{
	image->environment.cache = cache;
	REQUIRE(btrfs_mount(&image->environment, BTRFS_TOP_LEVEL_TREE, fs) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(*fs, FILE_PATH, inode) == BTRFS_OK);
}

/* Every read pattern through each kind of cache equals the uncached file. */
static void
reads(const char *path, struct btrfs_cache_counts *shared)
{
	struct btrfs_image image;
	struct btrfs_environment refusing;
	struct btrfs_cache_locks locks;
	struct btrfs_cache_counts counts;
	struct btrfs_cache *cache;
	struct btrfs_inode inode;
	struct btrfs_fs *fs;
	struct reader readers[READERS];
	pthread_t threads[READERS];
	pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	uint8_t *expected;
	uint8_t *buffer;
	uint64_t offset;
	uint32_t state = 1;
	size_t completed;
	size_t length;
	unsigned i;

	REQUIRE(btrfs_image_open(path, &image) == 0);
	image_environment = image.environment;
	locks = (struct btrfs_cache_locks){ &mutex, cache_lock, cache_unlock };
	mount_with(&image, NULL, &fs, &inode);
	REQUIRE(inode.size > 2 * BT_MAX_COMPRESSED_SIZE && inode.size < SIZE_MAX);
	expected = malloc((size_t)inode.size);
	buffer = malloc(MAX_READ);
	REQUIRE(expected != NULL && buffer != NULL);
	REQUIRE(btrfs_read(fs, &inode, 0, expected, (size_t)inode.size, &completed) == BTRFS_OK &&
	    completed == inode.size);
	btrfs_unmount(fs);

	/* Sequential small reads decompress each extent once. */
	REQUIRE(btrfs_cache_create(&image.environment, &locks, CACHE_BYTES, &cache) == BTRFS_OK);
	mount_with(&image, cache, &fs, &inode);
	for (offset = 0; offset < inode.size; offset += length) {
		length =
		    inode.size - offset < SMALL_READ ? (size_t)(inode.size - offset) : SMALL_READ;
		REQUIRE(btrfs_read(fs, &inode, offset, buffer, length, &completed) == BTRFS_OK);
		REQUIRE(completed == length && memcmp(buffer, expected + offset, length) == 0);
	}
	btrfs_cache_counts(cache, &counts);
	REQUIRE(counts.extent_misses != 0 && counts.extent_misses <= inode.size / SMALL_READ / 2);
	REQUIRE(counts.extent_hits + counts.extent_misses == inode.size / SMALL_READ);
	for (i = 0; i < RANDOM_READS; i++) {
		read_random(fs, &inode, expected, buffer, &state);
	}
	btrfs_unmount(fs);
	btrfs_cache_destroy(cache);

	/* Without locks nothing is kept or looked up. */
	REQUIRE(btrfs_cache_create(&image.environment, NULL, CACHE_BYTES, &cache) == BTRFS_OK);
	mount_with(&image, cache, &fs, &inode);
	for (i = 0; i < RANDOM_READS; i++) {
		read_random(fs, &inode, expected, buffer, &state);
	}
	btrfs_unmount(fs);
	btrfs_cache_counts(cache, &counts);
	REQUIRE(counts.extent_hits == 0 && counts.extent_misses == 0);
	btrfs_cache_destroy(cache);

	/* Refused storage only skips keeping. */
	refusing = image.environment;
	refusing.allocate = refuse_extents;
	REQUIRE(btrfs_cache_create(&refusing, &locks, CACHE_BYTES, &cache) == BTRFS_OK);
	mount_with(&image, cache, &fs, &inode);
	for (i = 0; i < RANDOM_READS; i++) {
		read_random(fs, &inode, expected, buffer, &state);
	}
	btrfs_unmount(fs);
	btrfs_cache_counts(cache, &counts);
	REQUIRE(counts.extent_hits == 0 && counts.extent_misses != 0);
	btrfs_cache_destroy(cache);

	/* Concurrent readers of one mount share the cache. */
	REQUIRE(btrfs_cache_create(&image.environment, &locks, CACHE_BYTES, &cache) == BTRFS_OK);
	mount_with(&image, cache, &fs, &inode);
	for (i = 0; i < READERS; i++) {
		readers[i] = (struct reader){ fs, &inode, expected, i + 7 };
		REQUIRE(pthread_create(&threads[i], NULL, read_worker, &readers[i]) == 0);
	}
	for (i = 0; i < READERS; i++) {
		REQUIRE(pthread_join(threads[i], NULL) == 0);
	}
	btrfs_unmount(fs);
	btrfs_cache_counts(cache, shared);
	REQUIRE(shared->extent_hits != 0 && shared->extent_misses != 0);
	btrfs_cache_destroy(cache);
	free(expected);
	free(buffer);
	REQUIRE(image.live_allocations == 0 && image.live_bytes == 0);
	btrfs_image_close(&image);
}

static struct bt_extent_key
extent_key(unsigned index)
{
	return (struct bt_extent_key){ (uint64_t)(index + 1) * BT_MAX_COMPRESSED_SIZE, 4096,
		BT_MAX_COMPRESSED_SIZE, 7, BTRFS_COMPRESSION_ZLIB };
}

/* The rules of keeping, on keys alone. */
static void
rules(const char *path)
{
	static uint8_t decoded[BT_MAX_COMPRESSED_SIZE];
	static uint8_t copy[BT_MAX_COMPRESSED_SIZE];
	struct btrfs_image image;
	struct btrfs_cache_locks locks;
	struct btrfs_cache *cache;
	struct bt_extent_key key;
	pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	unsigned i;

	REQUIRE(btrfs_image_open(path, &image) == 0);
	locks = (struct btrfs_cache_locks){ &mutex, cache_lock, cache_unlock };
	REQUIRE(btrfs_cache_create(&image.environment, &locks, CACHE_BYTES, &cache) == BTRFS_OK);
	for (i = 0; i < sizeof(decoded); i++) {
		decoded[i] = (uint8_t)(i * 31);
	}
	/* An unverified decompression serves only reads that skip checksums. */
	key = extent_key(0);
	bt_cache_extent_put(cache, &key, 0, decoded);
	REQUIRE(!bt_cache_extent_get(cache, &key, 1, 0, copy, sizeof(copy)));
	REQUIRE(bt_cache_extent_get(cache, &key, 0, 100, copy, 1000));
	REQUIRE(memcmp(copy, decoded + 100, 1000) == 0);
	bt_cache_extent_put(cache, &key, 1, decoded);
	REQUIRE(bt_cache_extent_get(cache, &key, 1, 0, copy, sizeof(copy)));
	/* Any other key field names another extent. */
	key.generation++;
	REQUIRE(!bt_cache_extent_get(cache, &key, 0, 0, copy, 1));
	key = extent_key(0);
	key.compression = BTRFS_COMPRESSION_ZSTD;
	REQUIRE(!bt_cache_extent_get(cache, &key, 0, 0, copy, 1));
	key = extent_key(0);
	key.disk_bytes += 4096;
	REQUIRE(!bt_cache_extent_get(cache, &key, 0, 0, copy, 1));
	/* Ranges beyond the decompressed size and oversized extents miss. */
	key = extent_key(0);
	REQUIRE(!bt_cache_extent_get(cache, &key, 0, BT_MAX_COMPRESSED_SIZE, copy, 1));
	key.ram_bytes = BT_MAX_COMPRESSED_SIZE + 4096;
	bt_cache_extent_put(cache, &key, 1, decoded);
	REQUIRE(!bt_cache_extent_get(cache, &key, 0, 0, copy, 1));
	/* The least recently used extent is replaced first. */
	key = extent_key(0);
	REQUIRE(bt_cache_extent_get(cache, &key, 0, 0, copy, 1));
	for (i = 1; i < EXTENT_KEYS; i++) {
		key = extent_key(i);
		bt_cache_extent_put(cache, &key, 1, decoded);
	}
	key = extent_key(0);
	REQUIRE(!bt_cache_extent_get(cache, &key, 0, 0, copy, 1));
	for (i = 1; i < EXTENT_KEYS; i++) {
		key = extent_key(i);
		REQUIRE(bt_cache_extent_get(cache, &key, 1, 0, copy, sizeof(copy)));
		REQUIRE(memcmp(copy, decoded, sizeof(copy)) == 0);
	}
	btrfs_cache_destroy(cache);
	REQUIRE(image.live_allocations == 0 && image.live_bytes == 0);
	btrfs_image_close(&image);
}

int
main(int argc, char **argv)
{
	struct btrfs_cache_counts counts[2];
	int i;

	REQUIRE(argc == 3);
	for (i = 1; i < argc; i++) {
		reads(argv[i], &counts[i - 1]);
	}
	rules(argv[1]);
	printf("decompressed extents: sequential, random and concurrent reads equal uncached reads "
	       "(%llu and %llu hits shared), none without locks or storage, verified and LRU "
	       "rules PASS\n",
	    (unsigned long long)counts[0].extent_hits, (unsigned long long)counts[1].extent_hits);
	return 0;
}
