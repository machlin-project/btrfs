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
#include <btrfs/volume.h>
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
    size_t input_size, void *output, size_t output_size)
{
	struct overlay *overlay = context;

	return overlay->image->environment.decompress(
	    overlay->image, codec, input, input_size, output, output_size);
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
		overlay_flush, NULL, BTRFS_COMPRESSION_NONE };
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
	new_fs = btrfs_volume_pin(volume, &new_view);
	REQUIRE(exists(new_fs, "/created") && !exists(new_fs, "/aborted"));
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

	length = stress_name(name, file);
	if (model->versions[file] == 0) {
		memset(&attributes, 0, sizeof(attributes));
		attributes.mode = BTRFS_MODE_REGULAR | 0644;
		attributes.time = time;
		result = btrfs_transaction_create(
		    transaction, root, name, length, &attributes, &model->ids[file]);
	} else {
		result = btrfs_transaction_truncate(transaction, model->ids[file], 0, time);
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

int
main(int argc, char **argv)
{
	struct harness harness;
	char *end;
	unsigned long iterations;

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
	return 0;
}
