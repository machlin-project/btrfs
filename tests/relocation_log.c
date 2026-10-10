/* SPDX-License-Identifier: BSD-3-Clause */
/* Relocation recovery of every state a crash can leave during a balance.
 * tests/prepare_relocation_linux.py (record) wrote a volume through
 * dm-log-writes and then balanced its data and metadata to the end. Each
 * durable point of the log between the `filled` and `balanced` marks (a FUA
 * write, a flush or a mark) is replayed as a crash state. It must mount, after
 * superblock recovery when its copies disagree, and be consistent: both audits
 * pass. btrfs_recover_relocation then merges and drops what relocation trees
 * it holds and cleans the data relocation tree, on an overlay of the state;
 * afterwards no relocation tree or orphan remains, the audits pass, a
 * transaction is admitted, and the files read as Linux's manifest says. States
 * still waiting for a merge must occur, and so must states whose merge Linux
 * had begun. A state waiting for a merge is also merged in steps of one swap
 * each, committed one by one: every state a crash leaves at a barrier of
 * those steps recovers to the same leaves in every file tree, and some hold
 * the merge's recorded progress. Replaying the whole log must reproduce
 * Linux's final image. With
 * an export directory, the first recovered states that merged relocation
 * trees are written there for Linux to check, each as the crash state
 * (crash) and as recovered here (ours). `--compare LINUX OURS` then requires
 * that Linux's recovery of such a crash state and this one leave every file
 * tree with the same leaves: both swapped the same subtrees, and only the
 * copied paths above them differ. */
#define _POSIX_C_SOURCE 200809L
#include "../adapters/posix/image.h"
#include "internal.h"
#include "namespace_audit.h"
#include "transaction.h"
#include "references.h"
#include <btrfs/write.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

/* dm-log-writes on-disk format (drivers/md/dm-log-writes.c). */
#define LOG_MAGIC UINT64_C(0x6a736677736872)
#define LOG_VERSION UINT64_C(1)
#define LOG_FLUSH_FLAG UINT64_C(1)
#define LOG_FUA_FLAG (UINT64_C(1) << 1)
#define LOG_DISCARD_FLAG (UINT64_C(1) << 2)
#define LOG_MARK_FLAG (UINT64_C(1) << 3)
#define LOG_ENTRY_BYTES 32U
#define LOG_MARK_LIMIT 64U
#define LOG_ENTRIES_LIMIT 10000000U

/* The workload: see tests/prepare_relocation_linux.py. */
#define DEVICE_BYTES (UINT64_C(512) * 1024 * 1024)
#define FILLED_MARK "filled"
#define BALANCED_MARK "balanced"
/* Relocation tree and orphan objectids (BTRFS_TREE_RELOC_OBJECTID -8,
 * BTRFS_DATA_RELOC_TREE_OBJECTID -9). */
#define TREE_RELOC_OBJECTID (UINT64_MAX - UINT64_C(7))
#define DATA_RELOC_OBJECTID (UINT64_MAX - UINT64_C(8))
#define MANIFEST_LINE 512U
#define MANIFEST_FIELDS 3U
#define MANIFEST_FILES 4096U
#define SHA256_HEX (2U * BT_SHA256_DIGEST)
#define MANIFEST_PREFIX "./"
/* States without relocation trees check their files at this stride. */
#define FILE_STRIDE 16U
#define EXPORT_STATES 3U
#define EXPORT_BLOCK 4096U
#define OVERLAY_GROWTH 1024U

struct log_entry {
	uint64_t sector;
	uint64_t sectors;
	uint64_t flags;
	uint64_t data;
	char mark[LOG_MARK_LIMIT];
};

struct write_record {
	uint64_t offset;
	size_t length;
	uint8_t *bytes;
};

/* The replayed device under a state's own writes. */
struct overlay {
	const uint8_t *device;
	struct write_record *writes;
	size_t count;
	size_t capacity;
	/* The first shared writes belong to the overlay this one was forked
	 * from; barriers record the writes before each flush. */
	size_t shared;
	size_t *barriers;
	size_t barrier_count;
	size_t barrier_capacity;
};

