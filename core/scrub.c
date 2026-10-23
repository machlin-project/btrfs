/* SPDX-License-Identifier: BSD-3-Clause */
/* Scrub, as Linux's btrfs scrub: every copy of every tree block and of every
 * checksummed data sector the extent tree names is read and verified, as a
 * read verifies it; with a writer, a copy that fails is rewritten in place
 * from a copy that verifies, the bytes the committed trees already name. A
 * RAID5/6 block's further copies are rebuilds from parity, which only serve
 * to repair its data stripe. Then, as Linux's scrub_raid56_parity_stripe,
 * each RAID5/6 row holding extents is read again: where every sector the
 * extents hold at an offset of its data units verifies, P and Q there must
 * equal the data's parity, and with a writer are rewritten to it. */
#include "internal.h"
#include <btrfs/write.h>

/* Data verified per read of each copy. */
#define BT_SCRUB_PIECE (UINT64_C(256) * 1024)

struct bt_scrub {
	const struct btrfs_fs *fs;
	const struct btrfs_write_environment *writer;
	struct btrfs_scrub_report *report;
	struct bt_cursor checksums;
	/* BT_MAX_COPIES buffers of BT_SCRUB_PIECE bytes, at least a node. */
	uint8_t *copies;
	size_t copy_size;
	/* Per sector of a piece: its stored checksum, whether it has one, and
	 * per copy what it held (enum bt_scrub_sector). */
	uint8_t *sums;
	uint8_t *present;
	uint8_t *good;
	/* Devices written (bits of their indexes), flushed at the end. */
	uint32_t written;
	/* The RAID5/6 row being checked: its chunk (chunk_count for none),
	 * number and BT_RAID56_UNITS stripe lengths for its units and their
	 * parity again, which read, and per offset of the data units (bits of
	 * sectors) where an extent lies and where one did not verify. */
	size_t row_chunk;
	uint64_t row;
	uint8_t *units;
	int row_read;
	uint32_t marked;
	uint32_t failed;
};

/* A row's units: its data units, P, Q, and P and Q again. */
#define BT_SCRUB_ROW_UNITS (BT_MAX_STRIPES + 2U)
/* The smallest sector a volume has: a row's sector bitmaps hold a stripe
 * unit's sectors. */
#define BT_SCRUB_SMALLEST_SECTOR 4096U
_Static_assert(BT_STRIPE_LENGTH / BT_SCRUB_SMALLEST_SECTOR <= 32, "row sector bitmaps");

static int
bt_scrub_parity(const struct btrfs_fs *fs, uint64_t logical)
{
	size_t chunk = bt_chunk_containing(fs, logical);

	return chunk < fs->chunk_count &&
	    (fs->chunks[chunk].type & (BT_BLOCK_RAID5 | BT_BLOCK_RAID6)) != 0;
}

/* Writes bytes at physical of the device of index. */
static enum btrfs_result
bt_scrub_write(
    struct bt_scrub *scrub, uint32_t device, uint64_t physical, const uint8_t *bytes, size_t length)
{
	const struct btrfs_write_environment *writer = scrub->writer;

	if (device >= scrub->fs->device_count) {
		return BTRFS_CORRUPT;
	}
	scrub->written |= UINT32_C(1) << device;
	return device == 0 ? writer->write(writer->context, physical, bytes, length)
			   : writer->devices[device - 1].write(
				 writer->devices[device - 1].context, physical, bytes, length);
}

/* Writes bytes over copy mirror of logical, where it lies on its devices. */
static enum btrfs_result
bt_scrub_rewrite(struct bt_scrub *scrub, uint64_t logical, const uint8_t *bytes, size_t length,
    uint64_t kind, unsigned mirror)
{
	struct bt_place place;
	size_t done;
	unsigned mirrors;
	enum btrfs_result error = BTRFS_OK;

	for (done = 0; error == BTRFS_OK && done < length; done += place.span) {
		error = bt_map_place(
		    scrub->fs, logical + done, length - done, kind, mirror, &place, &mirrors);
		if (error == BTRFS_OK) {
			error = bt_scrub_write(
			    scrub, place.device, place.physical, bytes + done, place.span);
		}
	}
	return error;
}

