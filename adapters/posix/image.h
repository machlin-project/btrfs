/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_IMAGE_H
#define MACHLIN_BTRFS_IMAGE_H
#include <btrfs/btrfs.h>
#include <stdatomic.h>

struct btrfs_image {
	int descriptor;
	struct btrfs_environment environment;
	_Atomic uint64_t reads, bytes_read, allocations, live_allocations, live_bytes;
	uint64_t fail_read, fail_allocate;
};

int btrfs_image_open(const char *path, struct btrfs_image *image);
void btrfs_image_close(struct btrfs_image *image);
/* zlib and Zstd compression for the write environment's compress callback. */
enum btrfs_result btrfs_image_compress(void *context, enum btrfs_compression codec,
    const void *input, size_t input_size, void *output, size_t capacity, size_t *size);
enum btrfs_result btrfs_image_lookup(
    struct btrfs_fs *fs, const char *path, struct btrfs_inode *inode);
#endif
