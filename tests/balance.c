/* SPDX-License-Identifier: BSD-3-Clause */
/* Relocation of block groups (btrfs_relocate_group), as Linux's balance
 * relocates chunks. On a recording overlay of each image, every block group
 * is relocated on its own: afterwards the group is gone, the volume reads
 * exactly as before (the namespace digest of every file, directory, xattr
 * and subvolume), the reference and namespace audits pass, the qgroup audit
 * passes under quotas, and the file trees keep as many blocks, and the data
 * as many extents, as before, so no sharing between subvolumes was lost.
 * Every state a crash leaves at a barrier of that relocation reads the same
 * after superblock recovery and passes the reference audit, and some of them
 * relocate again to the same end. Then every group of the image is relocated
 * in turn; with an export directory, that image goes there for Linux. Small
 * steps (the work argument) give many crash states. A volume without room
 * refuses a group (NO_SPACE) reading the same; with --unsupported, groups
 * whose relocation is unsupported (a v1 space cache) must be
 * refused before any write, and one at least is. */
#include "../adapters/posix/image.h"
#include "internal.h"
#include "namespace_audit.h"
#include "qgroup_audit.h"
#include "references.h"
#include "transaction.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/clonefile.h>
#include <unistd.h>

#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define MAX_BARRIERS 65536U
#define OVERLAY_GROWTH 1024U
#define MAX_GROUPS 256U
/* Crash states of one group's relocation that relocate again to the end. */
#define RESUMED_STATES 6U
#define EXPORT_PATH 4096U

struct write_record {
	uint64_t offset;
	size_t length;
	uint8_t *bytes;
};

/* The image under recorded writes: reads see the first visible writes in
 * order on top of the image; writes and barriers are appended. The first
 * shared writes belong to the overlay this one was forked from. */
struct overlay {
	struct btrfs_image *image;
	struct write_record *writes;
	size_t count;
	size_t capacity;
	size_t visible;
	size_t shared;
	size_t *barriers;
	size_t barrier_count;
	/* Faults: the write, barrier and allocation, counted from one, that
	 * fail; 0 for none. */
	size_t fail_write;
	size_t fail_flush;
	size_t fail_allocation;
	size_t writes_seen;
	size_t flushes_seen;
	size_t allocations;
};

/* What every state of the image must show. */
struct expected {
	struct namespace_digest digest;
	size_t file_blocks;
	size_t data_extents;
	uint64_t data_bytes;
	int quotas;
};

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
	if (++overlay->writes_seen == overlay->fail_write) {
		return BTRFS_IO;
	}
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
	if (++overlay->flushes_seen == overlay->fail_flush) {
		return BTRFS_IO;
	}
	overlay->barriers[overlay->barrier_count++] = overlay->count;
	return BTRFS_OK;
}

static void *
overlay_allocate(void *context, size_t size)
{
	struct overlay *overlay = context;

	if (++overlay->allocations == overlay->fail_allocation) {
		return NULL;
	}
	return overlay->image->environment.allocate(overlay->image, size);
}

static void
overlay_release(void *context, void *bytes, size_t size)
{
	struct overlay *overlay = context;

	overlay->image->environment.release(overlay->image, bytes, size);
}

static void
overlay_init(struct overlay *overlay, struct btrfs_image *image)
{
	memset(overlay, 0, sizeof(*overlay));
	overlay->image = image;
	overlay->barriers = malloc(MAX_BARRIERS * sizeof(*overlay->barriers));
	REQUIRE(overlay->barriers != NULL);
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
	*writer = (struct btrfs_write_environment){ overlay, overlay_write, overlay_flush,
		btrfs_image_compress, BTRFS_COMPRESSION_NONE, 0, NULL, 0 };
}

/* A crash state: the first visible writes of source, then fresh writes. */
static void
overlay_fork(struct overlay *fork, const struct overlay *source, size_t visible)
{
	overlay_init(fork, source->image);
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
	free(overlay->barriers);
}

/* File tree blocks, data extents and their bytes: a relocation that keeps
 * sharing moves each of them and creates none. */
