/* SPDX-License-Identifier: BSD-3-Clause */
/* An interrupted balance (tests/prepare_relocation_linux.py): Linux powered off
 * while its committed root tree held relocation trees, which a read-write
 * mount must merge and drop before anything else writes. The reader reads
 * every file as Linux's manifest says and both audits pass; writable admission
 * and a writable native volume require recovery (RECOVERY_REQUIRED) without a
 * write or a flush, while a read-only volume opens.
 *
 * btrfs_recover_relocation then runs on an overlay of the fixture, which
 * records every write and barrier: afterwards no relocation tree or data
 * relocation orphan remains, the audits pass, the files read as Linux's and a
 * transaction is admitted. Every state a crash can leave at a barrier of that
 * recovery is recovered again as a writable mount would: superblock recovery
 * when the copies disagree, the audits and the files, then relocation recovery
 * to the same end. */
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

/* Linux's relocation tree objectids (BTRFS_TREE_RELOC_OBJECTID, -8, and
 * BTRFS_DATA_RELOC_TREE_OBJECTID, -9). */
#define TREE_RELOC_OBJECTID (UINT64_MAX - UINT64_C(7))
#define DATA_RELOC_OBJECTID (UINT64_MAX - UINT64_C(8))
#define MANIFEST_LINE 512U
#define MANIFEST_FIELDS 3U
#define MANIFEST_FILES 4096U
#define SHA256_HEX (2U * BT_SHA256_DIGEST)
/* The manifest names paths below the mount as find prints them. */
#define MANIFEST_PREFIX "./"
#define MAX_BARRIERS 4096U
#define OVERLAY_GROWTH 1024U

struct refusal {
	size_t writes;
	size_t flushes;
};

struct write_record {
	uint64_t offset;
	size_t length;
	uint8_t *bytes;
};

/* The fixture under recorded writes: reads see the first visible writes in
 * order on top of the image; writes and barriers are appended. The first
 * shared writes belong to the overlay this one was forked from. */
struct overlay {
	struct btrfs_image *image;
	struct write_record *writes;
	size_t count;
	size_t capacity;
	size_t visible;
	size_t shared;
	size_t barriers[MAX_BARRIERS];
	size_t barrier_count;
};

struct manifest {
	char *lines[MANIFEST_FILES];
	size_t count;
};

static enum btrfs_result
refused_write(void *context, uint64_t offset, const void *bytes, size_t length)
{
	struct refusal *refusal = context;

	(void)offset;
	(void)bytes;
	(void)length;
	refusal->writes++;
	return BTRFS_IO;
}

static enum btrfs_result
refused_flush(void *context)
{
	struct refusal *refusal = context;

	refusal->flushes++;
	return BTRFS_IO;
}

static enum btrfs_result
overlay_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct overlay *overlay = context;
	const struct write_record *write;
	uint64_t start;
	uint64_t end;
	size_t i;
	enum btrfs_result error;

	error = overlay->image->environment.read(overlay->image, offset, buffer, length);
	for (i = 0; error == BTRFS_OK && i < overlay->visible; i++) {
		write = &overlay->writes[i];
		start = offset > write->offset ? offset : write->offset;
		end = offset + length < write->offset + write->length
		    ? offset + length
		    : write->offset + write->length;
		if (start < end) {
			memcpy((uint8_t *)buffer + (start - offset),
			    write->bytes + (start - write->offset), (size_t)(end - start));
		}
	}
	return error;
}

static enum btrfs_result
overlay_write(void *context, uint64_t offset, const void *bytes, size_t length)
{
	struct overlay *overlay = context;
	struct write_record *write;

	REQUIRE(overlay->visible == overlay->count);
	if (overlay->count == overlay->capacity) {
		overlay->capacity += OVERLAY_GROWTH;
		overlay->writes = realloc(overlay->writes, overlay->capacity * sizeof(*write));
		REQUIRE(overlay->writes != NULL);
	}
	write = &overlay->writes[overlay->count];
	write->offset = offset;
	write->length = length;
	write->bytes = malloc(length);
	REQUIRE(write->bytes != NULL);
	memcpy(write->bytes, bytes, length);
	overlay->count++;
	overlay->visible = overlay->count;
	return BTRFS_OK;
}

static enum btrfs_result
overlay_flush(void *context)
{
	struct overlay *overlay = context;

	REQUIRE(overlay->barrier_count < MAX_BARRIERS);
	overlay->barriers[overlay->barrier_count++] = overlay->count;
	return BTRFS_OK;
}

static void *
overlay_allocate(void *context, size_t size)
{
	struct overlay *overlay = context;

	return overlay->image->environment.allocate(overlay->image, size);
}

static void
overlay_release(void *context, void *bytes, size_t size)
{
	struct overlay *overlay = context;

	overlay->image->environment.release(overlay->image, bytes, size);
}

