/* SPDX-License-Identifier: BSD-3-Clause */
#include <btrfs/staging.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define DEVICE_BYTES 8192U

struct device {
	uint8_t live[DEVICE_BYTES];
	uint8_t stable[DEVICE_BYTES];
	uint8_t expected[DEVICE_BYTES];
	size_t allocations;
	size_t allocated;
	size_t peak;
	size_t fail_allocation;
	unsigned reads;
	unsigned writes;
	unsigned barriers;
	unsigned fail_write;
	int fail_read;
	int fail_barrier;
};

static void *
allocate(void *context, size_t size)
{
	struct device *device = context;
	max_align_t *memory;

	if (++device->allocations == device->fail_allocation) {
		return NULL;
	}
	memory = malloc(sizeof(*memory) + size);
	REQUIRE(memory != NULL);
	*(size_t *)memory = size;
	device->allocated += size;
	if (device->allocated > device->peak) {
		device->peak = device->allocated;
	}
	return memory + 1;
}

static void
release(void *context, void *bytes, size_t size)
{
	struct device *device = context;
	max_align_t *memory = (max_align_t *)bytes - 1;

	REQUIRE(*(size_t *)memory == size && device->allocated >= size);
	device->allocated -= size;
	free(memory);
}

static enum btrfs_result
read_device(void *context, uint64_t offset, void *bytes, size_t length)
{
	struct device *device = context;

	REQUIRE(offset <= DEVICE_BYTES && length <= DEVICE_BYTES - offset);
	device->reads++;
	if (device->fail_read) {
		return BTRFS_IO;
	}
	memcpy(bytes, device->live + offset, length);
	return BTRFS_OK;
}

static enum btrfs_result
write_device(void *context, uint64_t offset, const void *bytes, size_t length)
{
	struct device *device = context;

	REQUIRE(offset <= DEVICE_BYTES && length <= DEVICE_BYTES - offset);
	if (++device->writes == device->fail_write) {
		/* A failed exact write may have changed part of the device. */
		memcpy(device->live + offset, bytes, length / 2U);
		return BTRFS_IO;
	}
	memcpy(device->live + offset, bytes, length);
	return BTRFS_OK;
}

static enum btrfs_result
flush_device(void *context)
{
	struct device *device = context;

	device->barriers++;
	if (device->fail_barrier) {
		return BTRFS_IO;
	}
	memcpy(device->stable, device->live, sizeof(device->stable));
	return BTRFS_OK;
}

static enum btrfs_result
open_staging(
    struct device *device, struct btrfs_staging_limits limits, struct btrfs_staging **staging)
{
	struct btrfs_environment reader = { .context = device,
		.size_bytes = DEVICE_BYTES,
		.read = read_device,
		.allocate = allocate,
		.release = release };
	struct btrfs_write_environment writer = {
		.context = device, .write = write_device, .flush = flush_device
	};

	return btrfs_staging_create(&reader, &writer, limits, staging);
}

static void
check_read(struct btrfs_staging *staging, const struct device *device, size_t offset, size_t size)
{
	uint8_t bytes[DEVICE_BYTES];

	REQUIRE(btrfs_staging_read(staging, offset, bytes, size) == BTRFS_OK);
	REQUIRE(memcmp(bytes, device->expected + offset, size) == 0);
}

static enum btrfs_result
change(
    struct btrfs_staging *staging, struct device *device, size_t offset, size_t size, uint8_t value)
{
	uint8_t bytes[DEVICE_BYTES];
	enum btrfs_result result;

	memset(bytes, value, size);
	result = btrfs_staging_write(staging, offset, bytes, size);
	if (result == BTRFS_OK) {
		memcpy(device->expected + offset, bytes, size);
	}
	return result;
}

static void
coalescing(void)
{
	struct device device = { 0 };
	struct btrfs_staging *staging;
	struct btrfs_staging_limits limits = { 4096, 4096, 16 };
	unsigned i;

	REQUIRE(open_staging(&device, limits, &staging) == BTRFS_OK);
	for (i = 0; i < 64; i++) {
		REQUIRE(change(staging, &device, i * 64U, 64, (uint8_t)i) == BTRFS_OK);
	}
	REQUIRE(device.writes == 0);
	device.fail_read = 1;
	check_read(staging, &device, 0, 4096);
	REQUIRE(device.reads == 0);
	device.fail_read = 0;
	/* A replacement inside a run neither drains nor allocates. */
	device.fail_allocation = device.allocations + 1U;
	REQUIRE(change(staging, &device, 123, 400, 0xfe) == BTRFS_OK);
	device.fail_allocation = 0;
	check_read(staging, &device, 1, 5000);
	REQUIRE(btrfs_staging_flush(staging) == BTRFS_OK);
	REQUIRE(device.writes == 1 && device.barriers == 1);
	REQUIRE(memcmp(device.stable, device.expected, DEVICE_BYTES) == 0);
	btrfs_staging_destroy(staging);
	REQUIRE(device.allocated == 0);
}

