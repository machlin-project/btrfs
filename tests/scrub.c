/* SPDX-License-Identifier: BSD-3-Clause */
/* Scrub (btrfs_scrub) on clones of a Linux-written volume of one device or
 * several. A clean volume scrubs clean. Then one copy of every seventh tree
 * block and of every seventh checksummed data sector is corrupted where it
 * lies on its device (only tree blocks with another copy, so the volume
 * still mounts), and one superblock copy; of RAID5/6 rows, the P or Q sector
 * at the offset of another checksummed data sector in seven, never at an
 * offset of a row with a corrupted data stripe, and never two data stripes
 * of one row at one offset. A clean volume's rows hold Linux's parity. A
 * scrub without a writer finds every corrupted copy and parity sector and
 * writes nothing; with a writer it rewrites each from a copy that verifies
 * (a RAID5/6 data stripe from its rebuild, parity from the data), so a
 * further scrub finds only what had no other copy, the volume reads as
 * before and, where everything had another copy, the reference audit, which
 * reads every copy, passes. The superblock copy is left to superblock
 * recovery, which rewrites it; then nothing fails. The clones go to a
 * directory and, with --keep, stay there for Linux's own scrub; with
 * --faults every allocation and device read of a clean scrub fails in turn,
 * which fails it without a leak. */
#include "../adapters/posix/image.h"
#include "internal.h"
#include "namespace_audit.h"
#include "references.h"
#include <btrfs/write.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/clonefile.h>
#include <unistd.h>

#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define MAX_DEVICES 16U
#define PATH_BYTES 4096U
/* Every this many tree blocks and checksummed data sectors loses a copy. */
#define CORRUPTION_STRIDE 7U
#define CORRUPT_BYTE 0x5aU
/* The copy of the superblock corrupted: the first secondary. */
#define SUPER_MIRROR 1U
/* RAID5/6 rows with a corrupted data stripe or parity, at most. */
#define MAX_ROWS 65536U
/* Of the checksummed data sectors, which of every CORRUPTION_STRIDE marks a
 * RAID5/6 row offset whose parity is corrupted. */
#define PARITY_RESIDUE 3U

struct volume {
	char paths[MAX_DEVICES][PATH_BYTES];
	const char *path_list[MAX_DEVICES];
	size_t devices;
};

struct expectation {
	uint64_t corrupted;
	uint64_t unrepairable;
	uint64_t tree;
	uint64_t data;
	/* P and Q sectors corrupted. */
	uint64_t parity;
	/* The rows (chunk, row, offset in the stripe) of RAID5/6 data stripes
	 * or parity corrupted: a second one there would leave the rebuild
	 * nothing. */
	uint64_t rows[MAX_ROWS][2];
	size_t row_count;
};

static void
open_volume(const struct volume *volume, struct btrfs_image *image, int writable)
{
	REQUIRE(
	    (writable ? btrfs_image_open_devices_writable(volume->path_list, volume->devices, image)
		      : btrfs_image_open_devices(volume->path_list, volume->devices, image)) == 0);
}

static enum btrfs_result
scrub(const struct volume *volume, int repair, struct btrfs_scrub_report *report)
{
	struct btrfs_write_environment writer;
	struct btrfs_image image;
	enum btrfs_result result;

	open_volume(volume, &image, repair);
	btrfs_image_writer(&image, &writer);
	result = btrfs_scrub(&image.environment, repair ? &writer : NULL, report);
	REQUIRE(image.live_allocations == 0);
	btrfs_image_close(&image);
	return result;
}

/* Overwrites length bytes of one device of the volume with a pattern. */
static void
corrupt_device(const struct volume *volume, uint32_t device, uint64_t physical, size_t length)
{
	uint8_t *bytes = malloc(length);
	int file = open(volume->paths[device], O_WRONLY);

	REQUIRE(bytes != NULL && file >= 0);
	memset(bytes, CORRUPT_BYTE, length);
	REQUIRE(pwrite(file, bytes, length, (off_t)physical) == (ssize_t)length);
	REQUIRE(close(file) == 0);
	free(bytes);
}

