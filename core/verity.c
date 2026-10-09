/* SPDX-License-Identifier: BSD-3-Clause */
/* fs-verity as Linux's fs/verity and fs/btrfs/verity.c keep it. A file's
 * descriptor and Merkle tree live in VERITY_DESC and VERITY_MERKLE items of
 * its tree, keyed by byte offset. Reads hash every data block, zero-padded to
 * the tree's block size and with the salt before it, and verify the chain of
 * tree blocks up to the descriptor's root hash; the tree blocks one read has
 * verified stay in use, one per level, as Linux's verified-block marks do. */
#include "encode.h"
#include "namespace.h"
#include "verity.h"

/* File bytes one verification pass reads: whole blocks of every tree block
 * size up to the largest sector. */
#define BT_VERITY_WINDOW (64U * 1024U)
#define BT_VERITY_LOG_SHA256 5U
#define BT_VERITY_LOG_SHA512 6U
#define BT_VERITY_LOG_BLOCK_LIMIT 31U
/* File bytes one enable step reads at a time. */
#define BT_VERITY_BUILD_WINDOW (64U * 1024U)
/* Linux refuses a tree of more blocks when they are smaller than a page
 * (EFBIG); enables here refuse it for every block size, so that each page size
 * opens their files. */
#define BT_VERITY_TREE_BLOCKS_MAX (UINT64_C(1) << 23)
#define BT_VERITY_SIGNATURE_MAX                                                                    \
	(BT_VERITY_DESCRIPTOR_MAX - sizeof(struct bt_disk_verity_descriptor))
/* Items Linux's end_enable_verity and rollback_verity reserve for the inode
 * update and the orphan item. */
#define BT_VERITY_FINAL_ITEMS 2U

struct bt_verity_reader {
	struct bt_verity verity;
	struct bt_cursor cursor;
	uint64_t inode;
	/* The tree block of each level verified last, in blocks (levels times
	 * the block size). */
	uint64_t index[BT_VERITY_LEVELS_MAX];
	int valid[BT_VERITY_LEVELS_MAX];
	uint8_t *blocks;
	size_t blocks_size;
	uint8_t *window;
};

static int
bt_verity_file(const struct btrfs_inode *inode)
{
	return (inode->mode & BTRFS_MODE_TYPE) == BTRFS_MODE_REGULAR &&
	    (inode->flags & BT_INODE_RO_VERITY) != 0;
}

/* Linux's read_key_bytes: from offset, the bytes of consecutive items of
 * type, beginning in the item at or before offset, up to length. The first
 * copy of them go to out; *found counts all. */
