/* SPDX-License-Identifier: BSD-3-Clause */
/* The native volume's versioned views on a Linux fixture: a pinned view keeps
 * its committed root set after later commits, the next writer waits until older
 * views drain, an uncertain commit leaves the volume failed but readable, and
 * aborted or empty transactions publish nothing. Writes go to an overlay.
 *
 * --stress ITERATIONS runs reader threads against one writer: every reader
 * checks its pinned view against the model state the writer recorded for that
 * generation, while the writer commits, aborts, fails allocations inside
 * transactions and finally fails a commit write. Run it under ThreadSanitizer
 * as well as the default sanitizers. */
#define _POSIX_C_SOURCE 200809L
#include "../adapters/posix/image.h"
#include "../core/disk.h"
#include <btrfs/volume.h>
#include <stddef.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define SECTOR 512U
#define OVERLAY_SLOTS 131072U
#define WAIT_MILLISECONDS 100L
#define DATA_FILE_BYTES 8192U
#define STRESS_FILES 16U
#define STRESS_READERS 4U
#define STRESS_MAX_SIZE 70000U
#define STRESS_NAME 16U
#define STRESS_PREFIX "stress-"
#define STRESS_MAX_OPERATIONS 4U
#define STRESS_LONG_PIN_MICROSECONDS 500L
#define STRESS_DRAIN_MILLISECONDS 200L
/* Tree nodes a grouped test operation declares (btrfs_volume_join). */
#define GROUPED_NODES 64U
/* Half the per-transaction node limit: the largest operation a transaction can
 * take (btrfs_transaction_room). */
#define GROUPED_MAX_NODES 2048U
/* Allocation fault points tried to fail an operation after its first change. */
#define GROUPED_FAULT_POINTS 256U
/* The grouped stress test's sequence file, outside the stress names. */
#define GROUPED_STAMP "grouped-stamp"
/* Tree nodes a grouped stress operation declares. */
#define GROUPED_STRESS_NODES 256U
/* A compressible file of two compressed extents, read in three pieces. */
#define COMPRESSED_BYTES (256U * 1024U)
#define COMPRESSED_PIECE 100000U

/* An allocation countdown for the writer thread only: readers never see
 * injected failures. */
static _Thread_local size_t allocation_failure;

/* Written sectors, kept in an open-addressing table over the fixture. */
struct overlay {
	struct btrfs_image *image;
	pthread_rwlock_t lock;
	uint64_t *sectors;
	uint8_t *bytes;
	size_t count;
	size_t fail_write;
	size_t writes;
	size_t flushes;
};

struct locks {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
};

struct writer {
	struct btrfs_volume *volume;
	struct btrfs_transaction *transaction;
	enum btrfs_result result;
	int began;
};

static size_t
overlay_slot(const struct overlay *overlay, uint64_t sector)
{
	size_t slot = (size_t)(sector * UINT64_C(0x9e3779b97f4a7c15) >> 48) % OVERLAY_SLOTS;

	while (overlay->sectors[slot] != UINT64_MAX && overlay->sectors[slot] != sector) {
		slot = (slot + 1) % OVERLAY_SLOTS;
	}
	return slot;
}

static void *
overlay_allocate(void *context, size_t size)
{
	struct overlay *overlay = context;

	if (allocation_failure != 0 && --allocation_failure == 0) {
		return NULL;
	}
	return overlay->image->environment.allocate(overlay->image, size);
}

static void
overlay_release(void *context, void *bytes, size_t size)
{
	struct overlay *overlay = context;

	overlay->image->environment.release(overlay->image, bytes, size);
}

static enum btrfs_result
overlay_decompress(void *context, enum btrfs_compression codec, const void *input,
    size_t input_size, void *output, size_t capacity, size_t *produced)
{
	struct overlay *overlay = context;

	return overlay->image->environment.decompress(
	    overlay->image, codec, input, input_size, output, capacity, produced);
}

static enum btrfs_result
overlay_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct overlay *overlay = context;
	uint8_t *bytes = buffer;
	uint64_t sector;
	size_t slot;
	size_t done;
	size_t within;
	size_t amount;
	enum btrfs_result error;

	error = overlay->image->environment.read(overlay->image, offset, buffer, length);
	pthread_rwlock_rdlock(&overlay->lock);
	for (done = 0; error == BTRFS_OK && done < length; done += amount) {
		sector = (offset + done) / SECTOR;
		within = (size_t)((offset + done) % SECTOR);
		amount = SECTOR - within < length - done ? SECTOR - within : length - done;
		slot = overlay_slot(overlay, sector);
		if (overlay->sectors[slot] == sector) {
			memcpy(bytes + done, overlay->bytes + slot * SECTOR + within, amount);
		}
	}
	pthread_rwlock_unlock(&overlay->lock);
	return error;
}

static enum btrfs_result
overlay_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct overlay *overlay = context;
	const uint8_t *bytes = buffer;
	size_t slot;
	size_t done;

	overlay->writes++;
	if (overlay->writes == overlay->fail_write) {
		return BTRFS_IO;
	}
	REQUIRE(offset % SECTOR == 0 && length % SECTOR == 0);
	pthread_rwlock_wrlock(&overlay->lock);
	for (done = 0; done < length; done += SECTOR) {
		slot = overlay_slot(overlay, (offset + done) / SECTOR);
		if (overlay->sectors[slot] == UINT64_MAX) {
			REQUIRE(++overlay->count < OVERLAY_SLOTS / 2);
			overlay->sectors[slot] = (offset + done) / SECTOR;
		}
		memcpy(overlay->bytes + slot * SECTOR, bytes + done, SECTOR);
	}
	pthread_rwlock_unlock(&overlay->lock);
	return BTRFS_OK;
}

static enum btrfs_result
overlay_flush(void *context)
{
	struct overlay *overlay = context;

	overlay->flushes++;
	return BTRFS_OK;
}

static void
lock(void *context)
{
	pthread_mutex_lock(&((struct locks *)context)->mutex);
}

static void
unlock(void *context)
{
	pthread_mutex_unlock(&((struct locks *)context)->mutex);
}

static void
wait_on(void *context, const void *channel)
{
	struct locks *locks = context;

	(void)channel;
	pthread_cond_wait(&locks->condition, &locks->mutex);
}

static void
wake(void *context, const void *channel)
{
	(void)channel;
	pthread_cond_broadcast(&((struct locks *)context)->condition);
}

static int
exists(const struct btrfs_fs *fs, const char *path)
{
	struct btrfs_inode inode;

	return btrfs_image_lookup((struct btrfs_fs *)fs, path, &inode) == BTRFS_OK;
}

static enum btrfs_result
create_file(struct btrfs_volume *volume, struct btrfs_transaction *transaction, const char *name)
{
	struct btrfs_volume_view *view;
	struct btrfs_new_inode attributes;
	struct btrfs_object_id id;
	struct btrfs_inode root;
	const struct btrfs_fs *fs;
	enum btrfs_result result;

	fs = btrfs_volume_pin(volume, &view);
	REQUIRE(btrfs_root(fs, &root) == BTRFS_OK);
	btrfs_volume_unpin(volume, view);
	memset(&attributes, 0, sizeof(attributes));
	attributes.mode = BTRFS_MODE_REGULAR | 0644;
	attributes.time.seconds = 1800000000;
	result =
	    btrfs_transaction_create(transaction, root.id, name, strlen(name), &attributes, &id);
	if (result == BTRFS_OK) {
		result = btrfs_transaction_write(
		    transaction, id, 0, "native volume\n", 14, attributes.time);
	}
	return result;
}

