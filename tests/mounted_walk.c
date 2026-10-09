/* SPDX-License-Identifier: BSD-3-Clause */
/* Guest-only walk of a mounted Btrfs volume; it only reads. Every name of every
 * directory is checked: readdir's d_ino and d_type agree with lstat, "." and
 * ".." name the directory and its parent, and a number is shared only by the
 * links of one file, never by more names than its link count.
 * For each directory it prints a native-check manifest line: "inodes", the
 * path and the SHA-256 of the directory's sorted "name<TAB>number" lines,
 * where number is the native number of an object in the mount root's
 * subvolume (its Btrfs inode number) and "-" for an object in another
 * subvolume. Linux computes the same digest from its own inode numbers and
 * devices. A directory whose path or names the manifest cannot carry is
 * checked but not listed. The volume metadata directories macOS services
 * create and remove at the root on their own schedule (fseventsd purges its
 * log directory while the volume is mounted) are left out of the walk;
 * tests/native_oracle.sh leaves out the same names. */
#define _DARWIN_C_SOURCE
#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition,  \
			    errno);                                                                \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

/* Native numbers of the mount root's subvolume are below this bound
 * (BTRFS_NATIVE_SLOT_SHIFT in include/btrfs/identity.h). */
#define ROOT_SUBVOLUME_BOUND (UINT64_C(1) << 48)
#define NATIVE_ROOT_NUMBER UINT64_C(2)
#define MAX_DEPTH 64U
#define PATH_BYTES 1024U
/* "name<TAB>number" with a name of at most NAME_MAX bytes. */
#define LINE_BYTES (NAME_MAX + 32U)
#define HEX_BYTES (CC_SHA256_DIGEST_LENGTH * 2 + 1)
#define INITIAL_OBJECTS 4096U

struct object {
	uint64_t number;
	uint64_t size;
	uint32_t links;
	uint32_t seen;
	mode_t mode;
};

struct objects {
	struct object *slots;
	size_t capacity;
	size_t count;
};

struct directory {
	char path[PATH_BYTES];
	uint64_t number;
	uint64_t parent;
	unsigned depth;
};

struct walk {
	int root;
	dev_t device;
	struct objects objects;
	struct directory *pending;
	size_t pending_count;
	size_t pending_capacity;
	char **lines;
	size_t line_capacity;
	uint64_t directories;
	uint64_t names;
	uint64_t unlisted;
};

static uint64_t
mix(uint64_t value)
{
	value ^= value >> 33;
	value *= UINT64_C(0xff51afd7ed558ccd);
	value ^= value >> 33;
	return value;
}

static struct object *
object_slot(struct objects *objects, uint64_t number)
{
	size_t mask = objects->capacity - 1U;
	size_t slot = (size_t)mix(number) & mask;

	while (objects->slots[slot].number != 0 && objects->slots[slot].number != number) {
		slot = (slot + 1U) & mask;
	}
	return &objects->slots[slot];
}

static void
grow_objects(struct objects *objects)
{
	struct objects grown;
	size_t i;

	grown.capacity = objects->capacity == 0 ? INITIAL_OBJECTS : objects->capacity * 2U;
	grown.count = objects->count;
	grown.slots = calloc(grown.capacity, sizeof(*grown.slots));
	REQUIRE(grown.slots != NULL);
	for (i = 0; i < objects->capacity; i++) {
		if (objects->slots[i].number != 0) {
			*object_slot(&grown, objects->slots[i].number) = objects->slots[i];
		}
	}
	free(objects->slots);
	*objects = grown;
}

/* The first name of an object records it; later names must be links of the
 * same file. */
