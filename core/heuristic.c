/* SPDX-License-Identifier: BSD-3-Clause */
/* Linux's btrfs_compress_heuristic, which decides whether a range is worth an
 * attempt to compress it when the mount says compress and not compress-force.
 * It samples 16 bytes from every 256 of at most 128 KiB, taking the range by
 * pages as the page cache holds it, so bytes past the end of the file in its
 * last page sample as zeros. A sample whose halves are equal is compressible;
 * then the byte set (fewer than 64 values: compressible), the core byte set,
 * the values covering 90% of the sample after Linux's radix sort (at most 64:
 * compressible, at least 200: not), and an approximate Shannon entropy (up to
 * 65% compressible, from 80% not). The sort orders only as many low bits of
 * the counts as Linux's does, and the entropy sums the sorted counts up to the
 * first zero, so decisions match Linux's for every sample. */
#include "heuristic.h"

#define BT_HEURISTIC_READ 16U
#define BT_HEURISTIC_INTERVAL 256U
#define BT_HEURISTIC_RANGE (128U * 1024U)
#define BT_HEURISTIC_BUCKETS 256U
#define BT_HEURISTIC_BYTE_SET 64U
#define BT_HEURISTIC_CORE_LOW 64U
#define BT_HEURISTIC_CORE_HIGH 200U
#define BT_HEURISTIC_CORE_PERCENT 90U
#define BT_HEURISTIC_ENTROPY_LOW 65U
#define BT_HEURISTIC_ENTROPY_HIGH 80U
#define BT_HEURISTIC_RADIX_BITS 4U
#define BT_HEURISTIC_COUNTERS (1U << BT_HEURISTIC_RADIX_BITS)
#define BT_HEURISTIC_BYTE_BITS 8U

_Static_assert(
    BT_HEURISTIC_SAMPLE_MAX == BT_HEURISTIC_RANGE * BT_HEURISTIC_READ / BT_HEURISTIC_INTERVAL,
    "heuristic sample");

static unsigned
bt_heuristic_log2(uint64_t value)
{
	unsigned log = 0;

	while (value > 1) {
		value >>= 1;
		log++;
	}
	return log;
}

/* ilog2 of the fourth power, which keeps more of the fraction. */
static uint32_t
bt_heuristic_log2_4(uint64_t value)
{
	return bt_heuristic_log2(value * value * value * value);
}

/* The four bits of a count at shift, reversed so that the sort descends. */
static unsigned
bt_heuristic_digit(uint32_t count, unsigned shift)
{
	return (BT_HEURISTIC_COUNTERS - 1U) - ((count >> shift) % BT_HEURISTIC_COUNTERS);
}

/* One counting pass of the radix sort from source to target. */
static void
bt_heuristic_pass(const uint32_t *source, uint32_t *target, unsigned shift)
{
	uint32_t counters[BT_HEURISTIC_COUNTERS];
	unsigned digit;
	size_t i;

	for (i = 0; i < BT_HEURISTIC_COUNTERS; i++) {
		counters[i] = 0;
	}
	for (i = 0; i < BT_HEURISTIC_BUCKETS; i++) {
		counters[bt_heuristic_digit(source[i], shift)]++;
	}
	for (i = 1; i < BT_HEURISTIC_COUNTERS; i++) {
		counters[i] += counters[i - 1];
	}
	for (i = BT_HEURISTIC_BUCKETS; i-- > 0;) {
		digit = bt_heuristic_digit(source[i], shift);
		target[--counters[digit]] = source[i];
	}
}

/* Linux's radix_sort over the bucket counts: pairs of four-bit passes over
 * ilog2 of the largest count rounded up to a multiple of eight bits. */
static void
bt_heuristic_sort(struct bt_heuristic *heuristic)
{
	uint32_t largest = 0;
	unsigned bits;
	unsigned shift;
	size_t i;

	for (i = 0; i < BT_HEURISTIC_BUCKETS; i++) {
		largest = heuristic->bucket[i] > largest ? heuristic->bucket[i] : largest;
	}
	bits = bt_heuristic_log2(largest);
	bits =
	    (bits + BT_HEURISTIC_BYTE_BITS - 1U) / BT_HEURISTIC_BYTE_BITS * BT_HEURISTIC_BYTE_BITS;
	for (shift = 0; shift < bits; shift += 2U * BT_HEURISTIC_RADIX_BITS) {
		bt_heuristic_pass(heuristic->bucket, heuristic->scratch, shift);
		bt_heuristic_pass(
		    heuristic->scratch, heuristic->bucket, shift + BT_HEURISTIC_RADIX_BITS);
	}
}

