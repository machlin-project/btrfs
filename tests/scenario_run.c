/* SPDX-License-Identifier: BSD-3-Clause */
/* Running plans: commits with fault injection, crash states and their
 * resolution by mount and explicit recovery, audits and export. */
#define _POSIX_C_SOURCE 200809L
#include "scenario.h"

/* Superblock copies the device holds: two, or three past 256 GiB. */
static unsigned
super_copies(const struct context *context)
{
	unsigned copies = 0;
	unsigned mirror;

	for (mirror = 0; mirror < BTRFS_SUPER_COPIES; mirror++) {
		copies += bt_super_present(context->image.environment.size_bytes, mirror) != 0;
	}
	return copies;
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
	context->crash_commit = latest;
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
	REQUIRE(report.present == super_copies(context) && report.selected < BTRFS_SUPER_COPIES);
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
	context->crash_commit = 0;
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
		"noxattr", "stat", "device", "flags", "feature", "times", "reference", "subvolume",
		"subvolumes", "deleted", "compressed", "extents", "holes", "bitmaps", "groups",
		"quota" };
	const struct expectation *e;
	char payload[64];
	char argument[64];
	const char *detail;
	FILE *manifest;
	size_t stage;
	size_t i;

	_Static_assert(sizeof(kinds) / sizeof(kinds[0]) == EXPECT_QUOTA + 1, "expectation kinds");
	(void)context;
	manifest = export_open(exporter, "namespace.tsv");
	for (i = 0; i < plan->expectation_count; i++) {
		e = &plan->expectations[i];
		strcpy(payload, "-");
		if (e->kind == EXPECT_FILE || e->kind == EXPECT_SYMLINK ||
		    e->kind == EXPECT_DIRECTORY || e->kind == EXPECT_XATTR ||
		    e->kind == EXPECT_SUBVOLUMES) {
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
		} else if (e->kind == EXPECT_REFERENCE) {
			detail = e->value != 0 ? "extended" : "inode";
		} else if (e->kind == EXPECT_COMPRESSED) {
			/* codec:regular:inline */
			REQUIRE(e->value == BTRFS_COMPRESSION_ZLIB ||
			    e->value == BTRFS_COMPRESSION_ZSTD);
			REQUIRE(snprintf(argument, sizeof(argument), "%s:%u:%u",
				    e->value == BTRFS_COMPRESSION_ZLIB ? "zlib" : "zstd", e->links,
				    e->mode) < (int)sizeof(argument));
			detail = argument;
		} else if (e->kind == EXPECT_QUOTA) {
			/* consistent or inconsistent:qgroups */
			REQUIRE(snprintf(argument, sizeof(argument), "%s:%llu",
				    e->links != 0 ? "inconsistent" : "consistent",
				    (unsigned long long)e->value) < (int)sizeof(argument));
			detail = argument;
		} else if (e->kind == EXPECT_GROUPS) {
			/* block groups:system array entries */
			REQUIRE(
			    snprintf(argument, sizeof(argument), "%llu:%u",
				(unsigned long long)e->value, e->links) < (int)sizeof(argument));
			detail = argument;
		} else if (e->kind == EXPECT_EXTENTS) {
			/* regular:preallocated:distinct disk extents */
			REQUIRE(snprintf(argument, sizeof(argument), "%u:%u:%llu", e->links,
				    e->mode, (unsigned long long)e->value) < (int)sizeof(argument));
			detail = argument;
		} else if (e->kind == EXPECT_DELETED || e->kind == EXPECT_HOLES ||
		    e->kind == EXPECT_BITMAPS) {
			REQUIRE(snprintf(argument, sizeof(argument), "%llu",
				    (unsigned long long)e->value) < (int)sizeof(argument));
			detail = argument;
		} else if (e->kind == EXPECT_SUBVOLUME) {
			/* read-only flag and the snapshot source's path */
			REQUIRE(snprintf(argument, sizeof(argument), "%s:%s",
				    e->value != 0 ? "ro" : "rw",
				    e->other != NULL ? e->other : "-") < (int)sizeof(argument));
			detail = argument;
		} else if (e->kind == EXPECT_TIMES) {
			/* stat -c '%X:%Y' */
			REQUIRE(snprintf(argument, sizeof(argument), "%lld:%lld",
				    (long long)e->access_seconds,
				    (long long)e->modify_seconds) < (int)sizeof(argument));
			detail = argument;
		}
		for (stage = e->first; stage <= e->last && stage <= plan->commits; stage++) {
			REQUIRE(fprintf(manifest, "%zu\t%s\t%s\t%s\t%s\n", stage, kinds[e->kind],
				    e->path, detail, payload) > 0);
		}
	}
	REQUIRE(fclose(manifest) == 0);
	if (plan->volatile_count != 0) {
		/* volatile.tsv: commit, path, offset and length of in-place bytes. */
		manifest = export_open(exporter, "volatile.tsv");
		for (i = 0; i < plan->volatile_count; i++) {
			REQUIRE(fprintf(manifest, "%zu\t%s\t%llu\t%llu\n",
				    plan->volatiles[i].commit, plan->volatiles[i].path,
				    (unsigned long long)plan->volatiles[i].offset,
				    (unsigned long long)plan->volatiles[i].length) > 0);
		}
		REQUIRE(fclose(manifest) == 0);
	}
}

