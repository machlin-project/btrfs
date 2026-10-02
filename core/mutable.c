/* SPDX-License-Identifier: BSD-3-Clause */
#include "mutable.h"
#include "encode.h"

#define BT_MUTATION_BUCKETS 1024U
#define BT_MUTATION_MAX_NODES 65536U
/* Node-size buffers the view keeps for reuse: cursors and extent copies of
 * one operation allocate and release them in quick succession. */
#define BT_MUTATION_SPARES 16U

/* Sanitizer builds poison a kept buffer, so a use after release still fails. */
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define BT_MUTATION_POISON 1
void __asan_poison_memory_region(void const volatile *address, size_t size);
void __asan_unpoison_memory_region(void const volatile *address, size_t size);
#endif
#endif

struct bt_mutable_node;

struct bt_shadow {
	struct bt_shadow *next;
	struct bt_mutable_node *node;
	uint64_t physical;
};

struct bt_mutable_node {
	struct bt_mutable_node *next;
	struct bt_shadow shadows[2];
	uint8_t *bytes;
	uint64_t address;
	uint64_t original;
	uint64_t original_owner;
	uint64_t original_generation;
	uint64_t original_flags;
	uint8_t original_level;
	int checksum_valid;
	int discarded;
	/* The node has bt_mut_pack's layout: leaf data packed from the end in
	 * item order and every unused body byte zero. */
	int packed;
};

struct bt_edit_record {
	struct bt_key key;
	const void *value;
	size_t length;
};

/* One variable-size item can require three leaves: a large middle item may
 * fit with neither prefix nor suffix. Internal propagation has at most three
 * child pointers and splits into at most two nodes. */
struct bt_mut_split {
	struct bt_mutable_node *nodes[2];
	size_t count;
};

struct bt_mutation {
	const struct btrfs_fs *base;
	struct btrfs_fs view;
	struct bt_mutation_allocator allocator;
	struct bt_mutable_node *logical[BT_MUTATION_BUCKETS];
	struct bt_shadow *physical[BT_MUTATION_BUCKETS];
	struct bt_mutable_node **nodes;
	struct bt_edit_record *records;
	uint8_t *scratch;
	uint8_t *merge_scratch;
	size_t count;
	size_t record_capacity;
	void *spares[BT_MUTATION_SPARES];
	size_t spare_count;
	enum btrfs_result failure;
	int sealed;
	int accepted;
	/* Cursors on the view reading its nodes in place. */
	uint32_t holders;
};

static size_t
bt_mut_hash(uint64_t address)
{
	address ^= address >> 33;
	address *= UINT64_C(0xff51afd7ed558ccd);
	address ^= address >> 33;
	return (size_t)address & (BT_MUTATION_BUCKETS - 1U);
}

static struct bt_disk_header *
bt_mut_header(struct bt_mutable_node *node)
{
	return (void *)node->bytes;
}

static uint32_t
bt_mut_count(struct bt_mutable_node *node)
{
	return bt_u32(bt_mut_header(node)->count);
}

static struct bt_root
bt_mut_root(struct bt_mutable_node *node)
{
	struct bt_disk_header *header = bt_mut_header(node);
	struct bt_root root;

	root.address = node->address;
	root.generation = bt_u64(header->generation);
	root.owner = bt_u64(header->owner);
	root.level = header->level;
	return root;
}

static struct bt_key
bt_mut_key(struct bt_mutable_node *node, uint32_t slot)
{
	struct bt_disk_header *header = bt_mut_header(node);
	size_t stride =
	    header->level == 0 ? sizeof(struct bt_disk_item) : sizeof(struct bt_disk_pointer);

	return bt_key_decode((const void *)(node->bytes + sizeof(*header) + slot * stride));
}

static struct bt_mutable_node *
bt_mut_find_node(struct bt_mutation *mutation, uint64_t address)
{
	struct bt_mutable_node *node;

	for (node = mutation->logical[bt_mut_hash(address)]; node != NULL; node = node->next) {
		if (node->address == address) {
			return node;
		}
	}
	return NULL;
}

static void
bt_mut_checksum(struct bt_mutation *mutation, struct bt_mutable_node *node)
{
	struct bt_disk_header *header = bt_mut_header(node);
	struct bt_le32 checksum;

	if (!node->checksum_valid) {
		bt_zero(header->csum, sizeof(header->csum));
		bt_put32(&checksum,
		    ~bt_crc32c_block(&mutation->view.node_crc, UINT32_MAX,
			node->bytes + BT_CSUM_SIZE, mutation->view.info.node_size - BT_CSUM_SIZE));
		bt_copy(header->csum, &checksum, sizeof(checksum));
		node->checksum_valid = 1;
	}
}

static enum btrfs_result
bt_mut_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct bt_mutation *mutation = context;
	struct bt_shadow *shadow;
	const struct btrfs_environment *env = &mutation->base->env;

	for (shadow = mutation->physical[bt_mut_hash(offset)]; shadow != NULL;
	    shadow = shadow->next) {
		if (shadow->physical == offset) {
			if (length != mutation->view.info.node_size || shadow->node->discarded) {
				return BTRFS_CORRUPT;
			}
			bt_mut_checksum(mutation, shadow->node);
			bt_copy(buffer, shadow->node->bytes, length);
			return BTRFS_OK;
		}
	}
	return env->read(env->context, offset, buffer, length);
}

/* A cursor still reading the nodes in place would see them change: the edit
 * fails the mutation instead. */
static enum btrfs_result
bt_mut_unheld(struct bt_mutation *mutation)
{
	if (__atomic_load_n(&mutation->holders, __ATOMIC_RELAXED) != 0) {
		mutation->failure = BTRFS_INVALID_ARGUMENT;
		return mutation->failure;
	}
	return BTRFS_OK;
}

static enum btrfs_result
bt_mut_private_node(void *context, struct bt_root root, const uint8_t **bytes)
{
	struct bt_mutation *mutation = context;
	struct bt_mutable_node *node = bt_mut_find_node(mutation, root.address);

	if (node == NULL) {
		return BTRFS_NOT_FOUND;
	}
	/* A freed private node is no longer reachable from this view. */
	if (node->discarded) {
		return BTRFS_CORRUPT;
	}
	*bytes = node->bytes;
	return BTRFS_OK;
}

static void *
bt_mut_allocate(void *context, size_t size)
{
	struct bt_mutation *mutation = context;
	const struct btrfs_environment *env = &mutation->base->env;
	void *allocation;

	if (size == mutation->view.info.node_size && mutation->spare_count != 0) {
		allocation = mutation->spares[--mutation->spare_count];
#ifdef BT_MUTATION_POISON
		__asan_unpoison_memory_region(allocation, size);
#endif
		return allocation;
	}
	return env->allocate(env->context, size);
}

