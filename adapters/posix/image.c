/* SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 200809L
#include "image.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#ifdef BTRFS_HAVE_ZSTD
#include <zstd.h>
#endif

static enum btrfs_result
image_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct btrfs_image *image = context;
	size_t done = 0;
	ssize_t amount;
	uint64_t call;

	call = atomic_fetch_add(&image->reads, 1) + 1;
	if (call == image->fail_read || offset > INT64_MAX ||
	    length > (uint64_t)INT64_MAX - offset) {
		return BTRFS_IO;
	}
	while (done < length) {
		amount = pread(image->descriptor, (uint8_t *)buffer + done, length - done,
		    (off_t)(offset + done));
		if (amount < 0 && errno == EINTR) {
			continue;
		}
		if (amount <= 0) {
			return BTRFS_IO;
		}
		done += (size_t)amount;
	}
	atomic_fetch_add(&image->bytes_read, length);
	return BTRFS_OK;
}

static void *
image_allocate(void *context, size_t size)
{
	struct btrfs_image *image = context;
	void *allocation;
	uint64_t call;

	call = atomic_fetch_add(&image->allocations, 1) + 1;
	if (call == image->fail_allocate) {
		return NULL;
	}
	allocation = malloc(size);
	if (allocation != NULL) {
		atomic_fetch_add(&image->live_allocations, 1);
		atomic_fetch_add(&image->live_bytes, size);
	}
	return allocation;
}

static void
image_release(void *context, void *allocation, size_t size)
{
	struct btrfs_image *image = context;

	atomic_fetch_sub(&image->live_allocations, 1);
	atomic_fetch_sub(&image->live_bytes, size);
	free(allocation);
}

static enum btrfs_result
image_decompress(void *context, enum btrfs_compression codec, const void *input, size_t input_size,
    void *output, size_t output_size)
{
	z_stream stream;
	int result;
#ifdef BTRFS_HAVE_ZSTD
	size_t frame;
	size_t decoded;
#endif

	(void)context;
	if (codec == BTRFS_COMPRESSION_ZLIB) {
		if (input_size > UINT_MAX || output_size > UINT_MAX) {
			return BTRFS_RANGE;
		}
		memset(&stream, 0, sizeof(stream));
		stream.next_in = (Bytef *)input;
		stream.avail_in = (uInt)input_size;
		stream.next_out = output;
		stream.avail_out = (uInt)output_size;
		result = inflateInit(&stream);
		if (result != Z_OK) {
			return result == Z_MEM_ERROR ? BTRFS_NO_MEMORY : BTRFS_CORRUPT;
		}
		result = inflate(&stream, Z_FINISH);
		(void)inflateEnd(&stream);
		return result == Z_STREAM_END && stream.total_out == output_size
		    ? BTRFS_OK
		    : (result == Z_MEM_ERROR ? BTRFS_NO_MEMORY : BTRFS_CORRUPT);
	}
#ifdef BTRFS_HAVE_ZSTD
	if (codec == BTRFS_COMPRESSION_ZSTD) {
		frame = ZSTD_findFrameCompressedSize(input, input_size);
		if (ZSTD_isError(frame)) {
			return BTRFS_CORRUPT;
		}
		decoded = ZSTD_decompress(output, output_size, input, frame);
		return !ZSTD_isError(decoded) && decoded == output_size ? BTRFS_OK : BTRFS_CORRUPT;
	}
#endif
	return BTRFS_UNSUPPORTED;
}

int
btrfs_image_open(const char *path, struct btrfs_image *image)
{
	struct stat status;
	int error;

	memset(image, 0, sizeof(*image));
	image->descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (image->descriptor < 0) {
		return errno;
	}
	error = fstat(image->descriptor, &status) == 0 ? 0 : errno;
	if (error == 0 && (!S_ISREG(status.st_mode) || status.st_size < 0)) {
		error = EINVAL;
	}
	if (error != 0) {
		close(image->descriptor);
		image->descriptor = -1;
		return error;
	}
	image->environment.context = image;
	image->environment.size_bytes = (uint64_t)status.st_size;
	image->environment.read = image_read;
	image->environment.allocate = image_allocate;
	image->environment.release = image_release;
	image->environment.decompress = image_decompress;
	return 0;
}

void
btrfs_image_close(struct btrfs_image *image)
{
	if (image->descriptor >= 0) {
		close(image->descriptor);
		image->descriptor = -1;
	}
}

enum btrfs_result
btrfs_image_lookup(struct btrfs_fs *fs, const char *path, struct btrfs_inode *inode)
{
	struct btrfs_inode parent;
	const char *start;
	size_t length;
	enum btrfs_result error;

	error = btrfs_root(fs, inode);
	while (*path != '\0' && error == BTRFS_OK) {
		while (*path == '/') {
			path++;
		}
		start = path;
		while (*path != '\0' && *path != '/') {
			path++;
		}
		length = (size_t)(path - start);
		if (length == 0) {
			break;
		}
		parent = *inode;
		error = btrfs_lookup(fs, &parent, start, length, inode);
	}
	return error;
}