/* Every copy of a tree block, verified as a read verifies it (checksum,
 * identity, level, generation, owner, items); bad ones are rewritten from
 * the first good one. */
static enum btrfs_result
bt_scrub_block(struct bt_scrub *scrub, struct bt_root root, uint64_t kind)
{
	const struct btrfs_fs *fs = scrub->fs;
	enum btrfs_result status[BT_MAX_COPIES];
	uint8_t *copy;
	size_t node = fs->info.node_size;
	size_t span;
	unsigned mirrors = 1;
	unsigned mirror;
	unsigned good = BT_MAX_COPIES;
	int parity = bt_scrub_parity(fs, root.address);
	int bad = 0;
	enum btrfs_result error;

	for (mirror = 0; mirror < mirrors && mirror < BT_MAX_COPIES; mirror++) {
		/* A RAID5/6 block's rebuilds only serve to repair its data stripe. */
		if (parity && mirror != 0 && !bad) {
			break;
		}
		copy = scrub->copies + mirror * scrub->copy_size;
		error = bt_read_copy(fs, root.address, copy, node, kind, mirror, &span, &mirrors);
		if (error == BTRFS_OK && span != node) {
			error = BTRFS_CORRUPT;
		}
		if (error == BTRFS_OK) {
			error = bt_tree_verify(fs, root, copy);
		}
		if (error != BTRFS_OK && error != BTRFS_CORRUPT && error != BTRFS_IO) {
			return error;
		}
		status[mirror] = error;
		if (!parity || mirror == 0) {
			scrub->report->tree_copies++;
			scrub->report->corrupted += error == BTRFS_CORRUPT;
			scrub->report->unreadable += error == BTRFS_IO;
			bad |= error != BTRFS_OK;
		}
		if (error == BTRFS_OK && good == BT_MAX_COPIES) {
			good = mirror;
		}
	}
	if (!bad) {
		return BTRFS_OK;
	}
	if (good == BT_MAX_COPIES) {
		scrub->report->unrepairable++;
		return BTRFS_OK;
	}
	for (mirror = 0; scrub->writer != NULL && mirror < mirrors && mirror < BT_MAX_COPIES;
	    mirror++) {
		if ((parity && mirror != 0) || status[mirror] == BTRFS_OK) {
			continue;
		}
		error = bt_scrub_rewrite(scrub, root.address,
		    scrub->copies + good * scrub->copy_size, node, kind, mirror);
		if (error != BTRFS_OK) {
			return error;
		}
		scrub->report->repaired++;
	}
	return BTRFS_OK;
}

/* The stored checksum of the data sector at logical; NOT_FOUND when it has
 * none (NODATASUM, preallocated). */
static enum btrfs_result
bt_scrub_checksum(struct bt_scrub *scrub, uint64_t logical, uint8_t *checksum)
{
	const struct btrfs_fs *fs = scrub->fs;
	struct bt_key key = { BT_CSUM_OBJECTID, logical, BT_EXTENT_CSUM };
	struct bt_record record;
	size_t size = bt_checksum_size(fs->info.checksum_type);
	uint64_t index;
	enum btrfs_result error;

	error = bt_cursor_record(&scrub->checksums, &record);
	if (error != BTRFS_OK || record.key.objectid != key.objectid ||
	    record.key.type != key.type || logical < record.key.offset ||
	    (logical - record.key.offset) / fs->info.sector_size >= record.size / size) {
		error = bt_cursor_seek(&scrub->checksums, key, 1);
		if (error != BTRFS_OK) {
			return error;
		}
		(void)bt_cursor_record(&scrub->checksums, &record);
	}
	if (record.key.objectid != key.objectid || record.key.type != key.type ||
	    logical < record.key.offset) {
		return BTRFS_NOT_FOUND;
	}
	if (record.key.offset % fs->info.sector_size != 0 || record.size % size != 0) {
		return BTRFS_CORRUPT;
	}
	index = (logical - record.key.offset) / fs->info.sector_size;
	if (index >= record.size / size) {
		return BTRFS_NOT_FOUND;
	}
	bt_copy(checksum, record.data + (size_t)index * size, size);
	return BTRFS_OK;
}

