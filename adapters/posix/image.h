/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_IMAGE_H
#define MACHLIN_BTRFS_IMAGE_H
#include <btrfs/btrfs.h>
#include <btrfs/write.h>
#include <stdatomic.h>

/* Further images of a volume spanning several devices. */
#define BTRFS_IMAGE_DEVICES 15U

struct btrfs_image;

struct btrfs_image_device {
	struct btrfs_image *image;
	int descriptor;
};

struct btrfs_image {
	int descriptor;
	struct btrfs_environment environment;
	_Atomic uint64_t reads, bytes_read, allocations, live_allocations, live_bytes;
	uint64_t fail_read, fail_allocate;
	struct btrfs_image_device extra[BTRFS_IMAGE_DEVICES];
	struct btrfs_device devices[BTRFS_IMAGE_DEVICES];
	struct btrfs_write_device writers[BTRFS_IMAGE_DEVICES];
};

int btrfs_image_open(const char *path, struct btrfs_image *image);
/* Opens the images of a volume spanning several devices for reading: the
 * first is the device it is mounted from, the others its further devices. */
int btrfs_image_open_devices(const char *const *paths, size_t count, struct btrfs_image *image);
/* Opens the image for reading and writing; btrfs_image_writer then supplies
 * exact positioned writes and a barrier that reaches stable storage, on each
 * device the images of btrfs_image_open_devices_writable. */
int btrfs_image_open_writable(const char *path, struct btrfs_image *image);
int btrfs_image_open_devices_writable(
    const char *const *paths, size_t count, struct btrfs_image *image);
void btrfs_image_writer(struct btrfs_image *image, struct btrfs_write_environment *writer);
void btrfs_image_close(struct btrfs_image *image);
/* zlib and Zstd compression for the write environment's compress callback. */
enum btrfs_result btrfs_image_compress(void *context, enum btrfs_compression codec,
    const void *input, size_t input_size, void *output, size_t capacity, size_t *size);
enum btrfs_result btrfs_image_lookup(
    struct btrfs_fs *fs, const char *path, struct btrfs_inode *inode);
#endif
