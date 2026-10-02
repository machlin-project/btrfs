/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

static uint32_t
bt_count(const uint8_t *block)
{
	const struct bt_disk_header *header = (const void *)block;

	return bt_u32(header->count);
}

static struct bt_key
bt_node_key(const uint8_t *block, uint8_t level, uint32_t slot)
{
	const uint8_t *body = block + sizeof(struct bt_disk_header);
	size_t stride = level == 0 ? sizeof(struct bt_disk_item) : sizeof(struct bt_disk_pointer);

	return bt_key_decode((const void *)(body + slot * stride));
}

/* Checks a node's item count, key order, leaf data bounds and child pointers
 * against its own header level and generation. */
enum btrfs_result
bt_node_items(const struct btrfs_fs *fs, const uint8_t *block)
{
	const struct bt_disk_header *header = (const void *)block;
	const struct bt_disk_item *items = (const void *)(header + 1);
	const struct bt_disk_pointer *pointers = (const void *)(header + 1);
	struct bt_key previous = { 0 };
	struct bt_key key;
	uint64_t generation = bt_u64(header->generation);
	uint32_t count = bt_count(block);
	uint32_t i;
	uint8_t level = header->level;
	size_t body_size = fs->info.node_size - sizeof(*header);
	size_t stride = level == 0 ? sizeof(*items) : sizeof(*pointers);
	size_t start;
	size_t size;
	size_t end = body_size;

	if (count > body_size / stride || (level != 0 && count == 0)) {
		return BTRFS_CORRUPT;
	}
	for (i = 0; i < count; i++) {
		key = bt_node_key(block, level, i);
		if (i != 0 && bt_key_compare(previous, key) >= 0) {
			return BTRFS_CORRUPT;
		}
		previous = key;
		if (level == 0) {
			start = bt_u32(items[i].offset);
			size = bt_u32(items[i].size);
			if (start < count * stride || start > end || size > end - start) {
				return BTRFS_CORRUPT;
			}
			end = start;
		} else if (bt_u64(pointers[i].bytenr) == 0 ||
		    bt_u64(pointers[i].bytenr) % fs->info.sector_size != 0 ||
		    bt_u64(pointers[i].generation) == 0 ||
		    bt_u64(pointers[i].generation) > generation) {
			return BTRFS_CORRUPT;
		}
	}
	return BTRFS_OK;
}

/* built is nonzero only for a transaction's own nodes, which its editor built
 * in memory: their identity and item count are checked here, their items when
 * the transaction seals them, and their checksum when they are written. */
static enum btrfs_result
bt_validate_node(const struct btrfs_fs *fs, struct bt_root root, const uint8_t *block, int built)
{
	const struct bt_disk_header *header = (const void *)block;
	struct bt_le32 checksum;
	uint64_t owner;
	uint32_t count;
	size_t body_size = fs->info.node_size - sizeof(*header);
	size_t stride =
	    root.level == 0 ? sizeof(struct bt_disk_item) : sizeof(struct bt_disk_pointer);

	bt_copy(&checksum, header->csum, sizeof(checksum));
	if ((!built &&
		bt_u32(checksum) !=
		    ~bt_crc32c_block(&fs->node_crc, UINT32_MAX, block + BT_CSUM_SIZE,
			fs->info.node_size - BT_CSUM_SIZE)) ||
	    !bt_equal(header->fsid, fs->metadata_uuid, BTRFS_UUID_SIZE) ||
	    bt_u64(header->bytenr) != root.address || header->level != root.level ||
	    bt_u64(header->generation) != root.generation || root.generation == 0 ||
	    root.generation > fs->info.generation) {
		return BTRFS_CORRUPT;
	}
	owner = bt_u64(header->owner);
	/* Snapshot blocks can retain the originating subvolume's owner. */
	if (bt_file_tree(root.owner) ? !bt_file_tree(owner) : owner != root.owner) {
		return BTRFS_CORRUPT;
	}
	count = bt_count(block);
	if (count > body_size / stride || (root.level != 0 && count == 0)) {
		return BTRFS_CORRUPT;
	}
	return built ? BTRFS_OK : bt_node_items(fs, block);
}

/* Snapshot blocks can retain the originating subvolume's owner. */
static int
bt_owner_matches(struct bt_root root, uint64_t owner)
{
	return bt_file_tree(root.owner) ? bt_file_tree(owner) : owner == root.owner;
}

/* The shared cache holds only nodes committed in this view. */
static struct btrfs_cache *
bt_tree_cache(const struct btrfs_fs *fs, struct bt_root root)
{
	if (root.generation == 0 || root.generation >= fs->cache_limit ||
	    root.generation > fs->info.generation) {
		return NULL;
	}
	return fs->env.cache;
}

