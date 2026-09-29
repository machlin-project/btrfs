/* SPDX-License-Identifier: BSD-3-Clause */
#include "../adapters/posix/image.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct worker {
	struct btrfs_fs *fs;
	unsigned index;
};

static void *
read_worker(void *argument)
{
	struct worker *worker = argument;
	struct btrfs_inode inode;
	struct btrfs_inode directory;
	struct btrfs_directory *stream;
	struct btrfs_dir_entry entry;
	uint8_t *buffer;
	uint64_t offset;
	uint64_t cookie;
	size_t completed;
	size_t i;
	unsigned round;
	const size_t length = 65539;

	buffer = malloc(length);
	assert(buffer != NULL);
	for (round = 0; round < 32; round++) {
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
	}
	free(buffer);
	return NULL;
}

int
main(int argc, char **argv)
{
	struct btrfs_image image;
	struct btrfs_fs *fs;
	struct worker workers[8];
	pthread_t threads[8];
	size_t i;

	assert(argc == 2);
	assert(btrfs_image_open(argv[1], &image) == 0);
	assert(btrfs_mount(&image.environment, 5, &fs) == BTRFS_OK);
	for (i = 0; i < 8; i++) {
		workers[i].fs = fs;
		workers[i].index = (unsigned)i;
		assert(pthread_create(&threads[i], NULL, read_worker, &workers[i]) == 0);
	}
	for (i = 0; i < 8; i++) {
		assert(pthread_join(threads[i], NULL) == 0);
	}
	btrfs_unmount(fs);
	assert(image.live_allocations == 0 && image.live_bytes == 0);
	btrfs_image_close(&image);
	puts("8 concurrent readers and directory streams, shared immutable mount, balanced "
	     "allocations: PASS");
	return 0;
}