/* Corrupts copy mirror of a logical range, one place at a time. */
static void
corrupt_copy(const struct volume *volume, const struct btrfs_fs *fs, uint64_t logical,
    size_t length, uint64_t kind, unsigned mirror)
{
	struct bt_place place;
	size_t done;
	unsigned mirrors;

	for (done = 0; done < length; done += place.span) {
		REQUIRE(bt_map_place(fs, logical + done, length - done, kind, mirror, &place,
			    &mirrors) == BTRFS_OK);
		/* The image's devices are the volume's in order. */
		corrupt_device(volume, place.device, place.physical, place.span);
	}
}

/* The physical copies of a chunk: a RAID5/6 chunk's further copies are
 * rebuilds, so only its data stripe is corrupted. */
static unsigned
physical_copies(const struct btrfs_fs *fs, uint64_t logical)
{
	const struct bt_chunk *chunk = &fs->chunks[bt_chunk_containing(fs, logical)];

	return (chunk->type & (BT_BLOCK_RAID5 | BT_BLOCK_RAID6)) != 0 ? 1U : chunk->mirrors;
}

static int
parity_chunk(const struct btrfs_fs *fs, uint64_t logical)
{
	return (fs->chunks[bt_chunk_containing(fs, logical)].type &
		   (BT_BLOCK_RAID5 | BT_BLOCK_RAID6)) != 0;
}

/* Corrupts the sector of P (which 0) or Q (1) of the RAID5/6 row holding
 * logical, at its offset in its data stripe. */
static void
corrupt_parity(
    const struct volume *volume, const struct btrfs_fs *fs, uint64_t logical, unsigned which)
{
	const struct bt_chunk *chunk = &fs->chunks[bt_chunk_containing(fs, logical)];
	uint64_t within = logical - chunk->logical;
	uint64_t data = chunk->length / bt_chunk_stripe_length(chunk);
	uint64_t row = within / BT_STRIPE_LENGTH / data;
	unsigned stripe = (unsigned)((row + data + which) % chunk->stripes);

	corrupt_device(volume, chunk->devices[stripe],
	    chunk->physical[stripe] + row * BT_STRIPE_LENGTH + within % BT_STRIPE_LENGTH,
	    fs->info.sector_size);
}

/* Whether a RAID5/6 sector's row already has a corrupted data stripe at
 * its offset; records it otherwise. Other profiles never share. */
static int
row_taken(const struct btrfs_fs *fs, struct expectation *expected, uint64_t logical)
{
	const struct bt_chunk *chunk = &fs->chunks[bt_chunk_containing(fs, logical)];
	uint64_t within = logical - chunk->logical;
	uint64_t data = chunk->length / bt_chunk_stripe_length(chunk);
	uint64_t row =
	    within / BT_STRIPE_LENGTH / data * BT_STRIPE_LENGTH + within % BT_STRIPE_LENGTH;
	size_t i;

	if ((chunk->type & (BT_BLOCK_RAID5 | BT_BLOCK_RAID6)) == 0) {
		return 0;
	}
	for (i = 0; i < expected->row_count; i++) {
		if (expected->rows[i][0] == chunk->logical && expected->rows[i][1] == row) {
			return 1;
		}
	}
	REQUIRE(expected->row_count < MAX_ROWS);
	expected->rows[expected->row_count][0] = chunk->logical;
	expected->rows[expected->row_count][1] = row;
	expected->row_count++;
	return 0;
}

static int
has_checksum(const struct btrfs_fs *fs, uint64_t logical)
{
	struct bt_cursor cursor;
	struct bt_record record;
	size_t size = bt_checksum_size(fs->info.checksum_type);
	int found = 0;

	bt_cursor_init(&cursor, fs, fs->checksum_tree);
	if (bt_cursor_seek(&cursor, (struct bt_key){ BT_CSUM_OBJECTID, logical, BT_EXTENT_CSUM },
		1) == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		found = record.key.objectid == BT_CSUM_OBJECTID &&
		    record.key.type == BT_EXTENT_CSUM && record.key.offset <= logical &&
		    (logical - record.key.offset) / fs->info.sector_size < record.size / size;
	}
	bt_cursor_fini(&cursor);
	return found;
}