static void
overlap_and_bounds(void)
{
	struct device device = { 0 };
	struct btrfs_staging *staging;
	struct btrfs_staging_limits limits = { 256, 128, 2 };
	size_t base;
	uint8_t byte;

	REQUIRE(open_staging(&device, limits, &staging) == BTRFS_OK);
	base = device.allocated;
	REQUIRE(change(staging, &device, 100, 100, 1) == BTRFS_OK);
	REQUIRE(change(staging, &device, 0, 50, 2) == BTRFS_OK);
	check_read(staging, &device, 0, 256);
	/* Crossing two runs issues the previous values before the replacement. */
	REQUIRE(change(staging, &device, 40, 128, 3) == BTRFS_OK);
	REQUIRE(device.writes == 2 && device.barriers == 0);
	REQUIRE(change(staging, &device, 400, 128, 4) == BTRFS_OK);
	REQUIRE(change(staging, &device, 600, 128, 5) == BTRFS_OK);
	REQUIRE(device.writes == 4 && device.barriers == 0);
	/* A large write bypasses staging only after all its predecessors issue. */
	REQUIRE(change(staging, &device, 500, 1000, 6) == BTRFS_OK);
	REQUIRE(device.writes == 6 && device.barriers == 0);
	check_read(staging, &device, 0, DEVICE_BYTES);
	REQUIRE(btrfs_staging_read(staging, UINT64_MAX, &byte, 2) == BTRFS_INVALID_ARGUMENT);
	REQUIRE(btrfs_staging_write(staging, DEVICE_BYTES, &byte, 1) == BTRFS_INVALID_ARGUMENT);
	REQUIRE(btrfs_staging_write(staging, 0, NULL, 1) == BTRFS_INVALID_ARGUMENT);
	REQUIRE(btrfs_staging_write(staging, DEVICE_BYTES, NULL, 0) == BTRFS_OK);
	REQUIRE(btrfs_staging_flush(staging) == BTRFS_OK);
	REQUIRE(memcmp(device.stable, device.expected, DEVICE_BYTES) == 0);
	REQUIRE(device.peak <= base + 2U * limits.bytes + limits.run_bytes);
	btrfs_staging_destroy(staging);
	REQUIRE(device.allocated == 0);
}

static void
failures(void)
{
	struct device device;
	struct btrfs_staging *staging;
	struct btrfs_staging_limits limits = { 1024, 512, 8 };
	enum btrfs_result result;
	unsigned fault;
	unsigned i;
	unsigned writes;
	unsigned barriers;

	for (fault = 1; fault <= 10; fault++) {
		memset(&device, 0, sizeof(device));
		device.fail_allocation = fault;
		result = open_staging(&device, limits, &staging);
		for (i = 0; result == BTRFS_OK && i < 8; i++) {
			result = change(staging, &device, i * 64U, 64, (uint8_t)i);
		}
		REQUIRE(result == BTRFS_OK || result == BTRFS_NO_MEMORY);
		/* Destruction, including after an allocation failure, writes nothing. */
		btrfs_staging_destroy(staging);
		REQUIRE(device.allocated == 0 && device.writes == 0 && device.barriers == 0);
	}
	for (fault = 1; fault <= 4; fault++) {
		memset(&device, 0, sizeof(device));
		REQUIRE(open_staging(&device, limits, &staging) == BTRFS_OK);
		for (i = 0; i < 3; i++) {
			REQUIRE(change(staging, &device, i * 512U, 64, (uint8_t)i) == BTRFS_OK);
		}
		device.fail_write = fault;
		device.fail_barrier = fault == 4;
		REQUIRE(btrfs_staging_flush(staging) == BTRFS_IO);
		writes = device.writes;
		barriers = device.barriers;
		REQUIRE(barriers == (fault == 4 ? 1U : 0U));
		REQUIRE(change(staging, &device, 0, 64, 9) == BTRFS_IO);
		REQUIRE(btrfs_staging_flush(staging) == BTRFS_IO);
		REQUIRE(device.writes == writes && device.barriers == barriers);
		btrfs_staging_destroy(staging);
		REQUIRE(device.allocated == 0);
	}
}

static void
randomized(void)
{
	struct device device = { 0 };
	struct btrfs_staging *staging;
	struct btrfs_staging_limits limits = { 1024, 256, 8 };
	uint32_t random = 0x4872a103;
	size_t offset;
	size_t size;
	unsigned i;

	REQUIRE(open_staging(&device, limits, &staging) == BTRFS_OK);
	for (i = 0; i < 10000; i++) {
		random = random * 1664525U + 1013904223U;
		offset = (random >> 16) % (DEVICE_BYTES - 512U);
		size = (random >> 4) % 512U + 1U;
		REQUIRE(change(staging, &device, offset, size, (uint8_t)random) == BTRFS_OK);
		check_read(staging, &device, offset / 2U, DEVICE_BYTES - offset / 2U);
		if (i % 31U == 0) {
			REQUIRE(btrfs_staging_flush(staging) == BTRFS_OK);
			REQUIRE(memcmp(device.stable, device.expected, DEVICE_BYTES) == 0);
		}
	}
	REQUIRE(btrfs_staging_flush(staging) == BTRFS_OK);
	REQUIRE(memcmp(device.stable, device.expected, DEVICE_BYTES) == 0);
	btrfs_staging_destroy(staging);
	REQUIRE(device.allocated == 0);
}

int
main(void)
{
	coalescing();
	overlap_and_bounds();
	failures();
	randomized();
	puts("staging: coalescing, overlaps, bounds, allocation/I/O failures and 10000 writes "
	     "passed");
	return 0;
}
