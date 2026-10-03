/* SPDX-License-Identifier: BSD-3-Clause */
/* The same POSIX durability workload on Linux and macOS. Every acknowledged
 * file has passed write, fsync(file), close and fsync(directory). The caller
 * supplies a dedicated existing directory; O_EXCL refuses previous output.
 * Verification runs outside the measured interval. No files are removed. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_FILES 100000U
#define MAX_FILE_BYTES (4U * 1024U * 1024U)
#define NS_PER_SECOND UINT64_C(1000000000)

enum phase { CREATE, WRITE, FILE_SYNC, CLOSE, DIRECTORY_SYNC, TOTAL, PHASES };

static const char *const names[PHASES] = { "create", "write", "file_fsync", "close",
	"directory_fsync", "total" };

struct sample {
	uint64_t start_ns;
	uint64_t ns[PHASES];
};

static void
require(int condition, const char *operation)
{
	if (!condition) {
		fprintf(stderr, "%s: %s\n", operation, strerror(errno));
		exit(1);
	}
}

static uint64_t
now(clockid_t clock)
{
	struct timespec time;

	require(clock_gettime(clock, &time) == 0, "clock_gettime");
	return (uint64_t)time.tv_sec * NS_PER_SECOND + (uint64_t)time.tv_nsec;
}

static size_t
number(const char *text, size_t maximum)
{
	char *end;
	uintmax_t value;

	errno = 0;
	value = strtoumax(text, &end, 10);
	if (text[0] < '0' || text[0] > '9' || *end != '\0' || errno != 0 || value == 0 ||
	    value > maximum) {
		fprintf(stderr, "invalid bounded argument: %s\n", text);
		exit(2);
	}
	return (size_t)value;
}

static void
fill(uint8_t *buffer, size_t bytes, size_t file)
{
	uint64_t state = (uint64_t)file + UINT64_C(0x9e3779b97f4a7c15);
	size_t i;

	for (i = 0; i < bytes; i++) {
		if (i % 8 == 0) {
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
		}
		buffer[i] = (uint8_t)(state >> (8 * (i % 8)));
	}
}

static void
write_all(int descriptor, const uint8_t *buffer, size_t bytes)
{
	size_t done = 0;
	ssize_t result;

	while (done < bytes) {
		result = write(descriptor, buffer + done, bytes - done);
		if (result < 0 && errno == EINTR) {
			continue;
		}
		if (result == 0) {
			errno = EIO;
		}
		require(result > 0, "write");
		done += (size_t)result;
	}
}

static void
verify(int directory, uint8_t *expected, uint8_t *actual, size_t files, size_t bytes)
{
	struct stat status;
	char name[48];
	size_t i;
	size_t done;
	ssize_t result;
	int descriptor;

	for (i = 0; i < files; i++) {
		(void)snprintf(name, sizeof(name), "fsync-%08zu", i);
		descriptor = openat(directory, name, O_RDONLY | O_NOFOLLOW);
		require(descriptor >= 0, "verify openat");
		require(fstat(descriptor, &status) == 0, "verify fstat");
		if (!S_ISREG(status.st_mode) || status.st_size != (off_t)bytes) {
			errno = EIO;
			require(0, "verify type/size");
		}
		for (done = 0; done < bytes; done += (size_t)result) {
			result = read(descriptor, actual + done, bytes - done);
			if (result < 0 && errno == EINTR) {
				result = 0;
				continue;
			}
			if (result == 0) {
				errno = EIO;
			}
			require(result > 0, "verify read");
		}
		require(close(descriptor) == 0, "verify close");
		fill(expected, bytes, i);
		if (memcmp(expected, actual, bytes) != 0) {
			errno = EIO;
			require(0, "verify contents");
		}
	}
}

static int
compare(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static void
report(const struct sample *samples, size_t count, uint64_t *sorted)
{
	size_t i;
	unsigned phase;
	double sum;

	for (phase = 0; phase < PHASES; phase++) {
		sum = 0;
		for (i = 0; i < count; i++) {
			sorted[i] = samples[i].ns[phase];
			sum += (double)sorted[i];
		}
		qsort(sorted, count, sizeof(*sorted), compare);
		printf("%s\"%s\":{\"mean_us\":%.3f,\"p50_us\":%.3f,\"p95_us\":%.3f,"
		       "\"p99_us\":%.3f,\"max_us\":%.3f}",
		    phase == 0 ? "" : ",", names[phase], sum / (double)count / 1000.0,
		    (double)sorted[(count * 50 + 99) / 100 - 1] / 1000.0,
		    (double)sorted[(count * 95 + 99) / 100 - 1] / 1000.0,
		    (double)sorted[(count * 99 + 99) / 100 - 1] / 1000.0,
		    (double)sorted[count - 1] / 1000.0);
	}
}

int
main(int argc, char **argv)
{
	struct sample *samples;
	uint64_t *sorted;
	uint8_t *buffer;
	uint8_t *actual;
	uint64_t start;
	uint64_t mark;
	uint64_t end;
	uint64_t wall_start;
	uint64_t wall_ns;
	uint64_t cpu_start;
	uint64_t cpu_ns;
	size_t files;
	size_t bytes;
	size_t i;
	unsigned phase;
	char name[48];
	int directory;
	int descriptor;
	int print_samples;

	if (argc < 4 || argc > 5 || (argc == 5 && strcmp(argv[4], "--samples") != 0)) {
		fprintf(stderr, "usage: %s DIRECTORY FILES BYTES [--samples]\n", argv[0]);
		return 2;
	}
	files = number(argv[2], MAX_FILES);
	bytes = number(argv[3], MAX_FILE_BYTES);
	print_samples = argc == 5;
	samples = calloc(files, sizeof(*samples));
	sorted = calloc(files, sizeof(*sorted));
	buffer = malloc(bytes);
	actual = malloc(bytes);
	require(samples != NULL && sorted != NULL && buffer != NULL && actual != NULL, "allocate");
	directory = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	require(directory >= 0, "open directory");
	cpu_start = now(CLOCK_PROCESS_CPUTIME_ID);
	wall_start = now(CLOCK_MONOTONIC);
	for (i = 0; i < files; i++) {
		fill(buffer, bytes, i);
		(void)snprintf(name, sizeof(name), "fsync-%08zu", i);
		start = now(CLOCK_MONOTONIC);
		samples[i].start_ns = start;
		descriptor =
		    openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
		require(descriptor >= 0, "create");
		mark = now(CLOCK_MONOTONIC);
		samples[i].ns[CREATE] = mark - start;
		write_all(descriptor, buffer, bytes);
		end = now(CLOCK_MONOTONIC);
		samples[i].ns[WRITE] = end - mark;
		require(fsync(descriptor) == 0, "fsync file");
		mark = now(CLOCK_MONOTONIC);
		samples[i].ns[FILE_SYNC] = mark - end;
		require(close(descriptor) == 0, "close");
		end = now(CLOCK_MONOTONIC);
		samples[i].ns[CLOSE] = end - mark;
		require(fsync(directory) == 0, "fsync directory");
		mark = now(CLOCK_MONOTONIC);
		samples[i].ns[DIRECTORY_SYNC] = mark - end;
		samples[i].ns[TOTAL] = mark - start;
	}
	wall_ns = now(CLOCK_MONOTONIC) - wall_start;
	cpu_ns = now(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
	verify(directory, buffer, actual, files, bytes);
	require(close(directory) == 0, "close directory");
	/* No output or verification I/O is included in the timing. This program
	 * verifies a live mount, not crash survival or the device's flush contract. */
	if (print_samples) {
		for (i = 0; i < files; i++) {
			printf("{\"sample\":%zu,\"start_ns\":%" PRIu64 ",\"ns\":{", i,
			    samples[i].start_ns);
			for (phase = 0; phase < PHASES; phase++) {
				printf("%s\"%s\":%" PRIu64, phase == 0 ? "" : ",", names[phase],
				    samples[i].ns[phase]);
			}
			printf("}}\n");
		}
	}
	printf("{\"workload\":\"create-write-fsync-file-and-directory\",\"files\":%zu,"
	       "\"file_bytes\":%zu,\"start_ns\":%" PRIu64 ",\"end_ns\":%" PRIu64
	       ",\"wall_ns\":%" PRIu64 ",\"caller_cpu_ns\":%" PRIu64
	       ",\"files_per_second\":%.3f,\"verified_files\":%zu,\"latency\":{",
	    files, bytes, wall_start, wall_start + wall_ns, wall_ns, cpu_ns,
	    (double)files * 1e9 / (double)wall_ns, files);
	report(samples, files, sorted);
	printf("}}\n");
	free(actual);
	free(buffer);
	free(sorted);
	free(samples);
	return 0;
}
