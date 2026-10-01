/* SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 200809L
#include "../adapters/posix/image.h"
#include "encode.h"
#include "namespace_audit.h"
#include "references.h"
#include "space.h"
#include "transaction.h"
#include <btrfs/write.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

/* Device persistence is modeled per 512-byte sector: between two successful
 * barriers, issued writes may persist in any order and any subset of sectors. */
#define DEVICE_SECTOR 512U
#define MAX_WRITE (1024U * 1024U)
#define WRITE_SECTORS (MAX_WRITE / DEVICE_SECTOR)
#define INLINE_LIMIT 2048U
#define MAX_FILES 4096U
#define MAX_STAGES 4U
#define MAX_OPERATIONS 4096U
#define MAX_FILE_BYTES (UINT64_C(64) * 1024 * 1024)
#define MAX_RECOVERY_WRITES BTRFS_SUPER_COPIES
#define METADATA_SAMPLES 32U
#define EXPORT_SAMPLES 8U
#define EXPORT_PREFIXES 16U
#define BARRIERS 3U
#define SUPER_EPOCHS 2U
#define TEAR_PATTERNS 6U
#define MANY_ENTRIES 700U
#define BATCH_STRIDE 37U
/* Mirrors core/transaction.c: the dirty-node limit of one transaction. */
#define TRANSACTION_NODE_LIMIT 4096U
#define RESERVATION_PROBE_LIMIT 1048576U
#define NO_STAGE SIZE_MAX
#define ABSENT_TREE UINT64_C(4000)
#define SHARED_LAST 1599U
#define PAIR_LAST 119U
#define DATA_SCENARIO_BYTES 8192U
#define EXHAUSTION_WRITE_BYTES (16U * 1024U * 1024U)
#define FRAGMENT_WRITE_BYTES (2U * 1024U * 1024U)
#define FAULT_POINTS 512U
#define GROW_FILES 2000U
#define GROW_DATA_BYTES (40U * 1024U * 1024U)
#define SYNTHETIC_COMMIT SIZE_MAX
#define NO_FILE SIZE_MAX
#define MAX_EXPECTATIONS 512U
/* The largest xattr value one 64 KiB leaf item can hold. */
#define XATTR_VALUE_LIMIT 65536U
/* Linux keeps device numbers in inode items as its internal dev_t. */
#define LINUX_MINOR_BITS 20U
#define LINUX_MINOR_MASK ((UINT64_C(1) << LINUX_MINOR_BITS) - 1)
#define LAST_STAGE (MAX_STAGES - 1U)
#define NAMESPACE_UID 4242U
#define NAMESPACE_GID 4343U
#define NAMESPACE_DEVICE ((UINT64_C(1) << LINUX_MINOR_BITS) | 5U)
#define LINUX_NULL_DEVICE ((UINT64_C(1) << LINUX_MINOR_BITS) | 3U)
#define NAMESPACE_FILES 100U
#define NAMESPACE_DATA_BYTES 16384U
/* The namespace fixture's extended references: see tests/prepare_linux.py. */
#define EXTREF_NAME_BYTES 200U
#define EXTREF_LINKS 40U
#define COLLISION_BLOCK_BYTES 8U
#define COLLISION_BLOCKS 10U
#define COLLISION_NAME_BYTES (COLLISION_BLOCK_BYTES * COLLISION_BLOCKS)
/* Linux's PATH_MAX, one more than the longest symlink target. */
#define LINUX_PATH_MAX 4096U

enum fault { FAULT_NONE, FAULT_ALLOCATE, FAULT_READ, FAULT_WRITE, FAULT_FLUSH, FAULT_MODES };

struct saved_write {
	uint64_t offset;
	size_t length;
	uint8_t *bytes;
	uint8_t visible[WRITE_SECTORS];
	size_t commit;
	unsigned epoch;
};

struct device {
	struct btrfs_image *image;
	struct saved_write *writes;
	size_t count;
	size_t capacity;
	size_t durable;
	size_t commit;
	unsigned epoch;
	size_t issued;
	size_t flushes;
	size_t fail_write;
	size_t fail_flush;
	int immediate;
};

struct tracked {
	char path[32];
	uint8_t *data[MAX_STAGES];
	size_t size[MAX_STAGES];
};

enum operation_kind {
	OPERATION_INLINE,
	OPERATION_WRITE,
	OPERATION_TRUNCATE,
	OPERATION_CREATE,
	OPERATION_LINK,
	OPERATION_UNLINK,
	OPERATION_RENAME,
	OPERATION_SET_XATTR,
	OPERATION_REMOVE_XATTR,
	OPERATION_EVICT,
	OPERATION_CLEAN_ORPHANS
};

/* Operations name objects by path. A path created, renamed or removed by an
 * earlier operation of the same commit resolves to that result; other paths
 * resolve in the committed state. file is the tracked file of a data operation,
 * or NO_FILE for files the plan models through expectations. */
struct operation {
	size_t file;
	enum operation_kind kind;
	uint64_t offset;
	uint8_t *data;
	size_t size;
	char *path;
	char *target;
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	uint64_t device;
	int flags;
	struct btrfs_object_id id;
};

enum expectation_kind {
	EXPECT_ABSENT,
	EXPECT_FILE,
	EXPECT_DIRECTORY,
	EXPECT_SYMLINK,
	EXPECT_SAME,
	EXPECT_XATTR,
	EXPECT_NO_XATTR,
	EXPECT_STAT,
	EXPECT_DEVICE,
	EXPECT_FLAGS,
	EXPECT_FEATURE
};

/* A namespace fact that holds in stages first..last. bytes are file contents,
 * a symlink target, an xattr value or a directory listing (sorted names, each
 * followed by a newline); other is a second path or an xattr name. */
struct expectation {
	size_t first;
	size_t last;
	enum expectation_kind kind;
	char *path;
	char *other;
	uint8_t *bytes;
	size_t size;
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	uint32_t links;
	uint64_t value;
	/* EXPECT_FLAGS: the inode flags in mask must equal value. */
	uint64_t mask;
};

/* Objects named during one attempt: the committed lookup or the latest
 * namespace operation of the commit, in an open-addressing table sized for
 * every path the commit's operations name. */
struct path_entry {
	char *path;
	struct btrfs_object_id id;
	int present;
};

struct path_table {
	struct path_entry *entries;
	size_t capacity;
	size_t count;
};

/* Source-controlled operation sequence: stage 0 is the Linux fixture and stage
 * k is the expected logical state after the k-th acknowledged commit, computed
 * by applying that commit's operations to a byte model of each file. */
struct plan {
	const char *name;
	struct tracked *files;
	size_t file_count;
	size_t commits;
	struct operation *operations[MAX_STAGES];
	size_t operation_count[MAX_STAGES];
	/* Large commits bound the checked prefixes and fault points per class. */
	size_t prefix_points;
	size_t fault_points;
	/* Chunks the first commit must add from unallocated device space. */
	size_t new_chunks;
	/* Every file is modeled; states check every verify_stride-th one. */
	size_t verify_stride;
	struct expectation *expectations;
	size_t expectation_count;
	/* Namespace plans check their expectations and the namespace audit in
	 * every state. */
	int namespace;
};

struct totals {
	size_t writes;
	size_t flushes;
	uint64_t allocations;
	uint64_t reads;
};

struct outcome {
	size_t mounted;
	size_t resolved;
	int recovered;
	size_t recovery_count;
	struct saved_write recovery[MAX_RECOVERY_WRITES];
};

struct exporter {
	char directory[2048];
	FILE *cases;
	size_t next;
	size_t exported;
};

struct context {
	struct btrfs_image image;
	struct device *device;
	struct btrfs_environment env;
	struct btrfs_write_environment writer;
	uint64_t base_generation;
	uint32_t node_size;
	uint32_t sector_size;
	const char *export_root;
	size_t states;
	size_t recoveries;
	size_t audits;
	struct btrfs_fs *plan_fs;
	uint32_t seed;
};

static uint32_t
next_random(struct context *context)
{
	context->seed ^= context->seed << 13;
	context->seed ^= context->seed >> 17;
	context->seed ^= context->seed << 5;
	return context->seed;
}

static size_t
sectors(const struct saved_write *write)
{
	return write->length / DEVICE_SECTOR;
}

static void
show(struct saved_write *write, int visible)
{
	memset(write->visible, visible, sectors(write));
}

static void
overlay(void *bytes, uint64_t offset, size_t size, const struct saved_write *write, size_t sector,
    size_t count)
{
	uint64_t source = write->offset + sector * DEVICE_SECTOR;
	uint64_t start = offset > source ? offset : source;
	uint64_t end = offset + size < source + count * DEVICE_SECTOR
	    ? offset + size
	    : source + count * DEVICE_SECTOR;

	if (start < end) {
		memcpy((uint8_t *)bytes + (start - offset), write->bytes + (start - write->offset),
		    (size_t)(end - start));
	}
}

static enum btrfs_result
read_device(void *context, uint64_t offset, void *bytes, size_t size)
{
	struct device *device = context;
	const struct saved_write *write;
	size_t i;
	size_t sector;
	size_t run;
	enum btrfs_result error;

	error = device->image->environment.read(
	    device->image->environment.context, offset, bytes, size);
	for (i = 0; error == BTRFS_OK && i < device->count; i++) {
		write = &device->writes[i];
		if (write->offset >= offset + size || offset >= write->offset + write->length) {
			continue;
		}
		for (sector = 0; sector < sectors(write); sector += run) {
			for (run = 1; sector + run < sectors(write) &&
			    write->visible[sector + run] == write->visible[sector];
			    run++) {
			}
			if (write->visible[sector]) {
				overlay(bytes, offset, size, write, sector, run);
			}
		}
	}
	return error;
}

static enum btrfs_result
decompress_device(void *context, enum btrfs_compression codec, const void *input, size_t input_size,
    void *output, size_t output_size)
{
	struct device *device = context;

	return device->image->environment.decompress(
	    device->image->environment.context, codec, input, input_size, output, output_size);
}

static void *
allocate(void *context, size_t size)
{
	struct device *device = context;

	return device->image->environment.allocate(device->image->environment.context, size);
}

static void
release(void *context, void *bytes, size_t size)
{
	struct device *device = context;

	device->image->environment.release(device->image->environment.context, bytes, size);
}

static struct saved_write *
record(struct device *device, uint64_t offset, const void *bytes, size_t size)
{
	struct saved_write *write;
	struct saved_write *grown;

	REQUIRE(offset % DEVICE_SECTOR == 0 && size % DEVICE_SECTOR == 0 && size != 0 &&
	    size <= MAX_WRITE);
	REQUIRE(offset <= device->image->environment.size_bytes &&
	    size <= device->image->environment.size_bytes - offset);
	if (device->count == device->capacity) {
		device->capacity = device->capacity == 0 ? 256 : device->capacity * 2;
		grown = realloc(device->writes, device->capacity * sizeof(*grown));
		REQUIRE(grown != NULL);
		device->writes = grown;
	}
	write = &device->writes[device->count++];
	memset(write, 0, sizeof(*write));
	write->offset = offset;
	write->length = size;
	write->bytes = malloc(size);
	REQUIRE(write->bytes != NULL);
	memcpy(write->bytes, bytes, size);
	write->commit = device->commit;
	write->epoch = device->epoch;
	return write;
}

static enum btrfs_result
write_device(void *context, uint64_t offset, const void *bytes, size_t size)
{
	struct device *device = context;
	struct saved_write *write;

	device->issued++;
	if (device->issued == device->fail_write) {
		return BTRFS_IO;
	}
	write = record(device, offset, bytes, size);
	if (device->immediate) {
		show(write, 1);
	}
	return BTRFS_OK;
}

static enum btrfs_result
flush_device(void *context)
{
	struct device *device = context;

	device->flushes++;
	if (device->flushes == device->fail_flush) {
		return BTRFS_IO;
	}
	for (; device->durable < device->count; device->durable++) {
		show(&device->writes[device->durable], 1);
	}
	device->epoch++;
	return BTRFS_OK;
}

static void
truncate_writes(struct device *device, size_t count)
{
	while (device->count > count) {
		free(device->writes[--device->count].bytes);
	}
	if (device->durable > count) {
		device->durable = count;
	}
}

static void
synthetic(struct context *context, uint64_t offset, const void *bytes, size_t size)
{
	size_t commit = context->device->commit;

	context->device->commit = SYNTHETIC_COMMIT;
	show(record(context->device, offset, bytes, size), 1);
	context->device->commit = commit;
	context->device->durable = context->device->count;
}

static void
read_exact(struct context *context, uint64_t offset, void *bytes, size_t size)
{
	REQUIRE(context->env.read(context->env.context, offset, bytes, size) == BTRFS_OK);
}

static void
check_text(struct btrfs_fs *fs, const char *path, const char *expected)
{
	struct btrfs_inode inode;
	uint8_t buffer[64];
	size_t completed;

	REQUIRE(btrfs_image_lookup(fs, path, &inode) == BTRFS_OK);
	REQUIRE(btrfs_read(fs, &inode, 0, buffer, sizeof(buffer), &completed) == BTRFS_OK);
	REQUIRE(completed == strlen(expected) && memcmp(buffer, expected, completed) == 0);
}

/* Objects outside the transaction's write set keep their Linux contents. */
static void
check_invariants(struct btrfs_fs *fs)
{
	struct btrfs_inode inode;
	struct btrfs_inode link;
	uint8_t value[32];
	size_t length;

	REQUIRE(btrfs_image_lookup(fs, "/greeting", &inode) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/hardlink", &link) == BTRFS_OK);
	REQUIRE(inode.id.tree == link.id.tree && inode.id.inode == link.id.inode);
	REQUIRE(inode.links == 2 && inode.uid == 1001 && inode.gid == 1002 &&
	    (inode.mode & 07777U) == 0640);
	REQUIRE(btrfs_get_xattr(fs, &inode, "user.text", strlen("user.text"), value, sizeof(value),
		    &length) == BTRFS_OK);
	REQUIRE(length == strlen("Linux xattr") && memcmp(value, "Linux xattr", length) == 0);
	check_text(fs, "/snapshot/value", "snapshot original\n");
	check_text(fs, "/subvol/value", "subvolume changed\n");
}

static uint8_t *
read_file(struct btrfs_fs *fs, const char *path, size_t *size)
{
	struct btrfs_inode inode;
	uint8_t *buffer;
	size_t completed;

	REQUIRE(btrfs_image_lookup(fs, path, &inode) == BTRFS_OK);
	REQUIRE(inode.size <= MAX_FILE_BYTES);
	buffer = malloc((size_t)inode.size + 1);
	REQUIRE(buffer != NULL);
	REQUIRE(btrfs_read(fs, &inode, 0, buffer, (size_t)inode.size, &completed) == BTRFS_OK);
	REQUIRE(completed == inode.size);
	*size = completed;
	return buffer;
}

static int
compare_entries(const void *left, const void *right)
{
	const struct btrfs_dir_entry *a = left;
	const struct btrfs_dir_entry *b = right;
	size_t length = a->name_length < b->name_length ? a->name_length : b->name_length;
	int order = memcmp(a->name, b->name, length);

	if (order != 0) {
		return order;
	}
	return (a->name_length > b->name_length) - (a->name_length < b->name_length);
}

/* Lists a directory through DIR_INDEX enumeration and confirms that each name
 * resolves through its DIR_ITEM to the same object. */
static uint8_t *
list_directory(struct btrfs_fs *fs, const struct btrfs_inode *directory, size_t *size)
{
	struct btrfs_dir_entry *entries = NULL;
	struct btrfs_inode child;
	uint8_t *listing;
	uint64_t cookie = 0;
	size_t count = 0;
	size_t capacity = 0;
	size_t used = 0;
	size_t i;
	enum btrfs_result result;

	for (;;) {
		if (count == capacity) {
			capacity = capacity == 0 ? 64 : capacity * 2;
			entries = realloc(entries, capacity * sizeof(*entries));
			REQUIRE(entries != NULL);
		}
		result = btrfs_next_dir(fs, directory, &cookie, &entries[count]);
		if (result == BTRFS_NOT_FOUND) {
			break;
		}
		REQUIRE(result == BTRFS_OK);
		REQUIRE(btrfs_lookup(fs, directory, entries[count].name, entries[count].name_length,
			    &child) == BTRFS_OK);
		REQUIRE(child.id.tree == entries[count].id.tree &&
		    child.id.inode == entries[count].id.inode);
		used += entries[count].name_length + 1U;
		count++;
	}
	qsort(entries, count, sizeof(*entries), compare_entries);
	listing = malloc(used + 1);
	REQUIRE(listing != NULL);
	*size = 0;
	for (i = 0; i < count; i++) {
		memcpy(listing + *size, entries[i].name, entries[i].name_length);
		*size += entries[i].name_length;
		listing[(*size)++] = '\n';
	}
	free(entries);
	return listing;
}

static void
expectation_failed(
    const struct plan *plan, size_t stage, const struct expectation *expectation, const char *what)
{
	fprintf(stderr, "%s stage %zu: %s: %s\n", plan->name, stage, expectation->path, what);
	exit(1);
}

static void
check_expectation(
    struct btrfs_fs *fs, const struct plan *plan, size_t stage, const struct expectation *e)
{
	struct btrfs_inode inode;
	struct btrfs_inode other;
	struct btrfs_info info;
	uint8_t *bytes = NULL;
	size_t size = 0;
	enum btrfs_result result;

	result = btrfs_image_lookup(fs, e->path, &inode);
	if (e->kind == EXPECT_ABSENT) {
		if (result != BTRFS_NOT_FOUND) {
			expectation_failed(plan, stage, e, "present");
		}
		return;
	}
	if (result != BTRFS_OK) {
		expectation_failed(plan, stage, e, btrfs_result_string(result));
	}
	switch (e->kind) {
	case EXPECT_FILE:
		REQUIRE((inode.mode & BTRFS_MODE_TYPE) == BTRFS_MODE_REGULAR);
		bytes = read_file(fs, e->path, &size);
		break;
	case EXPECT_SYMLINK:
		REQUIRE((inode.mode & BTRFS_MODE_TYPE) == BTRFS_MODE_SYMLINK);
		bytes = read_file(fs, e->path, &size);
		break;
	case EXPECT_DIRECTORY:
		REQUIRE((inode.mode & BTRFS_MODE_TYPE) == BTRFS_MODE_DIRECTORY);
		if (inode.size != e->value) {
			expectation_failed(plan, stage, e, "directory size");
		}
		bytes = list_directory(fs, &inode, &size);
		break;
	case EXPECT_XATTR:
		bytes = malloc(XATTR_VALUE_LIMIT + 1);
		REQUIRE(bytes != NULL);
		result = btrfs_get_xattr(
		    fs, &inode, e->other, strlen(e->other), bytes, XATTR_VALUE_LIMIT, &size);
		if (result != BTRFS_OK) {
			expectation_failed(plan, stage, e, btrfs_result_string(result));
		}
		break;
	case EXPECT_NO_XATTR:
		result = btrfs_get_xattr(fs, &inode, e->other, strlen(e->other), NULL, 0, &size);
		if (result != BTRFS_NOT_FOUND) {
			expectation_failed(plan, stage, e, "xattr present");
		}
		return;
	case EXPECT_SAME:
		REQUIRE(btrfs_image_lookup(fs, e->other, &other) == BTRFS_OK);
		if (inode.id.tree != other.id.tree || inode.id.inode != other.id.inode) {
			expectation_failed(plan, stage, e, "different object");
		}
		return;
	case EXPECT_STAT:
		if (inode.mode != e->mode || inode.uid != e->uid || inode.gid != e->gid ||
		    inode.links != e->links) {
			fprintf(stderr, "%s stage %zu: %s: mode %o uid %u gid %u links %u\n",
			    plan->name, stage, e->path, inode.mode, inode.uid, inode.gid,
			    inode.links);
			exit(1);
		}
		return;
	case EXPECT_DEVICE:
		if (inode.device != e->value) {
			expectation_failed(plan, stage, e, "device number");
		}
		return;
	case EXPECT_FLAGS:
		if ((inode.flags & e->mask) != e->value) {
			expectation_failed(plan, stage, e, "inode flags");
		}
		return;
	case EXPECT_FEATURE:
		btrfs_get_info(fs, &info);
		if ((info.incompat_features & e->value) == 0) {
			expectation_failed(plan, stage, e, "incompat feature");
		}
		return;
	default:
		REQUIRE(0);
	}
	if (size != e->size || (size != 0 && memcmp(bytes, e->bytes, size) != 0)) {
		expectation_failed(plan, stage, e, "contents differ");
	}
	free(bytes);
}

static void
check_namespace(struct btrfs_fs *fs, const struct plan *plan, size_t stage)
{
	struct namespace_audit audit;
	size_t i;

	for (i = 0; i < plan->expectation_count; i++) {
		if (plan->expectations[i].first <= stage && stage <= plan->expectations[i].last) {
			check_expectation(fs, plan, stage, &plan->expectations[i]);
		}
	}
	if (namespace_audit(fs, &audit) != 0) {
		fprintf(stderr, "%s stage %zu: namespace audit: %s\n", plan->name, stage,
		    audit.failure);
		exit(1);
	}
}

static void
check_stage(struct context *context, struct btrfs_fs *fs, const struct plan *plan, size_t stage)
{
	struct btrfs_info info;
	const struct tracked *file;
	uint8_t *contents;
	size_t size;
	size_t i;

	btrfs_get_info(fs, &info);
	REQUIRE(info.generation == context->base_generation + stage);
	for (i = 0; i < plan->file_count; i += plan->verify_stride == 0 ? 1 : plan->verify_stride) {
		file = &plan->files[i];
		contents = read_file(fs, file->path, &size);
		if (size != file->size[stage] ||
		    (size != 0 && memcmp(contents, file->data[stage], size) != 0)) {
			fprintf(stderr, "%s stage %zu: %s differs (%zu bytes, expected %zu)\n",
			    plan->name, stage, file->path, size, file->size[stage]);
			exit(1);
		}
		free(contents);
	}
	check_invariants(fs);
	if (plan->namespace) {
		check_namespace(fs, plan, stage);
	}
}

static void
plan_init(struct plan *plan)
{
	size_t stage;

	memset(plan, 0, sizeof(*plan));
	plan->files = calloc(MAX_FILES, sizeof(*plan->files));
	REQUIRE(plan->files != NULL);
	for (stage = 1; stage < MAX_STAGES; stage++) {
		plan->operations[stage] = calloc(MAX_OPERATIONS, sizeof(*plan->operations[stage]));
		REQUIRE(plan->operations[stage] != NULL);
	}
	plan->fault_points = FAULT_POINTS;
	plan->expectations = calloc(MAX_EXPECTATIONS, sizeof(*plan->expectations));
	REQUIRE(plan->expectations != NULL);
}

/* Plans are built against the committed state, which no write changes until
 * run_plan releases this mount. */
static struct btrfs_fs *
plan_mount(struct context *context)
{
	if (context->plan_fs == NULL) {
		REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &context->plan_fs) ==
		    BTRFS_OK);
	}
	return context->plan_fs;
}

