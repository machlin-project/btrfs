/* SPDX-License-Identifier: BSD-3-Clause */
/* fs-verity on writable volumes: data changes of Linux's verity files refused,
 * names and attributes changed, verity enabled as FS_IOC_ENABLE_VERITY does,
 * an enable interrupted and cleaned up as Linux's mount does, and one rolled
 * back. Digests and item counts come from a model below that builds the
 * Merkle tree one whole level at a time; Linux's oracle measures the same
 * digests and reads every file through its own verification. */
#include "namespace.h"
#include "scenario.h"
#include "verity.h"

/* tests/prepare_linux.py's verity files. */
#define VERITY_TAIL_BYTES 10000U
#define VERITY_SALT_BYTES 32U
/* Enable inputs: a file with an unaligned tail, an empty one, a NODATACOW
 * file and a Zstd property file, and the steps of their enables. */
#define ENABLE_FILE_BYTES 10000U
#define ENABLE_NOCOW_BYTES 65536U
#define ENABLE_ZSTD_BYTES (200U * 1024U)
#define ENABLE_SIGNATURE_BYTES 300U
#define ENABLE_STEP_BLOCKS 64U
#define SMALL_STEP_BLOCKS 3U
/* An interrupted enable of /big (4 MiB) hashes INTERRUPT_BLOCKS 4 KiB blocks
 * in steps of INTERRUPT_STEP: two of its level-0 tree blocks complete. */
#define INTERRUPT_STEP 100U
#define INTERRUPT_BLOCKS 300U
#define INTERRUPT_SMALL_STEP 10U
#define INTERRUPT_SMALL_BLOCKS 20U
#define FIXTURE_BIG_BYTES (4U * 1024U * 1024U)
/* Parameters Linux refuses. */
#define ODD_BLOCK_SIZE 3000U
#define SMALL_BLOCK_SIZE 512U
#define LARGE_BLOCK_SIZE 8192U
#define UNKNOWN_ALGORITHM 3U
#define LARGE_SALT_BYTES (VERITY_SALT_BYTES + 1U)
#define LARGE_SIGNATURE_BYTES (BT_VERITY_DESCRIPTOR_MAX - 256U + 1U)
#define FALLOCATE_BYTES (1024U * 1024U)

struct verity_case {
	unsigned algorithm;
	uint32_t block;
	const uint8_t *salt;
	size_t salt_size;
	size_t signature_size;
};

static const uint8_t short_salt[] = { 0xa1, 0xb2, 0xc3, 0xd4, 0xe5 };
static uint8_t full_salt[VERITY_SALT_BYTES];

static size_t
digest_size(const struct verity_case *c)
{
	return c->algorithm == BTRFS_VERITY_HASH_SHA512 ? BT_SHA512_DIGEST : BT_SHA256_DIGEST;
}

static void
verity_hash(const struct verity_case *c, const uint8_t *block, size_t length, uint8_t *digest)
{
	struct bt_sha2 hash;
	uint8_t padded[BT_SHA2_BLOCK_MAX];

	bt_sha2_init(&hash, c->algorithm == BTRFS_VERITY_HASH_SHA512);
	if (c->salt_size != 0) {
		memset(padded, 0, sizeof(padded));
		memcpy(padded, c->salt, c->salt_size);
		bt_sha2_update(&hash, padded, bt_sha2_block_size(&hash));
	}
	bt_sha2_update(&hash, block, length);
	bt_sha2_final(&hash, digest);
}

/* Items Linux stores for tree blocks: one per 2 KiB. */
static uint64_t
block_items(const struct verity_case *c)
{
	return (c->block + BT_VERITY_ITEM_BYTES - 1U) / BT_VERITY_ITEM_BYTES;
}

/* fs-verity's file digest of size bytes and the VERITY_DESC and
 * VERITY_MERKLE items Linux stores for them. */
