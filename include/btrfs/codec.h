/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_CODEC_H
#define MACHLIN_BTRFS_CODEC_H

#include <btrfs/btrfs.h>

/* Decoders for the codecs of Btrfs extents that no platform library offers to
 * every adapter: LZO1X, for one segment of Btrfs's LZO format (the core parses
 * the segment framing), and Zstandard (RFC 8878). They are freestanding,
 * allocate nothing, use little stack and treat every input byte as untrusted:
 * each decodes one stream into at most capacity bytes, stores its decoded
 * length in *produced and returns BTRFS_OK, or returns BTRFS_CORRUPT (also for
 * a stream longer than capacity) and leaves the output unspecified. Bytes of
 * the output beyond *produced are unspecified too. */

/* The stream must end exactly at the end of input. */
enum btrfs_result btrfs_lzo1x_decompress(
    const void *input, size_t input_size, void *output, size_t capacity, size_t *produced);

/* The Zstandard decoder's tables: the caller provides this many bytes, aligned
 * for a pointer, and uses them for one call at a time. Their contents need no
 * initialization. */
#define BTRFS_ZSTD_WORKSPACE_BYTES (16U * 1024U)

/* Decodes the frame at the start of input, which must have no dictionary; a
 * stated content size must match what the blocks decode. Bytes after the
 * frame, the padding of a stored extent, are not examined. A content checksum
 * is verified. */
enum btrfs_result btrfs_zstd_decompress(void *workspace, const void *input, size_t input_size,
    void *output, size_t capacity, size_t *produced);

/* The Zstandard encoder's tables and match state, as for the decoder. */
#define BTRFS_ZSTD_COMPRESS_WORKSPACE_BYTES (192U * 1024U)
/* The largest input of one encoded frame: Linux's Btrfs window (128 KiB). */
#define BTRFS_ZSTD_COMPRESS_MAX_BYTES (128U * 1024U)

/* Encodes input (at most BTRFS_ZSTD_COMPRESS_MAX_BYTES) as one single-segment
 * frame of one compressed block, for kernels without a Zstandard library: its
 * literals are stored raw and its sequences use the predefined tables, so it
 * compresses less than libzstd does, and every Zstandard decoder reads it.
 * Returns BTRFS_RANGE when the frame would not be smaller than the input or
 * would not fit capacity; the output is then unspecified. */
enum btrfs_result btrfs_zstd_compress(void *workspace, const void *input, size_t input_size,
    void *output, size_t capacity, size_t *produced);

#endif