static size_t
plan_file(struct context *context, struct plan *plan, const char *path)
{
	struct btrfs_fs *fs;
	struct tracked *file;
	size_t i;

	for (i = 0; i < plan->file_count; i++) {
		if (strcmp(plan->files[i].path, path) == 0) {
			return i;
		}
	}
	REQUIRE(plan->file_count < MAX_FILES && strlen(path) < sizeof(file->path));
	file = &plan->files[plan->file_count];
	memset(file, 0, sizeof(*file));
	strcpy(file->path, path);
	fs = plan_mount(context);
	file->data[0] = read_file(fs, path, &file->size[0]);
	return plan->file_count++;
}

static void
plan_operation(struct context *context, struct plan *plan, size_t commit, const char *path,
    enum operation_kind kind, uint64_t offset, const void *data, size_t size)
{
	struct operation *operation;

	REQUIRE(
	    commit > 0 && commit < MAX_STAGES && plan->operation_count[commit] < MAX_OPERATIONS);
	REQUIRE(kind != OPERATION_INLINE || size <= INLINE_LIMIT);
	operation = &plan->operations[commit][plan->operation_count[commit]++];
	operation->file = plan_file(context, plan, path);
	operation->path = strdup(path);
	REQUIRE(operation->path != NULL);
	operation->kind = kind;
	operation->offset = offset;
	operation->size = size;
	operation->data = malloc(size + 1);
	REQUIRE(operation->data != NULL);
	if (size != 0) {
		memcpy(operation->data, data, size);
	}
	if (commit > plan->commits) {
		plan->commits = commit;
	}
}

static void
plan_update(struct context *context, struct plan *plan, size_t commit, const char *path,
    const void *data, size_t size)
{
	plan_operation(context, plan, commit, path, OPERATION_INLINE, 0, data, size);
}

static void
plan_write(struct context *context, struct plan *plan, size_t commit, const char *path,
    uint64_t offset, const void *data, size_t size)
{
	plan_operation(context, plan, commit, path, OPERATION_WRITE, offset, data, size);
}

static void
plan_truncate(
    struct context *context, struct plan *plan, size_t commit, const char *path, uint64_t size)
{
	plan_operation(context, plan, commit, path, OPERATION_TRUNCATE, size, NULL, 0);
}

static void
model_resize(struct tracked *file, size_t stage, uint64_t size)
{
	uint8_t *grown;

	REQUIRE(size <= MAX_FILE_BYTES);
	grown = realloc(file->data[stage], (size_t)size + 1);
	REQUIRE(grown != NULL);
	if (size > file->size[stage]) {
		memset(grown + file->size[stage], 0, (size_t)size - file->size[stage]);
	}
	file->data[stage] = grown;
	file->size[stage] = (size_t)size;
}

static void
plan_finish(struct plan *plan)
{
	const struct operation *operation;
	struct tracked *file;
	size_t stage;
	size_t i;

	for (stage = 1; stage <= plan->commits; stage++) {
		for (i = 0; i < plan->file_count; i++) {
			file = &plan->files[i];
			file->size[stage] = file->size[stage - 1];
			file->data[stage] = malloc(file->size[stage] + 1);
			REQUIRE(file->data[stage] != NULL);
			memcpy(file->data[stage], file->data[stage - 1], file->size[stage]);
		}
		for (i = 0; i < plan->operation_count[stage]; i++) {
			operation = &plan->operations[stage][i];
			if (operation->file == NO_FILE || operation->kind > OPERATION_TRUNCATE) {
				continue;
			}
			file = &plan->files[operation->file];
			if (operation->kind == OPERATION_INLINE) {
				model_resize(file, stage, operation->size);
				memcpy(file->data[stage], operation->data, operation->size);
			} else if (operation->kind == OPERATION_WRITE) {
				if (operation->offset + operation->size > file->size[stage]) {
					model_resize(
					    file, stage, operation->offset + operation->size);
				}
				memcpy(file->data[stage] + operation->offset, operation->data,
				    operation->size);
			} else {
				model_resize(file, stage, operation->offset);
			}
		}
	}
}

static void
plan_destroy(struct plan *plan)
{
	size_t stage;
	size_t i;

	for (i = 0; i < plan->file_count; i++) {
		for (stage = 0; stage < MAX_STAGES; stage++) {
			free(plan->files[i].data[stage]);
		}
	}
	for (stage = 0; stage < MAX_STAGES; stage++) {
		for (i = 0; i < plan->operation_count[stage]; i++) {
			free(plan->operations[stage][i].data);
			free(plan->operations[stage][i].path);
			free(plan->operations[stage][i].target);
		}
		free(plan->operations[stage]);
	}
	for (i = 0; i < plan->expectation_count; i++) {
		free(plan->expectations[i].path);
		free(plan->expectations[i].other);
		free(plan->expectations[i].bytes);
	}
	free(plan->expectations);
	free(plan->files);
}

/* Classify a durable state the way an owner must: mount the primary, then ask
 * explicit recovery. Every state must resolve to the acknowledged or the newest
 * stage, never a mixture, never below acknowledgement, and remain admissible. */
static void
resolve(struct context *context, const struct plan *plan, size_t acknowledged, size_t latest,
    struct outcome *outcome)
{
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct btrfs_recovery_report report;
	struct btrfs_recovery_report applied;
	struct btrfs_info info;
	struct device *device = context->device;
	uint64_t floor = context->base_generation + acknowledged;
	size_t writes = device->count;
	size_t stage;
	size_t i;
	enum btrfs_result result;

	memset(outcome, 0, sizeof(*outcome));
	outcome->mounted = NO_STAGE;
	result = btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs);
	REQUIRE(result == BTRFS_OK || result == BTRFS_CORRUPT);
	if (result == BTRFS_OK) {
		btrfs_get_info(fs, &info);
		stage = (size_t)(info.generation - context->base_generation);
		REQUIRE(stage == acknowledged || stage == latest);
		check_stage(context, fs, plan, stage);
		btrfs_unmount(fs);
		outcome->mounted = stage;
	}
	result = btrfs_recover_supers(&context->env, NULL, floor, &report);
	REQUIRE(result == BTRFS_OK || result == BTRFS_RECOVERY_REQUIRED);
	REQUIRE(report.present == 2 && report.selected < BTRFS_SUPER_COPIES);
	stage = (size_t)(report.generation - context->base_generation);
	REQUIRE(stage == acknowledged || stage == latest);
	REQUIRE(outcome->mounted != NO_STAGE || result == BTRFS_RECOVERY_REQUIRED);
	outcome->resolved = stage;
	if (result == BTRFS_OK) {
		REQUIRE(stage == outcome->mounted && report.rewritten == 0);
	} else {
		device->immediate = 1;
		REQUIRE(btrfs_recover_supers(&context->env, &context->writer, floor, &applied) ==
		    BTRFS_OK);
		device->immediate = 0;
		REQUIRE(applied.generation == report.generation && applied.rewritten != 0 &&
		    applied.selected == report.selected);
		REQUIRE(device->count - writes == applied.rewritten);
		REQUIRE(device->count - writes <= MAX_RECOVERY_WRITES);
		for (i = writes; i < device->count; i++) {
			outcome->recovery[outcome->recovery_count++] = device->writes[i];
			outcome->recovery[outcome->recovery_count - 1].bytes =
			    malloc(device->writes[i].length);
			REQUIRE(outcome->recovery[outcome->recovery_count - 1].bytes != NULL);
			memcpy(outcome->recovery[outcome->recovery_count - 1].bytes,
			    device->writes[i].bytes, device->writes[i].length);
		}
		REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
		check_stage(context, fs, plan, stage);
		btrfs_unmount(fs);
		REQUIRE(btrfs_recover_supers(&context->env, NULL, floor, &applied) == BTRFS_OK &&
		    applied.rewritten == 0 && applied.generation == report.generation);
		outcome->recovered = 1;
		context->recoveries++;
	}
	/* A resolved state is consistent and admits the next transaction. */
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	REQUIRE(device->count == writes + outcome->recovery_count);
	truncate_writes(device, writes);
	REQUIRE(context->image.live_allocations == 0);
	context->states++;
}

static void
outcome_release(struct outcome *outcome)
{
	size_t i;

	for (i = 0; i < outcome->recovery_count; i++) {
		free(outcome->recovery[i].bytes);
	}
	outcome->recovery_count = 0;
}

static void
export_path(const struct exporter *exporter, const char *name, char *path, size_t size)
{
	REQUIRE(snprintf(path, size, "%s/%s", exporter->directory, name) < (int)size);
}

static FILE *
export_open(const struct exporter *exporter, const char *name)
{
	char path[4096];
	FILE *file;

	export_path(exporter, name, path, sizeof(path));
	file = fopen(path, "wx");
	REQUIRE(file != NULL);
	return file;
}

static void
export_bytes(const struct exporter *exporter, const char *name, const void *bytes, size_t size)
{
	FILE *file = export_open(exporter, name);

	REQUIRE(size == 0 || fwrite(bytes, 1, size, file) == size);
	REQUIRE(fclose(file) == 0);
}

/* namespace.tsv: stage, kind, path, argument and payload file per line, for
 * the Linux oracle to check the same facts on the mounted state. */
static void
export_namespace(struct context *context, const struct plan *plan, struct exporter *exporter)
{
	static const char *const kinds[] = { "absent", "file", "dir", "symlink", "same", "xattr",
		"noxattr", "stat", "device", "flags", "feature" };
	const struct expectation *e;
	char payload[64];
	char argument[64];
	const char *detail;
	FILE *manifest;
	size_t stage;
	size_t i;

	(void)context;
	manifest = export_open(exporter, "namespace.tsv");
	for (i = 0; i < plan->expectation_count; i++) {
		e = &plan->expectations[i];
		strcpy(payload, "-");
		if (e->kind == EXPECT_FILE || e->kind == EXPECT_SYMLINK ||
		    e->kind == EXPECT_DIRECTORY || e->kind == EXPECT_XATTR) {
			REQUIRE(snprintf(payload, sizeof(payload), "expect-%03zu.bin", i) <
			    (int)sizeof(payload));
			export_bytes(exporter, payload, e->bytes, e->size);
		}
		detail = "-";
		if (e->kind == EXPECT_SAME || e->kind == EXPECT_XATTR ||
		    e->kind == EXPECT_NO_XATTR) {
			detail = e->other;
		} else if (e->kind == EXPECT_DIRECTORY) {
			REQUIRE(snprintf(argument, sizeof(argument), "%llu",
				    (unsigned long long)e->value) < (int)sizeof(argument));
			detail = argument;
		} else if (e->kind == EXPECT_STAT) {
			/* stat -c '%f:%u:%g:%h' */
			REQUIRE(snprintf(argument, sizeof(argument), "%x:%u:%u:%u", e->mode, e->uid,
				    e->gid, e->links) < (int)sizeof(argument));
			detail = argument;
		} else if (e->kind == EXPECT_DEVICE) {
			/* stat -c '%t:%T' of a Linux dev_t stored as MAJOR << 20 | MINOR. */
			REQUIRE(snprintf(argument, sizeof(argument), "%llx:%llx",
				    (unsigned long long)(e->value >> LINUX_MINOR_BITS),
				    (unsigned long long)(e->value & LINUX_MINOR_MASK)) <
			    (int)sizeof(argument));
			detail = argument;
		} else if (e->kind == EXPECT_FLAGS) {
			/* The inode item flags in mask equal value. */
			REQUIRE(snprintf(argument, sizeof(argument), "0x%llx:0x%llx",
				    (unsigned long long)e->mask,
				    (unsigned long long)e->value) < (int)sizeof(argument));
			detail = argument;
		} else if (e->kind == EXPECT_FEATURE) {
			REQUIRE(e->value == BT_FEATURE_COMPRESS_LZO ||
			    e->value == BT_FEATURE_COMPRESS_ZSTD);
			detail =
			    e->value == BT_FEATURE_COMPRESS_LZO ? "COMPRESS_LZO" : "COMPRESS_ZSTD";
		}
		for (stage = e->first; stage <= e->last && stage <= plan->commits; stage++) {
			REQUIRE(fprintf(manifest, "%zu\t%s\t%s\t%s\t%s\n", stage, kinds[e->kind],
				    e->path, detail, payload) > 0);
		}
	}
	REQUIRE(fclose(manifest) == 0);
}

static void
export_begin(struct context *context, const struct plan *plan, struct exporter *exporter)
{
	char name[64];
	FILE *stages;
	size_t stage;
	size_t earlier;
	size_t i;
	size_t j = 0;
	int found;

	memset(exporter, 0, sizeof(*exporter));
	if (context->export_root == NULL) {
		return;
	}
	REQUIRE(snprintf(exporter->directory, sizeof(exporter->directory), "%s/%s",
		    context->export_root, plan->name) < (int)sizeof(exporter->directory));
	REQUIRE(mkdir(exporter->directory, 0755) == 0);
	stages = export_open(exporter, "stages.tsv");
	for (stage = 0; stage <= plan->commits; stage++) {
		for (i = 0; i < plan->file_count; i++) {
			/* Identical contents (snapshot copies, unchanged stages) share a file. */
			found = 0;
			for (earlier = 0; !found && earlier <= stage; earlier++) {
				for (j = 0; !found && j < (earlier == stage ? i : plan->file_count);
				    j++) {
					found = plan->files[j].size[earlier] ==
						plan->files[i].size[stage] &&
					    memcmp(plan->files[j].data[earlier],
						plan->files[i].data[stage],
						plan->files[i].size[stage]) == 0;
				}
			}
			if (found) {
				REQUIRE(snprintf(name, sizeof(name), "stage-%zu-%02zu.bin",
					    earlier - 1, j - 1) < (int)sizeof(name));
			} else {
				REQUIRE(snprintf(name, sizeof(name), "stage-%zu-%02zu.bin", stage,
					    i) < (int)sizeof(name));
				export_bytes(exporter, name, plan->files[i].data[stage],
				    plan->files[i].size[stage]);
			}
			REQUIRE(fprintf(stages, "%zu\t%llu\t%s\t%s\n", stage,
				    (unsigned long long)(context->base_generation + stage),
				    plan->files[i].path, name) > 0);
		}
	}
	REQUIRE(fclose(stages) == 0);
	if (plan->namespace) {
		export_namespace(context, plan, exporter);
	}
	exporter->cases = export_open(exporter, "cases.tsv");
}

static void
export_writes(struct context *context, struct exporter *exporter)
{
	const struct saved_write *write;
	char name[64];
	FILE *manifest;

	if (exporter->cases == NULL) {
		return;
	}
	manifest = exporter->exported == 0 ? export_open(exporter, "writes.tsv") : NULL;
	if (manifest == NULL) {
		char path[4096];

		export_path(exporter, "writes.tsv", path, sizeof(path));
		manifest = fopen(path, "a");
		REQUIRE(manifest != NULL);
	}
	for (; exporter->exported < context->device->count; exporter->exported++) {
		write = &context->device->writes[exporter->exported];
		REQUIRE(write->commit != SYNTHETIC_COMMIT);
		REQUIRE(snprintf(name, sizeof(name), "write-%04zu.bin", exporter->exported) <
		    (int)sizeof(name));
		export_bytes(exporter, name, write->bytes, write->length);
		REQUIRE(fprintf(manifest, "%zu\t%llu\t%zu\t%s\t%zu\t%u\n", exporter->exported,
			    (unsigned long long)write->offset, write->length, name, write->commit,
			    write->epoch) > 0);
	}
	REQUIRE(fclose(manifest) == 0);
}

/* A case lists every visible sector run of the issued writes, in issue order,
 * followed by the explicit recovery writes this implementation selected. */
static void
export_case(struct context *context, struct exporter *exporter, const char *kind, size_t commit,
    const struct outcome *outcome)
{
	const struct saved_write *write;
	char name[64];
	char mounted[32];
	FILE *fragments;
	size_t sector;
	size_t run;
	size_t i;

	if (exporter->cases == NULL) {
		return;
	}
	REQUIRE(snprintf(name, sizeof(name), "case-%04zu.tsv", exporter->next) < (int)sizeof(name));
	fragments = export_open(exporter, name);
	for (i = 0; i < context->device->count; i++) {
		write = &context->device->writes[i];
		for (sector = 0; sector < sectors(write); sector += run) {
			for (run = 1; sector + run < sectors(write) &&
			    write->visible[sector + run] == write->visible[sector];
			    run++) {
			}
			if (write->visible[sector]) {
				REQUIRE(fprintf(fragments, "%llu\t%zu\twrite-%04zu.bin\t%zu\n",
					    (unsigned long long)(write->offset / DEVICE_SECTOR +
						sector),
					    run, i, sector) > 0);
			}
		}
	}
	REQUIRE(fclose(fragments) == 0);
	REQUIRE(
	    snprintf(name, sizeof(name), "recover-%04zu.tsv", exporter->next) < (int)sizeof(name));
	fragments = export_open(exporter, name);
	for (i = 0; i < outcome->recovery_count; i++) {
		REQUIRE(snprintf(name, sizeof(name), "recover-%04zu-%zu.bin", exporter->next, i) <
		    (int)sizeof(name));
		export_bytes(
		    exporter, name, outcome->recovery[i].bytes, outcome->recovery[i].length);
		REQUIRE(fprintf(fragments, "%llu\t%zu\t%s\t0\n",
			    (unsigned long long)(outcome->recovery[i].offset / DEVICE_SECTOR),
			    outcome->recovery[i].length / DEVICE_SECTOR, name) > 0);
	}
	REQUIRE(fclose(fragments) == 0);
	if (outcome->mounted == NO_STAGE) {
		strcpy(mounted, "-");
	} else {
		REQUIRE(snprintf(mounted, sizeof(mounted), "%zu", outcome->mounted) <
		    (int)sizeof(mounted));
	}
	REQUIRE(fprintf(exporter->cases, "%04zu\t%zu\t%s\t%s\t%zu\t%d\n", exporter->next, commit,
		    kind, mounted, outcome->resolved, outcome->recovered) > 0);
	exporter->next++;
}

