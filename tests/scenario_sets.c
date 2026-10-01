/* SPDX-License-Identifier: BSD-3-Clause */
/* Inline, shared-reference, keyed-reference, data, free-space and growth
 * scenarios. */
#define _POSIX_C_SOURCE 200809L
#include "scenario.h"

void
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
void
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
void
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
void
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
void
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
void
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
