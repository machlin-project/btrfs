/* SPDX-License-Identifier: BSD-3-Clause */
/* The adapters' LZO1X and Zstandard decoders against the reference libraries
 * (liblzo2 and libzstd) when the build has them: frames from many compression
 * settings must decode to the same bytes, and mutated frames must never be
 * accepted unless the reference accepts them with the same bytes. The kernel's
 * Zstandard encoder must write frames that both decoders return exactly. */
#include <btrfs/codec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef BTRFS_HAVE_ZSTD
/* Experimental parameters: literal compression modes. */
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#endif
#ifdef BTRFS_HAVE_LZO
#include <lzo/lzo1x.h>
#endif

#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

/* Larger than the 128 KiB Btrfs extents: several blocks per frame. */
#define SAMPLE_MAX (512U * 1024U)
#define MUTATIONS_PER_FRAME 24U
#define LZO_SEGMENT_BYTES 4096U

static uint64_t random_state = 0x853c49e6748fea9bULL;

static uint32_t
next_random(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;
	return (uint32_t)(random_state >> 16);
}

enum pattern {
	PATTERN_ZERO,
	PATTERN_RANDOM,
	PATTERN_TEXT,
	PATTERN_STRIDE,
	PATTERN_MIXED,
	PATTERN_COUNT
};

/* Sample data with different statistics: runs, noise, a small vocabulary,
 * structured records with repeated offsets, and a mixture of all. */
static void
fill(uint8_t *data, size_t size, enum pattern pattern)
{
	static const char *const words[] = { "btrfs ", "extent ", "subvolume ", "checksum ",
		"inode ", "the ", "of ", "a ", "\n", "chunk-tree ", "0123456789 " };
	size_t i = 0;
	size_t length;
	const char *word;

	switch (pattern) {
	case PATTERN_ZERO:
		memset(data, 0, size);
		return;
	case PATTERN_RANDOM:
		for (i = 0; i < size; i++) {
			data[i] = (uint8_t)next_random();
		}
		return;
	case PATTERN_TEXT:
		while (i < size) {
			word = words[next_random() % (sizeof(words) / sizeof(words[0]))];
			length = strlen(word);
			if (length > size - i) {
				length = size - i;
			}
			memcpy(data + i, word, length);
			i += length;
		}
		return;
	case PATTERN_STRIDE:
		for (i = 0; i < size; i++) {
			data[i] = (uint8_t)((i % 24U) < 8U ? i / 24U : (i % 24U) * 7U);
		}
		return;
	default:
		while (i < size) {
			length = 1U + next_random() % 4096U;
			if (length > size - i) {
				length = size - i;
			}
			fill(data + i, length, (enum pattern)(next_random() % PATTERN_MIXED));
			i += length;
		}
		return;
	}
}

static size_t
sample_size(unsigned round)
{
	static const size_t edges[] = { 1, 2, 3, 7, 8, 9, 31, 32, 33, 255, 256, 4095, 4096, 4097,
		65535, 65536, 131071, 131072, 131073, 262144, SAMPLE_MAX };

	if (round < sizeof(edges) / sizeof(edges[0])) {
		return edges[round];
	}
	return 1U + next_random() % (round % 3U == 0 ? SAMPLE_MAX : 8192U);
}

#ifdef BTRFS_HAVE_ZSTD
static unsigned zstd_frames;
static unsigned zstd_mutations;
static unsigned zstd_mutations_accepted;

static void
dump(const char *label, const uint8_t *bytes, size_t size)
{
	size_t i;

	fprintf(stderr, "%s:", label);
	for (i = 0; i < size && i < 24; i++) {
		fprintf(stderr, " %02x", bytes[i]);
	}
	fprintf(stderr, "\n");
}

/* libzstd decoding a frame as one buffer, cut at the frame's end as a stored
 * extent pads after it: whether it decodes within capacity, and its length. */
static int
zstd_single(
    const uint8_t *frame, size_t frame_size, uint8_t *output, size_t capacity, size_t *decoded)
{
	size_t cut = ZSTD_findFrameCompressedSize(frame, frame_size);
	size_t result;

	if (ZSTD_isError(cut)) {
		return 0;
	}
	result = ZSTD_decompress(output, capacity, frame, cut);
	if (ZSTD_isError(result)) {
		return 0;
	}
	*decoded = result;
	return 1;
}

