/* SPDX-License-Identifier: BSD-3-Clause */
/* A volume Linux wrote across several devices (tests/prepare_multidev_linux.py:
 * RAID1, RAID1C3, RAID1C4, RAID10, RAID0, RAID5, RAID6 and single data over
 * two devices). Every file and symlink of Linux's manifest reads with its
 * mode, owner, size and SHA-256 or target; the reference audit, which reads
 * the checksummed sectors of every copy on its device, and the namespace
 * audit pass. Mounted from each of its devices, with the others in reverse
 * order, the volume reads the same. Without one of its devices a mount is
 * refused (degraded mounts are not supported), a device of another volume
 * among them is CORRUPT, and writable admission refuses the volume before
 * any write. */
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

/* Every manifest entry reads as Linux wrote it; returns their number. */
static size_t
check_manifest(struct btrfs_fs *fs, const char *path)
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
		REQUIRE(btrfs_read(fs, &inode, 0, bytes, (size_t)size, &completed) == BTRFS_OK &&
		    completed == size);
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

int
main(int argc, char **argv)
{
	const char *paths[MAX_DEVICES];
	struct btrfs_write_environment refused = { NULL, refused_write, refused_flush, NULL,
		BTRFS_COMPRESSION_NONE, 0 };
	struct reference_audit references;
	struct namespace_audit names;
	struct namespace_digest digest;
	struct namespace_digest other;
	struct btrfs_transaction *transaction = NULL;
	struct btrfs_image image;
	struct btrfs_image foreign;
	struct btrfs_fs *fs;
	size_t devices;
	size_t files;
	size_t i;
	size_t j;

	REQUIRE(argc >= 5);
	devices = (size_t)(argc - 3);
	REQUIRE(devices <= MAX_DEVICES);
	for (i = 0; i < devices; i++) {
		paths[i] = argv[3 + i];
	}
	REQUIRE(btrfs_image_open_devices(paths, devices, &image) == 0);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(fs->device_count == devices);
	files = check_manifest(fs, argv[1]);
	if (reference_audit(fs, &references) != 0) {
		fprintf(stderr, "reference audit: %s\n", references.failure);
		exit(1);
	}
	if (namespace_audit(fs, &names) != 0) {
		fprintf(stderr, "namespace audit: %s\n", names.failure);
		exit(1);
	}
	REQUIRE(namespace_content_digest(fs, &digest) == 0);
	REQUIRE(btrfs_transaction_begin(fs, &refused, &transaction) == BTRFS_UNSUPPORTED);
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

	/* Without its last device; with a device of another volume in its place. */
	for (i = 0; i < devices; i++) {
		paths[i] = argv[3 + i];
	}
	REQUIRE(btrfs_image_open_devices(paths, devices - 1, &image) == 0);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_UNSUPPORTED);
	btrfs_image_close(&image);
	paths[devices - 1] = argv[2];
	REQUIRE(btrfs_image_open_devices(paths, devices, &foreign) == 0);
	REQUIRE(btrfs_mount(&foreign.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_CORRUPT);
	REQUIRE(foreign.live_allocations == 0);
	btrfs_image_close(&foreign);
	printf("multi-device: %zu devices, %zu files read as Linux wrote them; both audits pass "
	       "(%zu checksummed copies); mounted from each device the volume reads the same; "
	       "a missing device, a foreign device and writable admission are refused PASS\n",
	    devices, files, references.checked_copies);
	return 0;
}
