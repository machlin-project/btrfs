/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

/* Ways per set: a lookup compares at most this many entries. */
#define BT_CACHE_WAYS 8U
/* A set's worth of buffers can move from retired cache entries into the next
 * private transaction. They are included in the configured node-byte budget. */
#define BT_CACHE_SPARES BT_CACHE_WAYS
/* Pin counters per entry. Every lookup pins the roots of the trees it walks,
 * so one shared counter per entry would bounce between processors; a lookup
 * uses the stripe its caller's stack address selects. */
#define BT_CACHE_STRIPES 16U
#define BT_CACHE_LINE 64U
/* Decompressed extents kept beside the nodes, each of at most
 * BT_MAX_COMPRESSED_SIZE bytes. */
#define BT_CACHE_EXTENTS 16U

/* Lookups run without the lock. A lookup raises its stripe's pin counter of an
 * entry and then reads the entry's replacing flag; the lock holder replacing
 * an entry sets the flag and then reads every stripe. Both orders are
 * sequentially consistent, so one of them sees the other: a pinned entry's
 * key and bytes cannot change. The holder clears the flag (release) after
 * writing the new key and bytes. Shared fields are accessed atomically. */
struct bt_cache_entry {
	uint64_t address;
	uint64_t generation;
	uint64_t owner;
	uint8_t *bytes;
	uint8_t replacing;
	uint8_t level;
	uint8_t used;
	uint8_t referenced;
};

/* A decompressed extent, used and replaced with the cache's lock held; used is
 * its last use (zero for an empty slot). */
struct bt_cache_extent {
	struct bt_extent_key key;
	uint64_t used;
	uint8_t *bytes;
	int verified;
};

/* Hits of one stripe, alone in its cache line. */
struct bt_cache_hits {
	uint64_t count;
	uint8_t padding[BT_CACHE_LINE - sizeof(uint64_t)];
};

struct btrfs_cache {
	struct btrfs_environment environment;
	struct btrfs_cache_locks locks;
	size_t bytes;
	/* Fixed by the first node stored; other node sizes bypass the cache. */
	uint32_t node_size;
	size_t sets;
	struct bt_cache_entry *entries;
	/* Stripe-major: pins[stripe * entry count + entry]. */
	uint32_t *pins;
	struct bt_cache_hits *hits;
	uint8_t *hands;
	uint8_t *spares[BT_CACHE_SPARES];
	size_t spare_count;
	size_t spare_limit;
	size_t node_buffers;
	/* Set (release) once the storage exists; lookups read it (acquire). */
	int ready;
	uint64_t misses;
	struct bt_cache_extent extents[BT_CACHE_EXTENTS];
	uint64_t extent_clock;
	uint64_t extent_hits;
	uint64_t extent_misses;
};