/* libzstd's streaming decoder, fed as Linux feeds a stored extent: a piece at
 * a time, so it never shortcuts to its single-buffer decoder. */
#define REFERENCE_INPUT_PIECE 1024U

static int
zstd_stream(
    const uint8_t *frame, size_t frame_size, uint8_t *output, size_t capacity, size_t *decoded)
{
	ZSTD_DCtx *context = ZSTD_createDCtx();
	ZSTD_outBuffer out = { output, capacity, 0 };
	ZSTD_inBuffer in;
	size_t offset = 0;
	size_t result;
	int done = 0;

	REQUIRE(context != NULL);
	while (!done && offset < frame_size) {
		in.src = frame + offset;
		in.size = frame_size - offset < REFERENCE_INPUT_PIECE ? frame_size - offset
								      : REFERENCE_INPUT_PIECE;
		in.pos = 0;
		while (in.pos < in.size) {
			result = ZSTD_decompressStream(context, &out, &in);
			if (ZSTD_isError(result)) {
				ZSTD_freeDCtx(context);
				return 0;
			}
			if (result == 0) {
				done = 1;
				break;
			}
			if (out.pos == out.size) {
				result = ZSTD_decompressStream(context, &out, &in);
				done = !ZSTD_isError(result) && result == 0;
				in.pos = in.size;
				offset = frame_size;
				break;
			}
		}
		offset += in.size;
	}
	ZSTD_freeDCtx(context);
	*decoded = out.pos;
	return done;
}

/* Mutated frames. The decoder must accept, with the same bytes, a frame that
 * libzstd decodes identically both ways, and may accept only what libzstd's
 * single-buffer decoder accepts, with its bytes. */
static void
zstd_mutate(void *workspace, const uint8_t *frame, size_t frame_size, size_t content_size,
    uint8_t *output, uint8_t *reference, uint8_t *streamed)
{
	uint8_t *copy = malloc(frame_size + 1);
	size_t size;
	size_t produced = 0;
	size_t single_size = 0;
	size_t stream_size = 0;
	unsigned i;
	unsigned kind;
	enum btrfs_result result;
	int single;
	int both;

	REQUIRE(copy != NULL);
	for (i = 0; i < MUTATIONS_PER_FRAME; i++) {
		memcpy(copy, frame, frame_size);
		size = frame_size;
		kind = next_random() % 4U;
		if (kind == 0) {
			copy[next_random() % size] ^= (uint8_t)(1U << (next_random() % 8U));
		} else if (kind == 1) {
			copy[next_random() % size] = (uint8_t)next_random();
		} else if (kind == 2) {
			size = next_random() % size;
		} else {
			copy[4U + next_random() % (size > 20 ? 16U : size - 4U)] =
			    (uint8_t)next_random();
		}
		result =
		    btrfs_zstd_decompress(workspace, copy, size, output, content_size, &produced);
		single = zstd_single(copy, size, reference, content_size, &single_size);
		/* Legacy frame formats, which this libzstd may decode, are not
		 * Linux's: only current frames are required. */
		both = single && size >= 4 &&
		    ((uint32_t)copy[0] | (uint32_t)copy[1] << 8 | (uint32_t)copy[2] << 16 |
			(uint32_t)copy[3] << 24) == ZSTD_MAGICNUMBER &&
		    zstd_stream(copy, size, streamed, content_size, &stream_size) &&
		    stream_size == single_size && memcmp(reference, streamed, single_size) == 0;
		zstd_mutations++;
		REQUIRE(result == BTRFS_OK || result == BTRFS_CORRUPT);
		if ((result == BTRFS_OK &&
			(!single || produced != single_size ||
			    memcmp(output, reference, produced) != 0)) ||
		    (result != BTRFS_OK && both)) {
			fprintf(stderr,
			    "zstd: mutation (kind %u, size %zu) decoded %d, libzstd %d/%d\n", kind,
			    size, result == BTRFS_OK, single, both);
			dump("original", frame, frame_size);
			dump("mutated", copy, size);
			exit(1);
		}
		if (result == BTRFS_OK) {
			zstd_mutations_accepted++;
		}
	}
	free(copy);
}

