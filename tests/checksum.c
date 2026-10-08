/* SPDX-License-Identifier: BSD-3-Clause */
/* The core's checksum algorithms against published digests and, where the
 * host has them, reference implementations: libxxhash for XXH64, libb2 for
 * BLAKE2b-256 and CommonCrypto for SHA-256, over every length around their
 * block sizes, sector and node sizes, and unaligned buffers. Sector batches
 * and node checksums must equal the one-buffer algorithm. */
#include "internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef BTRFS_HAVE_XXHASH
#include <xxhash.h>
#endif
#ifdef BTRFS_HAVE_B2
#include <blake2.h>
#endif
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

/* Every length up to this many bytes, beyond two BLAKE2b blocks. */
#define SHORT_LENGTHS 600U
#define MAX_OFFSET 7U
#define SECTOR_BYTES 4096U
#define MAX_NODE_BYTES 65536U
#define BATCH_SECTORS 70U
#define BUFFER_BYTES (BATCH_SECTORS * SECTOR_BYTES + MAX_OFFSET)
#define TYPES 4U

struct vector {
	unsigned type;
	const char *message;
	const char *digest;
};

/* Digests of the empty message, the "abc" test message of FIPS 180-2 and RFC
 * 7693 and a pangram, as their reference implementations compute them, in the
 * byte order of a checksum field. */
