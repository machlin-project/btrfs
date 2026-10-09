/* SPDX-License-Identifier: BSD-3-Clause */
/* fs-verity as Linux's fs/verity and fs/btrfs/verity.c keep it. A file's
 * descriptor and Merkle tree live in VERITY_DESC and VERITY_MERKLE items of
 * its tree, keyed by byte offset. Reads hash every data block, zero-padded to
 * the tree's block size and with the salt before it, and verify the chain of
 * tree blocks up to the descriptor's root hash; the tree blocks one read has
 * verified stay in use, one per level, as Linux's verified-block marks do. */
#include "encode.h"
#include "verity.h"

/* File bytes one verification pass reads: whole blocks of every tree block
 * size up to the largest sector. */
#define BT_VERITY_WINDOW (64U * 1024U)
#define BT_VERITY_LOG_SHA256 5U
#define BT_VERITY_LOG_SHA512 6U
#define BT_VERITY_LOG_BLOCK_LIMIT 31U

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