static void
verity_model(
    const struct verity_case *c, const uint8_t *data, size_t size, uint8_t *digest, uint64_t *items)
{
	struct bt_disk_verity_descriptor descriptor;
	const uint8_t *input = data;
	uint8_t *level = NULL;
	uint8_t *hashes;
	uint8_t *block = malloc(c->block);
	size_t input_size = size;
	size_t count = (size + c->block - 1U) / c->block;
	size_t tree_blocks = 0;
	size_t chunk;
	size_t i;
	unsigned log = 0;

	REQUIRE(block != NULL);
	memset(&descriptor, 0, sizeof(descriptor));
	while (count > 0) {
		hashes = calloc(count, digest_size(c));
		REQUIRE(hashes != NULL);
		for (i = 0; i < count; i++) {
			memset(block, 0, c->block);
			chunk = input_size - i * c->block < c->block ? input_size - i * c->block
								     : c->block;
			memcpy(block, input + i * c->block, chunk);
			verity_hash(c, block, c->block, hashes + i * digest_size(c));
		}
		if (count == 1) {
			memcpy(descriptor.root_hash, hashes, digest_size(c));
			free(hashes);
			break;
		}
		free(level);
		level = hashes;
		input = level;
		input_size = count * digest_size(c);
		count = (input_size + c->block - 1U) / c->block;
		tree_blocks += count;
	}
	free(level);
	free(block);
	while ((UINT32_C(1) << log) < c->block) {
		log++;
	}
	descriptor.version = BT_VERITY_VERSION;
	descriptor.hash_algorithm = (uint8_t)c->algorithm;
	descriptor.log_blocksize = (uint8_t)log;
	descriptor.salt_size = (uint8_t)c->salt_size;
	bt_put64(&descriptor.data_size, size);
	if (c->salt_size != 0) {
		memcpy(descriptor.salt, c->salt, c->salt_size);
	}
	/* The digest covers the descriptor without its signature. */
	verity_hash(&(struct verity_case){ .algorithm = c->algorithm },
	    (const uint8_t *)&descriptor, sizeof(descriptor), digest);
	*items = tree_blocks * block_items(c) + 1U +
	    (sizeof(descriptor) + c->signature_size + BT_VERITY_ITEM_BYTES - 1U) /
		BT_VERITY_ITEM_BYTES;
}

/* The tree items an enable stored after hashing blocks of a file: each level's
 * blocks are stored once full. */
static uint64_t
partial_items(const struct verity_case *c, uint64_t blocks)
{
	uint64_t hashes = c->block / digest_size(c);
	uint64_t stored = 0;

	for (blocks /= hashes; blocks != 0; blocks /= hashes) {
		stored += blocks;
	}
	return stored * block_items(c);
}

/* The digest and items the model gives the bytes of source, expected for path. */
static void
expect_model(struct plan *plan, size_t first, size_t last, const char *path,
    const struct verity_case *c, const uint8_t *data, size_t size)
{
	uint8_t digest[BTRFS_VERITY_DIGEST_MAX];
	uint64_t items;

	verity_model(c, data, size, digest, &items);
	expect_verity(plan, first, last, path, c->algorithm, digest, digest_size(c), items);
}

void
expect_verity_model(struct plan *plan, size_t first, size_t last, const char *path,
    unsigned algorithm, uint32_t block_size, const void *salt, size_t salt_size,
    const uint8_t *data, size_t size)
{
	const struct verity_case c = { algorithm, block_size, salt, salt_size, 0 };

	expect_model(plan, first, last, path, &c, data, size);
}

static void
expect_fixture_model(struct context *context, struct plan *plan, size_t first, size_t last,
    const char *path, const char *source, const struct verity_case *c)
{
	uint8_t *data;
	size_t size;

	data = fixture_bytes(context, source, &size);
	expect_model(plan, first, last, path, c, data, size);
	free(data);
}

static void
plan_enable(
    struct plan *plan, size_t commit, const char *path, const struct verity_case *c, size_t budget)
{
	plan_verity(plan, commit, path, VERITY_ENABLE, c->algorithm, c->block, c->salt,
	    c->salt_size, c->signature_size, budget, 0);
}

static void
plan_refused_enable(struct plan *plan, size_t commit, const char *path, const struct verity_case *c,
    enum btrfs_result result)
{
	plan_enable(plan, commit, path, c, ENABLE_STEP_BLOCKS);
	plan_expect_refusal(plan, commit, result);
}

/* Linux's verity files keep their data: writes, truncation, preallocation and
 * punching are refused (EPERM), as is a second enable (EEXIST); enables are
 * refused in Linux's order. Names, xattrs, modes and inode flags change, and
 * a deleted verity file takes its items. */