static void
count_extents(
    const struct btrfs_fs *fs, size_t *file_blocks, size_t *data_extents, uint64_t *data_bytes)
{
	const struct bt_disk_extent_item *item;
	const struct bt_disk_header *header;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root extents;
	uint8_t *node = malloc(fs->info.node_size);
	size_t chunk;
	enum btrfs_result result;

	REQUIRE(node != NULL);
	*file_blocks = 0;
	*data_extents = 0;
	*data_bytes = 0;
	REQUIRE(bt_find_root(fs, BT_EXTENT_TREE, &extents) == BTRFS_OK);
	bt_cursor_init(&cursor, fs, extents);
	result = bt_cursor_seek(&cursor, (struct bt_key){ 0 }, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		item = (const void *)record.data;
		if (record.key.type == BT_EXTENT_ITEM) {
			(*data_extents)++;
			*data_bytes += record.key.offset;
		} else if (record.key.type == BT_METADATA_ITEM) {
			REQUIRE(record.size >= sizeof(*item));
			chunk = bt_chunk_containing(fs, record.key.objectid);
			REQUIRE(chunk < fs->chunk_count);
			REQUIRE(
			    bt_tree_read(fs,
				(struct bt_root){ record.key.objectid, bt_u64(item->generation),
				    (fs->chunks[chunk].type & BT_BLOCK_SYSTEM) != 0 ? BT_CHUNK_TREE
										    : BT_OWNER_ANY,
				    (uint8_t)record.key.offset },
				node) == BTRFS_OK);
			header = (const void *)node;
			*file_blocks += bt_file_tree(bt_u64(header->owner));
		}
		result = bt_cursor_next(&cursor);
	}
	REQUIRE(result == BTRFS_NOT_FOUND);
	bt_cursor_fini(&cursor);
	free(node);
}

/* The audits, and the same files and extents as expected. */
static void
check_state(const struct btrfs_fs *fs, const struct expected *expected, int full, const char *when)
{
	struct reference_audit references;
	struct namespace_audit names;
	struct namespace_digest digest;
	struct qgroup_audit qgroups;
	size_t file_blocks;
	size_t data_extents;
	uint64_t data_bytes;

	if (reference_audit(fs, &references) != 0) {
		fprintf(stderr, "%s: reference audit: %s\n", when, references.failure);
		exit(1);
	}
	if (namespace_content_digest(fs, &digest) != 0 || digest.hash != expected->digest.hash ||
	    digest.objects != expected->digest.objects || digest.bytes != expected->digest.bytes) {
		fprintf(stderr,
		    "%s: namespace digest %s (%zu objects, %llu bytes; expected %zu, %llu)\n", when,
		    digest.failure, digest.objects, (unsigned long long)digest.bytes,
		    expected->digest.objects, (unsigned long long)expected->digest.bytes);
		exit(1);
	}
	if (!full) {
		return;
	}
	if (namespace_audit(fs, &names) != 0) {
		fprintf(stderr, "%s: namespace audit: %s\n", when, names.failure);
		exit(1);
	}
	if (expected->quotas && (qgroup_audit(fs, &qgroups) != 0 || qgroups.skipped)) {
		fprintf(stderr, "%s: qgroup audit: %s\n", when, qgroups.failure);
		exit(1);
	}
	count_extents(fs, &file_blocks, &data_extents, &data_bytes);
	if (file_blocks != expected->file_blocks || data_extents != expected->data_extents ||
	    data_bytes != expected->data_bytes) {
		fprintf(stderr, "%s: %zu file tree blocks, %zu data extents of %llu bytes\n", when,
		    file_blocks, data_extents, (unsigned long long)data_bytes);
		exit(1);
	}
}

static int
group_present(const struct btrfs_fs *fs, uint64_t logical)
{
	size_t i;

	for (i = 0; i < fs->chunk_count; i++) {
		if (fs->chunks[i].logical == logical) {
			return 1;
		}
	}
	return 0;
}

/* Mounts a state a crash left, recovering the superblocks first when their
 * copies disagree, as a writable mount does. */
static void
mount_state(struct overlay *state, struct btrfs_fs **fs, size_t *recoveries)
{
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_recovery_report supers;
	enum btrfs_result result;

	overlay_environment(state, &environment, &writer);
	result = btrfs_recover_supers(&environment, NULL, 0, &supers);
	if (result == BTRFS_RECOVERY_REQUIRED) {
		REQUIRE(btrfs_recover_supers(&environment, &writer, 0, &supers) == BTRFS_OK);
		(*recoveries)++;
	} else {
		REQUIRE(result == BTRFS_OK);
	}
	REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, fs) == BTRFS_OK);
}

