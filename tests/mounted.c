/* SPDX-License-Identifier: BSD-3-Clause */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#define PAYLOAD_SIZE (4U * 1024U * 1024U)
#define DIRECTORY_COUNT 700U
#define WORKER_COUNT 8U
#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition,  \
			    errno);                                                                \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

struct worker {
	int root;
	unsigned index;
	int error;
};

/* A mount without hole reporting (FSKit 26.x has no interface for it). */
static bool no_seek_hole;

static void *
read_worker(void *context)
{
	struct worker *worker = context;
	uint8_t bytes[8193];
	size_t offset;
	size_t i;
	unsigned round;
	int file;

	for (round = 0; round < 64 && worker->error == 0; round++) {
		file = openat(worker->root, "big", O_RDONLY);
		if (file < 0) {
			worker->error = errno;
			break;
		}
		offset = (worker->index * 65537U + round * 32003U) % (PAYLOAD_SIZE - sizeof(bytes));
		if (pread(file, bytes, sizeof(bytes), (off_t)offset) != sizeof(bytes)) {
			worker->error = EIO;
		}
		for (i = 0; i < sizeof(bytes) && worker->error == 0; i++) {
			if (bytes[i] != (uint8_t)(offset + i)) {
				worker->error = EIO;
			}
		}
		if (close(file) != 0) {
			worker->error = errno;
		}
	}
	return NULL;
}

static void
check_identity(int root)
{
	struct stat greeting;
	struct stat hardlink;
	struct stat original;
	struct stat snapshot;
	struct stat parent;
	struct stat mounted;
	char bytes[64];
	int file;
	ssize_t length;

	REQUIRE(fstatat(root, "greeting", &greeting, 0) == 0);
	REQUIRE(fstatat(root, "hardlink", &hardlink, 0) == 0);
	REQUIRE(greeting.st_ino == hardlink.st_ino && greeting.st_nlink == 2);
	REQUIRE(greeting.st_uid == 1001 && greeting.st_gid == 1002);
	REQUIRE((greeting.st_mode & 07777) == 0640);
	REQUIRE(readlinkat(root, "symlink", bytes, sizeof(bytes)) == 8);
	REQUIRE(memcmp(bytes, "greeting", 8) == 0);
	file = openat(root, "symlink", O_RDONLY);
	REQUIRE(file >= 0);
	REQUIRE(read(file, bytes, sizeof(bytes)) == sizeof("hello from Linux Btrfs\n") - 1);
	REQUIRE(
	    memcmp(bytes, "hello from Linux Btrfs\n", sizeof("hello from Linux Btrfs\n") - 1) == 0);
#ifdef __APPLE__
	length = fgetxattr(file, "user.binary", bytes, sizeof(bytes), 0, 0);
#else
	length = fgetxattr(file, "user.binary", bytes, sizeof(bytes));
#endif
	REQUIRE(length == 4 && memcmp(bytes, "\0\1\177\377", 4) == 0);
	errno = 0;
#ifdef __APPLE__
	length = fgetxattr(file, "user.binary", bytes, 1, 0, 0);
#else
	length = fgetxattr(file, "user.binary", bytes, 1);
#endif
	REQUIRE(length == -1 && errno == ERANGE);
#ifdef __APPLE__
	length = flistxattr(file, bytes, sizeof(bytes), 0);
#else
	length = flistxattr(file, bytes, sizeof(bytes));
#endif
	REQUIRE(length == sizeof("user.text\0user.binary"));
	REQUIRE(close(file) == 0);
	REQUIRE(fstatat(root, "subvol", &original, 0) == 0);
	REQUIRE(fstatat(root, "snapshot", &snapshot, 0) == 0);
	REQUIRE(original.st_ino != snapshot.st_ino || original.st_dev != snapshot.st_dev);
	REQUIRE(fstatat(root, "subvol/..", &parent, 0) == 0 && fstat(root, &mounted) == 0);
	REQUIRE(parent.st_ino == mounted.st_ino);
	file = openat(root, "snapshot/value", O_RDONLY);
	REQUIRE(file >= 0 && read(file, bytes, sizeof(bytes)) == 18);
	REQUIRE(memcmp(bytes, "snapshot original\n", 18) == 0);
	REQUIRE(close(file) == 0);
	file = openat(root, "subvol/value", O_RDONLY);
	REQUIRE(file >= 0 && read(file, bytes, sizeof(bytes)) == 18);
	REQUIRE(memcmp(bytes, "subvolume changed\n", 18) == 0);
	REQUIRE(close(file) == 0);
	file = openat(root, "raw-\xff", O_RDONLY);
	REQUIRE(file >= 0 && read(file, bytes, sizeof(bytes)) == 9);
	REQUIRE(memcmp(bytes, "raw name\n", 9) == 0);
	REQUIRE(close(file) == 0);
}