static void
overlay_environment(struct overlay *overlay, struct btrfs_environment *environment,
    struct btrfs_write_environment *writer)
{
	*environment = overlay->image->environment;
	environment->context = overlay;
	environment->read = overlay_read;
	environment->allocate = overlay_allocate;
	environment->release = overlay_release;
	*writer = (struct btrfs_write_environment){ overlay, overlay_write, overlay_flush, NULL,
		BTRFS_COMPRESSION_NONE, 0 };
}

/* A crash state: the first visible writes of source, then fresh writes. */
static void
overlay_fork(struct overlay *fork, const struct overlay *source, size_t visible)
{
	memset(fork, 0, sizeof(*fork));
	fork->image = source->image;
	fork->capacity = visible + OVERLAY_GROWTH;
	fork->writes = malloc(fork->capacity * sizeof(*fork->writes));
	REQUIRE(fork->writes != NULL);
	memcpy(fork->writes, source->writes, visible * sizeof(*fork->writes));
	fork->count = visible;
	fork->visible = visible;
	fork->shared = visible;
}

static void
overlay_free(struct overlay *overlay)
{
	size_t i;

	for (i = overlay->shared; i < overlay->count; i++) {
		free(overlay->writes[i].bytes);
	}
	free(overlay->writes);
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

/* Records of root whose key has first's objectid and type. */
static size_t
count_keys(const struct btrfs_fs *fs, struct bt_root root, struct bt_key first)
{
	struct bt_cursor cursor;
	struct bt_record record;
	size_t count = 0;
	enum btrfs_result result;

	bt_cursor_init(&cursor, fs, root);
	result = bt_cursor_seek(&cursor, first, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		if (record.key.objectid != first.objectid || record.key.type != first.type) {
			break;
		}
		count++;
		result = bt_cursor_next(&cursor);
	}
	REQUIRE(result == BTRFS_OK || result == BTRFS_NOT_FOUND);
	bt_cursor_fini(&cursor);
	return count;
}

static size_t
relocation_trees(const struct btrfs_fs *fs)
{
	return count_keys(fs, fs->root_tree,
	    (struct bt_key){ .objectid = TREE_RELOC_OBJECTID, .type = BT_ROOT_ITEM, .offset = 0 });
}

/* Orphan items of the data relocation tree. */
static size_t
relocation_orphans(const struct btrfs_fs *fs)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { .objectid = DATA_RELOC_OBJECTID, .type = BT_ROOT_ITEM, .offset = 0 };
	const struct bt_disk_root *item;
	struct bt_root root;

	bt_cursor_init(&cursor, fs, fs->root_tree);
	REQUIRE(bt_cursor_seek(&cursor, key, 0) == BTRFS_OK);
	REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
	REQUIRE(record.key.objectid == DATA_RELOC_OBJECTID && record.key.type == BT_ROOT_ITEM);
	item = (const void *)record.data;
	root = (struct bt_root){ bt_u64(item->bytenr), bt_u64(item->generation),
		DATA_RELOC_OBJECTID, item->level };
	bt_cursor_fini(&cursor);
	return count_keys(fs, root,
	    (struct bt_key){ .objectid = BT_ORPHAN_OBJECTID, .type = BT_ORPHAN_ITEM, .offset = 0 });
}

static void
read_manifest(const char *path, struct manifest *manifest)
{
	char line[MANIFEST_LINE];
	FILE *file;

	file = fopen(path, "r");
	REQUIRE(file != NULL);
	manifest->count = 0;
	while (fgets(line, sizeof(line), file) != NULL) {
		REQUIRE(strlen(line) > 0 && line[strlen(line) - 1] == '\n');
		line[strlen(line) - 1] = '\0';
		REQUIRE(manifest->count < MANIFEST_FILES);
		manifest->lines[manifest->count] = strdup(line);
		REQUIRE(manifest->lines[manifest->count] != NULL);
		manifest->count++;
	}
	REQUIRE(fclose(file) == 0);
	REQUIRE(manifest->count > 0);
}