/* What one copy of a data sector held. */
enum bt_scrub_sector {
	BT_SCRUB_UNREADABLE,
	/* Its checksum did not match. */
	BT_SCRUB_BAD,
	/* It matched its checksum, or read without one. */
	BT_SCRUB_GOOD,
};

/* Reads copy mirror of a piece of data and records what each sector held. */
static enum btrfs_result
bt_scrub_read_piece(
    struct bt_scrub *scrub, uint64_t logical, size_t length, unsigned mirror, unsigned *mirrors)
{
	const struct btrfs_fs *fs = scrub->fs;
	uint8_t sum[BT_CSUM_SIZE];
	uint8_t *copy = scrub->copies + mirror * scrub->copy_size;
	uint8_t *state = scrub->good + mirror * (scrub->copy_size / fs->info.sector_size);
	size_t sector = fs->info.sector_size;
	size_t size = bt_checksum_size(fs->info.checksum_type);
	size_t position;
	size_t span = 0;
	size_t s;
	enum btrfs_result error;

	for (position = 0; position < length; position += span) {
		error = bt_read_copy(fs, logical + position, copy + position, length - position,
		    BT_BLOCK_DATA, mirror, &span, mirrors);
		if (error != BTRFS_OK && error != BTRFS_IO) {
			return error;
		}
		if (span == 0 || span % sector != 0) {
			return BTRFS_CORRUPT;
		}
		for (s = position / sector; s < (position + span) / sector; s++) {
			state[s] = error != BTRFS_OK ? BT_SCRUB_UNREADABLE : BT_SCRUB_GOOD;
			if (error == BTRFS_OK && scrub->present[s]) {
				bt_checksum_sectors(fs, copy + s * sector, 1, sum);
				state[s] = bt_equal(sum, scrub->sums + s * size, size)
				    ? BT_SCRUB_GOOD
				    : BT_SCRUB_BAD;
			}
		}
	}
	return BTRFS_OK;
}

/* Every copy of each sector of a data extent: verified against its stored
 * checksum, or only read without one, as Linux's scrub counts read errors of
 * every sector. Copies that fail are rewritten from a good one. */