static enum btrfs_result
bt_verity_bytes(struct bt_cursor *cursor, uint64_t inode, uint8_t type, uint64_t offset,
    uint8_t *out, size_t copy, size_t length, size_t *found)
{
	struct bt_record record;
	struct bt_key key = { .objectid = inode, .type = type, .offset = offset };
	uint64_t item_end;
	size_t count;
	enum btrfs_result error;

	*found = 0;
	error = bt_cursor_seek(cursor, key, 1);
	while (error == BTRFS_OK && *found < length) {
		(void)bt_cursor_record(cursor, &record);
		if (record.key.objectid != inode || record.key.type != type ||
		    record.size > UINT64_MAX - record.key.offset) {
			break;
		}
		item_end = record.key.offset + record.size;
		/* Each further item continues where the previous one ended. */
		if (*found > 0 ? record.key.offset != offset
			       : record.key.offset > offset || item_end <= offset) {
			break;
		}
		count = item_end - offset < length - *found ? (size_t)(item_end - offset)
							    : length - *found;
		if (*found < copy) {
			bt_copy(out + *found, record.data + (size_t)(offset - record.key.offset),
			    count < copy - *found ? count : copy - *found);
		}
		offset += count;
		*found += count;
		error = bt_cursor_next(cursor);
	}
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

enum btrfs_result
bt_verity_parameters(struct bt_verity *verity, uint32_t sector_size)
{
	const struct bt_disk_verity_descriptor *descriptor = &verity->descriptor;
	uint8_t padding[BT_SHA2_BLOCK_MAX];
	uint64_t counts[BT_VERITY_LEVELS_MAX];
	uint64_t blocks;
	uint64_t offset = 0;
	uint64_t data_size = bt_u64(descriptor->data_size);
	unsigned log_digest;
	unsigned level;
	size_t block;

	switch (descriptor->hash_algorithm) {
	case BT_VERITY_HASH_SHA256:
		log_digest = BT_VERITY_LOG_SHA256;
		bt_sha2_init(&verity->salted, 0);
		break;
	case BT_VERITY_HASH_SHA512:
		log_digest = BT_VERITY_LOG_SHA512;
		bt_sha2_init(&verity->salted, 1);
		break;
	default:
		return BTRFS_UNSUPPORTED;
	}
	if (descriptor->log_blocksize < BT_VERITY_LOG_BLOCK_MIN ||
	    descriptor->log_blocksize > BT_VERITY_LOG_BLOCK_LIMIT ||
	    (UINT32_C(1) << descriptor->log_blocksize) > sector_size ||
	    descriptor->salt_size > sizeof(descriptor->salt) || data_size > INT64_MAX) {
		return BTRFS_CORRUPT;
	}
	verity->log_block = descriptor->log_blocksize;
	verity->block_size = UINT32_C(1) << verity->log_block;
	verity->digest_size = 1U << log_digest;
	verity->log_arity = verity->log_block - log_digest;
	verity->levels = 0;
	blocks = (data_size + verity->block_size - 1U) >> verity->log_block;
	while (blocks > 1) {
		if (verity->levels == BT_VERITY_LEVELS_MAX) {
			return BTRFS_CORRUPT;
		}
		blocks = (blocks + (UINT64_C(1) << verity->log_arity) - 1U) >> verity->log_arity;
		counts[verity->levels++] = blocks;
	}
	for (level = verity->levels; level-- > 0;) {
		verity->level_start[level] = offset;
		offset += counts[level];
	}
	verity->tree_size = offset << verity->log_block;
	if (descriptor->salt_size != 0) {
		block = bt_sha2_block_size(&verity->salted);
		bt_zero(padding, sizeof(padding));
		bt_copy(padding, descriptor->salt, descriptor->salt_size);
		bt_sha2_update(&verity->salted, padding, block);
	}
	return BTRFS_OK;
}

void
bt_verity_hash(const struct bt_verity *verity, const uint8_t *block, uint8_t *digest)
{
	struct bt_sha2 hash = verity->salted;

	bt_sha2_update(&hash, block, verity->block_size);
	bt_sha2_final(&hash, digest);
}

/* The descriptor of a verity file, checked as Linux's fsverity_get_descriptor
 * and validate_fsverity_descriptor check it at open. */
static enum btrfs_result
bt_verity_load(const struct btrfs_fs *fs, const struct btrfs_inode *inode, struct bt_cursor *cursor,
    struct bt_verity *verity)
{
	struct bt_disk_verity_item item;
	uint64_t size;
	size_t found;
	size_t i;
	enum btrfs_result error;

	bt_zero(verity, sizeof(*verity));
	bt_zero(&item, sizeof(item));
	error = bt_verity_bytes(cursor, inode->id.inode, BT_VERITY_DESC_ITEM, 0, (uint8_t *)&item,
	    sizeof(item), sizeof(item), &found);
	if (error != BTRFS_OK) {
		return error;
	}
	size = bt_u64(item.size);
	if (bt_u64(item.reserved[0]) != 0 || bt_u64(item.reserved[1]) != 0 ||
	    size < sizeof(verity->descriptor) || size > BT_VERITY_DESCRIPTOR_MAX) {
		return BTRFS_CORRUPT;
	}
	error = bt_verity_bytes(cursor, inode->id.inode, BT_VERITY_DESC_ITEM,
	    BT_VERITY_DESCRIPTOR_OFFSET, (uint8_t *)&verity->descriptor, sizeof(verity->descriptor),
	    (size_t)size, &found);
	if (error != BTRFS_OK) {
		return error;
	}
	if (found != size) {
		return BTRFS_CORRUPT;
	}
	verity->descriptor_size = (uint32_t)size;
	if (verity->descriptor.version != BT_VERITY_VERSION) {
		return BTRFS_UNSUPPORTED;
	}
	for (i = 0; i < sizeof(verity->descriptor.reserved); i++) {
		if (verity->descriptor.reserved[i] != 0) {
			return BTRFS_CORRUPT;
		}
	}
	if (bt_u64(verity->descriptor.data_size) != inode->size ||
	    bt_u32(verity->descriptor.sig_size) > size - sizeof(verity->descriptor)) {
		return BTRFS_CORRUPT;
	}
	return bt_verity_parameters(verity, fs->info.sector_size);
}

/* Merkle tree block index of level, zero-filled where the items stop as
 * Linux's tree pages are. */
static enum btrfs_result
bt_verity_tree_block(struct bt_verity_reader *reader, unsigned level, uint64_t index, uint8_t *out)
{
	const struct bt_verity *verity = &reader->verity;
	size_t found;
	enum btrfs_result error;

	error = bt_verity_bytes(&reader->cursor, reader->inode, BT_VERITY_MERKLE_ITEM,
	    (verity->level_start[level] + index) << verity->log_block, out, verity->block_size,
	    verity->block_size, &found);
	if (error == BTRFS_OK) {
		bt_zero(out + found, verity->block_size - found);
	}
	return error;
}

/* Linux's verify_data_block: climbs from the data block's hash to the first
 * tree block this read has verified, or to the root hash, then verifies the
 * blocks below it and the data. */
static enum btrfs_result
bt_verity_block(struct bt_verity_reader *reader, uint64_t data_index, const uint8_t *data)
{
	const struct bt_verity *verity = &reader->verity;
	uint64_t index[BT_VERITY_LEVELS_MAX];
	size_t position[BT_VERITY_LEVELS_MAX];
	uint64_t child = data_index;
	uint64_t hashes = UINT64_C(1) << verity->log_arity;
	uint8_t real[BT_SHA512_DIGEST];
	const uint8_t *want = verity->descriptor.root_hash;
	uint8_t *block;
	unsigned level;
	enum btrfs_result error;

	for (level = 0; level < verity->levels; level++) {
		index[level] = child >> verity->log_arity;
		position[level] = (size_t)(child & (hashes - 1U)) * verity->digest_size;
		if (reader->valid[level] && reader->index[level] == index[level]) {
			want =
			    reader->blocks + (size_t)level * verity->block_size + position[level];
			break;
		}
		child = index[level];
	}
	while (level > 0) {
		level--;
		block = reader->blocks + (size_t)level * verity->block_size;
		reader->valid[level] = 0;
		error = bt_verity_tree_block(reader, level, index[level], block);
		if (error != BTRFS_OK) {
			return error;
		}
		bt_verity_hash(verity, block, real);
		if (!bt_equal(real, want, verity->digest_size)) {
			return BTRFS_CORRUPT;
		}
		reader->index[level] = index[level];
		reader->valid[level] = 1;
		want = block + position[level];
	}
	bt_verity_hash(verity, data, real);
	return bt_equal(real, want, verity->digest_size) ? BTRFS_OK : BTRFS_CORRUPT;
}

static void
bt_verity_close(const struct btrfs_fs *fs, struct bt_verity_reader *reader)
{
	bt_cursor_fini(&reader->cursor);
	if (reader->window != NULL) {
		fs->env.release(fs->env.context, reader->window, BT_VERITY_WINDOW);
	}
	if (reader->blocks != NULL) {
		fs->env.release(fs->env.context, reader->blocks, reader->blocks_size);
	}
	fs->env.release(fs->env.context, reader, sizeof(*reader));
}

static enum btrfs_result
bt_verity_open(
    const struct btrfs_fs *fs, const struct btrfs_inode *inode, struct bt_verity_reader **result)
{
	struct bt_verity_reader *reader;
	enum btrfs_result error;

	*result = NULL;
	reader = fs->env.allocate(fs->env.context, sizeof(*reader));
	if (reader == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(reader, sizeof(*reader));
	reader->inode = inode->id.inode;
	error = bt_inode_cursor(fs, inode, &reader->cursor);
	if (error != BTRFS_OK) {
		fs->env.release(fs->env.context, reader, sizeof(*reader));
		return error;
	}
	error = bt_verity_load(fs, inode, &reader->cursor, &reader->verity);
	if (error != BTRFS_OK) {
		bt_verity_close(fs, reader);
		return error;
	}
	*result = reader;
	return BTRFS_OK;
}

enum btrfs_result
bt_verity_read(const struct btrfs_fs *fs, const struct btrfs_inode *inode, uint64_t offset,
    void *buffer, size_t length, size_t *completed)
{
	struct bt_verity_reader *reader;
	uint64_t end = offset + length;
	uint64_t position;
	size_t window;
	size_t data;
	size_t got;
	size_t skip;
	size_t count;
	size_t i;
	enum btrfs_result error;

	*completed = 0;
	error = bt_verity_open(fs, inode, &reader);
	if (error != BTRFS_OK) {
		return error;
	}
	reader->blocks_size = (size_t)reader->verity.levels * reader->verity.block_size;
	reader->window = fs->env.allocate(fs->env.context, BT_VERITY_WINDOW);
	if (reader->blocks_size != 0) {
		reader->blocks = fs->env.allocate(fs->env.context, reader->blocks_size);
	}
	if (reader->window == NULL || (reader->blocks_size != 0 && reader->blocks == NULL)) {
		bt_verity_close(fs, reader);
		return BTRFS_NO_MEMORY;
	}
	position = offset & ~(uint64_t)(reader->verity.block_size - 1U);
	while (position < end) {
		window = end - position >= BT_VERITY_WINDOW
		    ? BT_VERITY_WINDOW
		    : (size_t)((end - position + reader->verity.block_size - 1U) &
			  ~(uint64_t)(reader->verity.block_size - 1U));
		data = inode->size - position < window ? (size_t)(inode->size - position) : window;
		error = bt_read_data(fs, inode, position, reader->window, data, &got);
		if (error == BTRFS_OK && got != data) {
			error = BTRFS_CORRUPT;
		}
		if (error != BTRFS_OK) {
			break;
		}
		bt_zero(reader->window + data, window - data);
		for (i = 0; i < window && error == BTRFS_OK; i += reader->verity.block_size) {
			error = bt_verity_block(
			    reader, (position + i) >> reader->verity.log_block, reader->window + i);
		}
		if (error != BTRFS_OK) {
			break;
		}
		skip = offset > position ? (size_t)(offset - position) : 0;
		count =
		    (size_t)((end < position + window ? end : position + window) - position) - skip;
		bt_copy((uint8_t *)buffer + *completed, reader->window + skip, count);
		*completed += count;
		position += window;
	}
	bt_verity_close(fs, reader);
	return error;
}

enum btrfs_result
btrfs_verity_digest(const struct btrfs_fs *fs, const struct btrfs_inode *inode, unsigned *algorithm,
    uint8_t *digest, size_t capacity, size_t *length)
{
	struct bt_verity_reader *reader;
	struct bt_disk_verity_descriptor descriptor;
	struct bt_sha2 hash;
	enum btrfs_result error;

	if (length != NULL) {
		*length = 0;
	}
	if (fs == NULL || inode == NULL || algorithm == NULL || length == NULL ||
	    (digest == NULL && capacity != 0)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (!bt_verity_file(inode)) {
		return BTRFS_NOT_FOUND;
	}
	error = bt_verity_open(fs, inode, &reader);
	if (error != BTRFS_OK) {
		return error;
	}
	*algorithm = reader->verity.descriptor.hash_algorithm;
	*length = reader->verity.digest_size;
	if (capacity < reader->verity.digest_size) {
		bt_verity_close(fs, reader);
		return BTRFS_RANGE;
	}
	/* Linux's compute_file_digest: the descriptor without its signature. */
	descriptor = reader->verity.descriptor;
	bt_put32(&descriptor.sig_size, 0);
	bt_sha2_init(&hash, reader->verity.descriptor.hash_algorithm == BT_VERITY_HASH_SHA512);
	bt_sha2_update(&hash, &descriptor, sizeof(descriptor));
	bt_sha2_final(&hash, digest);
	bt_verity_close(fs, reader);
	return BTRFS_OK;
}

struct btrfs_verity_build {
	struct bt_verity verity;
	struct btrfs_object_id id;
	uint64_t generation;
	uint64_t position;
	/* The next block of each tree level, and the hash bytes pending in each
	 * level's block; level `levels` is the root hash. */
	uint64_t next[BT_VERITY_LEVELS_MAX];
	size_t filled[BT_VERITY_LEVELS_MAX + 1];
	/* One block per tree level, then the data window. */
	uint8_t *blocks;
	size_t blocks_size;
	/* The descriptor followed by the builtin signature. */
	uint8_t *descriptor;
	size_t descriptor_size;
	/* Items an earlier enable left are gone. */
	int cleared;
	/* A step or finish failed: only abort remains. */
	enum btrfs_result failure;
	void *(*allocate)(void *context, size_t size);
	void (*release)(void *context, void *allocation, size_t size);
	void *context;
};

/* Linux's write_key_bytes: bytes from offset in items of at most 2 KiB, each
 * reserved against qgroup limits as its own transaction is. */
static enum btrfs_result
bt_verity_store(struct btrfs_transaction *transaction, struct bt_owned_root *tree, uint64_t inode,
    uint8_t type, uint64_t offset, const uint8_t *bytes, size_t length)
{
	struct bt_key key = { .objectid = inode, .type = type };
	size_t count;
	size_t done;
	enum btrfs_result error = BTRFS_OK;

	for (done = 0; done < length && error == BTRFS_OK; done += count) {
		count = length - done < BT_VERITY_ITEM_BYTES ? length - done : BT_VERITY_ITEM_BYTES;
		key.offset = offset + done;
		error = bt_ns_reserve_items(transaction, tree->root.owner, 1);
		if (error == BTRFS_OK) {
			error = bt_tx_edit(
			    transaction, &tree->root, key, bytes + done, count, BT_INSERT);
			transaction->changed = 1;
		}
	}
	return error == BTRFS_EXISTS ? BTRFS_CORRUPT : error;
}

static size_t
bt_verity_items(size_t length)
{
	return (length + BT_VERITY_ITEM_BYTES - 1U) / BT_VERITY_ITEM_BYTES;
}

static uint8_t *
bt_verity_level(struct btrfs_verity_build *build, unsigned level)
{
	return level == build->verity.levels
	    ? build->verity.descriptor.root_hash
	    : build->blocks + (size_t)level * build->verity.block_size;
}

/* Linux's hash_one_block: the hash of a block joins level's pending block;
 * the root takes exactly one. */
static enum btrfs_result
bt_verity_add_hash(struct btrfs_verity_build *build, unsigned level, const uint8_t *block)
{
	size_t room =
	    level == build->verity.levels ? build->verity.digest_size : build->verity.block_size;

	if (build->filled[level] + build->verity.digest_size > room) {
		return BTRFS_CORRUPT;
	}
	bt_verity_hash(&build->verity, block, bt_verity_level(build, level) + build->filled[level]);
	build->filled[level] += build->verity.digest_size;
	return BTRFS_OK;
}

/* A level's pending block, zero-padded, joins the level above and is stored
 * at its place in the tree (write_merkle_tree_block). */
static enum btrfs_result
bt_verity_flush(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct btrfs_verity_build *build, unsigned level)
{
	const struct bt_verity *verity = &build->verity;
	uint8_t *block = bt_verity_level(build, level);
	enum btrfs_result error;

	bt_zero(block + build->filled[level], verity->block_size - build->filled[level]);
	error = bt_verity_add_hash(build, level + 1, block);
	if (error == BTRFS_OK) {
		error = bt_verity_store(transaction, tree, build->id.inode, BT_VERITY_MERKLE_ITEM,
		    (verity->level_start[level] + build->next[level]) << verity->log_block, block,
		    verity->block_size);
	}
	if (error == BTRFS_OK) {
		build->next[level]++;
		build->filled[level] = 0;
	}
	return error;
}

static unsigned
bt_verity_log2(uint32_t value)
{
	unsigned log = 0;

	while ((UINT32_C(1) << log) < value) {
		log++;
	}
	return log;
}

void
btrfs_verity_build_free(struct btrfs_verity_build *build)
{
	if (build == NULL) {
		return;
	}
	if (build->blocks != NULL) {
		build->release(build->context, build->blocks, build->blocks_size);
	}
	if (build->descriptor != NULL) {
		build->release(build->context, build->descriptor, build->descriptor_size);
	}
	build->release(build->context, build, sizeof(*build));
}

/* fsverity_ioctl_enable's and fsverity_init_merkle_tree_params's refusals of
 * the parameters for a file of size bytes. */
static enum btrfs_result
bt_verity_check_parameters(const struct btrfs_transaction *transaction,
    const struct btrfs_verity_parameters *parameters, uint64_t size, struct bt_verity *verity)
{
	struct bt_disk_verity_descriptor *descriptor = &verity->descriptor;
	enum btrfs_result error;

	bt_zero(verity, sizeof(*verity));
	if ((parameters->algorithm != BTRFS_VERITY_HASH_SHA256 &&
		parameters->algorithm != BTRFS_VERITY_HASH_SHA512) ||
	    parameters->block_size < (UINT32_C(1) << BT_VERITY_LOG_BLOCK_MIN) ||
	    parameters->block_size > transaction->base->info.sector_size) {
		return BTRFS_INVALID_ARGUMENT;
	}
	descriptor->version = BT_VERITY_VERSION;
	descriptor->hash_algorithm = (uint8_t)parameters->algorithm;
	descriptor->log_blocksize = (uint8_t)bt_verity_log2(parameters->block_size);
	descriptor->salt_size = (uint8_t)parameters->salt_size;
	bt_put32(&descriptor->sig_size, (uint32_t)parameters->signature_size);
	bt_put64(&descriptor->data_size, size);
	if (parameters->salt_size != 0) {
		bt_copy(descriptor->salt, parameters->salt, parameters->salt_size);
	}
	error = bt_verity_parameters(verity, transaction->base->info.sector_size);
	if (error == BTRFS_OK &&
	    (verity->tree_size >> verity->log_block) > BT_VERITY_TREE_BLOCKS_MAX) {
		error = BTRFS_CORRUPT;
	}
	/* Only the depth and size of the tree remain to refuse (EFBIG). */
	return error == BTRFS_CORRUPT ? BTRFS_RANGE : error;
}

enum btrfs_result
btrfs_transaction_verity_begin(struct btrfs_transaction *transaction, struct btrfs_object_id id,
    const struct btrfs_verity_parameters *parameters, struct btrfs_verity_build **result)
{
	struct btrfs_verity_build *build;
	struct bt_owned_root view;
	struct bt_owned_root *tree = NULL;
	struct bt_disk_inode item;
	struct bt_key orphan = {
		.objectid = BT_ORPHAN_OBJECTID, .type = BT_ORPHAN_ITEM, .offset = id.inode
	};
	const struct btrfs_environment *environment;
	uint32_t type;
	int present;
	enum btrfs_result error;

	if (result != NULL) {
		*result = NULL;
	}
	if (transaction == NULL || parameters == NULL || result == NULL ||
	    (parameters->salt == NULL && parameters->salt_size != 0) ||
	    (parameters->signature == NULL && parameters->signature_size != 0)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (parameters->block_size == 0 ||
	    (parameters->block_size & (parameters->block_size - 1U)) != 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (parameters->salt_size > BT_VERITY_SALT_MAX ||
	    parameters->signature_size > BT_VERITY_SIGNATURE_MAX) {
		return BTRFS_RANGE;
	}
	if (transaction->failure != BTRFS_OK || transaction->finished) {
		return transaction->failure == BTRFS_OK ? BTRFS_READ_ONLY : transaction->failure;
	}
	/* Write permission first: an immutable file refuses it (EPERM) before
	 * a read-only subvolume does (EROFS). */
	error = bt_tx_tree_view(transaction, id.tree, &view);
	if (error == BTRFS_OK) {
		error = bt_ns_inode(transaction, &view, id.inode, &item);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	if (bt_ns_immutable(&item)) {
		return BTRFS_NOT_PERMITTED;
	}
	error = bt_ns_begin(transaction, id.tree, &tree);
	if (error != BTRFS_OK) {
		return error;
	}
	type = bt_u32(item.mode) & BTRFS_MODE_TYPE;
	if ((bt_u64(item.flags) & BT_INODE_APPEND) != 0) {
		return BTRFS_NOT_PERMITTED;
	}
	if (type != BTRFS_MODE_REGULAR) {
		return type == BTRFS_MODE_DIRECTORY ? BTRFS_IS_DIRECTORY : BTRFS_INVALID_ARGUMENT;
	}
	environment = &transaction->base->env;
	build = environment->allocate(environment->context, sizeof(*build));
	if (build == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(build, sizeof(*build));
	build->allocate = environment->allocate;
	build->release = environment->release;
	build->context = environment->context;
	error =
	    bt_verity_check_parameters(transaction, parameters, bt_u64(item.size), &build->verity);
	if (error == BTRFS_OK && (bt_u64(item.flags) & BT_INODE_RO_VERITY) != 0) {
		error = BTRFS_EXISTS;
	}
	if (error == BTRFS_OK) {
		build->blocks_size = (size_t)build->verity.levels * build->verity.block_size +
		    BT_VERITY_BUILD_WINDOW;
		build->blocks = build->allocate(build->context, build->blocks_size);
		build->descriptor_size =
		    sizeof(build->verity.descriptor) + parameters->signature_size;
		build->descriptor = build->allocate(build->context, build->descriptor_size);
		if (build->blocks == NULL || build->descriptor == NULL) {
			error = BTRFS_NO_MEMORY;
		}
	}
	/* btrfs_begin_enable_verity's orphan item. */
	if (error == BTRFS_OK) {
		error = bt_ns_reserve_items(transaction, id.tree, BT_NS_ORPHAN_ITEMS);
	}
	if (error != BTRFS_OK) {
		btrfs_verity_build_free(build);
		return error;
	}
	if (parameters->signature_size != 0) {
		bt_copy(build->descriptor + sizeof(build->verity.descriptor), parameters->signature,
		    parameters->signature_size);
	}
	build->id = id;
	build->generation = bt_u64(item.generation);
	/* An orphan item the file already has (O_TMPFILE, unlinked while open,
	 * or an older enable) stays, as btrfs_orphan_add accepts EEXIST. */
	error = bt_ns_present(transaction, tree, orphan, &present);
	if (error == BTRFS_OK && !present) {
		error = bt_tx_edit(transaction, &tree->root, orphan, NULL, 0, BT_INSERT);
		transaction->changed = 1;
	}
	if (error != BTRFS_OK) {
		btrfs_verity_build_free(build);
		return bt_ns_poison(transaction, error);
	}
	*result = build;
	return BTRFS_OK;
}

enum btrfs_result
btrfs_transaction_verity_step(struct btrfs_transaction *transaction,
    struct btrfs_verity_build *build, size_t budget, int *done)
{
	struct bt_owned_root *tree = NULL;
	const struct bt_verity *verity;
	uint64_t size;
	uint8_t *window;
	size_t blocks = 0;
	size_t length;
	size_t i;
	unsigned level;
	int cleared;
	enum btrfs_result error;

	if (done != NULL) {
		*done = 0;
	}
	if (transaction == NULL || build == NULL || done == NULL || budget == 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (build->failure != BTRFS_OK) {
		return build->failure;
	}
	error = bt_ns_begin(transaction, build->id.tree, &tree);
	if (error != BTRFS_OK) {
		return error;
	}
	verity = &build->verity;
	size = bt_u64(verity->descriptor.data_size);
	window = build->blocks + (size_t)verity->levels * verity->block_size;
	/* btrfs_begin_enable_verity drops what an earlier enable left. */
	if (!build->cleared) {
		error = bt_ns_drop_verity(transaction, tree, build->id.inode, budget, &cleared);
		build->cleared = error == BTRFS_OK && cleared;
	}
	while (error == BTRFS_OK && build->cleared && blocks < budget && build->position < size) {
		length = BT_VERITY_BUILD_WINDOW / verity->block_size;
		length = (budget - blocks < length ? budget - blocks : length) * verity->block_size;
		if (length > size - build->position) {
			length = (size_t)(size - build->position + verity->block_size - 1U) &
			    ~(size_t)(verity->block_size - 1U);
		}
		error =
		    bt_tx_read(transaction, tree, build->id.inode, build->position, window, length);
		for (i = 0; error == BTRFS_OK && i < length; i += verity->block_size) {
			error = bt_verity_add_hash(build, 0, window + i);
			for (level = 0; error == BTRFS_OK && level < verity->levels &&
			    build->filled[level] + verity->digest_size > verity->block_size;
			    level++) {
				error = bt_verity_flush(transaction, tree, build, level);
			}
			blocks++;
		}
		build->position += length;
	}
	if (error != BTRFS_OK) {
		build->failure = error;
		return bt_ns_poison(transaction, error);
	}
	*done = build->cleared && build->position >= size;
	return BTRFS_OK;
}

enum btrfs_result
btrfs_transaction_verity_finish(
    struct btrfs_transaction *transaction, struct btrfs_verity_build *build)
{
	struct bt_owned_root *tree = NULL;
	struct bt_disk_inode item;
	struct bt_disk_verity_item size_item;
	struct bt_verity check;
	struct bt_key key = { .objectid = build != NULL ? build->id.inode : 0,
		.type = BT_INODE_ITEM };
	struct bt_key orphan = { .objectid = BT_ORPHAN_OBJECTID,
		.type = BT_ORPHAN_ITEM,
		.offset = build != NULL ? build->id.inode : 0 };
	const struct bt_verity *verity;
	uint64_t items = 0;
	unsigned level;
	int present = 0;
	enum btrfs_result error;

	if (transaction == NULL || build == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (build->failure != BTRFS_OK) {
		return build->failure;
	}
	verity = &build->verity;
	if (!build->cleared || build->position < bt_u64(verity->descriptor.data_size)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_ns_begin(transaction, build->id.tree, &tree);
	if (error == BTRFS_OK) {
		error = bt_ns_inode(transaction, tree, build->id.inode, &item);
		error = error == BTRFS_NOT_FOUND ? BTRFS_STALE : error;
	}
	if (error == BTRFS_OK &&
	    (bt_u64(item.size) != bt_u64(verity->descriptor.data_size) ||
		bt_u64(item.generation) != build->generation ||
		(bt_u32(item.mode) & BTRFS_MODE_TYPE) != BTRFS_MODE_REGULAR)) {
		error = BTRFS_STALE;
	}
	if (error == BTRFS_OK && (bt_u64(item.flags) & BT_INODE_RO_VERITY) != 0) {
		error = BTRFS_EXISTS;
	}
	/* The last tree blocks, the size item, the descriptor, and the inode
	 * update with the orphan item. */
	for (level = 0; level < verity->levels; level++) {
		items += build->filled[level] != 0 ? bt_verity_items(verity->block_size) : 0;
	}
	items += 1 + bt_verity_items(build->descriptor_size) + BT_VERITY_FINAL_ITEMS;
	if (error == BTRFS_OK) {
		error = bt_ns_reserve_items(transaction, build->id.tree, items);
	}
	if (error != BTRFS_OK) {
		return error;
	}
	for (level = 0; error == BTRFS_OK && level < verity->levels; level++) {
		if (build->filled[level] != 0) {
			error = bt_verity_flush(transaction, tree, build, level);
		}
	}
	if (error == BTRFS_OK && bt_u64(verity->descriptor.data_size) != 0 &&
	    build->filled[verity->levels] != verity->digest_size) {
		error = BTRFS_CORRUPT;
	}
	/* fsverity_create_info validates the descriptor once more. */
	check = *verity;
	if (error == BTRFS_OK) {
		error = bt_verity_parameters(&check, transaction->base->info.sector_size);
	}
	bt_zero(&size_item, sizeof(size_item));
	bt_put64(&size_item.size, build->descriptor_size);
	bt_copy(build->descriptor, &verity->descriptor, sizeof(verity->descriptor));
	if (error == BTRFS_OK) {
		error = bt_verity_store(transaction, tree, build->id.inode, BT_VERITY_DESC_ITEM, 0,
		    (const uint8_t *)&size_item, sizeof(size_item));
	}
	if (error == BTRFS_OK) {
		error = bt_verity_store(transaction, tree, build->id.inode, BT_VERITY_DESC_ITEM,
		    BT_VERITY_DESCRIPTOR_OFFSET, build->descriptor, build->descriptor_size);
	}
	/* btrfs_update_inode with the flag: no time or version changes. */
	if (error == BTRFS_OK) {
		bt_put64(&item.flags, bt_u64(item.flags) | BT_INODE_RO_VERITY);
		bt_put64(&item.transid, bt_ns_transid(transaction));
		error = bt_tx_edit(transaction, &tree->root, key, &item, sizeof(item), BT_REPLACE);
	}
	/* del_orphan: an inode without links keeps the orphan item of its
	 * unlink or O_TMPFILE; a missing one (ENOENT) is no error. */
	if (error == BTRFS_OK && bt_u32(item.links) != 0) {
		error = bt_ns_present(transaction, tree, orphan, &present);
	}
	if (error == BTRFS_OK && bt_u32(item.links) != 0 && present) {
		error = bt_tx_edit(transaction, &tree->root, orphan, NULL, 0, BT_DELETE);
	}
	if (error == BTRFS_OK) {
		bt_put64(&transaction->super.compat_ro,
		    bt_u64(transaction->super.compat_ro) | BT_COMPAT_RO_VERITY);
		transaction->changed = 1;
		build->failure = BTRFS_READ_ONLY;
		return BTRFS_OK;
	}
	build->failure = error;
	return bt_ns_poison(transaction, error);
}

enum btrfs_result
btrfs_transaction_verity_abort(struct btrfs_transaction *transaction,
    struct btrfs_verity_build *build, size_t budget, int *done)
{
	struct bt_owned_root *tree = NULL;
	struct bt_disk_inode item;
	struct bt_key key = { .objectid = build != NULL ? build->id.inode : 0,
		.type = BT_INODE_ITEM };
	struct bt_key orphan = { .objectid = BT_ORPHAN_OBJECTID,
		.type = BT_ORPHAN_ITEM,
		.offset = build != NULL ? build->id.inode : 0 };
	int present = 0;
	enum btrfs_result error;

	if (done != NULL) {
		*done = 0;
	}
	if (transaction == NULL || build == NULL || done == NULL || budget == 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = bt_ns_begin(transaction, build->id.tree, &tree);
	if (error != BTRFS_OK) {
		return error;
	}
	error = bt_ns_drop_verity(transaction, tree, build->id.inode, budget, done);
	/* rollback_verity clears the flag in an inode update and deletes the
	 * orphan item as del_orphan does; an inode deleted meanwhile took its
	 * orphan item with it. */
	if (error == BTRFS_OK && *done) {
		error = bt_ns_inode(transaction, tree, build->id.inode, &item);
		if (error == BTRFS_OK) {
			bt_put64(&item.flags, bt_u64(item.flags) & ~BT_INODE_RO_VERITY);
			bt_put64(&item.transid, bt_ns_transid(transaction));
			error = bt_tx_edit(
			    transaction, &tree->root, key, &item, sizeof(item), BT_REPLACE);
			transaction->changed = 1;
			if (error == BTRFS_OK && bt_u32(item.links) != 0) {
				error = bt_ns_present(transaction, tree, orphan, &present);
			}
			if (error == BTRFS_OK && bt_u32(item.links) != 0 && present) {
				error = bt_tx_edit(
				    transaction, &tree->root, orphan, NULL, 0, BT_DELETE);
			}
		} else if (error == BTRFS_NOT_FOUND) {
			error = BTRFS_OK;
		}
	}
	if (error == BTRFS_OK && *done) {
		build->failure = BTRFS_READ_ONLY;
	}
	return error == BTRFS_OK ? BTRFS_OK : bt_ns_poison(transaction, error);
}