static void
check_directory(int root)
{
	bool seen[DIRECTORY_COUNT] = { false };
	DIR *directory;
	struct dirent *entry;
	unsigned count = 0;
	unsigned index;
	char extra;
	char saved[256];
	long cookie;
	int file;

	file = openat(root, "many", O_RDONLY | O_DIRECTORY);
	REQUIRE(file >= 0);
	directory = fdopendir(file);
	REQUIRE(directory != NULL);
	errno = 0;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		REQUIRE(sscanf(entry->d_name, "entry-%u%c", &index, &extra) == 1);
		REQUIRE(index < DIRECTORY_COUNT && !seen[index]);
		seen[index] = true;
		count++;
	}
	REQUIRE(errno == 0 && count == DIRECTORY_COUNT);
	rewinddir(directory);
	REQUIRE(readdir(directory) != NULL);
	cookie = telldir(directory);
	REQUIRE(cookie >= 0);
	entry = readdir(directory);
	REQUIRE(entry != NULL && strlen(entry->d_name) < sizeof(saved));
	strcpy(saved, entry->d_name);
	seekdir(directory, cookie);
	entry = readdir(directory);
	REQUIRE(entry != NULL && strcmp(entry->d_name, saved) == 0);
	REQUIRE(closedir(directory) == 0);
}

static void
check_data(int root)
{
	uint8_t *mapping;
	uint8_t *bytes;
	struct stat status;
	size_t i;
	int file;

	file = openat(root, "big", O_RDONLY);
	REQUIRE(file >= 0);
	mapping = mmap(NULL, PAYLOAD_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, file, 0);
	REQUIRE(mapping != MAP_FAILED);
	REQUIRE(close(file) == 0);
	for (i = 0; i < PAYLOAD_SIZE; i++) {
		REQUIRE(mapping[i] == (uint8_t)i);
	}
	mapping[0] = 91;
	file = openat(root, "big", O_RDONLY);
	REQUIRE(file >= 0);
	bytes = malloc(8388608);
	REQUIRE(bytes != NULL);
	REQUIRE(pread(file, bytes, 1, 0) == 1 && bytes[0] == 0);
	REQUIRE(pread(file, bytes, 10, PAYLOAD_SIZE) == 0);
	REQUIRE(close(file) == 0 && munmap(mapping, PAYLOAD_SIZE) == 0);
	file = openat(root, "sparse", O_RDONLY);
	REQUIRE(file >= 0 && pread(file, bytes, 8388608, 0) == 8388608);
	REQUIRE(memcmp(bytes + 17, "LEFT", 4) == 0);
	REQUIRE(memcmp(bytes + 7340035, "RIGHT", 5) == 0);
	memset(bytes + 17, 0, 4);
	memset(bytes + 7340035, 0, 5);
	for (i = 0; i < 8388608; i++) {
		REQUIRE(bytes[i] == 0);
	}
	REQUIRE(close(file) == 0);
	file = openat(root, "huge", O_RDONLY);
	REQUIRE(file >= 0 && fstat(file, &status) == 0);
	REQUIRE(status.st_size == INT64_C(17179869191));
	REQUIRE(pread(file, bytes, 64, INT64_C(17179869180)) == 11);
	for (i = 0; i < 11; i++) {
		REQUIRE(bytes[i] == 0);
	}
	REQUIRE(close(file) == 0);
	free(bytes);
}

/* lseek(SEEK_DATA or SEEK_HOLE) from offset on path, as Linux answers it;
 * expected -1 is ENXIO. Without hole reporting a mount may refuse the call
 * or report the whole file as data, as POSIX allows. */
