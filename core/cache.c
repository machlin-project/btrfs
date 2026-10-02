/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

/* Ways per set: a lookup compares at most this many entries. */
#define BT_CACHE_WAYS 8U
/* Pin counters per entry. Every lookup pins the roots of the trees it walks,
 * so one shared counter per entry would bounce between processors; a lookup
 * uses the stripe its caller's stack address selects. */
#define BT_CACHE_STRIPES 16U
#define BT_CACHE_LINE 64U

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
	uint8_t replacing;
	uint8_t level;
	uint8_t used;
	uint8_t referenced;
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
	uint8_t *nodes;
	uint8_t *hands;
	/* Set (release) once the storage exists; lookups read it (acquire). */
	int ready;
	uint64_t misses;
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

	if (cache->entries != NULL) {
		env->release(env->context, cache->entries, count * sizeof(*cache->entries));
	}
	if (cache->pins != NULL) {
		env->release(
		    env->context, cache->pins, BT_CACHE_STRIPES * count * sizeof(*cache->pins));
	}
	if (cache->hits != NULL) {
		env->release(env->context, cache->hits, BT_CACHE_STRIPES * sizeof(*cache->hits));
	}
	if (cache->nodes != NULL) {
		env->release(env->context, cache->nodes, count * cache->node_size);
	}
	if (cache->hands != NULL) {
		env->release(env->context, cache->hands, cache->sets);
	}
	cache->entries = NULL;
	cache->pins = NULL;
	cache->hits = NULL;
	cache->nodes = NULL;
	cache->hands = NULL;
}

void
btrfs_cache_destroy(struct btrfs_cache *cache)
{
	if (cache == NULL) {
		return;
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
	cache->node_size = node_size;
	cache->sets = sets;
	count = bt_cache_entry_count(cache);
	cache->entries = env->allocate(env->context, count * sizeof(*cache->entries));
	cache->pins = env->allocate(env->context, BT_CACHE_STRIPES * count * sizeof(*cache->pins));
	cache->hits = env->allocate(env->context, BT_CACHE_STRIPES * sizeof(*cache->hits));
	cache->nodes = env->allocate(env->context, count * node_size);
	cache->hands = env->allocate(env->context, sets);
	if (cache->entries == NULL || cache->pins == NULL || cache->hits == NULL ||
	    cache->nodes == NULL || cache->hands == NULL) {
		bt_cache_release(cache);
		return 0;
	}
	bt_zero(cache->entries, count * sizeof(*cache->entries));
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
			return cache->nodes + index * node_size;
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

void
bt_cache_put(struct btrfs_cache *cache, struct bt_root root, uint32_t node_size, const void *node,
    uint64_t owner)
{
	struct bt_cache_entry *entry = NULL;
	struct bt_cache_entry *candidate;
	size_t set;
	size_t index;
	unsigned way;
	unsigned step;

	bt_cache_lock(cache);
	if (!bt_cache_ready(cache, node_size)) {
		bt_cache_unlock(cache);
		return;
	}
	set = bt_cache_set(cache, root.address);
	/* Another reader may have stored the same node meanwhile. */
	for (way = 0; way < BT_CACHE_WAYS; way++) {
		if (bt_cache_matches(&cache->entries[set * BT_CACHE_WAYS + way], root)) {
			bt_cache_unlock(cache);
			return;
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
		index = (size_t)(entry - cache->entries);
		bt_copy(cache->nodes + index * node_size, node, node_size);
		__atomic_store_n(&entry->address, root.address, __ATOMIC_RELAXED);
		__atomic_store_n(&entry->generation, root.generation, __ATOMIC_RELAXED);
		__atomic_store_n(&entry->level, root.level, __ATOMIC_RELAXED);
		__atomic_store_n(&entry->owner, owner, __ATOMIC_RELAXED);
		__atomic_store_n(&entry->used, 1, __ATOMIC_RELAXED);
		__atomic_store_n(&entry->referenced, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&entry->replacing, 0, __ATOMIC_RELEASE);
	}
	bt_cache_unlock(cache);
}
