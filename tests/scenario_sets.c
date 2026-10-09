/* SPDX-License-Identifier: BSD-3-Clause */
/* Inline, shared-reference, keyed-reference, data, free-space and growth
 * scenarios. */
#define _POSIX_C_SOURCE 200809L
#include "scenario.h"

#define STREAM_BYTES (UINT32_C(80) * 1024 * 1024)
#define COMPRESSION_PROPERTY "btrfs.compression"
#define COMPRESS_TEXT_BYTES (300U * 1024U)
#define COMPRESS_NOISE_BYTES (200U * 1024U)
#define COMPRESS_SMALL_BYTES 3000U
#define COMPRESS_TINY_BYTES 1000U
#define COMPRESS_PLAIN_BYTES 1500U
#define COMPRESS_PATCH_OFFSET 140000U
#define COMPRESS_PATCH_BYTES 8192U
#define COMPRESS_TRUNCATE_SIZE 200000U
#define COMPRESS_MOUNT_BYTES (150U * 1024U)
#define STREAM_PIECE (1024U * 1024U)
#define STREAM_MEMORY_BOUND (UINT64_C(16) * 1024 * 1024)

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

/* Shrinking and deleting a file of many extents in bounded steps, as Linux's
 * truncation does from the end of the file: the committed state after one
 * step is a valid shorter file with the original bytes, and an unlink leaves
 * the rest of a deletion to eviction under an orphan item, which a later
 * transaction's cleanup completes. The snapshots keep their copies. */
static void
release_plans(struct context *context)
{
	struct btrfs_object_id data;
	struct btrfs_fs *fs;
	struct plan plan;
	uint8_t *piece;
	size_t i;

	piece = malloc(RELEASE_PIECE_BYTES);
	REQUIRE(piece != NULL);
	plan_init(&plan);
	plan.name = "release-truncate";
	track_data(context, &plan, "big");
	for (i = 0; i < RELEASE_PIECES; i++) {
		fill_pattern(piece, RELEASE_PIECE_BYTES, (unsigned)i);
		plan_write(context, &plan, 1, "/data/big", (uint64_t)i * RELEASE_PIECE_BYTES, piece,
		    RELEASE_PIECE_BYTES);
	}
	plan_truncate_step(
	    context, &plan, 2, "/data/big", RELEASE_TARGET_BYTES, BTRFS_RELEASE_STEP_NODES);
	plan_truncate(context, &plan, 3, "/data/big", RELEASE_TARGET_BYTES);
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "release-unlink";
	track_data(context, &plan, "small");
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	data = object(fs, "/data");
	btrfs_unmount(fs);
	for (i = 0; i < RELEASE_PIECES; i++) {
		fill_pattern(piece, RELEASE_PIECE_BYTES, (unsigned)i);
		plan_write_new(&plan, 1, "/data/big", (uint64_t)i * RELEASE_PIECE_BYTES, piece,
		    RELEASE_PIECE_BYTES);
	}
	plan_unlink_deferred(&plan, 2, "/data/big");
	plan_clean(&plan, 3, data.tree, 1);
	expect(&plan, 2, LAST_STAGE, EXPECT_ABSENT, "/data/big");
	expect_current(context, &plan, 0, LAST_STAGE, "/data-snap/big", "/data-snap/big");
	run_plan(context, &plan);
	free(piece);
}

/* File data written as new extents: unaligned edges are rewritten from the
 * transaction's view, old extents are trimmed, moved or split, and snapshots,
 * reflinks, preallocation, compression and checksum policy are preserved. */
/* One transaction writes more than the former 64 MiB staging bound: new data
 * reaches the device as its extents are created, so the transaction's live
 * memory stays bounded; after the commit the file reads back exactly and both
 * audits pass. */