struct manifest {
	char *lines[MANIFEST_FILES];
	size_t count;
};

/* What a state's root tree holds of an unfinished balance. */
struct relocation_state {
	size_t trees;
	size_t waiting;
	size_t merging;
	size_t orphans;
};

static uint64_t
le64(const uint8_t *bytes)
{
	uint64_t value = 0;
	int i;

	for (i = 7; i >= 0; i--) {
		value = value << 8 | bytes[i];
	}
	return value;
}

static void *allocate(void *context, size_t size);
static void release(void *context, void *allocation, size_t size);

static enum btrfs_result
overlay_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct overlay *overlay = context;
	const struct write_record *write;
	uint64_t start;
	uint64_t end;
	size_t i;

	if (offset > DEVICE_BYTES || length > DEVICE_BYTES - offset) {
		return BTRFS_IO;
	}
	memcpy(buffer, overlay->device + offset, length);
	for (i = 0; i < overlay->count; i++) {
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
	return BTRFS_OK;
}

static enum btrfs_result
overlay_write(void *context, uint64_t offset, const void *bytes, size_t length)
{
	struct overlay *overlay = context;
	struct write_record *write;

	if (offset > DEVICE_BYTES || length > DEVICE_BYTES - offset) {
		return BTRFS_IO;
	}
	if (overlay->count == overlay->capacity) {
		overlay->capacity += OVERLAY_GROWTH;
		overlay->writes = realloc(overlay->writes, overlay->capacity * sizeof(*write));
		REQUIRE(overlay->writes != NULL);
	}
	write = &overlay->writes[overlay->count++];
	write->offset = offset;
	write->length = length;
	write->bytes = malloc(length);
	REQUIRE(write->bytes != NULL);
	memcpy(write->bytes, bytes, length);
	return BTRFS_OK;
}

static enum btrfs_result
overlay_flush(void *context)
{
	struct overlay *overlay = context;

	if (overlay->barrier_count == overlay->barrier_capacity) {
		overlay->barrier_capacity += OVERLAY_GROWTH;
		overlay->barriers = realloc(
		    overlay->barriers, overlay->barrier_capacity * sizeof(*overlay->barriers));
		REQUIRE(overlay->barriers != NULL);
	}
	overlay->barriers[overlay->barrier_count++] = overlay->count;
	return BTRFS_OK;
}

static void
overlay_clear(struct overlay *overlay)
{
	while (overlay->count > overlay->shared) {
		free(overlay->writes[--overlay->count].bytes);
	}
	overlay->count = 0;
	overlay->barrier_count = 0;
}

static void
overlay_free(struct overlay *overlay)
{
	overlay_clear(overlay);
	free(overlay->writes);
	free(overlay->barriers);
}

/* A crash state of source: its first visible writes, then fresh writes. */
static void
overlay_fork(struct overlay *fork, const struct overlay *source, size_t visible)
{
	memset(fork, 0, sizeof(*fork));
	fork->device = source->device;
	fork->capacity = visible + OVERLAY_GROWTH;
	fork->writes = malloc(fork->capacity * sizeof(*fork->writes));
	REQUIRE(fork->writes != NULL);
	memcpy(fork->writes, source->writes, visible * sizeof(*fork->writes));
	fork->count = visible;
	fork->shared = visible;
}

static void
overlay_environment(struct overlay *overlay, struct btrfs_environment *environment,
    struct btrfs_write_environment *writer)
{
	memset(environment, 0, sizeof(*environment));
	environment->context = overlay;
	environment->size_bytes = DEVICE_BYTES;
	environment->read = overlay_read;
	environment->allocate = allocate;
	environment->release = release;
	*writer = (struct btrfs_write_environment){ overlay, overlay_write, overlay_flush, NULL,
		BTRFS_COMPRESSION_NONE, 0 };
}