static int
bt_tree_address(const struct btrfs_fs *fs, struct bt_root root)
{
	return root.level < BT_MAX_LEVEL && root.address != 0 &&
	    root.address % fs->info.sector_size == 0;
}

/* Reads and verifies a node from the device, storing it in cache. */
static enum btrfs_result
bt_tree_fetch(
    const struct btrfs_fs *fs, struct bt_root root, void *buffer, struct btrfs_cache *cache)
{
	const struct bt_disk_header *header = buffer;
	enum btrfs_result error = BTRFS_CORRUPT;
	uint64_t physical;
	unsigned mirrors = 1;
	unsigned mirror;

	for (mirror = 0; mirror < mirrors; mirror++) {
		error = bt_map(fs, root.address, fs->info.node_size,
		    root.owner == BT_CHUNK_TREE ? BT_BLOCK_SYSTEM : BT_BLOCK_METADATA, mirror,
		    &physical, &mirrors);
		if (error != BTRFS_OK) {
			return error;
		}
		error = bt_read_physical(fs, physical, buffer, fs->info.node_size);
		if (error == BTRFS_OK) {
			error = bt_validate_node(fs, root, buffer, 0);
		}
		if (error == BTRFS_OK && cache != NULL) {
			bt_cache_put(
			    cache, root, fs->info.node_size, buffer, bt_u64(header->owner));
		}
		if (error == BTRFS_OK || (error != BTRFS_CORRUPT && error != BTRFS_IO)) {
			return error;
		}
	}
	return error;
}

/* A transaction's own node, still checked for identity and item count. */
static enum btrfs_result
bt_tree_private(const struct btrfs_fs *fs, struct bt_root root, const uint8_t **node)
{
	enum btrfs_result error;

	if (fs->private_node == NULL || root.generation < fs->cache_limit) {
		return BTRFS_NOT_FOUND;
	}
	error = fs->private_node(fs->private_context, root, node);
	return error == BTRFS_OK ? bt_validate_node(fs, root, *node, 1) : error;
}

enum btrfs_result
bt_tree_read(const struct btrfs_fs *fs, struct bt_root root, void *buffer)
{
	struct btrfs_cache *cache = bt_tree_cache(fs, root);
	const uint8_t *node;
	uint64_t owner = 0;
	enum btrfs_result error;

	if (!bt_tree_address(fs, root)) {
		return BTRFS_CORRUPT;
	}
	error = bt_tree_private(fs, root, &node);
	if (error == BTRFS_OK) {
		bt_copy(buffer, node, fs->info.node_size);
	}
	if (error != BTRFS_NOT_FOUND) {
		return error;
	}
	/* A stored node passed every check that depends only on its bytes. */
	if (cache != NULL && bt_cache_get(cache, root, fs->info.node_size, buffer, &owner)) {
		return bt_owner_matches(root, owner) ? BTRFS_OK : BTRFS_CORRUPT;
	}
	return bt_tree_fetch(fs, root, buffer, cache);
}

void
bt_cursor_init(struct bt_cursor *cursor, const struct btrfs_fs *fs, struct bt_root root)
{
	bt_zero(cursor, sizeof(*cursor));
	cursor->fs = fs;
	cursor->root = root;
	cursor->borrow = fs->private_borrow;
}

void
bt_cursor_fini(struct bt_cursor *cursor)
{
	unsigned i;

	for (i = 0; i < BT_MAX_LEVEL; i++) {
		if (cursor->pins[i] != 0) {
			bt_cache_unpin(cursor->fs->env.cache, cursor->pins[i] - 1);
			cursor->pins[i] = 0;
		}
		if (cursor->owned[i] != NULL) {
			cursor->fs->env.release(
			    cursor->fs->env.context, cursor->owned[i], cursor->fs->info.node_size);
			cursor->owned[i] = NULL;
		}
		cursor->blocks[i] = NULL;
	}
	cursor->valid = 0;
}

static enum btrfs_result
bt_cursor_buffer(struct bt_cursor *cursor, uint8_t level)
{
	const struct btrfs_fs *fs = cursor->fs;

	if (cursor->owned[level] == NULL) {
		cursor->owned[level] = fs->env.allocate(fs->env.context, fs->info.node_size);
		if (cursor->owned[level] == NULL) {
			return BTRFS_NO_MEMORY;
		}
	}
	return BTRFS_OK;
}

