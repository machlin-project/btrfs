/* SPDX-License-Identifier: BSD-3-Clause */
/* The native volume layer on clones of a volume Linux wrote across several
 * devices (tests/prepare_multidev_linux.py): a writer without a device for
 * each further one is refused, and a degraded mount is not admitted for
 * writing. Then grouped operations, as a native mount issues them, create
 * files and write data across the stripes, overwrite and remove some, and
 * sync; every commit's writes reach every device. Afterwards the volume,
 * mounted from each of its devices, holds what was written, and the
 * reference audit, which reads every copy and RAID5/6 rebuild, and the
 * namespace audit pass; read-only and degraded without each device in turn,
 * as the volume layer opens it, every written file reads the same where the
 * profile keeps a copy. */
#include "../adapters/posix/image.h"
#include "internal.h"
#include "namespace_audit.h"
#include "references.h"
#include <btrfs/volume.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/clonefile.h>
#include <unistd.h>

#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define MAX_DEVICES 16U
#define PATH_BYTES 4096U
#define SECONDS 1800000000
/* Files written, each in pieces as a native mount pushes clusters, larger
 * than a full row of stripes on every fixture. */
#define FILES 6U
#define FILE_BYTES (3U * 1024U * 1024U + 4096U + 17U)
#define PIECE_BYTES (256U * 1024U)
#define OPERATION_NODES 64U
#define DATA_BYTES_PER_NODE (64U * 1024U)
/* A sync after this many pieces, as a writer fsyncs. */
#define SYNC_PIECES 5U
/* The overwritten range of the first file and the removed file. */
#define OVERWRITE_OFFSET 70000U
#define OVERWRITE_BYTES 300000U
#define REMOVED 3U

struct locks {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
};

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
	(void)channel;
	pthread_cond_wait(&((struct locks *)context)->condition, &((struct locks *)context)->mutex);
}

static void
wake(void *context, const void *channel)
{
	(void)channel;
	pthread_cond_broadcast(&((struct locks *)context)->condition);
}

static void
fill(uint8_t *bytes, size_t size, uint32_t seed)
{
	size_t i;

	for (i = 0; i < size; i++) {
		seed ^= seed << 13;
		seed ^= seed >> 17;
		seed ^= seed << 5;
		bytes[i] = (uint8_t)seed;
	}
}

static struct btrfs_object_id
root_id(struct btrfs_volume *volume)
{
	struct btrfs_volume_view *view;
	struct btrfs_inode root;

	REQUIRE(btrfs_root(btrfs_volume_pin(volume, &view), &root) == BTRFS_OK);
	btrfs_volume_unpin(volume, view);
	return root.id;
}

static void
sync_volume(struct btrfs_volume *volume)
{
	REQUIRE(btrfs_volume_sync(volume, btrfs_volume_pending(volume)) == BTRFS_OK);
}

/* Whether the file named name reads as bytes from the volume fs. */
static int
reads_as(struct btrfs_fs *fs, const char *name, const uint8_t *bytes, size_t size)
{
	struct btrfs_inode inode;
	uint8_t *back = malloc(size);
	char path[PATH_BYTES];
	size_t completed = 0;
	int same;

	REQUIRE(back != NULL);
	REQUIRE(snprintf(path, sizeof(path), "/%s", name) < (int)sizeof(path));
	same = btrfs_image_lookup(fs, path, &inode) == BTRFS_OK && inode.size == size &&
	    btrfs_read(fs, &inode, 0, back, size, &completed) == BTRFS_OK && completed == size &&
	    memcmp(back, bytes, size) == 0;
	free(back);
	return same;
}

static void
audit(const struct btrfs_fs *fs, const char *when, size_t *copies)
{
	struct reference_audit references;
	struct namespace_audit names;

	if (reference_audit(fs, &references) != 0) {
		fprintf(stderr, "%s: reference audit: %s\n", when, references.failure);
		exit(1);
	}
	if (namespace_audit(fs, &names) != 0) {
		fprintf(stderr, "%s: namespace audit: %s\n", when, names.failure);
		exit(1);
	}
	*copies = references.checked_copies;
}

/* Opens the clones with first as the device mounted from and the others in
 * turn, without the device skip unless it is devices. */