/* A file larger than an inline extent: its data reaches the device while the
 * transaction runs. */
static enum btrfs_result
create_data_file(
    struct btrfs_volume *volume, struct btrfs_transaction *transaction, const char *name)
{
	struct btrfs_volume_view *view;
	struct btrfs_new_inode attributes;
	struct btrfs_object_id id;
	struct btrfs_inode root;
	const struct btrfs_fs *fs;
	uint8_t data[DATA_FILE_BYTES];
	enum btrfs_result result;

	fs = btrfs_volume_pin(volume, &view);
	REQUIRE(btrfs_root(fs, &root) == BTRFS_OK);
	btrfs_volume_unpin(volume, view);
	memset(&attributes, 0, sizeof(attributes));
	memset(data, 0x5a, sizeof(data));
	attributes.mode = BTRFS_MODE_REGULAR | 0644;
	attributes.time.seconds = 1800000000;
	result =
	    btrfs_transaction_create(transaction, root.id, name, strlen(name), &attributes, &id);
	if (result == BTRFS_OK) {
		result = btrfs_transaction_write(
		    transaction, id, 0, data, sizeof(data), attributes.time);
	}
	return result;
}

static struct btrfs_object_id
root_id(struct btrfs_volume *volume)
{
	struct btrfs_volume_view *view;
	struct btrfs_inode root;
	const struct btrfs_fs *fs;

	fs = btrfs_volume_pin(volume, &view);
	REQUIRE(btrfs_root(fs, &root) == BTRFS_OK);
	btrfs_volume_unpin(volume, view);
	return root.id;
}

static uint64_t
inode_number(struct btrfs_volume *volume, const char *path)
{
	struct btrfs_volume_view *view;
	struct btrfs_inode inode;
	const struct btrfs_fs *fs;

	fs = btrfs_volume_pin(volume, &view);
	REQUIRE(btrfs_image_lookup((struct btrfs_fs *)fs, path, &inode) == BTRFS_OK);
	btrfs_volume_unpin(volume, view);
	return inode.id.inode;
}

static void *
second_writer(void *context)
{
	struct writer *writer = context;

	writer->result = btrfs_volume_begin(writer->volume, &writer->transaction);
	__atomic_store_n(&writer->began, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

static void
pause_briefly(void)
{
	struct timespec delay = { 0, WAIT_MILLISECONDS * 1000000L };

	nanosleep(&delay, NULL);
}

struct harness {
	struct btrfs_image image;
	struct btrfs_environment environment;
	struct btrfs_write_environment device;
	struct btrfs_volume_locks callbacks;
	struct overlay overlay;
	struct locks locks;
	/* One node cache for every view of the volume, as an adapter keeps. */
	struct btrfs_cache *cache;
	pthread_mutex_t cache_mutex;
};

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

static void
harness_open(struct harness *harness, const char *path)
{
	size_t i;

	memset(harness, 0, sizeof(*harness));
	REQUIRE(btrfs_image_open(path, &harness->image) == 0);
	harness->overlay.image = &harness->image;
	harness->overlay.sectors = malloc(OVERLAY_SLOTS * sizeof(*harness->overlay.sectors));
	harness->overlay.bytes = malloc((size_t)OVERLAY_SLOTS * SECTOR);
	REQUIRE(harness->overlay.sectors != NULL && harness->overlay.bytes != NULL);
	for (i = 0; i < OVERLAY_SLOTS; i++) {
		harness->overlay.sectors[i] = UINT64_MAX;
	}
	REQUIRE(pthread_rwlock_init(&harness->overlay.lock, NULL) == 0);
	harness->environment = harness->image.environment;
	harness->environment.context = &harness->overlay;
	harness->environment.read = overlay_read;
	harness->environment.allocate = overlay_allocate;
	harness->environment.release = overlay_release;
	harness->environment.decompress = overlay_decompress;
	harness->device = (struct btrfs_write_environment){ &harness->overlay, overlay_write,
		overlay_flush, NULL, BTRFS_COMPRESSION_NONE, 0 };
	pthread_mutex_init(&harness->locks.mutex, NULL);
	pthread_cond_init(&harness->locks.condition, NULL);
	harness->callbacks =
	    (struct btrfs_volume_locks){ &harness->locks, lock, unlock, wait_on, wake };
	pthread_mutex_init(&harness->cache_mutex, NULL);
	/* Small enough that views evict each other's nodes. */
	REQUIRE(btrfs_cache_create(&harness->environment,
		    &(struct btrfs_cache_locks){ &harness->cache_mutex, cache_lock, cache_unlock },
		    512 * 1024, &harness->cache) == BTRFS_OK);
	harness->environment.cache = harness->cache;
}

static void
harness_close(struct harness *harness)
{
	struct btrfs_cache_counts counts;

	btrfs_cache_counts(harness->cache, &counts);
	REQUIRE(counts.hits != 0 && counts.pinned == 0);
	btrfs_cache_destroy(harness->cache);
	pthread_mutex_destroy(&harness->cache_mutex);
	REQUIRE(harness->image.live_allocations == 0);
	btrfs_image_close(&harness->image);
	pthread_rwlock_destroy(&harness->overlay.lock);
	pthread_mutex_destroy(&harness->locks.mutex);
	pthread_cond_destroy(&harness->locks.condition);
	free(harness->overlay.sectors);
	free(harness->overlay.bytes);
}

/* Changes the overlay's primary superblock as another writer would. */
static void
flip_primary_label(struct harness *harness)
{
	uint64_t sector = BT_SUPER_OFFSET / SECTOR;
	size_t slot = overlay_slot(&harness->overlay, sector);

	REQUIRE(harness->overlay.sectors[slot] == sector);
	harness->overlay.bytes[slot * SECTOR + offsetof(struct bt_disk_super, label)] ^= 1;
}

static void
views_test(struct harness *harness)
{
	struct btrfs_environment *environment = &harness->environment;
	struct btrfs_volume *volume;
	struct btrfs_volume_view *old_view;
	struct btrfs_volume_view *new_view;
	struct btrfs_transaction *transaction;
	struct writer writer;
	const struct btrfs_fs *old_fs;
	const struct btrfs_fs *new_fs;
	struct btrfs_time time = { 1800000001, 0 };
	pthread_t thread;
	uint64_t generation;
	uint64_t removed;
	uint64_t reads;
	uint64_t writes;

	/* A read-only volume serves views and refuses writers. */
	REQUIRE(btrfs_volume_open(environment, NULL, &harness->callbacks, BTRFS_TOP_LEVEL_TREE,
		    &volume) == BTRFS_OK);
	REQUIRE(!btrfs_volume_writable(volume));
	old_fs = btrfs_volume_pin(volume, &old_view);
	REQUIRE(exists(old_fs, "/greeting"));
	btrfs_volume_unpin(volume, old_view);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_READ_ONLY);
	btrfs_volume_close(volume);
	REQUIRE(harness->overlay.writes == 0);

	REQUIRE(btrfs_volume_open(environment, &harness->device, &harness->callbacks,
		    BTRFS_TOP_LEVEL_TREE, &volume) == BTRFS_OK);
	REQUIRE(btrfs_volume_writable(volume) && harness->overlay.writes == 0);
	generation = btrfs_volume_generation(volume);

	/* An empty transaction writes nothing; an aborted one wrote only its new
	 * data, to unreferenced space, and publishes nothing. */
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_OK);
	REQUIRE(harness->overlay.writes == 0 && harness->overlay.flushes == 0);
	/* A missing publication wrapper fails before any commit write and leaves
	 * the last view usable; the uncommitted name never reaches readers. */
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_file(volume, transaction, "no-view-memory") == BTRFS_OK);
	allocation_failure = 1;
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_NO_MEMORY);
	allocation_failure = 0;
	REQUIRE(harness->overlay.writes == 0 && harness->overlay.flushes == 0 &&
	    btrfs_volume_failure(volume) == BTRFS_OK &&
	    btrfs_volume_generation(volume) == generation);
	new_fs = btrfs_volume_pin(volume, &new_view);
	REQUIRE(!exists(new_fs, "/no-view-memory"));
	btrfs_volume_unpin(volume, new_view);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_data_file(volume, transaction, "aborted") == BTRFS_OK);
	REQUIRE(harness->overlay.writes != 0);
	btrfs_volume_abort(volume, transaction);
	REQUIRE(btrfs_volume_generation(volume) == generation && harness->overlay.flushes == 0 &&
	    btrfs_volume_failure(volume) == BTRFS_OK);

	/* A pinned view keeps the old root set; the new view has the file. */
	old_fs = btrfs_volume_pin(volume, &old_view);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_file(volume, transaction, "created") == BTRFS_OK);
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_OK);
	REQUIRE(harness->overlay.flushes == 3);
	REQUIRE(btrfs_volume_generation(volume) == generation + 1);
	/* The published commit's nodes entered the node cache: reading what it
	 * wrote takes no device read. */
	reads = atomic_load(&harness->image.reads);
	new_fs = btrfs_volume_pin(volume, &new_view);
	REQUIRE(exists(new_fs, "/created") && !exists(new_fs, "/aborted"));
	REQUIRE(atomic_load(&harness->image.reads) == reads);
	REQUIRE(!exists(old_fs, "/created") && exists(old_fs, "/greeting"));
	btrfs_volume_unpin(volume, new_view);

	/* The next writer waits while the older view is pinned. */
	memset(&writer, 0, sizeof(writer));
	writer.volume = volume;
	REQUIRE(pthread_create(&thread, NULL, second_writer, &writer) == 0);
	pause_briefly();
	REQUIRE(!__atomic_load_n(&writer.began, __ATOMIC_SEQ_CST));
	btrfs_volume_unpin(volume, old_view);
	REQUIRE(pthread_join(thread, NULL) == 0);
	REQUIRE(writer.began && writer.result == BTRFS_OK);
	REQUIRE(create_file(volume, writer.transaction, "second") == BTRFS_OK);
	REQUIRE(btrfs_volume_commit(volume, writer.transaction) == BTRFS_OK);
	REQUIRE(btrfs_volume_generation(volume) == generation + 2);

	/* The mount's counters do not hand out a removed inode number again. */
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_file(volume, transaction, "numbered") == BTRFS_OK);
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_OK);
	removed = inode_number(volume, "/numbered");
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_unlink(transaction, root_id(volume), "numbered", 8, time, 0) ==
	    BTRFS_OK);
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_OK);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_file(volume, transaction, "renumbered") == BTRFS_OK);
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_OK);
	REQUIRE(inode_number(volume, "/renumbered") == removed + 1);
	generation = btrfs_volume_generation(volume);

	/* A commit refused before it writes (the primary superblock changed
	 * underneath it) leaves the volume usable, and its sealed nodes stay out
	 * of the node cache: the next commit reuses the generation and the same
	 * addresses with other contents. */
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_file(volume, transaction, "refused") == BTRFS_OK);
	writes = harness->overlay.writes;
	flip_primary_label(harness);
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_STALE);
	flip_primary_label(harness);
	REQUIRE(harness->overlay.writes == writes && btrfs_volume_failure(volume) == BTRFS_OK &&
	    btrfs_volume_generation(volume) == generation);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_file(volume, transaction, "accepted") == BTRFS_OK);
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_OK);
	generation = btrfs_volume_generation(volume);
	new_fs = btrfs_volume_pin(volume, &new_view);
	REQUIRE(exists(new_fs, "/accepted") && !exists(new_fs, "/refused"));
	btrfs_volume_unpin(volume, new_view);

	/* A failed commit that issued writes leaves the volume failed. */
	old_fs = btrfs_volume_pin(volume, &old_view);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_file(volume, transaction, "uncertain") == BTRFS_OK);
	harness->overlay.fail_write = harness->overlay.writes + 2;
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_IO);
	REQUIRE(btrfs_volume_failure(volume) == BTRFS_IO);
	REQUIRE(btrfs_volume_generation(volume) == generation);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_IO);
	REQUIRE(exists(old_fs, "/second") && !exists(old_fs, "/uncertain"));
	btrfs_volume_unpin(volume, old_view);
	btrfs_volume_close(volume);
	printf("native volume views: pins, draining, abort, empty and failed commits, counters "
	       "PASS\n");
}