static void
export_end(struct exporter *exporter)
{
	if (exporter->cases != NULL) {
		REQUIRE(fclose(exporter->cases) == 0);
		exporter->cases = NULL;
	}
}

static void
evaluate(struct context *context, const struct plan *plan, struct exporter *exporter, size_t commit,
    const char *kind, int exported)
{
	struct outcome outcome;

	resolve(context, plan, commit - 1, commit, &outcome);
	if (exported) {
		export_case(context, exporter, kind, commit, &outcome);
	}
	outcome_release(&outcome);
}

static int
tear(unsigned pattern, size_t sector, size_t count)
{
	switch (pattern) {
	case 0:
		return 0;
	case 1:
		return 1;
	case 2:
		return sector == 0;
	case 3:
		return sector + 1 < count;
	case 4:
		return sector % 2 == 0;
	default:
		return sector + 1 == count;
	}
}

/* Crash states of one recorded commit. Earlier commits stay durable. Prefixes
 * follow issue order; epoch states persist arbitrary subsets of the writes issued
 * after the last successful barrier, including torn superblock copies. */
static void
crash_states(struct context *context, const struct plan *plan, struct exporter *exporter,
    size_t commit, size_t first)
{
	struct device *device = context->device;
	struct saved_write *write;
	size_t last = device->count;
	size_t count = last - first;
	size_t prefix;
	size_t sample;
	size_t sector;
	size_t i;
	size_t bucket;
	size_t stride;
	size_t exported = 0;
	unsigned epoch;
	unsigned pattern;
	unsigned choice;

	/* Every prefix is checked unless the plan bounds them; about EXPORT_PREFIXES
	 * evenly spaced ones are exported. */
	stride =
	    plan->prefix_points == 0 ? 1 : (count + plan->prefix_points - 1) / plan->prefix_points;
	for (prefix = 0; prefix <= count;
	    prefix = prefix < count && prefix + stride > count ? count : prefix + stride) {
		for (i = first; i < last; i++) {
			show(&device->writes[i], i - first < prefix);
		}
		bucket = prefix * (EXPORT_PREFIXES - 1) / count;
		evaluate(context, plan, exporter, commit, "prefix",
		    prefix == 0 || prefix == count || bucket != exported);
		exported = bucket;
	}
	for (epoch = 0; epoch < BARRIERS; epoch++) {
		for (sample = 0; sample < (epoch == 0 ? METADATA_SAMPLES + 2 : TEAR_PATTERNS);
		    sample++) {
			for (i = first; i < last; i++) {
				write = &device->writes[i];
				show(write, write->epoch < epoch);
				if (write->epoch != epoch) {
					continue;
				}
				if (epoch != 0) {
					/* One copy per superblock epoch on these devices. */
					for (sector = 0; sector < sectors(write); sector++) {
						write->visible[sector] = (uint8_t)tear(
						    (unsigned)sample, sector, sectors(write));
					}
					continue;
				}
				choice = sample < 2 ? (unsigned)sample : next_random(context) % 4;
				for (sector = 0; sector < sectors(write); sector++) {
					write->visible[sector] = (uint8_t)(choice == 1 ||
					    (choice == 2 && next_random(context) % 2 != 0));
				}
				if (choice == 3) {
					pattern =
					    next_random(context) % (unsigned)(sectors(write) + 1);
					memset(write->visible, 1, pattern);
				}
			}
			evaluate(context, plan, exporter, commit,
			    epoch == 0	     ? "metadata"
				: epoch == 1 ? "secondary"
					     : "primary",
			    epoch != 0 || sample < EXPORT_SAMPLES + 2);
		}
	}
	for (i = first; i < last; i++) {
		show(&device->writes[i], 1);
	}
}

static void
path_table_init(struct path_table *table, size_t paths)
{
	table->capacity = 16;
	while (table->capacity < 2 * paths) {
		table->capacity *= 2;
	}
	table->entries = calloc(table->capacity, sizeof(*table->entries));
	REQUIRE(table->entries != NULL);
	table->count = 0;
}

/* The slot of path: its entry, or the empty slot where it belongs. */
static struct path_entry *
path_slot(struct path_table *table, const char *path)
{
	uint64_t hash = UINT64_C(14695981039346656037);
	const char *byte;
	size_t slot;

	for (byte = path; *byte != '\0'; byte++) {
		hash = (hash ^ (uint8_t)*byte) * UINT64_C(1099511628211);
	}
	for (slot = (size_t)hash & (table->capacity - 1);
	    table->entries[slot].path != NULL && strcmp(table->entries[slot].path, path) != 0;
	    slot = (slot + 1) & (table->capacity - 1)) {
	}
	return &table->entries[slot];
}

static void
path_set(struct path_table *table, const char *path, struct btrfs_object_id id, int present)
{
	struct path_entry *entry = path_slot(table, path);

	if (entry->path == NULL) {
		REQUIRE(2 * (table->count + 1) <= table->capacity);
		entry->path = strdup(path);
		REQUIRE(entry->path != NULL);
		table->count++;
	}
	entry->id = id;
	entry->present = present;
}

/* Records the committed object of path (or its parent directory). */
static void
path_prepare(struct path_table *table, struct btrfs_fs *fs, const char *path, int parent)
{
	struct btrfs_object_id none = { 0, 0 };
	struct btrfs_inode inode;
	char copy[BTRFS_NAME_MAX * 4];
	const char *slash;
	enum btrfs_result result;

	REQUIRE(strlen(path) < sizeof(copy));
	strcpy(copy, path);
	if (parent) {
		slash = strrchr(copy, '/');
		REQUIRE(slash != NULL);
		copy[slash == copy ? 1 : (size_t)(slash - copy)] = '\0';
	}
	if (path_slot(table, copy)->path != NULL) {
		return;
	}
	result = btrfs_image_lookup(fs, copy, &inode);
	REQUIRE(result == BTRFS_OK || result == BTRFS_NOT_FOUND);
	path_set(table, copy, result == BTRFS_OK ? inode.id : none, result == BTRFS_OK);
}

static struct btrfs_object_id
path_object(struct path_table *table, const char *path, int parent, const char **leaf)
{
	struct path_entry *entry;
	char copy[BTRFS_NAME_MAX * 4];
	const char *slash = strrchr(path, '/');

	REQUIRE(slash != NULL && strlen(path) < sizeof(copy));
	strcpy(copy, path);
	if (parent) {
		copy[slash == path ? 1 : (size_t)(slash - path)] = '\0';
		*leaf = slash + 1;
	}
	entry = path_slot(table, copy);
	REQUIRE(entry->path != NULL && entry->present);
	return entry->id;
}

static void
path_table_release(struct path_table *table)
{
	size_t i;

	for (i = 0; i < table->capacity; i++) {
		free(table->entries[i].path);
	}
	free(table->entries);
}

static void
prepare_paths(struct path_table *table, struct btrfs_fs *fs, const struct operation *operation)
{
	switch (operation->kind) {
	case OPERATION_CREATE:
		path_prepare(table, fs, operation->path, 1);
		break;
	case OPERATION_LINK:
		path_prepare(table, fs, operation->path, 0);
		path_prepare(table, fs, operation->target, 1);
		break;
	case OPERATION_UNLINK:
		path_prepare(table, fs, operation->path, 1);
		break;
	case OPERATION_RENAME:
		path_prepare(table, fs, operation->path, 0);
		path_prepare(table, fs, operation->path, 1);
		path_prepare(table, fs, operation->target, 1);
		break;
	case OPERATION_EVICT:
	case OPERATION_CLEAN_ORPHANS:
		break;
	default:
		path_prepare(table, fs, operation->path, 0);
		break;
	}
}

static enum btrfs_result
execute(struct btrfs_transaction *transaction, struct path_table *table,
    const struct operation *operation, struct btrfs_time time)
{
	struct btrfs_new_inode attributes;
	struct btrfs_object_id none = { 0, 0 };
	struct btrfs_object_id parent;
	struct btrfs_object_id target;
	struct btrfs_object_id id;
	const char *leaf = NULL;
	const char *new_leaf = NULL;
	size_t cleaned;
	enum btrfs_result result;

	switch (operation->kind) {
	case OPERATION_INLINE:
		return btrfs_transaction_write_inline(transaction,
		    path_object(table, operation->path, 0, NULL), operation->data, operation->size,
		    time);
	case OPERATION_WRITE:
		return btrfs_transaction_write(transaction,
		    path_object(table, operation->path, 0, NULL), operation->offset,
		    operation->data, operation->size, time);
	case OPERATION_TRUNCATE:
		return btrfs_transaction_truncate(transaction,
		    path_object(table, operation->path, 0, NULL), operation->offset, time);
	case OPERATION_CREATE:
		parent = path_object(table, operation->path, 1, &leaf);
		memset(&attributes, 0, sizeof(attributes));
		attributes.mode = operation->mode;
		attributes.uid = operation->uid;
		attributes.gid = operation->gid;
		attributes.device = operation->device;
		attributes.time = time;
		attributes.target = operation->size != 0 ? operation->data : NULL;
		attributes.target_length = operation->size;
		result = btrfs_transaction_create(
		    transaction, parent, leaf, strlen(leaf), &attributes, &id);
		if (result == BTRFS_OK) {
			path_set(table, operation->path, id, 1);
		}
		return result;
	case OPERATION_LINK:
		id = path_object(table, operation->path, 0, NULL);
		parent = path_object(table, operation->target, 1, &leaf);
		result = btrfs_transaction_link(transaction, id, parent, leaf, strlen(leaf), time);
		if (result == BTRFS_OK) {
			path_set(table, operation->target, id, 1);
		}
		return result;
	case OPERATION_UNLINK:
		parent = path_object(table, operation->path, 1, &leaf);
		result = btrfs_transaction_unlink(
		    transaction, parent, leaf, strlen(leaf), time, operation->flags);
		if (result == BTRFS_OK) {
			path_set(table, operation->path, none, 0);
		}
		return result;
	case OPERATION_RENAME:
		id = path_object(table, operation->path, 0, NULL);
		parent = path_object(table, operation->path, 1, &leaf);
		target = path_object(table, operation->target, 1, &new_leaf);
		result = btrfs_transaction_rename(transaction, parent, leaf, strlen(leaf), target,
		    new_leaf, strlen(new_leaf), time, operation->flags);
		if (result == BTRFS_OK) {
			path_set(table, operation->path, none, 0);
			path_set(table, operation->target, id, 1);
		}
		return result;
	case OPERATION_SET_XATTR:
		return btrfs_transaction_set_xattr(transaction,
		    path_object(table, operation->path, 0, NULL), operation->target,
		    strlen(operation->target), operation->data, operation->size, operation->flags,
		    time);
	case OPERATION_REMOVE_XATTR:
		return btrfs_transaction_remove_xattr(transaction,
		    path_object(table, operation->path, 0, NULL), operation->target,
		    strlen(operation->target), time);
	case OPERATION_EVICT:
		return btrfs_transaction_evict(transaction, operation->id);
	case OPERATION_CLEAN_ORPHANS:
		result = btrfs_transaction_clean_orphans(transaction, operation->id.tree, &cleaned);
		if (result == BTRFS_OK && cleaned != operation->size) {
			fprintf(stderr, "cleaned %zu orphans, expected %zu\n", cleaned,
			    operation->size);
			exit(1);
		}
		return result;
	}
	return BTRFS_INVALID_ARGUMENT;
}

static enum btrfs_result
attempt(struct context *context, const struct plan *plan, size_t commit, enum fault fault,
    size_t point, struct totals *totals)
{
	struct path_table table;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction = NULL;
	struct btrfs_time time = { 1700000000 + (int64_t)commit, 123456789 };
	struct device *device = context->device;
	uint64_t allocations;
	uint64_t reads;
	size_t i;
	enum btrfs_result result;

	/* An operation names at most three paths and creates or moves at most one. */
	path_table_init(&table, 4 * plan->operation_count[commit]);
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	for (i = 0; i < plan->operation_count[commit]; i++) {
		prepare_paths(&table, fs, &plan->operations[commit][i]);
	}
	device->commit = commit;
	device->epoch = 0;
	device->issued = 0;
	device->flushes = 0;
	device->fail_write = fault == FAULT_WRITE ? point : 0;
	device->fail_flush = fault == FAULT_FLUSH ? point : 0;
	allocations = context->image.allocations;
	reads = context->image.reads;
	context->image.fail_allocate = fault == FAULT_ALLOCATE ? allocations + point : 0;
	context->image.fail_read = fault == FAULT_READ ? reads + point : 0;
	result = btrfs_transaction_begin(fs, &context->writer, &transaction);
	for (i = 0; result == BTRFS_OK && i < plan->operation_count[commit]; i++) {
		result = execute(transaction, &table, &plan->operations[commit][i], time);
	}
	if (result == BTRFS_OK) {
		result = btrfs_transaction_commit(transaction);
	}
	totals->writes = device->issued;
	totals->flushes = device->flushes;
	totals->allocations = context->image.allocations - allocations;
	totals->reads = context->image.reads - reads;
	context->image.fail_allocate = 0;
	context->image.fail_read = 0;
	device->fail_write = 0;
	device->fail_flush = 0;
	if (transaction != NULL &&
	    (fault == FAULT_NONE || fault == FAULT_WRITE || fault == FAULT_FLUSH ||
		result == BTRFS_OK)) {
		/* Success and uncertain persistence are both terminal. */
		i = device->count;
		REQUIRE(btrfs_transaction_commit(transaction) ==
		    (result == BTRFS_OK ? BTRFS_READ_ONLY : result));
		REQUIRE(device->count == i);
	}
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	path_table_release(&table);
	REQUIRE(context->image.live_allocations == 0);
	return result;
}

static void
fault_sweeps(struct context *context, const struct plan *plan, size_t commit, size_t first,
    const struct totals *totals)
{
	struct outcome outcome;
	struct totals ignored;
	struct device *device = context->device;
	size_t limits[FAULT_MODES];
	size_t failures[FAULT_MODES] = { 0 };
	size_t point;
	size_t stride;
	enum fault fault;
	enum btrfs_result result;

	limits[FAULT_NONE] = 0;
	limits[FAULT_ALLOCATE] = (size_t)totals->allocations;
	limits[FAULT_READ] = (size_t)totals->reads;
	limits[FAULT_WRITE] = totals->writes;
	limits[FAULT_FLUSH] = totals->flushes;
	for (fault = FAULT_ALLOCATE; fault < FAULT_MODES; fault++) {
		/* Every point up to FAULT_POINTS per class; larger commits use a
		 * deterministic stride that keeps the first and last points. */
		stride = (limits[fault] + plan->fault_points - 1) / plan->fault_points;
		for (point = 1; point <= limits[fault];
		    point = point < limits[fault] && point + stride > limits[fault]
			? limits[fault]
			: point + stride) {
			result = attempt(context, plan, commit, fault, point, &ignored);
			if (fault == FAULT_ALLOCATE) {
				REQUIRE(result == BTRFS_NO_MEMORY);
			} else if (fault == FAULT_READ) {
				/* A DUP copy may satisfy a failed metadata read. */
				REQUIRE(result == BTRFS_IO || result == BTRFS_OK);
			} else {
				REQUIRE(result == BTRFS_IO);
			}
			if (result != BTRFS_OK) {
				failures[fault]++;
			}
			if (fault == FAULT_ALLOCATE ||
			    (fault == FAULT_READ && result != BTRFS_OK)) {
				REQUIRE(device->count == first);
			}
			resolve(context, plan, commit - 1, commit, &outcome);
			REQUIRE(result == BTRFS_OK || device->count != first ||
			    outcome.resolved == commit - 1);
			outcome_release(&outcome);
			truncate_writes(device, first);
		}
		REQUIRE(failures[fault] != 0);
	}
}

static size_t
chunk_count(struct context *context)
{
	struct btrfs_fs *fs;
	size_t count;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	count = fs->chunk_count;
	btrfs_unmount(fs);
	return count;
}

static void
check_growth(struct context *context, const struct plan *plan)
{
	struct device *device = context->device;
	size_t count = device->count;
	size_t grown = chunk_count(context);
	size_t before;

	/* Compare with the committed state preceding this plan's first commit. */
	while (device->count != 0 && device->writes[device->count - 1].commit != SYNTHETIC_COMMIT &&
	    device->writes[device->count - 1].commit >= 1) {
		device->count--;
	}
	before = chunk_count(context);
	device->count = count;
	REQUIRE(grown >= before + plan->new_chunks);
	printf("%s: %zu chunks before, %zu after\n", plan->name, before, grown);
}

/* Every committed root set must satisfy the independent reference and namespace
 * audits. */
static void
audit_state(struct context *context, const char *name)
{
	struct reference_audit audit;
	struct namespace_audit names;
	struct btrfs_fs *fs;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	if (reference_audit(fs, &audit) != 0) {
		fprintf(stderr, "%s: reference audit: %s\n", name, audit.failure);
		exit(1);
	}
	if (namespace_audit(fs, &names) != 0) {
		fprintf(stderr, "%s: namespace audit: %s\n", name, names.failure);
		exit(1);
	}
	btrfs_unmount(fs);
	REQUIRE(context->image.live_allocations == 0);
	printf("%s references: %zu blocks, tree %zu, shared block %zu, data %zu, shared data %zu, "
	       "keyed %zu, full-backref blocks %zu\n",
	    name, audit.blocks, audit.tree_refs, audit.shared_block_refs, audit.data_refs,
	    audit.shared_data_refs, audit.keyed_refs, audit.full_backref_blocks);
	context->audits++;
}

static void
run_plan(struct context *context, struct plan *plan)
{
	struct exporter exporter;
	struct totals totals;
	struct btrfs_fs *fs;
	struct device *device = context->device;
	size_t commit;
	size_t first;
	size_t states = context->states;
	size_t recoveries = context->recoveries;
	enum btrfs_result result;

	btrfs_unmount(context->plan_fs);
	context->plan_fs = NULL;
	plan_finish(plan);
	export_begin(context, plan, &exporter);
	for (commit = 1; commit <= plan->commits; commit++) {
		first = device->count;
		result = attempt(context, plan, commit, FAULT_NONE, 0, &totals);
		if (result != BTRFS_OK) {
			fprintf(stderr, "%s commit %zu: %s\n", plan->name, commit,
			    btrfs_result_string(result));
			exit(1);
		}
		REQUIRE(totals.flushes == BARRIERS && totals.writes == device->count - first);
		REQUIRE(device->writes[device->count - 1].offset == BT_SUPER_OFFSET);
		printf("%s commit %zu: %zu writes, %zu barriers, %llu allocations, %llu reads\n",
		    plan->name, commit, totals.writes, totals.flushes,
		    (unsigned long long)totals.allocations, (unsigned long long)totals.reads);
		export_writes(context, &exporter);
		audit_state(context, plan->name);
		if (commit == 1 && plan->new_chunks != 0) {
			check_growth(context, plan);
		}
		crash_states(context, plan, &exporter, commit, first);
		if (commit == plan->commits) {
			truncate_writes(device, first);
			fault_sweeps(context, plan, commit, first, &totals);
			REQUIRE(attempt(context, plan, commit, FAULT_NONE, 0, &totals) == BTRFS_OK);
		}
	}
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	check_stage(context, fs, plan, plan->commits);
	btrfs_unmount(fs);
	export_end(&exporter);
	printf("%s: %zu crash states, %zu explicit recoveries PASS\n", plan->name,
	    context->states - states, context->recoveries - recoveries);
	truncate_writes(device, 0);
	plan_destroy(plan);
}

