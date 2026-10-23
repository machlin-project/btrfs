/* SPDX-License-Identifier: BSD-3-Clause */
/* Balance and scrub on a mounted native volume, on clones of a Linux image
 * (one device or several): reader threads pin the current view, take the
 * content digest of every file, directory, xattr and subvolume and unpin,
 * over and over, while btrfs_volume_balance relocates every group, one step
 * per published transaction, and btrfs_volume_scrub then verifies and
 * repairs every copy. Every digest any reader takes equals the volume's
 * before; the volume stays healthy; afterwards no group of before remains,
 * a later operation commits, and the image mounted again reads the same and
 * passes the reference and namespace audits. */
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
#define MAX_GROUPS 256U
#define PATH_BYTES 4096U
#define READERS 3U
#define SECONDS 1800000000
#define OPERATION_NODES 64U
/* The usage filter that selects every group (Linux's usage=100). */
#define BALANCE_EVERY_GROUP 100U

struct locks {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
};

struct reader {
	pthread_t thread;
	struct btrfs_volume *volume;
	const struct namespace_digest *expected;
	volatile int *stop;
	size_t digests;
	size_t mismatches;
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

static int
same_digest(const struct namespace_digest *a, const struct namespace_digest *b)
{
	return a->hash == b->hash && a->objects == b->objects && a->bytes == b->bytes;
}

/* Digests the current view until told to stop. */
static void *
read_views(void *argument)
{
	struct reader *reader = argument;
	struct btrfs_volume_view *view;
	struct namespace_digest digest;
	const struct btrfs_fs *fs;

	while (!__atomic_load_n(reader->stop, __ATOMIC_ACQUIRE)) {
		fs = btrfs_volume_pin(reader->volume, &view);
		if (namespace_content_digest(fs, &digest) != 0 ||
		    !same_digest(&digest, reader->expected)) {
			reader->mismatches++;
		}
		btrfs_volume_unpin(reader->volume, view);
		reader->digests++;
	}
	return NULL;
}

static void
open_images(const char *const *paths, size_t devices, int writable, struct btrfs_image *image)
{
	REQUIRE((writable ? btrfs_image_open_devices_writable(paths, devices, image)
			  : btrfs_image_open_devices(paths, devices, image)) == 0);
}

int
main(int argc, char **argv)
{
	static struct reader readers[READERS];
	struct btrfs_write_environment writer;
	struct btrfs_volume_locks callbacks;
	struct btrfs_volume_view *view;
	struct btrfs_volume *volume;
	struct btrfs_transaction *transaction;
	struct btrfs_balance_args args;
	struct btrfs_balance_report report;
	struct btrfs_scrub_report scrubbed;
	struct btrfs_new_inode attributes;
	struct btrfs_object_id created;
	struct btrfs_inode root;
	struct btrfs_group before[MAX_GROUPS];
	struct btrfs_group after[MAX_GROUPS];
	struct namespace_digest expected;
	struct namespace_digest digest;
	struct reference_audit references;
	struct namespace_audit names;
	struct btrfs_image image;
	struct btrfs_fs *fs;
	struct locks locks;
	char paths[MAX_DEVICES][PATH_BYTES];
	const char *list[MAX_DEVICES];
	volatile int stop = 0;
	size_t devices;
	size_t before_count;
	size_t after_count;
	size_t digests = 0;
	size_t i;
	size_t j;

	REQUIRE(argc >= 3);
	devices = (size_t)(argc - 2);
	REQUIRE(devices <= MAX_DEVICES);
	for (i = 0; i < devices; i++) {
		REQUIRE(snprintf(paths[i], sizeof(paths[i]), "%s/volume-balance-%ld-%zu.raw",
			    argv[1], (long)getpid(), i) < (int)PATH_BYTES);
		REQUIRE(clonefile(argv[2 + i], paths[i], 0) == 0);
		list[i] = paths[i];
	}
	pthread_mutex_init(&locks.mutex, NULL);
	pthread_cond_init(&locks.condition, NULL);
	callbacks = (struct btrfs_volume_locks){ &locks, lock, unlock, wait_on, wake };
	open_images(list, devices, 1, &image);
	btrfs_image_writer(&image, &writer);
	REQUIRE(btrfs_volume_open(&image.environment, &writer, &callbacks, BTRFS_TOP_LEVEL_TREE,
		    &volume) == BTRFS_OK);
	fs = (struct btrfs_fs *)btrfs_volume_pin(volume, &view);
	REQUIRE(namespace_content_digest(fs, &expected) == 0);
	REQUIRE(btrfs_get_groups(fs, before, MAX_GROUPS, &before_count) == BTRFS_OK);
	btrfs_volume_unpin(volume, view);

	/* Readers throughout the balance and the scrub. */
	for (i = 0; i < READERS; i++) {
		readers[i] = (struct reader){ 0, volume, &expected, &stop, 0, 0 };
		REQUIRE(pthread_create(&readers[i].thread, NULL, read_views, &readers[i]) == 0);
	}
	memset(&args, 0, sizeof(args));
	args.data = (struct btrfs_balance_kind){ 1, BALANCE_EVERY_GROUP, BTRFS_BALANCE_KEEP, 0 };
	args.metadata = args.data;
	args.system = args.data;
	REQUIRE(btrfs_volume_balance(volume, &args, &report) == BTRFS_OK);
	REQUIRE(report.groups == before_count && report.commits >= report.groups);
	REQUIRE(btrfs_volume_scrub(volume, 1, &scrubbed) == BTRFS_OK);
	REQUIRE(scrubbed.corrupted == 0 && scrubbed.unreadable == 0 && scrubbed.repaired == 0);
	__atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
	for (i = 0; i < READERS; i++) {
		REQUIRE(pthread_join(readers[i].thread, NULL) == 0);
		REQUIRE(readers[i].mismatches == 0);
		digests += readers[i].digests;
	}
	REQUIRE(digests >= READERS && btrfs_volume_failure(volume) == BTRFS_OK);

	/* Every group of before is gone, and the volume still writes. */
	fs = (struct btrfs_fs *)btrfs_volume_pin(volume, &view);
	REQUIRE(btrfs_get_groups(fs, after, MAX_GROUPS, &after_count) == BTRFS_OK);
	REQUIRE(btrfs_root(fs, &root) == BTRFS_OK);
	btrfs_volume_unpin(volume, view);
	for (i = 0; i < before_count; i++) {
		for (j = 0; j < after_count; j++) {
			REQUIRE(after[j].logical != before[i].logical);
		}
	}
	memset(&attributes, 0, sizeof(attributes));
	attributes.mode = BTRFS_MODE_REGULAR | 0644;
	attributes.time = (struct btrfs_time){ SECONDS, 0 };
	REQUIRE(btrfs_volume_join(volume, OPERATION_NODES, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_create(transaction, root.id, "after-balance",
		    strlen("after-balance"), &attributes, &created) == BTRFS_OK);
	btrfs_volume_leave(volume, transaction);
	REQUIRE(btrfs_volume_sync(volume, btrfs_volume_pending(volume)) == BTRFS_OK);
	btrfs_volume_close(volume);
	REQUIRE(image.live_allocations == 0);
	btrfs_image_close(&image);

	/* Mounted again: the same files besides the new one, both audits. */
	open_images(list, devices, 0, &image);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/after-balance", &root) == BTRFS_OK);
	REQUIRE(
	    namespace_content_digest(fs, &digest) == 0 && digest.objects == expected.objects + 1);
	if (reference_audit(fs, &references) != 0) {
		fprintf(stderr, "reference audit: %s\n", references.failure);
		exit(1);
	}
	if (namespace_audit(fs, &names) != 0) {
		fprintf(stderr, "namespace audit: %s\n", names.failure);
		exit(1);
	}
	btrfs_unmount(fs);
	REQUIRE(image.live_allocations == 0);
	btrfs_image_close(&image);
	for (i = 0; i < devices; i++) {
		REQUIRE(unlink(paths[i]) == 0);
	}
	printf("volume balance: %zu devices; %llu groups relocated in %llu published "
	       "transactions and every copy scrubbed while %u readers took %zu digests of the "
	       "pinned view, every one equal to the volume's before; no group of before "
	       "remains, a later operation commits, and the image reads the same and passes "
	       "both audits PASS\n",
	    devices, (unsigned long long)report.groups, (unsigned long long)report.commits, READERS,
	    digests);
	return 0;
}