static const struct vector vectors[] = {
	{ BT_CHECKSUM_CRC32C, "123456789", "839206e3" },
	{ BT_CHECKSUM_XXHASH, "", "99e9d85137db46ef" },
	{ BT_CHECKSUM_XXHASH, "abc", "990977adf52cbc44" },
	{ BT_CHECKSUM_XXHASH, "The quick brown fox jumps over the lazy dog", "bc71da1f362d240b" },
	{ BT_CHECKSUM_SHA256, "",
	    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
	{ BT_CHECKSUM_SHA256, "abc",
	    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
	{ BT_CHECKSUM_SHA256, "The quick brown fox jumps over the lazy dog",
	    "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592" },
	{ BT_CHECKSUM_BLAKE2, "",
	    "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8" },
	{ BT_CHECKSUM_BLAKE2, "abc",
	    "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319" },
	{ BT_CHECKSUM_BLAKE2, "The quick brown fox jumps over the lazy dog",
	    "01718cec35cd3d796dd00020e0bfecb473ad23457d063b75eff29c0ffa2e58a9" },
};

static uint8_t buffer[BUFFER_BYTES];
static uint64_t references;

static void
hex(const char *text, uint8_t *bytes, size_t size)
{
	size_t i;
	unsigned value;

	assert(strlen(text) == 2 * size);
	for (i = 0; i < size; i++) {
		assert(sscanf(text + 2 * i, "%2x", &value) == 1);
		bytes[i] = (uint8_t)value;
	}
}

static void
check_vectors(void)
{
	uint8_t expected[BT_CSUM_SIZE];
	uint8_t sum[BT_CSUM_SIZE];
	size_t size;
	size_t i;

	for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
		size = bt_checksum_size(vectors[i].type);
		memset(expected, 0, sizeof(expected));
		hex(vectors[i].digest, expected, size);
		bt_checksum(vectors[i].type, vectors[i].message, strlen(vectors[i].message), sum);
		if (memcmp(sum, expected, sizeof(sum)) != 0) {
			fprintf(stderr, "vector %zu of type %u differs\n", i, vectors[i].type);
			abort();
		}
	}
	assert(bt_checksum_size(BT_CHECKSUM_BLAKE2 + 1U) == 0);
	assert(bt_checksum_size(UINT16_MAX) == 0);
}

/* The reference digest as a checksum field; zero without a reference. */
static int
reference(unsigned type, const uint8_t *data, size_t length, uint8_t sum[BT_CSUM_SIZE])
{
#ifdef BTRFS_HAVE_XXHASH
	uint64_t xxh;
	size_t i;
#endif

	memset(sum, 0, BT_CSUM_SIZE);
	switch (type) {
	case BT_CHECKSUM_CRC32C:
		/* tests/crc.c checks CRC32C against its definition. */
		return 0;
	case BT_CHECKSUM_XXHASH:
#ifdef BTRFS_HAVE_XXHASH
		xxh = XXH64(data, length, 0);
		for (i = 0; i < sizeof(xxh); i++) {
			sum[i] = (uint8_t)(xxh >> (8U * i));
		}
		return 1;
#else
		return 0;
#endif
	case BT_CHECKSUM_SHA256:
#ifdef __APPLE__
		CC_SHA256(data, (CC_LONG)length, sum);
		return 1;
#else
		return 0;
#endif
	case BT_CHECKSUM_BLAKE2:
#ifdef BTRFS_HAVE_B2
		if (blake2b(sum, data, NULL, BT_CSUM_SIZE, length, 0) != 0) {
			abort();
		}
		return 1;
#else
		return 0;
#endif
	default:
		abort();
	}
}

static void
compare(unsigned type, const uint8_t *data, size_t length)
{
	uint8_t expected[BT_CSUM_SIZE];
	uint8_t sum[BT_CSUM_SIZE];

	bt_checksum(type, data, length, sum);
	if (reference(type, data, length, expected)) {
		references++;
		if (memcmp(sum, expected, sizeof(sum)) != 0) {
			fprintf(stderr, "type %u differs at length %zu\n", type, length);
			abort();
		}
	}
}

static void
check_lengths(void)
{
	static const size_t long_lengths[] = { 1023, 1024, 1025, 4095, SECTOR_BYTES, 4097, 16352,
		16384, 65504, MAX_NODE_BYTES };
	size_t length;
	size_t offset;
	size_t i;
	unsigned type;

	for (type = 0; type < TYPES; type++) {
		for (offset = 0; offset <= MAX_OFFSET; offset++) {
			for (length = 0; length <= SHORT_LENGTHS; length++) {
				compare(type, buffer + offset, length);
			}
		}
		for (i = 0; i < sizeof(long_lengths) / sizeof(long_lengths[0]); i++) {
			compare(type, buffer + 1, long_lengths[i]);
			compare(type, buffer, long_lengths[i]);
		}
	}
}

/* Batches and node checksums of each type equal the one-buffer checksums. */
static void
check_views(void)
{
	static struct btrfs_fs fs;
	static const uint32_t node_sizes[] = { 4096, 16384, MAX_NODE_BYTES };
	uint8_t sums[BT_CHECKSUM_BATCH_BYTES];
	uint8_t sum[BT_CSUM_SIZE];
	size_t size;
	size_t count;
	size_t i;
	unsigned type;
	unsigned node;

	for (type = 0; type < TYPES; type++) {
		memset(&fs, 0, sizeof(fs));
		fs.info.checksum_type = (uint16_t)type;
		fs.info.sector_size = SECTOR_BYTES;
		size = bt_checksum_size(type);
		assert(bt_checksum_batch(&fs) * size == BT_CHECKSUM_BATCH_BYTES);
		for (count = 1; count <= bt_checksum_batch(&fs); count += count < 8 ? 1 : 13) {
			bt_checksum_sectors(&fs, buffer + 3, count, sums);
			for (i = 0; i < count; i++) {
				bt_checksum(type, buffer + 3 + i * SECTOR_BYTES, SECTOR_BYTES, sum);
				assert(memcmp(sums + i * size, sum, size) == 0);
			}
		}
		for (node = 0; node < sizeof(node_sizes) / sizeof(node_sizes[0]); node++) {
			fs.info.node_size = node_sizes[node];
			bt_crc_shift_init(&fs.node_crc, fs.info.node_size - BT_CSUM_SIZE);
			bt_checksum_node(&fs, buffer, sums);
			bt_checksum(
			    type, buffer + BT_CSUM_SIZE, fs.info.node_size - BT_CSUM_SIZE, sum);
			assert(memcmp(sums, sum, sizeof(sum)) == 0);
		}
	}
}

int
main(void)
{
	uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
	size_t i;

	for (i = 0; i < sizeof(buffer); i++) {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		buffer[i] = (uint8_t)state;
	}
	check_vectors();
	check_lengths();
	check_views();
	printf("checksums: %zu published digests, %llu reference comparisons\n",
	    sizeof(vectors) / sizeof(vectors[0]), (unsigned long long)references);
	return 0;
}