static void
export_begin(struct context *context, const struct plan *plan, struct exporter *exporter)
{
	memset(exporter, 0, sizeof(*exporter));
	if (context->export_root == NULL) {
		return;
	}
	REQUIRE(snprintf(exporter->directory, sizeof(exporter->directory), "%s/%s",
		    context->export_root, plan->name) < (int)sizeof(exporter->directory));
	REQUIRE(mkdir(exporter->directory, 0755) == 0);
	if (plan->namespace) {
		export_namespace(context, plan, exporter);
	}
	exporter->cases = export_open(exporter, "cases.tsv");
}

/* Stage contents, once truncation steps fixed every stage's sizes. */
static void
export_stages(struct context *context, const struct plan *plan, struct exporter *exporter)
{
	char name[64];
	FILE *stages;
	size_t stage;
	size_t earlier;
	size_t i;
	size_t j = 0;
	int found;

	if (exporter->cases == NULL) {
		return;
	}
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
	size_t copies;
	size_t copy;
	size_t samples;
	size_t combination;
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
		/* A superblock epoch writes one copy per device location past the
		 * primary's; every combination of tear patterns across them is a
		 * state (two secondaries on devices over 256 GiB). */
		copies = 0;
		for (i = first; i < last; i++) {
			copies += device->writes[i].epoch == epoch;
		}
		samples = METADATA_SAMPLES + 2;
		if (epoch != 0) {
			REQUIRE(copies >= 1 && copies <= SUPER_COPIES_TORN);
			for (samples = 1, copy = 0; copy < copies; copy++) {
				samples *= TEAR_PATTERNS;
			}
		}
		for (sample = 0; sample < samples; sample++) {
			combination = sample;
			for (i = first; i < last; i++) {
				write = &device->writes[i];
				show(write, write->epoch < epoch);
				if (write->epoch != epoch) {
					continue;
				}
				if (epoch != 0) {
					for (sector = 0; sector < sectors(write); sector++) {
						write->visible[sector] = (uint8_t)tear(
						    (unsigned)(combination % TEAR_PATTERNS), sector,
						    sectors(write));
					}
					combination /= TEAR_PATTERNS;
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
	/* A path below a committed file does not exist either. */
	result = btrfs_image_lookup(fs, copy, &inode);
	REQUIRE(result == BTRFS_OK || result == BTRFS_NOT_FOUND || result == BTRFS_NOT_DIRECTORY);
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
	if (entry->path == NULL || !entry->present) {
		fprintf(stderr, "unresolved path %s\n", copy);
		exit(1);
	}
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
	case OPERATION_EXCHANGE:
	case OPERATION_RENAME_WHITEOUT:
		path_prepare(table, fs, operation->path, 0);
		path_prepare(table, fs, operation->path, 1);
		path_prepare(table, fs, operation->target, 0);
		path_prepare(table, fs, operation->target, 1);
		break;
	case OPERATION_TMPFILE:
		path_prepare(table, fs, operation->path, 1);
		break;
	case OPERATION_LINK_TMPFILE:
		path_prepare(table, fs, operation->path, 0);
		path_prepare(table, fs, operation->target, 1);
		break;
	case OPERATION_EVICT:
	case OPERATION_CLEAN_ORPHANS:
	case OPERATION_REMOVE_GROUPS:
	case OPERATION_SYSTEM_GROWTH:
		break;
	case OPERATION_SUBVOLUME:
	case OPERATION_DELETE_SUBVOLUME:
		path_prepare(table, fs, operation->path, 1);
		break;
	case OPERATION_CLEAN_SUBVOLUMES:
		break;
	case OPERATION_SNAPSHOT:
		path_prepare(table, fs, operation->path, 0);
		path_prepare(table, fs, operation->target, 1);
		break;
	default:
		path_prepare(table, fs, operation->path, 0);
		break;
	}
}

/* Stands in for a chunk tree that has filled its system chunks: takes every
 * remaining system-space reservation until the allocator grows a system
 * chunk, so this commit's chunk-tree blocks must come from the new chunk,
 * which only the superblock's system array can locate. */
static enum btrfs_result
system_growth(struct btrfs_transaction *transaction)
{
	struct bt_mutation_allocator allocator;
	uint64_t logical = 0;
	uint64_t steps;
	size_t original = bt_space_original_chunks(transaction->space);
	size_t i;
	enum btrfs_result result = BTRFS_OK;

	bt_space_allocator(transaction->space, &allocator);
	for (steps = 0; result == BTRFS_OK && steps < RESERVATION_PROBE_LIMIT; steps++) {
		result = allocator.reserve(allocator.context, BT_CHUNK_TREE, 0, &logical);
		for (i = original; result == BTRFS_OK && i < transaction->fs.chunk_count; i++) {
			if (logical - transaction->fs.chunks[i].logical <
			    transaction->fs.chunks[i].length) {
				return BTRFS_OK;
			}
		}
	}
	return result == BTRFS_OK ? BTRFS_UNSUPPORTED : result;
}

/* Truncation, eviction and orphan cleanup in the native writers' steps, run
 * to completion within the transaction. */
static enum btrfs_result
truncate_all(struct btrfs_transaction *transaction, struct btrfs_object_id id, uint64_t size,
    struct btrfs_time time)
{
	enum btrfs_result result;
	int done = 0;

	do {
		result = btrfs_transaction_truncate(
		    transaction, id, size, time, BTRFS_RELEASE_STEP_NODES, &done);
	} while (result == BTRFS_OK && !done);
	return result;
}

static enum btrfs_result
evict_all(struct btrfs_transaction *transaction, struct btrfs_object_id id)
{
	enum btrfs_result result;
	int done = 0;

	do {
		result = btrfs_transaction_evict(transaction, id, BTRFS_RELEASE_STEP_NODES, &done);
	} while (result == BTRFS_OK && !done);
	return result;
}

static enum btrfs_result
clean_all_orphans(struct btrfs_transaction *transaction, uint64_t tree, size_t *cleaned)
{
	enum btrfs_result result;
	size_t step;
	int pending = 0;

	*cleaned = 0;
	do {
		result = btrfs_transaction_clean_orphans(
		    transaction, tree, BTRFS_RELEASE_STEP_NODES, &step, &pending);
		*cleaned += step;
	} while (result == BTRFS_OK && pending);
	return result;
}

/* As the native volume does after each operation, the inodes an operation
 * left to eviction steps are evicted at once. */
static enum btrfs_result
evict_deferred(struct btrfs_transaction *transaction)
{
	struct btrfs_object_id id;
	enum btrfs_result result = BTRFS_OK;

	while (
	    result == BTRFS_OK && btrfs_transaction_take_deferred(transaction, &id) == BTRFS_OK) {
		result = evict_all(transaction, id);
	}
	return result;
}

static enum btrfs_result
execute(struct btrfs_transaction *transaction, struct path_table *table,
    const struct operation *operation, struct btrfs_time time)
{
	struct btrfs_new_inode attributes;
	struct btrfs_object_id none = { 0, 0 };
	struct btrfs_object_id parent;
	struct btrfs_object_id target;
	struct btrfs_object_id other;
	struct btrfs_object_id id;
	const char *leaf = NULL;
	const char *new_leaf = NULL;
	struct btrfs_attributes changes;
	struct path_entry *replaced;
	uint64_t tree;
	size_t cleaned;
	int pending;
	int done;
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
		return truncate_all(transaction, path_object(table, operation->path, 0, NULL),
		    operation->offset, time);
	case OPERATION_TRUNCATE_STEP:
		return btrfs_transaction_truncate(transaction,
		    path_object(table, operation->path, 0, NULL), operation->offset, time,
		    operation->size, &done);
	case OPERATION_SET_FSFLAGS:
		return btrfs_transaction_set_fsflags(transaction,
		    path_object(table, operation->path, 0, NULL), (unsigned)operation->flags, time);
	case OPERATION_FALLOCATE:
		return btrfs_transaction_fallocate(transaction,
		    path_object(table, operation->path, 0, NULL), (unsigned)operation->flags,
		    operation->offset, operation->size, time);
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
		replaced = path_slot(table, operation->target);
		/* Between two names of one inode, rename changes nothing. */
		if (result == BTRFS_OK &&
		    !(replaced->path != NULL && replaced->present && replaced->id.tree == id.tree &&
			replaced->id.inode == id.inode)) {
			path_set(table, operation->path, none, 0);
			path_set(table, operation->target, id, 1);
		}
		return result;
	case OPERATION_TMPFILE:
		parent = path_object(table, operation->path, 1, &leaf);
		memset(&attributes, 0, sizeof(attributes));
		attributes.mode = operation->mode;
		attributes.uid = operation->uid;
		attributes.gid = operation->gid;
		attributes.time = time;
		result = btrfs_transaction_create_tmpfile(transaction, parent, &attributes, &id);
		if (result == BTRFS_OK) {
			path_set(table, operation->path, id, 1);
		}
		return result;
	case OPERATION_LINK_TMPFILE:
		id = path_object(table, operation->path, 0, NULL);
		parent = path_object(table, operation->target, 1, &leaf);
		result = btrfs_transaction_link_tmpfile(
		    transaction, id, parent, leaf, strlen(leaf), time);
		if (result == BTRFS_OK) {
			path_set(table, operation->target, id, 1);
		}
		return result;
	case OPERATION_EXCHANGE:
		parent = path_object(table, operation->path, 1, &leaf);
		target = path_object(table, operation->target, 1, &new_leaf);
		result = btrfs_transaction_exchange(transaction, parent, leaf, strlen(leaf), target,
		    new_leaf, strlen(new_leaf), time);
		if (result == BTRFS_OK) {
			id = path_object(table, operation->path, 0, NULL);
			other = path_object(table, operation->target, 0, NULL);
			path_set(table, operation->path, other, 1);
			path_set(table, operation->target, id, 1);
		}
		return result;
	case OPERATION_RENAME_WHITEOUT:
		id = path_object(table, operation->path, 0, NULL);
		parent = path_object(table, operation->path, 1, &leaf);
		target = path_object(table, operation->target, 1, &new_leaf);
		result = btrfs_transaction_rename_whiteout(transaction, parent, leaf, strlen(leaf),
		    target, new_leaf, strlen(new_leaf), operation->uid, operation->gid, time,
		    operation->flags);
		replaced = path_slot(table, operation->target);
		if (result == BTRFS_OK &&
		    !(replaced->path != NULL && replaced->present && replaced->id.tree == id.tree &&
			replaced->id.inode == id.inode)) {
			path_set(table, operation->target, id, 1);
			/* The whiteout's inode is looked up by name when checked. */
			path_set(table, operation->path, none, 0);
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
		return evict_all(transaction, operation->id);
	case OPERATION_CLEAN_ORPHANS:
		result = clean_all_orphans(transaction, operation->id.tree, &cleaned);
		if (result == BTRFS_OK && cleaned != operation->size) {
			fprintf(stderr, "cleaned %zu orphans, expected %zu\n", cleaned,
			    operation->size);
			exit(1);
		}
		return result;
	case OPERATION_SET_ATTRIBUTES:
		memset(&changes, 0, sizeof(changes));
		changes.mask = (unsigned)operation->flags;
		changes.mode = operation->mode;
		changes.uid = operation->uid;
		changes.gid = operation->gid;
		changes.access_time = operation->access_time;
		changes.modify_time = operation->modify_time;
		changes.time = time;
		return btrfs_transaction_set_attributes(
		    transaction, path_object(table, operation->path, 0, NULL), &changes);
	case OPERATION_KEEP_PRIVILEGES:
		return btrfs_transaction_keep_privileges(
		    transaction, path_object(table, operation->path, 0, NULL));
	case OPERATION_DROP_PRIVILEGES:
		return btrfs_transaction_drop_privileges(
		    transaction, path_object(table, operation->path, 0, NULL), time);
	case OPERATION_SUBVOLUME:
		parent = path_object(table, operation->path, 1, &leaf);
		memset(&attributes, 0, sizeof(attributes));
		attributes.mode = operation->mode;
		attributes.uid = operation->uid;
		attributes.gid = operation->gid;
		attributes.time = time;
		result = btrfs_transaction_create_subvolume(
		    transaction, parent, leaf, strlen(leaf), &attributes, operation->data, &tree);
		if (result == BTRFS_OK) {
			path_set(table, operation->path,
			    (struct btrfs_object_id){ tree, BTRFS_ROOT_INODE }, 1);
		}
		return result;
	case OPERATION_DELETE_SUBVOLUME:
		parent = path_object(table, operation->path, 1, &leaf);
		result = btrfs_transaction_delete_subvolume(
		    transaction, parent, leaf, strlen(leaf), time);
		if (result == BTRFS_OK) {
			path_set(table, operation->path, none, 0);
		}
		return result;
	case OPERATION_CLEAN_SUBVOLUMES:
		result = btrfs_transaction_clean_subvolumes(
		    transaction, (size_t)operation->offset, &cleaned, &pending);
		if (result == BTRFS_OK &&
		    (cleaned != operation->size || pending != operation->flags)) {
			fprintf(stderr, "dropped %zu subvolumes (pending %d), expected %zu (%d)\n",
			    cleaned, pending, operation->size, operation->flags);
			exit(1);
		}
		return result;
	case OPERATION_REMOVE_GROUPS:
		result = btrfs_transaction_remove_unused_groups(transaction, &cleaned);
		if (result == BTRFS_OK && cleaned != operation->size) {
			fprintf(stderr, "removed %zu block groups, expected %zu\n", cleaned,
			    operation->size);
			exit(1);
		}
		return result;
	case OPERATION_SYSTEM_GROWTH:
		return system_growth(transaction);
	case OPERATION_SNAPSHOT:
		id = path_object(table, operation->path, 0, NULL);
		parent = path_object(table, operation->target, 1, &leaf);
		result = btrfs_transaction_snapshot(transaction, id.tree, parent, leaf,
		    strlen(leaf), operation->flags, time, operation->data, &tree);
		if (result == BTRFS_OK) {
			path_set(table, operation->target,
			    (struct btrfs_object_id){ tree, BTRFS_ROOT_INODE }, 1);
		}
		return result;
	}
	return BTRFS_INVALID_ARGUMENT;
}

static enum btrfs_result
attempt(struct context *context, const struct plan *plan, size_t commit, enum fault fault,
    size_t point, struct btrfs_allocation_map *map, struct totals *totals)
{
	struct path_table table;
	struct bt_cursor cursor;
	struct namespace_digest pending;
	struct namespace_digest published;
	struct namespace_digest promoted;
	const struct btrfs_fs *reader;
	struct btrfs_fs *fs;
	struct btrfs_fs *after;
	struct btrfs_fs *owned = NULL;
	struct btrfs_transaction *transaction = NULL;
	struct btrfs_time time = { 1700000000 + (int64_t)commit, 123456789 };
	struct device *device = context->device;
	uint64_t allocations;
	uint64_t reads;
	uint64_t digest_allocations;
	uint64_t digest_reads;
	size_t i;
	int compare = 0;
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
	result = btrfs_transaction_begin_mapped(fs, &context->writer, map, &transaction);
	device->coherent = 1;
	for (i = 0; result == BTRFS_OK && i < plan->operation_count[commit]; i++) {
		result = execute(transaction, &table, &plan->operations[commit][i], time);
		if (result == BTRFS_OK && !plan->operations[commit][i].keep_deferred) {
			result = evict_deferred(transaction);
		}
		if (plan->operations[commit][i].expected != BTRFS_OK &&
		    result == plan->operations[commit][i].expected) {
			REQUIRE(transaction->failure == BTRFS_OK);
			result = BTRFS_OK;
		} else if (plan->operations[commit][i].expected != BTRFS_OK && result == BTRFS_OK) {
			fprintf(stderr, "%s commit %zu operation %zu (%s) succeeded, expected %s\n",
			    plan->name, commit, i, plan->operations[commit][i].path,
			    btrfs_result_string(plan->operations[commit][i].expected));
			result = BTRFS_CORRUPT;
		} else if (result != BTRFS_OK && fault == FAULT_NONE) {
			fprintf(stderr, "%s commit %zu operation %zu (%s): %s, expected %s\n",
			    plan->name, commit, i, plan->operations[commit][i].path,
			    btrfs_result_string(result),
			    btrfs_result_string(plan->operations[commit][i].expected));
		}
	}
	/* The transaction's reader view must read what its commit publishes; its
	 * cost stays out of the commit's fault-point counts. */
	if (fault == FAULT_NONE && result == BTRFS_OK) {
		digest_allocations = context->image.allocations;
		digest_reads = context->image.reads;
		reader = btrfs_transaction_reader(transaction);
		REQUIRE(reader != NULL);
		/* Its readers finish before the next operation, so its cursors
		 * read the transaction's own nodes in place. */
		bt_cursor_init(&cursor, reader, reader->selected_tree);
		REQUIRE(cursor.borrow);
		bt_cursor_fini(&cursor);
		if (namespace_digest(reader, &pending) != 0) {
			fprintf(stderr, "%s commit %zu: transaction view: %s\n", plan->name, commit,
			    pending.failure);
			REQUIRE(0);
		}
		allocations += context->image.allocations - digest_allocations;
		reads += context->image.reads - digest_reads;
		compare = 1;
	}
	device->before_commit = device->count;
	if (result == BTRFS_OK) {
		result = map == NULL
		    ? btrfs_transaction_commit(transaction)
		    : btrfs_transaction_commit_view(transaction, BTRFS_TOP_LEVEL_TREE, &owned);
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
	device->coherent = 0;
	btrfs_unmount(fs);
	if (compare && result == BTRFS_OK) {
		REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &after) == BTRFS_OK);
		if (namespace_digest(after, &published) != 0) {
			fprintf(stderr, "%s commit %zu: published view: %s\n", plan->name, commit,
			    published.failure);
			REQUIRE(0);
		}
		if (owned != NULL) {
			/* The returned view survives both transaction and base destruction.
			 * Compare with an independent mount, including chunk growth/removal
			 * and feature flags changed by compression, then read its namespace. */
			REQUIRE(memcmp(&owned->info, &after->info, sizeof(after->info)) == 0);
			REQUIRE(owned->chunk_count == after->chunk_count &&
			    memcmp(owned->chunks, after->chunks,
				owned->chunk_count * sizeof(*owned->chunks)) == 0);
			REQUIRE(namespace_digest(owned, &promoted) == 0);
			REQUIRE(promoted.hash == published.hash &&
			    promoted.objects == published.objects &&
			    promoted.bytes == published.bytes);
		}
		btrfs_unmount(after);
		if (published.hash != pending.hash || published.objects != pending.objects ||
		    published.bytes != pending.bytes) {
			fprintf(stderr,
			    "%s commit %zu: transaction view read %zu objects, %llu bytes; "
			    "published view %zu objects, %llu bytes\n",
			    plan->name, commit, pending.objects, (unsigned long long)pending.bytes,
			    published.objects, (unsigned long long)published.bytes);
			REQUIRE(0);
		}
		context->reader_digests++;
	}
	btrfs_unmount(owned);
	path_table_release(&table);
	REQUIRE(context->image.live_allocations == 0);
	return result;
}

/* A commit's writes before its first barrier, from first on, are its node
 * copies in ascending physical order, contiguous ones merged up to
 * BT_WRITE_RUN bytes. */
static void
require_merged_writes(struct context *context, size_t first, size_t last)
{
	const struct saved_write *write;
	const struct saved_write *previous = NULL;
	size_t run = BT_WRITE_RUN / context->node_size * context->node_size;
	size_t end;
	size_t i;

	for (i = first; i < last && context->device->writes[i].epoch == 0; i++) {
		write = &context->device->writes[i];
		REQUIRE(write->length % context->node_size == 0 && write->length <= run);
		if (previous != NULL) {
			end = previous->offset + previous->length;
			REQUIRE(write->offset >= end &&
			    (write->offset != end || previous->length == run));
		}
		previous = write;
	}
	REQUIRE(previous != NULL);
}

/* Writes [first, last) of the device are new data: they touch no superblock
 * copy and no metadata or system chunk (a data chunk grown in the transaction
 * lies outside every committed chunk). */
void
require_data_writes(struct context *context, size_t first, size_t last)
{
	const struct saved_write *write;
	struct btrfs_fs *fs;
	uint64_t physical;
	size_t i;
	size_t c;
	unsigned mirror;
	unsigned mirrors;
	int metadata;

	if (first == last) {
		return;
	}
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	for (i = first; i < last; i++) {
		write = &context->device->writes[i];
		metadata = 0;
		for (mirror = 0; mirror < BT_SUPER_MIRRORS; mirror++) {
			physical = bt_super_offset(mirror);
			metadata |= write->offset < physical + BT_SUPER_SIZE &&
			    physical < write->offset + write->length;
		}
		for (c = 0; !metadata && c < fs->chunk_count; c++) {
			if ((fs->chunks[c].type & BT_BLOCK_DATA) != 0) {
				continue;
			}
			mirrors = 1;
			for (mirror = 0; !metadata && mirror < mirrors; mirror++) {
				REQUIRE(bt_map(fs, fs->chunks[c].logical, 1, fs->chunks[c].type,
					    mirror, &physical, &mirrors) == BTRFS_OK);
				metadata = write->offset < physical + fs->chunks[c].length &&
				    physical < write->offset + write->length;
			}
		}
		if (metadata) {
			fprintf(stderr, "write %zu at %llu before the commit is not new data\n", i,
			    (unsigned long long)write->offset);
			exit(1);
		}
	}
	btrfs_unmount(fs);
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
			result = attempt(context, plan, commit, fault, point, NULL, &ignored);
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
			/* A failed allocation or read stops before the commit's first
			 * write; only new data reached unreferenced space. */
			if (fault == FAULT_ALLOCATE ||
			    (fault == FAULT_READ && result != BTRFS_OK)) {
				REQUIRE(device->count == device->before_commit);
				require_data_writes(context, first, device->count);
			}
			resolve(context, plan, commit - 1, commit, &outcome);
			REQUIRE(result == BTRFS_OK || device->count != device->before_commit ||
			    outcome.resolved == commit - 1);
			outcome_release(&outcome);
			truncate_writes(device, first);
		}
		REQUIRE(failures[fault] != 0);
	}
}

