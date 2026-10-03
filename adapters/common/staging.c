/* SPDX-License-Identifier: BSD-3-Clause */
#include <btrfs/staging.h>

struct staging_run {
	uint64_t offset;
	size_t length;
	size_t capacity;
	uint8_t *bytes;
};

/* Offset-sorted disjoint runs. Insertion moves at most limits.runs records. */
struct btrfs_staging {
	struct btrfs_environment reader;
	struct btrfs_write_environment writer;
	struct btrfs_staging_limits limits;
	size_t count;
	size_t bytes;
	enum btrfs_result failure;
	struct staging_run runs[];
};

enum btrfs_result
btrfs_staging_create(const struct btrfs_environment *reader,
    const struct btrfs_write_environment *writer, struct btrfs_staging_limits limits,
    struct btrfs_staging **result)
{
	struct btrfs_staging *staging;
	size_t size;

	if (result == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (reader == NULL || reader->read == NULL || reader->allocate == NULL ||
	    reader->release == NULL || writer == NULL || writer->write == NULL ||
	    writer->flush == NULL || limits.bytes == 0 || limits.bytes > BTRFS_STAGING_MAX_BYTES ||
	    limits.run_bytes == 0 || limits.run_bytes > BTRFS_STAGING_MAX_RUN_BYTES ||
	    limits.run_bytes > limits.bytes || limits.runs == 0 ||
	    limits.runs > BTRFS_STAGING_MAX_RUNS) {
		return BTRFS_INVALID_ARGUMENT;
	}
	size = sizeof(*staging) + limits.runs * sizeof(staging->runs[0]);
	staging = reader->allocate(reader->context, size);
	if (staging == NULL) {
		return BTRFS_NO_MEMORY;
	}
	staging->reader = *reader;
	staging->writer = *writer;
	staging->limits = limits;
	staging->count = 0;
	staging->bytes = 0;
	staging->failure = BTRFS_OK;
	*result = staging;
	return BTRFS_OK;
}

static void
staging_discard(struct btrfs_staging *staging)
{
	size_t i;

	for (i = 0; i < staging->count; i++) {
		staging->reader.release(
		    staging->reader.context, staging->runs[i].bytes, staging->runs[i].capacity);
	}
	staging->count = 0;
	staging->bytes = 0;
}

void
btrfs_staging_destroy(struct btrfs_staging *staging)
{
	size_t size;

	if (staging == NULL) {
		return;
	}
	staging_discard(staging);
	size = sizeof(*staging) + staging->limits.runs * sizeof(staging->runs[0]);
	staging->reader.release(staging->reader.context, staging, size);
}

/* No barrier here: capacity/overlap drains may issue any part of this epoch.
 * Its publication still requires the owner's original persistence barriers. */
static enum btrfs_result
staging_drain(struct btrfs_staging *staging)
{
	struct staging_run *run;
	size_t i;

	for (i = 0; i < staging->count && staging->failure == BTRFS_OK; i++) {
		run = &staging->runs[i];
		staging->failure = staging->writer.write(
		    staging->writer.context, run->offset, run->bytes, run->length);
	}
	staging_discard(staging);
	return staging->failure;
}

/* First run with an end strictly greater than offset; logarithmic in runs. */
static size_t
staging_find(const struct btrfs_staging *staging, uint64_t offset)
{
	size_t low = 0;
	size_t high = staging->count;
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (staging->runs[middle].offset + staging->runs[middle].length <= offset) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return low;
}

enum btrfs_result
btrfs_staging_read(struct btrfs_staging *staging, uint64_t offset, void *bytes, size_t length)
{
	const struct staging_run *run;
	uint64_t end;
	uint64_t from;
	uint64_t to;
	size_t index;
	enum btrfs_result error;

	if (staging == NULL || offset > staging->reader.size_bytes ||
	    length > staging->reader.size_bytes - offset || (length != 0 && bytes == NULL)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (length == 0) {
		return BTRFS_OK;
	}
	end = offset + length;
	index = staging_find(staging, offset);
	run = index < staging->count ? &staging->runs[index] : NULL;
	if (run != NULL && run->offset <= offset && end <= run->offset + run->length) {
		__builtin_memcpy(bytes, run->bytes + (offset - run->offset), length);
		return BTRFS_OK;
	}
	error = staging->reader.read(staging->reader.context, offset, bytes, length);
	if (error == BTRFS_OK) {
		for (; index < staging->count && staging->runs[index].offset < end; index++) {
			run = &staging->runs[index];
			from = run->offset > offset ? run->offset : offset;
			to = run->offset + run->length;
			to = to < end ? to : end;
			__builtin_memcpy((uint8_t *)bytes + (from - offset),
			    run->bytes + (from - run->offset), (size_t)(to - from));
		}
	}
	return error;
}

enum btrfs_result
btrfs_staging_write(
    struct btrfs_staging *staging, uint64_t offset, const void *bytes, size_t length)
{
	struct staging_run *run;
	uint8_t *grown;
	uint64_t end;
	size_t index;
	size_t capacity;

	if (staging == NULL || offset > staging->reader.size_bytes ||
	    length > staging->reader.size_bytes - offset || (length != 0 && bytes == NULL)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (staging->failure != BTRFS_OK || length == 0) {
		return staging->failure;
	}
	if (length > staging->limits.run_bytes) {
		if (staging_drain(staging) == BTRFS_OK) {
			staging->failure =
			    staging->writer.write(staging->writer.context, offset, bytes, length);
		}
		return staging->failure;
	}
	end = offset + length;
	index = staging_find(staging, offset);
	if (index < staging->count && staging->runs[index].offset <= offset &&
	    end <= staging->runs[index].offset + staging->runs[index].length) {
		run = &staging->runs[index];
		__builtin_memcpy(run->bytes + (offset - run->offset), bytes, length);
		return BTRFS_OK;
	}
	if ((index < staging->count && staging->runs[index].offset < end) ||
	    length > staging->limits.bytes - staging->bytes ||
	    staging->count == staging->limits.runs) {
		if (staging_drain(staging) != BTRFS_OK) {
			return staging->failure;
		}
		index = 0;
	}
	if (index > 0 &&
	    staging->runs[index - 1].offset + staging->runs[index - 1].length == offset &&
	    length <= staging->limits.run_bytes - staging->runs[index - 1].length) {
		run = &staging->runs[index - 1];
		if (run->length + length > run->capacity) {
			capacity = run->capacity * 2U;
			capacity =
			    capacity > run->length + length ? capacity : run->length + length;
			capacity = capacity < staging->limits.run_bytes ? capacity
									: staging->limits.run_bytes;
			grown = staging->reader.allocate(staging->reader.context, capacity);
			if (grown == NULL) {
				return BTRFS_NO_MEMORY;
			}
			__builtin_memcpy(grown, run->bytes, run->length);
			staging->reader.release(staging->reader.context, run->bytes, run->capacity);
			run->bytes = grown;
			run->capacity = capacity;
		}
		__builtin_memcpy(run->bytes + run->length, bytes, length);
		run->length += length;
		staging->bytes += length;
		return BTRFS_OK;
	}
	grown = staging->reader.allocate(staging->reader.context, length);
	if (grown == NULL) {
		return BTRFS_NO_MEMORY;
	}
	__builtin_memcpy(grown, bytes, length);
	__builtin_memmove(&staging->runs[index + 1], &staging->runs[index],
	    (staging->count - index) * sizeof(staging->runs[0]));
	staging->runs[index] = (struct staging_run){ offset, length, length, grown };
	staging->count++;
	staging->bytes += length;
	return BTRFS_OK;
}

enum btrfs_result
btrfs_staging_flush(struct btrfs_staging *staging)
{
	if (staging == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (staging_drain(staging) == BTRFS_OK) {
		staging->failure = staging->writer.flush(staging->writer.context);
	}
	return staging->failure;
}
