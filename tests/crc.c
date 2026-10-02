/* SPDX-License-Identifier: BSD-3-Clause */
/* CRC32C of the implementation in use (hardware where the build has it, or
 * the table built with BT_CRC_PORTABLE) against a bitwise definition, for
 * every length up to several words, unaligned buffers and sector batches. */
#include "internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Reflected Castagnoli polynomial. */
#define CRC32C_POLYNOMIAL UINT32_C(0x82f63b78)
/* CRC32C of "123456789" with the standard initial value and final inversion. */
#define CRC32C_CHECK UINT32_C(0xe3069283)
#define MAX_LENGTH 300U
#define MAX_SECTORS 11U
#define MAX_SECTOR_BYTES 4096U
/* The largest batch at the largest offset. */
#define BUFFER_BYTES (MAX_SECTORS * MAX_SECTOR_BYTES + 8U)
/* The largest node and the alignments its checksum is tried at. */
#define MAX_BLOCK_BYTES 65536U
#define BLOCK_OFFSETS 4U
#define CSUM_BYTES 32U

static uint32_t
reference(uint32_t crc, const uint8_t *bytes, size_t length)
{
	size_t i;
	unsigned bit;

	for (i = 0; i < length; i++) {
		crc ^= bytes[i];
		for (bit = 0; bit < 8; bit++) {
			crc = (crc >> 1) ^ (CRC32C_POLYNOMIAL & (0U - (crc & 1U)));
		}
	}
	return crc;
}

/* Lanes joined for every node size and for lengths around the lane bounds,
 * a length the join was not prepared for, and lanes too short to join. */
static void
blocks(void)
{
	static uint8_t block[MAX_BLOCK_BYTES + BLOCK_OFFSETS];
	static const size_t lengths[] = { 4096 - CSUM_BYTES, 8192 - CSUM_BYTES, 16384 - CSUM_BYTES,
		32768 - CSUM_BYTES, MAX_BLOCK_BYTES - CSUM_BYTES, 3 * BT_CRC_BLOCK_MINIMUM,
		3 * BT_CRC_BLOCK_MINIMUM + 7, 3 * BT_CRC_BLOCK_MINIMUM - 1, 4096, 1 };
	struct bt_crc_shift shift;
	uint32_t state = 7;
	size_t offset;
	size_t length;
	size_t i;

	for (i = 0; i < sizeof(block); i++) {
		state = state * UINT32_C(1103515245) + UINT32_C(12345);
		block[i] = (uint8_t)(state >> 16);
	}
	for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		length = lengths[i];
		bt_crc_shift_init(&shift, length);
		assert((shift.lane != 0) == (length >= BT_CRC_BLOCK_LANES * BT_CRC_BLOCK_MINIMUM));
		for (offset = 0; offset < BLOCK_OFFSETS; offset++) {
			assert(bt_crc32c_block(&shift, UINT32_MAX, block + offset, length) ==
			    reference(UINT32_MAX, block + offset, length));
			assert(bt_crc32c_block(&shift, 0x9e3779b9U, block + offset, length) ==
			    reference(0x9e3779b9U, block + offset, length));
		}
		assert(bt_crc32c_block(&shift, UINT32_MAX, block, length - 1) ==
		    reference(UINT32_MAX, block, length - 1));
	}
}

int
main(void)
{
	static uint8_t buffer[BUFFER_BYTES];
	static const size_t sector_sizes[] = { MAX_SECTOR_BYTES, 512, 64, 12, 8, 1 };
	uint32_t sums[MAX_SECTORS];
	uint32_t state = 1;
	size_t offset;
	size_t length;
	size_t size;
	size_t count;
	size_t i;

	for (i = 0; i < sizeof(buffer); i++) {
		state = state * UINT32_C(1103515245) + UINT32_C(12345);
		buffer[i] = (uint8_t)(state >> 16);
	}
	assert(~bt_crc32c(UINT32_MAX, "123456789", 9) == CRC32C_CHECK);
	for (offset = 0; offset < 9; offset++) {
		for (length = 0; length <= MAX_LENGTH; length++) {
			assert(bt_crc32c(UINT32_MAX, buffer + offset, length) ==
			    reference(UINT32_MAX, buffer + offset, length));
			assert(bt_crc32c(0x12345678U + (uint32_t)length, buffer + offset, length) ==
			    reference(0x12345678U + (uint32_t)length, buffer + offset, length));
		}
	}
	for (size = 0; size < sizeof(sector_sizes) / sizeof(sector_sizes[0]); size++) {
		for (offset = 0; offset < 3; offset++) {
			for (count = 0; count <= MAX_SECTORS; count++) {
				memset(sums, 0, sizeof(sums));
				bt_crc32c_sectors(buffer + offset, sector_sizes[size], count, sums);
				for (i = 0; i < count; i++) {
					assert(sums[i] ==
					    ~reference(UINT32_MAX,
						buffer + offset + i * sector_sizes[size],
						sector_sizes[size]));
				}
				for (; i < MAX_SECTORS; i++) {
					assert(sums[i] == 0);
				}
			}
		}
	}
	blocks();
	printf("CRC32C against its bitwise definition: lengths, seeds, alignment, sector "
	       "batches and joined node lanes PASS\n");
	return 0;
}
