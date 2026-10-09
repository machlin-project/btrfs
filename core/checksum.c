/* SPDX-License-Identifier: BSD-3-Clause */
/* The checksum algorithms a superblock's checksum_type selects, as Linux's
 * crypto API computes them: CRC32C (stored complemented, little-endian), XXH64
 * with seed zero (little-endian), SHA-256 and unkeyed BLAKE2b with a 256-bit
 * digest. Each checksums one complete buffer; none uses SIMD registers.
 * SHA-256 and SHA-512 also keep state between calls (struct bt_sha2), for
 * fs-verity's salted block hashes. */
#include "encode.h"

#define BT_XXH_PRIME1 UINT64_C(0x9e3779b185ebca87)
#define BT_XXH_PRIME2 UINT64_C(0xc2b2ae3d27d4eb4f)
#define BT_XXH_PRIME3 UINT64_C(0x165667b19e3779f9)
#define BT_XXH_PRIME4 UINT64_C(0x85ebca77c2b2ae63)
#define BT_XXH_PRIME5 UINT64_C(0x27d4eb2f165667c5)
/* Bytes of the four accumulator lanes XXH64 consumes per round. */
#define BT_XXH_STRIPE 32U

#define BT_SHA256_BLOCK 64U
#define BT_SHA256_WORDS 8U
#define BT_SHA256_ROUNDS 64U
/* Bytes of the big-endian message length in bits that ends the final
 * block: 64 bits for SHA-256 and 128 for SHA-512. */
#define BT_SHA256_LENGTH_BYTES 8U
#define BT_SHA512_LENGTH_BYTES 16U
#define BT_SHA2_PAD 0x80U
#define BT_SHA512_BLOCK 128U
#define BT_SHA512_WORDS 8U
#define BT_SHA512_ROUNDS 80U

#define BT_BLAKE2B_BLOCK 128U
#define BT_BLAKE2B_WORDS 8U
#define BT_BLAKE2B_ROUNDS 12U
#define BT_BLAKE2B_DIGEST 32U
/* Parameter block word 0: digest length, key length (none) << 8, fanout 1 <<
 * 16, depth 1 << 24 (sequential mode). */
#define BT_BLAKE2B_PARAMETERS (UINT64_C(0x01010000) | BT_BLAKE2B_DIGEST)

static inline uint64_t
bt_rotl64(uint64_t value, unsigned bits)
{
	return value << bits | value >> (64U - bits);
}

static inline uint64_t
bt_rotr64(uint64_t value, unsigned bits)
{
	return value >> bits | value << (64U - bits);
}

static inline uint32_t
bt_rotr32(uint32_t value, unsigned bits)
{
	return value >> bits | value << (32U - bits);
}

static inline uint32_t
bt_load32(const uint8_t *bytes)
{
	struct bt_le32 word;

	bt_copy(&word, bytes, sizeof(word));
	return bt_u32(word);
}

static inline uint64_t
bt_load64(const uint8_t *bytes)
{
	struct bt_le64 word;

	bt_copy(&word, bytes, sizeof(word));
	return bt_u64(word);
}

static inline uint32_t
bt_load32_big(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 |
	    (uint32_t)bytes[3];
}

static inline uint64_t
bt_xxh_round(uint64_t accumulator, uint64_t input)
{
	accumulator += input * BT_XXH_PRIME2;
	accumulator = bt_rotl64(accumulator, 31);
	return accumulator * BT_XXH_PRIME1;
}

static inline uint64_t
bt_xxh_merge(uint64_t hash, uint64_t lane)
{
	hash ^= bt_xxh_round(0, lane);
	return hash * BT_XXH_PRIME1 + BT_XXH_PRIME4;
}