/* Creates name as one grouped operation. */
static enum btrfs_result
grouped_create(struct btrfs_volume *volume, const char *name)
{
	struct btrfs_transaction *transaction;
	enum btrfs_result result;

	result = btrfs_volume_join(volume, GROUPED_NODES, &transaction);
	if (result == BTRFS_OK) {
		result = create_file(volume, transaction, name);
		btrfs_volume_leave(volume, transaction);
	}
	return result;
}

static int
newest_has(struct btrfs_volume *volume, const char *path, int *running)
{
	struct btrfs_volume_view *view;
	const struct btrfs_fs *fs;
	int present;

	fs = btrfs_volume_read(volume, &view);
	present = exists(fs, path);
	*running = view == NULL;
	btrfs_volume_unread(volume, view);
	return present;
}

static int
committed_has(struct btrfs_volume *volume, const char *path)
{
	struct btrfs_volume_view *view;
	const struct btrfs_fs *fs;
	int present;

	fs = btrfs_volume_pin(volume, &view);
	present = exists(fs, path);
	btrfs_volume_unpin(volume, view);
	return present;
}

static void
compressed_bytes(uint8_t *bytes, unsigned version)
{
	size_t i;

	for (i = 0; i < COMPRESSED_BYTES; i++) {
		bytes[i] = (uint8_t)(i / 64 + version * 37U);
	}
}

static enum btrfs_result
create_compressed(struct btrfs_transaction *transaction, struct btrfs_object_id root,
    const char *name, unsigned version)
{
	static uint8_t bytes[COMPRESSED_BYTES];
	struct btrfs_new_inode attributes;
	struct btrfs_object_id id;
	enum btrfs_result result;

	memset(&attributes, 0, sizeof(attributes));
	attributes.mode = BTRFS_MODE_REGULAR | 0644;
	attributes.time.seconds = 1800000000;
	compressed_bytes(bytes, version);
	result = btrfs_transaction_create(transaction, root, name, strlen(name), &attributes, &id);
	if (result == BTRFS_OK) {
		result = btrfs_transaction_write(
		    transaction, id, 0, bytes, sizeof(bytes), attributes.time);
	}
	return result;
}

/* Reads the file in pieces that end inside extents and compares each byte. */
static void
require_compressed(const struct btrfs_fs *fs, const char *path, unsigned version)
{
	static uint8_t expected[COMPRESSED_BYTES];
	static uint8_t bytes[COMPRESSED_BYTES];
	struct btrfs_inode inode;
	size_t completed;
	size_t offset;
	size_t length;

	compressed_bytes(expected, version);
	REQUIRE(btrfs_image_lookup((struct btrfs_fs *)fs, path, &inode) == BTRFS_OK);
	REQUIRE(inode.size == COMPRESSED_BYTES);
	for (offset = 0; offset < COMPRESSED_BYTES; offset += length) {
		length = COMPRESSED_BYTES - offset < COMPRESSED_PIECE ? COMPRESSED_BYTES - offset
								      : COMPRESSED_PIECE;
		REQUIRE(btrfs_read(fs, &inode, offset, bytes + offset, length, &completed) ==
			BTRFS_OK &&
		    completed == length);
	}
	REQUIRE(memcmp(bytes, expected, sizeof(bytes)) == 0);
}

