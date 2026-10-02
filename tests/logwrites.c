/* SPDX-License-Identifier: BSD-3-Clause */
/* Crash states Linux wrote. tests/prepare_logwrites_linux.py records every
 * write of a Linux Btrfs workload with dm-log-writes; this test replays the log
 * from a zeroed device. Each prefix of the log, and sampled subsets of the
 * writes issued after each flush, is a state a power cut can leave. This
 * implementation must mount it, or recover it explicitly, and then find it
 * consistent: both audits pass, a transaction is admitted and every file reads
 * back with verified checksums. Where the workload marked a finished `sync`,
 * the state must hold exactly the files of that step. Replaying the whole log
 * must reproduce Linux's final device image. */
#define _POSIX_C_SOURCE 200809L
#include "../adapters/posix/image.h"
#include "namespace_audit.h"
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

/* The workload: see tests/prepare_logwrites_linux.py. */
#define DEVICE_BYTES (UINT64_C(256) * 1024 * 1024)
#define SMALL_FILES 50U
#define MANY_FILES 300U
#define SUBSETS_PER_EPOCH 8U
/* Prefixes before mkfs finished (mostly zeroing) are sampled; every later
 * prefix is checked. */
#define PREFIX_STRIDE 16U

struct log_entry {
	uint64_t sector;
	uint64_t sectors;
	uint64_t flags;
	uint64_t data;
	char mark[LOG_MARK_LIMIT];
};

struct input {
	const char *name;
	uint64_t seed;
	size_t size;
	uint8_t *bytes;
};

static struct input inputs[] = { { "a", 1, 300 * 1024, NULL }, { "b", 2, 64 * 1024, NULL },
	{ "c", 3, 5000, NULL }, { "small", 4, 100, NULL }, { "big", 5, 8 * 1024 * 1024, NULL } };

struct state {
	uint8_t *device;
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	int log;
	uint64_t sector_size;
	struct log_entry *entries;
	size_t count;
	uint64_t random;
	size_t checked;
	size_t recovered;
	size_t marks;
	size_t empty;
	size_t subsets;
	/* Where the state ends in the log and whether it is a subset. */
	size_t position;
	int subset;
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

static void
generate(struct input *input)
{
	uint64_t state = input->seed * UINT64_C(0x9E3779B97F4A7C15);
	size_t i;

	state = state == 0 ? 1 : state;
	input->bytes = malloc(input->size);
	REQUIRE(input->bytes != NULL);
	for (i = 0; i < input->size; i++) {
		state ^= state >> 12;
		state ^= state << 25;
		state ^= state >> 27;
		input->bytes[i] = (uint8_t)((state * UINT64_C(0x2545F4914F6CDD1D)) >> 56);
	}
}

static const struct input *
find_input(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
		if (strcmp(inputs[i].name, name) == 0) {
			return &inputs[i];
		}
	}
	REQUIRE(0);
	return NULL;
}

static enum btrfs_result
device_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct state *state = context;

	if (offset > DEVICE_BYTES || length > DEVICE_BYTES - offset) {
		return BTRFS_IO;
	}
	memcpy(buffer, state->device + offset, length);
	return BTRFS_OK;
}

static enum btrfs_result
device_write(void *context, uint64_t offset, const void *bytes, size_t length)
{
	struct state *state = context;

	if (offset > DEVICE_BYTES || length > DEVICE_BYTES - offset) {
		return BTRFS_IO;
	}
	memcpy(state->device + offset, bytes, length);
	return BTRFS_OK;
}