static enum btrfs_result
bt_scrub_data(struct bt_scrub *scrub, uint64_t start, uint64_t length)
{
	const struct btrfs_fs *fs = scrub->fs;
	size_t sector = fs->info.sector_size;
	size_t size = bt_checksum_size(fs->info.checksum_type);
	size_t per_copy = scrub->copy_size / sector;
	size_t piece;
	size_t sectors;
	size_t s;
	uint64_t offset;
	uint8_t state;
	unsigned mirrors;
	unsigned read;
	unsigned mirror;
	unsigned good;
	int parity = bt_scrub_parity(fs, start);
	int bad;
	enum btrfs_result error;

	for (offset = 0; offset < length; offset += piece) {
		piece = length - offset < BT_SCRUB_PIECE ? (size_t)(length - offset)
							 : (size_t)BT_SCRUB_PIECE;
		sectors = piece / sector;
		for (s = 0; s < sectors; s++) {
			error = bt_scrub_checksum(
			    scrub, start + offset + s * sector, scrub->sums + s * size);
			if (error != BTRFS_OK && error != BTRFS_NOT_FOUND) {
				return error;
			}
			scrub->present[s] = error == BTRFS_OK;
			scrub->report->unchecked += !scrub->present[s];
		}
		/* Every copy, a RAID5/6 piece's rebuilds only when its data stripe
		 * fails somewhere. */
		mirrors = 1;
		bad = 0;
		for (read = 0; read < mirrors && read < BT_MAX_COPIES; read++) {
			if (parity && read != 0 && !bad) {
				break;
			}
			error = bt_scrub_read_piece(scrub, start + offset, piece, read, &mirrors);
			if (error != BTRFS_OK) {
				return error;
			}
			for (s = 0; read == 0 && s < sectors; s++) {
				bad |= scrub->good[s] != BT_SCRUB_GOOD;
			}
		}
		for (s = 0; s < sectors; s++) {
			good = BT_MAX_COPIES;
			bad = 0;
			for (mirror = 0; mirror < read; mirror++) {
				state = scrub->good[mirror * per_copy + s];
				if (state == BT_SCRUB_GOOD) {
					good = good == BT_MAX_COPIES ? mirror : good;
				}
				if (parity && mirror != 0) {
					continue;
				}
				bad |= state != BT_SCRUB_GOOD;
				scrub->report->data_copies += scrub->present[s];
				scrub->report->corrupted += state == BT_SCRUB_BAD;
				scrub->report->unreadable += state == BT_SCRUB_UNREADABLE;
			}
			if (!bad) {
				continue;
			}
			if (good == BT_MAX_COPIES) {
				scrub->report->unrepairable++;
				continue;
			}
			for (mirror = 0; scrub->writer != NULL && mirror < read; mirror++) {
				if ((parity && mirror != 0) ||
				    scrub->good[mirror * per_copy + s] == BT_SCRUB_GOOD) {
					continue;
				}
				error = bt_scrub_rewrite(scrub, start + offset + s * sector,
				    scrub->copies + good * scrub->copy_size + s * sector, sector,
				    BT_BLOCK_DATA, mirror);
				if (error != BTRFS_OK) {
					return error;
				}
				scrub->report->repaired++;
			}
		}
	}
	return BTRFS_OK;
}

/* Every superblock copy each present device holds, counted when it does
 * not verify; btrfs_recover_supers resolves them. */
static enum btrfs_result
bt_scrub_supers(struct bt_scrub *scrub)
{
	const struct btrfs_fs *fs = scrub->fs;
	uint32_t device;
	unsigned mirror;
	enum btrfs_result error;

	for (device = 0; device < fs->device_count; device++) {
		for (mirror = 0; mirror < BT_SUPER_MIRRORS; mirror++) {
			if (!bt_super_present(fs->devices[device].size, mirror)) {
				continue;
			}
			error = bt_read_device(
			    fs, device, bt_super_offset(mirror), scrub->copies, BT_SUPER_SIZE);
			if (error == BTRFS_OK) {
				error = bt_super_check(
				    (const void *)scrub->copies, bt_super_offset(mirror));
			}
			if (error == BTRFS_NO_MEMORY) {
				return error;
			}
			scrub->report->super_copies++;
			scrub->report->super_errors += error != BTRFS_OK;
		}
	}
	return BTRFS_OK;
}

/* What an item of the extent tree names: a tree block (*tree, its root),
 * or a data extent of length bytes at its key's objectid, both inside the
 * chunk of index; NOT_FOUND for other items. */