static void
require_committed_compressed(struct btrfs_volume *volume, const char *path, unsigned version)
{
	struct btrfs_volume_view *view;
	const struct btrfs_fs *fs;

	fs = btrfs_volume_pin(volume, &view);
	require_compressed(fs, path, version);
	btrfs_volume_unpin(volume, view);
}

/* A volume writes through the caller's compressor: a compressible file takes
 * a fraction of its size on the device and reads back as written. */
static void
compression_test(struct harness *harness)
{
	struct btrfs_volume *volume;
	struct btrfs_transaction *transaction;
	size_t sectors;

	harness->device.compress = btrfs_image_compress;
	harness->device.compression = BTRFS_COMPRESSION_ZLIB;
	REQUIRE(btrfs_volume_open(&harness->environment, &harness->device, &harness->callbacks,
		    BTRFS_TOP_LEVEL_TREE, &volume) == BTRFS_OK);
	sectors = harness->overlay.count;
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_compressed(transaction, root_id(volume), "compressed", 1) == BTRFS_OK);
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_OK);
	REQUIRE(harness->overlay.count - sectors < COMPRESSED_BYTES / SECTOR);
	require_committed_compressed(volume, "/compressed", 1);
	btrfs_volume_close(volume);
	harness->device.compress = NULL;
	harness->device.compression = BTRFS_COMPRESSION_NONE;
	printf("compressed volume writes: stored in a fraction of their size, read back PASS\n");
}

/* Decompressed extents the cache keeps follow the device: after an extent is
 * freed and its space written by a newer generation, and after a refused
 * transaction, whose generation and space the next one uses again; the
 * running transaction's own extents never enter the cache. */
static void
compressed_test(struct harness *harness)
{
	struct btrfs_volume *volume;
	struct btrfs_transaction *transaction;
	struct btrfs_cache_counts before;
	struct btrfs_cache_counts after;
	const struct btrfs_fs *reader;
	struct btrfs_object_id root;
	struct btrfs_time time = { 1800000000, 0 };

	harness->device.compress = btrfs_image_compress;
	harness->device.compression = BTRFS_COMPRESSION_ZLIB;
	REQUIRE(btrfs_volume_open(&harness->environment, &harness->device, &harness->callbacks,
		    BTRFS_TOP_LEVEL_TREE, &volume) == BTRFS_OK);
	root = root_id(volume);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_compressed(transaction, root, "compressed-a", 1) == BTRFS_OK);
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_OK);
	btrfs_cache_counts(harness->cache, &before);
	require_committed_compressed(volume, "/compressed-a", 1);
	btrfs_cache_counts(harness->cache, &after);
	REQUIRE(after.extent_misses > before.extent_misses);
	require_committed_compressed(volume, "/compressed-a", 1);
	btrfs_cache_counts(harness->cache, &before);
	REQUIRE(before.extent_hits > after.extent_hits);

	/* The freed extents' space holds the next file's data. */
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_unlink(transaction, root, "compressed-a", strlen("compressed-a"),
		    time, 0) == BTRFS_OK);
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_OK);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_compressed(transaction, root, "compressed-b", 2) == BTRFS_OK);
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_OK);
	require_committed_compressed(volume, "/compressed-b", 2);

	/* A refused transaction's extents, read through its own view, stay out of
	 * the cache; the next transaction reuses their generation and space. */
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_compressed(transaction, root, "compressed-c", 3) == BTRFS_OK);
	reader = btrfs_transaction_reader(transaction);
	REQUIRE(reader != NULL);
	btrfs_cache_counts(harness->cache, &before);
	require_compressed(reader, "/compressed-c", 3);
	btrfs_cache_counts(harness->cache, &after);
	REQUIRE(
	    after.extent_hits == before.extent_hits && after.extent_misses == before.extent_misses);
	btrfs_volume_abort(volume, transaction);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_compressed(transaction, root, "compressed-c", 4) == BTRFS_OK);
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_OK);
	require_committed_compressed(volume, "/compressed-c", 4);
	require_committed_compressed(volume, "/compressed-b", 2);
	btrfs_volume_close(volume);
	harness->device.compress = NULL;
	harness->device.compression = BTRFS_COMPRESSION_NONE;
	printf("decompressed extents: kept for committed generations, read as written after "
	       "their space is reused and after a refused transaction PASS\n");
}

/* Grouped operations: one running transaction, visible before it commits,
 * committed by sync, by a begin or for room, and failure isolation. */