enum btrfs_result
btrfs_cache_create(const struct btrfs_environment *environment,
    const struct btrfs_cache_locks *locks, size_t bytes, struct btrfs_cache **result)
{
	struct btrfs_cache *cache;

	if (environment == NULL || result == NULL || environment->allocate == NULL ||
	    environment->release == NULL ||
	    (locks != NULL && (locks->lock == NULL) != (locks->unlock == NULL))) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*result = NULL;
	cache = environment->allocate(environment->context, sizeof(*cache));
	if (cache == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(cache, sizeof(*cache));
	cache->environment = *environment;
	cache->environment.cache = NULL;
	if (locks != NULL) {
		cache->locks = *locks;
	}
	cache->bytes = bytes;
	*result = cache;
	return BTRFS_OK;
}

static size_t
bt_cache_entry_count(const struct btrfs_cache *cache)
{
	return cache->sets * BT_CACHE_WAYS;
}

static void
bt_cache_release(struct btrfs_cache *cache)
{
	const struct btrfs_environment *env = &cache->environment;
	size_t count = bt_cache_entry_count(cache);
	size_t i;

	while (cache->spare_count != 0) {
		cache->spare_count--;
		env->release(env->context, cache->spares[cache->spare_count], cache->node_size);
	}
	if (cache->entries != NULL) {
		for (i = 0; i < count; i++) {
			if (cache->entries[i].bytes != NULL) {
				env->release(
				    env->context, cache->entries[i].bytes, cache->node_size);
			}
		}
		env->release(env->context, cache->entries, count * sizeof(*cache->entries));
	}
	if (cache->pins != NULL) {
		env->release(
		    env->context, cache->pins, BT_CACHE_STRIPES * count * sizeof(*cache->pins));
	}
	if (cache->hits != NULL) {
		env->release(env->context, cache->hits, BT_CACHE_STRIPES * sizeof(*cache->hits));
	}
	if (cache->hands != NULL) {
		env->release(env->context, cache->hands, cache->sets);
	}
	cache->entries = NULL;
	cache->pins = NULL;
	cache->hits = NULL;
	cache->hands = NULL;
}

void
btrfs_cache_destroy(struct btrfs_cache *cache)
{
	const struct btrfs_environment *env;
	size_t i;

	if (cache == NULL) {
		return;
	}
	env = &cache->environment;
	for (i = 0; i < BT_CACHE_EXTENTS; i++) {
		if (cache->extents[i].bytes != NULL) {
			env->release(env->context, cache->extents[i].bytes, BT_MAX_COMPRESSED_SIZE);
		}
	}
	bt_cache_release(cache);
	cache->environment.release(cache->environment.context, cache, sizeof(*cache));
}

static void
bt_cache_lock(struct btrfs_cache *cache)
{
	if (cache->locks.lock != NULL) {
		cache->locks.lock(cache->locks.context);
	}
}

static void
bt_cache_unlock(struct btrfs_cache *cache)
{
	if (cache->locks.unlock != NULL) {
		cache->locks.unlock(cache->locks.context);
	}
}

void
btrfs_cache_counts(struct btrfs_cache *cache, struct btrfs_cache_counts *counts)
{
	size_t i;

	bt_cache_lock(cache);
	counts->hits = 0;
	counts->misses = __atomic_load_n(&cache->misses, __ATOMIC_RELAXED);
	counts->pinned = 0;
	for (i = 0; cache->hits != NULL && i < BT_CACHE_STRIPES; i++) {
		counts->hits += __atomic_load_n(&cache->hits[i].count, __ATOMIC_RELAXED);
	}
	for (i = 0; cache->pins != NULL && i < BT_CACHE_STRIPES * bt_cache_entry_count(cache);
	    i++) {
		counts->pinned += __atomic_load_n(&cache->pins[i], __ATOMIC_RELAXED);
	}
	counts->extent_hits = cache->extent_hits;
	counts->extent_misses = cache->extent_misses;
	bt_cache_unlock(cache);
}

static int
bt_cache_extent_same(const struct bt_extent_key *a, const struct bt_extent_key *b)
{
	return a->disk_bytenr == b->disk_bytenr && a->disk_bytes == b->disk_bytes &&
	    a->ram_bytes == b->ram_bytes && a->generation == b->generation &&
	    a->compression == b->compression;
}

int
bt_cache_extent_get(struct btrfs_cache *cache, const struct bt_extent_key *key, int verified,
    uint64_t offset, void *output, size_t length)
{
	struct bt_cache_extent *extent;
	size_t i;
	int found = 0;

	/* Without locks, readers could see a decoding being replaced. */
	if (cache->locks.lock == NULL || offset > key->ram_bytes ||
	    length > key->ram_bytes - offset) {
		return 0;
	}
	bt_cache_lock(cache);
	for (i = 0; !found && i < BT_CACHE_EXTENTS; i++) {
		extent = &cache->extents[i];
		if (extent->used != 0 && bt_cache_extent_same(&extent->key, key) &&
		    (extent->verified || !verified)) {
			bt_copy(output, extent->bytes + offset, length);
			extent->used = ++cache->extent_clock;
			found = 1;
		}
	}
	if (found) {
		cache->extent_hits++;
	} else {
		cache->extent_misses++;
	}
	bt_cache_unlock(cache);
	return found;
}

void
bt_cache_extent_put(
    struct btrfs_cache *cache, const struct bt_extent_key *key, int verified, const void *bytes)
{
	const struct btrfs_environment *env = &cache->environment;
	struct bt_cache_extent *victim = &cache->extents[0];
	size_t i;

	if (cache->locks.lock == NULL || key->ram_bytes > BT_MAX_COMPRESSED_SIZE) {
		return;
	}
	bt_cache_lock(cache);
	for (i = 0; i < BT_CACHE_EXTENTS; i++) {
		if (cache->extents[i].used != 0 &&
		    bt_cache_extent_same(&cache->extents[i].key, key)) {
			victim = &cache->extents[i];
			break;
		}
		if (cache->extents[i].used < victim->used) {
			victim = &cache->extents[i];
		}
	}
	if (victim->bytes == NULL) {
		victim->bytes = env->allocate(env->context, BT_MAX_COMPRESSED_SIZE);
	}
	/* Without storage the decoding is simply not kept. */
	if (victim->bytes != NULL) {
		bt_copy(victim->bytes, bytes, (size_t)key->ram_bytes);
		victim->key = *key;
		victim->verified = verified;
		victim->used = ++cache->extent_clock;
	}
	bt_cache_unlock(cache);
}

/* Storage for node_size nodes, allocated on first use; with the lock held. */
static int
bt_cache_ready(struct btrfs_cache *cache, uint32_t node_size)
{
	const struct btrfs_environment *env = &cache->environment;
	size_t sets;
	size_t count;

	if (cache->node_size != 0) {
		return cache->node_size == node_size && cache->entries != NULL;
	}
	sets = cache->bytes / node_size / BT_CACHE_WAYS;
	if (sets == 0) {
		return 0;
	}
	cache->spare_limit = BT_CACHE_SPARES;
	cache->node_size = node_size;
	cache->sets = sets;
	count = bt_cache_entry_count(cache);
	cache->entries = env->allocate(env->context, count * sizeof(*cache->entries));
	if (cache->entries != NULL) {
		bt_zero(cache->entries, count * sizeof(*cache->entries));
	}
	cache->pins = env->allocate(env->context, BT_CACHE_STRIPES * count * sizeof(*cache->pins));
	cache->hits = env->allocate(env->context, BT_CACHE_STRIPES * sizeof(*cache->hits));
	cache->hands = env->allocate(env->context, sets);
	if (cache->entries == NULL || cache->pins == NULL || cache->hits == NULL ||
	    cache->hands == NULL) {
		bt_cache_release(cache);
		return 0;
	}
	bt_zero(cache->pins, BT_CACHE_STRIPES * count * sizeof(*cache->pins));
	bt_zero(cache->hits, BT_CACHE_STRIPES * sizeof(*cache->hits));
	bt_zero(cache->hands, sets);
	__atomic_store_n(&cache->ready, 1, __ATOMIC_RELEASE);
	return 1;
}

/* Whether lookups may use the storage for node_size nodes, without the lock
 * once it exists. */
static int
bt_cache_usable(struct btrfs_cache *cache, uint32_t node_size)
{
	int usable;

	if (__atomic_load_n(&cache->ready, __ATOMIC_ACQUIRE)) {
		return cache->node_size == node_size;
	}
	bt_cache_lock(cache);
	usable = bt_cache_ready(cache, node_size);
	bt_cache_unlock(cache);
	return usable;
}

static size_t
bt_cache_set(const struct btrfs_cache *cache, uint64_t address)
{
	uint64_t hash = (address / cache->node_size) * UINT64_C(0x9E3779B97F4A7C15);

	return (size_t)((hash >> 32) % cache->sets);
}

/* The stripe of a caller: threads have separate stacks, so the page of a
 * stack address separates them. */
static size_t
bt_cache_stripe(const void *hint)
{
	uint64_t page = (uint64_t)(uintptr_t)hint >> 12;

	return (size_t)((page * UINT64_C(0x9E3779B97F4A7C15)) >> 60) % BT_CACHE_STRIPES;
}

static uint32_t *
bt_cache_pin_counter(struct btrfs_cache *cache, size_t index, size_t stripe)
{
	return &cache->pins[stripe * bt_cache_entry_count(cache) + index];
}

static int
bt_cache_matches(const struct bt_cache_entry *entry, struct bt_root root)
{
	return __atomic_load_n(&entry->used, __ATOMIC_RELAXED) &&
	    __atomic_load_n(&entry->address, __ATOMIC_RELAXED) == root.address &&
	    __atomic_load_n(&entry->generation, __ATOMIC_RELAXED) == root.generation &&
	    __atomic_load_n(&entry->level, __ATOMIC_RELAXED) == root.level;
}

/* Pins entry index on stripe when it stores root's node. */
static int
bt_cache_try_pin(struct btrfs_cache *cache, size_t index, size_t stripe, struct bt_root root)
{
	struct bt_cache_entry *entry = &cache->entries[index];
	uint32_t *pins = bt_cache_pin_counter(cache, index, stripe);

	if (__atomic_load_n(&entry->replacing, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&entry->address, __ATOMIC_RELAXED) != root.address ||
	    __atomic_load_n(pins, __ATOMIC_RELAXED) == UINT32_MAX) {
		return 0;
	}
	__atomic_fetch_add(pins, 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&entry->replacing, __ATOMIC_SEQ_CST) ||
	    !bt_cache_matches(entry, root)) {
		__atomic_fetch_sub(pins, 1, __ATOMIC_RELEASE);
		return 0;
	}
	return 1;
}