static enum btrfs_result
bt_scrub_item(const struct btrfs_fs *fs, const struct bt_record *record, struct bt_root *root,
    uint64_t *length, size_t *chunk, int *tree)
{
	const struct bt_disk_extent_item *item = (const void *)record->data;
	const struct bt_disk_tree_block_info *info;
	uint64_t flags;
	enum btrfs_result error = BTRFS_OK;

	if (record->key.type != BT_METADATA_ITEM && record->key.type != BT_EXTENT_ITEM) {
		return BTRFS_NOT_FOUND;
	}
	if (record->size < sizeof(*item)) {
		return BTRFS_CORRUPT;
	}
	flags = bt_u64(item->flags);
	*root = (struct bt_root){ record->key.objectid, bt_u64(item->generation), BT_OWNER_ANY, 0 };
	*tree = record->key.type == BT_METADATA_ITEM || (flags & BT_EXTENT_FLAG_TREE) != 0;
	*length = *tree ? fs->info.node_size : record->key.offset;
	if (record->key.type == BT_METADATA_ITEM) {
		root->level = (uint8_t)record->key.offset;
		error = record->key.offset < BT_MAX_LEVEL ? BTRFS_OK : BTRFS_CORRUPT;
	} else if (*tree) {
		info = (const void *)(item + 1);
		error = record->size >= sizeof(*item) + sizeof(*info) &&
			record->key.offset == fs->info.node_size && info->level < BT_MAX_LEVEL
		    ? BTRFS_OK
		    : BTRFS_CORRUPT;
		root->level = error == BTRFS_OK ? info->level : 0;
	} else if ((flags & BT_EXTENT_FLAG_DATA) == 0) {
		return BTRFS_NOT_FOUND;
	} else if (record->key.offset % fs->info.sector_size != 0 ||
	    record->key.objectid % fs->info.sector_size != 0) {
		error = BTRFS_CORRUPT;
	}
	if (error != BTRFS_OK) {
		return error;
	}
	*chunk = bt_chunk_containing(fs, root->address);
	if (*chunk == fs->chunk_count ||
	    *length > fs->chunks[*chunk].logical + fs->chunks[*chunk].length - root->address) {
		return BTRFS_CORRUPT;
	}
	if (*tree) {
		root->owner =
		    (fs->chunks[*chunk].type & BT_BLOCK_SYSTEM) != 0 ? BT_CHUNK_TREE : BT_OWNER_ANY;
	}
	return BTRFS_OK;
}