static void
grouped_test(struct harness *harness)
{
	struct btrfs_volume *volume;
	struct btrfs_transaction *transaction;
	char name[32];
	uint64_t generation;
	uint64_t pending;
	size_t flushes;
	size_t point;
	int running;
	enum btrfs_result result = BTRFS_OK;

	REQUIRE(btrfs_volume_open(&harness->environment, &harness->device, &harness->callbacks,
		    BTRFS_TOP_LEVEL_TREE, &volume) == BTRFS_OK);
	generation = btrfs_volume_generation(volume);
	flushes = harness->overlay.flushes;

	/* Operations share the running transaction; readers see each at once. */
	REQUIRE(btrfs_volume_join(volume, GROUPED_NODES, &transaction) == BTRFS_OK);
	pending = btrfs_volume_pending(volume);
	REQUIRE(pending == generation + 1);
	REQUIRE(create_file(volume, transaction, "grouped-a") == BTRFS_OK);
	btrfs_volume_leave(volume, transaction);
	REQUIRE(newest_has(volume, "/grouped-a", &running) && running);
	REQUIRE(grouped_create(volume, "grouped-b") == BTRFS_OK);
	REQUIRE(grouped_create(volume, "grouped-a") == BTRFS_EXISTS);
	REQUIRE(btrfs_volume_failure(volume) == BTRFS_OK);
	REQUIRE(newest_has(volume, "/grouped-a", &running) &&
	    newest_has(volume, "/grouped-b", &running));
	REQUIRE(!committed_has(volume, "/grouped-a"));
	REQUIRE(
	    btrfs_volume_generation(volume) == generation && harness->overlay.flushes == flushes);

	/* One sync commits them together; a second has nothing to do. */
	REQUIRE(btrfs_volume_sync(volume, pending) == BTRFS_OK);
	REQUIRE(btrfs_volume_generation(volume) == generation + 1);
	REQUIRE(harness->overlay.flushes == flushes + 3);
	REQUIRE(newest_has(volume, "/grouped-b", &running) && !running);
	REQUIRE(committed_has(volume, "/grouped-a") && committed_has(volume, "/grouped-b"));
	REQUIRE(btrfs_volume_sync(volume, pending) == BTRFS_OK);
	REQUIRE(btrfs_volume_sync(volume, btrfs_volume_pending(volume)) == BTRFS_OK);
	REQUIRE(harness->overlay.flushes == flushes + 3);

	/* A begin commits the running transaction before its own. */
	REQUIRE(grouped_create(volume, "grouped-c") == BTRFS_OK);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_volume_generation(volume) == generation + 2);
	REQUIRE(create_file(volume, transaction, "grouped-d") == BTRFS_OK);
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_OK);
	REQUIRE(committed_has(volume, "/grouped-c") && committed_has(volume, "/grouped-d"));
	generation = btrfs_volume_generation(volume);

	/* An operation larger than any transaction is refused; one that does not
	 * fit beside the running transaction's changes commits them first. */
	REQUIRE(btrfs_volume_join(volume, GROUPED_MAX_NODES + 1, &transaction) == BTRFS_NO_SPACE);
	REQUIRE(btrfs_volume_failure(volume) == BTRFS_OK);
	REQUIRE(grouped_create(volume, "grouped-e") == BTRFS_OK);
	REQUIRE(btrfs_volume_join(volume, GROUPED_MAX_NODES, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_volume_generation(volume) == generation + 1);
	REQUIRE(committed_has(volume, "/grouped-e"));
	REQUIRE(create_file(volume, transaction, "grouped-f") == BTRFS_OK);
	btrfs_volume_leave(volume, transaction);
	pending = btrfs_volume_pending(volume);
	REQUIRE(btrfs_volume_sync(volume, pending) == BTRFS_OK);
	REQUIRE(committed_has(volume, "/grouped-f"));

	/* An operation that fails after its first change, alone in the running
	 * transaction, is discarded like an aborted transaction. */
	for (point = 1; point <= GROUPED_FAULT_POINTS; point++) {
		snprintf(name, sizeof(name), "discarded-%zu", point);
		REQUIRE(btrfs_volume_join(volume, GROUPED_NODES, &transaction) == BTRFS_OK);
		allocation_failure = point;
		result = create_file(volume, transaction, name);
		allocation_failure = 0;
		if (result == BTRFS_OK || btrfs_transaction_failure(transaction) == BTRFS_OK) {
			btrfs_volume_leave(volume, transaction);
			REQUIRE(
			    btrfs_volume_sync(volume, btrfs_volume_pending(volume)) == BTRFS_OK);
			continue;
		}
		btrfs_volume_leave(volume, transaction);
		break;
	}
	REQUIRE(point <= GROUPED_FAULT_POINTS && result == BTRFS_NO_MEMORY);
	REQUIRE(btrfs_volume_failure(volume) == BTRFS_OK);
	REQUIRE(!newest_has(volume, name, &running));

	/* With earlier operations in it, the volume fails: their changes are lost
	 * as at a crash, and no later operation or sync succeeds. */
	REQUIRE(grouped_create(volume, "acknowledged") == BTRFS_OK);
	pending = btrfs_volume_pending(volume);
	for (point = 1; point <= GROUPED_FAULT_POINTS; point++) {
		snprintf(name, sizeof(name), "poisoned-%zu", point);
		REQUIRE(btrfs_volume_join(volume, GROUPED_NODES, &transaction) == BTRFS_OK);
		allocation_failure = point;
		result = create_file(volume, transaction, name);
		allocation_failure = 0;
		btrfs_volume_leave(volume, transaction);
		if (btrfs_volume_failure(volume) != BTRFS_OK) {
			break;
		}
		REQUIRE(btrfs_volume_pending(volume) == pending);
	}
	REQUIRE(point <= GROUPED_FAULT_POINTS && result == BTRFS_NO_MEMORY);
	REQUIRE(btrfs_volume_failure(volume) == BTRFS_NO_MEMORY);
	REQUIRE(btrfs_volume_join(volume, GROUPED_NODES, &transaction) == BTRFS_NO_MEMORY);
	REQUIRE(btrfs_volume_sync(volume, pending) == BTRFS_NO_MEMORY);
	REQUIRE(!newest_has(volume, "/acknowledged", &running) && !running);
	btrfs_volume_close(volume);
	printf("grouped operations: visible before commit, one commit per sync, commits for a "
	       "begin and for room, refusals keep the transaction, failures discard or fail the "
	       "volume PASS\n");
}

/* Every regular file of version v holds stress_size(v) copies of
 * stress_byte(v); version 0 is an absent name. */
static uint8_t
stress_byte(uint32_t version)
{
	return (uint8_t)(version % 251U + 1U);
}

static size_t
stress_size(uint32_t version)
{
	return 1U + (size_t)(version * UINT32_C(7919)) % STRESS_MAX_SIZE;
}

static size_t
stress_name(char *name, unsigned file)
{
	return (size_t)snprintf(name, STRESS_NAME, STRESS_PREFIX "%02u", file);
}

static uint32_t
stress_random(uint32_t *state)
{
	*state ^= *state << 13;
	*state ^= *state >> 17;
	*state ^= *state << 5;
	return *state;
}

/* Checks one view against the model; returns a description of the first
 * difference, or NULL. */
static const char *
stress_check(const struct btrfs_fs *fs, const uint32_t *versions, uint8_t *buffer)
{
	struct btrfs_inode root;
	struct btrfs_inode inode;
	struct btrfs_dir_entry entry;
	enum btrfs_result result;
	char name[STRESS_NAME];
	uint64_t cookie = 0;
	size_t present = 0;
	size_t listed = 0;
	size_t completed;
	size_t length;
	size_t i;
	unsigned file;

	if (btrfs_root(fs, &root) != BTRFS_OK) {
		return "root inode";
	}
	for (file = 0; file < STRESS_FILES; file++) {
		length = stress_name(name, file);
		result = btrfs_lookup(fs, &root, name, length, &inode);
		if (versions[file] == 0) {
			if (result != BTRFS_NOT_FOUND) {
				return "a removed name is still visible";
			}
			continue;
		}
		present++;
		if (result != BTRFS_OK) {
			return "a committed name is missing";
		}
		if (inode.size != stress_size(versions[file])) {
			return "file size differs from its committed version";
		}
		if (btrfs_read(fs, &inode, 0, buffer, (size_t)inode.size, &completed) != BTRFS_OK ||
		    completed != inode.size) {
			return "file read failed";
		}
		for (i = 0; i < completed; i++) {
			if (buffer[i] != stress_byte(versions[file])) {
				return "file data differs from its committed version";
			}
		}
	}
	while ((result = btrfs_next_dir(fs, &root, &cookie, &entry)) == BTRFS_OK) {
		if (entry.name_length > strlen(STRESS_PREFIX) &&
		    memcmp(entry.name, STRESS_PREFIX, strlen(STRESS_PREFIX)) == 0) {
			listed++;
		}
	}
	if (result != BTRFS_NOT_FOUND) {
		return "directory enumeration failed";
	}
	return listed == present ? NULL : "directory listing differs from the committed names";
}

/* The writer records the model for generation base + k in published[k] before
 * the commit that may publish it; a reader reads the entry of its pinned
 * view's generation, ordered by the volume lock. */
struct stress {
	struct btrfs_volume *volume;
	uint64_t base;
	uint32_t (*published)[STRESS_FILES];
	size_t capacity;
	_Atomic int stop;
	_Atomic uint64_t views;
	_Atomic uint64_t long_pins;
};

struct reader {
	struct stress *stress;
	unsigned index;
	pthread_t thread;
};

static void *
stress_reader(void *context)
{
	struct reader *reader = context;
	struct stress *stress = reader->stress;
	struct btrfs_volume_view *view;
	struct btrfs_info info;
	struct timespec delay = { 0, STRESS_LONG_PIN_MICROSECONDS * 1000L };
	const struct btrfs_fs *fs;
	const char *difference;
	uint32_t versions[STRESS_FILES];
	uint8_t *buffer;
	uint64_t offset;

	buffer = malloc(STRESS_MAX_SIZE);
	REQUIRE(buffer != NULL);
	while (!stress->stop) {
		fs = btrfs_volume_pin(stress->volume, &view);
		btrfs_get_info(fs, &info);
		offset = info.generation - stress->base;
		REQUIRE(info.generation >= stress->base && offset < stress->capacity);
		memcpy(versions, stress->published[offset], sizeof(versions));
		difference = stress_check(fs, versions, buffer);
		/* One reader keeps its view pinned across later commits and checks
		 * it again: no later transaction may reuse its blocks. */
		if (difference == NULL && reader->index == 0) {
			nanosleep(&delay, NULL);
			difference = stress_check(fs, versions, buffer);
			stress->long_pins++;
		}
		if (difference != NULL) {
			fprintf(stderr, "reader %u, generation %llu: %s\n", reader->index,
			    (unsigned long long)info.generation, difference);
			exit(1);
		}
		btrfs_volume_unpin(stress->volume, view);
		stress->views++;
	}
	free(buffer);
	return NULL;
}