static enum btrfs_result
device_flush(void *context)
{
	(void)context;
	return BTRFS_OK;
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
read_log(struct state *state, uint64_t offset, void *buffer, size_t length)
{
	REQUIRE(pread(state->log, buffer, length, (off_t)offset) == (ssize_t)length);
}

/* Entries follow the super block, one log sector each, with a write's data in
 * the sectors after its entry. */
static void
parse_log(struct state *state)
{
	uint8_t header[LOG_ENTRY_BYTES + LOG_MARK_LIMIT];
	struct log_entry *entry;
	uint64_t position;
	uint64_t entries;
	size_t i;

	read_log(state, 0, header, 28);
	REQUIRE(le64(header) == LOG_MAGIC && le64(header + 8) == LOG_VERSION);
	entries = le64(header + 16);
	state->sector_size = (uint64_t)header[24] | (uint64_t)header[25] << 8 |
	    (uint64_t)header[26] << 16 | (uint64_t)header[27] << 24;
	REQUIRE(state->sector_size == 512 || state->sector_size == 4096);
	REQUIRE(entries != 0 && entries < 10000000);
	state->entries = calloc((size_t)entries, sizeof(*state->entries));
	REQUIRE(state->entries != NULL);
	position = state->sector_size;
	for (i = 0; i < entries; i++) {
		entry = &state->entries[i];
		read_log(state, position, header, sizeof(header));
		entry->sector = le64(header);
		entry->sectors = le64(header + 8);
		entry->flags = le64(header + 16);
		position += state->sector_size;
		if (entry->flags & LOG_MARK_FLAG) {
			REQUIRE(le64(header + 24) < LOG_MARK_LIMIT);
			memcpy(entry->mark, header + LOG_ENTRY_BYTES, (size_t)le64(header + 24));
			continue;
		}
		REQUIRE(le64(header + 24) == 0 && !(entry->flags & LOG_DISCARD_FLAG));
		entry->data = position;
		position += entry->sectors * state->sector_size;
		REQUIRE((entry->sector + entry->sectors) * state->sector_size <= DEVICE_BYTES);
	}
	state->count = (size_t)entries;
}

/* What one applied write replaced, so an epoch can be taken back. */
struct undo {
	uint64_t offset;
	size_t length;
	uint8_t *bytes;
};

struct undo_list {
	struct undo *items;
	size_t count;
	size_t capacity;
};

static void
apply(struct state *state, const struct log_entry *entry, struct undo_list *undo)
{
	uint64_t offset = entry->sector * state->sector_size;
	size_t length = (size_t)(entry->sectors * state->sector_size);
	struct undo *record;

	if (entry->sectors == 0 || entry->data == 0) {
		return;
	}
	if (undo != NULL) {
		if (undo->count == undo->capacity) {
			undo->capacity = undo->capacity == 0 ? 64 : undo->capacity * 2;
			undo->items = realloc(undo->items, undo->capacity * sizeof(*undo->items));
			REQUIRE(undo->items != NULL);
		}
		record = &undo->items[undo->count++];
		record->offset = offset;
		record->length = length;
		record->bytes = malloc(length);
		REQUIRE(record->bytes != NULL);
		memcpy(record->bytes, state->device + offset, length);
	}
	read_log(state, entry->data, state->device + offset, length);
}

/* Forgets the recorded writes without taking them back. */
static void
revert_forget(struct undo_list *undo)
{
	while (undo->count != 0) {
		free(undo->items[--undo->count].bytes);
	}
}

/* Takes back the recorded writes, newest first, and forgets them. */
static void
revert(struct state *state, struct undo_list *undo)
{
	while (undo->count != 0) {
		undo->count--;
		memcpy(state->device + undo->items[undo->count].offset,
		    undo->items[undo->count].bytes, undo->items[undo->count].length);
		free(undo->items[undo->count].bytes);
	}
}

static uint8_t *
read_path(struct btrfs_fs *fs, const char *path, uint64_t *size, enum btrfs_result *result)
{
	struct btrfs_inode inode;
	uint8_t *bytes;
	size_t completed = 0;

	*result = btrfs_image_lookup(fs, path, &inode);
	if (*result != BTRFS_OK) {
		return NULL;
	}
	*size = inode.size;
	bytes = malloc((size_t)inode.size + 1);
	REQUIRE(bytes != NULL);
	*result = btrfs_read(fs, &inode, 0, bytes, (size_t)inode.size, &completed);
	REQUIRE(*result != BTRFS_OK || completed == inode.size);
	return bytes;
}

static void
expect_input(struct btrfs_fs *fs, const char *mark, const char *path, const char *name)
{
	const struct input *input = name == NULL ? NULL : find_input(name);
	enum btrfs_result result;
	uint64_t size = 0;
	uint8_t *bytes = read_path(fs, path, &size, &result);

	if (input == NULL ? result != BTRFS_NOT_FOUND
			  : result != BTRFS_OK || size != input->size ||
		    memcmp(bytes, input->bytes, input->size) != 0) {
		fprintf(stderr, "mark %s: %s is not %s (%s)\n", mark, path,
		    name == NULL ? "absent" : name, btrfs_result_string(result));
		exit(1);
	}
	free(bytes);
}

/* The files each finished step synced. */
static void
expect_mark(struct btrfs_fs *fs, const char *mark)
{
	static const char *const steps[] = { "step-1", "step-2", "step-3", "step-4", "step-5",
		"step-6", "unmounted" };
	const char *directory;
	char path[32];
	enum btrfs_result result;
	uint64_t size = 1;
	unsigned step = 0;
	unsigned i;

	for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		if (strcmp(mark, steps[i]) == 0) {
			step = i + 1 > 6 ? 6 : i + 1;
		}
	}
	if (step == 0) {
		return;
	}
	directory = step >= 5 ? "/e" : "/d";
	expect_input(fs, mark, "/a", step == 1 ? "a" : "b");
	REQUIRE(snprintf(path, sizeof(path), "%s/c", directory) < (int)sizeof(path));
	expect_input(fs, mark, path, step == 1 ? "c" : NULL);
	if (step >= 2) {
		REQUIRE(snprintf(path, sizeof(path), "%s/a-link", directory) < (int)sizeof(path));
		expect_input(fs, mark, path, "b");
		if (step < 4) {
			expect_input(fs, mark, "/c2", "c");
		} else {
			free(read_path(fs, "/c2", &size, &result));
			REQUIRE(result == BTRFS_OK && size == 0);
		}
		for (i = 0; i < SMALL_FILES; i++) {
			REQUIRE(snprintf(path, sizeof(path), "%s/s%02u", directory, i) <
			    (int)sizeof(path));
			expect_input(
			    fs, mark, path, step >= 3 && i < SMALL_FILES / 2 ? NULL : "small");
		}
	}
	if (step >= 3) {
		expect_input(fs, mark, "/sv/big", step == 3 ? "big" : "a");
		expect_input(fs, mark, "/snap/big", step < 6 ? "big" : NULL);
	}
	if (step >= 5) {
		expect_input(fs, mark, "/d/c", NULL);
		for (i = 0; i < MANY_FILES; i++) {
			REQUIRE(snprintf(path, sizeof(path), "/many/f%03u", i) < (int)sizeof(path));
			expect_input(fs, mark, path, step >= 6 && i % 2 == 0 ? NULL : "c");
		}
	}
}

