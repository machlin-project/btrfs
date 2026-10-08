/* SPDX-License-Identifier: BSD-3-Clause */
#include <btrfs/codec.h>

static uint16_t
le16(const uint8_t *bytes)
{
	return (uint16_t)(bytes[0] | (unsigned)bytes[1] << 8);
}

static uint32_t
le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
	    (uint32_t)bytes[3] << 24;
}

static uint64_t
le64(const uint8_t *bytes)
{
	return (uint64_t)le32(bytes) | (uint64_t)le32(bytes + 4) << 32;
}

/* The index of the highest set bit of a nonzero value. */
static unsigned
highbit(uint32_t value)
{
	return 31U - (unsigned)__builtin_clz(value);
}

/* A copy that may overlap when source starts after destination. */
static void
copy_forward(uint8_t *destination, const uint8_t *source, size_t length)
{
	__builtin_memmove(destination, source, length);
}

/* A match: source may overlap the bytes it produces (offset < length). */
static void
copy_match(uint8_t *destination, size_t offset, size_t length)
{
	const uint8_t *source = destination - offset;
	size_t i;

	if (offset >= length) {
		__builtin_memcpy(destination, source, length);
		return;
	}
	for (i = 0; i < length; i++) {
		destination[i] = source[i];
	}
}

/* LZO1X, as the LZO library and Linux write it (lzo1x_1 and lzo1x_999). An
 * instruction's two low bits, or the low bits of its distance field, give the
 * literals (0-3) that follow a match; a run of four or more literals is an
 * instruction of its own. The stream ends with the three-byte instruction
 * 0x11 0x00 0x00, which must end the input as the output must be full. */
#define LZO_MATCH_1K_DISTANCE 1U
#define LZO_MATCH_3K_DISTANCE 2049U
#define LZO_MATCH_16K_DISTANCE 16384U
#define LZO_FIRST_LITERALS 17U
#define LZO_END_LENGTH 3U
#define LZO_RUN_STATE 4U

/* Adds a length extension: each zero byte counts 255, then a nonzero byte. */
static int
lzo_extend(const uint8_t **input, const uint8_t *end, size_t base, size_t *length)
{
	const uint8_t *in = *input;
	size_t zeros = 0;

	while (in < end && *in == 0) {
		zeros++;
		in++;
	}
	if (in == end) {
		return 0;
	}
	*length = base + zeros * 255U + *in++;
	*input = in;
	return 1;
}

static int
lzo_literals(const uint8_t **input, const uint8_t *in_end, uint8_t **output, const uint8_t *out_end,
    size_t count)
{
	if (count > (size_t)(in_end - *input) || count > (size_t)(out_end - *output)) {
		return 0;
	}
	__builtin_memcpy(*output, *input, count);
	*input += count;
	*output += count;
	return 1;
}

enum btrfs_result
btrfs_lzo1x_decompress(
    const void *input, size_t input_size, void *output, size_t capacity, size_t *produced)
{
	const uint8_t *in = input;
	const uint8_t *in_end = in + input_size;
	uint8_t *out = output;
	uint8_t *out_start = out;
	uint8_t *out_end = out + capacity;
	size_t state = 0;
	size_t length;
	size_t distance;
	size_t next;
	unsigned instruction;

	if (input == NULL || output == NULL || produced == NULL || input_size < LZO_END_LENGTH) {
		return BTRFS_CORRUPT;
	}
	if (*in > LZO_FIRST_LITERALS) {
		length = (size_t)(*in++ - LZO_FIRST_LITERALS);
		if (!lzo_literals(&in, in_end, &out, out_end, length)) {
			return BTRFS_CORRUPT;
		}
		state = length < LZO_RUN_STATE ? length : LZO_RUN_STATE;
	}
	for (;;) {
		if (in == in_end) {
			return BTRFS_CORRUPT;
		}
		instruction = *in++;
		if (instruction < 16U) {
			if (state == 0) {
				/* A run of four or more literals. */
				length = instruction;
				if (length == 0 && !lzo_extend(&in, in_end, 15U, &length)) {
					return BTRFS_CORRUPT;
				}
				if (!lzo_literals(&in, in_end, &out, out_end, length + 3U)) {
					return BTRFS_CORRUPT;
				}
				state = LZO_RUN_STATE;
				continue;
			}
			if (in == in_end) {
				return BTRFS_CORRUPT;
			}
			/* After one to three literals: two bytes within 1 KiB; after a
			 * literal run: three bytes at 2-3 KiB. */
			distance = (instruction >> 2) + ((size_t)*in++ << 2) +
			    (state < LZO_RUN_STATE ? LZO_MATCH_1K_DISTANCE : LZO_MATCH_3K_DISTANCE);
			length = state < LZO_RUN_STATE ? 2U : 3U;
			next = instruction & 3U;
		} else if (instruction >= 64U) {
			if (in == in_end) {
				return BTRFS_CORRUPT;
			}
			distance = ((instruction >> 2) & 7U) + ((size_t)*in++ << 3) + 1U;
			length = (instruction >> 5) + 1U;
			next = instruction & 3U;
		} else if (instruction >= 32U) {
			length = instruction & 31U;
			if (length == 0 && !lzo_extend(&in, in_end, 31U, &length)) {
				return BTRFS_CORRUPT;
			}
			length += 2U;
			if (in_end - in < 2) {
				return BTRFS_CORRUPT;
			}
			distance = (size_t)(le16(in) >> 2) + 1U;
			next = le16(in) & 3U;
			in += 2;
		} else {
			length = instruction & 7U;
			if (length == 0 && !lzo_extend(&in, in_end, 7U, &length)) {
				return BTRFS_CORRUPT;
			}
			length += 2U;
			if (in_end - in < 2) {
				return BTRFS_CORRUPT;
			}
			distance = ((size_t)(instruction & 8U) << 11) + (size_t)(le16(in) >> 2);
			next = le16(in) & 3U;
			in += 2;
			if (distance == 0) {
				if (length != LZO_END_LENGTH || in != in_end) {
					return BTRFS_CORRUPT;
				}
				*produced = (size_t)(out - out_start);
				return BTRFS_OK;
			}
			distance += LZO_MATCH_16K_DISTANCE;
		}
		if (distance > (size_t)(out - out_start) || length > (size_t)(out_end - out)) {
			return BTRFS_CORRUPT;
		}
		copy_match(out, distance, length);
		out += length;
		if (!lzo_literals(&in, in_end, &out, out_end, next)) {
			return BTRFS_CORRUPT;
		}
		state = next;
	}
}