static uint64_t
bt_xxh64(const uint8_t *data, size_t length)
{
	uint64_t lanes[4] = { BT_XXH_PRIME1 + BT_XXH_PRIME2, BT_XXH_PRIME2, 0,
		(uint64_t)0 - BT_XXH_PRIME1 };
	uint64_t hash;
	size_t position = 0;
	unsigned lane;

	if (length >= BT_XXH_STRIPE) {
		for (; length - position >= BT_XXH_STRIPE; position += BT_XXH_STRIPE) {
			for (lane = 0; lane < 4; lane++) {
				lanes[lane] = bt_xxh_round(
				    lanes[lane], bt_load64(data + position + lane * 8U));
			}
		}
		hash = bt_rotl64(lanes[0], 1) + bt_rotl64(lanes[1], 7) + bt_rotl64(lanes[2], 12) +
		    bt_rotl64(lanes[3], 18);
		for (lane = 0; lane < 4; lane++) {
			hash = bt_xxh_merge(hash, lanes[lane]);
		}
	} else {
		hash = BT_XXH_PRIME5;
	}
	hash += (uint64_t)length;
	for (; length - position >= 8U; position += 8U) {
		hash ^= bt_xxh_round(0, bt_load64(data + position));
		hash = bt_rotl64(hash, 27) * BT_XXH_PRIME1 + BT_XXH_PRIME4;
	}
	if (length - position >= 4U) {
		hash ^= (uint64_t)bt_load32(data + position) * BT_XXH_PRIME1;
		hash = bt_rotl64(hash, 23) * BT_XXH_PRIME2 + BT_XXH_PRIME3;
		position += 4U;
	}
	for (; position < length; position++) {
		hash ^= data[position] * BT_XXH_PRIME5;
		hash = bt_rotl64(hash, 11) * BT_XXH_PRIME1;
	}
	hash ^= hash >> 33;
	hash *= BT_XXH_PRIME2;
	hash ^= hash >> 29;
	hash *= BT_XXH_PRIME3;
	hash ^= hash >> 32;
	return hash;
}

static const uint32_t bt_sha256_constants[BT_SHA256_ROUNDS] = {
	UINT32_C(0x428a2f98),
	UINT32_C(0x71374491),
	UINT32_C(0xb5c0fbcf),
	UINT32_C(0xe9b5dba5),
	UINT32_C(0x3956c25b),
	UINT32_C(0x59f111f1),
	UINT32_C(0x923f82a4),
	UINT32_C(0xab1c5ed5),
	UINT32_C(0xd807aa98),
	UINT32_C(0x12835b01),
	UINT32_C(0x243185be),
	UINT32_C(0x550c7dc3),
	UINT32_C(0x72be5d74),
	UINT32_C(0x80deb1fe),
	UINT32_C(0x9bdc06a7),
	UINT32_C(0xc19bf174),
	UINT32_C(0xe49b69c1),
	UINT32_C(0xefbe4786),
	UINT32_C(0x0fc19dc6),
	UINT32_C(0x240ca1cc),
	UINT32_C(0x2de92c6f),
	UINT32_C(0x4a7484aa),
	UINT32_C(0x5cb0a9dc),
	UINT32_C(0x76f988da),
	UINT32_C(0x983e5152),
	UINT32_C(0xa831c66d),
	UINT32_C(0xb00327c8),
	UINT32_C(0xbf597fc7),
	UINT32_C(0xc6e00bf3),
	UINT32_C(0xd5a79147),
	UINT32_C(0x06ca6351),
	UINT32_C(0x14292967),
	UINT32_C(0x27b70a85),
	UINT32_C(0x2e1b2138),
	UINT32_C(0x4d2c6dfc),
	UINT32_C(0x53380d13),
	UINT32_C(0x650a7354),
	UINT32_C(0x766a0abb),
	UINT32_C(0x81c2c92e),
	UINT32_C(0x92722c85),
	UINT32_C(0xa2bfe8a1),
	UINT32_C(0xa81a664b),
	UINT32_C(0xc24b8b70),
	UINT32_C(0xc76c51a3),
	UINT32_C(0xd192e819),
	UINT32_C(0xd6990624),
	UINT32_C(0xf40e3585),
	UINT32_C(0x106aa070),
	UINT32_C(0x19a4c116),
	UINT32_C(0x1e376c08),
	UINT32_C(0x2748774c),
	UINT32_C(0x34b0bcb5),
	UINT32_C(0x391c0cb3),
	UINT32_C(0x4ed8aa4a),
	UINT32_C(0x5b9cca4f),
	UINT32_C(0x682e6ff3),
	UINT32_C(0x748f82ee),
	UINT32_C(0x78a5636f),
	UINT32_C(0x84c87814),
	UINT32_C(0x8cc70208),
	UINT32_C(0x90befffa),
	UINT32_C(0xa4506ceb),
	UINT32_C(0xbef9a3f7),
	UINT32_C(0xc67178f2),
};