/* Reads every file of a tree, following subvolume entries one level. */
static void
read_tree(struct btrfs_fs *fs, const struct btrfs_inode *directory, unsigned depth)
{
	struct btrfs_directory *stream;
	struct btrfs_dir_entry entry;
	struct btrfs_inode inode;
	uint8_t *buffer;
	uint64_t cookie;
	size_t completed;
	enum btrfs_result result;

	REQUIRE(depth < 8);
	REQUIRE(btrfs_directory_open(fs, directory, 0, &stream) == BTRFS_OK);
	while ((result = btrfs_directory_next(stream, &entry, &cookie)) == BTRFS_OK) {
		REQUIRE(btrfs_get_inode(fs, entry.id, &inode) == BTRFS_OK);
		if ((inode.mode & BTRFS_MODE_TYPE) == BTRFS_MODE_DIRECTORY) {
			read_tree(fs, &inode, depth + 1);
		} else if ((inode.mode & BTRFS_MODE_TYPE) == BTRFS_MODE_REGULAR) {
			buffer = malloc((size_t)inode.size + 1);
			REQUIRE(buffer != NULL);
			REQUIRE(btrfs_read(fs, &inode, 0, buffer, (size_t)inode.size, &completed) ==
				BTRFS_OK &&
			    completed == inode.size);
			free(buffer);
		}
	}
	REQUIRE(result == BTRFS_NOT_FOUND);
	btrfs_directory_close(stream);
}