static void
bt_mut_release(void *context, void *allocation, size_t size)
{
	struct bt_mutation *mutation = context;
	const struct btrfs_environment *env = &mutation->base->env;

	if (size == mutation->view.info.node_size && mutation->spare_count < BT_MUTATION_SPARES) {
#ifdef BT_MUTATION_POISON
		__asan_poison_memory_region(allocation, size);
#endif
		mutation->spares[mutation->spare_count++] = allocation;
		return;
	}
	env->release(env->context, allocation, size);
}

/* A verified source often already has the packed layout. Recognize it once
 * on CoW and clear only its unused area, so the first edit can move the
 * affected suffix directly. Valid leaves with gaps
 * retain the general unpack/repack path. Never alter the committed source. */
static void
bt_mut_adopt_layout(struct bt_mutation *mutation, struct bt_mutable_node *node)
{
	struct bt_disk_header *header = bt_mut_header(node);
	const struct bt_disk_item *items = (const void *)(header + 1);
	size_t end = mutation->view.info.node_size - sizeof(*header);
	size_t used;
	uint32_t count = bt_mut_count(node);
	uint32_t i;

	if (header->level == 0) {
		for (i = 0; i < count; i++) {
			if ((uint64_t)bt_u32(items[i].offset) + bt_u32(items[i].size) != end) {
				return;
			}
			end = bt_u32(items[i].offset);
		}
		used = count * sizeof(*items);
	} else {
		used = count * sizeof(struct bt_disk_pointer);
	}
	if (used <= end) {
		bt_zero((uint8_t *)(header + 1) + used, end - used);
		node->packed = 1;
	}
}

static enum btrfs_result
bt_mut_new(struct bt_mutation *mutation, struct bt_root root, const void *source, uint64_t original,
    struct bt_mutable_node **result)
{
	struct bt_mutable_node *node;
	struct bt_disk_header *header;
	struct bt_shadow *shadow;
	const struct btrfs_environment *env = &mutation->base->env;
	uint64_t address;
	uint64_t physical[BT_MAX_MIRRORS];
	uint64_t kind = root.owner == BT_CHUNK_TREE ? BT_BLOCK_SYSTEM : BT_BLOCK_METADATA;
	unsigned mirrors = 1;
	unsigned i;
	size_t bucket;
	enum btrfs_result error;

	if (mutation->count == mutation->allocator.node_limit) {
		return BTRFS_NO_SPACE;
	}
	node = env->allocate(env->context, sizeof(*node));
	if (node == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(node, sizeof(*node));
	if (env->cache != NULL) {
		node->bytes = bt_cache_reuse(env->cache, mutation->view.info.node_size, env);
	}
	if (node->bytes == NULL) {
		node->bytes = env->allocate(env->context, mutation->view.info.node_size);
	}
	if (node->bytes == NULL) {
		env->release(env->context, node, sizeof(*node));
		return BTRFS_NO_MEMORY;
	}
	error = mutation->allocator.reserve(
	    mutation->allocator.context, root.owner, root.level, &address);
	/* The allocator may have appended a chunk to the base's chunk map. */
	mutation->view.chunk_count = mutation->base->chunk_count;
	if (error != BTRFS_OK) {
		goto failed;
	}
	if (address == original || address == 0 || address % mutation->view.info.node_size != 0 ||
	    bt_mut_find_node(mutation, address) != NULL) {
		error = BTRFS_CORRUPT;
		goto unreserve;
	}
	for (i = 0; i < mirrors; i++) {
		error = bt_map(mutation->base, address, mutation->view.info.node_size, kind, i,
		    &physical[i], &mirrors);
		if (error != BTRFS_OK) {
			goto unreserve;
		}
		for (shadow = mutation->physical[bt_mut_hash(physical[i])]; shadow != NULL;
		    shadow = shadow->next) {
			if (shadow->physical == physical[i]) {
				error = BTRFS_CORRUPT;
				goto unreserve;
			}
		}
	}
	bt_copy(node->bytes, source, mutation->view.info.node_size);
	header = bt_mut_header(node);
	node->address = address;
	node->original = original;
	node->original_owner = original == 0 ? 0 : bt_u64(header->owner);
	node->original_generation = original == 0 ? 0 : bt_u64(header->generation);
	node->original_flags = original == 0 ? 0 : bt_u64(header->flags);
	node->original_level = header->level;
	bt_put64(&header->bytenr, address);
	bt_put64(&header->owner, root.owner);
	bt_put64(&header->generation, mutation->view.info.generation);
	bt_put64(&header->flags, BT_HEADER_WRITTEN | BT_HEADER_MIXED_BACKREF);
	header->level = root.level;
	if (original != 0) {
		bt_mut_adopt_layout(mutation, node);
	}
	bucket = bt_mut_hash(address);
	node->next = mutation->logical[bucket];
	mutation->logical[bucket] = node;
	for (i = 0; i < mirrors; i++) {
		shadow = &node->shadows[i];
		shadow->node = node;
		shadow->physical = physical[i];
		bucket = bt_mut_hash(physical[i]);
		shadow->next = mutation->physical[bucket];
		mutation->physical[bucket] = shadow;
	}
	mutation->nodes[mutation->count++] = node;
	*result = node;
	return BTRFS_OK;
unreserve:
	mutation->allocator.release(mutation->allocator.context, address);
failed:
	env->release(env->context, node->bytes, mutation->view.info.node_size);
	env->release(env->context, node, sizeof(*node));
	return error;
}

static enum btrfs_result
bt_mut_cow(struct bt_mutation *mutation, struct bt_root root, struct bt_mutable_node **result)
{
	struct bt_mutable_node *node;
	const uint8_t *source;
	size_t handle;
	enum btrfs_result error;

	node = bt_mut_find_node(mutation, root.address);
	if (node != NULL) {
		if (node->discarded || bt_mut_header(node)->level != root.level ||
		    bt_u64(bt_mut_header(node)->owner) != root.owner ||
		    bt_u64(bt_mut_header(node)->generation) != root.generation) {
			return BTRFS_CORRUPT;
		}
		*result = node;
		return BTRFS_OK;
	}
	error = bt_tree_source(mutation->base, root, mutation->scratch, &source, &handle);
	if (error == BTRFS_OK) {
		error = bt_mut_new(mutation, root, source, root.address, result);
	}
	bt_tree_release(mutation->base, handle);
	return error;
}

/* Copies node into buffer and describes its entries as records from first on. */
static size_t
bt_mut_unpack_into(struct bt_mutation *mutation, const uint8_t *node, uint8_t *buffer, size_t first)
{
	const struct bt_disk_header *header;
	const struct bt_disk_item *items;
	const struct bt_disk_pointer *pointers;
	const uint8_t *body;
	struct bt_edit_record *record;
	uint32_t count;
	uint32_t i;

	bt_copy(buffer, node, mutation->view.info.node_size);
	header = (const void *)buffer;
	body = buffer + sizeof(*header);
	items = (const void *)body;
	pointers = (const void *)body;
	count = bt_u32(header->count);
	for (i = 0; i < count; i++) {
		record = &mutation->records[first + i];
		if (header->level == 0) {
			record->key = bt_key_decode(&items[i].key);
			record->value = body + bt_u32(items[i].offset);
			record->length = bt_u32(items[i].size);
		} else {
			record->key = bt_key_decode(&pointers[i].key);
			record->value = &pointers[i].bytenr;
			record->length =
			    sizeof(pointers[i].bytenr) + sizeof(pointers[i].generation);
		}
	}
	return count;
}

static size_t
bt_mut_unpack(struct bt_mutation *mutation, struct bt_mutable_node *node)
{
	return bt_mut_unpack_into(mutation, node->bytes, mutation->scratch, 0);
}

static void
bt_mut_pack(struct bt_mutation *mutation, struct bt_mutable_node *node, size_t first, size_t count)
{
	struct bt_disk_header *header = bt_mut_header(node);
	struct bt_disk_item *items = (void *)(header + 1);
	struct bt_disk_pointer *pointers = (void *)(header + 1);
	struct bt_edit_record *record;
	size_t body_size = mutation->view.info.node_size - sizeof(*header);
	size_t end = body_size;
	size_t i;

	bt_zero(header + 1, body_size);
	for (i = 0; i < count; i++) {
		record = &mutation->records[first + i];
		if (header->level == 0) {
			end -= record->length;
			bt_key_encode(&items[i].key, record->key);
			bt_put32(&items[i].offset, (uint32_t)end);
			bt_put32(&items[i].size, (uint32_t)record->length);
			bt_copy((uint8_t *)(header + 1) + end, record->value, record->length);
		} else {
			bt_key_encode(&pointers[i].key, record->key);
			bt_copy(&pointers[i].bytenr, record->value, record->length);
		}
	}
	bt_put32(&header->count, (uint32_t)count);
	node->checksum_valid = 0;
	node->packed = 1;
}

static size_t
bt_mut_record_size(uint8_t level, const struct bt_edit_record *record)
{
	return level == 0 ? sizeof(struct bt_disk_item) + record->length
			  : sizeof(struct bt_disk_pointer);
}

static enum btrfs_result
bt_mut_rebuild(struct bt_mutation *mutation, struct bt_mutable_node *left, size_t count,
    struct bt_mut_split *result)
{
	struct bt_root root = bt_mut_root(left);
	size_t body_size = mutation->view.info.node_size - sizeof(struct bt_disk_header);
	size_t total = 0;
	size_t prefix = 0;
	size_t split = 0;
	size_t distance = SIZE_MAX;
	size_t difference;
	size_t i;
	size_t second;
	enum btrfs_result error;