struct stress_model {
	uint32_t versions[STRESS_FILES];
	struct btrfs_object_id ids[STRESS_FILES];
};

/* Writes version into file, creating the name when it is absent. */
static enum btrfs_result
stress_store(struct btrfs_transaction *transaction, struct btrfs_object_id root,
    struct stress_model *model, unsigned file, uint32_t version, const uint8_t *data,
    struct btrfs_time time)
{
	struct btrfs_new_inode attributes;
	enum btrfs_result result;
	char name[STRESS_NAME];
	size_t length;
	size_t size = stress_size(version);
	int done = 0;

	length = stress_name(name, file);
	if (model->versions[file] == 0) {
		memset(&attributes, 0, sizeof(attributes));
		attributes.mode = BTRFS_MODE_REGULAR | 0644;
		attributes.time = time;
		result = btrfs_transaction_create(
		    transaction, root, name, length, &attributes, &model->ids[file]);
	} else {
		do {
			result = btrfs_transaction_truncate(transaction, model->ids[file], 0, time,
			    BTRFS_RELEASE_STEP_NODES, &done);
		} while (result == BTRFS_OK && !done);
	}
	if (result == BTRFS_OK) {
		result =
		    btrfs_transaction_write(transaction, model->ids[file], 0, data, size, time);
	}
	if (result == BTRFS_OK) {
		model->versions[file] = version;
	}
	return result;
}

/* One transaction of up to STRESS_MAX_OPERATIONS stores, unlinks and
 * replacing renames applied to the model copy. */
static enum btrfs_result
stress_transaction(struct btrfs_transaction *transaction, struct btrfs_object_id root,
    struct stress_model *model, uint32_t *random, uint32_t *version, uint8_t *data,
    struct btrfs_time time)
{
	enum btrfs_result result = BTRFS_OK;
	char name[STRESS_NAME];
	char target[STRESS_NAME];
	size_t length;
	size_t target_length;
	unsigned operations;
	unsigned operation;
	unsigned file;
	unsigned other;

	operations = 1U + stress_random(random) % STRESS_MAX_OPERATIONS;
	for (operation = 0; operation < operations && result == BTRFS_OK; operation++) {
		file = stress_random(random) % STRESS_FILES;
		length = stress_name(name, file);
		switch (model->versions[file] == 0 ? 0U : stress_random(random) % 4U) {
		case 0:
		case 1:
			++*version;
			memset(data, stress_byte(*version), stress_size(*version));
			result = stress_store(transaction, root, model, file, *version, data, time);
			break;
		case 2:
			result = btrfs_transaction_unlink(transaction, root, name, length, time, 0);
			if (result == BTRFS_OK) {
				model->versions[file] = 0;
			}
			break;
		default:
			other = (file + 1U + stress_random(random) % (STRESS_FILES - 1U)) %
			    STRESS_FILES;
			target_length = stress_name(target, other);
			result = btrfs_transaction_rename(
			    transaction, root, name, length, root, target, target_length, time, 0);
			if (result == BTRFS_OK) {
				model->versions[other] = model->versions[file];
				model->ids[other] = model->ids[file];
				model->versions[file] = 0;
			}
			break;
		}
	}
	return result;
}

static void
stress_test(struct harness *harness, size_t iterations)
{
	struct stress stress;
	struct reader readers[STRESS_READERS];
	struct stress_model model;
	struct stress_model next;
	struct btrfs_volume_view *view;
	struct btrfs_transaction *transaction;
	struct btrfs_inode root;
	struct btrfs_time time = { 1800000000, 0 };
	struct timespec drain = { 0, STRESS_DRAIN_MILLISECONDS * 1000000L };
	const struct btrfs_fs *fs;
	const char *difference;
	enum btrfs_result result;
	uint8_t *data;
	uint8_t *buffer;
	uint32_t random = 0x2545f491U;
	uint32_t version = 0;
	uint64_t generation;
	uint64_t scans = 0;
	uint64_t reuses = 0;
	size_t commits = 0;
	size_t aborts = 0;
	size_t injected = 0;
	size_t iteration;
	unsigned i;
	int armed;

	memset(&stress, 0, sizeof(stress));
	memset(&model, 0, sizeof(model));
	stress.capacity = iterations + 2U;
	stress.published = calloc(stress.capacity, sizeof(*stress.published));
	data = malloc(STRESS_MAX_SIZE);
	buffer = malloc(STRESS_MAX_SIZE);
	REQUIRE(stress.published != NULL && data != NULL && buffer != NULL);
	REQUIRE(btrfs_volume_open(&harness->environment, &harness->device, &harness->callbacks,
		    BTRFS_TOP_LEVEL_TREE, &stress.volume) == BTRFS_OK);
	fs = btrfs_volume_pin(stress.volume, &view);
	REQUIRE(btrfs_root(fs, &root) == BTRFS_OK);
	REQUIRE(stress_check(fs, model.versions, buffer) == NULL);
	btrfs_volume_unpin(stress.volume, view);
	stress.base = btrfs_volume_generation(stress.volume);
	for (i = 0; i < STRESS_READERS; i++) {
		readers[i].stress = &stress;
		readers[i].index = i;
		REQUIRE(pthread_create(&readers[i].thread, NULL, stress_reader, &readers[i]) == 0);
	}
	for (iteration = 0; iteration < iterations; iteration++) {
		time.seconds++;
		REQUIRE(btrfs_volume_begin(stress.volume, &transaction) == BTRFS_OK);
		next = model;
		/* Some transactions fail an allocation part-way and abort. */
		armed = stress_random(&random) % 8U == 0;
		if (armed) {
			allocation_failure = 1U + stress_random(&random) % 64U;
		}
		result =
		    stress_transaction(transaction, root.id, &next, &random, &version, data, time);
		if (armed && allocation_failure == 0) {
			REQUIRE(result == BTRFS_NO_MEMORY);
			injected++;
			btrfs_volume_abort(stress.volume, transaction);
			continue;
		}
		allocation_failure = 0;
		REQUIRE(result == BTRFS_OK);
		if (stress_random(&random) % 8U == 0) {
			aborts++;
			btrfs_volume_abort(stress.volume, transaction);
			continue;
		}
		generation = btrfs_volume_generation(stress.volume);
		memcpy(stress.published[generation + 1U - stress.base], next.versions,
		    sizeof(next.versions));
		REQUIRE(btrfs_volume_commit(stress.volume, transaction) == BTRFS_OK);
		REQUIRE(btrfs_volume_generation(stress.volume) == generation + 1U);
		model = next;
		commits++;
		fs = btrfs_volume_pin(stress.volume, &view);
		difference = stress_check(fs, model.versions, buffer);
		btrfs_volume_unpin(stress.volume, view);
		if (difference != NULL) {
			fprintf(stderr, "writer, generation %llu: %s\n",
			    (unsigned long long)(generation + 1U), difference);
			exit(1);
		}
	}

	/* A commit whose device write fails leaves the published view readable
	 * and refuses later writers. */
	generation = btrfs_volume_generation(stress.volume);
	REQUIRE(btrfs_volume_begin(stress.volume, &transaction) == BTRFS_OK);
	next = model;
	REQUIRE(stress_transaction(transaction, root.id, &next, &random, &version, data, time) ==
	    BTRFS_OK);
	memcpy(
	    stress.published[generation + 1U - stress.base], next.versions, sizeof(next.versions));
	harness->overlay.fail_write = harness->overlay.writes + 1U;
	REQUIRE(btrfs_volume_commit(stress.volume, transaction) == BTRFS_IO);
	REQUIRE(btrfs_volume_failure(stress.volume) == BTRFS_IO);
	REQUIRE(btrfs_volume_generation(stress.volume) == generation);
	REQUIRE(btrfs_volume_begin(stress.volume, &transaction) == BTRFS_IO);
	/* Every transaction after the admission reused the allocator state the
	 * previous commit left, aborts and failed commits included. */
	btrfs_volume_allocation_counts(stress.volume, &scans, &reuses);
	REQUIRE(scans == 1 && reuses == commits + aborts + injected + 1);
	nanosleep(&drain, NULL);
	stress.stop = 1;
	for (i = 0; i < STRESS_READERS; i++) {
		REQUIRE(pthread_join(readers[i].thread, NULL) == 0);
	}
	btrfs_volume_close(stress.volume);

	/* The medium holds the last published generation. */
	REQUIRE(btrfs_volume_open(&harness->environment, NULL, &harness->callbacks,
		    BTRFS_TOP_LEVEL_TREE, &stress.volume) == BTRFS_OK);
	REQUIRE(btrfs_volume_generation(stress.volume) == generation);
	fs = btrfs_volume_pin(stress.volume, &view);
	REQUIRE(stress_check(fs, model.versions, buffer) == NULL);
	btrfs_volume_unpin(stress.volume, view);
	btrfs_volume_close(stress.volume);
	REQUIRE(commits != 0 && aborts != 0 && injected != 0 && stress.long_pins != 0);
	printf("native volume stress: %zu commits, %zu aborts, %zu allocation failures, "
	       "%llu checked views (%llu long pins), allocator state loaded %llu times and "
	       "reused %llu, failed final commit PASS\n",
	    commits, aborts, injected, (unsigned long long)stress.views,
	    (unsigned long long)stress.long_pins, (unsigned long long)scans,
	    (unsigned long long)reuses);
	free(stress.published);
	free(data);
	free(buffer);
}