static void
zstd_round(void *workspace, const uint8_t *data, size_t size, int level, unsigned variant,
    uint8_t *frame, size_t capacity, uint8_t *output, uint8_t *reference, uint8_t *streamed)
{
	ZSTD_CCtx *context = ZSTD_createCCtx();
	size_t frame_size;
	size_t produced = 0;

	REQUIRE(context != NULL);
	REQUIRE(!ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level)));
	REQUIRE(!ZSTD_isError(
	    ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, (variant & 1U) != 0)));
	REQUIRE(!ZSTD_isError(
	    ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, (variant & 2U) == 0)));
	if ((variant & 4U) != 0) {
		/* Small windows: frames that are not single segments, short blocks. */
		REQUIRE(!ZSTD_isError(ZSTD_CCtx_setParameter(
		    context, ZSTD_c_windowLog, 10 + (int)(next_random() % 8U))));
	}
	if ((variant & 8U) != 0) {
		REQUIRE(!ZSTD_isError(ZSTD_CCtx_setParameter(
		    context, ZSTD_c_targetCBlockSize, 1340 + (int)(next_random() % 4096U))));
	}
	if ((variant & 16U) != 0) {
		REQUIRE(!ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_literalCompressionMode,
		    next_random() % 2U ? ZSTD_ps_enable : ZSTD_ps_disable)));
	}
	frame_size = ZSTD_compress2(context, frame, capacity, data, size);
	REQUIRE(!ZSTD_isError(frame_size));
	ZSTD_freeCCtx(context);
	/* Btrfs pads a stored extent with zeros after the frame. */
	memset(frame + frame_size, 0, 16);
	memset(output, 0xA5, size);
	REQUIRE(btrfs_zstd_decompress(workspace, frame, frame_size + 16, output, size, &produced) ==
	    BTRFS_OK);
	REQUIRE(produced == size && memcmp(output, data, size) == 0);
	/* Room to spare is unused; too little room is refused. */
	REQUIRE(btrfs_zstd_decompress(workspace, frame, frame_size, output, size + 64, &produced) ==
	    BTRFS_OK);
	REQUIRE(produced == size && memcmp(output, data, size) == 0);
	REQUIRE(btrfs_zstd_decompress(workspace, frame, frame_size, output, size - 1, &produced) ==
	    BTRFS_CORRUPT);
	zstd_frames++;
	if (zstd_frames % 4U == 0) {
		zstd_mutate(workspace, frame, frame_size, size, output, reference, streamed);
	}
}

static void
zstd_tests(void *workspace)
{
	static const int levels[] = { -5, -1, 1, 3, 6, 9, 15, 19 };
	size_t capacity = ZSTD_compressBound(SAMPLE_MAX) + 16;
	uint8_t *data = malloc(SAMPLE_MAX);
	uint8_t *frame = malloc(capacity);
	uint8_t *output = malloc(SAMPLE_MAX + 64);
	uint8_t *reference = malloc(SAMPLE_MAX);
	uint8_t *streamed = malloc(SAMPLE_MAX);
	unsigned round;
	size_t size;

	REQUIRE(data != NULL && frame != NULL && output != NULL && reference != NULL &&
	    streamed != NULL);
	for (round = 0; round < 400; round++) {
		size = sample_size(round);
		fill(data, size, (enum pattern)(round % PATTERN_COUNT));
		zstd_round(workspace, data, size, levels[round % 8U], next_random() % 32U, frame,
		    capacity, output, reference, streamed);
	}
	printf("zstd: %u frames decoded as libzstd encoded them, %u mutations (%u accepted, "
	       "all as libzstd decodes them) PASS\n",
	    zstd_frames, zstd_mutations, zstd_mutations_accepted);
	free(data);
	free(frame);
	free(output);
	free(reference);
	free(streamed);
}
#endif