/* The first 32 bits of the fractional parts of the square roots of the first
 * eight primes, which are also BLAKE2b's IV in 64 bits below. */
static const uint32_t bt_sha256_initial[BT_SHA256_WORDS] = {
	UINT32_C(0x6a09e667),
	UINT32_C(0xbb67ae85),
	UINT32_C(0x3c6ef372),
	UINT32_C(0xa54ff53a),
	UINT32_C(0x510e527f),
	UINT32_C(0x9b05688c),
	UINT32_C(0x1f83d9ab),
	UINT32_C(0x5be0cd19),
};

static void
bt_sha256_block(uint32_t state[BT_SHA256_WORDS], const uint8_t *block)
{
	uint32_t schedule[16];
	uint32_t work[BT_SHA256_WORDS];
	uint32_t word;
	uint32_t s0;
	uint32_t s1;
	uint32_t choose;
	uint32_t majority;
	uint32_t first;
	uint32_t second;
	unsigned round;
	unsigned i;

	for (i = 0; i < BT_SHA256_WORDS; i++) {
		work[i] = state[i];
	}
	for (round = 0; round < BT_SHA256_ROUNDS; round++) {
		if (round < 16) {
			word = bt_load32_big(block + round * 4U);
		} else {
			s0 = bt_rotr32(schedule[(round + 1) % 16], 7) ^
			    bt_rotr32(schedule[(round + 1) % 16], 18) ^
			    schedule[(round + 1) % 16] >> 3;
			s1 = bt_rotr32(schedule[(round + 14) % 16], 17) ^
			    bt_rotr32(schedule[(round + 14) % 16], 19) ^
			    schedule[(round + 14) % 16] >> 10;
			word = schedule[round % 16] + s0 + schedule[(round + 9) % 16] + s1;
		}
		schedule[round % 16] = word;
		s1 = bt_rotr32(work[4], 6) ^ bt_rotr32(work[4], 11) ^ bt_rotr32(work[4], 25);
		choose = (work[4] & work[5]) ^ (~work[4] & work[6]);
		first = work[7] + s1 + choose + bt_sha256_constants[round] + word;
		s0 = bt_rotr32(work[0], 2) ^ bt_rotr32(work[0], 13) ^ bt_rotr32(work[0], 22);
		majority = (work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]);
		second = s0 + majority;
		for (i = BT_SHA256_WORDS - 1; i > 0; i--) {
			work[i] = work[i - 1];
		}
		work[4] += first;
		work[0] = first + second;
	}
	for (i = 0; i < BT_SHA256_WORDS; i++) {
		state[i] += work[i];
	}
}

/* The first 64 bits of the fractional parts of the cube roots of the first
 * eighty primes. */