size_t
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
void
audit_state(struct context *context, const char *name)
{
	struct reference_audit audit;
	struct namespace_audit names;
	struct qgroup_audit qgroups;
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
	if (qgroup_audit(fs, &qgroups) != 0) {
		fprintf(stderr, "%s: qgroup audit: %s\n", name, qgroups.failure);
		exit(1);
	}
	context->qgroup_audits += qgroups.quotas && !qgroups.skipped;
	btrfs_unmount(fs);
	REQUIRE(context->image.live_allocations == 0);
	printf("%s references: %zu blocks, tree %zu, shared block %zu, data %zu, shared data %zu, "
	       "keyed %zu, full-backref blocks %zu\n",
	    name, audit.blocks, audit.tree_refs, audit.shared_block_refs, audit.data_refs,
	    audit.shared_data_refs, audit.keyed_refs, audit.full_backref_blocks);
	context->audits++;
}

/* The kept map lives across transactions, outside the per-transaction
 * allocation accounting of the image. */
static void *
map_allocate(void *context, size_t size)
{
	(void)context;
	return malloc(size);
}

static void
map_release(void *context, void *allocation, size_t size)
{
	(void)context;
	(void)size;
	free(allocation);
}

/* Whether the map equals what a fresh, verified load of the current committed
 * state produces. */