static void
verity_refusal_plan(struct context *context)
{
	static const struct verity_case sha256 = { BTRFS_VERITY_HASH_SHA256, 4096, NULL, 0, 0 };
	static uint8_t data[ENABLE_FILE_BYTES];
	struct verity_case c = sha256;
	struct plan plan;
	uint8_t *tail;
	size_t tail_size;

	fill_pattern(data, sizeof(data), 61);
	plan_init(&plan);
	/* An unchanged file gives every stage its generation. */
	plan_file(context, &plan, "/greeting");
	plan.name = "verity-refusals";
	plan_write_new(&plan, 1, "/verity/tail", 0, data, 100);
	plan_expect_refusal(&plan, 1, BTRFS_NOT_PERMITTED);
	plan_write_new(&plan, 1, "/verity/inline", 0, data, 10);
	plan_expect_refusal(&plan, 1, BTRFS_NOT_PERMITTED);
	plan_truncate_new(&plan, 1, "/verity/tail", 0);
	plan_expect_refusal(&plan, 1, BTRFS_NOT_PERMITTED);
	plan_fallocate_new(&plan, 1, "/verity/tail", 0, 0, FALLOCATE_BYTES);
	plan_expect_refusal(&plan, 1, BTRFS_NOT_PERMITTED);
	plan_fallocate_new(&plan, 1, "/verity/tail",
	    BTRFS_FALLOCATE_PUNCH_HOLE | BTRFS_FALLOCATE_KEEP_SIZE, 0, 4096);
	plan_expect_refusal(&plan, 1, BTRFS_NOT_PERMITTED);
	plan_refused_enable(&plan, 1, "/verity/sha256", &c, BTRFS_EXISTS);
	/* A read-only subvolume refuses write access before the file. */
	plan_refused_enable(&plan, 1, "/verity-ro/sha256", &c, BTRFS_READ_ONLY);
	plan_refused_enable(&plan, 1, "/data-ro/big", &c, BTRFS_READ_ONLY);
	plan_refused_enable(&plan, 1, "/many", &c, BTRFS_IS_DIRECTORY);
	plan_refused_enable(&plan, 1, "/symlink", &c, BTRFS_INVALID_ARGUMENT);
	c.block = ODD_BLOCK_SIZE;
	c.salt_size = LARGE_SALT_BYTES;
	c.salt = data;
	plan_refused_enable(&plan, 1, "/random", &c, BTRFS_INVALID_ARGUMENT);
	c = sha256;
	c.block = SMALL_BLOCK_SIZE;
	plan_refused_enable(&plan, 1, "/random", &c, BTRFS_INVALID_ARGUMENT);
	c.block = LARGE_BLOCK_SIZE;
	plan_refused_enable(&plan, 1, "/random", &c, BTRFS_INVALID_ARGUMENT);
	c = sha256;
	c.algorithm = UNKNOWN_ALGORITHM;
	plan_refused_enable(&plan, 1, "/random", &c, BTRFS_INVALID_ARGUMENT);
	c = sha256;
	c.salt = data;
	c.salt_size = LARGE_SALT_BYTES;
	plan_refused_enable(&plan, 1, "/random", &c, BTRFS_RANGE);
	c = sha256;
	c.signature_size = LARGE_SIGNATURE_BYTES;
	plan_refused_enable(&plan, 1, "/random", &c, BTRFS_RANGE);
	plan_create(&plan, 1, "/immutable", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_set_fsflags(&plan, 1, "/immutable", BTRFS_FS_IMMUTABLE_FL);
	plan_create(&plan, 1, "/append", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_set_fsflags(&plan, 1, "/append", BTRFS_FS_APPEND_FL);
	/* The changes a verity file still takes. */
	plan_rename(&plan, 1, "/verity/sha256", "/verity/renamed", 0);
	plan_link(&plan, 1, "/verity/block", "/verity/block-link");
	plan_set_xattr(&plan, 1, "/verity/tail", "user.verity", "kept", 4, 0);
	plan_set_fsflags(&plan, 1, "/verity/tail", BTRFS_FS_NOATIME_FL);
	plan_unlink(&plan, 1, "/verity/boundary", 0);
	c = sha256;
	plan_refused_enable(&plan, 2, "/immutable", &c, BTRFS_NOT_PERMITTED);
	plan_refused_enable(&plan, 2, "/append", &c, BTRFS_NOT_PERMITTED);
	plan_set_fsflags(&plan, 2, "/immutable", 0);
	plan_set_fsflags(&plan, 2, "/verity/tail", BTRFS_FS_IMMUTABLE_FL);
	plan_unlink(&plan, 2, "/verity/block-link", 0);
	tail = fixture_bytes(context, "/verity/tail", &tail_size);
	REQUIRE(tail_size == VERITY_TAIL_BYTES);
	expect_file(&plan, 0, LAST_STAGE, "/verity/tail", tail, tail_size);
	expect_model(&plan, 0, LAST_STAGE, "/verity/tail", &sha256, tail, tail_size);
	free(tail);
	expect_fixture_model(
	    context, &plan, 1, LAST_STAGE, "/verity/renamed", "/verity/sha256", &sha256);
	expect_current(context, &plan, 1, LAST_STAGE, "/verity/renamed", "/verity/sha256");
	expect_absent(&plan, 1, LAST_STAGE, "/verity/sha256");
	expect_same(&plan, 1, 1, "/verity/block-link", "/verity/block");
	expect_absent(&plan, 2, LAST_STAGE, "/verity/block-link");
	expect_absent(&plan, 1, LAST_STAGE, "/verity/boundary");
	expect_xattr(&plan, 1, LAST_STAGE, "/verity/tail", "user.verity", "kept", 4);
	expect_flags(&plan, 1, 1, "/verity/tail", BT_INODE_RO_VERITY | BT_INODE_NOATIME,
	    BT_INODE_RO_VERITY | BT_INODE_NOATIME);
	expect_flags(&plan, 2, LAST_STAGE, "/verity/tail",
	    BT_INODE_RO_VERITY | BT_INODE_NOATIME | BT_INODE_IMMUTABLE,
	    BT_INODE_RO_VERITY | BT_INODE_IMMUTABLE);
	expect_verity(&plan, 1, LAST_STAGE, "/immutable", 0, NULL, 0, 0);
	run_plan(context, &plan);
}

/* Enables of new and Linux-written files of every shape, in steps that store
 * tree blocks as they complete, then changes to the new verity files. */
static void
verity_enable_plan(struct context *context)
{
	static uint8_t file[ENABLE_FILE_BYTES];
	static uint8_t nocow[ENABLE_NOCOW_BYTES];
	static uint8_t text[ENABLE_ZSTD_BYTES];
	const struct verity_case cases[] = {
		{ BTRFS_VERITY_HASH_SHA256, 4096, NULL, 0, 0 },
		{ BTRFS_VERITY_HASH_SHA512, 1024, short_salt, sizeof(short_salt), 0 },
		{ BTRFS_VERITY_HASH_SHA512, 4096, full_salt, sizeof(full_salt), 0 },
		{ BTRFS_VERITY_HASH_SHA256, 1024, NULL, 0, 0 },
	};
	struct plan plan;

	fill_pattern(file, sizeof(file), 62);
	fill_random(nocow, sizeof(nocow), 63);
	fill_text(text, sizeof(text), 64);
	plan_init(&plan);
	plan.name = "verity-enable";
	plan_file(context, &plan, "/greeting");
	plan_create(&plan, 1, "/v-file", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/v-file", 0, file, sizeof(file));
	plan_create(&plan, 1, "/v-empty", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_create(&plan, 1, "/v-nocow", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_set_fsflags(&plan, 1, "/v-nocow", BTRFS_FS_NOCOW_FL);
	plan_write_new(&plan, 1, "/v-nocow", 0, nocow, sizeof(nocow));
	plan_create(&plan, 1, "/v-zstd", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_set_xattr(&plan, 1, "/v-zstd", BT_COMPRESSION_PROPERTY, "zstd", 4, 0);
	plan_write_new(&plan, 1, "/v-zstd", 0, text, sizeof(text));
	plan_enable(&plan, 2, "/v-file", &cases[0], 1);
	plan_enable(&plan, 2, "/v-empty", &cases[1], 1);
	plan_enable(&plan, 2, "/v-nocow", &cases[2], SMALL_STEP_BLOCKS);
	plan_enable(&plan, 2, "/v-zstd", &cases[3], ENABLE_STEP_BLOCKS);
	plan_enable(&plan, 2, "/big", &cases[0], ENABLE_STEP_BLOCKS);
	plan_enable(&plan, 2, "/random", &cases[1], ENABLE_STEP_BLOCKS);
	plan_enable(&plan, 2, "/hardlink", &cases[0], 1);
	plan_write_new(&plan, 3, "/v-file", 0, file, 1);
	plan_expect_refusal(&plan, 3, BTRFS_NOT_PERMITTED);
	plan_refused_enable(&plan, 3, "/greeting", &cases[1], BTRFS_EXISTS);
	plan_rename(&plan, 3, "/v-file", "/v-renamed", 0);
	plan_unlink(&plan, 3, "/v-empty", 0);
	plan_unlink(&plan, 3, "/v-nocow", 1);
	expect_file(&plan, 1, 2, "/v-file", file, sizeof(file));
	expect_file(&plan, 3, LAST_STAGE, "/v-renamed", file, sizeof(file));
	expect_verity(&plan, 1, 1, "/v-file", 0, NULL, 0, 0);
	expect_model(&plan, 2, 2, "/v-file", &cases[0], file, sizeof(file));
	expect_model(&plan, 3, LAST_STAGE, "/v-renamed", &cases[0], file, sizeof(file));
	expect_model(&plan, 2, 2, "/v-empty", &cases[1], NULL, 0);
	expect_absent(&plan, 3, LAST_STAGE, "/v-empty");
	expect_file(&plan, 1, 2, "/v-nocow", nocow, sizeof(nocow));
	expect_model(&plan, 2, 2, "/v-nocow", &cases[2], nocow, sizeof(nocow));
	expect_absent(&plan, 3, LAST_STAGE, "/v-nocow");
	expect_file(&plan, 1, LAST_STAGE, "/v-zstd", text, sizeof(text));
	expect_model(&plan, 2, LAST_STAGE, "/v-zstd", &cases[3], text, sizeof(text));
	expect_current(context, &plan, 0, LAST_STAGE, "/big", "/big");
	expect_fixture_model(context, &plan, 2, LAST_STAGE, "/big", "/big", &cases[0]);
	expect_current(context, &plan, 0, LAST_STAGE, "/random", "/random");
	expect_fixture_model(context, &plan, 2, LAST_STAGE, "/random", "/random", &cases[1]);
	/* The inline file and its second name share the inode. */
	expect_fixture_model(context, &plan, 2, LAST_STAGE, "/greeting", "/greeting", &cases[0]);
	expect_flags(&plan, 2, LAST_STAGE, "/v-zstd", BT_INODE_RO_VERITY, BT_INODE_RO_VERITY);
	expect_compat_ro(&plan, 2, LAST_STAGE, BT_COMPAT_RO_VERITY);
	run_plan(context, &plan);
}

/* An enable interrupted after storing some tree blocks leaves them and an
 * orphan item, which orphan cleanup drops as Linux's mount does; an unlink
 * while open reuses that orphan item, and cleanup then deletes the file with
 * its items. A later enable finishes, and one rolled back changes nothing. */
static void
verity_interrupt_plan(struct context *context)
{
	const struct verity_case big = { BTRFS_VERITY_HASH_SHA256, 4096, NULL, 0, 0 };
	const struct verity_case random = { BTRFS_VERITY_HASH_SHA512, 1024, full_salt,
		sizeof(full_salt), 0 };
	struct plan plan;

	plan_init(&plan);
	plan.name = "verity-interrupt";
	plan_file(context, &plan, "/greeting");
	plan_verity(&plan, 1, "/big", VERITY_INTERRUPT, big.algorithm, big.block, NULL, 0, 0,
	    INTERRUPT_STEP, INTERRUPT_BLOCKS);
	plan_verity(&plan, 1, "/random", VERITY_INTERRUPT, random.algorithm, random.block,
	    random.salt, random.salt_size, 0, INTERRUPT_SMALL_STEP, INTERRUPT_SMALL_BLOCKS);
	plan_unlink(&plan, 2, "/random", 1);
	plan_clean(&plan, 2, BTRFS_TOP_LEVEL_TREE, 1);
	plan_enable(&plan, 3, "/big", &big, ENABLE_STEP_BLOCKS);
	plan_verity(&plan, 3, "/preallocated", VERITY_ROLLBACK, random.algorithm, random.block,
	    random.salt, random.salt_size, 0, ENABLE_STEP_BLOCKS, 0);
	expect_current(context, &plan, 0, LAST_STAGE, "/big", "/big");
	expect_current(context, &plan, 0, 1, "/random", "/random");
	expect_current(context, &plan, 0, LAST_STAGE, "/preallocated", "/preallocated");
	expect_verity(&plan, 1, 1, "/big", 0, NULL, 0, partial_items(&big, INTERRUPT_BLOCKS));
	expect_verity(
	    &plan, 1, 1, "/random", 0, NULL, 0, partial_items(&random, INTERRUPT_SMALL_BLOCKS));
	expect_verity(&plan, 2, 2, "/big", 0, NULL, 0, 0);
	expect_absent(&plan, 2, LAST_STAGE, "/random");
	expect_verity(&plan, 2, LAST_STAGE, "/preallocated", 0, NULL, 0, 0);
	expect_fixture_model(context, &plan, 3, LAST_STAGE, "/big", "/big", &big);
	run_plan(context, &plan);
}

/* A builtin signature is stored after the descriptor, unverified: the
 * caller's keyring policy decides it. Linux's reference kernel, with builtin
 * signatures and an empty keyring, opens no signed file (ENOKEY), so this
 * plan is not exported to it. */
static void
verity_signature_plan(struct context *context)
{
	static uint8_t file[ENABLE_FILE_BYTES];
	const struct verity_case signed_case = { BTRFS_VERITY_HASH_SHA256, 4096, short_salt,
		sizeof(short_salt), ENABLE_SIGNATURE_BYTES };
	const struct verity_case large = { BTRFS_VERITY_HASH_SHA512, 1024, NULL, 0,
		BT_VERITY_DESCRIPTOR_MAX - sizeof(struct bt_disk_verity_descriptor) };
	struct plan plan;

	fill_pattern(file, sizeof(file), 65);
	plan_init(&plan);
	plan.name = "verity-signature";
	plan_file(context, &plan, "/greeting");
	plan_create(&plan, 1, "/v-signed", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/v-signed", 0, file, sizeof(file));
	plan_create(&plan, 1, "/v-large", BTRFS_MODE_REGULAR | 0644, NULL);
	plan_write_new(&plan, 1, "/v-large", 0, file, sizeof(file));
	plan_enable(&plan, 2, "/v-signed", &signed_case, SMALL_STEP_BLOCKS);
	plan_enable(&plan, 2, "/v-large", &large, SMALL_STEP_BLOCKS);
	expect_file(&plan, 1, LAST_STAGE, "/v-signed", file, sizeof(file));
	expect_model(&plan, 2, LAST_STAGE, "/v-signed", &signed_case, file, sizeof(file));
	expect_file(&plan, 1, LAST_STAGE, "/v-large", file, sizeof(file));
	expect_model(&plan, 2, LAST_STAGE, "/v-large", &large, file, sizeof(file));
	run_plan(context, &plan);
}

/* On a volume without fs-verity, the first enable sets the feature. */
static void
verity_feature_plan(struct context *context)
{
	const struct verity_case big = { BTRFS_VERITY_HASH_SHA256, 4096, NULL, 0, 0 };
	struct plan plan;

	plan_init(&plan);
	plan.name = "verity-feature";
	plan_file(context, &plan, "/greeting");
	plan_enable(&plan, 1, "/big", &big, ENABLE_STEP_BLOCKS);
	expect_current(context, &plan, 0, LAST_STAGE, "/big", "/big");
	expect_verity(&plan, 0, 0, "/big", 0, NULL, 0, 0);
	expect_fixture_model(context, &plan, 1, LAST_STAGE, "/big", "/big", &big);
	expect_compat_ro(&plan, 1, LAST_STAGE, BT_COMPAT_RO_VERITY);
	run_plan(context, &plan);
}

void
verity_scenarios(struct context *context)
{
	struct btrfs_fs *fs;
	struct btrfs_info info;
	size_t i;

	for (i = 0; i < sizeof(full_salt); i++) {
		full_salt[i] = (uint8_t)i;
	}
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	btrfs_get_info(fs, &info);
	btrfs_unmount(fs);
	if ((info.readonly_features & BT_COMPAT_RO_VERITY) == 0) {
		verity_feature_plan(context);
		return;
	}
	verity_refusal_plan(context);
	verity_enable_plan(context);
	verity_interrupt_plan(context);
	if (context->export_root == NULL) {
		verity_signature_plan(context);
	}
}