static enum btrfs_result
bt_cursor_load(struct bt_cursor *cursor, struct bt_root root)
{
	const struct btrfs_fs *fs = cursor->fs;
	struct btrfs_cache *cache;
	const uint8_t *node;
	struct bt_root *loaded;
	uint64_t owner = 0;
	size_t handle = 0;
	enum btrfs_result error;

	if (root.level >= BT_MAX_LEVEL) {
		return BTRFS_CORRUPT;
	}
	loaded = &cursor->loaded[root.level];
	if (cursor->blocks[root.level] != NULL && loaded->address == root.address &&
	    loaded->generation == root.generation && loaded->owner == root.owner) {
		return BTRFS_OK;
	}
	loaded->address = 0;
	cursor->blocks[root.level] = NULL;
	if (cursor->pins[root.level] != 0) {
		bt_cache_unpin(fs->env.cache, cursor->pins[root.level] - 1);
		cursor->pins[root.level] = 0;
	}
	if (!bt_tree_address(fs, root)) {
		return BTRFS_CORRUPT;
	}
	/* A stored node is read in place instead of copied (see bt_tree_read). */
	cache = bt_tree_cache(fs, root);
	if (cache != NULL) {
		node = bt_cache_pin(cache, root, fs->info.node_size, &owner, &handle, cursor);
		if (node != NULL) {
			cursor->pins[root.level] = handle + 1;
			if (!bt_owner_matches(root, owner)) {
				return BTRFS_CORRUPT;
			}
			cursor->blocks[root.level] = node;
			*loaded = root;
			return BTRFS_OK;
		}
	}
	error = bt_tree_private(fs, root, &node);
	if (error == BTRFS_OK) {
		if (!cursor->borrow) {
			error = bt_cursor_buffer(cursor, root.level);
		}
		if (error == BTRFS_OK && !cursor->borrow) {
			bt_copy(cursor->owned[root.level], node, fs->info.node_size);
			node = cursor->owned[root.level];
		}
	} else if (error == BTRFS_NOT_FOUND) {
		error = bt_cursor_buffer(cursor, root.level);
		if (error == BTRFS_OK) {
			error = bt_tree_fetch(fs, root, cursor->owned[root.level], cache);
			node = cursor->owned[root.level];
		}
	}
	if (error == BTRFS_OK) {
		cursor->blocks[root.level] = node;
		*loaded = root;
	}
	return error;
}

static enum btrfs_result
bt_descend(struct bt_cursor *cursor, unsigned parent)
{
	const struct bt_disk_pointer *pointers;
	struct bt_root child = cursor->root;
	struct bt_key first;
	struct bt_key last;
	struct bt_key bound;
	enum btrfs_result error;
	uint32_t slot = cursor->slots[parent];
	unsigned level;

	pointers = (const void *)(cursor->blocks[parent] + sizeof(struct bt_disk_header));
	child.address = bt_u64(pointers[slot].bytenr);
	child.generation = bt_u64(pointers[slot].generation);
	child.level = (uint8_t)(parent - 1);
	error = bt_cursor_load(cursor, child);
	if (error != BTRFS_OK) {
		return error;
	}
	if (bt_count(cursor->blocks[child.level]) == 0) {
		return BTRFS_CORRUPT;
	}
	first = bt_node_key(cursor->blocks[child.level], child.level, 0);
	last = bt_node_key(
	    cursor->blocks[child.level], child.level, bt_count(cursor->blocks[child.level]) - 1);
	if (bt_key_compare(first, bt_key_decode(&pointers[slot].key)) != 0) {
		return BTRFS_CORRUPT;
	}
	for (level = parent; level <= cursor->root.level; level++) {
		slot = cursor->slots[level];
		if (slot + 1 < bt_count(cursor->blocks[level])) {
			bound = bt_node_key(cursor->blocks[level], (uint8_t)level, slot + 1);
			if (bt_key_compare(last, bound) >= 0) {
				return BTRFS_CORRUPT;
			}
		}
	}
	return BTRFS_OK;
}

static enum btrfs_result
bt_cursor_step(struct bt_cursor *cursor, int forward)
{
	unsigned level;
	uint32_t count;
	enum btrfs_result error;

	if (!cursor->valid) {
		return BTRFS_NOT_FOUND;
	}
	for (level = 0; level <= cursor->root.level; level++) {
		count = bt_count(cursor->blocks[level]);
		if (forward ? cursor->slots[level] + 1 < count : cursor->slots[level] > 0) {
			if (forward) {
				cursor->slots[level]++;
			} else {
				cursor->slots[level]--;
			}
			while (level != 0) {
				error = bt_descend(cursor, level);
				if (error != BTRFS_OK) {
					cursor->valid = 0;
					return error;
				}
				level--;
				cursor->slots[level] =
				    forward ? 0 : bt_count(cursor->blocks[level]) - 1;
			}
			return BTRFS_OK;
		}
	}
	cursor->valid = 0;
	return BTRFS_NOT_FOUND;
}

