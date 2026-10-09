/* SPDX-License-Identifier: BSD-3-Clause */
/* libFuzzer entry point for the adapters' LZO1X and Zstandard decoders and
 * encoders. The first input byte selects the codec (its top bit an encoder
 * round trip of the rest), the next two the output capacity;
 * the rest is the stream. With the reference libraries the decoders follow the
 * oracles of tests/codec.c: LZO1X as liblzo2 with Linux's stricter end
 * instruction, Zstandard within what libzstd accepts as one buffer and at
 * least what it decodes identically as one buffer and as a stream. */
#include <btrfs/codec.h>
#include <stdlib.h>
#include <string.h>
#ifdef BTRFS_HAVE_ZSTD
#include <zstd.h>
#endif
#ifdef BTRFS_HAVE_LZO
#include <lzo/lzo1x.h>
#endif

#define FUZZ_HEADER_BYTES 3U
#define REFERENCE_INPUT_PIECE 1024U
#define FUZZ_ENCODE 0x80U

static _Alignas(16) unsigned char workspace[BTRFS_ZSTD_WORKSPACE_BYTES];
static _Alignas(16) unsigned char encoder[BTRFS_ZSTD_COMPRESS_WORKSPACE_BYTES];
static _Alignas(16) unsigned char lzo_encoder[BTRFS_LZO1X_COMPRESS_WORKSPACE_BYTES];
static unsigned char output[1U << 16];
static unsigned char reference[1U << 16];

#ifdef BTRFS_HAVE_ZSTD
static unsigned char streamed[1U << 16];

static uint32_t
le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
	    (uint32_t)bytes[3] << 24;
}

static int
zstd_single(const uint8_t *frame, size_t frame_size, size_t capacity, size_t *decoded)
{
	size_t cut = ZSTD_findFrameCompressedSize(frame, frame_size);
	size_t result;

	if (ZSTD_isError(cut)) {
		return 0;
	}
	result = ZSTD_decompress(reference, capacity, frame, cut);
	if (ZSTD_isError(result)) {
		return 0;
	}
	*decoded = result;
	return 1;
}

static int
zstd_stream(const uint8_t *frame, size_t frame_size, size_t capacity, size_t *decoded)
{
	static ZSTD_DCtx *context;
	ZSTD_outBuffer out = { streamed, capacity, 0 };
	ZSTD_inBuffer in;
	size_t offset = 0;
	size_t result;

	if (context == NULL) {
		context = ZSTD_createDCtx();
		if (context == NULL) {
			abort();
		}
	}
	ZSTD_DCtx_reset(context, ZSTD_reset_session_and_parameters);
	while (offset < frame_size) {
		in.src = frame + offset;
		in.size = frame_size - offset < REFERENCE_INPUT_PIECE ? frame_size - offset
								      : REFERENCE_INPUT_PIECE;
		in.pos = 0;
		while (in.pos < in.size) {
			result = ZSTD_decompressStream(context, &out, &in);
			if (ZSTD_isError(result)) {
				return 0;
			}
			if (result == 0 || out.pos == out.size) {
				if (result != 0) {
					result = ZSTD_decompressStream(context, &out, &in);
				}
				*decoded = out.pos;
				return !ZSTD_isError(result) && result == 0;
			}
		}
		offset += in.size;
	}
	return 0;
}
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	size_t capacity;
	size_t produced = 0;
	size_t expected = 0;
	size_t decoded = 0;
	enum btrfs_result result;
	int accepted = -1;
#ifdef BTRFS_HAVE_ZSTD
	size_t streamed_size = 0;
	int single;
	int both;
#endif
#ifdef BTRFS_HAVE_LZO
	lzo_uint reference_size;
	int status;