/* Corrupts one copy of every CORRUPTION_STRIDE-th tree block that has
 * another copy and of every CORRUPTION_STRIDE-th checksummed data sector,
 * and the first secondary superblock of the last device. */
static void
corrupt(const struct volume *volume, struct expectation *expected)
{
	const struct bt_disk_extent_item *item;
	struct btrfs_image image;
	struct btrfs_fs *fs;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root extents;
	uint64_t sector;
	uint64_t kind;
	uint64_t flags;
	unsigned copies;
	unsigned redundant;
	unsigned parities;
	size_t blocks = 0;
	size_t sectors = 0;
	size_t position;
	enum btrfs_result result;

	memset(expected, 0, sizeof(*expected));
	open_volume(volume, &image, 0);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(bt_find_root(fs, BT_EXTENT_TREE, &extents) == BTRFS_OK);
	bt_cursor_init(&cursor, fs, extents);
	result = bt_cursor_seek(&cursor, (struct bt_key){ 0 }, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		item = (const void *)record.data;
		flags = record.size >= sizeof(*item) ? bt_u64(item->flags) : 0;
		if (record.key.type == BT_METADATA_ITEM ||
		    (record.key.type == BT_EXTENT_ITEM && (flags & BT_EXTENT_FLAG_TREE) != 0)) {
			kind = (fs->chunks[bt_chunk_containing(fs, record.key.objectid)].type &
				   BT_BLOCK_SYSTEM) != 0
			    ? BT_BLOCK_SYSTEM
			    : BT_BLOCK_METADATA;
			copies = physical_copies(fs, record.key.objectid);
			redundant =
			    fs->chunks[bt_chunk_containing(fs, record.key.objectid)].mirrors > 1;
			if (redundant && blocks++ % CORRUPTION_STRIDE == 0) {
				corrupt_copy(volume, fs, record.key.objectid, fs->info.node_size,
				    kind, (unsigned)(blocks % copies));
				expected->corrupted++;
				expected->tree++;
			}
		} else if (record.key.type == BT_EXTENT_ITEM &&
		    (flags & BT_EXTENT_FLAG_DATA) != 0) {
			for (sector = record.key.objectid;
			    sector < record.key.objectid + record.key.offset;
			    sector += fs->info.sector_size) {
				if (!has_checksum(fs, sector)) {
					continue;
				}
				position = sectors++;
				if (position % CORRUPTION_STRIDE == PARITY_RESIDUE &&
				    parity_chunk(fs, sector) && !row_taken(fs, expected, sector)) {
					parities =
					    fs->chunks[bt_chunk_containing(fs, sector)].type &
						BT_BLOCK_RAID6
					    ? 2U
					    : 1U;
					corrupt_parity(volume, fs, sector,
					    (unsigned)(position / CORRUPTION_STRIDE % parities));
					expected->parity++;
					continue;
				}
				if (position % CORRUPTION_STRIDE != 0 ||
				    row_taken(fs, expected, sector)) {
					continue;
				}
				copies = physical_copies(fs, sector);
				corrupt_copy(volume, fs, sector, fs->info.sector_size,
				    BT_BLOCK_DATA, (unsigned)(sectors % copies));
				expected->corrupted++;
				expected->data++;
				expected->unrepairable +=
				    fs->chunks[bt_chunk_containing(fs, sector)].mirrors == 1;
			}
		}
		result = bt_cursor_next(&cursor);
	}
	REQUIRE(result == BTRFS_NOT_FOUND);
	bt_cursor_fini(&cursor);
	REQUIRE(bt_super_present(fs->devices[fs->device_count - 1].size, SUPER_MIRROR));
	corrupt_device(
	    volume, (uint32_t)(volume->devices - 1), bt_super_offset(SUPER_MIRROR), BT_SUPER_SIZE);
	btrfs_unmount(fs);
	btrfs_image_close(&image);
}