/* Linux's heuristic_collect_sample over [start, end], end inclusive, where
 * the range's bytes from start are data (length of them) and zeros after. */
static void
bt_heuristic_sample(struct bt_heuristic *heuristic, const uint8_t *data, size_t length,
    uint64_t start, uint64_t end, uint32_t page)
{
	uint64_t origin = start;
	uint64_t index;
	uint64_t index_end;
	uint64_t offset;
	uint32_t within;
	size_t i;

	if (end - start > BT_HEURISTIC_RANGE) {
		end = start + BT_HEURISTIC_RANGE;
	}
	index = start / page;
	index_end = end / page + (end % page != 0 ? 1U : 0U);
	heuristic->size = 0;
	for (; index < index_end; index++) {
		within = (uint32_t)(start % page);
		while (within < page - BT_HEURISTIC_READ && start <= end - BT_HEURISTIC_READ) {
			for (i = 0; i < BT_HEURISTIC_READ; i++) {
				offset = index * page + within + i - origin;
				heuristic->sample[heuristic->size + i] =
				    offset < length ? data[offset] : 0;
			}
			within += BT_HEURISTIC_INTERVAL;
			start += BT_HEURISTIC_INTERVAL;
			heuristic->size += BT_HEURISTIC_READ;
		}
	}
}

static uint32_t
bt_heuristic_entropy(const struct bt_heuristic *heuristic)
{
	const uint32_t maximum = 8U * bt_heuristic_log2_4(2);
	uint32_t base = bt_heuristic_log2_4(heuristic->size);
	uint32_t sum = 0;
	size_t i;

	for (i = 0; i < BT_HEURISTIC_BUCKETS && heuristic->bucket[i] > 0; i++) {
		sum += heuristic->bucket[i] * (base - bt_heuristic_log2_4(heuristic->bucket[i]));
	}
	return sum / heuristic->size * 100U / maximum;
}

int
bt_compress_heuristic(struct bt_heuristic *heuristic, const uint8_t *data, size_t length,
    uint64_t start, uint64_t end, uint32_t page)
{
	uint32_t threshold;
	uint32_t sum = 0;
	unsigned values = 0;
	uint32_t entropy;
	size_t half;
	size_t i;

	bt_heuristic_sample(heuristic, data, length, start, end, page);
	half = heuristic->size / 2U;
	if (bt_equal(heuristic->sample, heuristic->sample + half, half)) {
		return 1;
	}
	for (i = 0; i < BT_HEURISTIC_BUCKETS; i++) {
		heuristic->bucket[i] = 0;
	}
	for (i = 0; i < heuristic->size; i++) {
		heuristic->bucket[heuristic->sample[i]]++;
	}
	/* Linux's byte_set_size stops counting past its threshold. */
	for (i = 0; i < BT_HEURISTIC_BUCKETS && values <= BT_HEURISTIC_BYTE_SET; i++) {
		values += heuristic->bucket[i] > 0;
	}
	if (values < BT_HEURISTIC_BYTE_SET) {
		return 1;
	}
	threshold = heuristic->size * BT_HEURISTIC_CORE_PERCENT / 100U;
	bt_heuristic_sort(heuristic);
	for (i = 0; i < BT_HEURISTIC_CORE_LOW; i++) {
		sum += heuristic->bucket[i];
	}
	if (sum <= threshold) {
		for (; i < BT_HEURISTIC_CORE_HIGH && heuristic->bucket[i] > 0; i++) {
			sum += heuristic->bucket[i];
			if (sum > threshold) {
				break;
			}
		}
	}
	if (i <= BT_HEURISTIC_CORE_LOW) {
		return 1;
	}
	if (i >= BT_HEURISTIC_CORE_HIGH) {
		return 0;
	}
	entropy = bt_heuristic_entropy(heuristic);
	return entropy < BT_HEURISTIC_ENTROPY_HIGH ? 1 : 0;
}