#ifdef BTRFS_HAVE_LZO
/* LZO1X instruction bytes (Linux's Documentation/staging/lzo.rst). */
#define LZO_FIRST_RUN_MIN 18U
#define LZO_M4_MIN 16U
#define LZO_M3_MIN 32U
#define LZO_M2_MIN 64U
#define LZO_LONG_RUN 4U
#define LZO_END_INSTRUCTION 0x11U

/* Whether a stream ends as Linux's lzo1x_decompress_safe requires: walking
 * instruction boundaries only, the end-of-stream marker (an M4 match at
 * distance 16384) must be the unextended instruction 0x11 and end the input. */
static int
lzo_linux_end(const uint8_t *stream, size_t size)
{
	size_t at = 0;
	size_t start;
	unsigned state = 0;
	unsigned byte;
	unsigned distance;

	if (size == 0) {
		return 0;
	}
	if (stream[0] >= LZO_FIRST_RUN_MIN) {
		at = 1U + (stream[0] - 17U);
		state = stream[0] - 17U < LZO_LONG_RUN ? stream[0] - 17U : LZO_LONG_RUN;
	}
	while (at < size) {
		start = at;
		byte = stream[at++];
		if (byte < LZO_M4_MIN && state == 0) {
			/* A literal run, its length extended by zero bytes. */
			size_t length = byte;

			if (length == 0) {
				while (at < size && stream[at] == 0) {
					at++;
					length += 255U;
				}
				if (at == size) {
					return 0;
				}
				length += 15U + stream[at++];
			}
			at += length + 3U;
			state = LZO_LONG_RUN;
			continue;
		}
		if (byte < LZO_M4_MIN || byte >= LZO_M2_MIN) {
			/* M1 and M2: one distance byte; the literals that follow
			 * are counted in the instruction. */
			at += 1U;
			state = byte & 3U;
		} else {
			/* M4 and M3: a length extended by zero bytes, then a
			 * 16-bit distance whose low bits count the literals. */
			if ((byte & (byte < LZO_M3_MIN ? 7U : 31U)) == 0) {
				while (at < size && stream[at] == 0) {
					at++;
				}
				at++;
			}
			if (at + 2U > size) {
				return 0;
			}
			distance = (unsigned)stream[at] | (unsigned)stream[at + 1] << 8;
			at += 2U;
			if (byte < LZO_M3_MIN && (byte & 8U) == 0 && (distance >> 2) == 0) {
				return byte == LZO_END_INSTRUCTION && start + 3U == size &&
				    at == size;
			}
			state = distance & 3U;
		}
		at += state;
	}
	return 0;
}