/* Zstandard (RFC 8878). Literals of a compressed block are decoded into the end
 * of the output, after everything the block can write: the block writes at
 * most all its literals plus its matches, so writing never overtakes the
 * literals it still has to copy, and no literal buffer is needed. A frame is
 * accepted only if libzstd accepts it both as one buffer and as a stream, the
 * way Linux reads Btrfs extents: a block's stored size and its literals are
 * bounded by the frame's block maximum (the stream's check), its decoded size
 * only by the output; a compressed block is never empty, and a stated content
 * size must match (the single buffer's checks). */
#define ZSTD_MAGIC 0xFD2FB528U
#define ZSTD_BLOCK_MAX (128U * 1024U)
#define ZSTD_WINDOW_LOG_MIN 10U
#define ZSTD_WINDOW_LOG_MAX 31U
#define ZSTD_BLOCK_RAW 0U
#define ZSTD_BLOCK_RLE 1U
#define ZSTD_BLOCK_COMPRESSED 2U
#define ZSTD_LITERALS_RAW 0U
#define ZSTD_LITERALS_RLE 1U
#define ZSTD_LITERALS_COMPRESSED 2U
#define ZSTD_LITERALS_TREELESS 3U
/* Four Huffman streams need at least this many literals and a jump table. */
#define ZSTD_FOUR_STREAM_LITERALS 6U
#define ZSTD_JUMP_TABLE_BYTES 6U
#define ZSTD_SEQUENCE_PREDEFINED 0U
#define ZSTD_SEQUENCE_RLE 1U
#define ZSTD_SEQUENCE_COMPRESSED 2U
#define ZSTD_SEQUENCE_REPEAT 3U
#define HUF_MAX_BITS 11U
#define HUF_MAX_SYMBOLS 256U
#define HUF_WEIGHT_LOG_MAX 6U
#define FSE_LOG_MIN 5U
#define FSE_MAX_SYMBOLS 256U
#define LL_MAX_SYMBOL 35U
#define ML_MAX_SYMBOL 52U
#define OF_MAX_SYMBOL 31U
#define LL_LOG_MAX 9U
#define ML_LOG_MAX 9U
#define OF_LOG_MAX 8U

struct huf_entry {
	uint8_t symbol;
	uint8_t bits;
};

struct fse_entry {
	uint16_t base;
	uint8_t symbol;
	uint8_t bits;
};

struct seq_table {
	struct fse_entry *entries;
	unsigned log;
	int valid;
};

struct zstd_workspace {
	struct huf_entry huf[1U << HUF_MAX_BITS];
	struct fse_entry ll[1U << LL_LOG_MAX];
	struct fse_entry ml[1U << ML_LOG_MAX];
	struct fse_entry of[1U << OF_LOG_MAX];
	struct fse_entry weights_table[1U << HUF_WEIGHT_LOG_MAX];
	int16_t norm[FSE_MAX_SYMBOLS];
	uint16_t next[FSE_MAX_SYMBOLS];
	uint8_t weights[HUF_MAX_SYMBOLS];
	unsigned huf_bits;
	struct seq_table tables[3];
};

_Static_assert(sizeof(struct zstd_workspace) <= BTRFS_ZSTD_WORKSPACE_BYTES,
    "the declared workspace holds the decoder's tables");

enum { TABLE_LL, TABLE_OF, TABLE_ML };

static const uint32_t ll_base[LL_MAX_SYMBOL + 1] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
	14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192,
	16384, 32768, 65536 };
static const uint8_t ll_bits[LL_MAX_SYMBOL + 1] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const uint32_t ml_base[ML_MAX_SYMBOL + 1] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 39, 41,
	43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539 };
static const uint8_t ml_bits[ML_MAX_SYMBOL + 1] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9,
	10, 11, 12, 13, 14, 15, 16 };
/* The predefined distributions (RFC 8878, 3.1.1.3.2.2). */
static const int16_t ll_default[LL_MAX_SYMBOL + 1] = { 4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1,
	1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1 };
