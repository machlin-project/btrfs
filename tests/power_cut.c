/* SPDX-License-Identifier: BSD-3-Clause */
/* Guest workload for native power-cut acceptance on a disposable mounted image.
 * "write MOUNT RUN" creates MOUNT/pc/run-RUN/f-N from N = 0 on until it is
 * killed: each file gets deterministic contents, fsync of the file and of its
 * directory, and then one acknowledgement line on stdout, already in the Linux
 * native-check manifest format (file, path, SHA-256, mode, uid, gid, links).
 * Each run has its own directory: a cut can leave files of a run beyond its
 * last acknowledgement.
 * Between acknowledgements it churns MOUNT/pc-churn without fsync: writes,
 * renames, unlinks and files kept open after their last name, so a cut finds
 * unacknowledged changes and orphans in every state. "verify MOUNT ACKS"
 * checks every acknowledged file after a cut, from a manifest of such lines. */
#define _DARWIN_C_SOURCE
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition,  \
			    errno);                                                                \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define ACKED_DIRECTORY "pc"
#define CHURN_DIRECTORY "pc-churn"
#define FILE_MODE 0644U
/* Largest acknowledged and churned file: inline, single-extent and
 * multi-extent sizes all occur (see file_size). */
#define MAX_FILE_BYTES (512U * 1024U)
#define INLINE_LIMIT 2048U
#define SMALL_LIMIT (64U * 1024U)
#define CHURN_MIN_BYTES (16U * 1024U)
/* Files kept open after their last name; the oldest closes first. */
#define ORPHANS 4U
#define RENAME_PERIOD 3U
#define UNLINK_PERIOD 5U
#define UNLINK_DISTANCE 5U
#define ORPHAN_PERIOD 7U
#define NAME_BYTES 64U
#define LINE_BYTES 512U
#define HEX_BYTES (CC_SHA256_DIGEST_LENGTH * 2 + 1)

static uint64_t
mix(uint64_t value)
{
	value ^= value >> 33;
	value *= UINT64_C(0xff51afd7ed558ccd);
	value ^= value >> 33;
	value *= UINT64_C(0xc4ceb9fe1a85ec53);
	return value ^ (value >> 33);
}

/* Sixteen classes: six inline-sized, seven up to 64 KiB, three up to 512 KiB. */
static size_t
file_size(uint32_t n)
{
	uint64_t hash = mix(n);
	unsigned class = (unsigned)(hash % 16U);

	if (class < 6U) {
		return 1U + (size_t)((hash >> 8) % INLINE_LIMIT);
	}
	if (class < 13U) {
		return INLINE_LIMIT + (size_t)((hash >> 8) % (SMALL_LIMIT - INLINE_LIMIT));
	}
	return SMALL_LIMIT + (size_t)((hash >> 8) % (MAX_FILE_BYTES - SMALL_LIMIT));
}

static void
fill(uint8_t *bytes, size_t size, uint64_t seed)
{
	uint64_t state = mix(seed) | 1U;
	size_t i;

	for (i = 0; i < size; i++) {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		bytes[i] = (uint8_t)state;
	}
}

static void
digest(const uint8_t *bytes, size_t size, char text[HEX_BYTES])
{
	unsigned char hash[CC_SHA256_DIGEST_LENGTH];
	size_t i;

	CC_SHA256(bytes, (CC_LONG)size, hash);
	for (i = 0; i < sizeof(hash); i++) {
		snprintf(text + i * 2, 3, "%02x", hash[i]);
	}
}

/* Writes all of bytes; ENOSPC (or another failure) returns the errno. */
static int
write_all(int file, const uint8_t *bytes, size_t size)
{
	ssize_t done;

	while (size != 0) {
		done = write(file, bytes, size);
		if (done < 0) {
			return errno;
		}
		REQUIRE(done != 0);
		bytes += done;
		size -= (size_t)done;
	}
	return 0;
}

static void
make_directory(int root, const char *name)
{
	REQUIRE(mkdirat(root, name, 0755) == 0 || errno == EEXIST);
}

/* Unsynchronized changes around acknowledgement n; failures from a full
 * device are part of the workload, not of its contract. */
static void
churn(int directory, uint32_t n, uint8_t *bytes, int orphans[ORPHANS])
{
	char name[NAME_BYTES];
	char renamed[NAME_BYTES];
	size_t size =
	    CHURN_MIN_BYTES + (size_t)(mix(~(uint64_t)n) % (MAX_FILE_BYTES - CHURN_MIN_BYTES));
	int file;

	snprintf(name, sizeof(name), "c-%08u", n);
	file = openat(directory, name, O_CREAT | O_TRUNC | O_WRONLY, FILE_MODE);
	if (file < 0) {
		return;
	}
	fill(bytes, size, ~(uint64_t)n);
	(void)write_all(file, bytes, size);
	if (n % ORPHAN_PERIOD == 0) {
		if (orphans[0] >= 0) {
			REQUIRE(close(orphans[0]) == 0);
		}
		memmove(orphans, orphans + 1, (ORPHANS - 1) * sizeof(*orphans));
		orphans[ORPHANS - 1] = file;
		(void)unlinkat(directory, name, 0);
	} else {
		REQUIRE(close(file) == 0);
	}
	if (n >= 1 && n % RENAME_PERIOD == 0) {
		snprintf(name, sizeof(name), "c-%08u", n - 1);
		snprintf(renamed, sizeof(renamed), "r-%08u", n - 1);
		(void)renameat(directory, name, directory, renamed);
	}
	if (n >= UNLINK_DISTANCE && n % UNLINK_PERIOD == 0) {
		snprintf(renamed, sizeof(renamed), "r-%08u", n - UNLINK_DISTANCE);
		(void)unlinkat(directory, renamed, 0);
		snprintf(name, sizeof(name), "c-%08u", n - UNLINK_DISTANCE);
		(void)unlinkat(directory, name, 0);
	}
}