static void
lzo_tests(void)
{
	uint8_t *work = malloc(LZO1X_999_MEM_COMPRESS);
	uint8_t data[LZO_SEGMENT_BYTES];
	uint8_t stream[LZO_SEGMENT_BYTES + LZO_SEGMENT_BYTES / 16 + 64 + 3];
	uint8_t copy[sizeof(stream)];
	uint8_t output[LZO_SEGMENT_BYTES + 8];
	uint8_t reference[LZO_SEGMENT_BYTES];
	lzo_uint stream_size;
	lzo_uint reference_size;
	unsigned round;
	unsigned i;
	unsigned accepted = 0;
	unsigned mutations = 0;
	size_t size;
	size_t cut;
	size_t produced = 0;
	enum btrfs_result result;
	int status;
	int linux_accepts;

	REQUIRE(work != NULL && lzo_init() == LZO_E_OK);
	for (round = 0; round < 3000; round++) {
		size = 1U + next_random() % LZO_SEGMENT_BYTES;
		if (round < 20) {
			size = round < 10 ? round + 1U : LZO_SEGMENT_BYTES - (round - 10U);
		}
		fill(data, size, (enum pattern)(round % PATTERN_COUNT));
		status = round % 2U ? lzo1x_999_compress(data, size, stream, &stream_size, work)
				    : lzo1x_1_compress(data, size, stream, &stream_size, work);
		REQUIRE(status == LZO_E_OK);
		memset(output, 0xA5, sizeof(output));
		REQUIRE(btrfs_lzo1x_decompress(stream, stream_size, output, size, &produced) ==
		    BTRFS_OK);
		REQUIRE(produced == size && memcmp(output, data, size) == 0);
		/* The stream must end the input and fit the output; room to spare is
		 * unused. */
		REQUIRE(btrfs_lzo1x_decompress(stream, stream_size - 1, output, size, &produced) ==
		    BTRFS_CORRUPT);
		REQUIRE(btrfs_lzo1x_decompress(stream, stream_size, output, size - 1, &produced) ==
		    BTRFS_CORRUPT);
		REQUIRE(btrfs_lzo1x_decompress(stream, stream_size, output, size + 8, &produced) ==
		    BTRFS_OK);
		REQUIRE(produced == size && memcmp(output, data, size) == 0);
		for (i = 0; i < 8; i++) {
			memcpy(copy, stream, stream_size);
			cut = stream_size;
			if (i % 3U == 2U) {
				cut = next_random() % stream_size;
			} else {
				copy[next_random() % stream_size] ^=
				    (uint8_t)(1U << (next_random() % 8U));
			}
			result = btrfs_lzo1x_decompress(copy, cut, output, size, &produced);
			reference_size = size;
			status = lzo1x_decompress_safe(copy, cut, reference, &reference_size, NULL);
			/* Linux, which Btrfs reads LZO with, also requires the end
			 * instruction to be the unextended 0x11 at the end. */
			linux_accepts = status == LZO_E_OK && lzo_linux_end(copy, cut);
			mutations++;
			if (result == BTRFS_OK) {
				accepted++;
				REQUIRE(linux_accepts && produced == reference_size &&
				    memcmp(output, reference, produced) == 0);
			} else {
				REQUIRE(result == BTRFS_CORRUPT);
				REQUIRE(!linux_accepts);
			}
		}
	}
	printf("lzo1x: 3000 streams of lzo1x_1 and lzo1x_999 decoded, %u mutations (%u "
	       "accepted, all as liblzo2 with Linux's end check) PASS\n",
	    mutations, accepted);
	free(work);
}
#endif

/* The LZO1X encoder: segments of every sector size Btrfs uses, of each data
 * pattern and of crafted repeats whose distances need M2, M3 and M4 matches
 * and length extensions, decode to their input with this decoder and, when
 * the build has it, with liblzo2 under Linux's end check; a stream never
 * starts with 17, which Linux's decoder takes for an LZO-RLE version, and one
 * byte less of output space is refused. */
#define LZO_ENCODER_MAX_INPUT 65536U
#define LZO_ENCODER_ROUNDS 600U
#define LZO_WORST(size) ((size) + (size) / 16U + 64U + 3U)
#define LZO_FAR_REPEAT 40000U
#define LZO_MIDDLE_REPEAT 12000U
#define LZO_NEAR_REPEAT 700U
#define LZO_REPEAT_BYTES 300U
/* A first byte Linux's decoder reads as LZO-RLE's marker. */
#define LZO_RLE_FIRST_BYTE 17U