const uint8_t *
bt_cache_pin(struct btrfs_cache *cache, struct bt_root root, uint32_t node_size, uint64_t *owner,
    size_t *handle, const void *hint)
{
	struct bt_cache_entry *entry;
	size_t stripe = bt_cache_stripe(hint);
	size_t set;
	size_t index;
	unsigned way;

	if (!bt_cache_usable(cache, node_size)) {
		return NULL;
	}
	set = bt_cache_set(cache, root.address);
	for (way = 0; way < BT_CACHE_WAYS; way++) {
		index = set * BT_CACHE_WAYS + way;
		entry = &cache->entries[index];
		if (bt_cache_try_pin(cache, index, stripe, root)) {
			if (!__atomic_load_n(&entry->referenced, __ATOMIC_RELAXED)) {
				__atomic_store_n(&entry->referenced, 1, __ATOMIC_RELAXED);
			}
			__atomic_fetch_add(&cache->hits[stripe].count, 1, __ATOMIC_RELAXED);
			*owner = __atomic_load_n(&entry->owner, __ATOMIC_RELAXED);
			*handle = index * BT_CACHE_STRIPES + stripe;
			return entry->bytes;
		}
	}
	__atomic_fetch_add(&cache->misses, 1, __ATOMIC_RELAXED);
	return NULL;
}