static const uint64_t bt_sha512_constants[BT_SHA512_ROUNDS] = {
	UINT64_C(0x428a2f98d728ae22),
	UINT64_C(0x7137449123ef65cd),
	UINT64_C(0xb5c0fbcfec4d3b2f),
	UINT64_C(0xe9b5dba58189dbbc),
	UINT64_C(0x3956c25bf348b538),
	UINT64_C(0x59f111f1b605d019),
	UINT64_C(0x923f82a4af194f9b),
	UINT64_C(0xab1c5ed5da6d8118),
	UINT64_C(0xd807aa98a3030242),
	UINT64_C(0x12835b0145706fbe),
	UINT64_C(0x243185be4ee4b28c),
	UINT64_C(0x550c7dc3d5ffb4e2),
	UINT64_C(0x72be5d74f27b896f),
	UINT64_C(0x80deb1fe3b1696b1),
	UINT64_C(0x9bdc06a725c71235),
	UINT64_C(0xc19bf174cf692694),
	UINT64_C(0xe49b69c19ef14ad2),
	UINT64_C(0xefbe4786384f25e3),
	UINT64_C(0x0fc19dc68b8cd5b5),
	UINT64_C(0x240ca1cc77ac9c65),
	UINT64_C(0x2de92c6f592b0275),
	UINT64_C(0x4a7484aa6ea6e483),
	UINT64_C(0x5cb0a9dcbd41fbd4),
	UINT64_C(0x76f988da831153b5),
	UINT64_C(0x983e5152ee66dfab),
	UINT64_C(0xa831c66d2db43210),
	UINT64_C(0xb00327c898fb213f),
	UINT64_C(0xbf597fc7beef0ee4),
	UINT64_C(0xc6e00bf33da88fc2),
	UINT64_C(0xd5a79147930aa725),
	UINT64_C(0x06ca6351e003826f),
	UINT64_C(0x142929670a0e6e70),
	UINT64_C(0x27b70a8546d22ffc),
	UINT64_C(0x2e1b21385c26c926),
	UINT64_C(0x4d2c6dfc5ac42aed),
	UINT64_C(0x53380d139d95b3df),
	UINT64_C(0x650a73548baf63de),
	UINT64_C(0x766a0abb3c77b2a8),
	UINT64_C(0x81c2c92e47edaee6),
	UINT64_C(0x92722c851482353b),
	UINT64_C(0xa2bfe8a14cf10364),
	UINT64_C(0xa81a664bbc423001),
	UINT64_C(0xc24b8b70d0f89791),
	UINT64_C(0xc76c51a30654be30),
	UINT64_C(0xd192e819d6ef5218),
	UINT64_C(0xd69906245565a910),
	UINT64_C(0xf40e35855771202a),
	UINT64_C(0x106aa07032bbd1b8),
	UINT64_C(0x19a4c116b8d2d0c8),
	UINT64_C(0x1e376c085141ab53),
	UINT64_C(0x2748774cdf8eeb99),
	UINT64_C(0x34b0bcb5e19b48a8),
	UINT64_C(0x391c0cb3c5c95a63),
	UINT64_C(0x4ed8aa4ae3418acb),
	UINT64_C(0x5b9cca4f7763e373),
	UINT64_C(0x682e6ff3d6b2b8a3),
	UINT64_C(0x748f82ee5defb2fc),
	UINT64_C(0x78a5636f43172f60),
	UINT64_C(0x84c87814a1f0ab72),
	UINT64_C(0x8cc702081a6439ec),
	UINT64_C(0x90befffa23631e28),
	UINT64_C(0xa4506cebde82bde9),
	UINT64_C(0xbef9a3f7b2c67915),
	UINT64_C(0xc67178f2e372532b),
	UINT64_C(0xca273eceea26619c),
	UINT64_C(0xd186b8c721c0c207),
	UINT64_C(0xeada7dd6cde0eb1e),
	UINT64_C(0xf57d4f7fee6ed178),
	UINT64_C(0x06f067aa72176fba),
	UINT64_C(0x0a637dc5a2c898a6),
	UINT64_C(0x113f9804bef90dae),
	UINT64_C(0x1b710b35131c471b),
	UINT64_C(0x28db77f523047d84),
	UINT64_C(0x32caab7b40c72493),
	UINT64_C(0x3c9ebe0a15c9bebc),
	UINT64_C(0x431d67c49c100d4c),
	UINT64_C(0x4cc5d4becb3e42b6),
	UINT64_C(0x597f299cfc657e2a),
	UINT64_C(0x5fcb6fab3ad6faec),
	UINT64_C(0x6c44198c4a475817),
};

/* The first 64 bits of the fractional parts of the square roots of the first
 * eight primes: SHA-512's initial state and BLAKE2b's IV. */
static const uint64_t bt_blake2b_initial[BT_BLAKE2B_WORDS] = {
	UINT64_C(0x6a09e667f3bcc908),
	UINT64_C(0xbb67ae8584caa73b),
	UINT64_C(0x3c6ef372fe94f82b),
	UINT64_C(0xa54ff53a5f1d36f1),
	UINT64_C(0x510e527fade682d1),
	UINT64_C(0x9b05688c2b3e6c1f),
	UINT64_C(0x1f83d9abfb41bd6b),
	UINT64_C(0x5be0cd19137e2179),
};

static inline uint64_t
bt_load64_big(const uint8_t *bytes)
{
	return (uint64_t)bt_load32_big(bytes) << 32 | bt_load32_big(bytes + 4U);
}