static void
data_stream_test(struct context *context)
{
	struct btrfs_new_inode attributes;
	struct btrfs_object_id id;
	struct btrfs_inode inode;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct btrfs_time time = { 1800000000, 0 };
	uint8_t *data;
	uint8_t *back;
	uint64_t before;
	uint64_t held;
	size_t completed;
	size_t offset;

	data = malloc(STREAM_BYTES);
	back = malloc(STREAM_PIECE);
	REQUIRE(data != NULL && back != NULL);
	fill_pattern(data, STREAM_BYTES, 77);
	memset(&attributes, 0, sizeof(attributes));
	attributes.mode = BTRFS_MODE_REGULAR | 0644;
	attributes.time = time;
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_create(
		    transaction, object(fs, "/data"), "stream", 6, &attributes, &id) == BTRFS_OK);
	before = context->image.live_bytes;
	REQUIRE(btrfs_transaction_write(transaction, id, 0, data, STREAM_BYTES, time) == BTRFS_OK);
	held = context->image.live_bytes - before;
	REQUIRE(held < STREAM_MEMORY_BOUND);
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	audit_state(context, "data-stream");
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/data/stream", &inode) == BTRFS_OK);
	REQUIRE(inode.size == STREAM_BYTES);
	for (offset = 0; offset < STREAM_BYTES; offset += STREAM_PIECE) {
		REQUIRE(btrfs_read(fs, &inode, offset, back, STREAM_PIECE, &completed) == BTRFS_OK);
		REQUIRE(
		    completed == STREAM_PIECE && memcmp(back, data + offset, STREAM_PIECE) == 0);
	}
	btrfs_unmount(fs);
	truncate_writes(context->device, 0);
	REQUIRE(context->image.live_allocations == 0);
	free(data);
	free(back);
	printf("data-stream: %u MiB in one transaction with %llu KiB held PASS\n",
	    (unsigned)(STREAM_BYTES >> 20), (unsigned long long)(held >> 10));
}

/* Compression on write, as Linux's compress_file_range: a file with the zlib
 * property is written in 128 KiB compressed extents; incompressible data stays
 * uncompressed; a small file is one inline extent, compressed when that
 * shrinks it; overwriting and truncating compressed extents keep the parts
 * still referenced. */