	result->count = 0;
	for (i = 0; i < count; i++) {
		total += bt_mut_record_size(root.level, &mutation->records[i]);
	}
	if (total <= body_size) {
		bt_mut_pack(mutation, left, 0, count);
		return BTRFS_OK;
	}
	for (i = 1; i < count; i++) {
		prefix += bt_mut_record_size(root.level, &mutation->records[i - 1]);
		if (prefix <= body_size && total - prefix <= body_size) {
			difference = prefix > total / 2 ? prefix - total / 2 : total / 2 - prefix;
			if (difference < distance) {
				distance = difference;
				split = i;
			}
		}
	}
	if (split == 0) {
		/* The edited leaf contained at most one leaf's worth of records before
		 * insertion/replacement. Greedy packing therefore needs at most three
		 * leaves. Keep the balanced two-way case above for ordinary records. */
		prefix = 0;
		for (split = 0; split < count; split++) {
			difference = bt_mut_record_size(root.level, &mutation->records[split]);
			if (difference > body_size) {
				return BTRFS_RANGE;
			}
			if (difference > body_size - prefix) {
				break;
			}
			prefix += difference;
		}
		prefix = 0;
		for (second = split; second < count; second++) {
			difference = bt_mut_record_size(root.level, &mutation->records[second]);
			if (difference > body_size - prefix) {
				break;
			}
			prefix += difference;
		}
		if (split == 0 || second == split || second == count) {
			return BTRFS_CORRUPT;
		}
		prefix = 0;
		for (i = second; i < count; i++) {
			prefix += bt_mut_record_size(root.level, &mutation->records[i]);
		}
		if (prefix > body_size) {
			return BTRFS_CORRUPT;
		}
		error = bt_mut_new(mutation, root, left->bytes, 0, &result->nodes[0]);
		if (error != BTRFS_OK) {
			return error;
		}
		error = bt_mut_new(mutation, root, left->bytes, 0, &result->nodes[1]);
		if (error != BTRFS_OK) {
			return error;
		}
		bt_mut_pack(mutation, left, 0, split);
		bt_mut_pack(mutation, result->nodes[0], split, second - split);
		bt_mut_pack(mutation, result->nodes[1], second, count - second);
		result->count = 2;
		return BTRFS_OK;
	}
	error = bt_mut_new(mutation, root, left->bytes, 0, &result->nodes[0]);
	if (error != BTRFS_OK) {
		return error;
	}
	bt_mut_pack(mutation, left, 0, split);
	bt_mut_pack(mutation, result->nodes[0], split, count - split);
	result->count = 1;
	return BTRFS_OK;
}

static uint32_t
bt_mut_slot(struct bt_mutable_node *node, struct bt_key key, int predecessor)
{
	uint32_t low = 0;
	uint32_t high = bt_mut_count(node);
	uint32_t middle;
	int comparison;

	while (low < high) {
		middle = low + (high - low) / 2;
		comparison = bt_key_compare(bt_mut_key(node, middle), key);
		if (comparison < 0 || (predecessor && comparison == 0)) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return predecessor && low != 0 ? low - 1 : low;
}

/* Inserts, resizes or deletes the item at slot of a packed leaf in place when
 * the result fits, leaving the bytes bt_mut_pack would write: only the item
 * headers and data after slot move. Returns zero when the leaf must split. */
static int
bt_mut_leaf_in_place(struct bt_mutation *mutation, struct bt_mutable_node *node, uint32_t slot,
    int present, struct bt_key key, const void *value, size_t length, enum bt_edit edit)
{
	struct bt_disk_header *header = bt_mut_header(node);
	struct bt_disk_item *items = (void *)(header + 1);
	uint8_t *body = (uint8_t *)(header + 1);
	size_t body_size = mutation->view.info.node_size - sizeof(*header);
	uint32_t count = bt_u32(header->count);
	uint32_t total = edit == BT_DELETE ? count - 1 : present ? count : count + 1;
	uint32_t i;
	size_t data_end = count == 0 ? body_size : bt_u32(items[count - 1].offset);
	size_t end = slot == 0 ? body_size : bt_u32(items[slot - 1].offset);
	size_t old_length = present ? bt_u32(items[slot].size) : 0;
	size_t new_length = edit == BT_DELETE ? 0 : length;
	size_t tail_end = end - old_length;
	size_t moved;

	if (total * sizeof(*items) > data_end + old_length ||
	    new_length > data_end + old_length - total * sizeof(*items)) {
		return 0;
	}
	moved = data_end + old_length - new_length;
	bt_move(body + moved, body + data_end, tail_end - data_end);
	if (edit == BT_DELETE) {
		bt_move(&items[slot], &items[slot + 1], (count - slot - 1) * sizeof(*items));
		bt_zero(&items[count - 1], sizeof(*items));
	} else if (!present) {
		bt_move(&items[slot + 1], &items[slot], (count - slot) * sizeof(*items));
		bt_key_encode(&items[slot].key, key);
	}
	if (edit != BT_DELETE) {
		bt_put32(&items[slot].offset, (uint32_t)(end - new_length));
		bt_put32(&items[slot].size, (uint32_t)new_length);
		bt_copy(body + end - new_length, value, new_length);
	}
	if (old_length != new_length) {
		for (i = edit == BT_DELETE ? slot : slot + 1; i < total; i++) {
			bt_put32(&items[i].offset,
			    (uint32_t)(bt_u32(items[i].offset) + old_length - new_length));
		}
	}
	if (moved > data_end) {
		bt_zero(body + data_end, moved - data_end);
	}
	bt_put32(&header->count, total);
	node->checksum_valid = 0;
	return 1;
}

static enum btrfs_result
bt_mut_leaf(struct bt_mutation *mutation, struct bt_mutable_node *node, struct bt_key key,
    const void *value, size_t length, enum bt_edit edit, struct bt_mut_split *right)
{
	struct bt_disk_item *items;
	size_t count;
	size_t slot;
	size_t i;
	int present;

	slot = bt_mut_slot(node, key, 0);
	count = bt_mut_count(node);
	present = slot < count && bt_key_compare(bt_mut_key(node, (uint32_t)slot), key) == 0;
	if ((edit == BT_REPLACE || edit == BT_DELETE) && !present) {
		return BTRFS_NOT_FOUND;
	}
	if (edit == BT_INSERT && present) {
		return BTRFS_EXISTS;
	}
	items = (void *)(bt_mut_header(node) + 1);
	if (present && edit != BT_DELETE && bt_u32(items[slot].size) == length) {
		bt_copy((uint8_t *)items + bt_u32(items[slot].offset), value, length);
		node->checksum_valid = 0;
		right->count = 0;
		return BTRFS_OK;
	}
	if (node->packed &&
	    bt_mut_leaf_in_place(
		mutation, node, (uint32_t)slot, present, key, value, length, edit)) {
		right->count = 0;
		return BTRFS_OK;
	}
	(void)bt_mut_unpack(mutation, node);
	if (edit == BT_DELETE) {
		for (i = slot; i + 1 < count; i++) {
			mutation->records[i] = mutation->records[i + 1];
		}
		count--;
	} else {
		if (!present) {
			for (i = count; i > slot; i--) {
				mutation->records[i] = mutation->records[i - 1];
			}
			count++;
		}
		mutation->records[slot] = (struct bt_edit_record){ key, value, length };
	}
	return bt_mut_rebuild(mutation, node, count, right);
}

/* Move a same-size record within one packed leaf. Only the interval between
 * the two slots moves; its payload and headers each move once. Keeping the
 * occupancy constant avoids a transient underfull leaf between delete and
 * insert, and never changes the wire format or the committed source. */
static enum btrfs_result
bt_mut_leaf_rekey(struct bt_mutation *mutation, struct bt_mutable_node *node, struct bt_key old_key,
    struct bt_key new_key, const void *value, size_t length, int *handled)
{
	struct bt_disk_header *header = bt_mut_header(node);
	struct bt_disk_item *items = (void *)(header + 1);
	uint8_t *body = (uint8_t *)(header + 1);
	size_t body_size = mutation->view.info.node_size - sizeof(*header);
	size_t old_offset;
	size_t offset;
	size_t end;
	uint32_t count = bt_mut_count(node);
	uint32_t from = bt_mut_slot(node, old_key, 0);
	uint32_t to;
	uint32_t i;

	*handled = 1;
	if (from == count || bt_key_compare(bt_mut_key(node, from), old_key) != 0) {
		return BTRFS_NOT_FOUND;
	}
	if (!node->packed || bt_u32(items[from].size) != length) {
		*handled = 0;
		return BTRFS_OK;
	}
	to = bt_mut_slot(node, new_key, 0);
	if (to < count && bt_key_compare(bt_mut_key(node, to), new_key) == 0) {
		return BTRFS_EXISTS;
	}
	to -= to > from;
	old_offset = bt_u32(items[from].offset);
	offset = old_offset;
	if (to > from) {
		offset = bt_u32(items[to].offset);
		if (length != 0) {
			bt_move(body + offset + length, body + offset, old_offset - offset);
		}
		bt_move(items + from, items + from + 1, (to - from) * sizeof(*items));
		if (length != 0) {
			for (i = from; i < to; i++) {
				bt_put32(
				    &items[i].offset, bt_u32(items[i].offset) + (uint32_t)length);
			}
		}
	} else if (to < from) {
		end = to == 0 ? body_size : bt_u32(items[to - 1].offset);
		offset = end - length;
		if (length != 0) {
			bt_move(body + old_offset, body + old_offset + length,
			    end - old_offset - length);
		}
		bt_move(items + to + 1, items + to, (from - to) * sizeof(*items));
		if (length != 0) {
			for (i = to + 1; i <= from; i++) {
				bt_put32(
				    &items[i].offset, bt_u32(items[i].offset) - (uint32_t)length);
			}
		}
	}
	bt_key_encode(&items[to].key, new_key);
	bt_put32(&items[to].offset, (uint32_t)offset);
	bt_put32(&items[to].size, (uint32_t)length);
	bt_copy(body + offset, value, length);
	node->checksum_valid = 0;
	return BTRFS_OK;
}

/* A sibling absorbed into the edited node (right), or the sibling that absorbed
 * it (left), as decided before the parent is updated. */
struct bt_mut_merge {
	struct bt_mutable_node *left;
	int right;
};

static size_t
bt_mut_used(const uint8_t *bytes)
{
	const struct bt_disk_header *header = (const void *)bytes;
	const struct bt_disk_item *items = (const void *)(header + 1);
	uint32_t count = bt_u32(header->count);
	size_t used = 0;
	uint32_t i;

	if (header->level != 0) {
		return count * sizeof(struct bt_disk_pointer);
	}
	for (i = 0; i < count; i++) {
		used += sizeof(*items) + bt_u32(items[i].size);
	}
	return used;
}

/* Packed leaves put the last item's payload at the bottom of the data area.
 * Its offset therefore gives the total occupancy without summing every item
 * after every edit. Linux-authored leaves need not have this layout. */
static size_t
bt_mut_node_used(struct bt_mutation *mutation, struct bt_mutable_node *node)
{
	const struct bt_disk_header *header = bt_mut_header(node);
	const struct bt_disk_item *items = (const void *)(header + 1);
	uint32_t count = bt_mut_count(node);

	if (header->level != 0 || !node->packed) {
		return bt_mut_used(node->bytes);
	}
	return count == 0 ? 0
			  : count * sizeof(*items) + mutation->view.info.node_size -
		sizeof(*header) - bt_u32(items[count - 1].offset);
}

/* Linux rebalances leaves below a third and nodes below a quarter of capacity. */
static int
bt_mut_underfull(struct bt_mutation *mutation, struct bt_mutable_node *node)
{
	size_t capacity = mutation->view.info.node_size - sizeof(struct bt_disk_header);

	return bt_mut_header(node)->level == 0
	    ? bt_mut_node_used(mutation, node) < capacity / 3
	    : bt_mut_count(node) < capacity / sizeof(struct bt_disk_pointer) / 4;
}

/* Merges an underfull edited node with its right sibling, else its left one,
 * when both fit in one node. The sibling is CoWed like any edited block. */
static enum btrfs_result
bt_mut_merge(struct bt_mutation *mutation, struct bt_mutable_node *parent, uint32_t slot,
    struct bt_mutable_node *child, struct bt_mut_merge *merge)
{
	struct bt_disk_pointer *pointers = (void *)(bt_mut_header(parent) + 1);
	struct bt_mutable_node *sibling;
	struct bt_root root;
	const uint8_t *bytes;
	size_t capacity = mutation->view.info.node_size - sizeof(struct bt_disk_header);
	size_t handle;
	size_t first;
	size_t count;
	uint32_t index;
	int side;
	enum btrfs_result error;

	merge->left = NULL;
	merge->right = 0;
	if (bt_mut_count(child) == 0 || !bt_mut_underfull(mutation, child)) {
		return BTRFS_OK;
	}
	for (side = 1; side >= 0; side--) {
		if (side ? slot + 1 >= bt_mut_count(parent) : slot == 0) {
			continue;
		}
		index = side ? slot + 1 : slot - 1;
		root = bt_mut_root(child);
		root.address = bt_u64(pointers[index].bytenr);
		root.generation = bt_u64(pointers[index].generation);
		sibling = bt_mut_find_node(mutation, root.address);
		handle = 0;
		if (sibling != NULL) {
			bytes = sibling->bytes;
		} else {
			error = bt_tree_source(
			    mutation->base, root, mutation->merge_scratch, &bytes, &handle);
			if (error != BTRFS_OK) {
				bt_tree_release(mutation->base, handle);
				return error;
			}
		}
		if ((sibling == NULL ? bt_mut_used(bytes) : bt_mut_node_used(mutation, sibling)) +
			bt_mut_node_used(mutation, child) >
		    capacity) {
			bt_tree_release(mutation->base, handle);
			continue;
		}
		if (sibling == NULL) {
			error = bt_mut_new(mutation, root, bytes, root.address, &sibling);
		} else {
			error = bt_mut_cow(mutation, root, &sibling);
		}
		bt_tree_release(mutation->base, handle);
		if (error != BTRFS_OK) {
			return error;
		}
		if (bt_mut_count(sibling) == 0 ||
		    bt_key_compare(bt_mut_key(sibling, 0), bt_key_decode(&pointers[index].key)) !=
			0) {
			return BTRFS_CORRUPT;
		}
		/* Entries keep their order: the left node's then the right node's. */
		first = bt_mut_unpack(mutation, side ? child : sibling);
		count = bt_mut_unpack_into(
		    mutation, side ? sibling->bytes : child->bytes, mutation->merge_scratch, first);
		if (bt_key_compare(
			mutation->records[first - 1].key, mutation->records[first].key) >= 0) {
			return BTRFS_CORRUPT;
		}
		bt_mut_pack(mutation, side ? child : sibling, 0, first + count);
		bt_mut_pack(mutation, side ? sibling : child, 0, 0);
		if (side) {
			sibling->discarded = 1;
			merge->right = 1;
		} else {
			merge->left = sibling;
		}
		return BTRFS_OK;
	}
	return BTRFS_OK;
}

static enum btrfs_result
bt_mut_parent(struct bt_mutation *mutation, struct bt_mutable_node *parent, uint32_t slot,
    struct bt_mutable_node *left, const struct bt_mut_split *right,
    const struct bt_mut_merge *merge, struct bt_mut_split *split)
{
	struct bt_disk_pointer children[4];
	struct bt_disk_pointer *pointers;
	struct bt_mutable_node *child;
	size_t count;
	size_t i;
	size_t j;

	if (bt_mut_count(left) != 0 && right->count == 0 && merge->left == NULL && !merge->right) {
		pointers = (void *)(bt_mut_header(parent) + 1);
		bt_key_encode(&pointers[slot].key, bt_mut_key(left, 0));
		bt_put64(&pointers[slot].bytenr, left->address);
		bt_put64(&pointers[slot].generation, mutation->view.info.generation);
		parent->checksum_valid = 0;
		split->count = 0;
		return BTRFS_OK;
	}
	count = bt_mut_unpack(mutation, parent);
	if (merge->right) {
		for (i = slot + 1; i + 1 < count; i++) {
			mutation->records[i] = mutation->records[i + 1];
		}
		count--;
	}
	if (bt_mut_count(left) == 0) {
		left->discarded = 1;
		for (i = slot; i + 1 < count; i++) {
			mutation->records[i] = mutation->records[i + 1];
		}
		count--;
	} else {
		for (i = count; i > slot + 1; i--) {
			mutation->records[i - 1 + right->count] = mutation->records[i - 1];
		}
		for (j = 0; j <= right->count; j++) {
			child = j == 0 ? left : right->nodes[j - 1];
			bt_put64(&children[j].bytenr, child->address);
			bt_put64(&children[j].generation, mutation->view.info.generation);
			mutation->records[slot + j] =
			    (struct bt_edit_record){ bt_mut_key(child, 0), &children[j].bytenr,
				    sizeof(children[j].bytenr) + sizeof(children[j].generation) };
		}
		count += right->count;
	}
	if (merge->left != NULL) {
		bt_put64(&children[3].bytenr, merge->left->address);
		bt_put64(&children[3].generation, mutation->view.info.generation);
		mutation->records[slot - 1] =
		    (struct bt_edit_record){ bt_mut_key(merge->left, 0), &children[3].bytenr,
			    sizeof(children[3].bytenr) + sizeof(children[3].generation) };
	}
	return bt_mut_rebuild(mutation, parent, count, split);
}

static enum btrfs_result
bt_mutation_apply(struct bt_mutation *mutation, struct bt_root *root, struct bt_key key,
    const void *value, size_t length, enum bt_edit edit, const struct bt_key *replacement)
{
	struct bt_mutable_node *path[BT_MAX_LEVEL];
	uint32_t slots[BT_MAX_LEVEL];
	struct bt_mutable_node *node;
	struct bt_mut_split right = { 0 };
	struct bt_mut_split next_right;
	struct bt_mut_merge merge;
	struct bt_mutable_node *new_root;
	struct bt_mutable_node *child;
	struct bt_disk_pointer *pointers;
	struct bt_disk_pointer children[3];
	struct bt_root current;
	struct bt_key first = { 0 };
	struct bt_key upper = { 0 };
	size_t i;
	uint8_t level;
	uint8_t top;
	int has_upper = 0;
	int handled = 0;
	int insert_after = 0;
	enum btrfs_result error;

	if (mutation == NULL || root == NULL || edit < BT_INSERT || edit > BT_DELETE ||
	    root->level >= BT_MAX_LEVEL || (value == NULL && length != 0) ||
	    length > mutation->view.info.node_size - sizeof(struct bt_disk_header) -
		    sizeof(struct bt_disk_item)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (mutation->sealed || mutation->failure != BTRFS_OK) {
		return mutation->failure == BTRFS_OK ? BTRFS_READ_ONLY : mutation->failure;
	}
	error = bt_mut_unheld(mutation);
	if (error != BTRFS_OK) {
		return error;
	}
	current = *root;
	top = current.level;
	for (level = top;; level--) {
		error = bt_mut_cow(mutation, current, &node);
		if (error != BTRFS_OK) {
			goto failed;
		}
		path[level] = node;
		if (level < top) {
			if (bt_mut_count(node) == 0 ||
			    bt_key_compare(first, bt_mut_key(node, 0)) != 0 ||
			    (has_upper &&
				bt_key_compare(bt_mut_key(node, bt_mut_count(node) - 1), upper) >=
				    0)) {
				error = BTRFS_CORRUPT;
				goto failed;
			}
		}
		if (level == 0) {
			break;
		}
		slots[level] = bt_mut_slot(node, key, 1);
		pointers = (void *)(bt_mut_header(node) + 1);
		first = bt_key_decode(&pointers[slots[level]].key);
		if (slots[level] + 1 < bt_mut_count(node)) {
			upper = bt_key_decode(&pointers[slots[level] + 1].key);
			has_upper = 1;
		}
		current.address = bt_u64(pointers[slots[level]].bytenr);
		current.generation = bt_u64(pointers[slots[level]].generation);
		current.level = level - 1;
	}
	if (replacement != NULL && bt_key_compare(*replacement, key) != 0) {
		/* Restrict the direct move to this leaf's existing parent interval.
		 * Crossing it follows the ordinary two bounded editor descents. */
		if (bt_mut_count(path[0]) != 0 &&
		    bt_key_compare(*replacement, bt_mut_key(path[0], 0)) >= 0 &&
		    (!has_upper || bt_key_compare(*replacement, upper) < 0)) {
			error = bt_mut_leaf_rekey(
			    mutation, path[0], key, *replacement, value, length, &handled);
		}
		if (!handled) {
			error = bt_mut_leaf(mutation, path[0], key, NULL, 0, BT_DELETE, &right);
			insert_after = 1;
		}
	} else {
		error = bt_mut_leaf(mutation, path[0], key, value, length, edit, &right);
	}
	if (error != BTRFS_OK) {
		goto failed;
	}
	for (level = 1; level <= top; level++) {
		merge.left = NULL;
		merge.right = 0;
		if (right.count == 0) {
			error = bt_mut_merge(
			    mutation, path[level], slots[level], path[level - 1], &merge);
			if (error != BTRFS_OK) {
				goto failed;
			}
		}
		error = bt_mut_parent(mutation, path[level], slots[level], path[level - 1], &right,
		    &merge, &next_right);
		if (error != BTRFS_OK) {
			goto failed;
		}
		right = next_right;
	}
	node = path[top];
	if (right.count != 0) {
		if (top + 1 == BT_MAX_LEVEL) {
			error = BTRFS_NO_SPACE;
			goto failed;
		}
		current = bt_mut_root(node);
		current.level++;
		error = bt_mut_new(mutation, current, node->bytes, 0, &new_root);
		if (error != BTRFS_OK) {
			goto failed;
		}
		for (i = 0; i <= right.count; i++) {
			child = i == 0 ? node : right.nodes[i - 1];
			bt_put64(&children[i].bytenr, child->address);
			bt_put64(&children[i].generation, mutation->view.info.generation);
			mutation->records[i] =
			    (struct bt_edit_record){ bt_mut_key(child, 0), &children[i].bytenr,
				    sizeof(children[i].bytenr) + sizeof(children[i].generation) };
		}
		bt_mut_pack(mutation, new_root, 0, right.count + 1);
		node = new_root;
	}
	while (bt_mut_header(node)->level != 0 && bt_mut_count(node) == 1) {
		current = bt_mut_root(node);
		pointers = (void *)(bt_mut_header(node) + 1);
		current.address = bt_u64(pointers[0].bytenr);
		current.generation = bt_u64(pointers[0].generation);
		current.level--;
		node->discarded = 1;
		error = bt_mut_cow(mutation, current, &node);
		if (error != BTRFS_OK) {
			goto failed;
		}
	}
	if (bt_mut_count(node) == 0) {
		bt_mut_header(node)->level = 0;
		node->checksum_valid = 0;
		node->packed = 0;
	}
	*root = bt_mut_root(node);
	return insert_after
	    ? bt_mutation_apply(mutation, root, *replacement, value, length, BT_INSERT, NULL)
	    : BTRFS_OK;
failed:
	mutation->failure = error;
	return error;
}

enum btrfs_result
bt_mutation_edit(struct bt_mutation *mutation, struct bt_root *root, struct bt_key key,
    const void *value, size_t length, enum bt_edit edit)
{
	return bt_mutation_apply(mutation, root, key, value, length, edit, NULL);
}

enum btrfs_result
bt_mutation_rekey(struct bt_mutation *mutation, struct bt_root *root, struct bt_key old_key,
    struct bt_key new_key, const void *value, size_t length)
{
	return bt_mutation_apply(mutation, root, old_key, value, length, BT_REPLACE, &new_key);
}

enum btrfs_result
bt_mutation_create(const struct btrfs_fs *base, const struct bt_mutation_allocator *allocator,
    struct bt_mutation **result)
{
	struct bt_mutation *mutation;
	const struct btrfs_environment *env;

	if (result == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (base == NULL || allocator == NULL || allocator->reserve == NULL ||
	    allocator->release == NULL || allocator->node_limit == 0 ||
	    allocator->node_limit > BT_MUTATION_MAX_NODES || base->info.generation == UINT64_MAX) {
		return BTRFS_INVALID_ARGUMENT;
	}
	env = &base->env;
	mutation = env->allocate(env->context, sizeof(*mutation));
	if (mutation == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(mutation, sizeof(*mutation));
	mutation->base = base;
	mutation->allocator = *allocator;
	mutation->view = *base;
	mutation->view.info.generation++;
	mutation->view.cache_limit = mutation->view.info.generation;
	mutation->view.env.context = mutation;
	mutation->view.env.read = bt_mut_read;
	mutation->view.env.allocate = bt_mut_allocate;
	mutation->view.env.release = bt_mut_release;
	mutation->view.private_node = bt_mut_private_node;
	mutation->view.private_borrow = 1;
	mutation->view.private_holders = &mutation->holders;
	mutation->view.private_context = mutation;
	/* This view supports private metadata traversal; file codec context stays with its adapter.
	 */
	mutation->view.env.decompress = NULL;
	mutation->record_capacity =
	    (base->info.node_size - sizeof(struct bt_disk_header)) / sizeof(struct bt_disk_item) +
	    2;
	mutation->nodes =
	    env->allocate(env->context, allocator->node_limit * sizeof(*mutation->nodes));
	mutation->scratch = env->allocate(env->context, base->info.node_size);
	mutation->merge_scratch = env->allocate(env->context, base->info.node_size);
	mutation->records =
	    env->allocate(env->context, mutation->record_capacity * sizeof(*mutation->records));
	if (mutation->nodes == NULL || mutation->scratch == NULL ||
	    mutation->merge_scratch == NULL || mutation->records == NULL) {
		bt_mutation_destroy(mutation);
		return BTRFS_NO_MEMORY;
	}
	*result = mutation;
	return BTRFS_OK;
}

enum btrfs_result
bt_mutation_new_root(struct bt_mutation *mutation, struct bt_root source, uint64_t owner, int copy,
    struct bt_root *result)
{
	struct bt_mutable_node *node;
	struct bt_disk_header *header;
	struct bt_root created = { 0, 0, owner, 0 };
	enum btrfs_result error = BTRFS_OK;

	if (mutation == NULL || result == NULL || owner == 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (mutation->failure != BTRFS_OK) {
		return mutation->failure;
	}
	node = bt_mut_find_node(mutation, source.address);
	if (node != NULL) {
		if (node->discarded) {
			error = BTRFS_CORRUPT;
		} else {
			bt_copy(mutation->scratch, node->bytes, mutation->view.info.node_size);
		}
	} else {
		error = bt_tree_read(mutation->base, source, mutation->scratch);
	}
	if (error == BTRFS_OK) {
		header = (struct bt_disk_header *)mutation->scratch;
		if (copy) {
			created.level = header->level;
		} else {
			bt_zero(header + 1, mutation->view.info.node_size - sizeof(*header));
			bt_put32(&header->count, 0);
		}
		error = bt_mut_new(mutation, created, mutation->scratch, 0, &node);
	}
	if (error != BTRFS_OK) {
		mutation->failure = error;
		return error;
	}
	*result =
	    (struct bt_root){ node->address, mutation->view.info.generation, owner, created.level };
	return BTRFS_OK;
}

/* Exact lookups whose entire path is private need no cursor buffers or pins:
 * the single writer copies the value before another edit can run. Preserve
 * identity, parent first-key and ancestor upper-bound checks. An untouched
 * node falls back to the ordinary verified cursor, never to raw disk bytes. */
static enum btrfs_result
bt_mut_find_private(struct bt_mutation *mutation, struct bt_root root, struct bt_key key,
    struct bt_record *record, int *handled)
{
	struct bt_mutable_node *node;
	const struct bt_disk_header *header;
	const struct bt_disk_item *items;
	const struct bt_disk_pointer *pointers;
	struct bt_key first = { 0 };
	struct bt_key upper = { 0 };
	uint8_t top = root.level;
	uint32_t count;
	uint32_t slot;
	size_t stride;
	int has_upper = 0;

	*handled = 1;
	if (root.level >= BT_MAX_LEVEL) {
		return BTRFS_CORRUPT;
	}
	/* At most BT_MAX_LEVEL nodes; root.level decreases at each step. */
	for (;;) {
		node = bt_mut_find_node(mutation, root.address);
		if (node == NULL) {
			*handled = 0;
			return BTRFS_OK;
		}
		header = bt_mut_header(node);
		count = bt_mut_count(node);
		stride = root.level == 0 ? sizeof(*items) : sizeof(*pointers);
		if (node->discarded || header->level != root.level ||
		    (bt_file_tree(root.owner) ? !bt_file_tree(bt_u64(header->owner))
					      : bt_u64(header->owner) != root.owner) ||
		    bt_u64(header->generation) != root.generation ||
		    root.generation != mutation->view.info.generation ||
		    !bt_equal(header->fsid, mutation->view.metadata_uuid, BTRFS_UUID_SIZE) ||
		    count > (mutation->view.info.node_size - sizeof(*header)) / stride ||
		    (count == 0 && root.level != 0)) {
			return BTRFS_CORRUPT;
		}
		if (root.level < top &&
		    (count == 0 || bt_key_compare(first, bt_mut_key(node, 0)) != 0 ||
			(has_upper && bt_key_compare(bt_mut_key(node, count - 1), upper) >= 0))) {
			return BTRFS_CORRUPT;
		}
		slot = bt_mut_slot(node, key, root.level != 0);
		if (root.level == 0) {
			if (slot == count || bt_key_compare(bt_mut_key(node, slot), key) != 0) {
				return BTRFS_NOT_FOUND;
			}
			items = (const void *)(header + 1);
			record->key = key;
			record->data = (const uint8_t *)(header + 1) + bt_u32(items[slot].offset);
			record->size = bt_u32(items[slot].size);
			return BTRFS_OK;
		}
		pointers = (const void *)(header + 1);
		first = bt_key_decode(&pointers[slot].key);
		if (slot + 1 < count) {
			upper = bt_key_decode(&pointers[slot + 1].key);
			has_upper = 1;
		}
		root.address = bt_u64(pointers[slot].bytenr);
		root.generation = bt_u64(pointers[slot].generation);
		root.level--;
	}
}

enum btrfs_result
bt_mutation_find(struct bt_mutation *mutation, struct bt_root root, struct bt_key key, void *value,
    size_t capacity, size_t *length)
{
	struct bt_cursor cursor;
	struct bt_record record;
	int handled;
	enum btrfs_result error;

	if (length == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*length = 0;
	if (mutation == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (mutation->failure != BTRFS_OK) {
		return mutation->failure;
	}
	error = bt_mut_find_private(mutation, root, key, &record, &handled);
	if (!handled) {
		bt_cursor_init(&cursor, &mutation->view, root);
		error = bt_cursor_seek(&cursor, key, 0);
		if (error == BTRFS_OK) {
			(void)bt_cursor_record(&cursor, &record);
		}
	}
	if (error == BTRFS_OK) {
		if (bt_key_compare(record.key, key) != 0) {
			error = BTRFS_NOT_FOUND;
		} else {
			*length = record.size;
			if (value != NULL) {
				if (capacity < record.size) {
					error = BTRFS_RANGE;
				} else {
					bt_copy(value, record.data, record.size);
				}
			}
		}
	}
	if (!handled) {
		bt_cursor_fini(&cursor);
	}
	return error;
}

const struct btrfs_fs *
bt_mutation_view(const struct bt_mutation *mutation)
{
	return mutation == NULL || mutation->failure != BTRFS_OK ? NULL : &mutation->view;
}

size_t
bt_mutation_count(const struct bt_mutation *mutation)
{
	return mutation == NULL ? 0 : mutation->count;
}

enum btrfs_result
bt_mutation_block(struct bt_mutation *mutation, size_t index, struct bt_mutated_block *block)
{
	struct bt_mutable_node *node;
	struct bt_disk_header *header;

	if (mutation == NULL || block == NULL || index >= mutation->count) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (mutation->failure != BTRFS_OK) {
		return mutation->failure;
	}
	node = mutation->nodes[index];
	header = bt_mut_header(node);
	/* Accounting reads blocks while it still edits them; checksums are
	 * computed once, when sealing. */
	if (mutation->sealed) {
		bt_mut_checksum(mutation, node);
	}
	block->address = node->address;
	block->original_address = node->original;
	block->owner = bt_u64(header->owner);
	block->original_owner = node->original_owner;
	block->original_generation = node->original_generation;
	block->original_flags = node->original_flags;
	block->original_level = node->original_level;
	block->level = header->level;
	block->discarded = node->discarded;
	block->bytes = node->bytes;
	block->size = mutation->view.info.node_size;
	return BTRFS_OK;
}

enum btrfs_result
bt_mutation_seal(struct bt_mutation *mutation)
{
	enum btrfs_result error;
	size_t i;

	if (mutation == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (mutation->failure != BTRFS_OK) {
		return mutation->failure;
	}
	error = bt_mut_unheld(mutation);
	if (error != BTRFS_OK) {
		return error;
	}
	/* Reads within the transaction trust its own nodes' items; every node it
	 * will write is checked once here instead. */
	for (i = 0; i < mutation->count; i++) {
		if (mutation->nodes[i]->discarded) {
			continue;
		}
		error = bt_node_items(&mutation->view, mutation->nodes[i]->bytes);
		if (error != BTRFS_OK) {
			mutation->failure = error;
			return error;
		}
	}
	for (i = 0; i < mutation->count; i++) {
		bt_mut_checksum(mutation, mutation->nodes[i]);
	}
	mutation->sealed = 1;
	return BTRFS_OK;
}

enum btrfs_result
bt_mutation_accept(struct bt_mutation *mutation)
{
	size_t i;

	if (mutation == NULL || !mutation->sealed || mutation->accepted) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (mutation->failure != BTRFS_OK) {
		return mutation->failure;
	}
	for (i = 0; i < mutation->count; i++) {
		if (mutation->nodes[i]->discarded) {
			mutation->allocator.release(
			    mutation->allocator.context, mutation->nodes[i]->address);
		}
	}
	mutation->accepted = 1;
	return BTRFS_OK;
}

void
bt_mutation_destroy(struct bt_mutation *mutation)
{
	const struct btrfs_environment *env;
	struct bt_mutable_node *node;
	size_t i;

	if (mutation == NULL) {
		return;
	}
	env = &mutation->base->env;
	while (mutation->spare_count != 0) {
		mutation->spare_count--;
#ifdef BT_MUTATION_POISON
		__asan_unpoison_memory_region(
		    mutation->spares[mutation->spare_count], mutation->view.info.node_size);
#endif
		env->release(env->context, mutation->spares[mutation->spare_count],
		    mutation->view.info.node_size);
	}
	for (i = 0; i < mutation->count; i++) {
		node = mutation->nodes[i];
		if (!mutation->accepted) {
			mutation->allocator.release(mutation->allocator.context, node->address);
		} else if (!node->discarded && env->cache != NULL) {
			/* The published buffer outlives the transaction in the cache.
			 * Transfer at destruction, after every private view has retired. */
			bt_cache_take(env->cache, bt_mut_root(node), mutation->view.info.node_size,
			    &node->bytes, bt_mut_root(node).owner, env);
		}
		if (node->bytes != NULL) {
			env->release(env->context, node->bytes, mutation->view.info.node_size);
		}
		env->release(env->context, node, sizeof(*node));
	}
	if (mutation->records != NULL) {
		env->release(env->context, mutation->records,
		    mutation->record_capacity * sizeof(*mutation->records));
	}
	if (mutation->scratch != NULL) {
		env->release(env->context, mutation->scratch, mutation->view.info.node_size);
	}
	if (mutation->merge_scratch != NULL) {
		env->release(env->context, mutation->merge_scratch, mutation->view.info.node_size);
	}
	if (mutation->nodes != NULL) {
		env->release(env->context, mutation->nodes,
		    mutation->allocator.node_limit * sizeof(*mutation->nodes));
	}
	env->release(env->context, mutation, sizeof(*mutation));
}