/* Applies an overlay's writes to a clone of the image at path. */
static void
export_overlay(const struct overlay *overlay, const char *source, const char *path)
{
	size_t i;
	int file;

	REQUIRE(clonefile(source, path, 0) == 0);
	file = open(path, O_WRONLY);
	REQUIRE(file >= 0);
	for (i = 0; i < overlay->visible; i++) {
		REQUIRE(
		    pwrite(file, overlay->writes[i].bytes, overlay->writes[i].length,
			(off_t)overlay->writes[i].offset) == (ssize_t)overlay->writes[i].length);
	}
	REQUIRE(fsync(file) == 0);
	REQUIRE(close(file) == 0);
	printf("exported %s\n", path);
}

/* Relocates group on a fresh overlay with one fault; afterwards the volume
 * reads as expected, and a relocation without faults completes. */
static void
fault_run(struct btrfs_image *image, uint64_t group, size_t work, const struct expected *expected,
    size_t write, size_t flush, size_t allocation, size_t *recoveries)
{
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_balance_report report;
	struct overlay faulted;
	struct btrfs_fs *fs;
	enum btrfs_result result;

	overlay_init(&faulted, image);
	faulted.fail_write = write;
	faulted.fail_flush = flush;
	faulted.fail_allocation = allocation;
	overlay_environment(&faulted, &environment, &writer);
	result = bt_relocate_group(&environment, &writer, group, work, &report);
	REQUIRE(result == BTRFS_IO || result == BTRFS_NO_MEMORY ||
	    (allocation != 0 && result == BTRFS_OK));
	faulted.fail_write = 0;
	faulted.fail_flush = 0;
	faulted.fail_allocation = 0;
	mount_state(&faulted, &fs, recoveries);
	check_state(fs, expected, 0, "after a fault");
	btrfs_unmount(fs);
	overlay_environment(&faulted, &environment, &writer);
	result = bt_relocate_group(&environment, &writer, group, work, &report);
	REQUIRE(result == BTRFS_OK || result == BTRFS_NOT_FOUND);
	REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(!group_present(fs, group));
	check_state(fs, expected, 1, "relocated after a fault");
	btrfs_unmount(fs);
	overlay_free(&faulted);
}

/* Every write, barrier and allocation of the group's relocation fails in
 * turn. */
static size_t
fault_sweep(struct btrfs_image *image, uint64_t group, size_t work, const struct expected *expected,
    size_t *recoveries)
{
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_balance_report report;
	struct overlay clean;
	size_t writes;
	size_t flushes;
	size_t allocations;
	size_t faults = 0;
	size_t i;

	overlay_init(&clean, image);
	overlay_environment(&clean, &environment, &writer);
	REQUIRE(bt_relocate_group(&environment, &writer, group, work, &report) == BTRFS_OK);
	writes = clean.writes_seen;
	flushes = clean.flushes_seen;
	allocations = clean.allocations;
	overlay_free(&clean);
	for (i = 1; i <= writes; i++, faults++) {
		fault_run(image, group, work, expected, i, 0, 0, recoveries);
	}
	for (i = 1; i <= flushes; i++, faults++) {
		fault_run(image, group, work, expected, 0, i, 0, recoveries);
	}
	for (i = 1; i <= allocations; i++, faults++) {
		fault_run(image, group, work, expected, 0, 0, i, recoveries);
	}
	return faults;
}

/* Whether every group of a kind has profile (BTRFS_GROUP_DUP or 0). */
static int
profiles_are(const struct btrfs_fs *fs, uint64_t kind, uint64_t profile)
{
	size_t i;

	for (i = 0; i < fs->chunk_count; i++) {
		if ((fs->chunks[i].type & kind) != 0 &&
		    (fs->chunks[i].type & BTRFS_GROUP_DUP) != profile) {
			return 0;
		}
	}
	return 1;
}

static struct btrfs_balance_kind
balance_kind(unsigned usage, uint64_t convert, int soft)
{
	return (struct btrfs_balance_kind){ 1, usage, convert, soft };
}