void
bt_cache_unpin(struct btrfs_cache *cache, size_t handle)
{
	__atomic_fetch_sub(
	    bt_cache_pin_counter(cache, handle / BT_CACHE_STRIPES, handle % BT_CACHE_STRIPES), 1,
	    __ATOMIC_RELEASE);
}

int
bt_cache_get(struct btrfs_cache *cache, struct bt_root root, uint32_t node_size, void *buffer,
    uint64_t *owner)
{
	const uint8_t *node;
	size_t handle;

	node = bt_cache_pin(cache, root, node_size, owner, &handle, buffer);
	if (node == NULL) {
		return 0;
	}
	bt_copy(buffer, node, node_size);
	bt_cache_unpin(cache, handle);
	return 1;
}

/* Claims an unpinned entry for replacement; with the lock held. */
static int
bt_cache_claim(struct btrfs_cache *cache, size_t index)
{
	struct bt_cache_entry *entry = &cache->entries[index];
	size_t stripe;

	__atomic_store_n(&entry->replacing, 1, __ATOMIC_SEQ_CST);
	for (stripe = 0; stripe < BT_CACHE_STRIPES; stripe++) {
		if (__atomic_load_n(bt_cache_pin_counter(cache, index, stripe), __ATOMIC_SEQ_CST) !=
		    0) {
			__atomic_store_n(&entry->replacing, 0, __ATOMIC_RELEASE);
			return 0;
		}
	}
	return 1;
}

/* Takes a node buffer only from the cache's own allocator. A claimed entry
 * has no readers; its bytes and ownership change before release publication. */