static void
check_map(struct context *context, const struct plan *plan, size_t commit,
    const struct btrfs_allocation_map *map)
{
	struct btrfs_fs *fs;
	struct bt_space *space;
	struct bt_root extents;
	struct bt_root devices;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(bt_find_root(fs, BT_EXTENT_TREE, &extents) == BTRFS_OK);
	REQUIRE(bt_find_root(fs, BT_DEV_TREE, &devices) == BTRFS_OK);
	REQUIRE(bt_space_create(fs, extents, NULL, TRANSACTION_NODE_LIMIT, &space) == BTRFS_OK);
	REQUIRE(bt_space_devices(space, fs->chunk_tree, devices) == BTRFS_OK);
	if (bt_space_map_check(map, space) != BTRFS_OK) {
		fprintf(stderr,
		    "%s commit %zu: the kept allocation map differs from a fresh load\n",
		    plan->name, commit);
		exit(1);
	}
	bt_space_destroy(space);
	btrfs_unmount(fs);
}

/* Replays the plan's commits from its base with one allocation map kept
 * across them, as a mounted volume does: every commit must issue exactly the
 * writes of the recorded run, which loaded and verified the allocator state
 * at each begin, and leave the map equal to a fresh load. */
static void
map_replay(struct context *context, const struct plan *plan, const size_t *starts)
{
	struct btrfs_allocation_map *map;
	struct btrfs_environment environment = context->env;
	struct saved_write *recorded;
	struct device *device = context->device;
	struct totals totals;
	size_t end = device->count;
	size_t count = end - starts[1];
	size_t commit;
	size_t i;
	uint64_t scans;
	uint64_t reuses;

	recorded = calloc(count == 0 ? 1 : count, sizeof(*recorded));
	REQUIRE(recorded != NULL);
	for (i = 0; i < count; i++) {
		recorded[i] = device->writes[starts[1] + i];
		recorded[i].bytes = malloc(recorded[i].length);
		REQUIRE(recorded[i].bytes != NULL);
		memcpy(recorded[i].bytes, device->writes[starts[1] + i].bytes, recorded[i].length);
	}
	truncate_writes(device, starts[1]);
	environment.allocate = map_allocate;
	environment.release = map_release;
	REQUIRE(btrfs_allocation_map_create(&environment, &map) == BTRFS_OK);
	for (commit = 1; commit <= plan->commits; commit++) {
		REQUIRE(attempt(context, plan, commit, FAULT_NONE, 0, map, &totals) == BTRFS_OK);
		REQUIRE(device->count == (commit < plan->commits ? starts[commit + 1] : end));
		for (i = starts[commit]; i < device->count; i++) {
			if (device->writes[i].offset != recorded[i - starts[1]].offset ||
			    device->writes[i].length != recorded[i - starts[1]].length ||
			    memcmp(device->writes[i].bytes, recorded[i - starts[1]].bytes,
				device->writes[i].length) != 0) {
				fprintf(stderr,
				    "%s commit %zu: write %zu differs with the allocation map "
				    "kept\n",
				    plan->name, commit, i - starts[commit]);
				exit(1);
			}
		}
		check_map(context, plan, commit, map);
	}
	btrfs_allocation_map_counts(map, &scans, &reuses);
	REQUIRE(scans == 1 && reuses == plan->commits - 1);
	btrfs_allocation_map_destroy(map);
	for (i = 0; i < count; i++) {
		free(recorded[i].bytes);
	}
	free(recorded);
	REQUIRE(context->image.live_allocations == 0);
}