static void
check_seek(int root, const char *path, int whence, off_t offset, off_t expected)
{
	struct stat status;
	off_t result;
	int file;

	file = openat(root, path, O_RDONLY);
	REQUIRE(file >= 0 && fstat(file, &status) == 0);
	errno = 0;
	result = lseek(file, offset, whence);
	if (no_seek_hole) {
		REQUIRE((result == -1 &&
			    (errno == ENOTSUP || errno == ENOTTY || errno == EINVAL ||
				errno == ENXIO)) ||
		    (whence == SEEK_DATA && result == offset) ||
		    (whence == SEEK_HOLE && result == status.st_size));
	} else if (expected < 0) {
		REQUIRE(result == -1 && errno == ENXIO);
	} else {
		REQUIRE(result == expected);
	}
	REQUIRE(close(file) == 0);
}

/* The sectors Linux wrote are data, never-written and preallocated ranges
 * holes, the end of a file its last hole. */
static void
check_holes(int root)
{
	check_seek(root, "sparse", SEEK_DATA, 0, 0);
	check_seek(root, "sparse", SEEK_HOLE, 0, 4096);
	check_seek(root, "sparse", SEEK_DATA, 4096, 7340032);
	check_seek(root, "sparse", SEEK_HOLE, 7340032, 7344128);
	check_seek(root, "sparse", SEEK_DATA, 7344128, -1);
	check_seek(root, "sparse", SEEK_HOLE, 8388607, 8388607);
	check_seek(root, "sparse", SEEK_HOLE, 8388608, -1);
	check_seek(root, "preallocated", SEEK_DATA, 0, -1);
	check_seek(root, "preallocated", SEEK_HOLE, 0, 0);
	check_seek(root, "huge", SEEK_DATA, 0, -1);
	check_seek(root, "huge", SEEK_HOLE, INT64_C(17179869190), INT64_C(17179869190));
	check_seek(root, "greeting", SEEK_DATA, 0, 0);
	check_seek(root, "greeting", SEEK_HOLE, 0, 23);
	check_seek(root, "greeting", SEEK_DATA, 23, -1);
}

static void
check_permission(int root, uid_t uid, gid_t gid, bool allowed)
{
	pid_t child;
	int file;
	int status;

	child = fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		if (setgid(gid) != 0 || setuid(uid) != 0) {
			_exit(2);
		}
		file = openat(root, "greeting", O_RDONLY);
		if (allowed) {
			_exit(file >= 0 ? 0 : 3);
		}
		_exit(file == -1 && errno == EACCES ? 0 : 4);
	}
	REQUIRE(waitpid(child, &status, 0) == child);
	REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int
main(int argc, char **argv)
{
	struct worker workers[WORKER_COUNT];
	pthread_t threads[WORKER_COUNT];
	struct statvfs filesystem;
	unsigned i;
	int root;
	int result;

	no_seek_hole = argc == 3 && strcmp(argv[2], "--no-seek-hole") == 0;
	if ((argc != 2 && !no_seek_hole) || geteuid() != 0) {
		fprintf(
		    stderr, "usage (guest root): btrfs-mounted-test MOUNTPOINT [--no-seek-hole]\n");
		return 2;
	}
	root = open(argv[1], O_RDONLY | O_DIRECTORY);
	REQUIRE(root >= 0 && fstatvfs(root, &filesystem) == 0);
	REQUIRE((filesystem.f_flag & ST_RDONLY) != 0);
	check_identity(root);
	check_directory(root);
	check_data(root);
	check_holes(root);
	check_permission(root, 1001, 1002, true);
	check_permission(root, 1003, 1003, false);
	for (i = 0; i < WORKER_COUNT; i++) {
		workers[i] = (struct worker){ .root = root, .index = i };
		REQUIRE(pthread_create(&threads[i], NULL, read_worker, &workers[i]) == 0);
	}
	for (i = 0; i < WORKER_COUNT; i++) {
		REQUIRE(pthread_join(threads[i], NULL) == 0 && workers[i].error == 0);
	}
	errno = 0;
	result = openat(root, "big", O_WRONLY);
	REQUIRE(result == -1 && errno == EROFS);
	errno = 0;
	result = openat(root, ".btrfs-write-probe", O_CREAT | O_EXCL | O_WRONLY, 0600);
	if (result >= 0) {
		close(result);
		unlinkat(root, ".btrfs-write-probe", 0);
	}
	REQUIRE(result == -1 && errno == EROFS);
	REQUIRE(close(root) == 0);
	printf("mounted Btrfs: namespace, identity, xattrs, permission, mmap, holes, %s, "
	       "concurrent reads, EROFS PASS\n",
	    no_seek_hole ? "hole reporting SKIPPED" : "SEEK_DATA and SEEK_HOLE");
	return 0;
}
