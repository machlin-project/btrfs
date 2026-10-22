/* SPDX-License-Identifier: BSD-3-Clause */
/* A volume Linux wrote across several devices (tests/prepare_multidev_linux.py:
 * RAID1, RAID1C3, RAID1C4, RAID10, RAID0, RAID5, RAID6 and single data over
 * two devices). Every file and symlink of Linux's manifest reads with its
 * mode, owner, size and SHA-256 or target; the reference audit, which reads
 * the checksummed sectors of every copy on its device, and the namespace
 * audit pass; it reads every copy, RAID5/6 rebuilds from parity included.
 * Mounted from each of its devices, with the others in reverse order, the
 * volume reads the same. Without one of its devices a mount is refused,
 * unless degraded: then every file reads as before where each chunk keeps a
 * copy or its parity, and otherwise fails to read (IO) but never reads other
 * bytes; volumes whose chunks all survive two lost devices are read without
 * each pair too. A device of another volume among them is CORRUPT, and a
 * writer without the further devices is refused before any write
 * (tests/multidev_write.c writes them). */
#include "../adapters/posix/image.h"
#include "internal.h"
#include "namespace_audit.h"
#include "references.h"
#include <btrfs/write.h>
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

#define MANIFEST_LINE 512U
#define MANIFEST_FIELDS 3U
#define MANIFEST_PREFIX "./"
#define SHA256_HEX (2U * BT_SHA256_DIGEST)
#define MODE_TYPE 0170000U
#define MODE_SYMLINK 0120000U
#define MAX_DEVICES 16U

static enum btrfs_result
refused_write(void *context, uint64_t offset, const void *bytes, size_t length)
{
	(void)context;
	(void)offset;
	(void)bytes;
	(void)length;
	abort();
}

static enum btrfs_result
refused_flush(void *context)
{
	(void)context;
	abort();
}

static void
hex_digest(const uint8_t *bytes, size_t size, char hex[SHA256_HEX + 1])
{
	uint8_t digest[BT_SHA256_DIGEST];
	struct bt_sha2 hash;
	size_t i;

	bt_sha2_init(&hash, 0);
	bt_sha2_update(&hash, bytes, size);
	bt_sha2_final(&hash, digest);
	for (i = 0; i < BT_SHA256_DIGEST; i++) {
		snprintf(hex + 2 * i, 3, "%02x", digest[i]);
	}
}

/* Every manifest entry reads as Linux wrote it; returns their number. With
 * lossy set, an entry may fail to read (IO) instead, counted in *lost. */
static size_t
check_manifest(struct btrfs_fs *fs, const char *path, int lossy, size_t *lost)
{
	char line[MANIFEST_LINE];
	char name[MANIFEST_LINE];
	char *fields[MANIFEST_FIELDS];
	char hex[SHA256_HEX + 1];
	struct btrfs_inode inode;
	unsigned long mode;
	unsigned long uid;
	unsigned long gid;
	unsigned long long size;
	uint8_t *bytes;
	size_t completed;
	size_t count = 0;
	size_t i;
	FILE *stream;
	enum btrfs_result result;

	stream = fopen(path, "r");
	REQUIRE(stream != NULL);
	while (fgets(line, sizeof(line), stream) != NULL) {
		REQUIRE(strlen(line) > 0 && line[strlen(line) - 1] == '\n');
		line[strlen(line) - 1] = '\0';
		fields[0] = line;
		for (i = 1; i < MANIFEST_FIELDS; i++) {
			fields[i] = strchr(fields[i - 1], '\t');
			REQUIRE(fields[i] != NULL);
			*fields[i]++ = '\0';
		}
		REQUIRE(strncmp(fields[0], MANIFEST_PREFIX, strlen(MANIFEST_PREFIX)) == 0);
		REQUIRE(sscanf(fields[1], "%lx:%lu:%lu:%llu", &mode, &uid, &gid, &size) == 4);
		REQUIRE(snprintf(name, sizeof(name), "/%s", fields[0] + strlen(MANIFEST_PREFIX)) <
		    (int)sizeof(name));
		if (btrfs_image_lookup(fs, name, &inode) != BTRFS_OK || inode.mode != mode ||
		    inode.uid != uid || inode.gid != gid || inode.size != size) {
			fprintf(stderr, "%s: mode %o uid %u gid %u size %llu\n", name, inode.mode,
			    inode.uid, inode.gid, (unsigned long long)inode.size);
			exit(1);
		}
		bytes = malloc((size_t)size + 1);
		REQUIRE(bytes != NULL);
		result = btrfs_read(fs, &inode, 0, bytes, (size_t)size, &completed);
		if (lossy && result == BTRFS_IO) {
			(*lost)++;
			free(bytes);
			count++;
			continue;
		}
		REQUIRE(result == BTRFS_OK && completed == size);
		if ((mode & MODE_TYPE) == MODE_SYMLINK) {
			REQUIRE(strlen(fields[2]) == size && memcmp(bytes, fields[2], size) == 0);
		} else {
			hex_digest(bytes, (size_t)size, hex);
			if (strcmp(hex, fields[2]) != 0) {
				fprintf(stderr, "%s: content differs\n", name);
				exit(1);
			}
		}
		free(bytes);
		count++;
	}
	REQUIRE(fclose(stream) == 0);
	return count;
}

/* How many lost devices every chunk of the volume survives: its copies but
 * one, RAID10's pairs one, RAID5's parity one and RAID6's two. */