static int
write_phase(int root, uint32_t run)
{
	char directory[NAME_BYTES];
	char name[NAME_BYTES];
	char path[NAME_BYTES];
	char hex[HEX_BYTES];
	struct stat status;
	uint8_t *bytes = malloc(MAX_FILE_BYTES);
	int orphans[ORPHANS] = { -1, -1, -1, -1 };
	int acked;
	int churned;
	int file;
	int error;
	uint32_t n;
	size_t size;

	REQUIRE(bytes != NULL);
	make_directory(root, ACKED_DIRECTORY);
	make_directory(root, CHURN_DIRECTORY);
	snprintf(directory, sizeof(directory), ACKED_DIRECTORY "/run-%02u", run);
	REQUIRE(mkdirat(root, directory, 0755) == 0);
	acked = openat(root, directory, O_RDONLY | O_DIRECTORY);
	churned = openat(root, CHURN_DIRECTORY, O_RDONLY | O_DIRECTORY);
	REQUIRE(acked >= 0 && churned >= 0);
	REQUIRE(fsync(root) == 0);
	for (n = 0;; n++) {
		snprintf(name, sizeof(name), "f-%08u", n);
		size = file_size(n);
		fill(bytes, size, (uint64_t)run << 32 | n);
		file = openat(acked, name, O_CREAT | O_EXCL | O_WRONLY, FILE_MODE);
		REQUIRE(file >= 0);
		error = write_all(file, bytes, size);
		if (error == 0 && fsync(file) != 0) {
			error = errno;
		}
		if (error != 0) {
			/* A full device ends the acknowledged sequence; wait for the cut. */
			REQUIRE(error == ENOSPC);
			fprintf(stderr, "power-cut workload: device full at %u\n", n);
			fflush(stderr);
			for (;;) {
				pause();
			}
		}
		REQUIRE(fstat(file, &status) == 0 && close(file) == 0);
		REQUIRE(fsync(acked) == 0);
		digest(bytes, size, hex);
		snprintf(path, sizeof(path), "%s/%s", directory, name);
		/* Printed only after both fsyncs returned: Linux must find it. */
		printf("file\t%s\t%s\t%o\t%u\t%u\t%u\n", path, hex,
		    (unsigned)(status.st_mode & 07777), (unsigned)status.st_uid,
		    (unsigned)status.st_gid, (unsigned)status.st_nlink);
		fflush(stdout);
		churn(churned, run * 100000U + n, bytes, orphans);
	}
	return 0;
}

static int
verify_phase(int root, const char *manifest)
{
	char line[LINE_BYTES];
	char path[LINE_BYTES];
	char expected[HEX_BYTES];
	char actual[HEX_BYTES];
	struct stat status;
	FILE *input = fopen(manifest, "r");
	uint8_t *bytes = malloc(MAX_FILE_BYTES + 1);
	unsigned mode;
	unsigned uid;
	unsigned gid;
	unsigned links;
	unsigned count = 0;
	ssize_t done;
	int file;

	REQUIRE(input != NULL && bytes != NULL);
	while (fgets(line, sizeof(line), input) != NULL) {
		REQUIRE(sscanf(line, "file\t%511s\t%64s\t%o\t%u\t%u\t%u", path, expected, &mode,
			    &uid, &gid, &links) == 6);
		file = openat(root, path, O_RDONLY);
		if (file < 0) {
			fprintf(stderr, "power-cut verify: acknowledged %s is missing\n", path);
			exit(1);
		}
		REQUIRE(fstat(file, &status) == 0 && (size_t)status.st_size <= MAX_FILE_BYTES);
		done = pread(file, bytes, MAX_FILE_BYTES + 1, 0);
		REQUIRE(done == status.st_size && close(file) == 0);
		digest(bytes, (size_t)done, actual);
		if (strcmp(actual, expected) != 0 || (status.st_mode & 07777) != mode ||
		    status.st_uid != uid || status.st_gid != gid || status.st_nlink != links) {
			fprintf(stderr, "power-cut verify: acknowledged %s differs\n", path);
			exit(1);
		}
		count++;
	}
	REQUIRE(ferror(input) == 0 && fclose(input) == 0);
	fprintf(stderr, "power-cut verify: %u acknowledged files PASS\n", count);
	return 0;
}

int
main(int argc, char **argv)
{
	char *end;
	unsigned long run;
	int root;

	if (argc != 4 || geteuid() != 0 ||
	    (strcmp(argv[1], "write") != 0 && strcmp(argv[1], "verify") != 0)) {
		fprintf(stderr,
		    "usage (guest root): btrfs-power-cut write MOUNT RUN | "
		    "verify MOUNT ACKS\n");
		return 2;
	}
	root = open(argv[2], O_RDONLY | O_DIRECTORY);
	REQUIRE(root >= 0);
	if (strcmp(argv[1], "verify") == 0) {
		return verify_phase(root, argv[3]);
	}
	errno = 0;
	run = strtoul(argv[3], &end, 10);
	REQUIRE(errno == 0 && *end == '\0' && run < 100U);
	return write_phase(root, (uint32_t)run);
}
