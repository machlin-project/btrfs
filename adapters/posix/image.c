/* SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
/* F_FULLFSYNC */
#define _DARWIN_C_SOURCE
#endif
#include "image.h"
#include <btrfs/codec.h>
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
#include <zstd_errors.h>
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
    void *output, size_t capacity, size_t *produced)
{
	/* The Zstandard decoder's tables, one set per thread. */
	static _Thread_local _Alignas(16) uint8_t workspace[BTRFS_ZSTD_WORKSPACE_BYTES];
	z_stream stream;
	int result;

	(void)context;
	if (codec == BTRFS_COMPRESSION_LZO) {
		return btrfs_lzo1x_decompress(input, input_size, output, capacity, produced);
	}
	if (codec == BTRFS_COMPRESSION_ZSTD) {
		return btrfs_zstd_decompress(
		    workspace, input, input_size, output, capacity, produced);
	}
	if (codec == BTRFS_COMPRESSION_ZLIB) {
		if (input_size > UINT_MAX || capacity > UINT_MAX) {
			return BTRFS_RANGE;
		}
		memset(&stream, 0, sizeof(stream));
		stream.next_in = (Bytef *)input;
		stream.avail_in = (uInt)input_size;
		stream.next_out = output;
		stream.avail_out = (uInt)capacity;
		result = inflateInit(&stream);
		if (result != Z_OK) {
			return result == Z_MEM_ERROR ? BTRFS_NO_MEMORY : BTRFS_CORRUPT;
		}
		result = inflate(&stream, Z_FINISH);
		(void)inflateEnd(&stream);
		if (result != Z_STREAM_END) {
			return result == Z_MEM_ERROR ? BTRFS_NO_MEMORY : BTRFS_CORRUPT;
		}
		*produced = stream.total_out;
		return BTRFS_OK;
	}
	return BTRFS_UNSUPPORTED;
}

/* Linux's default levels: zlib 3 and zstd 3. */
#define BTRFS_IMAGE_ZLIB_LEVEL 3
#define BTRFS_IMAGE_ZSTD_LEVEL 3

enum btrfs_result
btrfs_image_compress(void *context, enum btrfs_compression codec, const void *input,
    size_t input_size, void *output, size_t capacity, size_t *size)
{
	z_stream stream;
	int result;
#ifdef BTRFS_HAVE_ZSTD
	size_t encoded;
#endif

	/* The shared LZO1X encoder's match table, one per thread. */
	static _Thread_local _Alignas(16) uint8_t workspace[BTRFS_LZO1X_COMPRESS_WORKSPACE_BYTES];

	(void)context;
	*size = 0;
	if (codec == BTRFS_COMPRESSION_LZO) {
		return btrfs_lzo1x_compress(workspace, input, input_size, output, capacity, size);
	}
	if (codec == BTRFS_COMPRESSION_ZLIB) {
		if (input_size > UINT_MAX || capacity > UINT_MAX) {
			return BTRFS_RANGE;
		}
		memset(&stream, 0, sizeof(stream));
		result = deflateInit(&stream, BTRFS_IMAGE_ZLIB_LEVEL);
		if (result != Z_OK) {
			return result == Z_MEM_ERROR ? BTRFS_NO_MEMORY : BTRFS_IO;
		}
		stream.next_in = (Bytef *)input;
		stream.avail_in = (uInt)input_size;
		stream.next_out = output;
		stream.avail_out = (uInt)capacity;
		result = deflate(&stream, Z_FINISH);
		*size = stream.total_out;
		(void)deflateEnd(&stream);
		return result == Z_STREAM_END ? BTRFS_OK : BTRFS_RANGE;
	}
#ifdef BTRFS_HAVE_ZSTD
	if (codec == BTRFS_COMPRESSION_ZSTD) {
		encoded =
		    ZSTD_compress(output, capacity, input, input_size, BTRFS_IMAGE_ZSTD_LEVEL);
		if (ZSTD_isError(encoded)) {
			return ZSTD_getErrorCode(encoded) == ZSTD_error_dstSize_tooSmall
			    ? BTRFS_RANGE
			    : BTRFS_IO;
		}
		*size = encoded;
		return BTRFS_OK;
	}
#endif
	return BTRFS_UNSUPPORTED;
}

static enum btrfs_result
image_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct btrfs_image *image = context;
	size_t done = 0;
	ssize_t amount;

	if (offset > INT64_MAX || length > (uint64_t)INT64_MAX - offset) {
		return BTRFS_IO;
	}
	while (done < length) {
		amount = pwrite(image->descriptor, (const uint8_t *)buffer + done, length - done,
		    (off_t)(offset + done));
		if (amount < 0 && errno == EINTR) {
			continue;
		}
		if (amount <= 0) {
			return BTRFS_IO;
		}
		done += (size_t)amount;
	}
	return BTRFS_OK;
}

/* A barrier reaches stable storage: F_FULLFSYNC where the platform has it,
 * since fsync alone may leave data in the drive's cache. */
static enum btrfs_result
image_flush(void *context)
{
	struct btrfs_image *image = context;

#ifdef F_FULLFSYNC
	if (fcntl(image->descriptor, F_FULLFSYNC) == 0) {
		return BTRFS_OK;
	}
#endif
	return fsync(image->descriptor) == 0 ? BTRFS_OK : BTRFS_IO;
}

static int
image_open(const char *path, int flags, struct btrfs_image *image)
{
	struct stat status;
	int error;

	memset(image, 0, sizeof(*image));
	image->descriptor = open(path, flags | O_CLOEXEC);
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

int
btrfs_image_open(const char *path, struct btrfs_image *image)
{
	return image_open(path, O_RDONLY, image);
}

int
btrfs_image_open_writable(const char *path, struct btrfs_image *image)
{
	return image_open(path, O_RDWR, image);
}

void
btrfs_image_writer(struct btrfs_image *image, struct btrfs_write_environment *writer)
{
	memset(writer, 0, sizeof(*writer));
	writer->context = image;
	writer->write = image_write;
	writer->flush = image_flush;
	writer->compress = btrfs_image_compress;
	writer->compression = BTRFS_COMPRESSION_NONE;
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