static const int16_t ml_default[ML_MAX_SYMBOL + 1] = { 1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	-1, -1, -1, -1, -1, -1, -1 };
static const int16_t of_default[29] = { 1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, -1, -1, -1, -1, -1 };
#define LL_DEFAULT_LOG 6U
#define ML_DEFAULT_LOG 6U
#define OF_DEFAULT_LOG 5U
#define OF_DEFAULT_MAX_SYMBOL 28U

/* A backward bit stream: the last byte holds a marker bit above the padding,
 * and bits are read from there towards the first byte. container holds the
 * eight bytes from ptr (fewer, zero-extended, for a shorter stream), and
 * consumed counts the bits read from its top. Reading past the first bit
 * yields zeros and sets consumed beyond 64. */
struct bits {
	const uint8_t *start;
	const uint8_t *ptr;
	uint64_t container;
	unsigned consumed;
};

static int
bits_init(struct bits *bits, const uint8_t *start, size_t size)
{
	size_t i;

	if (size == 0 || start[size - 1] == 0) {
		return 0;
	}
	bits->start = start;
	if (size >= 8) {
		bits->ptr = start + size - 8;
		bits->container = le64(bits->ptr);
		bits->consumed = 8U - highbit(start[size - 1]);
		return 1;
	}
	bits->ptr = start;
	bits->container = 0;
	for (i = 0; i < size; i++) {
		bits->container |= (uint64_t)start[i] << (8U * i);
	}
	bits->consumed = (unsigned)(8U - size) * 8U + 8U - highbit(start[size - 1]);
	return 1;
}

static void
bits_refill(struct bits *bits)
{
	size_t step;

	if (bits->consumed > 64U || bits->ptr == bits->start) {
		return;
	}
	step = bits->consumed >> 3;
	if (step > (size_t)(bits->ptr - bits->start)) {
		step = (size_t)(bits->ptr - bits->start);
	}
	bits->ptr -= step;
	bits->consumed -= (unsigned)step * 8U;
	bits->container = le64(bits->ptr);
}

/* The next count bits (1-57) without consuming them. */
static uint64_t
bits_look(const struct bits *bits, unsigned count)
{
	if (bits->consumed >= 64U) {
		return 0;
	}
	return (bits->container << bits->consumed) >> (64U - count);
}

static uint64_t
bits_read(struct bits *bits, unsigned count)
{
	uint64_t value;

	if (count == 0) {
		return 0;
	}
	value = bits_look(bits, count);
	bits->consumed += count;
	return value;
}

static int
bits_overflowed(const struct bits *bits)
{
	return bits->consumed > 64U;
}

static int
bits_finished(const struct bits *bits)
{
	return bits->ptr == bits->start && bits->consumed == 64U;
}

/* A forward little-endian read of count (1-25) bits at bit position, zero past
 * the end. */
static uint32_t
forward_bits(const uint8_t *bytes, size_t size, uint64_t position, unsigned count)
{
	uint64_t value = 0;
	size_t first = (size_t)(position >> 3);
	size_t i;

	for (i = 0; i < 5 && first + i < size; i++) {
		value |= (uint64_t)bytes[first + i] << (8U * i);
	}
	return (uint32_t)((value >> (position & 7U)) & ((UINT64_C(1) << count) - 1U));
}

/* A normalized distribution (RFC 8878, 4.1.1): returns the bytes it used. */
static int
fse_read_counts(const uint8_t *in, size_t size, int16_t *norm, unsigned max_symbol,
    unsigned max_log, unsigned *log, size_t *used)
{
	uint64_t position = 4;
	int32_t remaining;
	int32_t threshold;
	int32_t count;
	int32_t max;
	unsigned width;
	unsigned symbol = 0;
	unsigned repeat;
	unsigned i;
	uint32_t value;

	if (size == 0) {
		return 0;
	}
	*log = (in[0] & 15U) + FSE_LOG_MIN;
	if (*log > max_log) {
		return 0;
	}
	remaining = (1 << *log) + 1;
	threshold = 1 << *log;
	width = *log + 1U;
	while (remaining > 1) {
		if (symbol > max_symbol) {
			return 0;
		}
		max = 2 * threshold - 1 - remaining;
		value = forward_bits(in, size, position, width);
		if ((int32_t)(value & (uint32_t)(threshold - 1)) < max) {
			count = (int32_t)(value & (uint32_t)(threshold - 1));
			position += width - 1U;
		} else {
			count = (int32_t)(value & (uint32_t)(2 * threshold - 1));
			if (count >= threshold) {
				count -= max;
			}
			position += width;
		}
		count--;
		remaining -= count < 0 ? -count : count;
		norm[symbol++] = (int16_t)count;
		/* Zero probabilities run on: two-bit counts of further zeros, a
		 * count of 3 followed by another. */
		if (count == 0) {
			do {
				repeat = forward_bits(in, size, position, 2);
				position += 2;
				for (i = 0; i < repeat; i++) {
					if (symbol > max_symbol) {
						return 0;
					}
					norm[symbol++] = 0;
				}
			} while (repeat == 3U);
		}
		while (remaining < threshold) {
			width--;
			threshold >>= 1;
		}
		if ((position + 7U) / 8U > size) {
			return 0;
		}
	}
	if (remaining != 1) {
		return 0;
	}
	while (symbol <= max_symbol) {
		norm[symbol++] = 0;
	}
	*used = (size_t)((position + 7U) / 8U);
	return 1;
}