static void *
allocate(void *context, size_t size)
{
	(void)context;
	return malloc(size);
}

static void
release(void *context, void *allocation, size_t size)
{
	(void)context;
	(void)size;
	free(allocation);
}

static void
read_log(int log, uint64_t offset, void *buffer, size_t length)
{
	REQUIRE(pread(log, buffer, length, (off_t)offset) == (ssize_t)length);
}

/* Entries follow the super block, one log sector each, with a write's data in
 * the sectors after its entry. */
static struct log_entry *
parse_log(int log, uint64_t *sector_size, size_t *count)
{
	uint8_t header[LOG_ENTRY_BYTES + LOG_MARK_LIMIT];
	struct log_entry *entries;
	struct log_entry *entry;
	uint64_t position;
	uint64_t total;
	size_t i;

	read_log(log, 0, header, 28);
	REQUIRE(le64(header) == LOG_MAGIC && le64(header + 8) == LOG_VERSION);
	total = le64(header + 16);
	*sector_size = (uint64_t)header[24] | (uint64_t)header[25] << 8 |
	    (uint64_t)header[26] << 16 | (uint64_t)header[27] << 24;
	REQUIRE(*sector_size == 512 || *sector_size == 4096);
	REQUIRE(total != 0 && total < LOG_ENTRIES_LIMIT);
	entries = calloc((size_t)total, sizeof(*entries));
	REQUIRE(entries != NULL);
	position = *sector_size;
	for (i = 0; i < total; i++) {
		entry = &entries[i];
		read_log(log, position, header, sizeof(header));
		entry->sector = le64(header);
		entry->sectors = le64(header + 8);
		entry->flags = le64(header + 16);
		position += *sector_size;
		if (entry->flags & LOG_MARK_FLAG) {
			REQUIRE(le64(header + 24) < LOG_MARK_LIMIT);
			memcpy(entry->mark, header + LOG_ENTRY_BYTES, (size_t)le64(header + 24));
			continue;
		}
		REQUIRE(le64(header + 24) == 0 && !(entry->flags & LOG_DISCARD_FLAG));
		entry->data = position;
		position += entry->sectors * *sector_size;
		REQUIRE((entry->sector + entry->sectors) * *sector_size <= DEVICE_BYTES);
	}
	*count = (size_t)total;
	return entries;
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

/* Every manifest line, PATH SIZE SHA-256, reads as Linux's. */
static void
check_files(struct btrfs_fs *fs, const struct manifest *manifest)
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
	size_t entry;
	size_t i;

	for (entry = 0; entry < manifest->count; entry++) {
		strcpy(line, manifest->lines[entry]);
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
		REQUIRE(
		    btrfs_read(fs, &inode, 0, bytes, (size_t)inode.size, &completed) == BTRFS_OK);
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
}

static void
audit(const struct btrfs_fs *fs, size_t position, const char *when)
{
	struct reference_audit references;
	struct namespace_audit names;

	if (reference_audit(fs, &references) != 0) {
		fprintf(stderr, "entry %zu %s: reference audit: %s\n", position, when,
		    references.failure);
		exit(1);
	}
	if (namespace_audit(fs, &names) != 0) {
		fprintf(
		    stderr, "entry %zu %s: namespace audit: %s\n", position, when, names.failure);
		exit(1);
	}
}

static void
classify(const struct btrfs_fs *fs, struct relocation_state *state)
{
	struct bt_cursor cursor;
	struct bt_record record;
	/* The data relocation tree's item (-9) comes before relocation trees'. */
	struct bt_key key = { .objectid = DATA_RELOC_OBJECTID, .type = BT_ROOT_ITEM, .offset = 0 };
	struct bt_key orphan = {
		.objectid = BT_ORPHAN_OBJECTID, .type = BT_ORPHAN_ITEM, .offset = 0
	};
	const struct bt_disk_root *item;
	struct bt_root data = { 0 };
	enum btrfs_result result;

	memset(state, 0, sizeof(*state));
	bt_cursor_init(&cursor, fs, fs->root_tree);
	result = bt_cursor_seek(&cursor, key, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		if (record.key.objectid == DATA_RELOC_OBJECTID && record.key.type == BT_ROOT_ITEM) {
			item = (const void *)record.data;
			data = (struct bt_root){ bt_u64(item->bytenr), bt_u64(item->generation),
				DATA_RELOC_OBJECTID, item->level };
		}
		if (record.key.objectid == TREE_RELOC_OBJECTID && record.key.type == BT_ROOT_ITEM) {
			item = (const void *)record.data;
			state->trees++;
			state->waiting += bt_u32(item->refs) != 0;
			state->merging +=
			    bt_u32(item->refs) != 0 && bt_u64(item->drop_progress.objectid) != 0;
		}
		result = bt_cursor_next(&cursor);
	}
	REQUIRE(result == BTRFS_NOT_FOUND);
	bt_cursor_fini(&cursor);
	REQUIRE(data.address != 0);
	bt_cursor_init(&cursor, fs, data);
	result = bt_cursor_seek(&cursor, orphan, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		if (record.key.objectid != BT_ORPHAN_OBJECTID ||
		    record.key.type != BT_ORPHAN_ITEM) {
			break;
		}
		state->orphans++;
		result = bt_cursor_next(&cursor);
	}
	REQUIRE(result == BTRFS_OK || result == BTRFS_NOT_FOUND);
	bt_cursor_fini(&cursor);
}

/* Writes the state's non-zero blocks, with or without its overlay, into a new
 * sparse image. */
static void
export_state(struct overlay *overlay, const char *directory, size_t position, const char *kind)
{
	static uint8_t block[EXPORT_BLOCK];
	static const uint8_t zero[EXPORT_BLOCK];
	char path[4096];
	uint64_t offset;
	int file;

	REQUIRE(snprintf(path, sizeof(path), "%s/relocation-state-%zu-%s.raw", directory, position,
		    kind) < (int)sizeof(path));
	file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	REQUIRE(file >= 0);
	REQUIRE(ftruncate(file, (off_t)DEVICE_BYTES) == 0);
	for (offset = 0; offset < DEVICE_BYTES; offset += EXPORT_BLOCK) {
		REQUIRE(overlay_read(overlay, offset, block, EXPORT_BLOCK) == BTRFS_OK);
		if (memcmp(block, zero, EXPORT_BLOCK) != 0) {
			REQUIRE(pwrite(file, block, EXPORT_BLOCK, (off_t)offset) == EXPORT_BLOCK);
		}
	}
	REQUIRE(close(file) == 0);
	printf("exported %s\n", path);
}

/* The leaf addresses of a tree, in key order. */
static size_t
tree_leaves(const struct btrfs_fs *fs, struct bt_root root, uint64_t *leaves, size_t capacity)
{
	struct bt_cursor cursor;
	struct bt_key first = { 0 };
	uint64_t leaf;
	size_t count = 0;
	enum btrfs_result result;

	bt_cursor_init(&cursor, fs, root);
	result = bt_cursor_seek(&cursor, first, 0);
	while (result == BTRFS_OK) {
		leaf = bt_u64(((const struct bt_disk_header *)cursor.blocks[0])->bytenr);
		if (count == 0 || leaves[count - 1] != leaf) {
			REQUIRE(count < capacity);
			leaves[count++] = leaf;
		}
		result = bt_cursor_next(&cursor);
	}
	REQUIRE(result == BTRFS_NOT_FOUND);
	bt_cursor_fini(&cursor);
	return count;
}

/* Every file tree of the two images holds the same leaves. */
static int
compare(const char *linux_path, const char *ours_path)
{
	struct btrfs_image images[2];
	struct btrfs_fs *fs[2];
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key first = { 0 };
	struct bt_root roots[2];
	uint64_t *leaves[2];
	size_t counts[2];
	size_t trees = 0;
	size_t total = 0;
	size_t capacity = DEVICE_BYTES / EXPORT_BLOCK;
	unsigned side;
	enum btrfs_result result;

	REQUIRE(btrfs_image_open(linux_path, &images[0]) == 0);
	REQUIRE(btrfs_image_open(ours_path, &images[1]) == 0);
	for (side = 0; side < 2; side++) {
		REQUIRE(btrfs_mount(&images[side].environment, BTRFS_TOP_LEVEL_TREE, &fs[side]) ==
		    BTRFS_OK);
		leaves[side] = malloc(capacity * sizeof(*leaves[side]));
		REQUIRE(leaves[side] != NULL);
	}
	bt_cursor_init(&cursor, fs[1], fs[1]->root_tree);
	result = bt_cursor_seek(&cursor, first, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		if (record.key.type == BT_ROOT_ITEM && bt_file_tree(record.key.objectid)) {
			for (side = 0; side < 2; side++) {
				REQUIRE(bt_find_root(fs[side], record.key.objectid, &roots[side]) ==
				    BTRFS_OK);
				counts[side] =
				    tree_leaves(fs[side], roots[side], leaves[side], capacity);
			}
			if (counts[0] != counts[1] ||
			    memcmp(leaves[0], leaves[1], counts[0] * sizeof(*leaves[0])) != 0) {
				fprintf(stderr,
				    "tree %llu: %zu leaves after Linux's recovery, %zu here\n",
				    (unsigned long long)record.key.objectid, counts[0], counts[1]);
				return 1;
			}
			trees++;
			total += counts[0];
		}
		result = bt_cursor_next(&cursor);
	}
	REQUIRE(result == BTRFS_NOT_FOUND);
	bt_cursor_fini(&cursor);
	for (side = 0; side < 2; side++) {
		free(leaves[side]);
		btrfs_unmount(fs[side]);
		btrfs_image_close(&images[side]);
	}
	printf("%zu file trees hold the same %zu leaves after Linux's relocation recovery and "
	       "this one PASS\n",
	    trees, total);
	return 0;
}

/* FNV-1a over every file tree's id and leaf addresses. */
static uint64_t
leaf_digest(const struct btrfs_fs *fs)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key first = { 0 };
	struct bt_root root;
	uint64_t *leaves;
	uint64_t digest = UINT64_C(0xcbf29ce484222325);
	size_t capacity = DEVICE_BYTES / EXPORT_BLOCK;
	size_t count;
	size_t i;
	enum btrfs_result result;

	leaves = malloc(capacity * sizeof(*leaves));
	REQUIRE(leaves != NULL);
	bt_cursor_init(&cursor, fs, fs->root_tree);
	result = bt_cursor_seek(&cursor, first, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		if (record.key.type == BT_ROOT_ITEM && bt_file_tree(record.key.objectid)) {
			REQUIRE(bt_find_root(fs, record.key.objectid, &root) == BTRFS_OK);
			count = tree_leaves(fs, root, leaves, capacity);
			digest = (digest ^ record.key.objectid) * UINT64_C(0x100000001b3);
			for (i = 0; i < count; i++) {
				digest = (digest ^ leaves[i]) * UINT64_C(0x100000001b3);
			}
		}
		result = bt_cursor_next(&cursor);
	}
	REQUIRE(result == BTRFS_NOT_FOUND);
	bt_cursor_fini(&cursor);
	free(leaves);
	return digest;
}