static void
lzo_encoder_tests(void)
{
	static const size_t sectors[] = { 4096, 16384, 65536 };
	static uint8_t data[LZO_ENCODER_MAX_INPUT];
	static uint8_t stream[LZO_WORST(LZO_ENCODER_MAX_INPUT)];
	static uint8_t output[LZO_ENCODER_MAX_INPUT];
	void *work = malloc(BTRFS_LZO1X_COMPRESS_WORKSPACE_BYTES);
	size_t size;
	size_t produced;
	size_t decoded;
	unsigned round;
	unsigned smaller = 0;
#ifdef BTRFS_HAVE_LZO
	lzo_uint reference_size;
#endif

	REQUIRE(work != NULL);
	for (round = 0; round < LZO_ENCODER_ROUNDS; round++) {
		size = round < 20 ? round : 1U + next_random() % sectors[round % 3U];
		if (round % 50U == 20U) {
			size = sectors[(round / 50U) % 3U];
		}
		fill(data, size, (enum pattern)(round % PATTERN_COUNT));
		if (round % 7U == 3U && size > LZO_FAR_REPEAT + LZO_REPEAT_BYTES) {
			/* Repeats far enough apart for M4, M3 and M2 matches. */
			memcpy(data + LZO_FAR_REPEAT, data, LZO_REPEAT_BYTES);
			memcpy(data + LZO_MIDDLE_REPEAT, data + 1, LZO_REPEAT_BYTES);
			memcpy(data + LZO_NEAR_REPEAT, data + 2, LZO_REPEAT_BYTES);
		}
		REQUIRE(btrfs_lzo1x_compress(work, data, size, stream, sizeof(stream), &produced) ==
		    BTRFS_OK);
		REQUIRE(produced <= LZO_WORST(size));
		REQUIRE(produced < 5U || stream[0] != LZO_RLE_FIRST_BYTE);
		REQUIRE(btrfs_lzo1x_decompress(
			    stream, produced, output, sizeof(output), &decoded) == BTRFS_OK);
		REQUIRE(decoded == size && memcmp(output, data, size) == 0);
#ifdef BTRFS_HAVE_LZO
		reference_size = sizeof(output);
		REQUIRE(lzo1x_decompress_safe(stream, produced, output, &reference_size, NULL) ==
		    LZO_E_OK);
		REQUIRE(reference_size == size && memcmp(output, data, size) == 0);
		REQUIRE(lzo_linux_end(stream, produced));
#endif
		REQUIRE(btrfs_lzo1x_compress(work, data, size, stream, produced - 1U, &decoded) ==
		    BTRFS_RANGE);
		smaller += produced < size;
	}
	free(work);
	printf("lzo1x encoder: %u segments decoded by this decoder%s, %u smaller than their "
	       "input PASS\n",
	    LZO_ENCODER_ROUNDS,
#ifdef BTRFS_HAVE_LZO
	    " and liblzo2 with Linux's end check",
#else
	    "",
#endif
	    smaller);
}

/* Streams written by hand, for builds without the reference libraries. */
#define ENCODER_ROUNDS 900U

/* Every frame the encoder writes is smaller than its input and decodes, with
 * this decoder and with libzstd, to exactly that input; runs and text always
 * compress. */
static void
zstd_encoder_tests(void *workspace)
{
	void *encoder = malloc(BTRFS_ZSTD_COMPRESS_WORKSPACE_BYTES);
	uint8_t *data = malloc(BTRFS_ZSTD_COMPRESS_MAX_BYTES);
	uint8_t *frame = malloc(BTRFS_ZSTD_COMPRESS_MAX_BYTES);
	uint8_t *back = malloc(BTRFS_ZSTD_COMPRESS_MAX_BYTES);
	uint64_t input_bytes = 0;
	uint64_t frame_bytes = 0;
	size_t size;
	size_t frame_size;
	size_t produced;
	unsigned round;
	unsigned written = 0;
	unsigned declined = 0;
	enum pattern pattern;
	enum btrfs_result result;

	REQUIRE(encoder != NULL && data != NULL && frame != NULL && back != NULL);
	for (round = 0; round < ENCODER_ROUNDS; round++) {
		size = sample_size(round);
		size = size > BTRFS_ZSTD_COMPRESS_MAX_BYTES ? BTRFS_ZSTD_COMPRESS_MAX_BYTES : size;
		pattern = (enum pattern)(round % PATTERN_COUNT);
		fill(data, size, pattern);
		result = btrfs_zstd_compress(encoder, data, size, frame, size, &frame_size);
		if (result == BTRFS_RANGE) {
			/* Short inputs may not pay for a frame; noise never does. */
			REQUIRE((pattern != PATTERN_ZERO && pattern != PATTERN_TEXT) || size < 64U);
			declined++;
			continue;
		}
		REQUIRE(result == BTRFS_OK && frame_size < size);
		memset(back, 0xa5, size);
		REQUIRE(btrfs_zstd_decompress(
			    workspace, frame, frame_size, back, size, &produced) == BTRFS_OK &&
		    produced == size && memcmp(back, data, size) == 0);
#ifdef BTRFS_HAVE_ZSTD
		memset(back, 0x5a, size);
		REQUIRE(ZSTD_getFrameContentSize(frame, frame_size) == size);
		REQUIRE(ZSTD_decompress(back, size, frame, frame_size) == size &&
		    memcmp(back, data, size) == 0);
#endif
		/* A smaller capacity than the frame is refused, never overrun. */
		REQUIRE(btrfs_zstd_compress(
			    encoder, data, size, frame, frame_size - 1U, &produced) == BTRFS_RANGE);
		input_bytes += size;
		frame_bytes += frame_size;
		written++;
	}
	printf("zstd encoder: %u frames decode exactly with this decoder%s, %u inputs declined; "
	       "%llu bytes in %llu PASS\n",
	    written,
#ifdef BTRFS_HAVE_ZSTD
	    " and libzstd",
#else
	    "",
#endif
	    declined, (unsigned long long)input_bytes, (unsigned long long)frame_bytes);
	free(encoder);
	free(data);
	free(frame);
	free(back);
}