/* The decoding table of a distribution summing to 1 << log (RFC 8878, 4.1.1). */
static int
fse_build(
    struct fse_entry *table, const int16_t *norm, unsigned max_symbol, unsigned log, uint16_t *next)
{
	uint32_t size = UINT32_C(1) << log;
	uint32_t high = size - 1U;
	uint32_t step = (size >> 1) + (size >> 3) + 3U;
	uint32_t position = 0;
	uint32_t steps;
	uint32_t state;
	unsigned symbol;
	unsigned width;
	int16_t i;

	for (symbol = 0; symbol <= max_symbol; symbol++) {
		if (norm[symbol] == -1) {
			table[high--].symbol = (uint8_t)symbol;
			next[symbol] = 1;
		} else {
			next[symbol] = (uint16_t)norm[symbol];
		}
	}
	for (symbol = 0; symbol <= max_symbol; symbol++) {
		for (i = 0; i < norm[symbol]; i++) {
			table[position].symbol = (uint8_t)symbol;
			steps = 0;
			do {
				position = (position + step) & (size - 1U);
				if (++steps > size) {
					return 0;
				}
			} while (position > high);
		}
	}
	if (position != 0) {
		return 0;
	}
	for (state = 0; state < size; state++) {
		symbol = table[state].symbol;
		width = log - highbit(next[symbol]);
		table[state].bits = (uint8_t)width;
		table[state].base = (uint16_t)(((uint32_t)next[symbol] << width) - size);
		next[symbol]++;
	}
	return 1;
}

/* Huffman weights (RFC 8878, 4.2.1.1) and the decoding table they describe:
 * returns the bytes of the description. */
static int
huf_read_table(struct zstd_workspace *ws, const uint8_t *in, size_t size, size_t *used)
{
	struct fse_entry *table = ws->weights_table;
	struct bits bits;
	uint32_t starts[HUF_MAX_BITS + 2];
	uint32_t total = 0;
	uint32_t rest;
	unsigned counts[HUF_MAX_BITS + 2] = { 0 };
	unsigned count = 0;
	unsigned log;
	unsigned state1;
	unsigned state2;
	unsigned last;
	unsigned bits_max;
	unsigned symbol;
	unsigned weight;
	unsigned i;
	size_t header;
	size_t j;

	if (size == 0) {
		return 0;
	}
	if (in[0] >= 128U) {
		count = in[0] - 127U;
		*used = 1U + (count + 1U) / 2U;
		if (*used > size) {
			return 0;
		}
		for (i = 0; i < count; i++) {
			ws->weights[i] =
			    (uint8_t)((i & 1U) ? in[1 + i / 2] & 15U : in[1 + i / 2] >> 4);
		}
	} else {
		*used = 1U + in[0];
		if (in[0] == 0 || *used > size) {
			return 0;
		}
		if (!fse_read_counts(in + 1, in[0], ws->norm, FSE_MAX_SYMBOLS - 1U,
			HUF_WEIGHT_LOG_MAX, &log, &header) ||
		    !fse_build(table, ws->norm, FSE_MAX_SYMBOLS - 1U, log, ws->next) ||
		    !bits_init(&bits, in + 1 + header, in[0] - header)) {
			return 0;
		}
		state1 = (unsigned)bits_read(&bits, log);
		state2 = (unsigned)bits_read(&bits, log);
		/* Two interleaved states; once a read passes the first bit, the other
		 * state gives the last weight. */
		for (;;) {
			if (count > HUF_MAX_SYMBOLS - 3U) {
				return 0;
			}
			ws->weights[count++] = table[state1].symbol;
			bits_refill(&bits);
			state1 =
			    table[state1].base + (unsigned)bits_read(&bits, table[state1].bits);
			if (bits_overflowed(&bits)) {
				ws->weights[count++] = table[state2].symbol;
				break;
			}
			ws->weights[count++] = table[state2].symbol;
			bits_refill(&bits);
			state2 =
			    table[state2].base + (unsigned)bits_read(&bits, table[state2].bits);
			if (bits_overflowed(&bits)) {
				ws->weights[count++] = table[state1].symbol;
				break;
			}
		}
	}
	for (i = 0; i < count; i++) {
		weight = ws->weights[i];
		if (weight > HUF_MAX_BITS) {
			return 0;
		}
		if (weight != 0) {
			total += UINT32_C(1) << (weight - 1U);
			counts[weight]++;
		}
	}
	if (total == 0) {
		return 0;
	}
	bits_max = highbit(total) + 1U;
	rest = (UINT32_C(1) << bits_max) - total;
	if (bits_max > HUF_MAX_BITS || (rest & (rest - 1U)) != 0) {
		return 0;
	}
	last = highbit(rest) + 1U;
	ws->weights[count++] = (uint8_t)last;
	counts[last]++;
	if (counts[1] < 2 || (counts[1] & 1U) != 0) {
		return 0;
	}
	starts[1] = 0;
	for (weight = 1; weight <= bits_max; weight++) {
		starts[weight + 1] =
		    starts[weight] + counts[weight] * (UINT32_C(1) << (weight - 1U));
	}
	for (symbol = 0; symbol < count; symbol++) {
		weight = ws->weights[symbol];
		if (weight == 0) {
			continue;
		}
		for (j = 0; j < (UINT32_C(1) << (weight - 1U)); j++) {
			ws->huf[starts[weight] + j].symbol = (uint8_t)symbol;
			ws->huf[starts[weight] + j].bits = (uint8_t)(bits_max + 1U - weight);
		}
		starts[weight] += UINT32_C(1) << (weight - 1U);
	}
	ws->huf_bits = bits_max;
	return 1;
}