static void
plan_scenarios(struct context *context)
{
	static const char replacement[] = "written by Machlin CoW transaction\n";
	struct plan plan;
	uint8_t data[INLINE_LIMIT];
	char path[32];
	size_t i;
	size_t entry;

	plan_init(&plan);
	plan.name = "replace";
	plan_update(context, &plan, 1, "/greeting", replacement, sizeof(replacement) - 1);
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "empty";
	plan_update(context, &plan, 1, "/greeting", NULL, 0);
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "maximum";
	for (i = 0; i < INLINE_LIMIT; i++) {
		data[i] = (uint8_t)(i * 7 + 3);
	}
	plan_update(context, &plan, 1, "/greeting", data, INLINE_LIMIT);
	run_plan(context, &plan);

	/* Many inodes in several leaves, with growing items, in one transaction. */
	plan_init(&plan);
	plan.name = "batch";
	plan_update(context, &plan, 1, "/greeting", replacement, sizeof(replacement) - 1);
	for (entry = 0; entry < MANY_ENTRIES; entry += BATCH_STRIDE) {
		REQUIRE(
		    snprintf(path, sizeof(path), "/many/entry-%04zu", entry) < (int)sizeof(path));
		for (i = 0; i < 16 + entry % 200; i++) {
			data[i] = (uint8_t)('a' + (entry + i) % 26);
		}
		plan_update(context, &plan, 1, path, data, i);
	}
	run_plan(context, &plan);

	/* The second commit starts from a Machlin-written root set. */
	plan_init(&plan);
	plan.name = "repeated";
	plan_update(context, &plan, 1, "/greeting", "first Machlin commit\n", 21);
	plan_update(context, &plan, 1, "/many/entry-0001", "one\n", 4);
	for (i = 0; i < 300; i++) {
		data[i] = (uint8_t)('A' + i % 23);
	}
	plan_update(context, &plan, 2, "/greeting", data, 300);
	plan_update(context, &plan, 2, "/many/entry-0002", NULL, 0);
	run_plan(context, &plan);
}

static void
shared_path(char *path, size_t size, const char *tree, size_t index)
{
	REQUIRE(snprintf(path, size, "/%s/inline/f%04zu", tree, index) < (int)size);
}

static void
shared_update(
    struct context *context, struct plan *plan, size_t commit, const char *tree, size_t index)
{
	static const char *const trees[] = { "shared", "shared-snap", "shared-ro" };
	char path[32];
	char data[64];
	size_t i;
	int length;

	/* Track every snapshot's copy so isolation is checked at each stage. */
	for (i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
		shared_path(path, sizeof(path), trees[i], index);
		(void)plan_file(context, plan, path);
	}
	shared_path(path, sizeof(path), tree, index);
	length = snprintf(data, sizeof(data), "%s %zu in commit %zu\n", tree, index, commit);
	REQUIRE(length > 0 && (size_t)length < sizeof(data));
	plan_update(context, plan, commit, path, data, (size_t)length);
}

static void
pair_update(
    struct context *context, struct plan *plan, size_t commit, const char *tree, size_t index)
{
	char path[32];
	char data[64];
	int length;

	REQUIRE(snprintf(path, sizeof(path), "/pair/i%03zu", index) < (int)sizeof(path));
	(void)plan_file(context, plan, path);
	REQUIRE(snprintf(path, sizeof(path), "/pair-snap/i%03zu", index) < (int)sizeof(path));
	(void)plan_file(context, plan, path);
	REQUIRE(snprintf(path, sizeof(path), "/%s/i%03zu", tree, index) < (int)sizeof(path));
	length = snprintf(data, sizeof(data), "%s %zu in commit %zu\n", tree, index, commit);
	REQUIRE(length > 0 && (size_t)length < sizeof(data));
	plan_update(context, plan, commit, path, data, (size_t)length);
}

/* Subvolume trees shared with a writable and a read-only snapshot. The last
 * inline file shares a leaf with regular data extents, and earlier writes by
 * Linux left parent-named references and FULL_BACKREF blocks. */
static void
shared_scenarios(struct context *context)
{
	static const size_t spread[] = { 0, 400, 800, 1200, SHARED_LAST };
	struct plan plan;
	size_t i;

	plan_init(&plan);
	plan.name = "shared-source";
	for (i = 0; i < sizeof(spread) / sizeof(spread[0]); i++) {
		shared_update(context, &plan, 1, "shared", spread[i]);
	}
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "shared-snapshot";
	for (i = 0; i < sizeof(spread) / sizeof(spread[0]); i++) {
		shared_update(context, &plan, 1, "shared-snap", spread[i]);
	}
	run_plan(context, &plan);

	/* The source converts shared blocks first; the snapshot then CoWs blocks
	 * with parent-named references, and the source continues afterwards. */
	plan_init(&plan);
	plan.name = "shared-alternate";
	shared_update(context, &plan, 1, "shared", 100);
	shared_update(context, &plan, 1, "shared", SHARED_LAST);
	shared_update(context, &plan, 2, "shared-snap", 100);
	shared_update(context, &plan, 2, "shared-snap", SHARED_LAST);
	shared_update(context, &plan, 3, "shared", 101);
	shared_update(context, &plan, 3, "shared-snap", 101);
	run_plan(context, &plan);

	/* The tail file's leaf holds the reflinked extent and the two references
	 * to one extent at different extent offsets. */
	plan_init(&plan);
	plan.name = "shared-extents";
	plan_file(context, &plan, "/shared-ro/tail");
	plan_update(context, &plan, 1, "/shared/tail", "source tail\n", 12);
	plan_update(context, &plan, 2, "/shared-snap/tail", "snapshot tail\n", 14);
	run_plan(context, &plan);

	/* Leaves shared by exactly two trees hold data references. The source moves
	 * them to parent-named references; the snapshot then holds the last
	 * reference, converts them back and frees the old leaves. */
	plan_init(&plan);
	plan.name = "pair-convert";
	pair_update(context, &plan, 1, "pair", 5);
	pair_update(context, &plan, 1, "pair", 60);
	pair_update(context, &plan, 1, "pair", PAIR_LAST);
	pair_update(context, &plan, 2, "pair-snap", 5);
	pair_update(context, &plan, 2, "pair-snap", 60);
	pair_update(context, &plan, 2, "pair-snap", PAIR_LAST);
	pair_update(context, &plan, 3, "pair", 6);
	pair_update(context, &plan, 3, "pair-snap", 61);
	run_plan(context, &plan);

	/* One transaction spanning three trees. */
	plan_init(&plan);
	plan.name = "shared-trees";
	plan_update(context, &plan, 1, "/greeting", "three trees\n", 12);
	shared_update(context, &plan, 1, "shared", 700);
	shared_update(context, &plan, 1, "shared-snap", 701);
	run_plan(context, &plan);
}

static void
keyed_update(
    struct context *context, struct plan *plan, size_t commit, const char *tree, const char *name)
{
	static const char *const trees[] = { "keyed", "keyed-07", "keyed-29" };
	char path[32];
	char data[64];
	size_t i;
	int length;

	for (i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
		REQUIRE(snprintf(path, sizeof(path), "/%s/%s", trees[i], name) < (int)sizeof(path));
		(void)plan_file(context, plan, path);
	}
	REQUIRE(snprintf(path, sizeof(path), "/%s/%s", tree, name) < (int)sizeof(path));
	length = snprintf(data, sizeof(data), "%s %s in commit %zu\n", tree, name, commit);
	REQUIRE(length > 0 && (size_t)length < sizeof(data));
	plan_update(context, plan, commit, path, data, (size_t)length);
}

/* Blocks referenced by 31 trees and an extent referenced by 31 reflinks carry
 * more references than one extent item lists inline; the rest are keyed items.
 * The last inline file shares a leaf with all reflinked file extents. */
static void
keyed_scenarios(struct context *context)
{
	struct plan plan;

	plan_init(&plan);
	plan.name = "keyed-source";
	keyed_update(context, &plan, 1, "keyed", "i00");
	keyed_update(context, &plan, 1, "keyed", "i20");
	keyed_update(context, &plan, 1, "keyed", "last");
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "keyed-snapshot";
	keyed_update(context, &plan, 1, "keyed-07", "i00");
	keyed_update(context, &plan, 1, "keyed-07", "last");
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "keyed-alternate";
	keyed_update(context, &plan, 1, "keyed", "last");
	keyed_update(context, &plan, 2, "keyed-07", "last");
	keyed_update(context, &plan, 3, "keyed-29", "last");
	keyed_update(context, &plan, 3, "keyed", "i39");
	run_plan(context, &plan);
}

static void
fill_pattern(uint8_t *data, size_t size, unsigned seed)
{
	size_t i;

	for (i = 0; i < size; i++) {
		data[i] = (uint8_t)(seed + i * 131U + (i >> 9));
	}
}

static void
track_data(struct context *context, struct plan *plan, const char *name)
{
	static const char *const trees[] = { "data", "data-snap", "data-ro" };
	char path[32];
	size_t i;

	for (i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
		REQUIRE(snprintf(path, sizeof(path), "/%s/%s", trees[i], name) < (int)sizeof(path));
		(void)plan_file(context, plan, path);
	}
}

/* File data written as new extents: unaligned edges are rewritten from the
 * transaction's view, old extents are trimmed, moved or split, and snapshots,
 * reflinks, preallocation, compression and checksum policy are preserved. */
static void
data_scenarios(struct context *context)
{
	static uint8_t data[DATA_SCENARIO_BYTES];
	struct plan plan;

	plan_init(&plan);
	plan.name = "data-overwrite";
	track_data(context, &plan, "big");
	plan_file(context, &plan, "/data/big-clone");
	fill_pattern(data, 8192, 1);
	plan_write(context, &plan, 1, "/data/big", 300000, data, 8192);
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "data-append";
	track_data(context, &plan, "small");
	fill_pattern(data, 5000, 2);
	plan_write(context, &plan, 1, "/data/small", 10000, data, 5000);
	fill_pattern(data, 10, 3);
	plan_write(context, &plan, 1, "/data/small", 4090, data, 10);
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "data-hole";
	track_data(context, &plan, "sparse");
	fill_pattern(data, 4096, 4);
	plan_write(context, &plan, 1, "/data/sparse", 2 * 1024 * 1024, data, 4096);
	fill_pattern(data, 100, 5);
	plan_write(context, &plan, 1, "/data/sparse", 6 * 1024 * 1024 + 7, data, 100);
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "data-prealloc";
	track_data(context, &plan, "prealloc");
	fill_pattern(data, 4096, 6);
	plan_write(context, &plan, 1, "/data/prealloc", 65536, data, 4096);
	fill_pattern(data, 100, 7);
	plan_write(context, &plan, 1, "/data/prealloc", 1000, data, 100);
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "data-truncate";
	track_data(context, &plan, "big");
	track_data(context, &plan, "small");
	track_data(context, &plan, "sparse");
	track_data(context, &plan, "zlib");
	plan_truncate(context, &plan, 1, "/data/big", 100001);
	plan_truncate(context, &plan, 1, "/data/small", 20000);
	plan_truncate(context, &plan, 1, "/data/sparse", 0);
	plan_truncate(context, &plan, 2, "/data/big", 300000);
	plan_truncate(context, &plan, 2, "/data/zlib", 5000);
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "data-compressed";
	track_data(context, &plan, "zlib");
	fill_pattern(data, 4096, 8);
	plan_write(context, &plan, 1, "/data/zlib", 65536, data, 4096);
	fill_pattern(data, 7, 9);
	plan_write(context, &plan, 1, "/data/zlib", 100003, data, 7);
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "data-nodatasum";
	track_data(context, &plan, "nodatasum");
	fill_pattern(data, 5000, 10);
	plan_write(context, &plan, 1, "/data/nodatasum", 3000, data, 5000);
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "data-inline";
	track_data(context, &plan, "inline");
	fill_pattern(data, 5000, 11);
	plan_write(context, &plan, 1, "/data/inline", 0, data, 5000);
	plan_write(context, &plan, 2, "/data/inline", 9000, "X", 1);
	run_plan(context, &plan);

	/* The source and its writable snapshot overwrite shared extents in turn;
	 * the third commit drops the first commit's extent entirely. */
	plan_init(&plan);
	plan.name = "data-snapshot";
	track_data(context, &plan, "big");
	fill_pattern(data, 4096, 12);
	plan_write(context, &plan, 1, "/data/big", 0, data, 4096);
	fill_pattern(data, 8192, 13);
	plan_write(context, &plan, 2, "/data-snap/big", 4096, data, 8192);
	fill_pattern(data, 4096, 14);
	plan_write(context, &plan, 3, "/data/big", 0, data, 4096);
	run_plan(context, &plan);

	/* Overlapping writes in one transaction read each other's staged data. */
	plan_init(&plan);
	plan.name = "data-overlap";
	track_data(context, &plan, "small");
	fill_pattern(data, 4096, 15);
	plan_write(context, &plan, 1, "/data/small", 0, data, 4096);
	fill_pattern(data, 4096, 16);
	plan_write(context, &plan, 1, "/data/small", 2048, data, 4096);
	plan_truncate(context, &plan, 1, "/data/small", 3000);
	run_plan(context, &plan);
}

/* A data block group whose free space Linux keeps as bitmaps: freeing a file
 * between two holes merges runs, and a write larger than the first group's
 * free tail allocates the remaining sectors from bitmap holes. */
static void
fragment_scenarios(struct context *context)
{
	uint8_t *data;
	struct plan plan;

	data = malloc(FRAGMENT_WRITE_BYTES);
	REQUIRE(data != NULL);
	plan_init(&plan);
	plan.name = "fst-free";
	plan_truncate(context, &plan, 1, "/fragment/f001", 0);
	plan_truncate(context, &plan, 1, "/fragment/f003", 0);
	plan_truncate(context, &plan, 2, "/fragment/f255", 1000);
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "fst-fill";
	track_data(context, &plan, "small");
	fill_pattern(data, FRAGMENT_WRITE_BYTES, 17);
	plan_write(context, &plan, 1, "/data/small", 0, data, FRAGMENT_WRITE_BYTES);
	run_plan(context, &plan);
	free(data);
}

/* Classes that run out allocate chunks from unallocated device space: metadata
 * while growing two thousand leaf-sized inline files, data for one large write. */
static void
grow_scenarios(struct context *context)
{
	struct plan plan;
	char path[32];
	uint8_t *data;
	size_t i;

	data = malloc(GROW_DATA_BYTES);
	REQUIRE(data != NULL);
	plan_init(&plan);
	plan.name = "grow-metadata";
	plan.prefix_points = 64;
	plan.fault_points = 16;
	plan.new_chunks = 1;
	plan.verify_stride = 50;
	fill_pattern(data, INLINE_LIMIT, 18);
	for (i = 0; i < GROW_FILES; i++) {
		REQUIRE(snprintf(path, sizeof(path), "/meta/f%zu", i) < (int)sizeof(path));
		plan_update(context, &plan, 1, path, data, INLINE_LIMIT);
	}
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "grow-data";
	plan.fault_points = 32;
	plan.new_chunks = 1;
	fill_pattern(data, GROW_DATA_BYTES, 19);
	plan_write(context, &plan, 1, "/big", 0, data, GROW_DATA_BYTES);
	run_plan(context, &plan);
	free(data);
}

/* The namespace fixture: four names share one CRC32C name hash (also as
 * user. xattrs); Linux wrote the first two. /ns/extref/target has more links
 * than its INODE_REF item holds, so Linux stored the rest as extended
 * references. */
static const char *const collisions[] = { "ethvq997ethvq997", "ethvq997wdkjavbx",
	"wdkjavbxethvq997", "wdkjavbxwdkjavbx" };
static const char *const collision_blocks[] = { "ethvq997", "wdkjavbx" };

static void
extref_path(char *path, size_t size, size_t index)
{
	char name[EXTREF_NAME_BYTES + 1];

	memset(name, 'x', EXTREF_NAME_BYTES);
	name[EXTREF_NAME_BYTES] = '\0';
	REQUIRE(snprintf(path, size, "/ns/extref/l%03zu%s", index, name) < (int)size);
}

/* Names of COLLISION_BLOCKS blocks, each one of collision_blocks: equal-length
 * CRC32C collisions stay collisions when concatenated. */
static void
collision_name(char *name, size_t index)
{
	size_t block;

	for (block = 0; block < COLLISION_BLOCKS; block++) {
		memcpy(name + block * COLLISION_BLOCK_BYTES,
		    collision_blocks[(index >> block) & 1U], COLLISION_BLOCK_BYTES);
	}
	name[COLLISION_BLOCKS * COLLISION_BLOCK_BYTES] = '\0';
}

static size_t
item_limit(const struct context *context)
{
	return context->node_size - sizeof(struct bt_disk_header) - sizeof(struct bt_disk_item);
}

static struct operation *
plan_namespace(struct plan *plan, size_t commit, enum operation_kind kind, const char *path,
    const char *target)
{
	struct operation *operation;

	REQUIRE(
	    commit > 0 && commit < MAX_STAGES && plan->operation_count[commit] < MAX_OPERATIONS);
	operation = &plan->operations[commit][plan->operation_count[commit]++];
	memset(operation, 0, sizeof(*operation));
	operation->file = NO_FILE;
	operation->kind = kind;
	operation->path = strdup(path);
	operation->target = target == NULL ? NULL : strdup(target);
	operation->data = malloc(1);
	REQUIRE(operation->path != NULL && (target == NULL || operation->target != NULL) &&
	    operation->data != NULL);
	if (commit > plan->commits) {
		plan->commits = commit;
	}
	plan->namespace = 1;
	return operation;
}

static void
operation_data(struct operation *operation, const void *data, size_t size)
{
	free(operation->data);
	operation->data = malloc(size + 1);
	REQUIRE(operation->data != NULL);
	if (size != 0) {
		memcpy(operation->data, data, size);
	}
	operation->size = size;
}

static void
plan_create(struct plan *plan, size_t commit, const char *path, uint32_t mode, const char *symlink)
{
	struct operation *operation = plan_namespace(plan, commit, OPERATION_CREATE, path, NULL);

	operation->mode = mode;
	operation->uid = NAMESPACE_UID;
	operation->gid = NAMESPACE_GID;
	if (symlink != NULL) {
		operation_data(operation, symlink, strlen(symlink));
	}
}

static void
plan_device(struct plan *plan, size_t commit, const char *path, uint32_t mode, uint64_t device)
{
	plan_create(plan, commit, path, mode, NULL);
	plan->operations[commit][plan->operation_count[commit] - 1].device = device;
}

static void
plan_link(struct plan *plan, size_t commit, const char *path, const char *target)
{
	(void)plan_namespace(plan, commit, OPERATION_LINK, path, target);
}

static void
plan_unlink(struct plan *plan, size_t commit, const char *path, int open)
{
	plan_namespace(plan, commit, OPERATION_UNLINK, path, NULL)->flags = open;
}

static void
plan_rename(struct plan *plan, size_t commit, const char *path, const char *target, int target_open)
{
	plan_namespace(plan, commit, OPERATION_RENAME, path, target)->flags = target_open;
}

static void
plan_set_xattr(struct plan *plan, size_t commit, const char *path, const char *name,
    const void *value, size_t size, int flags)
{
	struct operation *operation = plan_namespace(plan, commit, OPERATION_SET_XATTR, path, name);

	operation_data(operation, value, size);
	operation->flags = flags;
}

static void
plan_remove_xattr(struct plan *plan, size_t commit, const char *path, const char *name)
{
	(void)plan_namespace(plan, commit, OPERATION_REMOVE_XATTR, path, name);
}

static void
plan_write_new(struct plan *plan, size_t commit, const char *path, uint64_t offset,
    const void *data, size_t size)
{
	struct operation *operation = plan_namespace(plan, commit, OPERATION_WRITE, path, NULL);

	operation_data(operation, data, size);
	operation->offset = offset;
}

/* The inode is named at plan time; it has no name when the commit runs. */
static void
plan_evict(struct context *context, struct plan *plan, size_t commit, const char *path)
{
	struct btrfs_inode inode;

	REQUIRE(btrfs_image_lookup(plan_mount(context), path, &inode) == BTRFS_OK);
	plan_namespace(plan, commit, OPERATION_EVICT, path, NULL)->id = inode.id;
}

static void
plan_clean(struct plan *plan, size_t commit, uint64_t tree, size_t expected)
{
	struct operation *operation =
	    plan_namespace(plan, commit, OPERATION_CLEAN_ORPHANS, "/", NULL);

	operation->id.tree = tree;
	operation->size = expected;
}

static struct expectation *
expect(struct plan *plan, size_t first, size_t last, enum expectation_kind kind, const char *path)
{
	struct expectation *e;

	REQUIRE(plan->expectation_count < MAX_EXPECTATIONS && first <= last);
	e = &plan->expectations[plan->expectation_count++];
	memset(e, 0, sizeof(*e));
	e->first = first;
	e->last = last;
	e->kind = kind;
	e->path = strdup(path);
	REQUIRE(e->path != NULL);
	plan->namespace = 1;
	return e;
}

static void
expectation_bytes(struct expectation *e, const void *bytes, size_t size)
{
	e->bytes = malloc(size + 1);
	REQUIRE(e->bytes != NULL);
	if (size != 0) {
		memcpy(e->bytes, bytes, size);
	}
	e->size = size;
}

static void
expect_absent(struct plan *plan, size_t first, size_t last, const char *path)
{
	(void)expect(plan, first, last, EXPECT_ABSENT, path);
}