static void
digest_of(const struct volume *volume, struct namespace_digest *digest, int audit)
{
	struct reference_audit references;
	struct btrfs_image image;
	struct btrfs_fs *fs;

	open_volume(volume, &image, 0);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(namespace_content_digest(fs, digest) == 0);
	if (audit && reference_audit(fs, &references) != 0) {
		fprintf(stderr, "reference audit: %s\n", references.failure);
		exit(1);
	}
	btrfs_unmount(fs);
	REQUIRE(image.live_allocations == 0);
	btrfs_image_close(&image);
}

/* With --faults: every allocation and every device read of a scrub fails in
 * turn; the scrub then fails (NO_MEMORY, or for a read the copy unreadable
 * or the mount IO) and releases everything. */
static void
fault_sweep(const struct volume *volume, size_t *allocations, size_t *reads)
{
	struct btrfs_scrub_report report;
	struct btrfs_image image;
	uint64_t total;
	uint64_t point;
	enum btrfs_result result;

	for (point = 1;; point++) {
		open_volume(volume, &image, 0);
		image.fail_allocate = point;
		result = btrfs_scrub(&image.environment, NULL, &report);
		REQUIRE(image.live_allocations == 0);
		btrfs_image_close(&image);
		if (result == BTRFS_OK) {
			break;
		}
		REQUIRE(result == BTRFS_NO_MEMORY);
		(*allocations)++;
	}
	open_volume(volume, &image, 0);
	REQUIRE(btrfs_scrub(&image.environment, NULL, &report) == BTRFS_OK);
	total = image.reads;
	btrfs_image_close(&image);
	for (point = 1; point <= total; point++) {
		open_volume(volume, &image, 0);
		image.fail_read = point;
		result = btrfs_scrub(&image.environment, NULL, &report);
		REQUIRE(image.live_allocations == 0);
		btrfs_image_close(&image);
		if (result != BTRFS_IO && result != BTRFS_CORRUPT && result != BTRFS_NOT_BTRFS) {
			fprintf(stderr, "read %llu of %llu: %s\n", (unsigned long long)point,
			    (unsigned long long)total, btrfs_result_string(result));
			exit(1);
		}
		REQUIRE(
		    result != BTRFS_CORRUPT || report.unreadable != 0 || report.super_errors == 1);
		(*reads)++;
	}
}

/* Rewrites the corrupted superblock copy from the others. */
static void
recover(const struct volume *volume)
{
	struct btrfs_write_environment writer;
	struct btrfs_recovery_report report;
	struct btrfs_image image;

	open_volume(volume, &image, 1);
	btrfs_image_writer(&image, &writer);
	REQUIRE(btrfs_recover_supers(&image.environment, &writer, 0, &report) == BTRFS_OK &&
	    report.rewritten == 1);
	btrfs_image_close(&image);
}