/* One manifest line, PATH SIZE SHA-256: the file reads as Linux's. */
static void
check_file(struct btrfs_fs *fs, const char *entry)
{
	char *fields[MANIFEST_FIELDS];
	char line[MANIFEST_LINE];
	char path[MANIFEST_LINE];
	char hex[SHA256_HEX + 1];
	uint8_t digest[BT_SHA256_DIGEST];
	struct btrfs_inode inode;
	struct bt_sha2 hash;
	uint8_t *bytes;
	size_t completed;
	size_t i;

	REQUIRE(strlen(entry) < sizeof(line));
	strcpy(line, entry);
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

/* The mounted state is consistent and holds Linux's files; returns the
 * relocation trees it still holds. */
static size_t
check_state(struct btrfs_fs *fs, const struct manifest *manifest, struct reference_audit *audit)
{
	struct namespace_audit names;
	size_t i;

	if (reference_audit(fs, audit) != 0) {
		fprintf(stderr, "reference audit: %s\n", audit->failure);
		exit(1);
	}
	if (namespace_audit(fs, &names) != 0) {
		fprintf(stderr, "namespace audit: %s\n", names.failure);
		exit(1);
	}
	for (i = 0; i < manifest->count; i++) {
		check_file(fs, manifest->lines[i]);
	}
	return relocation_trees(fs);
}

/* A recovered state: nothing left to recover, a transaction admitted. */
static void
check_recovered(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, const struct manifest *manifest)
{
	struct btrfs_relocation_report report;
	struct reference_audit audit;
	struct btrfs_transaction *transaction = NULL;
	struct btrfs_fs *fs;

	REQUIRE(btrfs_mount(environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(check_state(fs, manifest, &audit) == 0);
	REQUIRE(relocation_orphans(fs) == 0);
	REQUIRE(btrfs_transaction_begin(fs, writer, &transaction) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	REQUIRE(btrfs_recover_relocation(environment, writer, &report) == BTRFS_NOT_FOUND);
}

int
main(int argc, char **argv)
{
	static const struct btrfs_volume_locks locks = { NULL, single_lock, single_lock,
		single_wait, single_wake };
	struct refusal refusal = { 0, 0 };
	struct btrfs_write_environment refused = { &refusal, refused_write, refused_flush, NULL,
		BTRFS_COMPRESSION_NONE, 0 };
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_relocation_report report;
	struct btrfs_relocation_report recovered;
	struct btrfs_recovery_report supers;
	struct reference_audit references;
	struct btrfs_transaction *transaction = NULL;
	struct btrfs_volume *volume = NULL;
	struct btrfs_image image;
	struct manifest manifest;
	struct overlay recorded;
	struct overlay crash;
	struct btrfs_fs *fs;
	enum btrfs_result result;
	size_t trees;
	size_t orphans;
	size_t states = 0;
	size_t pending_states = 0;
	size_t super_recoveries = 0;
	size_t i;

	REQUIRE(argc == 3);
	read_manifest(argv[2], &manifest);
	REQUIRE(btrfs_image_open(argv[1], &image) == 0);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	trees = check_state(fs, &manifest, &references);
	orphans = relocation_orphans(fs);
	REQUIRE(trees > 0 && orphans > 0);
	REQUIRE(btrfs_transaction_begin(fs, &refused, &transaction) == BTRFS_RECOVERY_REQUIRED);
	REQUIRE(transaction == NULL);
	btrfs_unmount(fs);
	REQUIRE(btrfs_volume_open(&image.environment, &refused, &locks, BTRFS_TOP_LEVEL_TREE,
		    &volume) == BTRFS_RECOVERY_REQUIRED);
	REQUIRE(btrfs_volume_open(
		    &image.environment, NULL, &locks, BTRFS_TOP_LEVEL_TREE, &volume) == BTRFS_OK);
	btrfs_volume_close(volume);
	REQUIRE(
	    btrfs_recover_relocation(&image.environment, NULL, &report) == BTRFS_RECOVERY_REQUIRED);
	REQUIRE(report.trees == trees);
	REQUIRE(refusal.writes == 0 && refusal.flushes == 0);

	/* Recovery on the recording overlay. */
	memset(&recorded, 0, sizeof(recorded));
	recorded.image = &image;
	overlay_environment(&recorded, &environment, &writer);
	REQUIRE(btrfs_recover_relocation(&environment, &writer, &recovered) == BTRFS_OK);
	REQUIRE(recovered.trees == trees && recovered.dropped == trees &&
	    recovered.orphans == orphans && recovered.commits > 0);
	check_recovered(&environment, &writer, &manifest);

	/* Every barrier of the recording ends a state a crash may leave. */
	for (i = 0; i < recorded.barrier_count; i++) {
		overlay_fork(&crash, &recorded, recorded.barriers[i]);
		overlay_environment(&crash, &environment, &writer);
		result = btrfs_recover_supers(&environment, NULL, 0, &supers);
		if (result == BTRFS_RECOVERY_REQUIRED) {
			REQUIRE(
			    btrfs_recover_supers(&environment, &writer, 0, &supers) == BTRFS_OK);
			super_recoveries++;
		} else {
			REQUIRE(result == BTRFS_OK);
		}
		REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
		pending_states +=
		    check_state(fs, &manifest, &references) != 0 || relocation_orphans(fs) != 0;
		btrfs_unmount(fs);
		result = btrfs_recover_relocation(&environment, &writer, &report);
		REQUIRE(result == BTRFS_OK || result == BTRFS_NOT_FOUND);
		check_recovered(&environment, &writer, &manifest);
		overlay_free(&crash);
		states++;
	}
	REQUIRE(pending_states > 0);
	overlay_free(&recorded);
	REQUIRE(image.live_allocations == 0);
	btrfs_image_close(&image);
	for (i = 0; i < manifest.count; i++) {
		free(manifest.lines[i]);
	}
	printf("interrupted balance: %zu relocation trees and %zu data relocation orphans, %zu "
	       "files read as Linux's; writable admission requires recovery, which drops them in "
	       "%llu commits; %zu crash states at its barriers (%zu still to recover, %zu after "
	       "superblock recovery) recover to the same end PASS\n",
	    trees, orphans, manifest.count, (unsigned long long)recovered.commits, states,
	    pending_states, super_recoveries);
	return 0;
}