static void
record_object(struct walk *walk, const struct stat *status)
{
	struct object *object;

	REQUIRE(status->st_ino != 0);
	if ((walk->objects.count + 1U) * 2U > walk->objects.capacity) {
		grow_objects(&walk->objects);
	}
	object = object_slot(&walk->objects, status->st_ino);
	if (object->number == 0) {
		object->number = status->st_ino;
		object->size = (uint64_t)status->st_size;
		object->links = (uint32_t)status->st_nlink;
		object->mode = status->st_mode;
		object->seen = 1;
		walk->objects.count++;
		return;
	}
	REQUIRE(!S_ISDIR(status->st_mode) && object->mode == status->st_mode &&
	    object->size == (uint64_t)status->st_size && object->links == status->st_nlink);
	object->seen++;
	REQUIRE(object->seen <= object->links);
}

static void
push_directory(
    struct walk *walk, const char *path, uint64_t number, uint64_t parent, unsigned depth)
{
	struct directory *directory;

	REQUIRE(depth <= MAX_DEPTH);
	if (walk->pending_count == walk->pending_capacity) {
		walk->pending_capacity =
		    walk->pending_capacity == 0 ? 64U : walk->pending_capacity * 2U;
		walk->pending =
		    realloc(walk->pending, walk->pending_capacity * sizeof(*walk->pending));
		REQUIRE(walk->pending != NULL);
	}
	directory = &walk->pending[walk->pending_count++];
	REQUIRE(strlen(path) < sizeof(directory->path));
	strcpy(directory->path, path);
	directory->number = number;
	directory->parent = parent;
	directory->depth = depth;
}

/* The manifest carries paths of portable name characters only. */
static bool
portable_path(const char *path)
{
	const char *c;

	for (c = path; *c != '\0'; c++) {
		if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
			(*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '-' ||
			*c == '/')) {
			return false;
		}
	}
	return true;
}

static int
compare_lines(const void *left, const void *right)
{
	return strcmp(*(char *const *)left, *(char *const *)right);
}

static void
print_listing(struct walk *walk, const char *path, size_t count, bool listable)
{
	CC_SHA256_CTX context;
	unsigned char hash[CC_SHA256_DIGEST_LENGTH];
	char hex[HEX_BYTES];
	size_t i;

	if (!listable || !portable_path(path)) {
		walk->unlisted++;
		fprintf(stderr, "mounted walk: %s checked, not listed\n", path);
		return;
	}
	qsort(walk->lines, count, sizeof(*walk->lines), compare_lines);
	CC_SHA256_Init(&context);
	for (i = 0; i < count; i++) {
		CC_SHA256_Update(&context, walk->lines[i], (CC_LONG)strlen(walk->lines[i]));
		CC_SHA256_Update(&context, "\n", 1);
	}
	CC_SHA256_Final(hash, &context);
	for (i = 0; i < sizeof(hash); i++) {
		snprintf(hex + i * 2, 3, "%02x", hash[i]);
	}
	printf("inodes\t%s\t%s\n", path, hex);
}

/* Directories macOS services own at a volume's root. */
static bool
volume_metadata(const char *name)
{
	static const char *const names[] = { ".fseventsd", ".Spotlight-V100", ".Trashes",
		".TemporaryItems", ".DocumentRevisions-V100" };
	size_t i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strcmp(name, names[i]) == 0) {
			return true;
		}
	}
	return false;
}

