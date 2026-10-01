/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_TEST_SCENARIO_H
#define MACHLIN_BTRFS_TEST_SCENARIO_H

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
	OPERATION_CLEAN_ORPHANS,
	OPERATION_SET_ATTRIBUTES,
	OPERATION_KEEP_PRIVILEGES,
	OPERATION_DROP_PRIVILEGES
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
	struct btrfs_time access_time;
	struct btrfs_time modify_time;
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
	EXPECT_FEATURE,
	EXPECT_TIMES
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
	/* EXPECT_TIMES: seconds of the access and modification times. */
	int64_t access_seconds;
	int64_t modify_seconds;
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

/* Recorded device (tests/scenario_device.c). */
uint32_t next_random(struct context *context);
size_t sectors(const struct saved_write *write);
void show(struct saved_write *write, int visible);
enum btrfs_result read_device(void *context, uint64_t offset, void *bytes, size_t size);
enum btrfs_result decompress_device(void *context, enum btrfs_compression codec, const void *input,
    size_t input_size, void *output, size_t output_size);
void *allocate(void *context, size_t size);
void release(void *context, void *bytes, size_t size);
struct saved_write *record(struct device *device, uint64_t offset, const void *bytes, size_t size);
enum btrfs_result write_device(void *context, uint64_t offset, const void *bytes, size_t size);
enum btrfs_result flush_device(void *context);
void truncate_writes(struct device *device, size_t count);
void synthetic(struct context *context, uint64_t offset, const void *bytes, size_t size);
void read_exact(struct context *context, uint64_t offset, void *bytes, size_t size);

/* Plans, expectations and state checks (tests/scenario_plan.c). */
void check_invariants(struct btrfs_fs *fs);
void check_stage(
    struct context *context, struct btrfs_fs *fs, const struct plan *plan, size_t stage);
void plan_init(struct plan *plan);
size_t plan_file(struct context *context, struct plan *plan, const char *path);
void plan_update(struct context *context, struct plan *plan, size_t commit, const char *path,
    const void *data, size_t size);
void plan_write(struct context *context, struct plan *plan, size_t commit, const char *path,
    uint64_t offset, const void *data, size_t size);
void plan_truncate(
    struct context *context, struct plan *plan, size_t commit, const char *path, uint64_t size);
void plan_finish(struct plan *plan);
void plan_destroy(struct plan *plan);
void fill_pattern(uint8_t *data, size_t size, unsigned seed);
void plan_create(
    struct plan *plan, size_t commit, const char *path, uint32_t mode, const char *symlink);
void plan_device(
    struct plan *plan, size_t commit, const char *path, uint32_t mode, uint64_t device);
void plan_link(struct plan *plan, size_t commit, const char *path, const char *target);
void plan_unlink(struct plan *plan, size_t commit, const char *path, int open);
void plan_rename(
    struct plan *plan, size_t commit, const char *path, const char *target, int target_open);
void plan_set_xattr(struct plan *plan, size_t commit, const char *path, const char *name,
    const void *value, size_t size, int flags);
void plan_remove_xattr(struct plan *plan, size_t commit, const char *path, const char *name);
void plan_write_new(struct plan *plan, size_t commit, const char *path, uint64_t offset,
    const void *data, size_t size);
void plan_evict(struct context *context, struct plan *plan, size_t commit, const char *path);
void plan_clean(struct plan *plan, size_t commit, uint64_t tree, size_t expected);
void plan_set_attributes(struct plan *plan, size_t commit, const char *path, unsigned mask,
    uint32_t mode, uint32_t uid, uint32_t gid, int64_t access_seconds, int64_t modify_seconds);
void plan_privileges(struct plan *plan, size_t commit, const char *path, int keep);
struct expectation *expect(
    struct plan *plan, size_t first, size_t last, enum expectation_kind kind, const char *path);
void expect_absent(struct plan *plan, size_t first, size_t last, const char *path);
void expect_file(
    struct plan *plan, size_t first, size_t last, const char *path, const void *bytes, size_t size);
void expect_text(struct plan *plan, size_t first, size_t last, const char *path, const char *text);
void expect_current(struct context *context, struct plan *plan, size_t first, size_t last,
    const char *path, const char *source);
void expect_symlink(
    struct plan *plan, size_t first, size_t last, const char *path, const char *target);
void expect_same(struct plan *plan, size_t first, size_t last, const char *path, const char *other);
void expect_xattr(struct plan *plan, size_t first, size_t last, const char *path, const char *name,
    const void *value, size_t size);
void expect_stat(
    struct plan *plan, size_t first, size_t last, const char *path, uint32_t mode, uint32_t links);
void expect_links(struct context *context, struct plan *plan, size_t first, size_t last,
    const char *path, const char *source, uint32_t links);
void expect_value(struct plan *plan, size_t first, size_t last, enum expectation_kind kind,
    const char *path, uint64_t value);
void expect_owner(struct plan *plan, size_t first, size_t last, const char *path, uint32_t mode,
    uint32_t uid, uint32_t gid, uint32_t links);
void expect_times(struct plan *plan, size_t first, size_t last, const char *path,
    int64_t access_seconds, int64_t modify_seconds);
void expect_flags(
    struct plan *plan, size_t first, size_t last, const char *path, uint64_t mask, uint64_t value);
void expect_names(struct plan *plan, size_t first, size_t last, const char *path,
    const char **names, size_t count);
void expect_listing(struct context *context, struct plan *plan, size_t first, size_t last,
    const char *path, const char *const *added, const char *const *removed);

/* Execution, crash states, fault sweeps and export (tests/scenario_run.c). */
size_t chunk_count(struct context *context);
void audit_state(struct context *context, const char *name);
void run_plan(struct context *context, struct plan *plan);

/* Scenario sets. */
void plan_scenarios(struct context *context);
void shared_scenarios(struct context *context);
void keyed_scenarios(struct context *context);
void data_scenarios(struct context *context);
void fragment_scenarios(struct context *context);
void grow_scenarios(struct context *context);

/* Scenario sets. */
struct btrfs_object_id object(struct btrfs_fs *fs, const char *path);
void namespace_scenarios(struct context *context);

/* Scenario sets. */
void admission_tests(struct context *context);
void copy_tests(struct context *context);
void allocation_map_tests(struct context *context);
void audit_self_test(struct context *context);
void exhaustion_test(struct context *context);

#endif