/* Merges a state waiting for a merge in steps of one swap each, then
 * recovers every state a crash leaves at a barrier of those steps. With an
 * export directory, the first such state holding the merge's progress is
 * written there (partial) with the merge's end (steps), for Linux to resume. */
static void
check_steps(const uint8_t *device, size_t position, const char *directory, int *exported,
    size_t *states, size_t *progress)
{
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_relocation_report report;
	struct btrfs_recovery_report supers;
	struct relocation_state left;
	struct overlay steps = { 0 };
	struct overlay crash;
	struct btrfs_fs *fs;
	enum btrfs_result result;
	uint64_t digest;
	size_t b;

	steps.device = device;
	overlay_environment(&steps, &environment, &writer);
	REQUIRE(bt_recover_relocation(&environment, &writer, 1, &report) == BTRFS_OK);
	REQUIRE(report.merged != 0);
	REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	audit(fs, position, "after merging in steps");
	classify(fs, &left);
	REQUIRE(left.trees == 0 && left.orphans == 0);
	digest = leaf_digest(fs);
	btrfs_unmount(fs);
	for (b = 0; b < steps.barrier_count; b++) {
		overlay_fork(&crash, &steps, steps.barriers[b]);
		overlay_environment(&crash, &environment, &writer);
		result = btrfs_recover_supers(&environment, NULL, 0, &supers);
		if (result == BTRFS_RECOVERY_REQUIRED) {
			REQUIRE(
			    btrfs_recover_supers(&environment, &writer, 0, &supers) == BTRFS_OK);
		} else {
			REQUIRE(result == BTRFS_OK);
		}
		REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
		audit(fs, position, "at a barrier of the merge steps");
		classify(fs, &left);
		*progress += left.merging != 0;
		btrfs_unmount(fs);
		if (directory != NULL && !*exported && left.merging != 0 && result == BTRFS_OK) {
			export_state(&crash, directory, position, "partial");
			export_state(&steps, directory, position, "steps");
			*exported = 1;
		}
		result = btrfs_recover_relocation(&environment, &writer, &report);
		REQUIRE(result == BTRFS_OK || result == BTRFS_NOT_FOUND);
		REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
		audit(fs, position, "recovered from a barrier of the merge steps");
		classify(fs, &left);
		REQUIRE(left.trees == 0 && left.orphans == 0);
		REQUIRE(leaf_digest(fs) == digest);
		btrfs_unmount(fs);
		overlay_free(&crash);
		(*states)++;
	}
	overlay_free(&steps);
}