static void
expect_file(
    struct plan *plan, size_t first, size_t last, const char *path, const void *bytes, size_t size)
{
	expectation_bytes(expect(plan, first, last, EXPECT_FILE, path), bytes, size);
}

static void
expect_text(struct plan *plan, size_t first, size_t last, const char *path, const char *text)
{
	expect_file(plan, first, last, path, text, strlen(text));
}

/* Contents of source in the committed state, expected at path. */
static void
expect_current(struct context *context, struct plan *plan, size_t first, size_t last,
    const char *path, const char *source)
{
	uint8_t *bytes;
	size_t size;

	bytes = read_file(plan_mount(context), source, &size);
	expect_file(plan, first, last, path, bytes, size);
	free(bytes);
}

static void
expect_symlink(struct plan *plan, size_t first, size_t last, const char *path, const char *target)
{
	expectation_bytes(expect(plan, first, last, EXPECT_SYMLINK, path), target, strlen(target));
}

static void
expect_same(struct plan *plan, size_t first, size_t last, const char *path, const char *other)
{
	struct expectation *e = expect(plan, first, last, EXPECT_SAME, path);

	e->other = strdup(other);
	REQUIRE(e->other != NULL);
}

/* value NULL expects the xattr to be absent. */
static void
expect_xattr(struct plan *plan, size_t first, size_t last, const char *path, const char *name,
    const void *value, size_t size)
{
	struct expectation *e =
	    expect(plan, first, last, value == NULL ? EXPECT_NO_XATTR : EXPECT_XATTR, path);

	e->other = strdup(name);
	REQUIRE(e->other != NULL);
	if (value != NULL) {
		expectation_bytes(e, value, size);
	}
}

static void
expect_stat(
    struct plan *plan, size_t first, size_t last, const char *path, uint32_t mode, uint32_t links)
{
	struct expectation *e = expect(plan, first, last, EXPECT_STAT, path);

	e->mode = mode;
	e->uid = NAMESPACE_UID;
	e->gid = NAMESPACE_GID;
	e->links = links;
}

/* Owner and mode of source in the committed state, with links at path. */
static void
expect_links(struct context *context, struct plan *plan, size_t first, size_t last,
    const char *path, const char *source, uint32_t links)
{
	struct btrfs_inode inode;
	struct expectation *e = expect(plan, first, last, EXPECT_STAT, path);

	REQUIRE(btrfs_image_lookup(plan_mount(context), source, &inode) == BTRFS_OK);
	e->mode = inode.mode;
	e->uid = inode.uid;
	e->gid = inode.gid;
	e->links = links;
}

static void
expect_value(struct plan *plan, size_t first, size_t last, enum expectation_kind kind,
    const char *path, uint64_t value)
{
	struct expectation *e = expect(plan, first, last, kind, path);

	e->value = value;
	e->mask = value;
}

/* The inode flags in mask equal value. */
static void
expect_flags(
    struct plan *plan, size_t first, size_t last, const char *path, uint64_t mask, uint64_t value)
{
	struct expectation *e = expect(plan, first, last, EXPECT_FLAGS, path);

	e->value = value;
	e->mask = mask;
}

static int
compare_names(const void *left, const void *right)
{
	const char *a = *(const char *const *)left;
	const char *b = *(const char *const *)right;

	return strcmp(a, b);
}

/* A directory listing: names (sorted bytewise, each followed by a newline)
 * and the size Linux keeps, twice the length of the names. */
static void
expect_names(struct plan *plan, size_t first, size_t last, const char *path, const char **names,
    size_t count)
{
	struct expectation *e = expect(plan, first, last, EXPECT_DIRECTORY, path);
	size_t used = 0;
	size_t i;

	if (count > 1) {
		qsort(names, count, sizeof(*names), compare_names);
	}
	for (i = 0; i < count; i++) {
		used += strlen(names[i]) + 1;
	}
	e->bytes = malloc(used + 1);
	REQUIRE(e->bytes != NULL);
	for (i = 0; i < count; i++) {
		memcpy(e->bytes + e->size, names[i], strlen(names[i]));
		e->size += strlen(names[i]);
		e->bytes[e->size++] = '\n';
		e->value += 2 * (uint64_t)strlen(names[i]);
	}
}

/* The committed listing of path with names added and removed (NULL-terminated
 * lists, either may be NULL). */
static void
expect_listing(struct context *context, struct plan *plan, size_t first, size_t last,
    const char *path, const char *const *added, const char *const *removed)
{
	struct btrfs_inode directory;
	const char **names;
	uint8_t *listing;
	size_t size;
	size_t count = 0;
	size_t start = 0;
	size_t i;
	size_t j;

	REQUIRE(btrfs_image_lookup(plan_mount(context), path, &directory) == BTRFS_OK);
	listing = list_directory(context->plan_fs, &directory, &size);
	names = calloc(size + 64, sizeof(*names));
	REQUIRE(names != NULL);
	for (i = 0; i < size; i++) {
		if (listing[i] == '\n') {
			listing[i] = '\0';
			names[count++] = (const char *)listing + start;
			start = i + 1;
		}
	}
	for (i = 0; removed != NULL && removed[i] != NULL; i++) {
		for (j = 0; j < count && strcmp(names[j], removed[i]) != 0; j++) {
		}
		REQUIRE(j < count);
		names[j] = names[--count];
	}
	for (i = 0; added != NULL && added[i] != NULL; i++) {
		names[count++] = added[i];
	}
	expect_names(plan, first, last, path, names, count);
	free(names);
	free(listing);
}

static void
namespace_plan(struct context *context, struct plan *plan, const char *name)
{
	plan_init(plan);
	plan->name = name;
	plan->namespace = 1;
	/* stages.tsv names every stage's generation through a tracked file. */
	(void)plan_file(context, plan, "/greeting");
}

/* New objects of every type, inherited inode flags, data in new files, and
 * enough entries in the second commit to split leaves. */
static void
namespace_create_plan(struct context *context)
{
	static const char *const added[] = { "new", NULL };
	static uint8_t data[NAMESPACE_DATA_BYTES];
	static char names[NAMESPACE_FILES][8];
	const char *listing[NAMESPACE_FILES + 8];
	struct plan plan;
	char path[64];
	size_t count = 0;
	size_t i;

	namespace_plan(context, &plan, "namespace-create");
	plan_create(&plan, 1, "/ns/new", BTRFS_MODE_DIRECTORY | 0755, NULL);
	plan_create(&plan, 1, "/ns/new/file", BTRFS_MODE_REGULAR | 0644, NULL);
	fill_pattern(data, 10000, 21);
	plan_write_new(&plan, 1, "/ns/new/file", 0, data, 10000);
	plan_create(&plan, 1, "/ns/new/link", BTRFS_MODE_SYMLINK | 0777, "../tree/a/b/deep");
	plan_device(&plan, 1, "/ns/new/char", BTRFS_MODE_CHARACTER | 0620, NAMESPACE_DEVICE);
	plan_create(&plan, 1, "/ns/new/pipe", BTRFS_MODE_FIFO | 0600, NULL);
	plan_create(&plan, 1, "/ns/new/sock", BTRFS_MODE_SOCKET | 0755, NULL);
	plan_create(&plan, 1, "/ns/nocow/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/ns/nocow/file", 0, data, 5000);
	plan_create(&plan, 1, "/ns/compress/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/ns/compress/file", 0, data, 5000);
	plan_create(&plan, 1, "/subvol/created", BTRFS_MODE_REGULAR | 0600, NULL);
	plan_write_new(&plan, 1, "/subvol/created", 0, "in a subvolume\n", 15);
	plan_create(&plan, 2, "/ns/new/sub", BTRFS_MODE_DIRECTORY | 0700, NULL);
	plan_create(&plan, 2, "/ns/new/sub/inner", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 2, "/ns/new/sub/inner", 0, data + 4096, 4096);
	for (i = 0; i < NAMESPACE_FILES; i++) {
		REQUIRE(snprintf(names[i], sizeof(names[i]), "f%03zu", i) < (int)sizeof(names[i]));
		REQUIRE(snprintf(path, sizeof(path), "/ns/new/%s", names[i]) < (int)sizeof(path));
		plan_create(&plan, 2, path, BTRFS_MODE_REGULAR | 0644, NULL);
	}
	plan_write_new(&plan, 2, "/ns/new/file", 10000, data + 10000, 3000);

	expect_listing(context, &plan, 0, 0, "/ns", NULL, NULL);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns", added, NULL);
	expect_absent(&plan, 0, 0, "/ns/new");
	expect_absent(&plan, 0, 0, "/subvol/created");
	listing[count++] = "char";
	listing[count++] = "file";
	listing[count++] = "link";
	listing[count++] = "pipe";
	listing[count++] = "sock";
	expect_names(&plan, 1, 1, "/ns/new", listing, count);
	listing[count++] = "sub";
	for (i = 0; i < NAMESPACE_FILES; i++) {
		listing[count++] = names[i];
	}
	expect_names(&plan, 2, LAST_STAGE, "/ns/new", listing, count);
	expect_stat(&plan, 1, LAST_STAGE, "/ns/new", BTRFS_MODE_DIRECTORY | 0755, 1);
	expect_file(&plan, 1, 1, "/ns/new/file", data, 10000);
	expect_file(&plan, 2, LAST_STAGE, "/ns/new/file", data, 13000);
	expect_stat(&plan, 1, LAST_STAGE, "/ns/new/file", BTRFS_MODE_REGULAR | 0644, 1);
	expect_symlink(&plan, 1, LAST_STAGE, "/ns/new/link", "../tree/a/b/deep");
	expect_stat(&plan, 1, LAST_STAGE, "/ns/new/link", BTRFS_MODE_SYMLINK | 0777, 1);
	expect_value(&plan, 1, LAST_STAGE, EXPECT_DEVICE, "/ns/new/char", NAMESPACE_DEVICE);
	expect_stat(&plan, 1, LAST_STAGE, "/ns/new/char", BTRFS_MODE_CHARACTER | 0620, 1);
	expect_stat(&plan, 1, LAST_STAGE, "/ns/new/pipe", BTRFS_MODE_FIFO | 0600, 1);
	expect_stat(&plan, 1, LAST_STAGE, "/ns/new/sock", BTRFS_MODE_SOCKET | 0755, 1);
	expect_file(&plan, 1, LAST_STAGE, "/ns/nocow/file", data, 5000);
	expect_value(&plan, 1, LAST_STAGE, EXPECT_FLAGS, "/ns/nocow/file",
	    BT_INODE_NODATACOW | BT_INODE_NODATASUM);
	expect_file(&plan, 1, LAST_STAGE, "/ns/compress/file", data, 5000);
	expect_value(&plan, 1, LAST_STAGE, EXPECT_FLAGS, "/ns/compress/file", BT_INODE_COMPRESS);
	expect_text(&plan, 1, LAST_STAGE, "/subvol/created", "in a subvolume\n");
	expect_names(&plan, 2, LAST_STAGE, "/ns/new/sub", (const char *[]){ "inner" }, 1);
	expect_file(&plan, 2, LAST_STAGE, "/ns/new/sub/inner", data + 4096, 4096);
	expect_stat(&plan, 2, LAST_STAGE, "/ns/new/sub", BTRFS_MODE_DIRECTORY | 0700, 1);
	run_plan(context, &plan);
}

/* Names and xattrs whose hashes collide share packed items: entries are
 * appended, removed from the front, replaced and renamed within one item. */
static void
namespace_collide_plan(struct context *context)
{
	static const char *const first_added[] = { collisions[2], NULL };
	static const char *const first_removed[] = { collisions[0], NULL };
	static const char *const second_added[] = { collisions[2], collisions[3], NULL };
	static const char *const second_removed[] = { collisions[1], NULL };
	char paths[4][64];
	char xattrs[4][64];
	struct plan plan;
	size_t i;

	for (i = 0; i < 4; i++) {
		REQUIRE(snprintf(paths[i], sizeof(paths[i]), "/ns/collide/%s", collisions[i]) <
		    (int)sizeof(paths[i]));
		REQUIRE(snprintf(xattrs[i], sizeof(xattrs[i]), "user.%s", collisions[i]) <
		    (int)sizeof(xattrs[i]));
	}
	namespace_plan(context, &plan, "namespace-collide");
	plan_create(&plan, 1, paths[2], BTRFS_MODE_REGULAR | 0644, NULL);
	plan_unlink(&plan, 1, paths[0], 0);
	plan_set_xattr(&plan, 1, "/ns/one", xattrs[2], "third", 5, BTRFS_XATTR_CREATE);
	plan_set_xattr(&plan, 1, "/ns/one", xattrs[0], "replaced", 8, BTRFS_XATTR_REPLACE);
	plan_remove_xattr(&plan, 1, "/ns/one", xattrs[1]);
	plan_rename(&plan, 2, paths[1], paths[3], 0);
	plan_create(&plan, 2, paths[0], BTRFS_MODE_REGULAR | 0600, NULL);
	plan_set_xattr(&plan, 2, "/ns/one", xattrs[3], "fourth", 6, 0);
	plan_remove_xattr(&plan, 2, "/ns/one", xattrs[0]);

	expect_listing(context, &plan, 0, 0, "/ns/collide", NULL, NULL);
	expect_listing(context, &plan, 1, 1, "/ns/collide", first_added, first_removed);
	expect_listing(context, &plan, 2, LAST_STAGE, "/ns/collide", second_added, second_removed);
	expect_text(&plan, 0, 0, paths[0], "first\n");
	expect_absent(&plan, 1, 1, paths[0]);
	expect_text(&plan, 2, LAST_STAGE, paths[0], "");
	expect_stat(&plan, 2, LAST_STAGE, paths[0], BTRFS_MODE_REGULAR | 0600, 1);
	expect_text(&plan, 0, 1, paths[1], "second\n");
	expect_absent(&plan, 2, LAST_STAGE, paths[1]);
	expect_text(&plan, 1, LAST_STAGE, paths[2], "");
	expect_absent(&plan, 0, 1, paths[3]);
	expect_text(&plan, 2, LAST_STAGE, paths[3], "second\n");
	expect_xattr(&plan, 0, 0, "/ns/one", xattrs[0], "first", 5);
	expect_xattr(&plan, 1, 1, "/ns/one", xattrs[0], "replaced", 8);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/one", xattrs[0], NULL, 0);
	expect_xattr(&plan, 0, 0, "/ns/one", xattrs[1], "second", 6);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/one", xattrs[1], NULL, 0);
	expect_xattr(&plan, 0, 0, "/ns/one", xattrs[2], NULL, 0);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/one", xattrs[2], "third", 5);
	expect_xattr(&plan, 0, 1, "/ns/one", xattrs[3], NULL, 0);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/one", xattrs[3], "fourth", 6);
	run_plan(context, &plan);
}

/* Hard links: across directories, to a device node, to an inode with
 * extended references, and a second name in a colliding item. */
static void
namespace_link_plan(struct context *context)
{
	static const char *const root_added[] = { "third", "target-link", NULL };
	static const char *const tree_added[] = { "null2", NULL };
	static const char *const a_added[] = { "second", NULL };
	static const char *const b_added[] = { "fourth", NULL };
	static const char *const collide_added[] = { collisions[2], NULL };
	char first[64];
	char third[64];
	char extref[320];
	struct plan plan;

	REQUIRE(
	    snprintf(first, sizeof(first), "/ns/collide/%s", collisions[0]) < (int)sizeof(first));
	REQUIRE(
	    snprintf(third, sizeof(third), "/ns/collide/%s", collisions[2]) < (int)sizeof(third));
	extref_path(extref, sizeof(extref), EXTREF_LINKS - 1);
	namespace_plan(context, &plan, "namespace-link");
	plan_link(&plan, 1, "/ns/one", "/ns/tree/a/second");
	plan_link(&plan, 1, "/ns/one", "/ns/third");
	plan_link(&plan, 1, "/ns/null", "/ns/tree/null2");
	plan_link(&plan, 1, "/ns/extref/target", "/ns/target-link");
	plan_link(&plan, 1, first, third);
	plan_link(&plan, 2, "/ns/tree/a/second", "/ns/tree/a/b/fourth");

	expect_listing(context, &plan, 0, 0, "/ns", NULL, NULL);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns", root_added, NULL);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns/tree", tree_added, NULL);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns/tree/a", a_added, NULL);
	expect_listing(context, &plan, 0, 1, "/ns/tree/a/b", NULL, NULL);
	expect_listing(context, &plan, 2, LAST_STAGE, "/ns/tree/a/b", b_added, NULL);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns/collide", collide_added, NULL);
	expect_links(context, &plan, 0, 0, "/ns/one", "/ns/one", 2);
	expect_links(context, &plan, 1, 1, "/ns/one", "/ns/one", 4);
	expect_links(context, &plan, 2, LAST_STAGE, "/ns/one", "/ns/one", 5);
	expect_same(&plan, 1, LAST_STAGE, "/ns/tree/a/second", "/ns/one");
	expect_same(&plan, 1, LAST_STAGE, "/ns/third", "/ns/one");
	expect_same(&plan, 2, LAST_STAGE, "/ns/tree/a/b/fourth", "/ns/one");
	expect_same(&plan, 1, LAST_STAGE, "/ns/tree/null2", "/ns/null");
	expect_value(&plan, 1, LAST_STAGE, EXPECT_DEVICE, "/ns/tree/null2", LINUX_NULL_DEVICE);
	expect_same(&plan, 1, LAST_STAGE, "/ns/target-link", extref);
	expect_links(context, &plan, 1, LAST_STAGE, "/ns/target-link", "/ns/extref/target",
	    EXTREF_LINKS + 2);
	expect_same(&plan, 1, LAST_STAGE, third, first);
	expect_links(context, &plan, 1, LAST_STAGE, first, first, 2);
	run_plan(context, &plan);
}

/* Unlinks drop link counts, empty directories, a shared data extent's
 * reference and then its last one, and names of an inode with extended
 * references. */
static void
namespace_unlink_plan(struct context *context)
{
	static const char *const root_first[] = { "data", "fifo", "empty", NULL };
	static const char *const root_second[] = { "data", "fifo", "empty", "clone", NULL };
	static const char *const tree_removed[] = { "one-link", NULL };
	static const char *const a_removed[] = { "b", NULL };
	char extref_first[320];
	char extref_remaining[320];
	const char *extref_one[] = { "target", NULL };
	const char *extref_two[] = { "target", NULL, NULL };
	struct plan plan;

	extref_path(extref_first, sizeof(extref_first), 0);
	extref_path(extref_remaining, sizeof(extref_remaining), 10);
	extref_two[1] = strrchr(extref_first, '/') + 1;
	namespace_plan(context, &plan, "namespace-unlink");
	plan_unlink(&plan, 1, "/ns/tree/one-link", 0);
	plan_unlink(&plan, 1, "/ns/tree/a/b/deep", 0);
	plan_unlink(&plan, 1, "/ns/tree/a/b", 0);
	plan_unlink(&plan, 1, "/ns/data", 0);
	plan_unlink(&plan, 1, "/ns/extref/target", 0);
	plan_unlink(&plan, 1, "/ns/fifo", 0);
	plan_unlink(&plan, 1, "/ns/empty", 0);
	plan_unlink(&plan, 2, "/ns/clone", 0);
	plan_unlink(&plan, 2, extref_first, 0);

	expect_listing(context, &plan, 0, 0, "/ns", NULL, NULL);
	expect_listing(context, &plan, 1, 1, "/ns", NULL, root_first);
	expect_listing(context, &plan, 2, LAST_STAGE, "/ns", NULL, root_second);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns/tree", NULL, tree_removed);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns/tree/a", NULL, a_removed);
	expect_listing(context, &plan, 1, 1, "/ns/extref", NULL, extref_one);
	expect_listing(context, &plan, 2, LAST_STAGE, "/ns/extref", NULL, extref_two);
	expect_links(context, &plan, 1, LAST_STAGE, "/ns/one", "/ns/one", 1);
	expect_current(context, &plan, 0, 1, "/ns/clone", "/ns/clone");
	expect_absent(&plan, 2, LAST_STAGE, "/ns/clone");
	expect_absent(&plan, 1, LAST_STAGE, "/ns/data");
	expect_links(context, &plan, 0, 0, extref_remaining, "/ns/extref/target", EXTREF_LINKS + 1);
	expect_links(context, &plan, 1, 1, extref_remaining, "/ns/extref/target", EXTREF_LINKS);
	expect_links(
	    context, &plan, 2, LAST_STAGE, extref_remaining, "/ns/extref/target", EXTREF_LINKS - 1);
	run_plan(context, &plan);
}