static void
bt_sha512_block(uint64_t state[BT_SHA512_WORDS], const uint8_t *block)
{
	uint64_t schedule[16];
	uint64_t work[BT_SHA512_WORDS];
	uint64_t word;
	uint64_t s0;
	uint64_t s1;
	uint64_t choose;
	uint64_t majority;
	uint64_t first;
	uint64_t second;
	unsigned round;
	unsigned i;

	for (i = 0; i < BT_SHA512_WORDS; i++) {
		work[i] = state[i];
	}
	for (round = 0; round < BT_SHA512_ROUNDS; round++) {
		if (round < 16) {
			word = bt_load64_big(block + round * 8U);
		} else {
			s0 = bt_rotr64(schedule[(round + 1) % 16], 1) ^
			    bt_rotr64(schedule[(round + 1) % 16], 8) ^
			    schedule[(round + 1) % 16] >> 7;
			s1 = bt_rotr64(schedule[(round + 14) % 16], 19) ^
			    bt_rotr64(schedule[(round + 14) % 16], 61) ^
			    schedule[(round + 14) % 16] >> 6;
			word = schedule[round % 16] + s0 + schedule[(round + 9) % 16] + s1;
		}
		schedule[round % 16] = word;
		s1 = bt_rotr64(work[4], 14) ^ bt_rotr64(work[4], 18) ^ bt_rotr64(work[4], 41);
		choose = (work[4] & work[5]) ^ (~work[4] & work[6]);
		first = work[7] + s1 + choose + bt_sha512_constants[round] + word;
		s0 = bt_rotr64(work[0], 28) ^ bt_rotr64(work[0], 34) ^ bt_rotr64(work[0], 39);
		majority = (work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]);
		second = s0 + majority;
		for (i = BT_SHA512_WORDS - 1; i > 0; i--) {
			work[i] = work[i - 1];
		}
		work[4] += first;
		work[0] = first + second;
	}
	for (i = 0; i < BT_SHA512_WORDS; i++) {
		state[i] += work[i];
	}
}

static void
bt_sha2_compress(struct bt_sha2 *hash, const uint8_t *block)
{
	if (hash->wide) {
		bt_sha512_block(hash->large, block);
	} else {
		bt_sha256_block(hash->small, block);
	}
}

void
bt_sha2_init(struct bt_sha2 *hash, int wide)
{
	unsigned i;

	bt_zero(hash, sizeof(*hash));
	hash->wide = wide;
	for (i = 0; i < BT_SHA256_WORDS; i++) {
		hash->small[i] = bt_sha256_initial[i];
	}
	/* BLAKE2b's IV is SHA-512's initial state. */
	for (i = 0; i < BT_SHA512_WORDS; i++) {
		hash->large[i] = bt_blake2b_initial[i];
	}
}

size_t
bt_sha2_block_size(const struct bt_sha2 *hash)
{
	return hash->wide ? BT_SHA512_BLOCK : BT_SHA256_BLOCK;
}

size_t
bt_sha2_digest_size(const struct bt_sha2 *hash)
{
	return hash->wide ? BT_SHA512_DIGEST : BT_SHA256_DIGEST;
}

void
bt_sha2_update(struct bt_sha2 *hash, const void *data, size_t length)
{
	const uint8_t *bytes = data;
	size_t block = bt_sha2_block_size(hash);
	size_t count;

	hash->length += length;
	if (hash->used != 0) {
		count = block - hash->used < length ? block - hash->used : length;
		bt_copy(hash->pending + hash->used, bytes, count);
		hash->used += count;
		bytes += count;
		length -= count;
		if (hash->used < block) {
			return;
		}
		bt_sha2_compress(hash, hash->pending);
		hash->used = 0;
	}
	for (; length >= block; length -= block) {
		bt_sha2_compress(hash, bytes);
		bytes += block;
	}
	bt_copy(hash->pending, bytes, length);
	hash->used = length;
}