static void
data_compress_plan(struct context *context)
{
	static uint8_t text[COMPRESS_TEXT_BYTES];
	static uint8_t noise[COMPRESS_NOISE_BYTES];
	static uint8_t small[COMPRESS_SMALL_BYTES];
	static uint8_t tiny[COMPRESS_TINY_BYTES];
	static uint8_t plain[COMPRESS_PLAIN_BYTES];
	static uint8_t patch[COMPRESS_PATCH_BYTES];
	static uint8_t grown[2 * COMPRESS_SMALL_BYTES];
	struct plan plan;

	fill_text(text, sizeof(text), 1);
	fill_random(noise, sizeof(noise), 2);
	fill_text(small, sizeof(small), 3);
	fill_random(tiny, sizeof(tiny), 4);
	fill_text(plain, sizeof(plain), 5);
	fill_text(patch, sizeof(patch), 6);
	fill_text(grown, sizeof(grown), 7);
	memcpy(grown, small, sizeof(small));
	plan_init(&plan);
	plan.name = "data-compress";
	track_data(context, &plan, "zlib");
	plan_write(context, &plan, 1, "/data/zlib", 0, text, sizeof(text));
	plan_create(&plan, 1, "/data/noise", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_set_xattr(&plan, 1, "/data/noise", COMPRESSION_PROPERTY, "zlib", 4, 0);
	plan_write_new(&plan, 1, "/data/noise", 0, noise, sizeof(noise));
	plan_create(&plan, 1, "/data/zsmall", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_set_xattr(&plan, 1, "/data/zsmall", COMPRESSION_PROPERTY, "zlib", 4, 0);
	plan_write_new(&plan, 1, "/data/zsmall", 0, small, sizeof(small));
	plan_create(&plan, 1, "/data/ztiny", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_set_xattr(&plan, 1, "/data/ztiny", COMPRESSION_PROPERTY, "zlib", 4, 0);
	plan_write_new(&plan, 1, "/data/ztiny", 0, tiny, sizeof(tiny));
	plan_create(&plan, 1, "/data/plain-small", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/data/plain-small", 0, plain, sizeof(plain));
	plan_write(context, &plan, 2, "/data/zlib", COMPRESS_PATCH_OFFSET, patch, sizeof(patch));
	plan_write_new(
	    &plan, 2, "/data/zsmall", sizeof(small), grown + sizeof(small), sizeof(small));
	plan_truncate(context, &plan, 3, "/data/zlib", COMPRESS_TRUNCATE_SIZE);

	/* 128 KiB, 128 KiB and the rest, each compressed. */
	expect_compressed(&plan, 1, 1, "/data/zlib", BTRFS_COMPRESSION_ZLIB, 3, 0);
	/* The patch splits the second extent around a new compressed one; with
	 * 16 KiB sectors its range starts with that extent, which keeps only
	 * its tail. */
	expect_compressed(&plan, 2, 2, "/data/zlib", BTRFS_COMPRESSION_ZLIB,
	    context->sector_size == 4096 ? 5 : 4, 0);
	/* The truncated tail drops the third extent; the new EOF sector, one
	 * sector after the file's start, is never compressed. */
	expect_compressed(&plan, 3, LAST_STAGE, "/data/zlib", BTRFS_COMPRESSION_ZLIB,
	    context->sector_size == 4096 ? 4 : 3, 0);
	expect_compressed(&plan, 1, LAST_STAGE, "/data/noise", BTRFS_COMPRESSION_ZLIB, 0, 0);
	expect_file(&plan, 1, LAST_STAGE, "/data/noise", noise, sizeof(noise));
	expect_compressed(&plan, 1, 1, "/data/zsmall", BTRFS_COMPRESSION_ZLIB, 0, 1);
	expect_file(&plan, 1, 1, "/data/zsmall", small, sizeof(small));
	/* Grown past one 4 KiB sector, the inline extent becomes a compressed
	 * one; within one 16 KiB sector it stays inline, as Linux keeps data
	 * inline that fits a sector and compresses below max_inline. */
	if (context->sector_size == 4096) {
		expect_compressed(
		    &plan, 2, LAST_STAGE, "/data/zsmall", BTRFS_COMPRESSION_ZLIB, 1, 0);
	} else {
		expect_compressed(
		    &plan, 2, LAST_STAGE, "/data/zsmall", BTRFS_COMPRESSION_ZLIB, 0, 1);
	}
	expect_file(&plan, 2, LAST_STAGE, "/data/zsmall", grown, sizeof(grown));
	expect_compressed(&plan, 1, LAST_STAGE, "/data/ztiny", BTRFS_COMPRESSION_ZLIB, 0, 0);
	expect_file(&plan, 1, LAST_STAGE, "/data/ztiny", tiny, sizeof(tiny));
	expect_compressed(&plan, 1, LAST_STAGE, "/data/plain-small", BTRFS_COMPRESSION_ZLIB, 0, 0);
	expect_file(&plan, 1, LAST_STAGE, "/data/plain-small", plain, sizeof(plain));
	run_plan(context, &plan);
}

/* The compress mount option: plain files compress with its codec, which
 * records the ZSTD incompat feature; a file whose property says "no" and a
 * NODATASUM file stay uncompressed. */
static void
data_compress_mount_plan(struct context *context)
{
	static uint8_t text[COMPRESS_MOUNT_BYTES];
	struct plan plan;

	fill_text(text, sizeof(text), 8);
	plan_init(&plan);
	plan.name = "data-compress-mount";
	(void)plan_file(context, &plan, "/data/small");
	plan_create(&plan, 1, "/data/mounted", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/data/mounted", 0, text, sizeof(text));
	plan_create(&plan, 1, "/data/refuses", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_set_xattr(&plan, 1, "/data/refuses", COMPRESSION_PROPERTY, "no", 2, 0);
	plan_write_new(&plan, 1, "/data/refuses", 0, text, sizeof(text));
	plan_write_new(&plan, 1, "/data/nodatasum", 0, text, sizeof(text));
	expect_compressed(&plan, 1, LAST_STAGE, "/data/mounted", BTRFS_COMPRESSION_ZSTD, 2, 0);
	expect_file(&plan, 1, LAST_STAGE, "/data/mounted", text, sizeof(text));
	expect_compressed(&plan, 1, LAST_STAGE, "/data/refuses", BTRFS_COMPRESSION_ZSTD, 0, 0);
	expect_file(&plan, 1, LAST_STAGE, "/data/refuses", text, sizeof(text));
	expect_compressed(&plan, 1, LAST_STAGE, "/data/nodatasum", BTRFS_COMPRESSION_ZSTD, 0, 0);
	expect_value(&plan, 1, LAST_STAGE, EXPECT_FEATURE, "/", BT_FEATURE_COMPRESS_ZSTD);
	context->writer.compression = BTRFS_COMPRESSION_ZSTD;
	run_plan(context, &plan);
	context->writer.compression = BTRFS_COMPRESSION_NONE;
}

/* fallocate as Linux's btrfs_fallocate makes it on a NO_HOLES filesystem: an
 * unaligned edge rewrites its sector, the rest of a punched range loses its
 * extents, zeroed and preallocated ranges become unwritten extents, and a
 * later write into one is made in place. */
static void
fallocate_plans(struct context *context)
{
	static uint8_t data[FALLOCATE_WRITE_BYTES];
	const unsigned punch = BTRFS_FALLOCATE_PUNCH_HOLE | BTRFS_FALLOCATE_KEEP_SIZE;
	struct plan plan;

	fill_pattern(data, sizeof(data), 61);
	plan_init(&plan);
	plan.name = "fallocate-punch";
	track_data(context, &plan, "big");
	plan_fallocate(
	    context, &plan, 1, "/data/big", punch, FALLOCATE_PUNCH_OFFSET, FALLOCATE_PUNCH_BYTES);
	/* Punching needs KEEP_SIZE and excludes zeroing; other modes and an
	 * empty range are refused before any change. */
	plan_fallocate(
	    context, &plan, 1, "/data/big", BTRFS_FALLOCATE_PUNCH_HOLE, 0, FALLOCATE_GROW_BYTES);
	plan_expect_refusal(&plan, 1, BTRFS_UNSUPPORTED);
	plan_fallocate(context, &plan, 1, "/data/big", punch | BTRFS_FALLOCATE_ZERO_RANGE, 0,
	    FALLOCATE_GROW_BYTES);
	plan_expect_refusal(&plan, 1, BTRFS_UNSUPPORTED);
	plan_fallocate(context, &plan, 1, "/data/big", 0, 0, 0);
	plan_expect_refusal(&plan, 1, BTRFS_INVALID_ARGUMENT);
	/* Past EOF only the EOF sector is zeroed and the extents to EOF go. */
	plan_fallocate(
	    context, &plan, 2, "/data/big", punch, FALLOCATE_TAIL_OFFSET, FALLOCATE_TAIL_BYTES);
	/* A range that is all hole changes nothing. */
	plan_fallocate(context, &plan, 2, "/data/big", punch,
	    FALLOCATE_PUNCH_OFFSET + FALLOCATE_GROW_BYTES, FALLOCATE_GROW_BYTES);
	expect_extents(&plan, 1, 1, "/data/big", 4, 0, 3);
	expect_extents(&plan, 2, LAST_STAGE, "/data/big", 5, 0, 4);
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "fallocate-zero";
	track_data(context, &plan, "big");
	plan_fallocate(context, &plan, 1, "/data/big", BTRFS_FALLOCATE_ZERO_RANGE,
	    FALLOCATE_ZERO_OFFSET, FALLOCATE_ZERO_BYTES);
	plan_fallocate(context, &plan, 1, "/data/big",
	    BTRFS_FALLOCATE_ZERO_RANGE | BTRFS_FALLOCATE_KEEP_SIZE,
	    FALLOCATE_BIG_BYTES + FALLOCATE_BEYOND_GAP, FALLOCATE_BEYOND_BYTES);
	/* btrfs_zero_range rewrites the partial sectors at both ends; with 16 KiB
	 * sectors the first one starts the file, so no piece of the original
	 * extent stays before it. */
	expect_extents(
	    &plan, 1, LAST_STAGE, "/data/big", context->sector_size == 4096 ? 4 : 3, 2, 5);
	expect_flags(&plan, 1, LAST_STAGE, "/data/big", BT_INODE_PREALLOC, BT_INODE_PREALLOC);
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "fallocate-keep-size";
	track_data(context, &plan, "small");
	plan_fallocate(context, &plan, 1, "/data/small", BTRFS_FALLOCATE_KEEP_SIZE,
	    FALLOCATE_SMALL_BYTES, FALLOCATE_KEEP_BYTES);
	plan_write(context, &plan, 2, "/data/small", FALLOCATE_INSIDE_OFFSET, data, sizeof(data));
	/* Linux rewrites the sector holding EOF before it preallocates past it:
	 * one 16 KiB sector holds all of /data/small, so nothing of its extent
	 * stays, and the write fills one whole preallocated sector. */
	if (context->sector_size == 4096) {
		expect_extents(&plan, 1, 1, "/data/small", 2, 1, 3);
		expect_extents(&plan, 2, LAST_STAGE, "/data/small", 3, 2, 3);
	} else {
		expect_extents(&plan, 1, 1, "/data/small", 1, 1, 2);
		expect_extents(&plan, 2, LAST_STAGE, "/data/small", 2, 1, 2);
	}
	expect_flags(&plan, 1, LAST_STAGE, "/data/small", BT_INODE_PREALLOC, BT_INODE_PREALLOC);
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "fallocate-grow";
	track_data(context, &plan, "small");
	plan_fallocate(
	    context, &plan, 1, "/data/small", 0, FALLOCATE_GROW_OFFSET, FALLOCATE_GROW_BYTES);
	plan_write(context, &plan, 2, "/data/small", FALLOCATE_WRITE_OFFSET, data, sizeof(data));
	/* With 16 KiB sectors the preallocation is one sector, which the write
	 * turns into data. */
	if (context->sector_size == 4096) {
		expect_extents(&plan, 1, 1, "/data/small", 2, 1, 3);
		expect_extents(&plan, 2, LAST_STAGE, "/data/small", 3, 1, 3);
	} else {
		expect_extents(&plan, 1, 1, "/data/small", 1, 1, 2);
		expect_extents(&plan, 2, LAST_STAGE, "/data/small", 2, 0, 2);
	}
	run_plan(context, &plan);
}

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

	fallocate_plans(context);
	data_stream_test(context);
	data_compress_plan(context);
	data_compress_mount_plan(context);
	release_plans(context);
}

/* A filesystem without NO_HOLES and with DUP data. Growing a file by
 * truncation or by a write past EOF covers the new range with hole items as
 * btrfs_cont_expand does; an inline file converts its own sector before a
 * distant write; writes into Linux's hole items split them and truncation
 * trims them; every data write reaches both copies, in place included. */
void
holes_scenarios(struct context *context)
{
	static uint8_t data[HOLES_WRITE_BYTES];
	static uint8_t first[NOCOW_FILE_BYTES];
	static uint8_t second[NOCOW_FILE_BYTES];
	struct plan plan;

	fill_pattern(data, sizeof(data), 41);
	plan_init(&plan);
	plan.name = "holes-grow";
	track_data(context, &plan, "small");
	track_data(context, &plan, "inline");
	track_data(context, &plan, "big");
	plan_truncate(context, &plan, 1, "/data/small", HOLES_SMALL_SIZE);
	plan_write(
	    context, &plan, 1, "/data/inline", HOLES_INLINE_OFFSET, data, HOLES_INLINE_BYTES);
	plan_write(context, &plan, 2, "/data/small", HOLES_SMALL_PATCH, data, 4096);
	plan_truncate(context, &plan, 2, "/data/inline", HOLES_INLINE_SIZE);
	plan_write(context, &plan, 3, "/data/big", HOLES_BIG_OFFSET, data, 4096);
	expect_holes(&plan, 0, 0, "/data/small", 0);
	/* The EOF sector is rewritten, then one hole covers the growth. */
	expect_holes(&plan, 1, 1, "/data/small", 1);
	expect_extents(&plan, 1, 1, "/data/small", 2, 0, 2);
	/* A write inside the hole splits it. */
	expect_holes(&plan, 2, LAST_STAGE, "/data/small", 2);
	expect_extents(&plan, 2, LAST_STAGE, "/data/small", 3, 0, 3);
	/* The inline sector becomes its own extent, then a hole, then the data. */
	expect_holes(&plan, 1, 1, "/data/inline", 1);
	expect_extents(&plan, 1, 1, "/data/inline", 2, 0, 2);
	/* Truncation inside the hole: its EOF sector holds zeros as data. */
	expect_holes(&plan, 2, LAST_STAGE, "/data/inline", 1);
	expect_extents(&plan, 2, LAST_STAGE, "/data/inline", 2, 0, 2);
	expect_holes(&plan, 2, 2, "/data/big", 0);
	expect_holes(&plan, 3, LAST_STAGE, "/data/big", 1);
	run_plan(context, &plan);

	plan_init(&plan);
	plan.name = "holes-linux";
	track_data(context, &plan, "sparse");
	(void)plan_file(context, &plan, "/data/grown");
	plan_write(context, &plan, 1, "/data/sparse", HOLES_SPARSE_PATCH, data, 4096);
	plan_truncate(context, &plan, 1, "/data/grown", HOLES_GROWN_SHRUNK);
	plan_truncate(context, &plan, 2, "/data/sparse", HOLES_SPARSE_SIZE);
	plan_truncate(context, &plan, 2, "/data/grown", HOLES_GROWN_SIZE);
	plan_write_new(&plan, 3, "/huge", HOLES_HUGE_OFFSET, data, 4096);
	/* Linux's holes around its two data sectors. */
	expect_holes(&plan, 0, 0, "/data/sparse", 3);
	expect_holes(&plan, 1, 1, "/data/sparse", 4);
	/* The new EOF sector takes the head of the last hole; the rest goes. */
	expect_holes(&plan, 2, LAST_STAGE, "/data/sparse", 3);
	expect_extents(&plan, 2, LAST_STAGE, "/data/sparse", 4, 0, 4);
	expect_holes(&plan, 0, 1, "/data/grown", 1);
	expect_extents(&plan, 1, 1, "/data/grown", 1, 0, 1);
	expect_holes(&plan, 2, LAST_STAGE, "/data/grown", 2);
	expect_extents(&plan, 2, LAST_STAGE, "/data/grown", 1, 0, 1);
	/* One sector written into the middle of a 16 GiB hole item. */
	expect_holes(&plan, 0, 2, "/huge", 1);
	expect_holes(&plan, 3, LAST_STAGE, "/huge", 2);
	expect_extents(&plan, 3, LAST_STAGE, "/huge", 1, 0, 1);
	run_plan(context, &plan);

	/* Without NO_HOLES a punch below EOF leaves a hole item, growth by
	 * fallocate first covers the gap with one, and zeroing inside a hole
	 * item splits it around the unwritten extent. */
	plan_init(&plan);
	plan.name = "holes-fallocate";
	track_data(context, &plan, "big");
	track_data(context, &plan, "small");
	plan_fallocate(context, &plan, 1, "/data/big",
	    BTRFS_FALLOCATE_PUNCH_HOLE | BTRFS_FALLOCATE_KEEP_SIZE, FALLOCATE_PUNCH_OFFSET,
	    FALLOCATE_PUNCH_BYTES);
	plan_fallocate(
	    context, &plan, 1, "/data/small", 0, FALLOCATE_GROW_OFFSET, FALLOCATE_GROW_BYTES);
	plan_fallocate(context, &plan, 2, "/data/small", BTRFS_FALLOCATE_ZERO_RANGE,
	    FALLOCATE_INSIDE_OFFSET, FALLOCATE_INSIDE_BYTES);
	expect_holes(&plan, 1, LAST_STAGE, "/data/big", 1);
	expect_extents(&plan, 1, LAST_STAGE, "/data/big", 4, 0, 3);
	expect_holes(&plan, 1, 1, "/data/small", 1);
	expect_extents(&plan, 1, 1, "/data/small", 2, 1, 3);
	expect_holes(&plan, 2, LAST_STAGE, "/data/small", 2);
	expect_extents(&plan, 2, LAST_STAGE, "/data/small", 2, 2, 4);
	run_plan(context, &plan);

	/* In place on DUP data: Linux's preallocated file and a new NODATACOW
	 * file, whose overwrite may be torn on either copy. */
	fill_random(first, sizeof(first), 42);
	memcpy(second, first, sizeof(second));
	fill_random(second + NOCOW_PATCH_OFFSET, NOCOW_PATCH_BYTES, 43);
	plan_init(&plan);
	plan.name = "holes-in-place";
	(void)plan_file(context, &plan, "/preallocated");
	plan_write(context, &plan, 1, "/preallocated", NOCOW_PREALLOC_FIRST, data,
	    NOCOW_PREALLOC_FIRST_BYTES);
	plan_create(&plan, 1, "/data/nocow/file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/data/nocow/file", 0, first, sizeof(first));
	plan_write_new(&plan, 2, "/data/nocow/file", NOCOW_PATCH_OFFSET,
	    second + NOCOW_PATCH_OFFSET, NOCOW_PATCH_BYTES);
	plan_volatile(&plan, 2, "/data/nocow/file", NOCOW_PATCH_OFFSET, NOCOW_PATCH_BYTES);
	expect_extents(&plan, 1, LAST_STAGE, "/preallocated", 1, 2, 1);
	expect_file(&plan, 1, 1, "/data/nocow/file", first, sizeof(first));
	expect_file(&plan, 2, LAST_STAGE, "/data/nocow/file", second, sizeof(second));
	expect_flags(
	    &plan, 1, LAST_STAGE, "/data/nocow/file", BT_INODE_NODATACOW, BT_INODE_NODATACOW);
	expect_extents(&plan, 1, LAST_STAGE, "/data/nocow/file", 1, 0, 1);
	run_plan(context, &plan);
}

/* Both conversions of one 112 MiB data block group (thresholds 157 and 57,
 * which Linux itself crossed at 158 and 56 when it wrote the fixture). A
 * filler takes the free space before the group's tail, so new one-sector
 * files lie side by side there; removing every other one pushes the count
 * past the high threshold and the group becomes bitmaps; removing the rest
 * merges the runs below the low threshold and it returns to extent items. */
void
convert_scenarios(struct context *context)
{
	static uint8_t data[FST_FILE_BYTES];
	static uint8_t filler[CONVERT_FILLER_BYTES];
	struct plan plan;
	char path[64];
	size_t i;

	fill_random(data, sizeof(data), 52);
	fill_pattern(filler, sizeof(filler), 53);
	namespace_plan(context, &plan, "fst-round-trip");
	plan_create(&plan, 1, "/convert/more", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/convert/more", 0, filler, sizeof(filler));
	for (i = 0; i < CONVERT_FILES; i++) {
		REQUIRE(snprintf(path, sizeof(path), "/convert/r%03zu", i) < (int)sizeof(path));
		plan_create(&plan, 1, path, BTRFS_MODE_REGULAR | 0644, NULL);
		plan_write_new(&plan, 1, path, 0, data, sizeof(data));
		plan_unlink(&plan, i % 2 == 1 ? 2 : 3, path, 0);
	}
	expect_bitmaps(&plan, 0, 1, "/convert/anchor", 0);
	expect_bitmaps(&plan, 2, 2, "/convert/anchor", 1);
	expect_bitmaps(&plan, 3, LAST_STAGE, "/convert/anchor", 0);
	expect_absent(&plan, 3, LAST_STAGE, "/convert/r000");
	run_plan(context, &plan);
}

/* A group whose last extents this transaction freed stays until the next
 * one, as Linux keeps a group with pinned bytes. Data frees land at commit,
 * so this test frees /sparse's sectors at once, as the cleaner frees blocks. */
static void
groups_pinned_test(struct context *context)
{
	const struct bt_disk_extent *extent;
	struct btrfs_inode inode;
	struct btrfs_fs *fs;
	struct btrfs_transaction *transaction;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	struct bt_key key;
	size_t removed = 1;
	size_t freed = 0;
	enum btrfs_result result;

	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/sparse", &inode) == BTRFS_OK);
	REQUIRE(btrfs_transaction_begin(fs, &context->writer, &transaction) == BTRFS_OK);
	REQUIRE(bt_find_root(fs, inode.id.tree, &root) == BTRFS_OK);
	key = (struct bt_key){ inode.id.inode, 0, BT_EXTENT_DATA };
	bt_cursor_init(&cursor, fs, root);
	result = bt_cursor_seek(&cursor, key, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		if (record.key.objectid != inode.id.inode || record.key.type != BT_EXTENT_DATA) {
			break;
		}
		extent = (const void *)record.data;
		if (record.size == sizeof(*extent) && bt_u64(extent->disk_bytenr) != 0) {
			REQUIRE(
			    bt_space_change_used(transaction->space, bt_u64(extent->disk_bytenr),
				bt_u64(extent->disk_bytes), 0) == BTRFS_OK);
			freed++;
		}
		result = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	REQUIRE(freed != 0);
	REQUIRE(btrfs_transaction_remove_unused_groups(transaction, &removed) == BTRFS_OK);
	REQUIRE(removed == 0);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	REQUIRE(context->image.live_allocations == 0);
	printf("groups-pinned: a group emptied in the transaction stays PASS\n");
}

/* Block groups come and go as on Linux. Removing /sparse empties the 32 MiB
 * data group holding its two sectors; like btrfs_delete_unused_bgs, the
 * cleaner keeps it in that commit (its space was freed there) and removes it
 * in the next: block group, free-space items, device extents and chunk item go
 * and the device item shrinks. A later write needs a new data chunk, which
 * takes the device space back. A chunk tree that outgrows its system chunk
 * gets a new one through the superblock's system array, and the emptied old
 * system group is removed from both. */
void
groups_scenarios(struct context *context)
{
	static uint8_t data[GROUPS_WRITE_BYTES];
	struct plan plan;
	struct btrfs_fs *fs;
	uint64_t groups;

	groups_pinned_test(context);
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	groups = fs->chunk_count;
	btrfs_unmount(fs);
	fill_random(data, sizeof(data), 61);
	namespace_plan(context, &plan, "groups-remove");
	plan_unlink(&plan, 1, "/sparse", 0);
	plan_remove_groups(&plan, 1, 0);
	plan_remove_groups(&plan, 2, 1);
	plan_create(&plan, 3, "/grown", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 3, "/grown", 0, data, sizeof(data));
	expect_groups(&plan, 0, 1, groups, 1);
	expect_groups(&plan, 2, 2, groups - 1, 1);
	expect_groups(&plan, 3, LAST_STAGE, groups, 1);
	expect_file(&plan, 3, LAST_STAGE, "/grown", data, sizeof(data));
	run_plan(context, &plan);

	namespace_plan(context, &plan, "groups-system");
	plan_system_growth(&plan, 1);
	plan_create(&plan, 1, "/system-grown", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_remove_groups(&plan, 2, 1);
	expect_groups(&plan, 0, 0, groups, 1);
	expect_groups(&plan, 1, 1, groups + 1, 2);
	expect_groups(&plan, 2, LAST_STAGE, groups, 1);
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
