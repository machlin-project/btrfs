/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"
#include "../adapters/posix/image.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_REGION_LIMIT 16U

struct fuzz_region {
	uint64_t physical[2];
	size_t size;
	unsigned copies;
};
static struct btrfs_image image;
static struct btrfs_fs *base;
static struct fuzz_region regions[FUZZ_REGION_LIMIT];
static unsigned region_count;
static struct fuzz_region active;
static uint8_t patch[BT_MAX_NODE_SIZE];

static void
cleanup(void)
{
	btrfs_unmount(base);
	btrfs_image_close(&image);
}

static void
add_region(uint64_t address, size_t size, uint64_t kind)
{
	struct fuzz_region region = { .size = size, .copies = 1 };
	unsigned i;

	if (region_count == FUZZ_REGION_LIMIT) {
		return;
	}
	for (i = 0; i < region.copies; i++) {
		assert(bt_map(base, address, size, kind, i, &region.physical[i], &region.copies) ==
		    BTRFS_OK);
	}
	regions[region_count++] = region;
}

int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
	const char *path = getenv("BTRFS_FUZZ_IMAGE");
	struct btrfs_inode inode;
	struct bt_cursor cursor;
	struct bt_root root;
	struct bt_key key;
	const struct bt_disk_header *header;
	const char *names[] = { "/", "many", "big", "greeting", "subvol", "snapshot" };
	size_t i;

	(void)argc;
	(void)argv;
	if (path == NULL || btrfs_image_open(path, &image) != 0 ||
	    btrfs_mount(&image.environment, 5, &base) != BTRFS_OK) {
		fputs("BTRFS_FUZZ_IMAGE must name a valid plain fixture\n", stderr);
		exit(1);
	}
	atexit(cleanup);
	regions[region_count++] = (struct fuzz_region){ { BT_SUPER_OFFSET, 0 }, BT_SUPER_SIZE, 1 };
	add_region(base->chunk_tree.address, base->info.node_size, BT_BLOCK_SYSTEM);
	add_region(base->root_tree.address, base->info.node_size, BT_BLOCK_METADATA);
	add_region(base->checksum_tree.address, base->info.node_size, BT_BLOCK_METADATA);
	add_region(base->selected_tree.address, base->info.node_size, BT_BLOCK_METADATA);
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		assert(btrfs_image_lookup(base, names[i], &inode) == BTRFS_OK);
		assert(bt_find_root(base, inode.id.tree, &root) == BTRFS_OK);
		bt_cursor_init(&cursor, base, root);
		key = (struct bt_key){ .objectid = inode.id.inode, .type = BT_INODE_ITEM };
		assert(bt_cursor_seek(&cursor, key, 0) == BTRFS_OK);
		header = (const void *)cursor.blocks[0];
		add_region(bt_u64(header->bytenr), base->info.node_size, BT_BLOCK_METADATA);
		bt_cursor_fini(&cursor);
	}
	return 0;
}

static enum btrfs_result
fuzz_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	uint64_t start;
	uint64_t end;
	unsigned i;
	enum btrfs_result result;

	(void)context;
	result = image.environment.read(&image, offset, buffer, length);
	if (result != BTRFS_OK) {
		return result;
	}
	for (i = 0; i < active.copies; i++) {
		start = offset > active.physical[i] ? offset : active.physical[i];
		end = offset + length < active.physical[i] + active.size
		    ? offset + length
		    : active.physical[i] + active.size;
		if (start < end) {
			memcpy((uint8_t *)buffer + (size_t)(start - offset),
			    patch + (size_t)(start - active.physical[i]), (size_t)(end - start));
		}
	}
	return BTRFS_OK;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct btrfs_environment environment;
	struct btrfs_fs *fs = NULL;
	struct btrfs_inode inode;
	struct btrfs_directory *stream;
	struct btrfs_dir_entry entry;
	uint8_t buffer[512];
	uint64_t cookie;
	uint64_t live;
	uint32_t checksum;
	size_t position;
	size_t length;
	size_t completed;
	unsigned i;
	const char *names[] = { "big", "greeting", "subvol/value", "snapshot/value" };

	if (size < 4) {
		return 0;
	}
	active = regions[data[0] % region_count];
	assert(image.environment.read(&image, active.physical[0], patch, active.size) == BTRFS_OK);
	position = ((size_t)data[2] | (size_t)data[3] << 8) % active.size;
	length = size - 4 < active.size - position ? size - 4 : active.size - position;
	memcpy(patch + position, data + 4, length);
	if (data[1] & 1U) {
		checksum = ~bt_crc32c(UINT32_MAX, patch + BT_CSUM_SIZE, active.size - BT_CSUM_SIZE);
		for (i = 0; i < sizeof(checksum); i++) {
			patch[i] = (uint8_t)(checksum >> (i * 8U));
		}
	}
	live = image.live_allocations;
	environment = image.environment;
	environment.read = fuzz_read;
	if (btrfs_mount(&environment, 0, &fs) == BTRFS_OK) {
		for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			if (btrfs_image_lookup(fs, names[i], &inode) == BTRFS_OK) {
				(void)btrfs_read(fs, &inode, 0, buffer, sizeof(buffer), &completed);
				(void)btrfs_list_xattrs(
				    fs, &inode, buffer, sizeof(buffer), &completed);
			}
		}
		if (btrfs_root(fs, &inode) == BTRFS_OK &&
		    btrfs_directory_open(fs, &inode, 0, &stream) == BTRFS_OK) {
			for (i = 0; i < 16; i++) {
				if (btrfs_directory_next(stream, &entry, &cookie) != BTRFS_OK) {
					break;
				}
			}
			btrfs_directory_close(stream);
		}
	}
	btrfs_unmount(fs);
	assert(image.live_allocations == live);
	return 0;
}