static int
bt_cache_store(struct btrfs_cache *cache, struct bt_root root, uint32_t node_size, const void *node,
    uint64_t owner, int take)
{
	struct bt_cache_entry *entry = NULL;
	struct bt_cache_entry *candidate;
	size_t set;
	size_t index;
	unsigned way;
	unsigned step;
	int stored = 0;

	bt_cache_lock(cache);
	if (!bt_cache_ready(cache, node_size)) {
		bt_cache_unlock(cache);
		return 0;
	}
	set = bt_cache_set(cache, root.address);
	/* Another reader may have stored the same node meanwhile. */
	for (way = 0; way < BT_CACHE_WAYS; way++) {
		if (bt_cache_matches(&cache->entries[set * BT_CACHE_WAYS + way], root)) {
			bt_cache_unlock(cache);
			return 0;
		}
	}
	/* CLOCK within the set: a free way, else the first unreferenced and
	 * unpinned one from the hand, clearing reference bits as it passes. */
	for (way = 0; way < BT_CACHE_WAYS && entry == NULL; way++) {
		index = set * BT_CACHE_WAYS + way;
		if (!__atomic_load_n(&cache->entries[index].used, __ATOMIC_RELAXED) &&
		    bt_cache_claim(cache, index)) {
			entry = &cache->entries[index];
		}
	}
	for (step = 0; entry == NULL && step < 2 * BT_CACHE_WAYS; step++) {
		way = cache->hands[set];
		cache->hands[set] = (uint8_t)((way + 1) % BT_CACHE_WAYS);
		index = set * BT_CACHE_WAYS + way;
		candidate = &cache->entries[index];
		if (__atomic_load_n(&candidate->referenced, __ATOMIC_RELAXED)) {
			__atomic_store_n(&candidate->referenced, 0, __ATOMIC_RELAXED);
		} else if (bt_cache_claim(cache, index)) {
			entry = candidate;
		}
	}
	if (entry != NULL) {
		if (take) {
			if (entry->bytes != NULL) {
				if (cache->spare_count < cache->spare_limit &&
				    cache->node_buffers < bt_cache_entry_count(cache)) {
					cache->spares[cache->spare_count++] = entry->bytes;
					cache->node_buffers++;
				} else {
					cache->environment.release(
					    cache->environment.context, entry->bytes, node_size);
				}
			} else {
				/* An empty entry can meet the byte limit only when a spare
				 * holds its share of storage. Prefer this published node. */
				if (cache->node_buffers == bt_cache_entry_count(cache)) {
					cache->environment.release(cache->environment.context,
					    cache->spares[--cache->spare_count], node_size);
				} else {
					cache->node_buffers++;
				}
			}
			entry->bytes = (uint8_t *)node;
		} else {
			if (entry->bytes == NULL && cache->spare_count != 0) {
				entry->bytes = cache->spares[--cache->spare_count];
			}
			if (entry->bytes == NULL) {
				entry->bytes = cache->environment.allocate(
				    cache->environment.context, node_size);
				cache->node_buffers += entry->bytes != NULL;
			}
			if (entry->bytes == NULL) {
				__atomic_store_n(&entry->replacing, 0, __ATOMIC_RELEASE);
				bt_cache_unlock(cache);
				return 0;
			}
			bt_copy(entry->bytes, node, node_size);
		}
		__atomic_store_n(&entry->address, root.address, __ATOMIC_RELAXED);
		__atomic_store_n(&entry->generation, root.generation, __ATOMIC_RELAXED);
		__atomic_store_n(&entry->level, root.level, __ATOMIC_RELAXED);
		__atomic_store_n(&entry->owner, owner, __ATOMIC_RELAXED);
		__atomic_store_n(&entry->used, 1, __ATOMIC_RELAXED);
		__atomic_store_n(&entry->referenced, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&entry->replacing, 0, __ATOMIC_RELEASE);
		stored = 1;
	}
	bt_cache_unlock(cache);
	return stored;
}

void
bt_cache_put(struct btrfs_cache *cache, struct bt_root root, uint32_t node_size, const void *node,
    uint64_t owner)
{
	(void)bt_cache_store(cache, root, node_size, node, owner, 0);
}

void
bt_cache_take(struct btrfs_cache *cache, struct bt_root root, uint32_t node_size, uint8_t **node,
    uint64_t owner, const struct btrfs_environment *source)
{
	if (source->context != cache->environment.context ||
	    source->allocate != cache->environment.allocate ||
	    source->release != cache->environment.release) {
		bt_cache_put(cache, root, node_size, *node, owner);
	} else if (bt_cache_store(cache, root, node_size, *node, owner, 1)) {
		*node = NULL;
	}
}

uint8_t *
bt_cache_reuse(
    struct btrfs_cache *cache, uint32_t node_size, const struct btrfs_environment *source)
{
	uint8_t *bytes = NULL;

	if (source->context != cache->environment.context ||
	    source->allocate != cache->environment.allocate ||
	    source->release != cache->environment.release) {
		return NULL;
	}
	bt_cache_lock(cache);
	if (cache->node_size == node_size && cache->spare_count != 0) {
		bytes = cache->spares[--cache->spare_count];
		cache->node_buffers--;
	}
	bt_cache_unlock(cache);
	return bytes;
}
