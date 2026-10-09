/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_HEURISTIC_H
#define MACHLIN_BTRFS_HEURISTIC_H

#include "internal.h"

/* 16 bytes of every 256 of the 128 KiB Linux samples at most. */
#define BT_HEURISTIC_SAMPLE_MAX 8192U

/* The heuristic's sample and byte counts (core/heuristic.c). */
struct bt_heuristic {
	uint8_t sample[BT_HEURISTIC_SAMPLE_MAX];
	uint32_t bucket[256];
	uint32_t scratch[256];
	uint32_t size;
};

/* Linux's btrfs_compress_heuristic over the range [start, end] (end
 * inclusive) of a file whose bytes from start are data (length of them), then
 * zeros, read in pages of page bytes: nonzero when compression is worth an
 * attempt. */
int bt_compress_heuristic(struct bt_heuristic *heuristic, const uint8_t *data, size_t length,
    uint64_t start, uint64_t end, uint32_t page);

#endif
