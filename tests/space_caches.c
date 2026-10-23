/* SPDX-License-Identifier: BSD-3-Clause */
/* v1 space caches of groups that empty, on a clone of a Linux volume whose
 * three 112 MiB data groups each have one (tests/prepare_linux.py,
 * space-caches). Removing the file that fills the first two of them empties a
 * group; as Linux's btrfs_delete_unused_bgs keeps a group emptied in that
 * commit, the next transaction removes it, and with it its cache, as
 * btrfs_remove_block_group does: the free-space header and every item of the
 * cache inode go, and the cache's data extent with its reference. The other
 * caches stay as they were, the volume reads as it did after the removal, and
 * the reference and namespace audits pass. With an export directory, the
 * clone is made there and stays for Linux. */
#include "../adapters/posix/image.h"
#include "internal.h"
#include "namespace_audit.h"
#include "references.h"
#include <btrfs/write.h>
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

#define PATH_BYTES 4096U
#define MAX_CACHES 64U
#define SECONDS 1800000000
/* The fixture's file that fills the first two cached data groups. */
#define FILLER "groups"
#define EVICT_BUDGET 256U

/* A v1 space cache: its group, its inode in the root tree, that inode's size
 * and data extent. */
struct cache {
	uint64_t group;
	uint64_t inode;
	uint64_t size;
	uint64_t data;
};

static size_t
list_caches(const struct btrfs_fs *fs, struct cache *caches)
{
	const struct bt_disk_free_space_header *header;
	const struct bt_disk_extent *file;
	const struct bt_disk_inode *inode;
	struct bt_cursor cursor;
	struct bt_record record;
	size_t count = 0;
	size_t i;
	enum btrfs_result result;

	bt_cursor_init(&cursor, fs, fs->root_tree);
	result = bt_cursor_seek(&cursor, (struct bt_key){ .objectid = BT_FREE_SPACE_OBJECTID }, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		if (record.key.objectid != BT_FREE_SPACE_OBJECTID) {
			break;
		}
		REQUIRE(record.size == sizeof(*header) && count < MAX_CACHES);
		header = (const void *)record.data;
		caches[count] = (struct cache){ record.key.offset,
			bt_key_decode(&header->location).objectid, 0, 0 };
		count++;
		result = bt_cursor_next(&cursor);
	}
	REQUIRE(result == BTRFS_OK || result == BTRFS_NOT_FOUND);
	bt_cursor_fini(&cursor);
	for (i = 0; i < count; i++) {
		bt_cursor_init(&cursor, fs, fs->root_tree);
		result = bt_cursor_seek(&cursor, (struct bt_key){ .objectid = caches[i].inode }, 0);
		while (result == BTRFS_OK) {
			REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
			if (record.key.objectid != caches[i].inode) {
				break;
			}
			inode = (const void *)record.data;
			file = (const void *)record.data;
			if (record.key.type == BT_INODE_ITEM && record.size == sizeof(*inode)) {
				caches[i].size = bt_u64(inode->size);
			} else if (record.key.type == BT_EXTENT_DATA &&
			    record.size == sizeof(*file)) {
				caches[i].data = bt_u64(file->disk_bytenr);
			}
			result = bt_cursor_next(&cursor);
		}
		REQUIRE(result == BTRFS_OK || result == BTRFS_NOT_FOUND);
		bt_cursor_fini(&cursor);
	}
	return count;
}

/* Whether any item of the root tree has objectid. */
static int
root_items(const struct btrfs_fs *fs, uint64_t objectid)
{
	struct bt_cursor cursor;
	struct bt_record record;
	int found = 0;

	bt_cursor_init(&cursor, fs, fs->root_tree);
	if (bt_cursor_seek(&cursor, (struct bt_key){ .objectid = objectid }, 0) == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		found = record.key.objectid == objectid;
	}
	bt_cursor_fini(&cursor);
	return found;
}

/* Whether the extent tree holds an extent item at logical. */
static int
extent_present(const struct btrfs_fs *fs, uint64_t logical)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root extents;
	int found = 0;

	REQUIRE(bt_find_root(fs, BT_EXTENT_TREE, &extents) == BTRFS_OK);
	bt_cursor_init(&cursor, fs, extents);
	if (bt_cursor_seek(&cursor, (struct bt_key){ .objectid = logical }, 0) == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		found = record.key.objectid == logical && record.key.type == BT_EXTENT_ITEM;
	}
	bt_cursor_fini(&cursor);
	return found;
}