static int
huf_decode_stream(
    const struct zstd_workspace *ws, const uint8_t *in, size_t size, uint8_t *out, size_t count)
{
	const struct huf_entry *entry;
	struct bits bits;
	size_t i;

	if (!bits_init(&bits, in, size)) {
		return 0;
	}
	for (i = 0; i < count; i++) {
		bits_refill(&bits);
		entry = &ws->huf[bits_look(&bits, ws->huf_bits)];
		bits.consumed += entry->bits;
		if (bits_overflowed(&bits)) {
			return 0;
		}
		out[i] = entry->symbol;
	}
	bits_refill(&bits);
	return bits_finished(&bits);
}

/* The literals section: *literals points at the regenerated literals, in the
 * input or at the end of the output. Returns the section's bytes. */
static int
zstd_literals(struct zstd_workspace *ws, const uint8_t *in, size_t size, uint8_t *out,
    uint8_t *out_end, size_t block_max, const uint8_t **literals, size_t *literal_count,
    size_t *used)
{
	unsigned type = in[0] & 3U;
	unsigned format = (in[0] >> 2) & 3U;
	size_t header;
	size_t regenerated;
	size_t compressed = 0;
	size_t streams_size;
	size_t tree = 0;
	size_t segment;
	size_t sizes[4];
	const uint8_t *streams;
	uint8_t *destination;
	unsigned i;

	if (type == ZSTD_LITERALS_RAW || type == ZSTD_LITERALS_RLE) {
		header = format == 1U ? 2U : format == 3U ? 3U : 1U;
		if (header > size) {
			return 0;
		}
		regenerated = format == 1U ? (size_t)(in[0] >> 4) + ((size_t)in[1] << 4)
		    : format == 3U
		    ? (size_t)(in[0] >> 4) + ((size_t)in[1] << 4) + ((size_t)in[2] << 12)
		    : (size_t)(in[0] >> 3);
	} else {
		header = format <= 1U ? 3U : format == 2U ? 4U : 5U;
		if (header > size) {
			return 0;
		}
		if (format <= 1U) {
			regenerated = (le32(in) >> 4) & 0x3FFU;
			compressed = (le32(in) >> 14) & 0x3FFU;
		} else if (format == 2U) {
			regenerated = (le32(in) >> 4) & 0x3FFFU;
			compressed = le32(in) >> 18;
		} else {
			regenerated = (le32(in) >> 4) & 0x3FFFFU;
			compressed = (le32(in) >> 22) + ((size_t)in[4] << 10);
		}
	}
	if (regenerated > block_max || regenerated > (size_t)(out_end - out)) {
		return 0;
	}
	*literal_count = regenerated;
	destination = out_end - regenerated;
	if (type == ZSTD_LITERALS_RAW) {
		if (regenerated > size - header) {
			return 0;
		}
		*literals = in + header;
		*used = header + regenerated;
		return 1;
	}
	if (type == ZSTD_LITERALS_RLE) {
		if (header == size) {
			return 0;
		}
		for (segment = 0; segment < regenerated; segment++) {
			destination[segment] = in[header];
		}
		*literals = destination;
		*used = header + 1U;
		return 1;
	}
	if (regenerated == 0 || compressed > size - header) {
		return 0;
	}
	*used = header + compressed;
	streams = in + header;
	if (type == ZSTD_LITERALS_COMPRESSED) {
		if (!huf_read_table(ws, streams, compressed, &tree)) {
			return 0;
		}
	} else if (ws->huf_bits == 0) {
		return 0;
	}
	streams += tree;
	streams_size = compressed - tree;
	*literals = destination;
	if (format == 0U) {
		return huf_decode_stream(ws, streams, streams_size, destination, regenerated);
	}
	if (regenerated < ZSTD_FOUR_STREAM_LITERALS || streams_size < ZSTD_JUMP_TABLE_BYTES + 4U) {
		return 0;
	}
	sizes[0] = le16(streams);
	sizes[1] = le16(streams + 2);
	sizes[2] = le16(streams + 4);
	if (sizes[0] + sizes[1] + sizes[2] >= streams_size - ZSTD_JUMP_TABLE_BYTES) {
		return 0;
	}
	sizes[3] = streams_size - ZSTD_JUMP_TABLE_BYTES - sizes[0] - sizes[1] - sizes[2];
	segment = (regenerated + 3U) / 4U;
	streams += ZSTD_JUMP_TABLE_BYTES;
	for (i = 0; i < 4; i++) {
		if (!huf_decode_stream(ws, streams, sizes[i], destination + segment * i,
			i < 3 ? segment : regenerated - 3U * segment)) {
			return 0;
		}
		streams += sizes[i];
	}
	return 1;
}

