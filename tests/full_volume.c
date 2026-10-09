/* SPDX-License-Identifier: BSD-3-Clause */
/* A full volume through the native volume layer, on a clone of a Linux image:
 * a file grows by grouped writes until NO_SPACE, synced every few pieces as a
 * writer fsyncs, the last round applying in part; then files with
 * large xattrs fill the metadata until NO_SPACE; the volume stays healthy.
 * Deleting the large file then runs in eviction steps that may take the
 * metadata reserve, committed as room requires, a new file fits, and a fresh
 * mount reads the result with the space released. */
#include "../adapters/posix/image.h"
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

/* One grouped write, as the XNU adapter pushes a cluster of cached data, and
 * the tree nodes it declares: an operation and one node per 64 KiB of data.
 * A sync follows every few pieces, as the mounted write suite fsyncs 4 MiB. */
#define FILL_PIECE_BYTES (256U * 1024U)
#define FILL_SYNC_PIECES 16U
#define OPERATION_NODES 64U
#define DATA_BYTES_PER_NODE (64U * 1024U)
/* More pieces than any fixture holds: the fill must end with NO_SPACE. */
#define FILL_LIMIT_PIECES 16384U
/* Metadata ballast: files with an xattr of this many bytes, synced in groups,
 * more than any fixture's metadata holds. */
#define BALLAST_XATTR_BYTES 3000U
#define BALLAST_SYNC_FILES 64U
#define BALLAST_LIMIT_FILES 100000U
#define AFTER_BYTES 4096U

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

static enum btrfs_result
create(struct btrfs_volume *volume, const char *name, struct btrfs_object_id *id)
{
	struct btrfs_new_inode attributes;
	struct btrfs_transaction *transaction;
	enum btrfs_result result;

	memset(&attributes, 0, sizeof(attributes));
	attributes.mode = BTRFS_MODE_REGULAR | 0644;
	attributes.time.seconds = 1800000000;
	result = btrfs_volume_join(volume, OPERATION_NODES, &transaction);
	if (result == BTRFS_OK) {
		result = btrfs_transaction_create(
		    transaction, root_id(volume), name, strlen(name), &attributes, id);
		btrfs_volume_leave(volume, transaction);
	}
	return result;
}

static enum btrfs_result
write_piece(struct btrfs_volume *volume, struct btrfs_object_id id, uint64_t offset,
    const uint8_t *data, size_t size)
{
	struct btrfs_transaction *transaction;
	struct btrfs_time time = { 1800000000, 0 };
	enum btrfs_result result;

	result =
	    btrfs_volume_join(volume, OPERATION_NODES + size / DATA_BYTES_PER_NODE, &transaction);
	if (result == BTRFS_OK) {
		result = btrfs_transaction_write(transaction, id, offset, data, size, time);
		btrfs_volume_leave(volume, transaction);
	}
	return result;
}

static void
sync_volume(struct btrfs_volume *volume)
{
	REQUIRE(btrfs_volume_sync(volume, btrfs_volume_pending(volume)) == BTRFS_OK);
}

/* A file with a large xattr, in one operation. */
static enum btrfs_result
ballast(struct btrfs_volume *volume, unsigned index, const uint8_t *value)
{
	struct btrfs_new_inode attributes;
	struct btrfs_transaction *transaction;
	struct btrfs_object_id id;
	char name[32];
	enum btrfs_result result;

	memset(&attributes, 0, sizeof(attributes));
	attributes.mode = BTRFS_MODE_REGULAR | 0644;
	attributes.time.seconds = 1800000000;
	REQUIRE(snprintf(name, sizeof(name), "ballast-%06u", index) < (int)sizeof(name));
	result = btrfs_volume_join(volume, OPERATION_NODES, &transaction);
	if (result != BTRFS_OK) {
		return result;
	}
	result = btrfs_transaction_create(
	    transaction, root_id(volume), name, strlen(name), &attributes, &id);
	if (result == BTRFS_OK) {
		result = btrfs_transaction_set_xattr(transaction, id, "user.ballast",
		    strlen("user.ballast"), value, BALLAST_XATTR_BYTES, 0, attributes.time);
	}
	btrfs_volume_leave(volume, transaction);
	return result;
}