static int
group_present(const struct btrfs_fs *fs, uint64_t logical)
{
	size_t i;

	for (i = 0; i < fs->chunk_count; i++) {
		if (fs->chunks[i].logical == logical) {
			return 1;
		}
	}
	return 0;
}

static void
audit(const struct btrfs_fs *fs, const char *when)
{
	struct reference_audit references;
	struct namespace_audit names;

	if (reference_audit(fs, &references) != 0) {
		fprintf(stderr, "%s: reference audit: %s\n", when, references.failure);
		exit(1);
	}
	if (namespace_audit(fs, &names) != 0) {
		fprintf(stderr, "%s: namespace audit: %s\n", when, names.failure);
		exit(1);
	}
}

int
main(int argc, char **argv)
{
	static struct cache before[MAX_CACHES];
	static struct cache after[MAX_CACHES];
	struct btrfs_write_environment writer;
	struct btrfs_transaction *transaction;
	struct btrfs_time time = { SECONDS, 0 };
	struct namespace_digest emptied;
	struct namespace_digest removed_digest;
	struct btrfs_object_id id;
	struct btrfs_image image;
	struct btrfs_inode inode;
	struct btrfs_inode root;
	struct btrfs_fs *fs;
	char path[PATH_BYTES];
	size_t before_count;
	size_t after_count;
	size_t removed = 0;
	size_t gone = 0;
	size_t kept = 0;
	size_t i;
	size_t j;
	int done;
	int found;

	REQUIRE(argc == 3 || argc == 4);
	/* The clone lies in the export directory when there is one. */
	REQUIRE(snprintf(path, sizeof(path), "%s/space-caches-%ld.raw", argv[argc == 4 ? 3 : 2],
		    (long)getpid()) < (int)sizeof(path));
	REQUIRE(clonefile(argv[1], path, 0) == 0);
	REQUIRE(btrfs_image_open_writable(path, &image) == 0);
	btrfs_image_writer(&image, &writer);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	before_count = list_caches(fs, before);
	REQUIRE(before_count >= 2);
	audit(fs, "image");

	/* The file goes, its extents with it at commit. */
	REQUIRE(btrfs_root(fs, &root) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/" FILLER, &inode) == BTRFS_OK);
	REQUIRE(btrfs_transaction_begin(fs, &writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_unlink(transaction, root.id, FILLER, strlen(FILLER), time, 0) ==
	    BTRFS_OK);
	while (btrfs_transaction_take_deferred(transaction, &id) == BTRFS_OK) {
		done = 0;
		while (!done) {
			REQUIRE(btrfs_transaction_evict(transaction, id, EVICT_BUDGET, &done) ==
			    BTRFS_OK);
		}
	}
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/" FILLER, &inode) == BTRFS_NOT_FOUND);
	REQUIRE(namespace_content_digest(fs, &emptied) == 0);

	/* The emptied group goes with its cache. */
	REQUIRE(btrfs_transaction_begin(fs, &writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_remove_unused_groups(transaction, &removed) == BTRFS_OK);
	REQUIRE(removed > 0);
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	after_count = list_caches(fs, after);
	for (i = 0; i < before_count; i++) {
		found = 0;
		for (j = 0; j < after_count; j++) {
			if (after[j].group == before[i].group) {
				found = 1;
				REQUIRE(after[j].inode == before[i].inode &&
				    after[j].size == before[i].size &&
				    after[j].data == before[i].data);
			}
		}
		if (group_present(fs, before[i].group)) {
			REQUIRE(found);
			kept++;
			continue;
		}
		REQUIRE(!found);
		REQUIRE(!root_items(fs, before[i].inode));
		REQUIRE(!extent_present(fs, before[i].data));
		gone++;
	}
	REQUIRE(gone > 0 && kept > 0);
	REQUIRE(namespace_content_digest(fs, &removed_digest) == 0 &&
	    removed_digest.hash == emptied.hash && removed_digest.objects == emptied.objects &&
	    removed_digest.bytes == emptied.bytes);
	audit(fs, "groups removed");
	btrfs_unmount(fs);
	REQUIRE(image.live_allocations == 0);
	btrfs_image_close(&image);
	if (argc == 3) {
		REQUIRE(unlink(path) == 0);
	}
	printf("space caches: %zu groups removed once emptied, %zu of them with their v1 space "
	       "cache (header, inode and data), %zu caches kept as they were; the volume reads "
	       "the same and passes both audits PASS\n",
	    removed, gone, kept);
	return 0;
}