/* One of the sequence section's three tables, by its mode. */
static int
zstd_sequence_table(
    struct zstd_workspace *ws, unsigned kind, unsigned mode, const uint8_t **in, const uint8_t *end)
{
	static const unsigned max_symbols[3] = { LL_MAX_SYMBOL, OF_MAX_SYMBOL, ML_MAX_SYMBOL };
	static const unsigned max_logs[3] = { LL_LOG_MAX, OF_LOG_MAX, ML_LOG_MAX };
	struct seq_table *table = &ws->tables[kind];
	const int16_t *defaults;
	size_t used;
	unsigned log;
	unsigned i;

	switch (mode) {
	case ZSTD_SEQUENCE_PREDEFINED:
		defaults = kind == TABLE_LL ? ll_default
		    : kind == TABLE_OF	    ? of_default
					    : ml_default;
		log = kind == TABLE_LL ? LL_DEFAULT_LOG
		    : kind == TABLE_OF ? OF_DEFAULT_LOG
				       : ML_DEFAULT_LOG;
		for (i = 0; i <= max_symbols[kind]; i++) {
			ws->norm[i] =
			    kind == TABLE_OF && i > OF_DEFAULT_MAX_SYMBOL ? 0 : defaults[i];
		}
		if (!fse_build(table->entries, ws->norm, max_symbols[kind], log, ws->next)) {
			return 0;
		}
		table->log = log;
		break;
	case ZSTD_SEQUENCE_RLE:
		if (*in == end || **in > max_symbols[kind]) {
			return 0;
		}
		table->entries[0] = (struct fse_entry){ 0, **in, 0 };
		table->log = 0;
		(*in)++;
		break;
	case ZSTD_SEQUENCE_COMPRESSED:
		if (!fse_read_counts(*in, (size_t)(end - *in), ws->norm, max_symbols[kind],
			max_logs[kind], &log, &used) ||
		    !fse_build(table->entries, ws->norm, max_symbols[kind], log, ws->next)) {
			return 0;
		}
		table->log = log;
		*in += used;
		break;
	default:
		if (!table->valid) {
			return 0;
		}
		return 1;
	}
	table->valid = 1;
	return 1;
}

struct zstd_frame {
	uint8_t *start;
	uint8_t *end;
	uint64_t repeat[3];
};

/* A compressed block: its literals, then its sequences, executed in place. */
static int
zstd_compressed_block(struct zstd_workspace *ws, struct zstd_frame *frame, const uint8_t *in,
    size_t size, uint8_t **output, size_t block_max)
{
	const struct fse_entry *ll_entry;
	const struct fse_entry *ml_entry;
	const struct fse_entry *of_entry;
	const uint8_t *end = in + size;
	const uint8_t *literals;
	struct bits bits;
	uint8_t *out = *output;
	size_t literal_count;
	size_t used;
	size_t sequences;
	size_t n;
	uint64_t literal_length;
	uint64_t match_length;
	uint64_t offset_value;
	uint64_t offset;
	unsigned ll_state;
	unsigned ml_state;
	unsigned of_state;
	unsigned index;
	unsigned modes;

	if (size == 0 ||
	    !zstd_literals(
		ws, in, size, out, frame->end, block_max, &literals, &literal_count, &used)) {
		return 0;
	}
	in += used;
	if (in == end) {
		return 0;
	}
	if (*in < 128U) {
		sequences = *in++;
	} else if (*in < 255U) {
		if (end - in < 2) {
			return 0;
		}
		sequences = ((size_t)(in[0] - 128U) << 8) + in[1];
		in += 2;
	} else {
		if (end - in < 3) {
			return 0;
		}
		sequences = (size_t)in[1] + ((size_t)in[2] << 8) + 0x7F00U;
		in += 3;
	}
	if (sequences == 0) {
		if (in != end) {
			return 0;
		}
		copy_forward(out, literals, literal_count);
		*output = out + literal_count;
		return 1;
	}
	if (in == end) {
		return 0;
	}
	modes = *in++;
	if ((modes & 3U) != 0 || !zstd_sequence_table(ws, TABLE_LL, modes >> 6, &in, end) ||
	    !zstd_sequence_table(ws, TABLE_OF, (modes >> 4) & 3U, &in, end) ||
	    !zstd_sequence_table(ws, TABLE_ML, (modes >> 2) & 3U, &in, end) ||
	    !bits_init(&bits, in, (size_t)(end - in))) {
		return 0;
	}
	ll_state = (unsigned)bits_read(&bits, ws->tables[TABLE_LL].log);
	bits_refill(&bits);
	of_state = (unsigned)bits_read(&bits, ws->tables[TABLE_OF].log);
	bits_refill(&bits);
	ml_state = (unsigned)bits_read(&bits, ws->tables[TABLE_ML].log);
	for (n = 0; n < sequences; n++) {
		ll_entry = &ws->tables[TABLE_LL].entries[ll_state];
		ml_entry = &ws->tables[TABLE_ML].entries[ml_state];
		of_entry = &ws->tables[TABLE_OF].entries[of_state];
		bits_refill(&bits);
		offset_value =
		    (UINT64_C(1) << of_entry->symbol) + bits_read(&bits, of_entry->symbol);
		bits_refill(&bits);
		match_length =
		    ml_base[ml_entry->symbol] + bits_read(&bits, ml_bits[ml_entry->symbol]);
		bits_refill(&bits);
		literal_length =
		    ll_base[ll_entry->symbol] + bits_read(&bits, ll_bits[ll_entry->symbol]);
		if (offset_value > 3U) {
			offset = offset_value - 3U;
			frame->repeat[2] = frame->repeat[1];
			frame->repeat[1] = frame->repeat[0];
			frame->repeat[0] = offset;
		} else {
			index = (unsigned)offset_value - 1U + (literal_length == 0 ? 1U : 0U);
			if (index == 0) {
				offset = frame->repeat[0];
			} else {
				offset = index == 3U ? frame->repeat[0] - 1U : frame->repeat[index];
				if (index != 1U) {
					frame->repeat[2] = frame->repeat[1];
				}
				frame->repeat[1] = frame->repeat[0];
				frame->repeat[0] = offset;
			}
		}
		if (n + 1 < sequences) {
			bits_refill(&bits);
			ll_state = ll_entry->base + (unsigned)bits_read(&bits, ll_entry->bits);
			bits_refill(&bits);
			ml_state = ml_entry->base + (unsigned)bits_read(&bits, ml_entry->bits);
			bits_refill(&bits);
			of_state = of_entry->base + (unsigned)bits_read(&bits, of_entry->bits);
		}
		/* The block's remaining literals must still fit after the match: at
		 * the end of the output they also stay ahead of everything written. */
		if (bits_overflowed(&bits) || offset == 0 || literal_length > literal_count ||
		    match_length + literal_count > (uint64_t)(frame->end - out)) {
			return 0;
		}
		copy_forward(out, literals, (size_t)literal_length);
		out += literal_length;
		literals += literal_length;
		literal_count -= (size_t)literal_length;
		if (offset > (uint64_t)(out - frame->start)) {
			return 0;
		}
		copy_match(out, (size_t)offset, (size_t)match_length);
		out += match_length;
	}
	bits_refill(&bits);
	if (!bits_finished(&bits)) {
		return 0;
	}
	copy_forward(out, literals, literal_count);
	*output = out + literal_count;
	return 1;
}

