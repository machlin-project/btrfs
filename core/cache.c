/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

/* Ways per set: a lookup compares at most this many entries. */
#define BT_CACHE_WAYS 8U

struct bt_cache_entry {
	uint64_t address;
	uint64_t generation;
	uint64_t owner;
	uint8_t level;
	uint8_t used;
	uint8_t referenced;
};

struct btrfs_cache {
	struct btrfs_environment environment;
	struct btrfs_cache_locks locks;
	size_t bytes;
	/* Fixed by the first node stored; other node sizes bypass the cache. */
	uint32_t node_size;
	size_t sets;
	struct bt_cache_entry *entries;
	uint8_t *nodes;
	uint8_t *hands;
	uint64_t hits;
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

static void
bt_cache_release(struct btrfs_cache *cache)
{
	const struct btrfs_environment *env = &cache->environment;

	if (cache->entries != NULL) {
		env->release(env->context, cache->entries,
		    cache->sets * BT_CACHE_WAYS * sizeof(*cache->entries));
	}
	if (cache->nodes != NULL) {
		env->release(
		    env->context, cache->nodes, cache->sets * BT_CACHE_WAYS * cache->node_size);
	}
	if (cache->hands != NULL) {
		env->release(env->context, cache->hands, cache->sets);
	}
	cache->entries = NULL;
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

void
btrfs_cache_counts(const struct btrfs_cache *cache, uint64_t *hits, uint64_t *misses)
{
	*hits = cache->hits;
	*misses = cache->misses;
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

/* Storage for node_size nodes, allocated on first use. */
static int
bt_cache_ready(struct btrfs_cache *cache, uint32_t node_size)
{
	const struct btrfs_environment *env = &cache->environment;
	size_t sets;

	if (cache->node_size != 0) {
		return cache->node_size == node_size && cache->entries != NULL;
	}
	sets = cache->bytes / node_size / BT_CACHE_WAYS;
	if (sets == 0) {
		return 0;
	}
	cache->node_size = node_size;
	cache->sets = sets;
	cache->entries =
	    env->allocate(env->context, sets * BT_CACHE_WAYS * sizeof(*cache->entries));
	cache->nodes = env->allocate(env->context, sets * BT_CACHE_WAYS * node_size);
	cache->hands = env->allocate(env->context, sets);
	if (cache->entries == NULL || cache->nodes == NULL || cache->hands == NULL) {
		bt_cache_release(cache);
		return 0;
	}
	bt_zero(cache->entries, sets * BT_CACHE_WAYS * sizeof(*cache->entries));
	bt_zero(cache->hands, sets);
	return 1;
}

static size_t
bt_cache_set(const struct btrfs_cache *cache, uint64_t address)
{
	uint64_t hash = (address / cache->node_size) * UINT64_C(0x9E3779B97F4A7C15);

	return (size_t)((hash >> 32) % cache->sets);
}

int
bt_cache_get(struct btrfs_cache *cache, struct bt_root root, uint32_t node_size, void *buffer,
    uint64_t *owner)
{
	struct bt_cache_entry *entry;
	size_t set;
	unsigned way;
	int found = 0;

	bt_cache_lock(cache);
	if (bt_cache_ready(cache, node_size)) {
		set = bt_cache_set(cache, root.address);
		for (way = 0; way < BT_CACHE_WAYS && !found; way++) {
			entry = &cache->entries[set * BT_CACHE_WAYS + way];
			if (entry->used && entry->address == root.address &&
			    entry->generation == root.generation && entry->level == root.level) {
				bt_copy(buffer,
				    cache->nodes + (set * BT_CACHE_WAYS + way) * node_size,
				    node_size);
				*owner = entry->owner;
				entry->referenced = 1;
				found = 1;
			}
		}
		if (found) {
			cache->hits++;
		} else {
			cache->misses++;
		}
	}
	bt_cache_unlock(cache);
	return found;
}

void
bt_cache_put(struct btrfs_cache *cache, struct bt_root root, uint32_t node_size, const void *node,
    uint64_t owner)
{
	struct bt_cache_entry *entry = NULL;
	size_t set;
	unsigned way;
	unsigned step;

	bt_cache_lock(cache);
	if (bt_cache_ready(cache, node_size)) {
		set = bt_cache_set(cache, root.address);
		/* CLOCK within the set: a free way, else the first unreferenced one
		 * from the hand, clearing reference bits as it passes. */
		for (way = 0; way < BT_CACHE_WAYS && entry == NULL; way++) {
			if (!cache->entries[set * BT_CACHE_WAYS + way].used) {
				entry = &cache->entries[set * BT_CACHE_WAYS + way];
			}
		}
		for (step = 0; entry == NULL && step < 2 * BT_CACHE_WAYS; step++) {
			way = cache->hands[set];
			cache->hands[set] = (uint8_t)((way + 1) % BT_CACHE_WAYS);
			if (cache->entries[set * BT_CACHE_WAYS + way].referenced) {
				cache->entries[set * BT_CACHE_WAYS + way].referenced = 0;
			} else {
				entry = &cache->entries[set * BT_CACHE_WAYS + way];
			}
		}
		if (entry != NULL) {
			way = (unsigned)(entry - &cache->entries[set * BT_CACHE_WAYS]);
			bt_copy(cache->nodes + (set * BT_CACHE_WAYS + way) * node_size, node,
			    node_size);
			entry->address = root.address;
			entry->generation = root.generation;
			entry->level = root.level;
			entry->owner = owner;
			entry->used = 1;
			entry->referenced = 0;
		}
	}
	bt_cache_unlock(cache);
}
