/* SPDX-License-Identifier: BSD-3-Clause */
/* Scrub, as Linux's btrfs scrub: every copy of every tree block and of every
 * checksummed data sector the extent tree names is read and verified, as a
 * read verifies it; with a writer, a copy that fails is rewritten in place
 * from a copy that verifies, the bytes the committed trees already name. A
 * RAID5/6 block's further copies are rebuilds from parity, which only serve
 * to repair its data stripe; parity itself is not verified. */
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
};

static int
bt_scrub_parity(const struct btrfs_fs *fs, uint64_t logical)
{
	size_t chunk = bt_chunk_containing(fs, logical);

	return chunk < fs->chunk_count &&
	    (fs->chunks[chunk].type & (BT_BLOCK_RAID5 | BT_BLOCK_RAID6)) != 0;
}

/* Writes bytes over copy mirror of logical, where it lies on its devices. */
static enum btrfs_result
bt_scrub_rewrite(struct bt_scrub *scrub, uint64_t logical, const uint8_t *bytes, size_t length,
    uint64_t kind, unsigned mirror)
{
	const struct btrfs_fs *fs = scrub->fs;
	const struct btrfs_write_environment *writer = scrub->writer;
	struct bt_place place;
	size_t done;
	unsigned mirrors;
	enum btrfs_result error = BTRFS_OK;

	for (done = 0; error == BTRFS_OK && done < length; done += place.span) {
		error =
		    bt_map_place(fs, logical + done, length - done, kind, mirror, &place, &mirrors);
		if (error == BTRFS_OK && place.device >= fs->device_count) {
			error = BTRFS_CORRUPT;
		}
		if (error == BTRFS_OK) {
			error = place.device == 0 ? writer->write(writer->context, place.physical,
							bytes + done, place.span)
						  : writer->devices[place.device - 1].write(
							writer->devices[place.device - 1].context,
							place.physical, bytes + done, place.span);
			scrub->written |= UINT32_C(1) << place.device;
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

/* The extent tree's tree blocks and data extents, each scrubbed in turn. */
static enum btrfs_result
bt_scrub_extents(struct bt_scrub *scrub, struct bt_root extents)
{
	const struct btrfs_fs *fs = scrub->fs;
	const struct bt_disk_extent_item *item;
	const struct bt_disk_tree_block_info *info;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	uint64_t flags;
	size_t chunk;
	enum btrfs_result error;

	bt_cursor_init(&cursor, fs, extents);
	error = bt_cursor_seek(&cursor, (struct bt_key){ 0 }, 0);
	while (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		if (record.key.type == BT_METADATA_ITEM || record.key.type == BT_EXTENT_ITEM) {
			item = (const void *)record.data;
			if (record.size < sizeof(*item)) {
				error = BTRFS_CORRUPT;
				break;
			}
			flags = bt_u64(item->flags);
			root = (struct bt_root){ record.key.objectid, bt_u64(item->generation),
				BT_OWNER_ANY, 0 };
			if (record.key.type == BT_METADATA_ITEM) {
				root.level = (uint8_t)record.key.offset;
				error = record.key.offset < BT_MAX_LEVEL ? BTRFS_OK : BTRFS_CORRUPT;
			} else if ((flags & BT_EXTENT_FLAG_TREE) != 0) {
				info = (const void *)(item + 1);
				error = record.size >= sizeof(*item) + sizeof(*info) &&
					record.key.offset == fs->info.node_size &&
					info->level < BT_MAX_LEVEL
				    ? BTRFS_OK
				    : BTRFS_CORRUPT;
				root.level = error == BTRFS_OK ? info->level : 0;
			}
			if (error == BTRFS_OK &&
			    (record.key.type == BT_METADATA_ITEM ||
				(flags & BT_EXTENT_FLAG_TREE) != 0)) {
				chunk = bt_chunk_containing(fs, root.address);
				error = chunk == fs->chunk_count ? BTRFS_CORRUPT : BTRFS_OK;
				if (error == BTRFS_OK) {
					root.owner = (fs->chunks[chunk].type & BT_BLOCK_SYSTEM) != 0
					    ? BT_CHUNK_TREE
					    : BT_OWNER_ANY;
					error = bt_scrub_block(scrub, root,
					    root.owner == BT_CHUNK_TREE ? BT_BLOCK_SYSTEM
									: BT_BLOCK_METADATA);
				}
			} else if (error == BTRFS_OK && (flags & BT_EXTENT_FLAG_DATA) != 0) {
				error = record.key.offset % fs->info.sector_size == 0 &&
					record.key.objectid % fs->info.sector_size == 0
				    ? bt_scrub_data(scrub, record.key.objectid, record.key.offset)
				    : BTRFS_CORRUPT;
			}
		}
		if (error == BTRFS_OK) {
			error = bt_cursor_next(&cursor);
		}
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
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
	error = scrub.copies == NULL || scrub.sums == NULL || scrub.present == NULL ||
		scrub.good == NULL
	    ? BTRFS_NO_MEMORY
	    : bt_find_root(fs, BT_EXTENT_TREE, &extents);
	if (error == BTRFS_OK) {
		bt_cursor_init(&scrub.checksums, fs, fs->checksum_tree);
		error = bt_scrub_supers(&scrub);
		if (error == BTRFS_OK) {
			error = bt_scrub_extents(&scrub, extents);
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
	btrfs_unmount(fs);
	if (error == BTRFS_OK &&
	    (report->unrepairable != 0 || report->super_errors != 0 ||
		(writer == NULL && report->corrupted + report->unreadable != 0))) {
		error = BTRFS_CORRUPT;
	}
	return error;
}