enum btrfs_result
bt_cursor_seek(struct bt_cursor *cursor, struct bt_key key, int predecessor)
{
	uint32_t base;
	uint32_t half;
	uint32_t count;
	unsigned level = cursor->root.level;
	struct bt_key found;
	enum btrfs_result error;

	cursor->valid = 0;
	error = bt_cursor_load(cursor, cursor->root);
	if (error != BTRFS_OK) {
		return error;
	}
	for (;;) {
		count = bt_count(cursor->blocks[level]);
		if (count == 0) {
			return BTRFS_NOT_FOUND;
		}
		/* The last key at or below key, or slot 0: a branch-free search. */
		base = 0;
		while (count > 1) {
			half = count / 2;
			found = bt_node_key(cursor->blocks[level], (uint8_t)level, base + half);
			base = bt_key_at_most(found, key) ? base + half : base;
			count -= half;
		}
		cursor->slots[level] = base;
		if (level == 0) {
			break;
		}
		error = bt_descend(cursor, level);
		if (error != BTRFS_OK) {
			return error;
		}
		level--;
	}
	cursor->valid = 1;
	found = bt_node_key(cursor->blocks[0], 0, cursor->slots[0]);
	if (predecessor && bt_key_compare(found, key) > 0) {
		return bt_cursor_step(cursor, 0);
	}
	if (!predecessor && bt_key_compare(found, key) < 0) {
		return bt_cursor_step(cursor, 1);
	}
	return BTRFS_OK;
}

enum btrfs_result
bt_cursor_seek_near(struct bt_cursor *cursor, struct bt_key key)
{
	const uint8_t *leaf = cursor->blocks[0];
	uint32_t base = 0;
	uint32_t half;
	uint32_t count;

	if (!cursor->valid || leaf == NULL) {
		return bt_cursor_seek(cursor, key, 0);
	}
	count = bt_count(leaf);
	/* Keys are unique and ordered across leaves, so a key between a leaf's
	 * first and last keys is in that leaf if anywhere. */
	if (count == 0 || !bt_key_at_most(bt_node_key(leaf, 0, 0), key) ||
	    !bt_key_at_most(key, bt_node_key(leaf, 0, count - 1))) {
		return bt_cursor_seek(cursor, key, 0);
	}
	while (count > 1) {
		half = count / 2;
		base = bt_key_at_most(bt_node_key(leaf, 0, base + half), key) ? base + half : base;
		count -= half;
	}
	/* The next key, when base's is below key, is still in this leaf. */
	cursor->slots[0] = bt_key_compare(bt_node_key(leaf, 0, base), key) < 0 ? base + 1 : base;
	return BTRFS_OK;
}

enum btrfs_result
bt_cursor_next(struct bt_cursor *cursor)
{
	return bt_cursor_step(cursor, 1);
}

enum btrfs_result
bt_cursor_record(const struct bt_cursor *cursor, struct bt_record *record)
{
	const struct bt_disk_item *items;
	const struct bt_disk_item *item;
	const uint8_t *body;

	if (!cursor->valid) {
		return BTRFS_NOT_FOUND;
	}
	body = cursor->blocks[0] + sizeof(struct bt_disk_header);
	items = (const void *)body;
	item = &items[cursor->slots[0]];
	record->key = bt_key_decode(&item->key);
	record->data = body + bt_u32(item->offset);
	record->size = bt_u32(item->size);
	return BTRFS_OK;
}

enum btrfs_result
bt_find_root(const struct btrfs_fs *fs, uint64_t tree, struct bt_root *root)
{
	struct bt_cursor cursor;
	struct bt_record record;
	const struct bt_disk_root *disk;
	struct bt_key key = { .objectid = tree, .type = BT_ROOT_ITEM, .offset = UINT64_MAX };
	enum btrfs_result error;

	if (fs->private_root != NULL &&
	    fs->private_root(fs->private_root_context, tree, root, &error)) {
		return error;
	}
	if (fs->selected_tree.owner == tree) {
		*root = fs->selected_tree;
		return BTRFS_OK;
	}
	bt_cursor_init(&cursor, fs, fs->root_tree);
	error = bt_cursor_seek(&cursor, key, 1);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.objectid != tree || record.key.type != BT_ROOT_ITEM) {
			error = BTRFS_NOT_FOUND;
		} else if (record.size < sizeof(*disk)) {
			error = BTRFS_CORRUPT;
		} else {
			disk = (const void *)record.data;
			root->address = bt_u64(disk->bytenr);
			root->generation = bt_u64(disk->generation);
			root->level = disk->level;
			root->owner = tree;
			if (root->level >= BT_MAX_LEVEL || root->generation == 0 ||
			    root->generation > fs->info.generation ||
			    (bt_file_tree(tree) && bt_u64(disk->root_dir) != BTRFS_ROOT_INODE)) {
				error = BTRFS_CORRUPT;
			} else if (bt_file_tree(tree) && bt_u32(disk->refs) == 0) {
				/* A deleted subvolume waiting for the cleaner, as
				 * btrfs_get_fs_root reports it (ENOENT). */
				error = BTRFS_NOT_FOUND;
			}
		}
	}
	bt_cursor_fini(&cursor);
	return error;
}