int
main(int argc, char **argv)
{
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_relocation_report report;
	struct btrfs_recovery_report supers;
	struct btrfs_transaction *transaction;
	struct relocation_state before;
	struct relocation_state after;
	struct log_entry *entries;
	struct manifest manifest;
	struct overlay overlay = { 0 };
	struct btrfs_fs *fs;
	uint8_t *final;
	int image;
	enum btrfs_result result;
	uint64_t sector_size;
	uint64_t offset;
	uint8_t *device;
	size_t count;
	size_t i;
	size_t states = 0;
	size_t relocating = 0;
	size_t waiting = 0;
	size_t merging = 0;
	size_t orphan_states = 0;
	size_t merged = 0;
	size_t dropped = 0;
	size_t super_recoveries = 0;
	size_t exported = 0;
	size_t kept;
	size_t step_states = 0;
	int exported_partial = 0;
	size_t progress_states = 0;
	int recovered_supers;
	int log;
	int filled = 0;
	int balanced = 0;

	if (argc == 4 && strcmp(argv[1], "--compare") == 0) {
		return compare(argv[2], argv[3]);
	}
	REQUIRE(argc == 4 || argc == 5);
	read_manifest(argv[3], &manifest);
	log = open(argv[1], O_RDONLY);
	REQUIRE(log >= 0);
	entries = parse_log(log, &sector_size, &count);
	device = calloc(1, (size_t)DEVICE_BYTES);
	REQUIRE(device != NULL);
	overlay.device = device;
	overlay_environment(&overlay, &environment, &writer);
	for (i = 0; i < count; i++) {
		if (entries[i].sectors != 0 && entries[i].data != 0) {
			offset = entries[i].sector * sector_size;
			read_log(log, entries[i].data, device + offset,
			    (size_t)(entries[i].sectors * sector_size));
		}
		if (!filled || balanced ||
		    !(entries[i].flags & (LOG_FUA_FLAG | LOG_FLUSH_FLAG | LOG_MARK_FLAG))) {
			filled = filled ||
			    ((entries[i].flags & LOG_MARK_FLAG) &&
				strcmp(entries[i].mark, FILLED_MARK) == 0);
			continue;
		}
		balanced = (entries[i].flags & LOG_MARK_FLAG) &&
		    strcmp(entries[i].mark, BALANCED_MARK) == 0;
		/* A crash state: the log up to this durable point. */
		recovered_supers = 0;
		result = btrfs_recover_supers(&environment, NULL, 0, &supers);
		if (result == BTRFS_RECOVERY_REQUIRED) {
			REQUIRE(
			    btrfs_recover_supers(&environment, &writer, 0, &supers) == BTRFS_OK);
			super_recoveries++;
			recovered_supers = 1;
		} else {
			REQUIRE(result == BTRFS_OK);
		}
		REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
		audit(fs, i, "before recovery");
		classify(fs, &before);
		if (before.trees != 0 || states % FILE_STRIDE == 0) {
			check_files(fs, &manifest);
		}
		btrfs_unmount(fs);
		if (before.waiting != 0 && !recovered_supers) {
			check_steps(device, i, argc == 5 ? argv[4] : NULL, &exported_partial,
			    &step_states, &progress_states);
		}
		result = btrfs_recover_relocation(&environment, &writer, &report);
		REQUIRE(result == BTRFS_OK || result == BTRFS_NOT_FOUND);
		REQUIRE((result == BTRFS_NOT_FOUND) == (before.trees == 0 && before.orphans == 0));
		REQUIRE(report.merged == before.waiting && report.dropped == before.trees &&
		    report.orphans == before.orphans);
		REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
		audit(fs, i, "after recovery");
		classify(fs, &after);
		REQUIRE(after.trees == 0 && after.orphans == 0);
		if (before.trees != 0 || states % FILE_STRIDE == 0) {
			check_files(fs, &manifest);
		}
		REQUIRE(btrfs_transaction_begin(fs, &writer, &transaction) == BTRFS_OK);
		btrfs_transaction_destroy(transaction);
		btrfs_unmount(fs);
		/* Linux mounts only states whose primary copy it can use. */
		if (argc == 5 && before.waiting != 0 && !recovered_supers &&
		    exported < EXPORT_STATES) {
			kept = overlay.count;
			overlay.count = 0;
			export_state(&overlay, argv[4], i, "crash");
			overlay.count = kept;
			export_state(&overlay, argv[4], i, "ours");
			exported++;
		}
		overlay_clear(&overlay);
		states++;
		relocating += before.trees != 0;
		waiting += before.waiting != 0;
		merging += before.merging != 0;
		orphan_states += before.orphans != 0;
		merged += before.waiting;
		dropped += before.trees;
	}
	REQUIRE(filled && balanced);
	REQUIRE(waiting > 0 && progress_states > 0);
	image = open(argv[2], O_RDONLY);
	REQUIRE(image >= 0);
	final = malloc((size_t)DEVICE_BYTES);
	REQUIRE(final != NULL);
	REQUIRE(pread(image, final, (size_t)DEVICE_BYTES, 0) == (ssize_t)DEVICE_BYTES);
	REQUIRE(memcmp(final, device, (size_t)DEVICE_BYTES) == 0);
	REQUIRE(close(image) == 0);
	free(final);
	overlay_free(&overlay);
	free(entries);
	free(device);
	close(log);
	for (i = 0; i < manifest.count; i++) {
		free(manifest.lines[i]);
	}
	printf("interrupted balance log: %zu crash states between the marks, %zu with relocation "
	       "trees (%zu waiting for a merge, %zu with Linux's merge under way, %zu with data "
	       "relocation orphans), %zu after superblock recovery; recovery merged %zu and "
	       "dropped %zu relocation trees, and every state ends consistent with Linux's files; "
	       "merged in single swaps, %zu crash states at their barriers (%zu holding the "
	       "merge's progress) recover to the same leaves; final image reproduced PASS\n",
	    states, relocating, waiting, merging, orphan_states, super_recoveries, merged, dropped,
	    step_states, progress_states);
	return 0;
}
