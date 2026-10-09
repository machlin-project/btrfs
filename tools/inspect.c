/* SPDX-License-Identifier: BSD-3-Clause */
#include "../adapters/posix/image.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Directory depth and path length the walk command follows. */
#define WALK_DEPTH 256U
#define WALK_PATH 4096U
#define WALK_BUFFER (1024U * 1024U)

static void
print_hex(const uint8_t *bytes, size_t length)
{
	size_t i;

	for (i = 0; i < length; i++) {
		printf("%02x", bytes[i]);
	}
}

/* One JSON line for inode at path: its attributes, xattrs and, with data, a
 * regular file's or symlink's bytes; names and bytes are hexadecimal. */
static enum btrfs_result
walk_print(struct btrfs_fs *fs, const struct btrfs_inode *inode, const char *path, int data,
    uint8_t *buffer, uint8_t *value)
{
	uint32_t type = inode->mode & BTRFS_MODE_TYPE;
	uint64_t offset = 0;
	size_t names;
	size_t length;
	size_t position;
	size_t name_length;
	enum btrfs_result error;

	error = btrfs_list_xattrs(fs, inode, buffer, WALK_BUFFER, &names);
	if (error != BTRFS_OK) {
		return error;
	}
	printf("{\"path\":\"");
	print_hex((const uint8_t *)path, strlen(path));
	printf("\",\"tree\":%" PRIu64 ",\"inode\":%" PRIu64 ",\"mode\":%u,\"uid\":%u,\"gid\":%u"
	       ",\"links\":%u,\"size\":%" PRIu64 ",\"allocated\":%" PRIu64 ",\"mtime\":%" PRId64
	       ",\"xattrs\":[",
	    inode->id.tree, inode->id.inode, inode->mode, inode->uid, inode->gid, inode->links,
	    inode->size, inode->allocated_bytes, inode->modify_time.seconds);
	for (position = 0; position < names; position += name_length + 1) {
		name_length = strnlen((const char *)buffer + position, names - position);
		error = btrfs_get_xattr(
		    fs, inode, buffer + position, name_length, value, WALK_BUFFER, &length);
		if (error != BTRFS_OK) {
			return error;
		}
		printf("%s[\"", position == 0 ? "" : ",");
		print_hex(buffer + position, name_length);
		printf("\",\"");
		print_hex(value, length);
		printf("\"]");
	}
	printf("]");
	if (data && (type == BTRFS_MODE_REGULAR || type == BTRFS_MODE_SYMLINK)) {
		printf(",\"data\":\"");
		while (offset < inode->size) {
			error = btrfs_read(fs, inode, offset, buffer, WALK_BUFFER, &length);
			if (error != BTRFS_OK) {
				return error;
			}
			if (length == 0) {
				return BTRFS_CORRUPT;
			}
			print_hex(buffer, length);
			offset += length;
		}
		printf("\"");
	}
	printf("}\n");
	return BTRFS_OK;
}

/* Every path below directory, depth first in directory index order. */
static enum btrfs_result
walk(struct btrfs_fs *fs, const struct btrfs_inode *directory, char *path, size_t length,
    unsigned depth, int data, uint8_t *buffer, uint8_t *value)
{
	struct btrfs_directory *stream;
	struct btrfs_dir_entry entry;
	struct btrfs_inode inode;
	uint64_t cookie = 0;
	enum btrfs_result error;

	if (depth == WALK_DEPTH) {
		return BTRFS_UNSUPPORTED;
	}
	error = btrfs_directory_open(fs, directory, cookie, &stream);
	if (error != BTRFS_OK) {
		return error;
	}
	while ((error = btrfs_directory_next(stream, &entry, &cookie)) == BTRFS_OK) {
		if (length + 1 + entry.name_length >= WALK_PATH) {
			error = BTRFS_UNSUPPORTED;
			break;
		}
		path[length] = '/';
		memcpy(path + length + 1, entry.name, entry.name_length);
		path[length + 1 + entry.name_length] = '\0';
		error = btrfs_directory_inode(stream, &entry, &inode);
		if (error == BTRFS_OK) {
			error = walk_print(fs, &inode, path, data, buffer, value);
		}
		if (error == BTRFS_OK && (inode.mode & BTRFS_MODE_TYPE) == BTRFS_MODE_DIRECTORY) {
			error = walk(fs, &inode, path, length + 1 + entry.name_length, depth + 1,
			    data, buffer, value);
		}
		path[length] = '\0';
		if (error != BTRFS_OK) {
			break;
		}
	}
	btrfs_directory_close(stream);
	return error == BTRFS_NOT_FOUND ? BTRFS_OK : error;
}