void
bt_sha2_final(struct bt_sha2 *hash, uint8_t *digest)
{
	size_t block = bt_sha2_block_size(hash);
	size_t length_bytes = hash->wide ? BT_SHA512_LENGTH_BYTES : BT_SHA256_LENGTH_BYTES;
	uint64_t bits = hash->length * 8U;
	unsigned i;

	hash->pending[hash->used++] = BT_SHA2_PAD;
	bt_zero(hash->pending + hash->used, block - hash->used);
	if (block - hash->used < length_bytes) {
		bt_sha2_compress(hash, hash->pending);
		bt_zero(hash->pending, block);
	}
	/* The upper bits of a 128-bit length stay zero: no message here
	 * reaches 2^61 bytes. */
	for (i = 0; i < 8; i++) {
		hash->pending[block - 1U - i] = (uint8_t)(bits >> (8U * i));
	}
	bt_sha2_compress(hash, hash->pending);
	if (hash->wide) {
		for (i = 0; i < BT_SHA512_WORDS; i++) {
			digest[i * 8U] = (uint8_t)(hash->large[i] >> 56);
			digest[i * 8U + 1U] = (uint8_t)(hash->large[i] >> 48);
			digest[i * 8U + 2U] = (uint8_t)(hash->large[i] >> 40);
			digest[i * 8U + 3U] = (uint8_t)(hash->large[i] >> 32);
			digest[i * 8U + 4U] = (uint8_t)(hash->large[i] >> 24);
			digest[i * 8U + 5U] = (uint8_t)(hash->large[i] >> 16);
			digest[i * 8U + 6U] = (uint8_t)(hash->large[i] >> 8);
			digest[i * 8U + 7U] = (uint8_t)hash->large[i];
		}
	} else {
		for (i = 0; i < BT_SHA256_WORDS; i++) {
			digest[i * 4U] = (uint8_t)(hash->small[i] >> 24);
			digest[i * 4U + 1U] = (uint8_t)(hash->small[i] >> 16);
			digest[i * 4U + 2U] = (uint8_t)(hash->small[i] >> 8);
			digest[i * 4U + 3U] = (uint8_t)hash->small[i];
		}
	}
}

static void
bt_sha256(const uint8_t *data, size_t length, uint8_t digest[BT_CSUM_SIZE])
{
	struct bt_sha2 hash;

	bt_sha2_init(&hash, 0);
	bt_sha2_update(&hash, data, length);
	bt_sha2_final(&hash, digest);
}