/* The extent tree's tree blocks and data extents, each scrubbed in turn. */
static enum btrfs_result
bt_scrub_extents(struct bt_scrub *scrub, struct bt_root extents)
{
	const struct btrfs_fs *fs = scrub->fs;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	uint64_t length;
	size_t chunk;
	int tree;
	enum btrfs_result error;

	bt_cursor_init(&cursor, fs, extents);
	error = bt_cursor_seek(&cursor, (struct bt_key){ 0 }, 0);
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		error = bt_scrub_item(fs, &record, &root, &length, &chunk, &tree);
		if (error == BTRFS_OK && tree) {
			error = bt_scrub_block(scrub, root,
			    root.owner == BT_CHUNK_TREE ? BT_BLOCK_SYSTEM : BT_BLOCK_METADATA);
		} else if (error == BTRFS_OK) {
			error = bt_scrub_data(scrub, root.address, length);
		}
		if (error == BTRFS_OK || error == BTRFS_NOT_FOUND) {
			error = bt_cursor_next(&cursor);
		}
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

/* Checks the parity of the row being checked at each offset where an extent
 * lies and every extent there verified; with a writer, P and Q there are
 * rewritten from the data. Then no row is being checked. */
static enum btrfs_result
bt_scrub_row_end(struct bt_scrub *scrub)
{
	const struct btrfs_fs *fs = scrub->fs;
	const struct bt_chunk *chunk;
	const uint8_t *data[BT_MAX_STRIPES];
	const uint8_t *stored;
	const uint8_t *computed;
	size_t sector = fs->info.sector_size;
	uint64_t profile;
	uint32_t check;
	unsigned stripes;
	unsigned count;
	unsigned stripe;
	unsigned d;
	unsigned s;
	enum btrfs_result error = BTRFS_OK;

	if (scrub->row_chunk == fs->chunk_count) {
		return BTRFS_OK;
	}
	chunk = &fs->chunks[scrub->row_chunk];
	scrub->row_chunk = fs->chunk_count;
	check = scrub->marked & ~scrub->failed;
	if (!scrub->row_read || check == 0) {
		return BTRFS_OK;
	}
	profile = chunk->type & BT_BLOCK_PROFILES;
	stripes = chunk->stripes;
	count = stripes - (profile == BT_BLOCK_RAID5 ? 1U : 2U);
	for (d = 0; d < count; d++) {
		data[d] = scrub->units + d * BT_STRIPE_LENGTH;
	}
	bt_raid56_parity(profile, data, count, scrub->units + stripes * BT_STRIPE_LENGTH,
	    scrub->units + (stripes + 1) * BT_STRIPE_LENGTH, BT_STRIPE_LENGTH);
	scrub->report->parity_rows++;
	/* P, then Q. */
	for (d = count; error == BTRFS_OK && d < stripes; d++) {
		stripe = (unsigned)((scrub->row + d) % stripes);
		for (s = 0; error == BTRFS_OK && s < BT_STRIPE_LENGTH / sector; s++) {
			stored = scrub->units + d * BT_STRIPE_LENGTH + s * sector;
			computed =
			    scrub->units + (stripes + d - count) * BT_STRIPE_LENGTH + s * sector;
			if ((check & (UINT32_C(1) << s)) == 0 ||
			    bt_equal(stored, computed, sector)) {
				continue;
			}
			scrub->report->parity_errors++;
			if (scrub->writer != NULL) {
				error = bt_scrub_write(scrub, chunk->devices[stripe],
				    chunk->physical[stripe] + scrub->row * BT_STRIPE_LENGTH +
					s * sector,
				    computed, sector);
				scrub->report->repaired += error == BTRFS_OK;
			}
		}
	}
	return error;
}

/* Begins checking row of the RAID5/6 chunk of index: reads its data units,
 * P and Q, as they lie on the devices now. */
static enum btrfs_result
bt_scrub_row_begin(struct bt_scrub *scrub, size_t index, uint64_t row)
{
	const struct bt_chunk *chunk = &scrub->fs->chunks[index];
	unsigned stripe;
	unsigned d;
	enum btrfs_result error = BTRFS_OK;

	scrub->row_chunk = index;
	scrub->row = row;
	scrub->marked = 0;
	scrub->failed = 0;
	for (d = 0; error == BTRFS_OK && d < chunk->stripes; d++) {
		stripe = (unsigned)((row + d) % chunk->stripes);
		error = bt_read_device(scrub->fs, chunk->devices[stripe],
		    chunk->physical[stripe] + row * BT_STRIPE_LENGTH,
		    scrub->units + d * BT_STRIPE_LENGTH, BT_STRIPE_LENGTH);
	}
	/* A row that cannot be read whole is not checked. */
	scrub->row_read = error == BTRFS_OK;
	return error == BTRFS_IO || error == BTRFS_CORRUPT ? BTRFS_OK : error;
}

/* Marks where an extent of a RAID5/6 chunk lies in its rows' data units and
 * whether what it holds there verifies now: each checksummed data sector,
 * or the tree block of root (*tree). */
static enum btrfs_result
bt_scrub_mark(struct bt_scrub *scrub, size_t index, struct bt_root root, uint64_t length, int tree)
{
	const struct btrfs_fs *fs = scrub->fs;
	const struct bt_chunk *chunk = &fs->chunks[index];
	uint8_t stored[BT_CSUM_SIZE];
	uint8_t sum[BT_CSUM_SIZE];
	const uint8_t *bytes;
	size_t sector = fs->info.sector_size;
	size_t size = bt_checksum_size(fs->info.checksum_type);
	uint64_t profile = chunk->type & BT_BLOCK_PROFILES;
	uint64_t row_length =
	    (uint64_t)(chunk->stripes - (profile == BT_BLOCK_RAID5 ? 1U : 2U)) * BT_STRIPE_LENGTH;
	uint64_t position;
	uint64_t within;
	uint64_t offset;
	uint64_t row;
	uint64_t piece;
	uint64_t s;
	uint32_t bits;
	enum btrfs_result error = BTRFS_OK;

	for (position = root.address; error == BTRFS_OK && position < root.address + length;
	    position += piece) {
		within = position - chunk->logical;
		row = within / row_length;
		offset = within % BT_STRIPE_LENGTH;
		piece = root.address + length - position < BT_STRIPE_LENGTH - offset
		    ? root.address + length - position
		    : BT_STRIPE_LENGTH - offset;
		if (scrub->row_chunk != index || scrub->row != row) {
			error = bt_scrub_row_end(scrub);
			if (error == BTRFS_OK) {
				error = bt_scrub_row_begin(scrub, index, row);
			}
			if (error != BTRFS_OK) {
				return error;
			}
		}
		bits = 0;
		for (s = offset / sector; s * sector < offset + piece; s++) {
			bits |= UINT32_C(1) << s;
		}
		scrub->marked |= bits;
		if (!scrub->row_read) {
			continue;
		}
		/* The data units lie in row order. */
		bytes = scrub->units + within % row_length;
		if (tree) {
			/* A tree block lies in one stripe unit. */
			if (piece != length || bt_tree_verify(fs, root, bytes) != BTRFS_OK) {
				scrub->failed |= bits;
			}
			continue;
		}
		for (s = 0; error == BTRFS_OK && s < piece / sector; s++) {
			error = bt_scrub_checksum(scrub, position + s * sector, stored);
			if (error == BTRFS_NOT_FOUND) {
				error = BTRFS_OK;
				continue;
			}
			if (error == BTRFS_OK) {
				bt_checksum_sectors(fs, bytes + s * sector, 1, sum);
				if (!bt_equal(sum, stored, size)) {
					scrub->failed |= UINT32_C(1) << (offset / sector + s);
				}
			}
		}
	}
	return error;
}

/* The parity of every RAID5/6 row holding extents, read again after the
 * repairs of the extents. */
static enum btrfs_result
bt_scrub_rows(struct bt_scrub *scrub, struct bt_root extents)
{
	const struct btrfs_fs *fs = scrub->fs;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	uint64_t length;
	size_t chunk;
	int tree;
	enum btrfs_result error;

	scrub->row_chunk = fs->chunk_count;
	bt_cursor_init(&cursor, fs, extents);
	error = bt_cursor_seek(&cursor, (struct bt_key){ 0 }, 0);
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		error = bt_scrub_item(fs, &record, &root, &length, &chunk, &tree);
		if (error == BTRFS_OK && bt_scrub_parity(fs, root.address)) {
			error = bt_scrub_mark(scrub, chunk, root, length, tree);
		}
		if (error == BTRFS_OK || error == BTRFS_NOT_FOUND) {
			error = bt_cursor_next(&cursor);
		}
	}
	bt_cursor_fini(&cursor);
	if (error == BTRFS_NOT_FOUND) {
		error = bt_scrub_row_end(scrub);
	}
	return error;
}