static void
fixed_tests(void *workspace)
{
	/* Four literals, then the end instruction. */
	static const uint8_t lzo_literals[] = { 21, 'a', 'b', 'c', 'd', 0x11, 0, 0 };
	/* One literal, then a match of 7 bytes at distance 1, then the end. */
	static const uint8_t lzo_match[] = { 18, 'z', 0x20 | 5, 0x00, 0x00, 0x11, 0, 0 };
	/* A single-segment frame of one raw block "hello", content size 5. */
	static const uint8_t zstd_raw[] = { 0x28, 0xB5, 0x2F, 0xFD, 0x20, 5, 0x29, 0, 0, 'h', 'e',
		'l', 'l', 'o' };
	/* The same with an RLE block of six 'x'. */
	static const uint8_t zstd_rle[] = { 0x28, 0xB5, 0x2F, 0xFD, 0x20, 6, 0x33, 0, 0, 'x' };
	uint8_t output[16];
	size_t produced = 0;

	REQUIRE(btrfs_lzo1x_decompress(lzo_literals, sizeof(lzo_literals), output, 16, &produced) ==
	    BTRFS_OK);
	REQUIRE(produced == 4 && memcmp(output, "abcd", 4) == 0);
	REQUIRE(
	    btrfs_lzo1x_decompress(lzo_match, sizeof(lzo_match), output, 8, &produced) == BTRFS_OK);
	REQUIRE(produced == 8 && memcmp(output, "zzzzzzzz", 8) == 0);
	REQUIRE(btrfs_lzo1x_decompress(lzo_match, sizeof(lzo_match) - 1, output, 8, &produced) ==
	    BTRFS_CORRUPT);
	REQUIRE(btrfs_lzo1x_decompress(lzo_match, sizeof(lzo_match), output, 7, &produced) ==
	    BTRFS_CORRUPT);
	REQUIRE(btrfs_zstd_decompress(
		    workspace, zstd_raw, sizeof(zstd_raw), output, 16, &produced) == BTRFS_OK);
	REQUIRE(produced == 5 && memcmp(output, "hello", 5) == 0);
	REQUIRE(btrfs_zstd_decompress(
		    workspace, zstd_rle, sizeof(zstd_rle), output, 6, &produced) == BTRFS_OK);
	REQUIRE(produced == 6 && memcmp(output, "xxxxxx", 6) == 0);
	/* The stated content size must fit. */
	REQUIRE(btrfs_zstd_decompress(
		    workspace, zstd_rle, sizeof(zstd_rle), output, 5, &produced) == BTRFS_CORRUPT);
	printf("codec fixed streams PASS\n");
}

int
main(void)
{
	void *workspace = malloc(BTRFS_ZSTD_WORKSPACE_BYTES);

	REQUIRE(workspace != NULL);
	fixed_tests(workspace);
	zstd_encoder_tests(workspace);
	lzo_encoder_tests();
#ifdef BTRFS_HAVE_ZSTD
	zstd_tests(workspace);
#else
	printf("SKIP zstd differential tests: built without libzstd\n");
#endif
#ifdef BTRFS_HAVE_LZO
	lzo_tests();
#else
	printf("SKIP lzo1x differential tests: built without liblzo2\n");
#endif
	free(workspace);
	return 0;
}