static void
open_order(const char *const *paths, size_t devices, size_t first, size_t skip, int writable,
    struct btrfs_image *image)
{
	const char *order[MAX_DEVICES];
	size_t count = 0;
	size_t i;

	for (i = 0; i < devices; i++) {
		if ((first + i) % devices != skip) {
			order[count++] = paths[(first + i) % devices];
		}
	}
	REQUIRE((writable ? btrfs_image_open_devices_writable(order, count, image)
			  : btrfs_image_open_devices(order, count, image)) == 0);
	image->environment.degraded = skip < devices;
}

int
main(int argc, char **argv)
{
	static uint8_t data[FILES][FILE_BYTES];
	struct btrfs_new_inode attributes;
	struct btrfs_write_environment writer;
	struct btrfs_volume_locks callbacks;
	struct btrfs_transaction *transaction;
	struct btrfs_volume *volume;
	struct btrfs_volume_view *view;
	struct btrfs_fs *pinned;
	struct btrfs_object_id ids[FILES];
	struct btrfs_time time = { SECONDS, 0 };
	struct btrfs_image image;
	struct btrfs_fs *fs;
	struct locks locks;
	char paths[MAX_DEVICES][PATH_BYTES];
	const char *list[MAX_DEVICES];
	char name[32];
	size_t devices;
	size_t copies = 0;
	size_t pieces = 0;
	size_t offset;
	size_t piece;
	size_t degraded_reads = 0;
	size_t f;
	size_t i;
	int redundant;

	REQUIRE(argc >= 5);
	devices = (size_t)(argc - 3);
	REQUIRE(devices <= MAX_DEVICES);
	redundant = strcmp(argv[1], "redundant") == 0;
	for (i = 0; i < devices; i++) {
		REQUIRE(snprintf(paths[i], sizeof(paths[i]), "%s/volume-devices-%ld-%zu.raw",
			    argv[2], (long)getpid(), i) < (int)PATH_BYTES);
		REQUIRE(clonefile(argv[3 + i], paths[i], 0) == 0);
		list[i] = paths[i];
	}
	pthread_mutex_init(&locks.mutex, NULL);
	pthread_cond_init(&locks.condition, NULL);
	callbacks = (struct btrfs_volume_locks){ &locks, lock, unlock, wait_on, wake };

	/* Refusals: a writer short of a device, a degraded writable mount. */
	open_order(list, devices, 0, devices, 1, &image);
	btrfs_image_writer(&image, &writer);
	writer.device_count--;
	REQUIRE(btrfs_volume_open(&image.environment, &writer, &callbacks, BTRFS_TOP_LEVEL_TREE,
		    &volume) == BTRFS_INVALID_ARGUMENT);
	btrfs_image_close(&image);
	open_order(list, devices, 0, devices - 1, 1, &image);
	btrfs_image_writer(&image, &writer);
	REQUIRE(btrfs_volume_open(&image.environment, &writer, &callbacks, BTRFS_TOP_LEVEL_TREE,
		    &volume) == BTRFS_UNSUPPORTED);
	REQUIRE(image.live_allocations == 0);
	btrfs_image_close(&image);

	/* Grouped operations, synced every few pieces. */
	open_order(list, devices, 0, devices, 1, &image);
	btrfs_image_writer(&image, &writer);
	REQUIRE(btrfs_volume_open(&image.environment, &writer, &callbacks, BTRFS_TOP_LEVEL_TREE,
		    &volume) == BTRFS_OK);
	memset(&attributes, 0, sizeof(attributes));
	attributes.mode = BTRFS_MODE_REGULAR | 0644;
	attributes.time = time;
	for (f = 0; f < FILES; f++) {
		fill(data[f], FILE_BYTES, (uint32_t)(0x9e3779b9U * (f + 1)));
		REQUIRE(snprintf(name, sizeof(name), "spread-%zu", f) < (int)sizeof(name));
		REQUIRE(btrfs_volume_join(volume, OPERATION_NODES, &transaction) == BTRFS_OK);
		REQUIRE(btrfs_transaction_create(transaction, root_id(volume), name, strlen(name),
			    &attributes, &ids[f]) == BTRFS_OK);
		btrfs_volume_leave(volume, transaction);
		for (offset = 0; offset < FILE_BYTES; offset += piece) {
			piece =
			    FILE_BYTES - offset < PIECE_BYTES ? FILE_BYTES - offset : PIECE_BYTES;
			REQUIRE(btrfs_volume_join(volume,
				    OPERATION_NODES + piece / DATA_BYTES_PER_NODE + 1,
				    &transaction) == BTRFS_OK);
			REQUIRE(btrfs_transaction_write(transaction, ids[f], offset,
				    data[f] + offset, piece, time) == BTRFS_OK);
			btrfs_volume_leave(volume, transaction);
			if (++pieces % SYNC_PIECES == 0) {
				sync_volume(volume);
			}
		}
	}
	sync_volume(volume);
	/* An overwrite inside the first file and a removal. */
	fill(data[0] + OVERWRITE_OFFSET, OVERWRITE_BYTES, 0x2545f491U);
	REQUIRE(
	    btrfs_volume_join(volume, OPERATION_NODES + OVERWRITE_BYTES / DATA_BYTES_PER_NODE + 1,
		&transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_write(transaction, ids[0], OVERWRITE_OFFSET,
		    data[0] + OVERWRITE_OFFSET, OVERWRITE_BYTES, time) == BTRFS_OK);
	btrfs_volume_leave(volume, transaction);
	REQUIRE(snprintf(name, sizeof(name), "spread-%u", REMOVED) < (int)sizeof(name));
	REQUIRE(btrfs_volume_join(volume, OPERATION_NODES, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_unlink(
		    transaction, root_id(volume), name, strlen(name), time, 0) == BTRFS_OK);
	btrfs_volume_leave(volume, transaction);
	sync_volume(volume);
	REQUIRE(btrfs_volume_failure(volume) == BTRFS_OK);
	btrfs_volume_close(volume);
	REQUIRE(image.live_allocations == 0);
	btrfs_image_close(&image);

	/* Mounted from each device, the files read as written. */
	for (i = 0; i < devices; i++) {
		open_order(list, devices, i, devices, 0, &image);
		REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
		for (f = 0; f < FILES; f++) {
			REQUIRE(snprintf(name, sizeof(name), "spread-%zu", f) < (int)sizeof(name));
			REQUIRE(f == REMOVED ? !reads_as(fs, name, data[f], FILE_BYTES)
					     : reads_as(fs, name, data[f], FILE_BYTES));
		}
		if (i == 0) {
			audit(fs, "written", &copies);
		}
		btrfs_unmount(fs);
		REQUIRE(image.live_allocations == 0);
		btrfs_image_close(&image);
	}
	/* Read-only and degraded through the volume layer. */
	for (i = 0; redundant && i < devices; i++) {
		open_order(list, devices, i == 0 ? 1 : 0, i, 0, &image);
		REQUIRE(btrfs_volume_open(&image.environment, NULL, &callbacks,
			    BTRFS_TOP_LEVEL_TREE, &volume) == BTRFS_OK);
		pinned = (struct btrfs_fs *)btrfs_volume_pin(volume, &view);
		for (f = 0; f < FILES; f++) {
			if (f == REMOVED) {
				continue;
			}
			REQUIRE(snprintf(name, sizeof(name), "spread-%zu", f) < (int)sizeof(name));
			REQUIRE(reads_as(pinned, name, data[f], FILE_BYTES));
			degraded_reads++;
		}
		btrfs_volume_unpin(volume, view);
		btrfs_volume_close(volume);
		REQUIRE(image.live_allocations == 0);
		btrfs_image_close(&image);
	}
	for (i = 0; i < devices; i++) {
		REQUIRE(unlink(paths[i]) == 0);
	}
	printf("volume devices: %zu devices; %u files of %u bytes written in %zu grouped pieces, "
	       "one overwritten and one removed, read the same mounted from each device and "
	       "pass both audits (%zu checksummed copies); %zu files read degraded without each "
	       "device; a writer short of a device and a degraded writer refused PASS\n",
	    devices, FILES, FILE_BYTES, pieces, copies, degraded_reads);
	return 0;
}
