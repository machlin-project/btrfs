/* SPDX-License-Identifier: BSD-3-Clause */
/* The recorded device: writes are kept per barrier epoch so that crash states
 * can make any subset of sectors visible. */
#define _POSIX_C_SOURCE 200809L
#include "scenario.h"

uint32_t
next_random(struct context *context)
{
	context->seed ^= context->seed << 13;
	context->seed ^= context->seed >> 17;
	context->seed ^= context->seed << 5;
	return context->seed;
}

size_t
sectors(const struct saved_write *write)
{
	return write->length / DEVICE_SECTOR;
}

void
show(struct saved_write *write, int visible)
{
	memset(write->visible, visible, sectors(write));
}

static void
overlay(void *bytes, uint64_t offset, size_t size, const struct saved_write *write, size_t sector,
    size_t count)
{
	uint64_t source = write->offset + sector * DEVICE_SECTOR;
	uint64_t start = offset > source ? offset : source;
	uint64_t end = offset + size < source + count * DEVICE_SECTOR
	    ? offset + size
	    : source + count * DEVICE_SECTOR;

	if (start < end) {
		memcpy((uint8_t *)bytes + (start - offset), write->bytes + (start - write->offset),
		    (size_t)(end - start));
	}
}

enum btrfs_result
read_device(void *context, uint64_t offset, void *bytes, size_t size)
{
	struct device *device = context;
	const struct saved_write *write;
	size_t i;
	size_t sector;
	size_t run;
	enum btrfs_result error;

	error = device->image->environment.read(
	    device->image->environment.context, offset, bytes, size);
	for (i = 0; error == BTRFS_OK && i < device->count; i++) {
		write = &device->writes[i];
		if (write->offset >= offset + size || offset >= write->offset + write->length) {
			continue;
		}
		for (sector = 0; sector < sectors(write); sector += run) {
			for (run = 1; sector + run < sectors(write) &&
			    write->visible[sector + run] == write->visible[sector];
			    run++) {
			}
			if (write->visible[sector] || device->coherent) {
				overlay(bytes, offset, size, write, sector, run);
			}
		}
	}
	return error;
}

enum btrfs_result
decompress_device(void *context, enum btrfs_compression codec, const void *input, size_t input_size,
    void *output, size_t capacity, size_t *produced)
{
	struct device *device = context;

	return device->image->environment.decompress(device->image->environment.context, codec,
	    input, input_size, output, capacity, produced);
}

void *
allocate(void *context, size_t size)
{
	struct device *device = context;

	return device->image->environment.allocate(device->image->environment.context, size);
}

void
release(void *context, void *bytes, size_t size)
{
	struct device *device = context;

	device->image->environment.release(device->image->environment.context, bytes, size);
}

struct saved_write *
record(struct device *device, uint64_t offset, const void *bytes, size_t size)
{
	struct saved_write *write;
	struct saved_write *grown;

	REQUIRE(offset % DEVICE_SECTOR == 0 && size % DEVICE_SECTOR == 0 && size != 0 &&
	    size <= MAX_WRITE);
	REQUIRE(offset <= device->image->environment.size_bytes &&
	    size <= device->image->environment.size_bytes - offset);
	if (device->count == device->capacity) {
		device->capacity = device->capacity == 0 ? 256 : device->capacity * 2;
		grown = realloc(device->writes, device->capacity * sizeof(*grown));
		REQUIRE(grown != NULL);
		device->writes = grown;
	}
	write = &device->writes[device->count++];
	memset(write, 0, sizeof(*write));
	write->offset = offset;
	write->length = size;
	write->bytes = malloc(size);
	REQUIRE(write->bytes != NULL);
	memcpy(write->bytes, bytes, size);
	write->commit = device->commit;
	write->epoch = device->epoch;
	return write;
}

enum btrfs_result
write_device(void *context, uint64_t offset, const void *bytes, size_t size)
{
	struct device *device = context;
	struct saved_write *write;

	device->issued++;
	if (device->issued == device->fail_write) {
		return BTRFS_IO;
	}
	write = record(device, offset, bytes, size);
	if (device->immediate) {
		show(write, 1);
	}
	return BTRFS_OK;
}

enum btrfs_result
flush_device(void *context)
{
	struct device *device = context;

	device->flushes++;
	if (device->flushes == device->fail_flush) {
		return BTRFS_IO;
	}
	for (; device->durable < device->count; device->durable++) {
		show(&device->writes[device->durable], 1);
	}
	device->epoch++;
	return BTRFS_OK;
}

void
truncate_writes(struct device *device, size_t count)
{
	while (device->count > count) {
		free(device->writes[--device->count].bytes);
	}
	if (device->durable > count) {
		device->durable = count;
	}
}

void
synthetic(struct context *context, uint64_t offset, const void *bytes, size_t size)
{
	size_t commit = context->device->commit;

	context->device->commit = SYNTHETIC_COMMIT;
	show(record(context->device, offset, bytes, size), 1);
	context->device->commit = commit;
	context->device->durable = context->device->count;
}

void
read_exact(struct context *context, uint64_t offset, void *bytes, size_t size)
{
	REQUIRE(context->env.read(context->env.context, offset, bytes, size) == BTRFS_OK);
}
