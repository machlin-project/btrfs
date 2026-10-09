/* SPDX-License-Identifier: BSD-3-Clause */
/* An interrupted balance (tests/prepare_relocation_linux.py): Linux powered off
 * while its committed root tree held relocation trees, which only a read-write
 * Linux mount merges. The reader reads every file as Linux's manifest says,
 * the reference and namespace audits pass, and writable admission refuses
 * the volume (UNSUPPORTED), natively too, without a write or a flush; a
 * read-only native volume opens. */
#include "../adapters/posix/image.h"
#include "internal.h"
#include "namespace_audit.h"
#include "references.h"
#include <btrfs/volume.h>
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

/* Linux's relocation tree objectid (BTRFS_TREE_RELOC_OBJECTID, -8). */
#define TREE_RELOC_OBJECTID (UINT64_MAX - UINT64_C(7))
#define MANIFEST_LINE 512U
#define MANIFEST_FIELDS 3U
#define SHA256_HEX (2U * BT_SHA256_DIGEST)
/* The manifest names paths below the mount as find prints them. */
#define MANIFEST_PREFIX "./"

struct device {
	size_t writes;
	size_t flushes;
};

static enum btrfs_result
refused_write(void *context, uint64_t offset, const void *bytes, size_t length)
{
	struct device *device = context;

	(void)offset;
	(void)bytes;
	(void)length;
	device->writes++;
	return BTRFS_IO;
}

static enum btrfs_result
refused_flush(void *context)
{
	struct device *device = context;

	device->flushes++;
	return BTRFS_IO;
}

static void
single_lock(void *context)
{
	(void)context;
}

static void
single_wait(void *context, const void *channel)
{
	(void)context;
	(void)channel;
	REQUIRE(0);
}

static void
single_wake(void *context, const void *channel)
{
	(void)context;
	(void)channel;
}

static size_t
relocation_trees(const struct btrfs_fs *fs)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = TREE_RELOC_OBJECTID, .type = BT_ROOT_ITEM, .offset = 0 };
	size_t count = 0;
	enum btrfs_result result;

	bt_cursor_init(&cursor, fs, fs->root_tree);
	result = bt_cursor_seek(&cursor, key, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		if (record.key.objectid != TREE_RELOC_OBJECTID || record.key.type != BT_ROOT_ITEM) {
			break;
		}
		count++;
		result = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	return count;
}

/* One manifest line, PATH SIZE SHA-256: the file reads as Linux's. */
static void
check_file(struct btrfs_fs *fs, char *line)
{
	char *fields[MANIFEST_FIELDS];
	char path[MANIFEST_LINE];
	char hex[SHA256_HEX + 1];
	uint8_t digest[BT_SHA256_DIGEST];
	struct btrfs_inode inode;
	struct bt_sha2 hash;
	uint8_t *bytes;
	size_t completed;
	size_t i;

	fields[0] = line;
	for (i = 1; i < MANIFEST_FIELDS; i++) {
		fields[i] = strchr(fields[i - 1], '\t');
		REQUIRE(fields[i] != NULL);
		*fields[i]++ = '\0';
	}
	REQUIRE(strncmp(fields[0], MANIFEST_PREFIX, strlen(MANIFEST_PREFIX)) == 0);
	REQUIRE(snprintf(path, sizeof(path), "/%s", fields[0] + strlen(MANIFEST_PREFIX)) <
	    (int)sizeof(path));
	REQUIRE(btrfs_image_lookup(fs, path, &inode) == BTRFS_OK);
	REQUIRE(inode.size == strtoull(fields[1], NULL, 10));
	bytes = malloc((size_t)inode.size + 1);
	REQUIRE(bytes != NULL);
	REQUIRE(btrfs_read(fs, &inode, 0, bytes, (size_t)inode.size, &completed) == BTRFS_OK);
	REQUIRE(completed == inode.size);
	bt_sha2_init(&hash, 0);
	bt_sha2_update(&hash, bytes, completed);
	bt_sha2_final(&hash, digest);
	for (i = 0; i < BT_SHA256_DIGEST; i++) {
		snprintf(hex + 2 * i, 3, "%02x", digest[i]);
	}
	if (strcmp(hex, fields[2]) != 0) {
		fprintf(stderr, "%s: SHA-256 %s, Linux %s\n", path, hex, fields[2]);
		exit(1);
	}
	free(bytes);
}

int
main(int argc, char **argv)
{
	static const struct btrfs_volume_locks locks = { NULL, single_lock, single_lock,
		single_wait, single_wake };
	struct device device = { 0, 0 };
	struct btrfs_write_environment writer = { &device, refused_write, refused_flush, NULL,
		BTRFS_COMPRESSION_NONE, 0 };
	struct reference_audit references;
	struct namespace_audit names;
	struct btrfs_transaction *transaction = NULL;
	struct btrfs_volume *volume = NULL;
	struct btrfs_image image;
	struct btrfs_fs *fs;
	char line[MANIFEST_LINE];
	FILE *manifest;
	size_t trees;
	size_t files = 0;

	REQUIRE(argc == 3);
	REQUIRE(btrfs_image_open(argv[1], &image) == 0);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	trees = relocation_trees(fs);
	REQUIRE(trees > 0);
	if (reference_audit(fs, &references) != 0) {
		fprintf(stderr, "reference audit: %s\n", references.failure);
		exit(1);
	}
	REQUIRE(namespace_audit(fs, &names) == 0);
	manifest = fopen(argv[2], "r");
	REQUIRE(manifest != NULL);
	while (fgets(line, sizeof(line), manifest) != NULL) {
		REQUIRE(strlen(line) > 0 && line[strlen(line) - 1] == '\n');
		line[strlen(line) - 1] = '\0';
		check_file(fs, line);
		files++;
	}
	REQUIRE(fclose(manifest) == 0);
	REQUIRE(files > 0);
	REQUIRE(btrfs_transaction_begin(fs, &writer, &transaction) == BTRFS_UNSUPPORTED);
	REQUIRE(transaction == NULL);
	btrfs_unmount(fs);
	REQUIRE(btrfs_volume_open(&image.environment, &writer, &locks, BTRFS_TOP_LEVEL_TREE,
		    &volume) == BTRFS_UNSUPPORTED);
	REQUIRE(btrfs_volume_open(
		    &image.environment, NULL, &locks, BTRFS_TOP_LEVEL_TREE, &volume) == BTRFS_OK);
	btrfs_volume_close(volume);
	REQUIRE(device.writes == 0 && device.flushes == 0);
	REQUIRE(image.live_allocations == 0);
	btrfs_image_close(&image);
	printf("interrupted balance: %zu relocation trees, %zu files read as Linux's, %zu trees "
	       "audited; writable admission refused without a write PASS\n",
	    trees, files, references.trees);
	return 0;
}
