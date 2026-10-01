/* SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 200809L
#include "../adapters/posix/image.h"
#include "encode.h"
#include "references.h"
#include "space.h"
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
#define MAX_FILES 32U
#define MAX_STAGES 4U
#define MAX_OPERATIONS 32U
#define MAX_FILE_BYTES (UINT64_C(16) * 1024 * 1024)
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
#define SYNTHETIC_COMMIT SIZE_MAX

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

enum operation_kind { OPERATION_INLINE, OPERATION_WRITE, OPERATION_TRUNCATE };

struct operation {
	size_t file;
	enum operation_kind kind;
	uint64_t offset;
	uint8_t *data;
	size_t size;
};

/* Source-controlled operation sequence: stage 0 is the Linux fixture and stage
 * k is the expected logical state after the k-th acknowledged commit, computed
 * by applying that commit's operations to a byte model of each file. */
struct plan {
	const char *name;
	struct tracked files[MAX_FILES];
	size_t file_count;
	size_t commits;
	struct operation operations[MAX_STAGES][MAX_OPERATIONS];
	size_t operation_count[MAX_STAGES];
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
	for (i = 0; i < plan->file_count; i++) {
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
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	file->data[0] = read_file(fs, path, &file->size[0]);
	btrfs_unmount(fs);
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
		}
	}
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
	size_t exported = 0;
	unsigned epoch;
	unsigned pattern;
	unsigned choice;

	/* Every prefix is checked; about EXPORT_PREFIXES evenly spaced ones are exported. */
	for (prefix = 0; prefix <= count; prefix++) {
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

static enum btrfs_result
attempt(struct context *context, const struct plan *plan, size_t commit, enum fault fault,
    size_t point, struct totals *totals)
{
	struct btrfs_object_id ids[MAX_OPERATIONS];
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction = NULL;
	struct btrfs_inode inode;
	struct btrfs_time time = { 1700000000 + (int64_t)commit, 123456789 };
	const struct operation *operation;
	struct device *device = context->device;
	uint64_t allocations;
	uint64_t reads;
	size_t i;
	enum btrfs_result result;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	for (i = 0; i < plan->operation_count[commit]; i++) {
		REQUIRE(btrfs_image_lookup(fs, plan->files[plan->operations[commit][i].file].path,
			    &inode) == BTRFS_OK);
		ids[i] = inode.id;
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
		operation = &plan->operations[commit][i];
		if (operation->kind == OPERATION_INLINE) {
			result = btrfs_transaction_write_inline(
			    transaction, ids[i], operation->data, operation->size, time);
		} else if (operation->kind == OPERATION_WRITE) {
			result = btrfs_transaction_write(transaction, ids[i], operation->offset,
			    operation->data, operation->size, time);
		} else {
			result = btrfs_transaction_truncate(
			    transaction, ids[i], operation->offset, time);
		}
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
		stride = (limits[fault] + FAULT_POINTS - 1) / FAULT_POINTS;
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

/* Every committed root set must satisfy the independent reference audit. */
static void
audit_state(struct context *context, const char *name)
{
	struct reference_audit audit;
	struct btrfs_fs *fs;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	if (reference_audit(fs, &audit) != 0) {
		fprintf(stderr, "%s: reference audit: %s\n", name, audit.failure);
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

	plan_finish(plan);
	export_begin(context, plan, &exporter);
	for (commit = 1; commit <= plan->commits; commit++) {
		first = device->count;
		REQUIRE(attempt(context, plan, commit, FAULT_NONE, 0, &totals) == BTRFS_OK);
		REQUIRE(totals.flushes == BARRIERS && totals.writes == device->count - first);
		REQUIRE(device->writes[device->count - 1].offset == BT_SUPER_OFFSET);
		printf("%s commit %zu: %zu writes, %zu barriers, %llu allocations, %llu reads\n",
		    plan->name, commit, totals.writes, totals.flushes,
		    (unsigned long long)totals.allocations, (unsigned long long)totals.reads);
		export_writes(context, &exporter);
		audit_state(context, plan->name);
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

	memset(&plan, 0, sizeof(plan));
	plan.name = "replace";
	plan_update(context, &plan, 1, "/greeting", replacement, sizeof(replacement) - 1);
	run_plan(context, &plan);

	memset(&plan, 0, sizeof(plan));
	plan.name = "empty";
	plan_update(context, &plan, 1, "/greeting", NULL, 0);
	run_plan(context, &plan);

	memset(&plan, 0, sizeof(plan));
	plan.name = "maximum";
	for (i = 0; i < INLINE_LIMIT; i++) {
		data[i] = (uint8_t)(i * 7 + 3);
	}
	plan_update(context, &plan, 1, "/greeting", data, INLINE_LIMIT);
	run_plan(context, &plan);

	/* Many inodes in several leaves, with growing items, in one transaction. */
	memset(&plan, 0, sizeof(plan));
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
	memset(&plan, 0, sizeof(plan));
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

	memset(&plan, 0, sizeof(plan));
	plan.name = "shared-source";
	for (i = 0; i < sizeof(spread) / sizeof(spread[0]); i++) {
		shared_update(context, &plan, 1, "shared", spread[i]);
	}
	run_plan(context, &plan);

	memset(&plan, 0, sizeof(plan));
	plan.name = "shared-snapshot";
	for (i = 0; i < sizeof(spread) / sizeof(spread[0]); i++) {
		shared_update(context, &plan, 1, "shared-snap", spread[i]);
	}
	run_plan(context, &plan);

	/* The source converts shared blocks first; the snapshot then CoWs blocks
	 * with parent-named references, and the source continues afterwards. */
	memset(&plan, 0, sizeof(plan));
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
	memset(&plan, 0, sizeof(plan));
	plan.name = "shared-extents";
	plan_file(context, &plan, "/shared-ro/tail");
	plan_update(context, &plan, 1, "/shared/tail", "source tail\n", 12);
	plan_update(context, &plan, 2, "/shared-snap/tail", "snapshot tail\n", 14);
	run_plan(context, &plan);

	/* Leaves shared by exactly two trees hold data references. The source moves
	 * them to parent-named references; the snapshot then holds the last
	 * reference, converts them back and frees the old leaves. */
	memset(&plan, 0, sizeof(plan));
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
	memset(&plan, 0, sizeof(plan));
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

	memset(&plan, 0, sizeof(plan));
	plan.name = "keyed-source";
	keyed_update(context, &plan, 1, "keyed", "i00");
	keyed_update(context, &plan, 1, "keyed", "i20");
	keyed_update(context, &plan, 1, "keyed", "last");
	run_plan(context, &plan);

	memset(&plan, 0, sizeof(plan));
	plan.name = "keyed-snapshot";
	keyed_update(context, &plan, 1, "keyed-07", "i00");
	keyed_update(context, &plan, 1, "keyed-07", "last");
	run_plan(context, &plan);

	memset(&plan, 0, sizeof(plan));
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

	memset(&plan, 0, sizeof(plan));
	plan.name = "data-overwrite";
	track_data(context, &plan, "big");
	plan_file(context, &plan, "/data/big-clone");
	fill_pattern(data, 8192, 1);
	plan_write(context, &plan, 1, "/data/big", 300000, data, 8192);
	run_plan(context, &plan);

	memset(&plan, 0, sizeof(plan));
	plan.name = "data-append";
	track_data(context, &plan, "small");
	fill_pattern(data, 5000, 2);
	plan_write(context, &plan, 1, "/data/small", 10000, data, 5000);
	fill_pattern(data, 10, 3);
	plan_write(context, &plan, 1, "/data/small", 4090, data, 10);
	run_plan(context, &plan);

	memset(&plan, 0, sizeof(plan));
	plan.name = "data-hole";
	track_data(context, &plan, "sparse");
	fill_pattern(data, 4096, 4);
	plan_write(context, &plan, 1, "/data/sparse", 2 * 1024 * 1024, data, 4096);
	fill_pattern(data, 100, 5);
	plan_write(context, &plan, 1, "/data/sparse", 6 * 1024 * 1024 + 7, data, 100);
	run_plan(context, &plan);

	memset(&plan, 0, sizeof(plan));
	plan.name = "data-prealloc";
	track_data(context, &plan, "prealloc");
	fill_pattern(data, 4096, 6);
	plan_write(context, &plan, 1, "/data/prealloc", 65536, data, 4096);
	fill_pattern(data, 100, 7);
	plan_write(context, &plan, 1, "/data/prealloc", 1000, data, 100);
	run_plan(context, &plan);

	memset(&plan, 0, sizeof(plan));
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

	memset(&plan, 0, sizeof(plan));
	plan.name = "data-compressed";
	track_data(context, &plan, "zlib");
	fill_pattern(data, 4096, 8);
	plan_write(context, &plan, 1, "/data/zlib", 65536, data, 4096);
	fill_pattern(data, 7, 9);
	plan_write(context, &plan, 1, "/data/zlib", 100003, data, 7);
	run_plan(context, &plan);

	memset(&plan, 0, sizeof(plan));
	plan.name = "data-nodatasum";
	track_data(context, &plan, "nodatasum");
	fill_pattern(data, 5000, 10);
	plan_write(context, &plan, 1, "/data/nodatasum", 3000, data, 5000);
	run_plan(context, &plan);

	memset(&plan, 0, sizeof(plan));
	plan.name = "data-inline";
	track_data(context, &plan, "inline");
	fill_pattern(data, 5000, 11);
	plan_write(context, &plan, 1, "/data/inline", 0, data, 5000);
	plan_write(context, &plan, 2, "/data/inline", 9000, "X", 1);
	run_plan(context, &plan);

	/* The source and its writable snapshot overwrite shared extents in turn;
	 * the third commit drops the first commit's extent entirely. */
	memset(&plan, 0, sizeof(plan));
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
	memset(&plan, 0, sizeof(plan));
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
	memset(&plan, 0, sizeof(plan));
	plan.name = "fst-free";
	plan_truncate(context, &plan, 1, "/fragment/f001", 0);
	plan_truncate(context, &plan, 1, "/fragment/f003", 0);
	plan_truncate(context, &plan, 2, "/fragment/f255", 1000);
	run_plan(context, &plan);

	memset(&plan, 0, sizeof(plan));
	plan.name = "fst-fill";
	track_data(context, &plan, "small");
	fill_pattern(data, FRAGMENT_WRITE_BYTES, 17);
	plan_write(context, &plan, 1, "/data/small", 0, data, FRAGMENT_WRITE_BYTES);
	run_plan(context, &plan);
	free(data);
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