#endif

	if (size < FUZZ_HEADER_BYTES) {
		return 0;
	}
	capacity = ((size_t)data[1] | (size_t)data[2] << 8) + 1U;
	/* Every LZO1X stream the encoder writes, into any capacity, decodes to
	 * its input with this decoder and with liblzo2 under Linux's end rule. */
	if ((data[0] & FUZZ_ENCODE) != 0 && (data[0] & 1U) == 0) {
		expected = size - FUZZ_HEADER_BYTES;
		if (expected > sizeof(reference)) {
			return 0;
		}
		capacity = capacity < sizeof(output) ? capacity : sizeof(output);
		result = btrfs_lzo1x_compress(
		    lzo_encoder, data + FUZZ_HEADER_BYTES, expected, output, capacity, &produced);
		if (result == BTRFS_RANGE) {
			return 0;
		}
		if (result != BTRFS_OK || produced > capacity ||
		    btrfs_lzo1x_decompress(
			output, produced, reference, sizeof(reference), &decoded) != BTRFS_OK ||
		    decoded != expected ||
		    memcmp(reference, data + FUZZ_HEADER_BYTES, expected) != 0) {
			abort();
		}
#ifdef BTRFS_HAVE_LZO
		reference_size = sizeof(reference);
		if (lzo1x_decompress_safe(output, produced, reference, &reference_size, NULL) !=
			LZO_E_OK ||
		    reference_size != expected ||
		    memcmp(reference, data + FUZZ_HEADER_BYTES, expected) != 0) {
			abort();
		}
#endif
		return 0;
	}
	/* Every frame the Zstandard encoder writes, into any capacity, decodes
	 * to its input with this decoder and with libzstd. */
	if ((data[0] & FUZZ_ENCODE) != 0) {
		expected = size - FUZZ_HEADER_BYTES;
		if (expected == 0 || expected > sizeof(reference)) {
			return 0;
		}
		capacity = capacity < sizeof(output) ? capacity : sizeof(output);
		result = btrfs_zstd_compress(
		    encoder, data + FUZZ_HEADER_BYTES, expected, output, capacity, &produced);
		if (result == BTRFS_RANGE) {
			return 0;
		}
		if (result != BTRFS_OK || produced > capacity || produced >= expected ||
		    btrfs_zstd_decompress(
			workspace, output, produced, reference, expected, &decoded) != BTRFS_OK ||
		    decoded != expected ||
		    memcmp(reference, data + FUZZ_HEADER_BYTES, expected) != 0) {
			abort();
		}
#ifdef BTRFS_HAVE_ZSTD
		if (ZSTD_decompress(reference, expected, output, produced) != expected ||
		    memcmp(reference, data + FUZZ_HEADER_BYTES, expected) != 0) {
			abort();
		}
#endif
		return 0;
	}
	if ((data[0] & 1U) != 0) {
		result = btrfs_zstd_decompress(workspace, data + FUZZ_HEADER_BYTES,
		    size - FUZZ_HEADER_BYTES, output, capacity, &produced);
#ifdef BTRFS_HAVE_ZSTD
		/* Accepted only what libzstd accepts as one buffer, with its bytes;
		 * required to accept what it decodes identically both ways. */
		single = zstd_single(
		    data + FUZZ_HEADER_BYTES, size - FUZZ_HEADER_BYTES, capacity, &expected);
		/* Legacy frame formats, which this libzstd may decode, are not
		 * Linux's: only current frames are required. */
		both = single && size - FUZZ_HEADER_BYTES >= 4 &&
		    le32(data + FUZZ_HEADER_BYTES) == ZSTD_MAGICNUMBER &&
		    zstd_stream(data + FUZZ_HEADER_BYTES, size - FUZZ_HEADER_BYTES, capacity,
			&streamed_size) &&
		    streamed_size == expected && memcmp(reference, streamed, expected) == 0;
		if ((result == BTRFS_OK &&
			(!single || produced != expected ||
			    memcmp(output, reference, produced) != 0)) ||
		    (result != BTRFS_OK && both)) {
			abort();
		}
#endif
	} else {
		result = btrfs_lzo1x_decompress(data + FUZZ_HEADER_BYTES, size - FUZZ_HEADER_BYTES,
		    output, capacity, &produced);
#ifdef BTRFS_HAVE_LZO
		reference_size = capacity;
		status = lzo1x_decompress_safe(data + FUZZ_HEADER_BYTES, size - FUZZ_HEADER_BYTES,
		    reference, &reference_size, NULL);
		accepted = status == LZO_E_OK && size >= FUZZ_HEADER_BYTES + 3U &&
		    data[size - 3] == 0x11 && data[size - 2] < 4 && data[size - 1] == 0;
		expected = reference_size;
		/* From its last bytes alone, an end instruction with a length
		 * extension (0x10, zeros, then a final 0x11) looks like the short
		 * one Linux accepts; leave that case undecided. */
		if (accepted && result != BTRFS_OK && size >= FUZZ_HEADER_BYTES + 4U &&
		    (data[size - 4] == 0 || data[size - 4] == 0x10)) {
			return 0;
		}
#endif
	}
	if (result != BTRFS_OK && result != BTRFS_CORRUPT) {
		abort();
	}
	if (result == BTRFS_OK && produced > capacity) {
		abort();
	}
	if (accepted >= 0 && (result == BTRFS_OK) != (accepted != 0)) {
		abort();
	}
	if (accepted > 0 && result == BTRFS_OK &&
	    (produced != expected || memcmp(output, reference, produced) != 0)) {
		abort();
	}
	return 0;
}