int
main(int argc, char **argv)
{
	static struct expectation expected;
	struct btrfs_scrub_report clean;
	struct btrfs_scrub_report report;
	struct namespace_digest before;
	struct namespace_digest after;
	struct volume volume;
	const char *name;
	size_t allocation_points = 0;
	size_t read_points = 0;
	size_t i;
	int keep;
	int faults;

	keep = argc > 1 && strcmp(argv[1], "--keep") == 0;
	argv += keep;
	argc -= keep;
	faults = argc > 1 && strcmp(argv[1], "--faults") == 0;
	argv += faults;
	argc -= faults;
	REQUIRE(argc >= 4);
	name = argv[1];
	volume.devices = (size_t)(argc - 3);
	REQUIRE(volume.devices <= MAX_DEVICES);
	for (i = 0; i < volume.devices; i++) {
		REQUIRE(snprintf(volume.paths[i], sizeof(volume.paths[i]), "%s/%s-%ld-%zu.raw",
			    argv[2], name, (long)getpid(), i) < (int)PATH_BYTES);
		REQUIRE(clonefile(argv[3 + i], volume.paths[i], 0) == 0);
		volume.path_list[i] = volume.paths[i];
	}

	/* A clean volume scrubs clean, read-only and with a writer; its
	 * RAID5/6 rows hold Linux's parity. */
	REQUIRE(scrub(&volume, 0, &clean) == BTRFS_OK && clean.corrupted == 0 &&
	    clean.unreadable == 0 && clean.super_errors == 0 && clean.parity_errors == 0);
	REQUIRE(scrub(&volume, 1, &report) == BTRFS_OK && report.repaired == 0);
	digest_of(&volume, &before, 1);
	if (faults) {
		fault_sweep(&volume, &allocation_points, &read_points);
	}

	corrupt(&volume, &expected);
	REQUIRE(expected.corrupted != 0 && (expected.parity != 0) == (clean.parity_rows != 0));
	/* Without a writer: every corrupted copy and parity sector found,
	 * nothing written. */
	REQUIRE(scrub(&volume, 0, &report) == BTRFS_CORRUPT);
	REQUIRE(report.corrupted == expected.corrupted &&
	    report.unrepairable == expected.unrepairable && report.repaired == 0 &&
	    report.tree_copies == clean.tree_copies && report.data_copies == clean.data_copies &&
	    report.super_errors == 1 && report.parity_errors == expected.parity);
	REQUIRE(
	    scrub(&volume, 0, &report) == BTRFS_CORRUPT && report.corrupted == expected.corrupted);
	/* With a writer: every copy with a good one and every parity sector is
	 * rewritten; the superblock copy still fails. */
	REQUIRE(scrub(&volume, 1, &report) == BTRFS_CORRUPT);
	REQUIRE(report.repaired == expected.corrupted - expected.unrepairable + expected.parity &&
	    report.unrepairable == expected.unrepairable && report.super_errors == 1 &&
	    report.parity_errors == expected.parity);
	REQUIRE(scrub(&volume, 0, &report) == BTRFS_CORRUPT);
	REQUIRE(report.corrupted == expected.unrepairable && report.repaired == 0 &&
	    report.super_errors == 1 && report.parity_errors == 0);
	recover(&volume);
	REQUIRE(
	    scrub(&volume, 0, &report) == (expected.unrepairable != 0 ? BTRFS_CORRUPT : BTRFS_OK) &&
	    report.super_errors == 0);
	if (expected.unrepairable == 0) {
		digest_of(&volume, &after, 1);
		REQUIRE(after.hash == before.hash && after.objects == before.objects &&
		    after.bytes == before.bytes);
	}
	for (i = 0; !keep && i < volume.devices; i++) {
		REQUIRE(unlink(volume.paths[i]) == 0);
	}
	printf("scrub %s: %zu devices, %llu tree block and %llu data sector copies verified, %llu "
	       "data sectors without checksums, %llu RAID5/6 rows holding Linux's parity; %llu "
	       "tree block and %llu data sector copies and %llu parity sectors corrupted, all "
	       "found without writing, %llu copies repaired from another copy and %llu with "
	       "none left as found, the parity rewritten; a superblock copy left to recovery; "
	       "%zu allocation and %zu read failures fail the scrub cleanly PASS\n",
	    name, volume.devices, (unsigned long long)clean.tree_copies,
	    (unsigned long long)clean.data_copies, (unsigned long long)clean.unchecked,
	    (unsigned long long)clean.parity_rows, (unsigned long long)expected.tree,
	    (unsigned long long)expected.data, (unsigned long long)expected.parity,
	    (unsigned long long)(expected.corrupted - expected.unrepairable),
	    (unsigned long long)expected.unrepairable, allocation_points, read_points);
	return 0;
}