int
main(int argc, char **argv)
{
	struct btrfs_image image;
	struct btrfs_write_environment writer;
	struct btrfs_volume_locks callbacks;
	struct btrfs_volume *volume;
	struct btrfs_volume_view *view;
	struct btrfs_transaction *transaction;
	struct btrfs_object_id filler;
	struct btrfs_object_id after;
	struct btrfs_inode inode;
	struct btrfs_info info;
	struct btrfs_fs *fs;
	struct btrfs_time time = { 1800000000, 0 };
	struct locks locks;
	char path[4096];
	uint8_t *data;
	uint8_t back[AFTER_BYTES];
	uint64_t filled = 0;
	uint64_t full_used;
	size_t completed;
	unsigned piece;
	unsigned files;
	enum btrfs_result result = BTRFS_OK;

	REQUIRE(argc == 3);
	REQUIRE(snprintf(path, sizeof(path), "%s/full-volume-%ld.raw", argv[2], (long)getpid()) <
	    (int)sizeof(path));
	/* An APFS clone: the fixture stays as it is, and the copy costs nothing. */
	REQUIRE(clonefile(argv[1], path, 0) == 0);
	data = malloc(FILL_PIECE_BYTES);
	REQUIRE(data != NULL);
	memset(data, 0x5a, FILL_PIECE_BYTES);
	REQUIRE(btrfs_image_open_writable(path, &image) == 0);
	btrfs_image_writer(&image, &writer);
	pthread_mutex_init(&locks.mutex, NULL);
	pthread_cond_init(&locks.condition, NULL);
	callbacks = (struct btrfs_volume_locks){ &locks, lock, unlock, wait_on, wake };
	REQUIRE(btrfs_volume_open(&image.environment, &writer, &callbacks, BTRFS_TOP_LEVEL_TREE,
		    &volume) == BTRFS_OK);
	REQUIRE(create(volume, "filler", &filler) == BTRFS_OK);
	for (piece = 0; piece < FILL_LIMIT_PIECES; piece++) {
		result = write_piece(
		    volume, filler, (uint64_t)piece * FILL_PIECE_BYTES, data, FILL_PIECE_BYTES);
		if (result != BTRFS_OK) {
			break;
		}
		filled += FILL_PIECE_BYTES;
		if (piece % FILL_SYNC_PIECES == FILL_SYNC_PIECES - 1) {
			sync_volume(volume);
		}
	}
	REQUIRE(result == BTRFS_NO_SPACE && btrfs_volume_failure(volume) == BTRFS_OK);
	sync_volume(volume);
	for (files = 0; files < BALLAST_LIMIT_FILES; files++) {
		result = ballast(volume, files, data);
		if (result != BTRFS_OK) {
			break;
		}
		if (files % BALLAST_SYNC_FILES == BALLAST_SYNC_FILES - 1) {
			sync_volume(volume);
		}
	}
	REQUIRE(result == BTRFS_NO_SPACE && btrfs_volume_failure(volume) == BTRFS_OK);
	sync_volume(volume);
	btrfs_get_info(btrfs_volume_pin(volume, &view), &info);
	btrfs_volume_unpin(volume, view);
	full_used = info.used_bytes;
	/* In the same mount, as the mounted write suite does. */
	REQUIRE(btrfs_volume_join_releasing(volume, OPERATION_NODES, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_unlink(transaction, root_id(volume), "filler", 6, time, 0) ==
	    BTRFS_OK);
	btrfs_volume_leave(volume, transaction);
	REQUIRE(btrfs_volume_failure(volume) == BTRFS_OK);
	sync_volume(volume);
	REQUIRE(create(volume, "after-full", &after) == BTRFS_OK);
	memset(data, 0xa5, AFTER_BYTES);
	REQUIRE(write_piece(volume, after, 0, data, AFTER_BYTES) == BTRFS_OK);
	sync_volume(volume);
	REQUIRE(btrfs_volume_failure(volume) == BTRFS_OK);
	btrfs_volume_close(volume);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/filler", &inode) == BTRFS_NOT_FOUND);
	REQUIRE(btrfs_image_lookup(fs, "/after-full", &inode) == BTRFS_OK);
	REQUIRE(btrfs_read(fs, &inode, 0, back, sizeof(back), &completed) == BTRFS_OK);
	REQUIRE(completed == AFTER_BYTES && memcmp(back, data, AFTER_BYTES) == 0);
	btrfs_get_info(fs, &info);
	REQUIRE(info.used_bytes + filled / 2 < full_used);
	btrfs_unmount(fs);
	btrfs_image_close(&image);
	REQUIRE(unlink(path) == 0);
	free(data);
	printf("full volume: %llu MiB written and %u xattr files until NO_SPACE, %llu MiB used; "
	       "after deletion %llu MiB used and a new file written PASS\n",
	    (unsigned long long)(filled >> 20), files, (unsigned long long)(full_used >> 20),
	    (unsigned long long)(info.used_bytes >> 20));
	return 0;
}