enum btrfs_result
btrfs_scrub(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, struct btrfs_scrub_report *report)
{
	struct btrfs_environment uncached;
	struct bt_scrub scrub;
	struct btrfs_fs *fs = NULL;
	struct bt_root extents = { 0 };
	size_t sectors = 0;
	size_t i;
	int parity = 0;
	enum btrfs_result error;

	if (report == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	bt_zero(report, sizeof(*report));
	if (environment == NULL ||
	    (writer != NULL &&
		(writer->write == NULL || writer->flush == NULL ||
		    writer->device_count != environment->device_count ||
		    (writer->device_count != 0 && writer->devices == NULL)))) {
		return BTRFS_INVALID_ARGUMENT;
	}
	for (i = 0; writer != NULL && i < writer->device_count; i++) {
		if (writer->devices[i].write == NULL || writer->devices[i].flush == NULL) {
			return BTRFS_INVALID_ARGUMENT;
		}
	}
	/* Repairs write every device of the volume. */
	if (writer != NULL && environment->degraded) {
		return BTRFS_UNSUPPORTED;
	}
	/* Copies are read from the devices, never from the node cache. */
	uncached = *environment;
	uncached.cache = NULL;
	error = btrfs_mount(&uncached, BTRFS_TOP_LEVEL_TREE, &fs);
	if (error != BTRFS_OK) {
		return error;
	}
	bt_zero(&scrub, sizeof(scrub));
	scrub.fs = fs;
	scrub.writer = writer;
	scrub.report = report;
	scrub.copy_size = BT_SCRUB_PIECE > fs->info.node_size ? BT_SCRUB_PIECE : fs->info.node_size;
	sectors = scrub.copy_size / fs->info.sector_size;
	scrub.copies = fs->env.allocate(fs->env.context, BT_MAX_COPIES * scrub.copy_size);
	scrub.sums = fs->env.allocate(fs->env.context, sectors * BT_CSUM_SIZE);
	scrub.present = fs->env.allocate(fs->env.context, sectors);
	scrub.good = fs->env.allocate(fs->env.context, BT_MAX_COPIES * sectors);
	for (i = 0; i < fs->chunk_count; i++) {
		parity |= (fs->chunks[i].type & (BT_BLOCK_RAID5 | BT_BLOCK_RAID6)) != 0;
	}
	if (parity) {
		scrub.units =
		    fs->env.allocate(fs->env.context, BT_SCRUB_ROW_UNITS * BT_STRIPE_LENGTH);
	}
	error = scrub.copies == NULL || scrub.sums == NULL || scrub.present == NULL ||
		scrub.good == NULL || (parity && scrub.units == NULL)
	    ? BTRFS_NO_MEMORY
	    : bt_find_root(fs, BT_EXTENT_TREE, &extents);
	if (error == BTRFS_OK) {
		bt_cursor_init(&scrub.checksums, fs, fs->checksum_tree);
		error = bt_scrub_supers(&scrub);
		if (error == BTRFS_OK) {
			error = bt_scrub_extents(&scrub, extents);
		}
		if (error == BTRFS_OK && parity) {
			error = bt_scrub_rows(&scrub, extents);
		}
		bt_cursor_fini(&scrub.checksums);
	}
	/* Repairs reach stable storage on every device written. */
	for (i = 0; error == BTRFS_OK && i < fs->device_count; i++) {
		if ((scrub.written & (UINT32_C(1) << i)) != 0) {
			error = i == 0
			    ? writer->flush(writer->context)
			    : writer->devices[i - 1].flush(writer->devices[i - 1].context);
		}
	}
	if (scrub.copies != NULL) {
		fs->env.release(fs->env.context, scrub.copies, BT_MAX_COPIES * scrub.copy_size);
	}
	if (scrub.sums != NULL) {
		fs->env.release(fs->env.context, scrub.sums, sectors * BT_CSUM_SIZE);
	}
	if (scrub.present != NULL) {
		fs->env.release(fs->env.context, scrub.present, sectors);
	}
	if (scrub.good != NULL) {
		fs->env.release(fs->env.context, scrub.good, BT_MAX_COPIES * sectors);
	}
	if (scrub.units != NULL) {
		fs->env.release(
		    fs->env.context, scrub.units, BT_SCRUB_ROW_UNITS * BT_STRIPE_LENGTH);
	}
	btrfs_unmount(fs);
	if (error == BTRFS_OK &&
	    (report->unrepairable != 0 || report->super_errors != 0 ||
		(writer == NULL &&
		    report->corrupted + report->unreadable + report->parity_errors != 0))) {
		error = BTRFS_CORRUPT;
	}
	return error;
}