/* Grouped stress: each grouped operation applies stress_transaction and
 * writes its sequence number into GROUPED_STAMP; the writer records the model
 * of each sequence number before leaving. Readers read the newest state and
 * check it against the model of the sequence number they see, so a view that
 * mixes operations fails, and a read never shows less than the operations
 * acknowledged before it started, during a commit included. Syncs come from
 * the writer and from a reader. */
struct grouped {
	struct btrfs_volume *volume;
	uint32_t (*models)[STRESS_FILES];
	size_t capacity;
	_Atomic int stop;
	/* The last operation that left. */
	_Atomic uint64_t acknowledged;
	_Atomic uint64_t running_reads;
	_Atomic uint64_t committed_reads;
	_Atomic uint64_t reader_syncs;
};

struct grouped_reader {
	struct grouped *grouped;
	unsigned index;
	pthread_t thread;
};

static uint64_t
grouped_sequence(const struct btrfs_fs *fs)
{
	struct btrfs_inode inode;

	struct btrfs_le_sequence {
		uint8_t bytes[8];
	} stamp;

	uint64_t sequence = 0;
	size_t completed;
	unsigned i;

	if (btrfs_image_lookup((struct btrfs_fs *)fs, "/" GROUPED_STAMP, &inode) != BTRFS_OK) {
		return 0;
	}
	REQUIRE(btrfs_read(fs, &inode, 0, &stamp, sizeof(stamp), &completed) == BTRFS_OK &&
	    completed == sizeof(stamp));
	for (i = 0; i < sizeof(stamp.bytes); i++) {
		sequence |= (uint64_t)stamp.bytes[i] << (8U * i);
	}
	return sequence;
}

static void *
grouped_reader(void *context)
{
	struct grouped_reader *reader = context;
	struct grouped *grouped = reader->grouped;
	struct btrfs_volume_view *view;
	const struct btrfs_fs *fs;
	const char *difference;
	uint32_t versions[STRESS_FILES];
	uint8_t *buffer;
	uint64_t sequence;
	uint64_t acknowledged;

	buffer = malloc(STRESS_MAX_SIZE);
	REQUIRE(buffer != NULL);
	while (!grouped->stop) {
		acknowledged = grouped->acknowledged;
		fs = btrfs_volume_read(grouped->volume, &view);
		sequence = grouped_sequence(fs);
		REQUIRE(sequence < grouped->capacity);
		if (sequence < acknowledged) {
			fprintf(stderr,
			    "grouped reader %u: read operation %llu after %llu was acknowledged\n",
			    reader->index, (unsigned long long)sequence,
			    (unsigned long long)acknowledged);
			exit(1);
		}
		memcpy(versions, grouped->models[sequence], sizeof(versions));
		difference = stress_check(fs, versions, buffer);
		if (view == NULL) {
			grouped->running_reads++;
		} else {
			grouped->committed_reads++;
		}
		btrfs_volume_unread(grouped->volume, view);
		if (difference != NULL) {
			fprintf(stderr, "grouped reader %u, operation %llu: %s\n", reader->index,
			    (unsigned long long)sequence, difference);
			exit(1);
		}
		if (reader->index == 0 && sequence % 16U == 7U) {
			REQUIRE(btrfs_volume_sync(grouped->volume,
				    btrfs_volume_pending(grouped->volume)) == BTRFS_OK);
			grouped->reader_syncs++;
		}
	}
	free(buffer);
	return NULL;
}