/* Renames across directories, over files, over an empty directory, between
 * names of one inode, and within and into a colliding item. */
static void
namespace_rename_plan(struct context *context)
{
	static const char *const root_first_added[] = { "moved", NULL };
	static const char *const root_first_removed[] = { "victim", "full", NULL };
	static const char *const root_second_removed[] = { "victim", "full", "clone", "data",
		NULL };
	static const char *const tree_first_removed[] = { "a", NULL };
	static const char *const tree_second_added[] = { "b2", NULL };
	static const char *const collide_added[] = { collisions[2], collisions[3], NULL };
	static const char *const collide_removed[] = { collisions[0], NULL };
	char paths[4][64];
	struct plan plan;
	size_t i;

	for (i = 0; i < 4; i++) {
		REQUIRE(snprintf(paths[i], sizeof(paths[i]), "/ns/collide/%s", collisions[i]) <
		    (int)sizeof(paths[i]));
	}
	namespace_plan(context, &plan, "namespace-rename");
	plan_rename(&plan, 1, "/ns/tree/a", "/ns/moved", 0);
	plan_rename(&plan, 1, "/ns/victim", "/ns/data", 0);
	plan_rename(&plan, 1, "/ns/full", "/ns/empty", 0);
	plan_rename(&plan, 1, "/ns/tree/one-link", "/ns/one", 0);
	plan_rename(&plan, 1, "/ns/compress", "/ns/compress", 0);
	plan_rename(&plan, 2, "/ns/moved/b", "/ns/tree/b2", 0);
	plan_rename(&plan, 2, "/ns/clone", paths[2], 0);
	plan_rename(&plan, 2, "/ns/data", "/ns/tree/one-link", 0);
	plan_rename(&plan, 2, paths[0], paths[3], 0);

	expect_listing(context, &plan, 0, 0, "/ns", NULL, NULL);
	expect_listing(context, &plan, 1, 1, "/ns", root_first_added, root_first_removed);
	expect_listing(context, &plan, 2, LAST_STAGE, "/ns", root_first_added, root_second_removed);
	expect_listing(context, &plan, 0, 0, "/ns/tree", NULL, NULL);
	expect_listing(context, &plan, 1, 1, "/ns/tree", NULL, tree_first_removed);
	expect_listing(
	    context, &plan, 2, LAST_STAGE, "/ns/tree", tree_second_added, tree_first_removed);
	expect_names(&plan, 1, 1, "/ns/moved", (const char *[]){ "b" }, 1);
	expect_names(&plan, 2, LAST_STAGE, "/ns/moved", NULL, 0);
	expect_text(&plan, 1, 1, "/ns/moved/b/deep", "deep\n");
	expect_text(&plan, 2, LAST_STAGE, "/ns/tree/b2/deep", "deep\n");
	expect_current(context, &plan, 0, 0, "/ns/data", "/ns/data");
	expect_text(&plan, 1, 1, "/ns/data", "victim\n");
	expect_same(&plan, 0, 1, "/ns/tree/one-link", "/ns/one");
	expect_text(&plan, 2, LAST_STAGE, "/ns/tree/one-link", "victim\n");
	expect_names(&plan, 1, LAST_STAGE, "/ns/empty", (const char *[]){ "inner" }, 1);
	expect_text(&plan, 1, LAST_STAGE, "/ns/empty/inner", "inner\n");
	expect_absent(&plan, 1, LAST_STAGE, "/ns/full");
	expect_links(context, &plan, 0, 1, "/ns/one", "/ns/one", 2);
	expect_links(context, &plan, 2, LAST_STAGE, "/ns/one", "/ns/one", 1);
	expect_current(context, &plan, 0, 1, "/ns/clone", "/ns/clone");
	expect_current(context, &plan, 2, LAST_STAGE, paths[2], "/ns/clone");
	expect_listing(
	    context, &plan, 2, LAST_STAGE, "/ns/collide", collide_added, collide_removed);
	expect_text(&plan, 2, LAST_STAGE, paths[3], "first\n");
	run_plan(context, &plan);
}

/* Last names of open inodes go, leaving orphan items; the final state keeps
 * them, so Linux's read-write mount must clean them up. */
static void
namespace_open_plan(struct context *context)
{
	static const char *const removed[] = { "victim", "clone", "fifo", NULL };
	struct plan plan;

	namespace_plan(context, &plan, "namespace-open");
	plan_unlink(&plan, 1, "/ns/victim", 1);
	plan_unlink(&plan, 1, "/ns/clone", 1);
	plan_rename(&plan, 1, "/ns/fifo", "/ns/null", 1);
	expect_listing(context, &plan, 0, 0, "/ns", NULL, NULL);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns", NULL, removed);
	expect_current(context, &plan, 0, LAST_STAGE, "/ns/data", "/ns/data");
	expect_links(context, &plan, 1, LAST_STAGE, "/ns/null", "/ns/fifo", 1);
	run_plan(context, &plan);
}

/* Orphans go by eviction, then by cleanup as after a crash. */
static void
namespace_orphan_plan(struct context *context)
{
	static const char *const removed[] = { "victim", "clone", NULL };
	struct plan plan;

	namespace_plan(context, &plan, "namespace-orphans");
	plan_unlink(&plan, 1, "/ns/victim", 1);
	plan_unlink(&plan, 1, "/ns/clone", 1);
	plan_evict(context, &plan, 2, "/ns/victim");
	plan_clean(&plan, 3, BTRFS_TOP_LEVEL_TREE, 1);
	expect_listing(context, &plan, 1, LAST_STAGE, "/ns", NULL, removed);
	expect_current(context, &plan, 0, LAST_STAGE, "/ns/data", "/ns/data");
	run_plan(context, &plan);
}

/* The largest xattr one item holds, an empty value, special files, then
 * replacement and removal. */
static void
namespace_xattr_plan(struct context *context)
{
	struct plan plan;
	uint8_t *big;
	size_t size = item_limit(context) - sizeof(struct bt_disk_dir) - strlen("user.big");

	big = malloc(size);
	REQUIRE(big != NULL);
	fill_pattern(big, size, 31);
	namespace_plan(context, &plan, "namespace-xattr");
	plan_set_xattr(&plan, 1, "/ns/tree", "user.big", big, size, BTRFS_XATTR_CREATE);
	/* Linux exposes user xattrs only on regular files and directories. */
	plan_set_xattr(&plan, 1, "/ns/null", "trusted.empty", "", 0, 0);
	plan_set_xattr(&plan, 1, "/ns/fifo", "trusted.fifo", "special", 7, 0);
	plan_set_xattr(&plan, 2, "/ns/tree", "user.big", "small", 5, BTRFS_XATTR_REPLACE);
	plan_remove_xattr(&plan, 2, "/ns/null", "trusted.empty");
	expect_xattr(&plan, 0, 0, "/ns/tree", "user.big", NULL, 0);
	expect_xattr(&plan, 1, 1, "/ns/tree", "user.big", big, size);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/tree", "user.big", "small", 5);
	expect_xattr(&plan, 1, 1, "/ns/null", "trusted.empty", "", 0);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/null", "trusted.empty", NULL, 0);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/fifo", "trusted.fifo", "special", 7);
	free(big);
	run_plan(context, &plan);
}

/* A subvolume's own tree: new names, a hard link and a rename. */
static void
namespace_subvolume_plan(struct context *context)
{
	static const char *const added[] = { "dir", "renamed", NULL };
	struct plan plan;

	namespace_plan(context, &plan, "namespace-subvolume");
	plan_create(&plan, 1, "/subvol/dir", BTRFS_MODE_DIRECTORY | 0755, NULL);
	plan_create(&plan, 1, "/subvol/dir/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/subvol/dir/file", 0, "moved file\n", 11);
	plan_link(&plan, 1, "/subvol/value", "/subvol/dir/value-link");
	plan_rename(&plan, 1, "/subvol/dir/file", "/subvol/renamed", 0);
	expect_listing(context, &plan, 0, 0, "/subvol", NULL, NULL);
	expect_listing(context, &plan, 1, LAST_STAGE, "/subvol", added, NULL);
	expect_names(&plan, 1, LAST_STAGE, "/subvol/dir", (const char *[]){ "value-link" }, 1);
	expect_same(&plan, 1, LAST_STAGE, "/subvol/dir/value-link", "/subvol/value");
	expect_links(context, &plan, 1, LAST_STAGE, "/subvol/value", "/subvol/value", 2);
	expect_text(&plan, 1, LAST_STAGE, "/subvol/renamed", "moved file\n");
	run_plan(context, &plan);
}

/* The btrfs.compression property: inherited by new regular files and
 * directories, applied to inode flags when set or removed, and recording the
 * codec's incompat feature. */