static enum btrfs_result
walk_tree(struct btrfs_fs *fs, int data)
{
	struct btrfs_inode root;
	char *path = malloc(WALK_PATH);
	uint8_t *buffer = malloc(WALK_BUFFER);
	uint8_t *value = malloc(WALK_BUFFER);
	enum btrfs_result error = BTRFS_NO_MEMORY;

	if (path != NULL && buffer != NULL && value != NULL) {
		strcpy(path, ".");
		error = btrfs_root(fs, &root);
		if (error == BTRFS_OK) {
			error = walk_print(fs, &root, path, data, buffer, value);
		}
		if (error == BTRFS_OK) {
			error = walk(fs, &root, path, 1, 0, data, buffer, value);
		}
	}
	free(path);
	free(buffer);
	free(value);
	return error;
}

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
		       ",\"sector_size\":%u,\"node_size\":%u,\"checksum_type\":%u"
		       ",\"tree\":%" PRIu64 "}\n",
		    info.generation, info.sector_size, info.node_size, info.checksum_type,
		    info.default_tree);
		return BTRFS_OK;
	}
	if (strcmp(argv[0], "walk") == 0) {
		return walk_tree(fs, argc >= 2 && strcmp(argv[1], "--data") == 0);
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

/* Explicit superblock recovery, never implied by any other command. Without
 * --apply only the decision is printed and nothing is written; exit status 3
 * then means RECOVERY_REQUIRED. --acknowledged refuses to select a generation
 * below the last one the caller saw committed. */
static int
recover(const char *path, int argc, char **argv)
{
	struct btrfs_recovery_report report;
	struct btrfs_write_environment writer;
	struct btrfs_image image;
	uint64_t acknowledged = 0;
	unsigned i;
	int apply = 0;
	int argument;
	enum btrfs_result error;

	for (argument = 0; argument < argc; argument++) {
		if (strcmp(argv[argument], "--apply") == 0) {
			apply = 1;
		} else if (strcmp(argv[argument], "--acknowledged") == 0 && argument + 1 < argc) {
			acknowledged = strtoull(argv[++argument], NULL, 0);
		} else {
			fprintf(stderr,
			    "Usage: btrfs-inspect IMAGE recover [--apply] "
			    "[--acknowledged GENERATION]\n");
			return 2;
		}
	}
	if ((apply ? btrfs_image_open_writable(path, &image) : btrfs_image_open(path, &image)) !=
	    0) {
		perror("open image");
		return 1;
	}
	btrfs_image_writer(&image, &writer);
	error =
	    btrfs_recover_supers(&image.environment, apply ? &writer : NULL, acknowledged, &report);
	printf("{\"result\":\"%s\",\"apply\":%s,\"generation\":%" PRIu64
	       ",\"selected\":%u,\"rewritten\":%u,\"copies\":[",
	    btrfs_result_string(error), apply ? "true" : "false", report.generation,
	    report.selected, report.rewritten);
	for (i = 0; i < BTRFS_SUPER_COPIES; i++) {
		printf("%s{\"offset\":%" PRIu64 ",\"status\":\"%s\",\"generation\":%" PRIu64
		       ",\"current\":%s}",
		    i == 0 ? "" : ",", report.copies[i].offset,
		    btrfs_result_string(report.copies[i].status), report.copies[i].generation,
		    report.copies[i].current ? "true" : "false");
	}
	printf("]}\n");
	btrfs_image_close(&image);
	if (image.live_allocations != 0 || image.live_bytes != 0) {
		fprintf(stderr, "allocation leak\n");
		return 1;
	}
	return error == BTRFS_OK ? 0 : error == BTRFS_RECOVERY_REQUIRED ? 3 : 1;
}

/* Explicit tree-log replay, never implied by any other command. Without
 * --apply the log is replayed without a commit and nothing is written; exit
 * status 3 then means it replays and is pending, 4 that no log is pending. */
static int
replay(const char *path, int argc, char **argv)
{
	struct btrfs_replay_report report;
	struct btrfs_write_environment writer;
	struct btrfs_image image;
	struct btrfs_time now = { 0 };
	struct timespec clock;
	int apply = argc == 1 && strcmp(argv[0], "--apply") == 0;
	enum btrfs_result error;

	if (argc != 0 && !apply) {
		fprintf(stderr, "Usage: btrfs-inspect IMAGE replay [--apply]\n");
		return 2;
	}
	if ((apply ? btrfs_image_open_writable(path, &image) : btrfs_image_open(path, &image)) !=
	    0) {
		perror("open image");
		return 1;
	}
	if (clock_gettime(CLOCK_REALTIME, &clock) == 0) {
		now.seconds = clock.tv_sec;
		now.nanoseconds = (uint32_t)clock.tv_nsec;
	}
	btrfs_image_writer(&image, &writer);
	error = btrfs_replay_log(&image.environment, apply ? &writer : NULL, now, &report);
	printf("{\"result\":\"%s\",\"apply\":%s,\"generation\":%" PRIu64 ",\"logs\":%" PRIu64
	       ",\"inodes\":%" PRIu64 ",\"names\":%" PRIu64 ",\"unlinked\":%" PRIu64
	       ",\"extents\":%" PRIu64 ",\"allocated\":%" PRIu64 ",\"orphans\":%" PRIu64 "}\n",
	    btrfs_result_string(error), apply ? "true" : "false", report.generation, report.logs,
	    report.inodes, report.names, report.unlinked, report.extents, report.allocated,
	    report.orphans);
	btrfs_image_close(&image);
	if (image.live_allocations != 0 || image.live_bytes != 0) {
		fprintf(stderr, "allocation leak\n");
		return 1;
	}
	return error == BTRFS_OK	       ? 0
	    : error == BTRFS_RECOVERY_REQUIRED ? 3
	    : error == BTRFS_NOT_FOUND	       ? 4
					       : 1;
}

/* Node cache for one command; the image does not change while it runs. */
#define INSPECT_CACHE_BYTES (16U * 1024U * 1024U)

int
main(int argc, char **argv)
{
	struct btrfs_image image;
	struct btrfs_cache *cache = NULL;
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
		    "[PATH] [ARGS]\n       btrfs-inspect [--tree ID] IMAGE walk [--data]\n       "
		    "btrfs-inspect IMAGE recover [--apply] [--acknowledged "
		    "GENERATION]\n       btrfs-inspect IMAGE replay [--apply]\n");
		return 2;
	}
	if (strcmp(argv[argument + 1], "recover") == 0) {
		return recover(argv[argument], argc - argument - 2, argv + argument + 2);
	}
	if (strcmp(argv[argument + 1], "replay") == 0) {
		return replay(argv[argument], argc - argument - 2, argv + argument + 2);
	}
	if (btrfs_image_open(argv[argument], &image) != 0) {
		perror("open image");
		return 1;
	}
	error = btrfs_cache_create(&image.environment, NULL, INSPECT_CACHE_BYTES, &cache);
	image.environment.cache = cache;
	if (error == BTRFS_OK) {
		error = btrfs_mount(&image.environment, tree, &fs);
	}
	if (error == BTRFS_OK) {
		error = inspect(fs, argc - argument - 1, argv + argument + 1);
	}
	btrfs_unmount(fs);
	btrfs_cache_destroy(cache);
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