static void
grouped_stress_test(struct harness *harness, size_t iterations)
{
	struct grouped grouped;
	struct grouped_reader readers[STRESS_READERS];
	struct stress_model model;
	struct btrfs_volume_view *view;
	struct btrfs_transaction *transaction;
	struct btrfs_new_inode attributes;
	struct btrfs_object_id stamp = { 0, 0 };
	struct btrfs_inode root;
	struct btrfs_time time = { 1800000000, 0 };
	const struct btrfs_fs *fs;
	uint8_t sequence[8];
	uint8_t *data;
	uint8_t *buffer;
	uint32_t random = 0x9e3779b9U;
	uint32_t version = 0;
	uint64_t generation;
	size_t syncs = 0;
	size_t iteration;
	unsigned i;

	memset(&grouped, 0, sizeof(grouped));
	memset(&model, 0, sizeof(model));
	memset(&attributes, 0, sizeof(attributes));
	attributes.mode = BTRFS_MODE_REGULAR | 0644;
	grouped.capacity = iterations + 1U;
	grouped.models = calloc(grouped.capacity, sizeof(*grouped.models));
	data = malloc(STRESS_MAX_SIZE);
	buffer = malloc(STRESS_MAX_SIZE);
	REQUIRE(grouped.models != NULL && data != NULL && buffer != NULL);
	REQUIRE(btrfs_volume_open(&harness->environment, &harness->device, &harness->callbacks,
		    BTRFS_TOP_LEVEL_TREE, &grouped.volume) == BTRFS_OK);
	fs = btrfs_volume_pin(grouped.volume, &view);
	REQUIRE(btrfs_root(fs, &root) == BTRFS_OK);
	REQUIRE(stress_check(fs, model.versions, buffer) == NULL && grouped_sequence(fs) == 0);
	btrfs_volume_unpin(grouped.volume, view);
	generation = btrfs_volume_generation(grouped.volume);
	for (i = 0; i < STRESS_READERS; i++) {
		readers[i].grouped = &grouped;
		readers[i].index = i;
		REQUIRE(pthread_create(&readers[i].thread, NULL, grouped_reader, &readers[i]) == 0);
	}
	for (iteration = 1; iteration <= iterations; iteration++) {
		time.seconds++;
		attributes.time = time;
		REQUIRE(btrfs_volume_join(grouped.volume, GROUPED_STRESS_NODES, &transaction) ==
		    BTRFS_OK);
		REQUIRE(stress_transaction(transaction, root.id, &model, &random, &version, data,
			    time) == BTRFS_OK);
		if (stamp.inode == 0) {
			REQUIRE(btrfs_transaction_create(transaction, root.id, GROUPED_STAMP,
				    strlen(GROUPED_STAMP), &attributes, &stamp) == BTRFS_OK);
		}
		for (i = 0; i < sizeof(sequence); i++) {
			sequence[i] = (uint8_t)(iteration >> (8U * i));
		}
		REQUIRE(btrfs_transaction_write(
			    transaction, stamp, 0, sequence, sizeof(sequence), time) == BTRFS_OK);
		/* Readers see this model once the operation leaves. */
		memcpy(grouped.models[iteration], model.versions, sizeof(model.versions));
		btrfs_volume_leave(grouped.volume, transaction);
		grouped.acknowledged = iteration;
		if (stress_random(&random) % 8U == 0) {
			REQUIRE(btrfs_volume_sync(grouped.volume,
				    btrfs_volume_pending(grouped.volume)) == BTRFS_OK);
			syncs++;
		}
	}
	REQUIRE(
	    btrfs_volume_sync(grouped.volume, btrfs_volume_pending(grouped.volume)) == BTRFS_OK);
	grouped.stop = 1;
	for (i = 0; i < STRESS_READERS; i++) {
		REQUIRE(pthread_join(readers[i].thread, NULL) == 0);
	}
	REQUIRE(btrfs_volume_failure(grouped.volume) == BTRFS_OK);
	generation = btrfs_volume_generation(grouped.volume) - generation;
	btrfs_volume_close(grouped.volume);

	/* The medium holds the last operation. */
	REQUIRE(btrfs_volume_open(&harness->environment, NULL, &harness->callbacks,
		    BTRFS_TOP_LEVEL_TREE, &grouped.volume) == BTRFS_OK);
	fs = btrfs_volume_pin(grouped.volume, &view);
	REQUIRE(grouped_sequence(fs) == iterations);
	REQUIRE(stress_check(fs, model.versions, buffer) == NULL);
	btrfs_volume_unpin(grouped.volume, view);
	btrfs_volume_close(grouped.volume);
	REQUIRE(grouped.running_reads != 0 && grouped.committed_reads != 0 && generation != 0 &&
	    generation < iterations);
	printf("grouped volume stress: %zu operations in %llu commits (%zu writer and %llu "
	       "reader syncs), %llu reads of the running transaction and %llu of committed "
	       "views PASS\n",
	    iterations, (unsigned long long)generation, syncs,
	    (unsigned long long)grouped.reader_syncs, (unsigned long long)grouped.running_reads,
	    (unsigned long long)grouped.committed_reads);
	free(grouped.models);
	free(data);
	free(buffer);
}

#ifdef BTRFS_VOLUME_TEST_HOOKS
extern void (*btrfs_volume_test_pin_hook)(void);

/* Milliseconds a pinner stays between reading the current view and pinning
 * it. */
#define WINDOW_MILLISECONDS 100L

static _Atomic int window_armed;

static void
window_pause(void)
{
	struct timespec delay = { 0, WINDOW_MILLISECONDS * 1000000L };

	if (atomic_exchange(&window_armed, 0)) {
		nanosleep(&delay, NULL);
	}
}

struct window_reader {
	struct btrfs_volume *volume;
	struct btrfs_volume_view *view;
	const struct btrfs_fs *fs;
};

static void *
window_pin(void *context)
{
	struct window_reader *reader = context;

	reader->fs = btrfs_volume_pin(reader->volume, &reader->view);
	return NULL;
}

/* A pinner that read the current view just before a commit replaced it still
 * pins that view: the commit's retirement waits for it and keeps the view. */
static void
window_test(struct harness *harness)
{
	struct window_reader reader;
	struct btrfs_transaction *transaction;
	struct timespec poll = { 0, 1000000L };
	pthread_t thread;
	uint64_t generation;

	REQUIRE(btrfs_volume_open(&harness->environment, &harness->device, &harness->callbacks,
		    BTRFS_TOP_LEVEL_TREE, &reader.volume) == BTRFS_OK);
	generation = btrfs_volume_generation(reader.volume);
	btrfs_volume_test_pin_hook = window_pause;
	window_armed = 1;
	REQUIRE(pthread_create(&thread, NULL, window_pin, &reader) == 0);
	while (window_armed) {
		nanosleep(&poll, NULL);
	}
	REQUIRE(btrfs_volume_begin(reader.volume, &transaction) == BTRFS_OK);
	REQUIRE(create_file(reader.volume, transaction, "window") == BTRFS_OK);
	REQUIRE(btrfs_volume_commit(reader.volume, transaction) == BTRFS_OK);
	REQUIRE(pthread_join(thread, NULL) == 0);
	btrfs_volume_test_pin_hook = NULL;
	REQUIRE(btrfs_volume_generation(reader.volume) == generation + 1);
	REQUIRE(exists(reader.fs, "/greeting") && !exists(reader.fs, "/window"));
	btrfs_volume_unpin(reader.volume, reader.view);
	btrfs_volume_close(reader.volume);
	printf("native volume pin window: a view read before a commit replaced it stays pinned "
	       "PASS\n");
}
#endif

int
main(int argc, char **argv)
{
	struct harness harness;
	char *end;
	unsigned long iterations;

#ifdef BTRFS_VOLUME_TEST_HOOKS
	if (argc == 3 && strcmp(argv[1], "--window") == 0) {
		harness_open(&harness, argv[2]);
		window_test(&harness);
		harness_close(&harness);
		return 0;
	}
#endif
	if (argc == 4 && strcmp(argv[1], "--grouped") == 0) {
		iterations = strtoul(argv[2], &end, 10);
		REQUIRE(*end == '\0' && iterations != 0 && iterations < 1000000UL);
		harness_open(&harness, argv[3]);
		grouped_stress_test(&harness, (size_t)iterations);
		harness_close(&harness);
		return 0;
	}
	if (argc == 4 && strcmp(argv[1], "--stress") == 0) {
		iterations = strtoul(argv[2], &end, 10);
		REQUIRE(*end == '\0' && iterations != 0 && iterations < 1000000UL);
		harness_open(&harness, argv[3]);
		stress_test(&harness, (size_t)iterations);
		harness_close(&harness);
		return 0;
	}
	REQUIRE(argc == 2);
	harness_open(&harness, argv[1]);
	views_test(&harness);
	harness_close(&harness);
	harness_open(&harness, argv[1]);
	grouped_test(&harness);
	harness_close(&harness);
	harness_open(&harness, argv[1]);
	compression_test(&harness);
	harness_close(&harness);
	harness_open(&harness, argv[1]);
	compressed_test(&harness);
	harness_close(&harness);
	return 0;
}