static void
walk_directory(struct walk *walk, const struct directory *current)
{
	char path[PATH_BYTES];
	struct stat status;
	struct dirent *entry;
	DIR *stream;
	size_t count = 0;
	size_t i;
	bool listable = true;
	bool dot = false;
	bool dot_dot = false;
	int file;

	file = openat(walk->root, current->path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	REQUIRE(file >= 0 && fstat(file, &status) == 0);
	REQUIRE(status.st_ino == current->number && status.st_dev == walk->device);
	if (current->depth != 0) {
		REQUIRE(fstatat(file, "..", &status, AT_SYMLINK_NOFOLLOW) == 0);
		REQUIRE(status.st_ino == current->parent);
	}
	stream = fdopendir(file);
	REQUIRE(stream != NULL);
	walk->directories++;
	errno = 0;
	while ((entry = readdir(stream)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0) {
			REQUIRE(!dot && entry->d_ino == current->number);
			dot = true;
			continue;
		}
		if (strcmp(entry->d_name, "..") == 0) {
			REQUIRE(
			    !dot_dot && (current->depth == 0 || entry->d_ino == current->parent));
			dot_dot = true;
			continue;
		}
		if (current->depth == 0 && volume_metadata(entry->d_name)) {
			continue;
		}
		REQUIRE(fstatat(file, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) == 0);
		REQUIRE(entry->d_ino == status.st_ino && status.st_dev == walk->device);
		REQUIRE(entry->d_type == DT_UNKNOWN || entry->d_type == IFTODT(status.st_mode));
		REQUIRE(status.st_ino != NATIVE_ROOT_NUMBER);
		record_object(walk, &status);
		walk->names++;
		if (strchr(entry->d_name, '\n') != NULL || strchr(entry->d_name, '\t') != NULL) {
			listable = false;
		}
		if (count == walk->line_capacity) {
			walk->line_capacity =
			    walk->line_capacity == 0 ? 256U : walk->line_capacity * 2U;
			walk->lines =
			    realloc(walk->lines, walk->line_capacity * sizeof(*walk->lines));
			REQUIRE(walk->lines != NULL);
		}
		walk->lines[count] = malloc(LINE_BYTES);
		REQUIRE(walk->lines[count] != NULL);
		if (status.st_ino < ROOT_SUBVOLUME_BOUND) {
			snprintf(walk->lines[count], LINE_BYTES, "%s\t%llu", entry->d_name,
			    (unsigned long long)status.st_ino);
		} else {
			snprintf(walk->lines[count], LINE_BYTES, "%s\t-", entry->d_name);
		}
		count++;
		if (S_ISDIR(status.st_mode)) {
			if (current->depth == 0) {
				snprintf(path, sizeof(path), "%s", entry->d_name);
			} else {
				REQUIRE(snprintf(path, sizeof(path), "%s/%s", current->path,
					    entry->d_name) < (int)sizeof(path));
			}
			push_directory(
			    walk, path, status.st_ino, current->number, current->depth + 1U);
		}
		errno = 0;
	}
	REQUIRE(errno == 0 && dot && dot_dot && closedir(stream) == 0);
	print_listing(walk, current->path, count, listable);
	for (i = 0; i < count; i++) {
		free(walk->lines[i]);
	}
}

int
main(int argc, char **argv)
{
	struct walk walk = { 0 };
	struct directory current;
	struct timespec start;
	struct timespec end;
	struct stat status;
	double seconds;

	if (argc != 2) {
		fprintf(stderr, "usage (guest): btrfs-mounted-walk-test MOUNT\n");
		return 2;
	}
	walk.root = open(argv[1], O_RDONLY | O_DIRECTORY);
	REQUIRE(walk.root >= 0 && fstat(walk.root, &status) == 0);
	REQUIRE(status.st_ino == NATIVE_ROOT_NUMBER);
	walk.device = status.st_dev;
	REQUIRE(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	push_directory(&walk, ".", status.st_ino, 0, 0);
	while (walk.pending_count != 0) {
		current = walk.pending[--walk.pending_count];
		walk_directory(&walk, &current);
	}
	REQUIRE(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
	seconds = (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
	fprintf(stderr,
	    "mounted walk: %llu directories, %llu names, %zu objects, %llu unlisted, "
	    "%.2f s, %.0f names/s PASS\n",
	    (unsigned long long)walk.directories, (unsigned long long)walk.names,
	    walk.objects.count, (unsigned long long)walk.unlisted, seconds,
	    seconds > 0 ? (double)walk.names / seconds : 0.0);
	REQUIRE(close(walk.root) == 0);
	return 0;
}