static void
namespace_property_plan(struct context *context)
{
	static const char *const zstd_added[] = { "file", "sub", "link", "after", NULL };
	static uint8_t data[NAMESPACE_DATA_BYTES];
	const uint64_t codec_flags = BT_INODE_COMPRESS | BT_INODE_NOCOMPRESS;
	struct plan plan;

	fill_pattern(data, 6000, 41);
	namespace_plan(context, &plan, "namespace-property");
	plan_create(&plan, 1, "/ns/zstd/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/ns/zstd/file", 0, data, 6000);
	plan_create(&plan, 1, "/ns/zstd/sub", BTRFS_MODE_DIRECTORY | 0755, NULL);
	plan_create(&plan, 1, "/ns/zstd/sub/deeper", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_create(&plan, 1, "/ns/zstd/link", BTRFS_MODE_SYMLINK | 0777, "file");
	plan_create(&plan, 1, "/ns/nocompress/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_set_xattr(&plan, 1, "/ns/tree", "btrfs.compression", "lzo", 3, 0);
	plan_create(&plan, 1, "/ns/tree/lzo-file", BTRFS_MODE_REGULAR | 0600, NULL);
	plan_remove_xattr(&plan, 2, "/ns/zstd", "btrfs.compression");
	plan_set_xattr(
	    &plan, 2, "/ns/tree/lzo-file", "btrfs.compression", "no", 2, BTRFS_XATTR_REPLACE);
	plan_create(&plan, 2, "/ns/zstd/after", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_set_xattr(&plan, 2, "/ns/nocompress", "btrfs.compression", "", 0, 0);

	expect_listing(context, &plan, 2, LAST_STAGE, "/ns/zstd", zstd_added, NULL);
	expect_xattr(&plan, 0, 1, "/ns/zstd", "btrfs.compression", "zstd", 4);
	expect_flags(&plan, 0, 1, "/ns/zstd", codec_flags, BT_INODE_COMPRESS);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/zstd", "btrfs.compression", NULL, 0);
	expect_flags(&plan, 2, LAST_STAGE, "/ns/zstd", codec_flags, 0);
	expect_file(&plan, 1, LAST_STAGE, "/ns/zstd/file", data, 6000);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/zstd/file", "btrfs.compression", "zstd", 4);
	expect_flags(&plan, 1, LAST_STAGE, "/ns/zstd/file", codec_flags, BT_INODE_COMPRESS);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/zstd/sub", "btrfs.compression", "zstd", 4);
	expect_flags(&plan, 1, LAST_STAGE, "/ns/zstd/sub", codec_flags, BT_INODE_COMPRESS);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/zstd/sub/deeper", "btrfs.compression", "zstd", 4);
	expect_flags(&plan, 1, LAST_STAGE, "/ns/zstd/sub/deeper", codec_flags, BT_INODE_COMPRESS);
	expect_symlink(&plan, 1, LAST_STAGE, "/ns/zstd/link", "file");
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/zstd/after", "btrfs.compression", NULL, 0);
	expect_flags(&plan, 2, LAST_STAGE, "/ns/zstd/after", codec_flags, 0);
	expect_xattr(&plan, 0, 1, "/ns/nocompress", "btrfs.compression", "no", 2);
	expect_flags(&plan, 0, 1, "/ns/nocompress", codec_flags, BT_INODE_NOCOMPRESS);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/nocompress", "btrfs.compression", NULL, 0);
	expect_flags(&plan, 2, LAST_STAGE, "/ns/nocompress", codec_flags, 0);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/nocompress/file", "btrfs.compression", NULL, 0);
	expect_flags(&plan, 1, LAST_STAGE, "/ns/nocompress/file", codec_flags, BT_INODE_NOCOMPRESS);
	expect_xattr(&plan, 0, 0, "/ns/tree", "btrfs.compression", NULL, 0);
	expect_xattr(&plan, 1, LAST_STAGE, "/ns/tree", "btrfs.compression", "lzo", 3);
	expect_flags(&plan, 1, LAST_STAGE, "/ns/tree", codec_flags, BT_INODE_COMPRESS);
	expect_xattr(&plan, 1, 1, "/ns/tree/lzo-file", "btrfs.compression", "lzo", 3);
	expect_flags(&plan, 1, 1, "/ns/tree/lzo-file", codec_flags, BT_INODE_COMPRESS);
	expect_xattr(&plan, 2, LAST_STAGE, "/ns/tree/lzo-file", "btrfs.compression", "no", 2);
	expect_flags(&plan, 2, LAST_STAGE, "/ns/tree/lzo-file", codec_flags, BT_INODE_NOCOMPRESS);
	expect_value(&plan, 1, LAST_STAGE, EXPECT_FEATURE, "/", BT_FEATURE_COMPRESS_LZO);
	run_plan(context, &plan);
}

/* Colliding names fill one DIR_ITEM to the largest item a leaf holds. */
static void
namespace_full_item_plan(struct context *context)
{
	const char **listing;
	char (*names)[COLLISION_NAME_BYTES + 1];
	char path[COLLISION_NAME_BYTES + 32];
	struct plan plan;
	size_t count = item_limit(context) / (sizeof(struct bt_disk_dir) + COLLISION_NAME_BYTES);
	size_t i;

	names = calloc(count, sizeof(*names));
	listing = calloc(count, sizeof(*listing));
	REQUIRE(names != NULL && listing != NULL);
	namespace_plan(context, &plan, "namespace-full-item");
	for (i = 0; i < count; i++) {
		collision_name(names[i], i);
		listing[i] = names[i];
		REQUIRE(snprintf(path, sizeof(path), "/ns/empty/%s", names[i]) < (int)sizeof(path));
		plan_create(&plan, 1, path, BTRFS_MODE_REGULAR | 0644, NULL);
	}
	expect_names(&plan, 0, 0, "/ns/empty", NULL, 0);
	expect_names(&plan, 1, LAST_STAGE, "/ns/empty", listing, count);
	free(listing);
	free(names);
	run_plan(context, &plan);
}

static void
refused(struct btrfs_transaction *transaction, enum btrfs_result result, enum btrfs_result expected,
    const char *what)
{
	if (result != expected) {
		fprintf(stderr, "%s: %s, expected %s\n", what, btrfs_result_string(result),
		    btrfs_result_string(expected));
		exit(1);
	}
	/* Refusals before any change leave the transaction usable. */
	REQUIRE(transaction->failure == BTRFS_OK);
}

static struct btrfs_object_id
object(struct btrfs_fs *fs, const char *path)
{
	struct btrfs_inode inode;

	REQUIRE(btrfs_image_lookup(fs, path, &inode) == BTRFS_OK);
	return inode.id;
}

static struct btrfs_new_inode
new_inode(uint32_t mode)
{
	struct btrfs_new_inode attributes;

	memset(&attributes, 0, sizeof(attributes));
	attributes.mode = mode;
	attributes.uid = NAMESPACE_UID;
	attributes.gid = NAMESPACE_GID;
	attributes.time.seconds = 1800000000;
	return attributes;
}

/* Every refusal happens before a change and leaves the transaction able to
 * commit; an operation that names one inode twice changes nothing. */
static void
namespace_refusals(struct context *context)
{
	static char long_name[BTRFS_NAME_MAX + 2];
	static char long_target[LINUX_PATH_MAX + 1];
	struct btrfs_new_inode file = new_inode(BTRFS_MODE_REGULAR | 0644);
	struct btrfs_new_inode attributes;
	struct btrfs_object_id ns;
	struct btrfs_object_id one;
	struct btrfs_object_id id;
	struct btrfs_object_id root;
	struct btrfs_object_id subvolume;
	struct btrfs_object_id snapshot;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct btrfs_time time = { 1800000000, 0 };
	char extref[320];
	char xattr[64];
	uint8_t *value;
	size_t limit = item_limit(context);

	memset(long_name, 'n', BTRFS_NAME_MAX + 1);
	memset(long_target, 't', LINUX_PATH_MAX);
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	ns = object(fs, "/ns");
	one = object(fs, "/ns/one");
	root = object(fs, "/");
	subvolume = object(fs, "/subvol");
	snapshot = object(fs, "/snapshot");
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	refused(transaction, btrfs_transaction_create(transaction, ns, "one", 3, &file, &id),
	    BTRFS_EXISTS, "create over a name");
	refused(transaction, btrfs_transaction_create(transaction, root, "subvol", 6, &file, &id),
	    BTRFS_EXISTS, "create over a subvolume");
	refused(transaction, btrfs_transaction_create(transaction, ns, ".", 1, &file, &id),
	    BTRFS_INVALID_ARGUMENT, "create dot");
	refused(transaction, btrfs_transaction_create(transaction, ns, "..", 2, &file, &id),
	    BTRFS_INVALID_ARGUMENT, "create dot-dot");
	refused(transaction, btrfs_transaction_create(transaction, ns, "a/b", 3, &file, &id),
	    BTRFS_INVALID_ARGUMENT, "create with a slash");
	refused(transaction, btrfs_transaction_create(transaction, ns, "", 0, &file, &id),
	    BTRFS_INVALID_ARGUMENT, "create an empty name");
	refused(transaction,
	    btrfs_transaction_create(transaction, ns, long_name, BTRFS_NAME_MAX + 1, &file, &id),
	    BTRFS_NAME_TOO_LONG, "create a long name");
	refused(transaction, btrfs_transaction_create(transaction, one, "x", 1, &file, &id),
	    BTRFS_NOT_DIRECTORY, "create below a file");
	attributes = new_inode(0644);
	refused(transaction, btrfs_transaction_create(transaction, ns, "x", 1, &attributes, &id),
	    BTRFS_INVALID_ARGUMENT, "create without a type");
	attributes = new_inode(BTRFS_MODE_SYMLINK | 0777);
	refused(transaction, btrfs_transaction_create(transaction, ns, "x", 1, &attributes, &id),
	    BTRFS_INVALID_ARGUMENT, "symlink without a target");
	attributes.target = long_target;
	attributes.target_length = LINUX_PATH_MAX;
	refused(transaction, btrfs_transaction_create(transaction, ns, "x", 1, &attributes, &id),
	    BTRFS_NAME_TOO_LONG, "symlink beyond PATH_MAX");
	attributes = file;
	attributes.target = "t";
	attributes.target_length = 1;
	refused(transaction, btrfs_transaction_create(transaction, ns, "x", 1, &attributes, &id),
	    BTRFS_INVALID_ARGUMENT, "regular file with a target");
	refused(transaction, btrfs_transaction_create(transaction, snapshot, "x", 1, &file, &id),
	    BTRFS_READ_ONLY, "create in a read-only snapshot");
	refused(transaction,
	    btrfs_transaction_link(transaction, object(fs, "/ns/tree"), ns, "x", 1, time),
	    BTRFS_IS_DIRECTORY, "link a directory");
	refused(transaction, btrfs_transaction_link(transaction, one, subvolume, "x", 1, time),
	    BTRFS_CROSS_TREE, "link across trees");
	refused(transaction, btrfs_transaction_link(transaction, one, ns, "victim", 6, time),
	    BTRFS_EXISTS, "link over a name");
	/* A short name still fits the INODE_REF item; a long one needs an
	 * extended reference. */
	extref_path(extref, sizeof(extref), EXTREF_LINKS);
	refused(transaction,
	    btrfs_transaction_link(transaction, object(fs, "/ns/extref/target"),
		object(fs, "/ns/extref"), strrchr(extref, '/') + 1,
		strlen(strrchr(extref, '/') + 1), time),
	    BTRFS_UNSUPPORTED, "link beyond a full INODE_REF");
	refused(transaction, btrfs_transaction_unlink(transaction, ns, "missing", 7, time, 0),
	    BTRFS_NOT_FOUND, "unlink a missing name");
	refused(transaction, btrfs_transaction_unlink(transaction, ns, "full", 4, time, 0),
	    BTRFS_NOT_EMPTY, "unlink a non-empty directory");
	refused(transaction, btrfs_transaction_unlink(transaction, root, "subvol", 6, time, 0),
	    BTRFS_CROSS_TREE, "unlink a subvolume");
	extref_path(extref, sizeof(extref), EXTREF_LINKS - 1);
	refused(transaction,
	    btrfs_transaction_unlink(transaction, object(fs, "/ns/extref"),
		strrchr(extref, '/') + 1, strlen(strrchr(extref, '/') + 1), time, 0),
	    BTRFS_UNSUPPORTED, "unlink an extended reference");
	refused(transaction, btrfs_transaction_unlink(transaction, snapshot, "value", 5, time, 0),
	    BTRFS_READ_ONLY, "unlink in a read-only snapshot");
	refused(transaction,
	    btrfs_transaction_rename(
		transaction, ns, "tree", 4, object(fs, "/ns/tree/a"), "inside", 6, time, 0),
	    BTRFS_INVALID_ARGUMENT, "rename a directory below itself");
	refused(transaction,
	    btrfs_transaction_rename(transaction, ns, "one", 3, ns, "empty", 5, time, 0),
	    BTRFS_IS_DIRECTORY, "rename a file over a directory");
	refused(transaction,
	    btrfs_transaction_rename(transaction, ns, "empty", 5, ns, "one", 3, time, 0),
	    BTRFS_NOT_DIRECTORY, "rename a directory over a file");
	refused(transaction,
	    btrfs_transaction_rename(transaction, ns, "empty", 5, ns, "full", 4, time, 0),
	    BTRFS_NOT_EMPTY, "rename over a non-empty directory");
	refused(transaction,
	    btrfs_transaction_rename(transaction, subvolume, "value", 5, ns, "value", 5, time, 0),
	    BTRFS_CROSS_TREE, "rename across trees");
	refused(transaction,
	    btrfs_transaction_rename(transaction, root, "subvol", 6, root, "other", 5, time, 0),
	    BTRFS_CROSS_TREE, "rename a subvolume");
	refused(transaction,
	    btrfs_transaction_rename(transaction, ns, "missing", 7, ns, "other", 5, time, 0),
	    BTRFS_NOT_FOUND, "rename a missing name");
	refused(transaction,
	    btrfs_transaction_rename(
		transaction, object(fs, "/ns/tree"), "one-link", 8, ns, "one", 3, time, 0),
	    BTRFS_OK, "rename between names of one inode");
	REQUIRE(snprintf(xattr, sizeof(xattr), "user.%s", collisions[0]) < (int)sizeof(xattr));
	refused(transaction,
	    btrfs_transaction_set_xattr(
		transaction, one, xattr, strlen(xattr), "x", 1, BTRFS_XATTR_CREATE, time),
	    BTRFS_EXISTS, "create an existing xattr");
	refused(transaction,
	    btrfs_transaction_set_xattr(
		transaction, one, "user.none", 9, "x", 1, BTRFS_XATTR_REPLACE, time),
	    BTRFS_NOT_FOUND, "replace a missing xattr");
	refused(transaction, btrfs_transaction_remove_xattr(transaction, one, "user.none", 9, time),
	    BTRFS_NOT_FOUND, "remove a missing xattr");
	value = calloc(1, limit);
	REQUIRE(value != NULL);
	refused(transaction,
	    btrfs_transaction_set_xattr(transaction, one, "user.big", 8, value,
		limit - sizeof(struct bt_disk_dir) - 8 + 1, 0, time),
	    BTRFS_NO_SPACE, "an xattr beyond one item");
	free(value);
	refused(transaction,
	    btrfs_transaction_set_xattr(
		transaction, one, long_name, BTRFS_NAME_MAX + 1, "x", 1, 0, time),
	    BTRFS_RANGE, "a long xattr name");
	refused(transaction, btrfs_transaction_set_xattr(transaction, one, "", 0, "x", 1, 0, time),
	    BTRFS_INVALID_ARGUMENT, "an empty xattr name");
	refused(transaction,
	    btrfs_transaction_set_xattr(transaction, one, "user.x", 6, "x", 1, 4, time),
	    BTRFS_INVALID_ARGUMENT, "unknown xattr flags");
	refused(transaction, btrfs_transaction_evict(transaction, one), BTRFS_INVALID_ARGUMENT,
	    "evict a linked inode");
	refused(transaction,
	    btrfs_transaction_set_xattr(transaction, one, "btrfs.other", 11, "x", 1, 0, time),
	    BTRFS_INVALID_ARGUMENT, "an unknown property");
	refused(transaction,
	    btrfs_transaction_set_xattr(
		transaction, one, "btrfs.compression", 17, "bogus", 5, 0, time),
	    BTRFS_INVALID_ARGUMENT, "an invalid compression property");
	refused(transaction,
	    btrfs_transaction_set_xattr(
		transaction, object(fs, "/ns/nocow"), "btrfs.compression", 17, "zstd", 4, 0, time),
	    BTRFS_INVALID_ARGUMENT, "compression without data checksums");
	refused(transaction,
	    btrfs_transaction_remove_xattr(transaction, one, "btrfs.compression", 17, time),
	    BTRFS_NOT_FOUND, "remove a missing property");
	/* Linux ignores compression on objects other than files and directories. */
	refused(transaction,
	    btrfs_transaction_set_xattr(
		transaction, object(fs, "/symlink"), "btrfs.compression", 17, "zstd", 4, 0, time),
	    BTRFS_OK, "compression on a symlink");
	REQUIRE(transaction->changed == 0);
	/* The same transaction still commits a change. */
	REQUIRE(btrfs_transaction_create(transaction, ns, "after", 5, &file, &id) == BTRFS_OK);
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	audit_state(context, "namespace-refusals");
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(object(fs, "/ns/after").inode == id.inode);
	btrfs_unmount(fs);
	truncate_writes(context->device, 0);
	REQUIRE(context->image.live_allocations == 0);
	printf("namespace refusals PASS\n");
}

/* Limits of numbering and of packed items, refused before any change. */
static void
namespace_limits(struct context *context)
{
	struct btrfs_new_inode file = new_inode(BTRFS_MODE_REGULAR | 0644);
	struct btrfs_new_inode directory = new_inode(BTRFS_MODE_DIRECTORY | 0755);
	struct bt_disk_inode inode;
	struct bt_owned_root *tree;
	struct btrfs_object_id ns;
	struct btrfs_object_id empty;
	struct btrfs_object_id id;
	struct btrfs_object_id *made;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct btrfs_time time = { 1800000000, 0 };
	struct bt_key key;
	char name[COLLISION_NAME_BYTES + 1];
	size_t fits = item_limit(context) / (sizeof(struct bt_disk_dir) + COLLISION_NAME_BYTES);
	size_t i;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	ns = object(fs, "/ns");
	empty = object(fs, "/ns/empty");

	/* The last directory index is UINT64_MAX; then the directory is full. */
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	REQUIRE(bt_tx_tree(transaction, BTRFS_TOP_LEVEL_TREE, &tree) == BTRFS_OK);
	key = (struct bt_key){ empty.inode, UINT64_MAX - 1, BT_DIR_INDEX };
	REQUIRE(bt_tx_edit(transaction, &tree->root, key, "x", 1, BT_INSERT) == BTRFS_OK);
	REQUIRE(btrfs_transaction_create(transaction, empty, "a", 1, &file, &id) == BTRFS_OK);
	refused(transaction, btrfs_transaction_create(transaction, empty, "b", 1, &file, &id),
	    BTRFS_RANGE, "create beyond the last directory index");
	btrfs_transaction_destroy(transaction);

	/* Inode numbers end below BTRFS_LAST_FREE_OBJECTID. */
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	REQUIRE(bt_tx_tree(transaction, BTRFS_TOP_LEVEL_TREE, &tree) == BTRFS_OK);
	memset(&inode, 0, sizeof(inode));
	key = (struct bt_key){ BT_LAST_FREE_OBJECTID - 2, 0, BT_INODE_ITEM };
	REQUIRE(bt_tx_edit(transaction, &tree->root, key, &inode, sizeof(inode), BT_INSERT) ==
	    BTRFS_OK);
	REQUIRE(btrfs_transaction_create(transaction, empty, "a", 1, &file, &id) == BTRFS_OK);
	REQUIRE(id.inode == BT_LAST_FREE_OBJECTID - 1);
	refused(transaction, btrfs_transaction_create(transaction, empty, "b", 1, &file, &id),
	    BTRFS_NO_SPACE, "create beyond the last inode number");
	btrfs_transaction_destroy(transaction);

	/* A transaction numbers entries of a bounded set of directories. */
	made = calloc(BT_TRANSACTION_INDEXES + 1, sizeof(*made));
	REQUIRE(made != NULL);
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	for (i = 0; i <= BT_TRANSACTION_INDEXES; i++) {
		REQUIRE(snprintf(name, sizeof(name), "d%03zu", i) < (int)sizeof(name));
		REQUIRE(btrfs_transaction_create(
			    transaction, ns, name, strlen(name), &directory, &made[i]) == BTRFS_OK);
	}
	for (i = 0; i + 1 < BT_TRANSACTION_INDEXES; i++) {
		REQUIRE(
		    btrfs_transaction_create(transaction, made[i], "f", 1, &file, &id) == BTRFS_OK);
	}
	refused(transaction, btrfs_transaction_create(transaction, made[i], "f", 1, &file, &id),
	    BTRFS_UNSUPPORTED, "index more directories than one transaction tracks");
	REQUIRE(btrfs_transaction_create(transaction, ns, "later", 5, &file, &id) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	free(made);

	/* Colliding names fill their DIR_ITEM; Linux reports EOVERFLOW. */
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	for (i = 0; i < fits; i++) {
		collision_name(name, i);
		REQUIRE(btrfs_transaction_create(
			    transaction, empty, name, strlen(name), &file, &id) == BTRFS_OK);
	}
	collision_name(name, fits);
	refused(transaction,
	    btrfs_transaction_create(transaction, empty, name, strlen(name), &file, &id),
	    BTRFS_RANGE, "create beyond a full DIR_ITEM");
	refused(transaction,
	    btrfs_transaction_link(
		transaction, object(fs, "/ns/one"), empty, name, strlen(name), time),
	    BTRFS_RANGE, "link beyond a full DIR_ITEM");
	refused(transaction,
	    btrfs_transaction_rename(
		transaction, ns, "victim", 6, empty, name, strlen(name), time, 0),
	    BTRFS_RANGE, "rename beyond a full DIR_ITEM");
	collision_name(name, 0);
	REQUIRE(
	    btrfs_transaction_unlink(transaction, empty, name, strlen(name), time, 0) == BTRFS_OK);
	collision_name(name, fits);
	REQUIRE(btrfs_transaction_create(transaction, empty, name, strlen(name), &file, &id) ==
	    BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	REQUIRE(context->device->count == 0);
	REQUIRE(context->image.live_allocations == 0);
	printf("namespace limits: %zu colliding names per DIR_ITEM PASS\n", fits);
}

/* The namespace audit rejects committed states that Linux would reject. */
static void
namespace_audit_self_test(struct context *context)
{
	struct namespace_audit audit;
	struct bt_disk_inode inode;
	struct bt_owned_root *tree;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct bt_key key;
	size_t length;
	size_t pass;

	for (pass = 0; pass < 2; pass++) {
		REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
		key = (struct bt_key){ object(fs, pass == 0 ? "/ns/one" : "/ns/tree").inode, 0,
			BT_INODE_ITEM };
		REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
		REQUIRE(bt_tx_tree(transaction, BTRFS_TOP_LEVEL_TREE, &tree) == BTRFS_OK);
		REQUIRE(bt_mutation_find(transaction->mutation, tree->root, key, &inode,
			    sizeof(inode), &length) == BTRFS_OK);
		if (pass == 0) {
			bt_put32(&inode.links, bt_u32(inode.links) + 1);
		} else {
			bt_put64(&inode.size, bt_u64(inode.size) + 2);
		}
		REQUIRE(bt_tx_edit(transaction, &tree->root, key, &inode, sizeof(inode),
			    BT_REPLACE) == BTRFS_OK);
		transaction->changed = 1;
		REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
		btrfs_transaction_destroy(transaction);
		btrfs_unmount(fs);
		REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
		REQUIRE(namespace_audit(fs, &audit) != 0);
		printf("namespace audit detects: %s\n", audit.failure);
		btrfs_unmount(fs);
		truncate_writes(context->device, 0);
	}
	REQUIRE(context->image.live_allocations == 0);
}

static void
namespace_scenarios(struct context *context)
{
	namespace_refusals(context);
	namespace_limits(context);
	namespace_audit_self_test(context);
	namespace_create_plan(context);
	namespace_collide_plan(context);
	namespace_link_plan(context);
	namespace_unlink_plan(context);
	namespace_rename_plan(context);
	namespace_open_plan(context);
	namespace_orphan_plan(context);
	namespace_xattr_plan(context);
	namespace_subvolume_plan(context);
	namespace_property_plan(context);
	namespace_full_item_plan(context);
}

static void
admission_tests(struct context *context)
{
	static const char replacement[] = "written by Machlin CoW transaction\n";
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct btrfs_inode inode;
	struct btrfs_inode snapshot;
	struct btrfs_time time = { 1700000000, 0 };
	struct device *device = context->device;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/snapshot/value", &snapshot) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/greeting", &inode) == BTRFS_OK);
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_write_inline(transaction, snapshot.id, replacement,
		    sizeof(replacement) - 1, time) == BTRFS_READ_ONLY);
	REQUIRE(btrfs_transaction_write_inline(transaction,
		    (struct btrfs_object_id){ BT_EXTENT_TREE, inode.id.inode }, replacement,
		    sizeof(replacement) - 1, time) == BTRFS_INVALID_ARGUMENT);
	REQUIRE(btrfs_transaction_write_inline(transaction,
		    (struct btrfs_object_id){ ABSENT_TREE, inode.id.inode }, replacement,
		    sizeof(replacement) - 1, time) == BTRFS_NOT_FOUND);
	REQUIRE(btrfs_transaction_write_inline(transaction, inode.id, replacement, INLINE_LIMIT + 1,
		    time) == BTRFS_UNSUPPORTED);
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
	REQUIRE(device->count == 0 && device->flushes == 0);
	btrfs_transaction_destroy(transaction);
	/* Destroying an uncommitted transaction discards private work without I/O. */
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_write_inline(
		    transaction, inode.id, replacement, sizeof(replacement) - 1, time) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	REQUIRE(device->count == 0);
	btrfs_unmount(fs);
	REQUIRE(context->image.live_allocations == 0);
	printf("admission, no-op and abort: PASS\n");
}

static void
super_copy(struct context *context, unsigned mirror, struct bt_disk_super *super)
{
	read_exact(context, bt_super_offset(mirror), super, sizeof(*super));
}

static enum btrfs_result
begin_and_commit(struct context *context, struct btrfs_fs *fs, int damage_after_begin,
    const struct bt_disk_super *damage, unsigned mirror)
{
	struct btrfs_transaction *transaction;
	struct btrfs_inode inode;
	struct btrfs_time time = { 1700000000, 0 };
	enum btrfs_result result;

	REQUIRE(btrfs_image_lookup(fs, "/greeting", &inode) == BTRFS_OK);
	result = btrfs_transaction_begin(fs, &context->writer, &transaction);
	if (result != BTRFS_OK) {
		return result;
	}
	REQUIRE(
	    btrfs_transaction_write_inline(transaction, inode.id, "stale\n", 6, time) == BTRFS_OK);
	if (damage_after_begin) {
		synthetic(context, bt_super_offset(mirror), damage, sizeof(*damage));
	}
	result = btrfs_transaction_commit(transaction);
	btrfs_transaction_destroy(transaction);
	return result;
}

/* Admission requires agreeing copies; commit rejects copies changed underneath
 * it; recovery never rolls back, merges filesystems or discards a tree log. */
static void
copy_tests(struct context *context)
{
	struct bt_disk_super *primary;
	struct bt_disk_super *secondary;
	struct bt_disk_super *damage;
	struct btrfs_recovery_report report;
	struct btrfs_fs *fs;
	struct device *device = context->device;
	uint64_t generation = context->base_generation;
	size_t writes;

	primary = malloc(sizeof(*primary));
	secondary = malloc(sizeof(*secondary));
	damage = malloc(sizeof(*damage));
	REQUIRE(primary != NULL && secondary != NULL && damage != NULL);
	super_copy(context, 0, primary);
	super_copy(context, 1, secondary);
	REQUIRE(bt_super_same(primary, secondary));

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	*damage = *primary;
	bt_put64(&damage->generation, generation + 1);
	bt_super_seal(damage, BT_SUPER_OFFSET);
	synthetic(context, BT_SUPER_OFFSET, damage, sizeof(*damage));
	REQUIRE(begin_and_commit(context, fs, 0, NULL, 0) == BTRFS_STALE && device->count == 1);
	truncate_writes(device, 0);
	*damage = *primary;
	damage->label[0] ^= 1;
	bt_super_seal(damage, BT_SUPER_OFFSET);
	REQUIRE(begin_and_commit(context, fs, 1, damage, 0) == BTRFS_STALE && device->count == 1);
	truncate_writes(device, 0);
	*damage = *secondary;
	damage->label[0] ^= 1;
	bt_super_seal(damage, bt_super_offset(1));
	REQUIRE(begin_and_commit(context, fs, 1, damage, 1) == BTRFS_STALE && device->count == 1);
	truncate_writes(device, 0);

	/* A torn secondary blocks admission until explicit recovery rewrites it. */
	*damage = *secondary;
	((uint8_t *)damage)[DEVICE_SECTOR * 3] ^= 1;
	synthetic(context, bt_super_offset(1), damage, sizeof(*damage));
	REQUIRE(begin_and_commit(context, fs, 0, NULL, 0) == BTRFS_RECOVERY_REQUIRED);
	REQUIRE(btrfs_recover_supers(&context->env, NULL, generation, &report) ==
		BTRFS_RECOVERY_REQUIRED &&
	    report.selected == 0 && report.copies[1].status == BTRFS_CORRUPT);
	REQUIRE(btrfs_recover_supers(&context->env, NULL, generation + 1, &report) == BTRFS_STALE);
	writes = device->count;
	REQUIRE(btrfs_recover_supers(&context->env, &context->writer, generation, &report) ==
		BTRFS_OK &&
	    report.rewritten == 1 && device->count == writes + 1);
	show(&device->writes[writes], 1);
	REQUIRE(btrfs_recover_supers(&context->env, NULL, generation, &report) == BTRFS_OK &&
	    report.rewritten == 0);
	btrfs_unmount(fs);
	truncate_writes(device, 0);

	/* No valid copy: nothing is selected or written. */
	*damage = *primary;
	((uint8_t *)damage)[DEVICE_SECTOR * 3] ^= 1;
	synthetic(context, BT_SUPER_OFFSET, damage, sizeof(*damage));
	*damage = *secondary;
	((uint8_t *)damage)[DEVICE_SECTOR * 3] ^= 1;
	synthetic(context, bt_super_offset(1), damage, sizeof(*damage));
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_CORRUPT);
	REQUIRE(
	    btrfs_recover_supers(&context->env, &context->writer, 0, &report) == BTRFS_CORRUPT &&
	    device->count == 2);
	truncate_writes(device, 0);

	/* A torn primary with an intact secondary recovers the same generation. */
	*damage = *primary;
	((uint8_t *)damage)[DEVICE_SECTOR * 3] ^= 1;
	synthetic(context, BT_SUPER_OFFSET, damage, sizeof(*damage));
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_CORRUPT);
	REQUIRE(btrfs_recover_supers(&context->env, &context->writer, generation, &report) ==
		BTRFS_OK &&
	    report.selected == 1 && report.generation == generation && report.rewritten == 1);
	show(&device->writes[device->count - 1], 1);
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	check_invariants(fs);
	btrfs_unmount(fs);
	truncate_writes(device, 0);

	/* A newer copy from another filesystem is ambiguous, never selected. */
	*damage = *secondary;
	damage->fsid[0] ^= 1;
	bt_put64(&damage->generation, generation + 1);
	bt_super_seal(damage, bt_super_offset(1));
	synthetic(context, bt_super_offset(1), damage, sizeof(*damage));
	REQUIRE(
	    btrfs_recover_supers(&context->env, &context->writer, 0, &report) == BTRFS_CORRUPT &&
	    device->count == 1);
	truncate_writes(device, 0);

	/* Choosing a copy without the pending log would drop fsynced data. */
	*damage = *primary;
	bt_put64(&damage->log_root, bt_u64(primary->root));
	bt_super_seal(damage, BT_SUPER_OFFSET);
	synthetic(context, BT_SUPER_OFFSET, damage, sizeof(*damage));
	REQUIRE(btrfs_recover_supers(&context->env, &context->writer, 0, &report) ==
		BTRFS_UNSUPPORTED &&
	    device->count == 1);
	truncate_writes(device, 0);
	REQUIRE(context->image.live_allocations == 0);
	free(damage);
	free(secondary);
	free(primary);
	printf("superblock copies: stale rejection, admission and explicit recovery PASS\n");
}