void
run_plan(struct context *context, struct plan *plan)
{
	struct exporter exporter;
	struct totals totals;
	struct btrfs_fs *fs;
	struct device *device = context->device;
	size_t starts[MAX_STAGES + 1];
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
		starts[commit] = first;
		result = attempt(context, plan, commit, FAULT_NONE, 0, NULL, &totals);
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
		require_merged_writes(context, device->before_commit, device->count);
		REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
		resolve_bounds(fs, plan, commit);
		btrfs_unmount(fs);
		export_writes(context, &exporter);
		audit_state(context, plan->name);
		if (commit == 1 && plan->new_chunks != 0) {
			check_growth(context, plan);
		}
		if (plan->quick) {
			/* Without crash states, check each committed stage directly. */
			REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
			check_stage(context, fs, plan, commit);
			btrfs_unmount(fs);
			continue;
		}
		crash_states(context, plan, &exporter, commit, first);
		if (commit == plan->commits) {
			truncate_writes(device, first);
			fault_sweeps(context, plan, commit, first, &totals);
			REQUIRE(attempt(context, plan, commit, FAULT_NONE, 0, NULL, &totals) ==
			    BTRFS_OK);
		}
	}
	map_replay(context, plan, starts);
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	check_stage(context, fs, plan, plan->commits);
	btrfs_unmount(fs);
	export_stages(context, plan, &exporter);
	export_end(&exporter);
	printf("%s: %zu crash states, %zu explicit recoveries PASS\n", plan->name,
	    context->states - states, context->recoveries - recoveries);
	truncate_writes(device, 0);
	plan_destroy(plan);
}