static unsigned
survivable(const struct btrfs_fs *fs)
{
	uint64_t profile;
	unsigned lowest = UINT32_MAX;
	unsigned chunk;
	size_t i;

	for (i = 0; i < fs->chunk_count; i++) {
		profile = fs->chunks[i].type & BT_BLOCK_PROFILES;
		chunk = profile == BT_BLOCK_RAID1 || profile == BT_BLOCK_RAID1C3 ||
			profile == BT_BLOCK_RAID1C4
		    ? fs->chunks[i].stripes - 1U
		    : profile == BT_BLOCK_RAID10 || profile == BT_BLOCK_RAID5 ? 1U
		    : profile == BT_BLOCK_RAID6				      ? 2U
									      : 0U;
		lowest = chunk < lowest ? chunk : lowest;
	}
	return lowest;
}

/* Mounts the volume degraded without the devices missing marks, from the
 * first remaining one; returns the files that failed to read. */
static size_t
check_degraded(char **images, size_t devices, const int *missing, int complete,
    const char *manifest, const struct namespace_digest *expected)
{
	const char *paths[MAX_DEVICES];
	struct namespace_digest digest;
	struct btrfs_image image;
	struct btrfs_fs *fs;
	size_t count = 0;
	size_t lost = 0;
	size_t i;

	for (i = 0; i < devices; i++) {
		if (!missing[i]) {
			paths[count++] = images[i];
		}
	}
	REQUIRE(btrfs_image_open_devices(paths, count, &image) == 0);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_UNSUPPORTED);
	image.environment.degraded = 1;
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	(void)check_manifest(fs, manifest, !complete, &lost);
	if (complete) {
		REQUIRE(namespace_content_digest(fs, &digest) == 0 &&
		    digest.hash == expected->hash && digest.objects == expected->objects &&
		    digest.bytes == expected->bytes);
	}
	btrfs_unmount(fs);
	REQUIRE(image.live_allocations == 0);
	btrfs_image_close(&image);
	return lost;
}

int
main(int argc, char **argv)
{
	const char *paths[MAX_DEVICES];
	struct btrfs_write_environment refused = { NULL, refused_write, refused_flush, NULL,
		BTRFS_COMPRESSION_NONE, 0, NULL, 0 };
	struct reference_audit references;
	struct namespace_audit names;
	struct namespace_digest digest;
	struct namespace_digest other;
	struct btrfs_transaction *transaction = NULL;
	struct btrfs_image image;
	struct btrfs_image foreign;
	struct btrfs_fs *fs;
	int missing[MAX_DEVICES] = { 0 };
	size_t devices;
	size_t files;
	size_t lost = 0;
	size_t degraded = 0;
	size_t pairs = 0;
	size_t i;
	size_t j;
	unsigned survives;

	REQUIRE(argc >= 5);
	devices = (size_t)(argc - 3);
	REQUIRE(devices <= MAX_DEVICES);
	for (i = 0; i < devices; i++) {
		paths[i] = argv[3 + i];
	}
	REQUIRE(btrfs_image_open_devices(paths, devices, &image) == 0);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(fs->device_count == devices);
	files = check_manifest(fs, argv[1], 0, &lost);
	survives = survivable(fs);
	if (reference_audit(fs, &references) != 0) {
		fprintf(stderr, "reference audit: %s\n", references.failure);
		exit(1);
	}
	if (namespace_audit(fs, &names) != 0) {
		fprintf(stderr, "namespace audit: %s\n", names.failure);
		exit(1);
	}
	REQUIRE(namespace_content_digest(fs, &digest) == 0);
	REQUIRE(btrfs_transaction_begin(fs, &refused, &transaction) == BTRFS_INVALID_ARGUMENT);
	btrfs_unmount(fs);
	REQUIRE(image.live_allocations == 0);
	btrfs_image_close(&image);

	/* From each device, the others in reverse order. */
	for (i = 0; i < devices; i++) {
		paths[0] = argv[3 + i];
		for (j = 1; j < devices; j++) {
			paths[j] = argv[3 + (i + devices - j) % devices];
		}
		REQUIRE(btrfs_image_open_devices(paths, devices, &image) == 0);
		REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
		REQUIRE(namespace_content_digest(fs, &other) == 0 && other.hash == digest.hash &&
		    other.objects == digest.objects && other.bytes == digest.bytes);
		btrfs_unmount(fs);
		btrfs_image_close(&image);
	}

	/* Without each device, then without each pair where every chunk
	 * survives two lost devices. */
	for (i = 0; i < devices; i++) {
		missing[i] = 1;
		lost += check_degraded(argv + 3, devices, missing, survives >= 1, argv[1], &digest);
		missing[i] = 0;
		degraded++;
	}
	REQUIRE(survives >= 1 || lost > 0);
	for (i = 0; survives >= 2 && i < devices; i++) {
		for (j = i + 1; j < devices; j++) {
			missing[i] = 1;
			missing[j] = 1;
			REQUIRE(
			    check_degraded(argv + 3, devices, missing, 1, argv[1], &digest) == 0);
			missing[i] = 0;
			missing[j] = 0;
			pairs++;
		}
	}

	/* A device of another volume in place of the last. */
	for (i = 0; i < devices; i++) {
		paths[i] = argv[3 + i];
	}
	paths[devices - 1] = argv[2];
	REQUIRE(btrfs_image_open_devices(paths, devices, &foreign) == 0);
	REQUIRE(btrfs_mount(&foreign.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_CORRUPT);
	REQUIRE(foreign.live_allocations == 0);
	btrfs_image_close(&foreign);
	printf("multi-device: %zu devices, %zu files read as Linux wrote them; both audits pass "
	       "(%zu checksummed copies); mounted from each device the volume reads the same; "
	       "degraded without each of %zu devices (%zu files lost in all) and each of %zu "
	       "pairs the volume reads as before where its chunks survive; a foreign device "
	       "and a writer without the further devices are refused PASS\n",
	    devices, files, references.checked_copies, degraded, lost, pairs);
	return 0;
}