/* XXH64 with seed 0, for the content checksum. */
#define XXH_P1 UINT64_C(0x9E3779B185EBCA87)
#define XXH_P2 UINT64_C(0xC2B2AE3D27D4EB4F)
#define XXH_P3 UINT64_C(0x165667B19E3779F9)
#define XXH_P4 UINT64_C(0x85EBCA77C2B2AE63)
#define XXH_P5 UINT64_C(0x27D4EB2F165667C5)
#define XXH_STRIPE_BYTES 32U

static uint64_t
rotl64(uint64_t value, unsigned count)
{
	return value << count | value >> (64U - count);
}

static uint64_t
xxh_round(uint64_t accumulator, uint64_t input)
{
	return rotl64(accumulator + input * XXH_P2, 31) * XXH_P1;
}

static uint64_t
xxh_merge(uint64_t hash, uint64_t accumulator)
{
	return (hash ^ xxh_round(0, accumulator)) * XXH_P1 + XXH_P4;
}

static uint64_t
xxh64(const uint8_t *bytes, size_t length)
{
	const uint8_t *end = bytes + length;
	uint64_t v1 = XXH_P1 + XXH_P2;
	uint64_t v2 = XXH_P2;
	uint64_t v3 = 0;
	uint64_t v4 = (uint64_t)0 - XXH_P1;
	uint64_t hash;

	if (length >= XXH_STRIPE_BYTES) {
		while ((size_t)(end - bytes) >= XXH_STRIPE_BYTES) {
			v1 = xxh_round(v1, le64(bytes));
			v2 = xxh_round(v2, le64(bytes + 8));
			v3 = xxh_round(v3, le64(bytes + 16));
			v4 = xxh_round(v4, le64(bytes + 24));
			bytes += XXH_STRIPE_BYTES;
		}
		hash = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
		hash = xxh_merge(hash, v1);
		hash = xxh_merge(hash, v2);
		hash = xxh_merge(hash, v3);
		hash = xxh_merge(hash, v4);
	} else {
		hash = XXH_P5;
	}
	hash += (uint64_t)length;
	while ((size_t)(end - bytes) >= 8U) {
		hash = rotl64(hash ^ xxh_round(0, le64(bytes)), 27) * XXH_P1 + XXH_P4;
		bytes += 8;
	}
	if ((size_t)(end - bytes) >= 4U) {
		hash = rotl64(hash ^ (uint64_t)le32(bytes) * XXH_P1, 23) * XXH_P2 + XXH_P3;
		bytes += 4;
	}
	while (bytes < end) {
		hash = rotl64(hash ^ *bytes++ * XXH_P5, 11) * XXH_P1;
	}
	hash ^= hash >> 33;
	hash *= XXH_P2;
	hash ^= hash >> 29;
	hash *= XXH_P3;
	return hash ^ hash >> 32;
}