/* Conversions as `btrfs balance start -dconvert=dup -mconvert=dup
 * -sconvert=dup`, then `-dconvert=single,soft`, then usage filters: each
 * leaves its kinds with the profile and the volume reading as before, and
 * so does every state a crash leaves at a barrier of the first. */
static void
convert_run(struct btrfs_image *image, const char *source, size_t work,
    const struct expected *expected, const char *directory)
{
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_balance_report report;
	struct btrfs_balance_args args;
	struct overlay recorded;
	struct overlay crash;
	struct btrfs_fs *fs;
	char path[EXPORT_PATH];
	uint64_t before[MAX_GROUPS];
	size_t before_count;
	size_t states = 0;
	size_t recoveries = 0;
	size_t kept = 0;
	size_t i;
	size_t j;
	int present;

	overlay_init(&recorded, image);
	overlay_environment(&recorded, &environment, &writer);
	memset(&args, 0, sizeof(args));
	args.data = balance_kind(101, BTRFS_GROUP_DUP, 0);
	args.metadata = balance_kind(101, BTRFS_GROUP_DUP, 0);
	args.system = balance_kind(101, BTRFS_GROUP_DUP, 0);
	REQUIRE(bt_balance(&environment, &writer, &args, work, &report) == BTRFS_OK);
	REQUIRE(report.groups > 0);
	REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(profiles_are(
	    fs, BTRFS_GROUP_DATA | BTRFS_GROUP_METADATA | BTRFS_GROUP_SYSTEM, BTRFS_GROUP_DUP));
	check_state(fs, expected, 1, "converted to DUP");
	btrfs_unmount(fs);
	if (directory != NULL) {
		REQUIRE(snprintf(path, sizeof(path), "%s/dup-%ld.raw", directory, (long)getpid()) <
		    (int)sizeof(path));
		export_overlay(&recorded, source, path);
	}
	for (i = 0; i < recorded.barrier_count; i++, states++) {
		overlay_fork(&crash, &recorded, recorded.barriers[i]);
		mount_state(&crash, &fs, &recoveries);
		check_state(fs, expected, 0, "conversion crash state");
		btrfs_unmount(fs);
		overlay_free(&crash);
	}

	/* Data back to single; soft leaves the groups already single. */
	overlay_environment(&recorded, &environment, &writer);
	memset(&args, 0, sizeof(args));
	args.data = balance_kind(101, BTRFS_BALANCE_SINGLE, 1);
	args.metadata.convert = BTRFS_BALANCE_KEEP;
	args.system.convert = BTRFS_BALANCE_KEEP;
	REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	/* Mixed groups convert their metadata too, which Linux leaves without
	 * a second copy only when forced. */
	if ((fs->info.incompat_features & BT_FEATURE_MIXED_GROUPS) != 0) {
		args.metadata = args.data;
		args.force = 1;
	}
	btrfs_unmount(fs);
	REQUIRE(bt_balance(&environment, &writer, &args, work, &report) == BTRFS_OK);
	REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(profiles_are(fs, BTRFS_GROUP_DATA, 0));
	if ((fs->info.incompat_features & BT_FEATURE_MIXED_GROUPS) == 0) {
		REQUIRE(
		    profiles_are(fs, BTRFS_GROUP_METADATA | BTRFS_GROUP_SYSTEM, BTRFS_GROUP_DUP));
	}
	check_state(fs, expected, 1, "data converted to single");
	before_count = fs->chunk_count;
	for (i = 0; i < before_count; i++) {
		before[i] = fs->chunks[i].logical;
	}
	btrfs_unmount(fs);

	/* usage=0 relocates only empty groups, and soft conversion to the
	 * profile every group has relocates none. */
	memset(&args, 0, sizeof(args));
	args.data = balance_kind(0, BTRFS_BALANCE_KEEP, 0);
	args.metadata = balance_kind(0, BTRFS_BALANCE_KEEP, 0);
	args.system = balance_kind(101, BTRFS_GROUP_DUP, 1);
	REQUIRE(bt_balance(&environment, &writer, &args, work, &report) == BTRFS_OK);
	REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	for (i = 0; i < before_count; i++) {
		present = 0;
		for (j = 0; j < fs->chunk_count; j++) {
			present |= fs->chunks[j].logical == before[i];
		}
		kept += present;
	}
	REQUIRE(kept + report.groups == before_count);
	check_state(fs, expected, 1, "usage filter");
	btrfs_unmount(fs);
	if (directory != NULL) {
		REQUIRE(snprintf(path, sizeof(path), "%s/converted-%ld.raw", directory,
			    (long)getpid()) < (int)sizeof(path));
		export_overlay(&recorded, source, path);
	}
	overlay_free(&recorded);
	printf("balance conversions: every kind to DUP (%zu crash states read the same, %zu "
	       "after superblock recovery), data back to single, usage and soft filters; the "
	       "volume reads as before PASS\n",
	    states, recoveries);
}