typedef int (*item_match)(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument);

/* Copy the leaf holding the first matching item of a tree. */
static int
find_leaf(const struct btrfs_fs *fs, struct bt_root root, item_match match, void *argument,
    uint8_t *leaf, uint64_t *logical, uint32_t *slot)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key first = { 0 };
	enum btrfs_result error;
	int found = 0;

	bt_cursor_init(&cursor, fs, root);
	error = bt_cursor_seek(&cursor, first, 0);
	while (error == BTRFS_OK && !found) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		found = match(fs, &cursor, &record, argument);
		if (found) {
			memcpy(leaf, cursor.blocks[0], fs->info.node_size);
			*logical = cursor.loaded[0].address;
			*slot = cursor.slots[0];
		} else {
			error = bt_cursor_next(&cursor);
		}
	}
	REQUIRE(found || error == BTRFS_NOT_FOUND);
	bt_cursor_fini(&cursor);
	return found;
}

static void
replace_leaf(struct context *context, const struct btrfs_fs *fs, uint8_t *leaf, uint64_t logical,
    uint64_t kind)
{
	struct bt_le32 checksum;
	uint64_t physical;
	unsigned mirrors = 1;
	unsigned mirror;

	bt_put32(&checksum,
	    ~bt_crc32c(UINT32_MAX, leaf + BT_CSUM_SIZE, fs->info.node_size - BT_CSUM_SIZE));
	memset(leaf, 0, BT_CSUM_SIZE);
	memcpy(leaf, &checksum, sizeof(checksum));
	for (mirror = 0; mirror < mirrors; mirror++) {
		REQUIRE(bt_map(fs, logical, fs->info.node_size, kind, mirror, &physical,
			    &mirrors) == BTRFS_OK);
		synthetic(context, physical, leaf, fs->info.node_size);
	}
}

static struct bt_disk_item *
leaf_item(uint8_t *leaf, uint32_t slot)
{
	return (struct bt_disk_item *)(leaf + sizeof(struct bt_disk_header)) + slot;
}

static void *
leaf_data(uint8_t *leaf, uint32_t slot)
{
	return leaf + sizeof(struct bt_disk_header) + bt_u32(leaf_item(leaf, slot)->offset);
}

/* Key edits avoid slot zero, whose key a parent pointer also records. */
static int
match_metadata(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument)
{
	(void)fs;
	(void)argument;
	return record->key.type == BT_METADATA_ITEM && cursor->slots[0] > 0;
}

static int
match_successor(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument)
{
	const struct bt_disk_item *items =
	    (const void *)(cursor->blocks[0] + sizeof(struct bt_disk_header));

	(void)fs;
	(void)argument;
	return record->key.type == BT_METADATA_ITEM && cursor->slots[0] > 0 &&
	    items[cursor->slots[0] - 1].key.type == BT_METADATA_ITEM;
}

static int
match_group(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument)
{
	const struct bt_disk_block_group *group = (const void *)record->data;

	(void)fs;
	(void)argument;
	return record->key.type == BT_BLOCK_GROUP_ITEM && cursor->slots[0] > 0 &&
	    (bt_u64(group->flags) & BT_BLOCK_METADATA) != 0;
}

static int
match_last(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument)
{
	struct bt_key *last = argument;

	(void)fs;
	(void)cursor;
	return bt_key_compare(record->key, *last) == 0;
}

static int
match_group_at(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument)
{
	const uint64_t *logical = argument;

	(void)fs;
	(void)cursor;
	return record->key.type == BT_BLOCK_GROUP_ITEM && record->key.objectid == *logical;
}

static int
match_chunk(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument)
{
	const struct bt_disk_chunk *chunk = (const void *)record->data;
	const uint64_t *type = argument;

	(void)fs;
	(void)cursor;
	return record->key.type == BT_CHUNK_ITEM && bt_u64(chunk->type) == *type;
}

static void
expect_corrupt_map(struct context *context, const char *name)
{
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction = NULL;
	size_t writes = context->device->count;
	uint64_t live = context->image.live_allocations;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_CORRUPT);
	REQUIRE(transaction == NULL && context->device->count == writes);
	btrfs_unmount(fs);
	REQUIRE(context->image.live_allocations == live);
	truncate_writes(context->device, 0);
	printf("allocation map %s: PASS (corrupt)\n", name);
}

/* Checksum-correct allocation maps damaged by this test, not by the writer. */
static void
allocation_map_tests(struct context *context)
{
	struct btrfs_fs *fs;
	struct bt_root extents;
	struct bt_disk_extent_item *extent;
	struct bt_disk_block_group *group;
	struct bt_disk_item *item;
	struct bt_disk_chunk *chunk;
	struct bt_disk_stripe *stripes;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key last = { UINT64_MAX, UINT64_MAX, UINT8_MAX };
	uint8_t *leaf;
	uint64_t logical;
	uint64_t metadata_physical = 0;
	uint64_t type;
	uint64_t chunk_end = 0;
	uint64_t group_logical = UINT64_MAX;
	uint64_t length;
	uint32_t slot;
	size_t i;

	leaf = malloc(BT_MAX_NODE_SIZE);
	REQUIRE(leaf != NULL);
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(bt_find_root(fs, BT_EXTENT_TREE, &extents) == BTRFS_OK);
	for (i = 0; i < fs->chunk_count; i++) {
		if (fs->chunks[i].logical + fs->chunks[i].length > chunk_end) {
			chunk_end = fs->chunks[i].logical + fs->chunks[i].length;
		}
		if (fs->chunks[i].type & BT_BLOCK_METADATA) {
			metadata_physical = fs->chunks[i].physical[0];
		}
	}

	REQUIRE(find_leaf(fs, extents, match_group, NULL, leaf, &logical, &slot));
	group = leaf_data(leaf, slot);
	bt_put64(&group->used_bytes, bt_u64(group->used_bytes) + fs->info.node_size);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_corrupt_map(context, "block-group total");

	REQUIRE(find_leaf(fs, extents, match_group, NULL, leaf, &logical, &slot));
	leaf_item(leaf, slot)->key.type = BT_BLOCK_GROUP_ITEM + 1;
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_corrupt_map(context, "missing block group");

	REQUIRE(find_leaf(fs, extents, match_metadata, NULL, leaf, &logical, &slot));
	extent = leaf_data(leaf, slot);
	bt_put64(&extent->refs, 0);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_corrupt_map(context, "zero references");

	REQUIRE(find_leaf(fs, extents, match_metadata, NULL, leaf, &logical, &slot));
	extent = leaf_data(leaf, slot);
	bt_put64(&extent->generation, fs->info.generation + 1);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_corrupt_map(context, "future generation");

	REQUIRE(find_leaf(fs, extents, match_metadata, NULL, leaf, &logical, &slot));
	extent = leaf_data(leaf, slot);
	bt_put64(&extent->flags, BT_EXTENT_FLAG_DATA);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_corrupt_map(context, "data extent in metadata");

	REQUIRE(find_leaf(fs, extents, match_metadata, NULL, leaf, &logical, &slot));
	bt_put64(&leaf_item(leaf, slot)->key.offset, BT_MAX_LEVEL);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_corrupt_map(context, "tree level bound");

	REQUIRE(find_leaf(fs, extents, match_successor, NULL, leaf, &logical, &slot));
	item = leaf_item(leaf, slot);
	bt_put64(&item->key.objectid, bt_u64(item->key.objectid) + DEVICE_SECTOR);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_corrupt_map(context, "unaligned extent");

	if (fs->info.node_size > fs->info.sector_size) {
		REQUIRE(find_leaf(fs, extents, match_successor, NULL, leaf, &logical, &slot));
		item = leaf_item(leaf, slot);
		bt_put64(&item->key.objectid,
		    bt_u64(leaf_item(leaf, slot - 1)->key.objectid) + fs->info.sector_size);
		replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
		expect_corrupt_map(context, "overlapping extents");
	}

	/* The last extent record moves beyond every chunk while its block group total
	 * is reduced to match, so only the containment rule can reject it. Moving a
	 * record keeps key order only when it is the last record of the tree. */
	bt_cursor_init(&cursor, fs, extents);
	REQUIRE(bt_cursor_seek(&cursor, last, 1) == BTRFS_OK);
	REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
	last = record.key;
	bt_cursor_fini(&cursor);
	if (last.type == BT_EXTENT_ITEM || last.type == BT_METADATA_ITEM) {
		length = last.type == BT_METADATA_ITEM ? fs->info.node_size : last.offset;
		for (i = 0; i < fs->chunk_count; i++) {
			if (last.objectid - fs->chunks[i].logical < fs->chunks[i].length) {
				group_logical = fs->chunks[i].logical;
			}
		}
		REQUIRE(find_leaf(fs, extents, match_last, &last, leaf, &logical, &slot));
		bt_put64(&leaf_item(leaf, slot)->key.objectid, chunk_end + fs->info.node_size);
		replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
		REQUIRE(
		    find_leaf(fs, extents, match_group_at, &group_logical, leaf, &logical, &slot));
		group = leaf_data(leaf, slot);
		bt_put64(&group->used_bytes, bt_u64(group->used_bytes) - length);
		replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
		expect_corrupt_map(context, "extent outside chunks");
	} else {
		printf("allocation map extent outside chunks: not constructed on this image "
		       "(last record is a keyed backreference)\n");
	}

	/* Logical separation does not authorize writes into another chunk's stripe. */
	type = BT_BLOCK_DATA;
	REQUIRE(find_leaf(fs, fs->chunk_tree, match_chunk, &type, leaf, &logical, &slot));
	chunk = leaf_data(leaf, slot);
	stripes = (void *)(chunk + 1);
	REQUIRE(
	    metadata_physical != 0 && metadata_physical + bt_u64(chunk->length) <= fs->device_size);
	bt_put64(&stripes[0].offset, metadata_physical);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_SYSTEM);
	btrfs_unmount(fs);
	expect_corrupt_map(context, "physical chunk alias");
	free(leaf);
}

static int
match_inline_ref(const struct btrfs_fs *fs, const struct bt_cursor *cursor,
    const struct bt_record *record, void *argument)
{
	const uint8_t *type = argument;

	(void)fs;
	(void)cursor;
	return (record->key.type == BT_METADATA_ITEM || record->key.type == BT_EXTENT_ITEM) &&
	    record->size > sizeof(struct bt_disk_extent_item) &&
	    record->data[sizeof(struct bt_disk_extent_item)] == *type;
}

static void
expect_audit_failure(struct context *context, const char *name)
{
	struct reference_audit audit;
	struct btrfs_fs *fs;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(reference_audit(fs, &audit) != 0);
	btrfs_unmount(fs);
	truncate_writes(context->device, 0);
	printf("reference audit detects %s: PASS (%s)\n", name, audit.failure);
}

/* The audit is an oracle only if it rejects damaged references. */
static void
audit_self_test(struct context *context)
{
	struct bt_disk_extent_item *extent;
	struct bt_disk_data_ref *data;
	struct btrfs_fs *fs;
	struct bt_root extents;
	struct bt_le64 value;
	uint8_t *leaf;
	uint8_t *reference;
	uint8_t type;
	uint64_t logical;
	uint32_t slot;

	leaf = malloc(BT_MAX_NODE_SIZE);
	REQUIRE(leaf != NULL);
	audit_state(context, "fixture");
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(bt_find_root(fs, BT_EXTENT_TREE, &extents) == BTRFS_OK);
	type = BT_TREE_BLOCK_REF;
	REQUIRE(find_leaf(fs, extents, match_inline_ref, &type, leaf, &logical, &slot));
	reference = (uint8_t *)leaf_data(leaf, slot) + sizeof(*extent) + 1;
	memcpy(&value, reference, sizeof(value));
	bt_put64(&value, bt_u64(value) + 1);
	memcpy(reference, &value, sizeof(value));
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_audit_failure(context, "a wrong tree reference");

	REQUIRE(find_leaf(fs, extents, match_inline_ref, &type, leaf, &logical, &slot));
	extent = leaf_data(leaf, slot);
	bt_put64(&extent->refs, bt_u64(extent->refs) + 1);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_audit_failure(context, "an inconsistent reference total");

	type = BT_EXTENT_DATA_REF;
	REQUIRE(find_leaf(fs, extents, match_inline_ref, &type, leaf, &logical, &slot));
	extent = leaf_data(leaf, slot);
	data = (void *)((uint8_t *)extent + sizeof(*extent) + 1);
	bt_put64(&extent->refs, bt_u64(extent->refs) + 1);
	bt_put32(&data->count, bt_u32(data->count) + 1);
	replace_leaf(context, fs, leaf, logical, BT_BLOCK_METADATA);
	expect_audit_failure(context, "an overcounted data reference");
	btrfs_unmount(fs);
	REQUIRE(context->image.live_allocations == 0);
	free(leaf);
}

static int
inline_regular(const struct btrfs_inode *inode)
{
	return (inode->mode & BTRFS_MODE_TYPE) == BTRFS_MODE_REGULAR && inode->size <= INLINE_LIMIT;
}

static size_t
collect_inline(struct btrfs_fs *fs, const char *path, struct btrfs_object_id *ids, size_t count,
    size_t capacity)
{
	struct btrfs_directory *stream;
	struct btrfs_dir_entry entry;
	struct btrfs_inode directory;
	struct btrfs_inode inode;
	uint64_t cookie;
	enum btrfs_result error;

	REQUIRE(btrfs_image_lookup(fs, path, &directory) == BTRFS_OK);
	REQUIRE(btrfs_directory_open(fs, &directory, 0, &stream) == BTRFS_OK);
	while ((error = btrfs_directory_next(stream, &entry, &cookie)) == BTRFS_OK) {
		REQUIRE(btrfs_get_inode(fs, entry.id, &inode) == BTRFS_OK);
		if (inline_regular(&inode)) {
			REQUIRE(count < capacity);
			ids[count++] = entry.id;
		}
	}
	REQUIRE(error == BTRFS_NOT_FOUND);
	btrfs_directory_close(stream);
	return count;
}

/* The Linux fixture's metadata is full and fragmented. A transaction needing
 * more nodes than remain must fail with NO_SPACE before any media write. */
static void
exhaustion_test(struct context *context)
{
	struct bt_mutation_allocator allocator;
	struct btrfs_object_id *ids;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct btrfs_time time = { 1700000000, 0 };
	struct bt_space *space;
	struct bt_root extents;
	struct btrfs_inode inode;
	uint8_t data[INLINE_LIMIT];
	uint8_t *big;
	uint64_t logical;
	size_t available = 0;
	size_t count = 0;
	size_t i;
	enum btrfs_result result;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(bt_find_root(fs, BT_EXTENT_TREE, &extents) == BTRFS_OK);
	REQUIRE(bt_space_create(fs, extents, TRANSACTION_NODE_LIMIT, &space) == BTRFS_OK);
	bt_space_allocator(space, &allocator);
	while (available < RESERVATION_PROBE_LIMIT &&
	    allocator.reserve(allocator.context, BTRFS_TOP_LEVEL_TREE, 0, &logical) == BTRFS_OK) {
		available++;
	}
	bt_space_destroy(space);
	REQUIRE(available != 0 && available < TRANSACTION_NODE_LIMIT / 2);
	ids = malloc(RESERVATION_PROBE_LIMIT * sizeof(*ids));
	REQUIRE(ids != NULL);
	count = collect_inline(fs, "/meta", ids, count, RESERVATION_PROBE_LIMIT);
	count = collect_inline(fs, "/many", ids, count, RESERVATION_PROBE_LIMIT);
	for (i = 0; i < INLINE_LIMIT; i++) {
		data[i] = (uint8_t)i;
	}
	context->device->issued = 0;
	context->device->flushes = 0;
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	result = BTRFS_OK;
	for (i = 0; result == BTRFS_OK && i < count; i++) {
		result =
		    btrfs_transaction_write_inline(transaction, ids[i], data, INLINE_LIMIT, time);
	}
	if (result == BTRFS_OK) {
		result = btrfs_transaction_commit(transaction);
	}
	REQUIRE(result == BTRFS_NO_SPACE);
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_NO_SPACE);
	REQUIRE(context->device->count == 0 && context->device->issued == 0 &&
	    context->device->flushes == 0);
	btrfs_transaction_destroy(transaction);
	/* Data space is exhausted as well: a large write fails before any I/O. */
	big = malloc(EXHAUSTION_WRITE_BYTES);
	REQUIRE(big != NULL);
	memset(big, 0x5a, EXHAUSTION_WRITE_BYTES);
	REQUIRE(btrfs_image_lookup(fs, "/big", &inode) == BTRFS_OK);
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_write(
		    transaction, inode.id, 0, big, EXHAUSTION_WRITE_BYTES, time) == BTRFS_NO_SPACE);
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_NO_SPACE);
	REQUIRE(context->device->count == 0 && context->device->issued == 0);
	btrfs_transaction_destroy(transaction);
	free(big);
	check_invariants(fs);
	btrfs_unmount(fs);
	free(ids);
	REQUIRE(context->image.live_allocations == 0);
	printf("metadata and data exhaustion: %zu reservable nodes, %zu files stopped after %zu "
	       "edits; "
	       "no write PASS\n",
	    available, count, i);
}

int
main(int argc, char **argv)
{
	struct context *context;
	struct btrfs_fs *fs;
	struct btrfs_info info;
	const char *image = NULL;
	int full = 0;
	int shared = 0;
	int keyed = 0;
	int data = 0;
	int fragment = 0;
	int grow = 0;
	int names = 0;
	int i;

	context = calloc(1, sizeof(*context));
	REQUIRE(context != NULL);
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--export") == 0 && i + 1 < argc) {
			context->export_root = argv[++i];
		} else if (strcmp(argv[i], "--full") == 0) {
			full = 1;
		} else if (strcmp(argv[i], "--shared") == 0) {
			shared = 1;
		} else if (strcmp(argv[i], "--keyed") == 0) {
			keyed = 1;
		} else if (strcmp(argv[i], "--data") == 0) {
			data = 1;
		} else if (strcmp(argv[i], "--fragment") == 0) {
			fragment = 1;
		} else if (strcmp(argv[i], "--grow") == 0) {
			grow = 1;
		} else if (strcmp(argv[i], "--namespace") == 0) {
			names = 1;
		} else {
			REQUIRE(image == NULL);
			image = argv[i];
		}
	}
	REQUIRE(image != NULL);
	REQUIRE(btrfs_image_open(image, &context->image) == 0);
	context->device = calloc(1, sizeof(*context->device));
	REQUIRE(context->device != NULL);
	context->device->image = &context->image;
	context->env = context->image.environment;
	context->env.context = context->device;
	context->env.read = read_device;
	context->env.allocate = allocate;
	context->env.release = release;
	context->env.decompress = decompress_device;
	context->writer =
	    (struct btrfs_write_environment){ context->device, write_device, flush_device };
	context->seed = UINT32_C(0x142857);
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	btrfs_get_info(fs, &info);
	context->base_generation = info.generation;
	context->node_size = info.node_size;
	context->sector_size = info.sector_size;
	check_invariants(fs);
	btrfs_unmount(fs);
	admission_tests(context);
	copy_tests(context);
	allocation_map_tests(context);
	audit_self_test(context);
	if (full) {
		exhaustion_test(context);
	}
	plan_scenarios(context);
	if (shared) {
		shared_scenarios(context);
	}
	if (keyed) {
		keyed_scenarios(context);
	}
	if (data) {
		data_scenarios(context);
	}
	if (fragment) {
		fragment_scenarios(context);
	}
	if (grow) {
		grow_scenarios(context);
	}
	if (names) {
		namespace_scenarios(context);
	}
	REQUIRE(context->image.live_allocations == 0);
	btrfs_image_close(&context->image);
	free(context->device->writes);
	free(context->device);
	printf("transactions (%u-byte nodes): %zu crash states, %zu explicit recoveries; fault "
	       "sweeps, stale copies and allocation maps PASS\n",
	    context->node_size, context->states, context->recoveries);
	free(context);
	return 0;
}
