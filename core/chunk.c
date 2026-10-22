/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

size_t
bt_chunk_position(const struct btrfs_fs *fs, uint64_t logical)
{
	size_t low = 0;
	size_t high = fs->chunk_count;
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (fs->chunks[middle].logical < logical) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return low;
}

size_t
bt_chunk_containing(const struct btrfs_fs *fs, uint64_t logical)
{
	size_t index = bt_chunk_position(fs, logical);

	if (index < fs->chunk_count && fs->chunks[index].logical == logical) {
		return index;
	}
	if (index > 0 && logical - fs->chunks[index - 1].logical < fs->chunks[index - 1].length) {
		return index - 1;
	}
	return fs->chunk_count;
}

/* Doubles the table, up to BT_MAX_CHUNKS. */
static enum btrfs_result
bt_chunk_grow(struct btrfs_fs *fs)
{
	struct bt_chunk *grown;
	size_t capacity;

	if (fs->chunk_capacity >= BT_MAX_CHUNKS) {
		return BTRFS_UNSUPPORTED;
	}
	capacity = fs->chunk_capacity * 2 > BT_MAX_CHUNKS ? BT_MAX_CHUNKS : fs->chunk_capacity * 2;
	grown = fs->env.allocate(fs->env.context, capacity * sizeof(*grown));
	if (grown == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_copy(grown, fs->chunks, fs->chunk_count * sizeof(*grown));
	fs->env.release(fs->env.context, fs->chunks, fs->chunk_capacity * sizeof(*fs->chunks));
	fs->chunks = grown;
	fs->chunk_capacity = capacity;
	return BTRFS_OK;
}

/* Linux's btrfs_raid_array for one profile: stripes a chunk must have
 * (exactly, or at least with minimum), stripes that hold one copy of the
 * data (data_stripes counts them as a divisor of the stripe count), copies a
 * read chooses among and RAID10's stripes per copy. */
struct bt_profile {
	uint64_t bits;
	unsigned exact;
	unsigned minimum;
	unsigned parity;
	unsigned sub_stripes;
};

static const struct bt_profile bt_profiles[] = {
	{ 0, 1, 1, 0, 1 },
	{ BT_BLOCK_DUP, 2, 2, 0, 1 },
	{ BT_BLOCK_RAID0, 0, 1, 0, 1 },
	{ BT_BLOCK_RAID1, 2, 2, 0, 1 },
	{ BT_BLOCK_RAID1C3, 3, 3, 0, 1 },
	{ BT_BLOCK_RAID1C4, 4, 4, 0, 1 },
	{ BT_BLOCK_RAID10, 0, 2, 0, 2 },
	{ BT_BLOCK_RAID5, 0, 2, 1, 1 },
	{ BT_BLOCK_RAID6, 0, 3, 2, 1 },
};

/* Stripes over which the chunk's data spreads: each holds this share of it. */
static unsigned
bt_chunk_data_stripes(uint64_t profile, unsigned stripes, unsigned sub_stripes)
{
	if (profile == BT_BLOCK_RAID0) {
		return stripes;
	}
	if (profile == BT_BLOCK_RAID10) {
		return stripes / sub_stripes;
	}
	if (profile == BT_BLOCK_RAID5) {
		return stripes - 1;
	}
	return profile == BT_BLOCK_RAID6 ? stripes - 2 : 1;
}

uint64_t
bt_chunk_stripe_length(const struct bt_chunk *chunk)
{
	return chunk->length /
	    bt_chunk_data_stripes(
		chunk->type & BT_BLOCK_PROFILES, chunk->stripes, chunk->sub_stripes);
}

/* The volume's device with id and uuid, as an index; device_count if none.
 * A degraded mount adds a device it lacks, with no reads, the first time a
 * chunk names it, up to the volume's device count. */
static size_t
bt_chunk_device(struct btrfs_fs *fs, uint64_t id, const uint8_t *uuid)
{
	struct bt_device *missing;
	size_t i;

	for (i = 0; i < fs->device_count; i++) {
		if (fs->devices[i].id == id) {
			return bt_equal(fs->devices[i].uuid, uuid, BTRFS_UUID_SIZE)
			    ? i
			    : fs->device_count;
		}
	}
	if (!fs->env.degraded || id == 0 || fs->device_count == fs->device_capacity) {
		return fs->device_count;
	}
	missing = &fs->devices[fs->device_count];
	bt_zero(missing, sizeof(*missing));
	missing->id = id;
	missing->size = UINT64_MAX;
	bt_copy(missing->uuid, uuid, BTRFS_UUID_SIZE);
	return fs->device_count++;
}

enum btrfs_result
bt_chunk_add(struct btrfs_fs *fs, struct bt_key key, const void *data, size_t length, int bootstrap)
{
	const struct bt_disk_chunk *disk = data;
	const struct bt_disk_stripe *stripes;
	const struct bt_profile *rule = NULL;
	struct bt_chunk chunk;
	struct bt_chunk *existing;
	uint64_t profile;
	uint64_t kind;
	uint64_t share;
	size_t device;
	unsigned count;
	unsigned sub_stripes;
	unsigned j;
	unsigned k;
	size_t i;
	enum btrfs_result error;

	if (key.objectid != BT_FIRST_CHUNK_OBJECTID || key.type != BT_CHUNK_ITEM ||
	    length < sizeof(*disk)) {
		return BTRFS_CORRUPT;
	}
	bt_zero(&chunk, sizeof(chunk));
	chunk.logical = key.offset;
	chunk.length = bt_u64(disk->length);
	chunk.type = bt_u64(disk->type);
	chunk.confirmed = !bootstrap;
	count = bt_u16(disk->stripes);
	sub_stripes = bt_u16(disk->sub_stripes);
	if (count == 0 || (length - sizeof(*disk)) / sizeof(*stripes) != count ||
	    (length - sizeof(*disk)) % sizeof(*stripes) != 0) {
		return BTRFS_CORRUPT;
	}
	profile = chunk.type & ~(BT_BLOCK_DATA | BT_BLOCK_METADATA | BT_BLOCK_SYSTEM);
	kind = chunk.type & (BT_BLOCK_DATA | BT_BLOCK_METADATA | BT_BLOCK_SYSTEM);
	for (i = 0; i < sizeof(bt_profiles) / sizeof(bt_profiles[0]); i++) {
		if (bt_profiles[i].bits == profile) {
			rule = &bt_profiles[i];
		}
	}
	if (rule == NULL) {
		return (profile & ~BT_BLOCK_PROFILES) != 0 ? BTRFS_UNSUPPORTED : BTRFS_CORRUPT;
	}
	if (count > BT_MAX_STRIPES) {
		return BTRFS_UNSUPPORTED;
	}
	/* Linux's btrfs_check_chunk_valid: the stripe counts its profile allows,
	 * and RAID10's two-copy stripes in whole pairs; only RAID10 reads the
	 * recorded sub-stripe count. */
	if ((rule->exact != 0 ? count != rule->exact : count < rule->minimum) ||
	    (profile == BT_BLOCK_RAID10 &&
		(sub_stripes != rule->sub_stripes || count % rule->sub_stripes != 0)) ||
	    (kind != BT_BLOCK_DATA && kind != BT_BLOCK_METADATA && kind != BT_BLOCK_SYSTEM &&
		kind != (BT_BLOCK_DATA | BT_BLOCK_METADATA)) ||
	    (bootstrap && kind != BT_BLOCK_SYSTEM) || bt_u64(disk->owner) != BT_EXTENT_TREE ||
	    bt_u64(disk->stripe_length) != BT_STRIPE_LENGTH ||
	    bt_u32(disk->sector_size) != fs->info.sector_size || chunk.length == 0 ||
	    chunk.length > UINT64_MAX - chunk.logical ||
	    chunk.logical % fs->info.sector_size != 0 || chunk.length % fs->info.sector_size != 0) {
		return BTRFS_CORRUPT;
	}
	sub_stripes = rule->sub_stripes;
	share = chunk.length / bt_chunk_data_stripes(profile, count, sub_stripes);
	if (chunk.length % bt_chunk_data_stripes(profile, count, sub_stripes) != 0 ||
	    share % fs->info.sector_size != 0) {
		return BTRFS_CORRUPT;
	}
	stripes = (const void *)((const uint8_t *)data + sizeof(*disk));
	chunk.stripes = (uint8_t)count;
	chunk.sub_stripes = (uint8_t)sub_stripes;
	chunk.mirrors = (uint8_t)(profile == 0 || profile == BT_BLOCK_RAID0 ? 1U
		: profile == BT_BLOCK_RAID10				    ? sub_stripes
		: (profile & (BT_BLOCK_RAID5 | BT_BLOCK_RAID6)) != 0
		? bt_raid56_copies(profile, count)
		: count);
	for (j = 0; j < count; j++) {
		chunk.physical[j] = bt_u64(stripes[j].offset);
		device = bt_chunk_device(fs, bt_u64(stripes[j].device), stripes[j].uuid);
		if (device == fs->device_count || chunk.physical[j] % fs->info.sector_size != 0 ||
		    chunk.physical[j] > fs->devices[device].size ||
		    share > fs->devices[device].size - chunk.physical[j]) {
			return BTRFS_CORRUPT;
		}
		chunk.devices[j] = (uint8_t)device;
		/* Stripes on one device never overlap. */
		for (k = 0; k < j; k++) {
			if (chunk.devices[k] == chunk.devices[j] &&
			    chunk.physical[k] < chunk.physical[j] + share &&
			    chunk.physical[j] < chunk.physical[k] + share) {
				return BTRFS_CORRUPT;
			}
		}
	}
	/* The table is sorted and disjoint, so a new chunk can only collide with
	 * its neighbours. */
	i = bt_chunk_position(fs, chunk.logical);
	if (i < fs->chunk_count && fs->chunks[i].logical == chunk.logical) {
		existing = &fs->chunks[i];
		if (bootstrap || existing->confirmed || existing->length != chunk.length ||
		    existing->type != chunk.type || existing->stripes != chunk.stripes ||
		    existing->sub_stripes != chunk.sub_stripes ||
		    !bt_equal(existing->physical, chunk.physical, sizeof(chunk.physical)) ||
		    !bt_equal(existing->devices, chunk.devices, sizeof(chunk.devices))) {
			return BTRFS_CORRUPT;
		}
		existing->confirmed = 1;
		return BTRFS_OK;
	}
	if ((i > 0 && fs->chunks[i - 1].logical + fs->chunks[i - 1].length > chunk.logical) ||
	    (i < fs->chunk_count && chunk.logical + chunk.length > fs->chunks[i].logical)) {
		return BTRFS_CORRUPT;
	}
	if (fs->chunk_count == fs->chunk_capacity) {
		error = bt_chunk_grow(fs);
		if (error != BTRFS_OK) {
			return error;
		}
	}
	bt_move(&fs->chunks[i + 1], &fs->chunks[i], (fs->chunk_count - i) * sizeof(*fs->chunks));
	fs->chunks[i] = chunk;
	fs->chunk_count++;
	return BTRFS_OK;
}

enum btrfs_result
bt_map_place(const struct btrfs_fs *fs, uint64_t logical, size_t length, uint64_t kind,
    unsigned mirror, struct bt_place *place, unsigned *mirrors)
{
	const struct bt_chunk *chunk;
	uint64_t profile;
	uint64_t within;
	uint64_t stripe;
	uint64_t offset;
	unsigned index;
	unsigned factor;
	size_t low = 0;
	size_t high = fs->chunk_count;
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (fs->chunks[middle].logical <= logical) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	if (low == 0) {
		return BTRFS_CORRUPT;
	}
	chunk = &fs->chunks[low - 1];
	within = logical - chunk->logical;
	*mirrors = chunk->mirrors;
	if ((chunk->type & kind) == 0 || within >= chunk->length ||
	    length > chunk->length - within || mirror >= chunk->mirrors) {
		return BTRFS_CORRUPT;
	}
	profile = chunk->type & BT_BLOCK_PROFILES;
	if ((profile & (BT_BLOCK_RAID0 | BT_BLOCK_RAID10 | BT_BLOCK_RAID5 | BT_BLOCK_RAID6)) == 0) {
		/* Every copy holds the chunk whole on one stripe. */
		place->device = chunk->devices[mirror];
		place->physical = chunk->physical[mirror] + within;
		place->span = length;
		return BTRFS_OK;
	}
	/* Linux's btrfs_map_block: 64 KiB stripes in turn over the stripes that
	 * hold data. */
	stripe = within / BT_STRIPE_LENGTH;
	offset = within % BT_STRIPE_LENGTH;
	if (profile == BT_BLOCK_RAID0) {
		index = (unsigned)(stripe % chunk->stripes);
		stripe /= chunk->stripes;
	} else if (profile == BT_BLOCK_RAID10) {
		factor = chunk->stripes / chunk->sub_stripes;
		index = (unsigned)(stripe % factor) * chunk->sub_stripes + mirror;
		stripe /= factor;
	} else if (mirror != 0) {
		/* A RAID5/6 copy above the first is a rebuild (bt_read_copy). */
		return BTRFS_UNSUPPORTED;
	} else {
		/* RAID5/6: the data stripe, its parity rotating over the stripes. */
		factor = chunk->stripes - (profile == BT_BLOCK_RAID5 ? 1U : 2U);
		index = (unsigned)(stripe % factor);
		stripe /= factor;
		index = (unsigned)((stripe + index) % chunk->stripes);
	}
	place->device = chunk->devices[index];
	place->physical = chunk->physical[index] + stripe * BT_STRIPE_LENGTH + offset;
	place->span =
	    length < BT_STRIPE_LENGTH - offset ? length : (size_t)(BT_STRIPE_LENGTH - offset);
	return BTRFS_OK;
}

enum btrfs_result
bt_map(const struct btrfs_fs *fs, uint64_t logical, size_t length, uint64_t kind, unsigned mirror,
    uint64_t *physical, unsigned *mirrors)
{
	struct bt_place place;
	enum btrfs_result error;

	error = bt_map_place(fs, logical, length, kind, mirror, &place, mirrors);
	if (error == BTRFS_OK && (place.device != 0 || place.span != length)) {
		error = BTRFS_UNSUPPORTED;
	}
	if (error == BTRFS_OK) {
		*physical = place.physical;
	}
	return error;
}

enum btrfs_result
bt_read_copy(const struct btrfs_fs *fs, uint64_t logical, void *buffer, size_t length,
    uint64_t kind, unsigned mirror, size_t *span, unsigned *mirrors)
{
	const struct bt_chunk *chunk;
	struct bt_place place;
	size_t index = bt_chunk_containing(fs, logical);
	uint64_t within;
	enum btrfs_result error;

	*span = 0;
	if (index == fs->chunk_count) {
		return BTRFS_CORRUPT;
	}
	chunk = &fs->chunks[index];
	if (mirror != 0 && (chunk->type & (BT_BLOCK_RAID5 | BT_BLOCK_RAID6)) != 0) {
		within = logical - chunk->logical;
		*mirrors = chunk->mirrors;
		if ((chunk->type & kind) == 0 || length > chunk->length - within ||
		    mirror >= chunk->mirrors) {
			return BTRFS_CORRUPT;
		}
		*span = length < BT_STRIPE_LENGTH - within % BT_STRIPE_LENGTH
		    ? length
		    : (size_t)(BT_STRIPE_LENGTH - within % BT_STRIPE_LENGTH);
		return bt_raid56_rebuild(fs, chunk, within, buffer, *span, mirror);
	}
	error = bt_map_place(fs, logical, length, kind, mirror, &place, mirrors);
	if (error == BTRFS_OK) {
		*span = place.span;
		error = bt_read_device(fs, place.device, place.physical, buffer, place.span);
	}
	return error;
}