enum btrfs_result
btrfs_zstd_decompress(void *workspace, const void *input, size_t input_size, void *output,
    size_t capacity, size_t *produced)
{
	static const uint8_t content_bytes[4] = { 0, 2, 4, 8 };
	static const uint8_t dictionary_bytes[4] = { 0, 1, 2, 4 };
	struct zstd_workspace *ws = workspace;
	struct zstd_frame frame;
	const uint8_t *in = input;
	const uint8_t *end = in + input_size;
	uint8_t *out = output;
	uint64_t window = 0;
	uint64_t content = 0;
	size_t block_max;
	size_t header_bytes;
	size_t block_size;
	unsigned descriptor;
	unsigned content_field;
	unsigned window_log;
	unsigned block_header;
	unsigned type;
	unsigned last;
	unsigned i;

	if (workspace == NULL || input == NULL || output == NULL || produced == NULL ||
	    input_size < 6) {
		return BTRFS_CORRUPT;
	}
	if (le32(in) != ZSTD_MAGIC) {
		return BTRFS_CORRUPT;
	}
	descriptor = in[4];
	in += 5;
	/* No reserved bit and no dictionary. */
	if ((descriptor & 0x08U) != 0) {
		return BTRFS_CORRUPT;
	}
	content_field = content_bytes[descriptor >> 6];
	if (content_field == 0 && (descriptor & 0x20U) != 0) {
		content_field = 1;
	}
	header_bytes = ((descriptor & 0x20U) == 0 ? 1U : 0U) + dictionary_bytes[descriptor & 3U] +
	    content_field;
	if (header_bytes > (size_t)(end - in)) {
		return BTRFS_CORRUPT;
	}
	if ((descriptor & 0x20U) == 0) {
		window_log = ZSTD_WINDOW_LOG_MIN + (*in >> 3);
		if (window_log > ZSTD_WINDOW_LOG_MAX) {
			return BTRFS_CORRUPT;
		}
		window =
		    (UINT64_C(1) << window_log) + ((UINT64_C(1) << window_log) / 8U) * (*in & 7U);
		in++;
	}
	for (i = 0; i < dictionary_bytes[descriptor & 3U]; i++) {
		if (in[i] != 0) {
			return BTRFS_CORRUPT;
		}
	}
	in += dictionary_bytes[descriptor & 3U];
	if (content_field != 0) {
		for (i = 0; i < content_field; i++) {
			content |= (uint64_t)in[i] << (8U * i);
		}
		if (content_field == 2) {
			content += 256U;
		}
		in += content_field;
		if (content > capacity) {
			return BTRFS_CORRUPT;
		}
		if ((descriptor & 0x20U) != 0) {
			window = content;
		}
	}
	block_max = window < ZSTD_BLOCK_MAX ? (size_t)window : ZSTD_BLOCK_MAX;
	/* A stated content size bounds the output exactly. */
	frame.start = out;
	frame.end = out + (content_field != 0 ? (size_t)content : capacity);
	frame.repeat[0] = 1;
	frame.repeat[1] = 4;
	frame.repeat[2] = 8;
	ws->huf_bits = 0;
	ws->tables[TABLE_LL] = (struct seq_table){ ws->ll, 0, 0 };
	ws->tables[TABLE_OF] = (struct seq_table){ ws->of, 0, 0 };
	ws->tables[TABLE_ML] = (struct seq_table){ ws->ml, 0, 0 };
	do {
		if (end - in < 3) {
			return BTRFS_CORRUPT;
		}
		block_header = (unsigned)in[0] | (unsigned)in[1] << 8 | (unsigned)in[2] << 16;
		in += 3;
		last = block_header & 1U;
		type = (block_header >> 1) & 3U;
		block_size = block_header >> 3;
		/* An RLE block's size is its decoded size, bounded by the output. */
		if (block_size > block_max && type != ZSTD_BLOCK_RLE) {
			return BTRFS_CORRUPT;
		}
		switch (type) {
		case ZSTD_BLOCK_RAW:
			if (block_size > (size_t)(end - in) ||
			    block_size > (size_t)(frame.end - out)) {
				return BTRFS_CORRUPT;
			}
			__builtin_memcpy(out, in, block_size);
			in += block_size;
			out += block_size;
			break;
		case ZSTD_BLOCK_RLE:
			if (in == end || block_size > (size_t)(frame.end - out)) {
				return BTRFS_CORRUPT;
			}
			__builtin_memset(out, *in++, block_size);
			out += block_size;
			break;
		case ZSTD_BLOCK_COMPRESSED:
			if (block_size > (size_t)(end - in) ||
			    !zstd_compressed_block(ws, &frame, in, block_size, &out, block_max)) {
				return BTRFS_CORRUPT;
			}
			in += block_size;
			break;
		default:
			return BTRFS_CORRUPT;
		}
	} while (!last);
	if (content_field != 0 && out != frame.end) {
		return BTRFS_CORRUPT;
	}
	if ((descriptor & 0x04U) != 0) {
		if (end - in < 4 ||
		    le32(in) != (uint32_t)xxh64(frame.start, (size_t)(out - frame.start))) {
			return BTRFS_CORRUPT;
		}
	}
	*produced = (size_t)(out - frame.start);
	return BTRFS_OK;
}
