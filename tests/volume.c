/* SPDX-License-Identifier: BSD-3-Clause */
/* The native volume's versioned views on a Linux fixture: a pinned view keeps
 * its committed root set after later commits, the next writer waits until older
 * views drain, an uncertain commit leaves the volume failed but readable, and
 * aborted or empty transactions publish nothing. Writes go to an overlay. */
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
#define OVERLAY_SLOTS 65536U
#define WAIT_MILLISECONDS 100L

/* Written sectors, kept in an open-addressing table over the fixture. */
struct overlay {
	struct btrfs_image *image;
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
	for (done = 0; error == BTRFS_OK && done < length; done += amount) {
		sector = (offset + done) / SECTOR;
		within = (size_t)((offset + done) % SECTOR);
		amount = SECTOR - within < length - done ? SECTOR - within : length - done;
		slot = overlay_slot(overlay, sector);
		if (overlay->sectors[slot] == sector) {
			memcpy(bytes + done, overlay->bytes + slot * SECTOR + within, amount);
		}
	}
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
	for (done = 0; done < length; done += SECTOR) {
		slot = overlay_slot(overlay, (offset + done) / SECTOR);
		if (overlay->sectors[slot] == UINT64_MAX) {
			REQUIRE(++overlay->count < OVERLAY_SLOTS / 2);
			overlay->sectors[slot] = (offset + done) / SECTOR;
		}
		memcpy(overlay->bytes + slot * SECTOR, bytes + done, SECTOR);
	}
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

int
main(int argc, char **argv)
{
	struct btrfs_image image;
	struct btrfs_environment environment;
	struct btrfs_write_environment device;
	struct btrfs_volume_locks callbacks;
	struct btrfs_volume *volume;
	struct btrfs_volume_view *old_view;
	struct btrfs_volume_view *new_view;
	struct btrfs_transaction *transaction;
	struct overlay overlay;
	struct locks locks;
	struct writer writer;
	const struct btrfs_fs *old_fs;
	const struct btrfs_fs *new_fs;
	pthread_t thread;
	uint64_t generation;
	size_t i;

	REQUIRE(argc == 2);
	REQUIRE(btrfs_image_open(argv[1], &image) == 0);
	memset(&overlay, 0, sizeof(overlay));
	overlay.image = &image;
	overlay.sectors = malloc(OVERLAY_SLOTS * sizeof(*overlay.sectors));
	overlay.bytes = malloc((size_t)OVERLAY_SLOTS * SECTOR);
	REQUIRE(overlay.sectors != NULL && overlay.bytes != NULL);
	for (i = 0; i < OVERLAY_SLOTS; i++) {
		overlay.sectors[i] = UINT64_MAX;
	}
	environment = image.environment;
	environment.context = &overlay;
	environment.read = overlay_read;
	environment.allocate = overlay_allocate;
	environment.release = overlay_release;
	environment.decompress = overlay_decompress;
	device = (struct btrfs_write_environment){ &overlay, overlay_write, overlay_flush };
	pthread_mutex_init(&locks.mutex, NULL);
	pthread_cond_init(&locks.condition, NULL);
	callbacks = (struct btrfs_volume_locks){ &locks, lock, unlock, wait_on, wake };

	/* A read-only volume serves views and refuses writers. */
	REQUIRE(btrfs_volume_open(&environment, NULL, &callbacks, BTRFS_TOP_LEVEL_TREE, &volume) ==
	    BTRFS_OK);
	REQUIRE(!btrfs_volume_writable(volume));
	old_fs = btrfs_volume_pin(volume, &old_view);
	REQUIRE(exists(old_fs, "/greeting"));
	btrfs_volume_unpin(volume, old_view);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_READ_ONLY);
	btrfs_volume_close(volume);
	REQUIRE(overlay.writes == 0);

	REQUIRE(btrfs_volume_open(
		    &environment, &device, &callbacks, BTRFS_TOP_LEVEL_TREE, &volume) == BTRFS_OK);
	REQUIRE(btrfs_volume_writable(volume) && overlay.writes == 0);
	generation = btrfs_volume_generation(volume);

	/* An empty and an aborted transaction publish nothing. */
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_OK);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_file(volume, transaction, "aborted") == BTRFS_OK);
	btrfs_volume_abort(volume, transaction);
	REQUIRE(btrfs_volume_generation(volume) == generation && overlay.writes == 0);

	/* A pinned view keeps the old root set; the new view has the file. */
	old_fs = btrfs_volume_pin(volume, &old_view);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_file(volume, transaction, "created") == BTRFS_OK);
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_OK);
	REQUIRE(overlay.flushes == 3);
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

	/* A failed commit that issued writes leaves the volume failed. */
	old_fs = btrfs_volume_pin(volume, &old_view);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_OK);
	REQUIRE(create_file(volume, transaction, "uncertain") == BTRFS_OK);
	overlay.fail_write = overlay.writes + 2;
	REQUIRE(btrfs_volume_commit(volume, transaction) == BTRFS_IO);
	REQUIRE(btrfs_volume_failure(volume) == BTRFS_IO);
	REQUIRE(btrfs_volume_generation(volume) == generation + 2);
	REQUIRE(btrfs_volume_begin(volume, &transaction) == BTRFS_IO);
	REQUIRE(exists(old_fs, "/second") && !exists(old_fs, "/uncertain"));
	btrfs_volume_unpin(volume, old_view);
	btrfs_volume_close(volume);
	REQUIRE(image.live_allocations == 0);
	btrfs_image_close(&image);
	free(overlay.sectors);
	free(overlay.bytes);
	printf("native volume views: pins, draining, abort, empty and failed commits PASS\n");
	return 0;
}
