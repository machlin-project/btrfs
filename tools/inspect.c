/* SPDX-License-Identifier: BSD-3-Clause */
#include "../adapters/posix/image.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum btrfs_result
inspect(struct btrfs_fs *fs, int argc, char **argv)
{
	struct btrfs_info info;
	struct btrfs_inode inode;
	struct btrfs_dir_entry entry;
	struct btrfs_directory *stream;
	uint8_t *buffer;
	uint64_t cookie = 0;
	uint64_t offset;
	uint64_t remaining;
	size_t length;
	size_t count;
	size_t i;
	enum btrfs_result error;
	const size_t buffer_size = 1024U * 1024U;

	if (strcmp(argv[0], "info") == 0) {
		btrfs_get_info(fs, &info);
		printf("{\"generation\":%" PRIu64
		       ",\"sector_size\":%u,\"node_size\":%u,\"tree\":%" PRIu64 "}\n",
		    info.generation, info.sector_size, info.node_size, info.default_tree);
		return BTRFS_OK;
	}
	if (argc < 2) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = btrfs_image_lookup(fs, argv[1], &inode);
	if (error != BTRFS_OK) {
		return error;
	}
	if (strcmp(argv[0], "stat") == 0) {
		printf("{\"tree\":%" PRIu64 ",\"inode\":%" PRIu64 ",\"size\":%" PRIu64
		       ",\"allocated\":%" PRIu64
		       ",\"mode\":%u,\"uid\":%u,\"gid\":%u,\"links\":%u}\n",
		    inode.id.tree, inode.id.inode, inode.size, inode.allocated_bytes, inode.mode,
		    inode.uid, inode.gid, inode.links);
		return BTRFS_OK;
	}
	if (strcmp(argv[0], "ls") == 0) {
		error = btrfs_directory_open(fs, &inode, cookie, &stream);
		if (error != BTRFS_OK) {
			return error;
		}
		while ((error = btrfs_directory_next(stream, &entry, &cookie)) == BTRFS_OK) {
			printf("%" PRIu64 "\t%" PRIu64 "\t%u\t%" PRIu64 "\t", entry.id.tree,
			    entry.id.inode, entry.type, cookie);
			for (i = 0; i < entry.name_length; i++) {
				printf("%02x", entry.name[i]);
			}
			putchar('\n');
		}
		btrfs_directory_close(stream);
		return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
	}
	buffer = malloc(buffer_size);
	if (buffer == NULL) {
		return BTRFS_NO_MEMORY;
	}
	if (strcmp(argv[0], "cat") == 0) {
		offset = argc >= 3 ? strtoull(argv[2], NULL, 0) : 0;
		remaining = argc >= 4 ? strtoull(argv[3], NULL, 0) : UINT64_MAX;
		error = BTRFS_OK;
		while (remaining != 0) {
			count = remaining < buffer_size ? (size_t)remaining : buffer_size;
			error = btrfs_read(fs, &inode, offset, buffer, count, &length);
			if (error != BTRFS_OK || length == 0) {
				break;
			}
			if (fwrite(buffer, 1, length, stdout) != length) {
				error = BTRFS_IO;
				break;
			}
			offset += length;
			remaining -= length;
		}
	} else if (strcmp(argv[0], "xattr") == 0 && argc >= 3) {
		error = btrfs_get_xattr(
		    fs, &inode, argv[2], strlen(argv[2]), buffer, buffer_size, &length);
		if (error == BTRFS_OK && fwrite(buffer, 1, length, stdout) != length) {
			error = BTRFS_IO;
		}
	} else if (strcmp(argv[0], "listxattr") == 0) {
		error = btrfs_list_xattrs(fs, &inode, buffer, buffer_size, &length);
		if (error == BTRFS_OK && fwrite(buffer, 1, length, stdout) != length) {
			error = BTRFS_IO;
		}
	} else {
		error = BTRFS_INVALID_ARGUMENT;
	}
	free(buffer);
	return error;
}

int
main(int argc, char **argv)
{
	struct btrfs_image image;
	struct btrfs_fs *fs = NULL;
	enum btrfs_result error;
	uint64_t tree = 0;
	int argument = 1;

	if (argc > 3 && strcmp(argv[1], "--tree") == 0) {
		tree = strtoull(argv[2], NULL, 0);
		argument = 3;
	}
	if (argc - argument < 2) {
		fprintf(stderr,
		    "Usage: btrfs-inspect [--tree ID] IMAGE info|stat|ls|cat|xattr|listxattr "
		    "[PATH] [ARGS]\n");
		return 2;
	}
	if (btrfs_image_open(argv[argument], &image) != 0) {
		perror("open image");
		return 1;
	}
	error = btrfs_mount(&image.environment, tree, &fs);
	if (error == BTRFS_OK) {
		error = inspect(fs, argc - argument - 1, argv + argument + 1);
	}
	btrfs_unmount(fs);
	if (getenv("BTRFS_IO_STATS") != NULL) {
		fprintf(stderr,
		    "reads=%" PRIu64 " bytes=%" PRIu64 " allocations=%" PRIu64 " live=%" PRIu64
		    "\n",
		    atomic_load(&image.reads), atomic_load(&image.bytes_read),
		    atomic_load(&image.allocations), atomic_load(&image.live_allocations));
	}
	btrfs_image_close(&image);
	if (image.live_allocations != 0 || image.live_bytes != 0) {
		fprintf(stderr, "allocation leak\n");
		return 1;
	}
	if (error != BTRFS_OK) {
		fprintf(stderr, "%s\n", btrfs_result_string(error));
	}
	return error == BTRFS_OK ? 0 : 1;
}