/* One crash state: mounted as is, or after explicit recovery, consistent. A
 * device without any superblock yet is only acceptable before mkfs finished. */
static void
check_state(struct state *state, const char *mark, int formatted)
{
	struct btrfs_recovery_report report;
	struct reference_audit references;
	struct namespace_audit names;
	struct btrfs_transaction *transaction;
	struct btrfs_inode root;
	struct btrfs_fs *fs;
	uint8_t supers[2][4096];
	uint64_t offsets[2] = { 65536, 64 * 1024 * 1024 };
	enum btrfs_result result;
	int i;

	for (i = 0; i < 2; i++) {
		memcpy(supers[i], state->device + offsets[i], sizeof(supers[i]));
	}
	result = btrfs_recover_supers(&state->environment, NULL, 0, &report);
	if (!formatted && (result == BTRFS_NOT_BTRFS || result == BTRFS_CORRUPT)) {
		state->empty++;
		return;
	}
	if (result == BTRFS_RECOVERY_REQUIRED) {
		REQUIRE(btrfs_recover_supers(&state->environment, &state->writer, 0, &report) ==
		    BTRFS_OK);
		state->recovered++;
	} else if (result != BTRFS_OK) {
		fprintf(stderr, "entry %zu%s: recovery decision %s\n", state->position,
		    state->subset ? " (subset)" : "", btrfs_result_string(result));
		exit(1);
	}
	REQUIRE(btrfs_mount(&state->environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	if (reference_audit(fs, &references) != 0) {
		fprintf(stderr, "entry %zu%s: reference audit: %s\n", state->position,
		    state->subset ? " (subset)" : "", references.failure);
		exit(1);
	}
	if (namespace_audit(fs, &names) != 0) {
		fprintf(stderr, "entry %zu%s: namespace audit: %s\n", state->position,
		    state->subset ? " (subset)" : "", names.failure);
		exit(1);
	}
	REQUIRE(btrfs_transaction_begin(fs, &state->writer, &transaction) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	REQUIRE(btrfs_root(fs, &root) == BTRFS_OK);
	read_tree(fs, &root, 0);
	if (mark != NULL) {
		expect_mark(fs, mark);
		state->marks++;
	}
	btrfs_unmount(fs);
	for (i = 0; i < 2; i++) {
		memcpy(state->device + offsets[i], supers[i], sizeof(supers[i]));
	}
	state->checked++;
}

static uint64_t
next_random(struct state *state)
{
	state->random ^= state->random << 13;
	state->random ^= state->random >> 7;
	state->random ^= state->random << 17;
	return state->random;
}

/* Subsets of the writes issued since the last flush, on top of the flushed
 * state: the device may persist any of them. The device holds every write
 * up to end; epoch records what the writes since flushed replaced. */
static void
check_subsets(
    struct state *state, size_t flushed, size_t end, struct undo_list *epoch, int formatted)
{
	struct undo_list subset = { NULL, 0, 0 };
	size_t i;
	unsigned sample;

	if (epoch->count < 2) {
		return;
	}
	for (sample = 0; sample < SUBSETS_PER_EPOCH; sample++) {
		/* Back to the flushed state, then a subset forward. */
		while (epoch->count != 0) {
			epoch->count--;
			memcpy(state->device + epoch->items[epoch->count].offset,
			    epoch->items[epoch->count].bytes, epoch->items[epoch->count].length);
			free(epoch->items[epoch->count].bytes);
		}
		for (i = flushed; i < end; i++) {
			if (next_random(state) % 2 == 0) {
				apply(state, &state->entries[i], &subset);
			}
		}
		state->subset = 1;
		check_state(state, NULL, formatted);
		state->subset = 0;
		state->subsets++;
		revert(state, &subset);
		for (i = flushed; i < end; i++) {
			apply(state, &state->entries[i], epoch);
		}
	}
	free(subset.items);
}

int
main(int argc, char **argv)
{
	struct state state;
	struct undo_list epoch = { NULL, 0, 0 };
	uint8_t *final;
	size_t i;
	size_t flushed = 0;
	int durable;
	int image;
	int formatted = 0;

	REQUIRE(argc == 3);
	memset(&state, 0, sizeof(state));
	for (i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
		generate(&inputs[i]);
	}
	state.random = UINT64_C(0x5eed);
	state.device = calloc(1, (size_t)DEVICE_BYTES);
	REQUIRE(state.device != NULL);
	state.log = open(argv[1], O_RDONLY);
	REQUIRE(state.log >= 0);
	state.environment.context = &state;
	state.environment.size_bytes = DEVICE_BYTES;
	state.environment.read = device_read;
	state.environment.allocate = allocate;
	state.environment.release = release;
	state.environment.decompress = NULL;
	state.writer = (struct btrfs_write_environment){ &state, device_write, device_flush, NULL,
		BTRFS_COMPRESSION_NONE };
	parse_log(&state);
	/* Epochs end at a flush, before its write, and at a FUA write or a mark,
	 * after it: a FUA write is durable once complete, and the workload issues
	 * nothing after a mark until its sync returned. Writes of one epoch may
	 * persist in any subset. Writes in flight across a FUA completion are kept
	 * in order, which omits some reachable states but adds none. */
	for (i = 0; i < state.count; i++) {
		state.position = i;
		durable = (state.entries[i].flags & (LOG_FUA_FLAG | LOG_MARK_FLAG)) != 0;
		if ((state.entries[i].flags & LOG_FLUSH_FLAG) != 0 || durable) {
			check_subsets(&state, flushed, i, &epoch, formatted);
			revert_forget(&epoch);
			flushed = i;
		}
		apply(&state, &state.entries[i], durable ? NULL : &epoch);
		if (durable) {
			flushed = i + 1;
		}
		if (state.entries[i].flags & LOG_MARK_FLAG) {
			formatted = 1;
			check_state(&state, state.entries[i].mark, formatted);
		} else if (formatted ||
		    (state.entries[i].flags & (LOG_FLUSH_FLAG | LOG_FUA_FLAG)) != 0 ||
		    i % PREFIX_STRIDE == 0 || i + 1 == state.count) {
			check_state(&state, NULL, formatted);
		}
	}
	image = open(argv[2], O_RDONLY);
	REQUIRE(image >= 0);
	final = malloc((size_t)DEVICE_BYTES);
	REQUIRE(final != NULL);
	REQUIRE(pread(image, final, (size_t)DEVICE_BYTES, 0) == (ssize_t)DEVICE_BYTES);
	REQUIRE(memcmp(final, state.device, (size_t)DEVICE_BYTES) == 0);
	printf("Linux-written crash states: %zu log entries, %zu states checked (%zu subsets of a "
	       "flush epoch, %zu after explicit recovery, %zu at marks, %zu before mkfs finished); "
	       "final image reproduced PASS\n",
	    state.count, state.checked, state.subsets, state.recovered, state.marks, state.empty);
	revert_forget(&epoch);
	free(epoch.items);
	close(image);
	close(state.log);
	free(final);
	free(state.device);
	free(state.entries);
	for (i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
		free(inputs[i].bytes);
	}
	return 0;
}