/* Message word order of each round; rounds 10 and 11 repeat rounds 0 and 1. */
static const uint8_t bt_blake2b_sigma[10][16] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
	{ 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
	{ 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
	{ 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
	{ 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
	{ 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
	{ 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
	{ 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
	{ 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
};

/* The G mixing function's state words for the four column then four diagonal
 * steps of a round: a, b, c, d. */
static const uint8_t bt_blake2b_steps[8][4] = {
	{ 0, 4, 8, 12 },
	{ 1, 5, 9, 13 },
	{ 2, 6, 10, 14 },
	{ 3, 7, 11, 15 },
	{ 0, 5, 10, 15 },
	{ 1, 6, 11, 12 },
	{ 2, 7, 8, 13 },
	{ 3, 4, 9, 14 },
};

static void
bt_blake2b_block(uint64_t state[BT_BLAKE2B_WORDS], const uint8_t *block, uint64_t counter, int last)
{
	uint64_t message[16];
	uint64_t work[16];
	const uint8_t *sigma;
	const uint8_t *step;
	unsigned round;
	unsigned i;

	for (i = 0; i < 16; i++) {
		message[i] = bt_load64(block + i * 8U);
	}
	for (i = 0; i < BT_BLAKE2B_WORDS; i++) {
		work[i] = state[i];
		work[i + BT_BLAKE2B_WORDS] = bt_blake2b_initial[i];
	}
	/* The byte counter's high word stays zero: lengths fit in 64 bits. */
	work[12] ^= counter;
	if (last) {
		work[14] = ~work[14];
	}
	for (round = 0; round < BT_BLAKE2B_ROUNDS; round++) {
		sigma = bt_blake2b_sigma[round % 10U];
		for (i = 0; i < 8; i++) {
			step = bt_blake2b_steps[i];
			work[step[0]] += work[step[1]] + message[sigma[2U * i]];
			work[step[3]] = bt_rotr64(work[step[3]] ^ work[step[0]], 32);
			work[step[2]] += work[step[3]];
			work[step[1]] = bt_rotr64(work[step[1]] ^ work[step[2]], 24);
			work[step[0]] += work[step[1]] + message[sigma[2U * i + 1U]];
			work[step[3]] = bt_rotr64(work[step[3]] ^ work[step[0]], 16);
			work[step[2]] += work[step[3]];
			work[step[1]] = bt_rotr64(work[step[1]] ^ work[step[2]], 63);
		}
	}
	for (i = 0; i < BT_BLAKE2B_WORDS; i++) {
		state[i] ^= work[i] ^ work[i + BT_BLAKE2B_WORDS];
	}
}

static void
bt_blake2b(const uint8_t *data, size_t length, uint8_t digest[BT_CSUM_SIZE])
{
	uint8_t block[BT_BLAKE2B_BLOCK];
	uint64_t state[BT_BLAKE2B_WORDS];
	struct bt_le64 word;
	size_t position;
	unsigned i;

	for (i = 0; i < BT_BLAKE2B_WORDS; i++) {
		state[i] = bt_blake2b_initial[i];
	}
	state[0] ^= BT_BLAKE2B_PARAMETERS;
	/* The final block, full or not, is the last: never an empty extra one,
	 * except for an empty message. */
	for (position = 0; length - position > BT_BLAKE2B_BLOCK; position += BT_BLAKE2B_BLOCK) {
		bt_blake2b_block(state, data + position, (uint64_t)position + BT_BLAKE2B_BLOCK, 0);
	}
	bt_zero(block, sizeof(block));
	bt_copy(block, data + position, length - position);
	bt_blake2b_block(state, block, (uint64_t)length, 1);
	for (i = 0; i < BT_BLAKE2B_DIGEST / sizeof(uint64_t); i++) {
		bt_put64(&word, state[i]);
		bt_copy(digest + i * sizeof(uint64_t), &word, sizeof(word));
	}
}

size_t
bt_checksum_size(unsigned type)
{
	switch (type) {
	case BT_CHECKSUM_CRC32C:
		return sizeof(uint32_t);
	case BT_CHECKSUM_XXHASH:
		return sizeof(uint64_t);
	case BT_CHECKSUM_SHA256:
	case BT_CHECKSUM_BLAKE2:
		return BT_CSUM_SIZE;
	default:
		return 0;
	}
}

void
bt_checksum(unsigned type, const void *data, size_t length, uint8_t sum[BT_CSUM_SIZE])
{
	struct bt_le32 crc;
	struct bt_le64 xxh;

	bt_zero(sum, BT_CSUM_SIZE);
	switch (type) {
	case BT_CHECKSUM_CRC32C:
		bt_put32(&crc, ~bt_crc32c(UINT32_MAX, data, length));
		bt_copy(sum, &crc, sizeof(crc));
		break;
	case BT_CHECKSUM_XXHASH:
		bt_put64(&xxh, bt_xxh64(data, length));
		bt_copy(sum, &xxh, sizeof(xxh));
		break;
	case BT_CHECKSUM_SHA256:
		bt_sha256(data, length, sum);
		break;
	case BT_CHECKSUM_BLAKE2:
		bt_blake2b(data, length, sum);
		break;
	default:
		break;
	}
}

void
bt_checksum_node(const struct btrfs_fs *fs, const uint8_t *node, uint8_t sum[BT_CSUM_SIZE])
{
	struct bt_le32 crc;

	if (fs->info.checksum_type == BT_CHECKSUM_CRC32C) {
		bt_zero(sum, BT_CSUM_SIZE);
		bt_put32(&crc,
		    ~bt_crc32c_block(&fs->node_crc, UINT32_MAX, node + BT_CSUM_SIZE,
			fs->info.node_size - BT_CSUM_SIZE));
		bt_copy(sum, &crc, sizeof(crc));
	} else {
		bt_checksum(fs->info.checksum_type, node + BT_CSUM_SIZE,
		    fs->info.node_size - BT_CSUM_SIZE, sum);
	}
}

size_t
bt_checksum_batch(const struct btrfs_fs *fs)
{
	return BT_CHECKSUM_BATCH_BYTES / bt_checksum_size(fs->info.checksum_type);
}

void
bt_checksum_sectors(const struct btrfs_fs *fs, const void *data, size_t count, uint8_t *sums)
{
	const uint8_t *bytes = data;
	uint8_t sum[BT_CSUM_SIZE];
	uint32_t crcs[BT_CRC_BATCH];
	struct bt_le32 crc;
	size_t sector = fs->info.sector_size;
	size_t size = bt_checksum_size(fs->info.checksum_type);
	size_t i;

	if (fs->info.checksum_type == BT_CHECKSUM_CRC32C) {
		bt_crc32c_sectors(data, sector, count, crcs);
		for (i = 0; i < count; i++) {
			bt_put32(&crc, crcs[i]);
			bt_copy(sums + i * sizeof(crc), &crc, sizeof(crc));
		}
		return;
	}
	for (i = 0; i < count; i++) {
		bt_checksum(fs->info.checksum_type, bytes + i * sector, sector, sum);
		bt_copy(sums + i * size, sum, size);
	}
}