int
main(int argc, char **argv)
{
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_balance_report report;
	struct btrfs_balance_report again;
	struct btrfs_balance_report total;
	struct btrfs_image image;
	struct expected expected;
	struct qgroup_audit qgroups;
	struct overlay recorded;
	struct overlay crash;
	struct btrfs_fs *fs;
	char path[EXPORT_PATH];
	enum btrfs_result result;
	uint64_t groups[MAX_GROUPS];
	int refused[MAX_GROUPS] = { 0 };
	size_t refusals = 0;
	size_t unsupported_groups = 0;
	size_t fault_points = 0;
	int unsupported = 0;
	int faults = 0;
	int convert = 0;
	size_t group_count;
	size_t work;
	size_t states = 0;
	size_t resumed = 0;
	size_t recoveries = 0;
	size_t g;
	size_t i;

	if (argc > 1 && strcmp(argv[1], "--unsupported") == 0) {
		unsupported = 1;
		argc--;
		argv++;
	}
	if (argc > 1 && strcmp(argv[1], "--faults") == 0) {
		faults = 1;
		argc--;
		argv++;
	}
	if (argc > 1 && strcmp(argv[1], "--convert") == 0) {
		convert = 1;
		argc--;
		argv++;
	}
	REQUIRE(argc == 3 || argc == 4);
	work = (size_t)strtoul(argv[2], NULL, 10);
	REQUIRE(work > 0);
	REQUIRE(btrfs_image_open(argv[1], &image) == 0);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	memset(&expected, 0, sizeof(expected));
	REQUIRE(namespace_content_digest(fs, &expected.digest) == 0);
	count_extents(fs, &expected.file_blocks, &expected.data_extents, &expected.data_bytes);
	REQUIRE(qgroup_audit(fs, &qgroups) == 0);
	expected.quotas = qgroups.quotas;
	check_state(fs, &expected, 1, "image");
	group_count = fs->chunk_count;
	REQUIRE(group_count <= MAX_GROUPS);
	for (g = 0; g < group_count; g++) {
		groups[g] = fs->chunks[g].logical;
	}
	btrfs_unmount(fs);
	memset(&total, 0, sizeof(total));
	if (convert) {
		convert_run(&image, argv[1], work, &expected, argc == 4 ? argv[3] : NULL);
		REQUIRE(image.live_allocations == 0);
		btrfs_image_close(&image);
		return 0;
	}
	if (faults) {
		for (g = 0; g < group_count; g++) {
			fault_points +=
			    fault_sweep(&image, groups[g], work, &expected, &recoveries);
		}
		REQUIRE(image.live_allocations == 0);
		btrfs_image_close(&image);
		printf("balance faults: every write, barrier and allocation of %zu group "
		       "relocations failed in turn (%zu fault points); each left the volume "
		       "reading the same (%zu after superblock recovery) and relocated again "
		       "PASS\n",
		    group_count, fault_points, recoveries);
		return 0;
	}

	for (g = 0; g < group_count; g++) {
		overlay_init(&recorded, &image);
		overlay_environment(&recorded, &environment, &writer);
		result = bt_relocate_group(&environment, &writer, groups[g], work, &report);
		/* A group whose relocation is unsupported is refused before any
		 * write. */
		if (result == BTRFS_UNSUPPORTED && unsupported) {
			REQUIRE(recorded.count == 0 && recorded.barrier_count == 0);
			overlay_free(&recorded);
			refused[g] = 1;
			unsupported_groups++;
			continue;
		}
		/* A volume without room for the group's extents elsewhere refuses
		 * the relocation, as Linux's balance does, reading the same. */
		if (result == BTRFS_NO_SPACE) {
			REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
			REQUIRE(group_present(fs, groups[g]));
			check_state(fs, &expected, 1, "refused");
			btrfs_unmount(fs);
			overlay_free(&recorded);
			refused[g] = 1;
			refusals++;
			continue;
		}
		REQUIRE(result == BTRFS_OK);
		REQUIRE(report.groups == 1 && report.commits > 0);
		if (getenv("BALANCE_VERBOSE") != NULL) {
			fprintf(stderr, "group %llu: %llu blocks, %llu extents, %llu commits\n",
			    (unsigned long long)groups[g], (unsigned long long)report.blocks,
			    (unsigned long long)report.extents, (unsigned long long)report.commits);
		}
		REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
		REQUIRE(!group_present(fs, groups[g]));
		check_state(fs, &expected, 1, "relocated");
		btrfs_unmount(fs);
		REQUIRE(bt_relocate_group(&environment, &writer, groups[g], work, &again) ==
		    BTRFS_NOT_FOUND);
		total.blocks += report.blocks;
		total.extents += report.extents;
		total.bytes += report.bytes;
		total.commits += report.commits;
		/* Every barrier of the recording ends a state a crash may leave. */
		for (i = 0; i < recorded.barrier_count; i++) {
			overlay_fork(&crash, &recorded, recorded.barriers[i]);
			mount_state(&crash, &fs, &recoveries);
			check_state(fs, &expected, 0, "crash state");
			btrfs_unmount(fs);
			if (recorded.barrier_count <= RESUMED_STATES ||
			    i % (recorded.barrier_count / RESUMED_STATES) == 0) {
				overlay_environment(&crash, &environment, &writer);
				result = bt_relocate_group(
				    &environment, &writer, groups[g], work, &again);
				REQUIRE(result == BTRFS_OK || result == BTRFS_NOT_FOUND);
				REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) ==
				    BTRFS_OK);
				REQUIRE(!group_present(fs, groups[g]));
				check_state(fs, &expected, 1, "resumed");
				btrfs_unmount(fs);
				resumed++;
			}
			overlay_free(&crash);
			states++;
		}
		overlay_free(&recorded);
	}

	/* Every group in turn. */
	overlay_init(&recorded, &image);
	overlay_environment(&recorded, &environment, &writer);
	for (g = 0; g < group_count; g++) {
		result = refused[g]
		    ? BTRFS_NO_SPACE
		    : bt_relocate_group(&environment, &writer, groups[g], work, &report);
		REQUIRE(result == BTRFS_OK || result == BTRFS_NO_SPACE ||
		    (unsupported && result == BTRFS_UNSUPPORTED));
		refused[g] = result != BTRFS_OK;
	}
	REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	for (g = 0; g < group_count; g++) {
		REQUIRE(group_present(fs, groups[g]) == refused[g]);
	}
	check_state(fs, &expected, 1, "balanced");
	btrfs_unmount(fs);
	if (argc == 4) {
		REQUIRE(snprintf(path, sizeof(path), "%s/balanced-%ld.raw", argv[3],
			    (long)getpid()) < (int)sizeof(path));
		export_overlay(&recorded, argv[1], path);
	}
	overlay_free(&recorded);
	REQUIRE(image.live_allocations == 0);
	btrfs_image_close(&image);
	REQUIRE(!unsupported || unsupported_groups > 0);
	printf("balance: %zu unsupported groups refused without a write\n", unsupported_groups);
	printf("balance: %zu groups relocated one by one (%llu tree blocks, %llu data extents of "
	       "%llu bytes, %llu commits) read as before with sharing kept, %zu refused for "
	       "space; %zu crash states read the same (%zu after superblock recovery), %zu of "
	       "them relocated again; all groups in turn PASS\n",
	    group_count - refusals - unsupported_groups, (unsigned long long)total.blocks,
	    (unsigned long long)total.extents, (unsigned long long)total.bytes,
	    (unsigned long long)total.commits, refusals, states, recoveries, resumed);
	return 0;
}
