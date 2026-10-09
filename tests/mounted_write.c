/* SPDX-License-Identifier: BSD-3-Clause */
/* Guest-only write acceptance on a disposable, mounted read-write fixture copy.
 * "write" runs the write contracts in MOUNT/native; after an unmount and a new
 * mount, "verify" checks that everything persisted and prints a manifest of the
 * final namespace for an independent Linux check of the same image.
 * --skip-set-id leaves out the set-id group on a mount where it is a recorded
 * failure (FSKit 26.x: no caller credentials, stale native mode cache); the
 * verdict names the skip and the manifest omits those files. --no-punch-hole
 * requires F_PUNCHHOLE to be refused on a mount that has no such interface
 * (FSKit offers none); the verdict names it and the file keeps that block. */
#define _DARWIN_C_SOURCE
#include <CommonCrypto/CommonDigest.h>
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
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition,  \
			    errno);                                                                \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define DATA_BYTES (256U * 4096U)
#define REPLACEMENT_OFFSET 4093U
#define MAPPED_OFFSET 8191U
#define SHRUNK_BYTES 4097U
#define GROWN_BYTES 8193U
#define BIG_BYTES (8U * 1024U * 1024U)
#define CHUNK_BYTES (1024U * 1024U)
#define APPENDERS 4U
#define APPENDS 64U
#define BLOCK_BYTES 4096U
#define RENAMES 200U
#define FILL_BYTES (4U * 1024U * 1024U)
#define FILL_LIMIT 256U
/* "suid" and "suid-root", the last names of the native listing. */
#define SET_ID_FILES 2U
#define OWNER_UID 1001U
#define OWNER_GID 1002U
#define WRITER_UID 1003U
#define WRITER_GID 1003U
#define FIXED_SECONDS 1600000000
#define LINK_MODE 0640U
#define SUID_MODE 04755U
#define SHARED_MODE 04777U
/* native/sparse: written blocks, the block punched among them and the blocks
 * preallocated past its end, counted in 512-byte st_blocks units. */
#define SPARSE_BLOCKS 3U
#define PUNCHED_BLOCK 1U
#define PREALLOCATED_BLOCKS 2U
#define STAT_BLOCK_BYTES 512U

static const char replacement_unit[] = "unaligned CoW replacement";
static const char native_value[] = "macOS value";
static const char after_full[] = "space returned\n";
static const char unlinked_write[] = "open after unlink";

struct appender {
	int directory;
	uint8_t id;
};

static void
fill_data(uint8_t *bytes)
{
	size_t unit = sizeof(replacement_unit) - 1;
	size_t i;

	for (i = 0; i < DATA_BYTES; i++) {
		bytes[i] = (uint8_t)i;
	}
	for (i = 0; i < unit * 4096; i++) {
		bytes[REPLACEMENT_OFFSET + i] = (uint8_t)replacement_unit[i % unit];
	}
	memcpy(bytes + MAPPED_OFFSET, "read!!", 6);
}

static uint8_t
big_byte(size_t offset)
{
	return (uint8_t)(offset * 131U + (offset >> 12));
}

static void
expect_error(int result, int expected)
{
	REQUIRE(result == -1 && errno == expected);
}

/* The data file's final contents: the first SHRUNK_BYTES of the edited data,
 * then zeros up to GROWN_BYTES. */
static void
final_data(uint8_t *bytes)
{
	uint8_t *edited = malloc(DATA_BYTES);

	REQUIRE(edited != NULL);
	fill_data(edited);
	memcpy(bytes, edited, SHRUNK_BYTES);
	memset(bytes + SHRUNK_BYTES, 0, GROWN_BYTES - SHRUNK_BYTES);
	free(edited);
}

static void
data_contracts(int directory)
{
	uint8_t *expected = malloc(DATA_BYTES);
	uint8_t *actual = malloc(DATA_BYTES);
	uint8_t *mapping;
	size_t unit = sizeof(replacement_unit) - 1;
	size_t i;
	int file;

	REQUIRE(expected != NULL && actual != NULL);
	file = openat(directory, "data", O_CREAT | O_EXCL | O_RDWR, 0600);
	REQUIRE(file >= 0);
	expect_error(openat(directory, "data", O_CREAT | O_EXCL | O_RDWR, 0600), EEXIST);
	for (i = 0; i < DATA_BYTES; i++) {
		expected[i] = (uint8_t)i;
	}
	REQUIRE(write(file, expected, DATA_BYTES) == (ssize_t)DATA_BYTES);
	for (i = 0; i < unit * 4096; i++) {
		actual[i] = (uint8_t)replacement_unit[i % unit];
	}
	REQUIRE(pwrite(file, actual, unit * 4096, REPLACEMENT_OFFSET) == (ssize_t)(unit * 4096));
	memcpy(expected + REPLACEMENT_OFFSET, actual, unit * 4096);
	REQUIRE(pread(file, actual, DATA_BYTES, 0) == (ssize_t)DATA_BYTES);
	REQUIRE(memcmp(actual, expected, DATA_BYTES) == 0);
	REQUIRE(fsync(file) == 0);
	mapping = mmap(NULL, DATA_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
	REQUIRE(mapping != MAP_FAILED);
	memcpy(mapping + MAPPED_OFFSET, "mmap!!", 6);
	REQUIRE(msync(mapping, DATA_BYTES, MS_SYNC) == 0);
	memcpy(expected + MAPPED_OFFSET, "mmap!!", 6);
	REQUIRE(pread(file, actual, DATA_BYTES, 0) == (ssize_t)DATA_BYTES);
	REQUIRE(memcmp(actual, expected, DATA_BYTES) == 0);
	REQUIRE(pwrite(file, "read!!", 6, MAPPED_OFFSET) == 6);
	REQUIRE(memcmp(mapping + MAPPED_OFFSET, "read!!", 6) == 0);
	REQUIRE(munmap(mapping, DATA_BYTES) == 0);
	REQUIRE(ftruncate(file, SHRUNK_BYTES) == 0);
	REQUIRE(ftruncate(file, GROWN_BYTES) == 0);
	final_data(expected);
	REQUIRE(pread(file, actual, DATA_BYTES, 0) == (ssize_t)GROWN_BYTES);
	REQUIRE(memcmp(actual, expected, GROWN_BYTES) == 0);
	REQUIRE(fchmod(file, LINK_MODE) == 0);
	REQUIRE(fsetxattr(file, "user.binary", "\0\1\177\377", 4, 0, 0) == 0);
	REQUIRE(fgetxattr(file, "user.binary", actual, 16, 0, 0) == 4);
	REQUIRE(memcmp(actual, "\0\1\177\377", 4) == 0);
	REQUIRE(fremovexattr(file, "user.binary", 0) == 0);
	expect_error((int)fgetxattr(file, "user.binary", actual, 16, 0, 0), ENOATTR);
	REQUIRE(fsetxattr(file, "user.native", native_value, sizeof(native_value) - 1, 0,
		    XATTR_CREATE) == 0);
	expect_error(fsetxattr(file, "user.native", "x", 1, 0, XATTR_CREATE), EEXIST);
	REQUIRE(close(file) == 0);
	free(expected);
	free(actual);
}

static void
namespace_contracts(int directory)
{
	struct stat data;
	struct stat linked;
	struct stat child;
	struct stat parent;
	char target[64];
	int file;

	REQUIRE(linkat(directory, "data", directory, "data-link", 0) == 0);
	REQUIRE(fstatat(directory, "data", &data, 0) == 0);
	REQUIRE(fstatat(directory, "data-link", &linked, 0) == 0);
	REQUIRE(data.st_ino == linked.st_ino && linked.st_nlink == 2);
	REQUIRE(symlinkat("data-link", directory, "data-symlink") == 0);
	REQUIRE(readlinkat(directory, "data-symlink", target, sizeof(target)) == 9);
	REQUIRE(memcmp(target, "data-link", 9) == 0);
	file = openat(directory, "target", O_CREAT | O_EXCL | O_WRONLY, 0600);
	REQUIRE(file >= 0 && write(file, "old destination", 15) == 15 && close(file) == 0);
	REQUIRE(renameat(directory, "data", directory, "target") == 0);
	expect_error(fstatat(directory, "data", &data, 0), ENOENT);
	REQUIRE(fstatat(directory, "target", &data, 0) == 0 && data.st_ino == linked.st_ino);
	REQUIRE(mkdirat(directory, "child", 0755) == 0);
	REQUIRE(renameat(directory, "target", directory, "child/moved") == 0);
	REQUIRE(fstatat(directory, "child/..", &child, 0) == 0 && fstat(directory, &parent) == 0);
	REQUIRE(child.st_ino == parent.st_ino);
	expect_error(unlinkat(directory, "child", AT_REMOVEDIR), ENOTEMPTY);
	REQUIRE(fchownat(directory, "child/moved", OWNER_UID, OWNER_GID, 0) == 0);
	file = openat(directory, "child/moved", O_RDONLY);
	REQUIRE(file >= 0);
	{
		struct timeval times[2] = { { FIXED_SECONDS, 0 }, { FIXED_SECONDS, 0 } };

		REQUIRE(futimes(file, times) == 0);
	}
	REQUIRE(close(file) == 0);
}

static void
unlinked_contracts(int directory)
{
	uint8_t bytes[SHRUNK_BYTES];
	struct stat status;
	int file;

	memset(bytes, 'g', sizeof(bytes));
	file = openat(directory, "ghost", O_CREAT | O_EXCL | O_RDWR, 0600);
	REQUIRE(file >= 0 && write(file, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes));
	REQUIRE(unlinkat(directory, "ghost", 0) == 0);
	REQUIRE(fstat(file, &status) == 0 && status.st_nlink == 0);
	REQUIRE(pread(file, bytes, sizeof(bytes), 0) == (ssize_t)sizeof(bytes) && bytes[0] == 'g');
	REQUIRE(pwrite(file, unlinked_write, sizeof(unlinked_write) - 1, 0) ==
	    (ssize_t)(sizeof(unlinked_write) - 1));
	REQUIRE(fsync(file) == 0);
	REQUIRE(pread(file, bytes, sizeof(unlinked_write) - 1, 0) ==
	    (ssize_t)(sizeof(unlinked_write) - 1));
	REQUIRE(memcmp(bytes, unlinked_write, sizeof(unlinked_write) - 1) == 0);
	REQUIRE(close(file) == 0);
	REQUIRE(fsync(directory) == 0);
}

static void
big_contracts(int directory)
{
	uint8_t *bytes = malloc(BIG_BYTES);
	size_t i;
	int file;

	REQUIRE(bytes != NULL);
	for (i = 0; i < BIG_BYTES; i++) {
		bytes[i] = big_byte(i);
	}
	file = openat(directory, "big", O_CREAT | O_EXCL | O_WRONLY, 0644);
	REQUIRE(file >= 0);
	for (i = 0; i < BIG_BYTES; i += CHUNK_BYTES) {
		REQUIRE(write(file, bytes + i, CHUNK_BYTES) == (ssize_t)CHUNK_BYTES);
	}
	REQUIRE(fsync(file) == 0 && close(file) == 0);
	free(bytes);
}

static void *
append_worker(void *context)
{
	struct appender *appender = context;
	uint8_t block[BLOCK_BYTES];
	unsigned i;
	int file;

	memset(block, appender->id, sizeof(block));
	file = openat(appender->directory, "appended", O_WRONLY | O_APPEND);
	REQUIRE(file >= 0);
	for (i = 0; i < APPENDS; i++) {
		REQUIRE(write(file, block, sizeof(block)) == (ssize_t)sizeof(block));
	}
	REQUIRE(close(file) == 0);
	return NULL;
}

static void *
rename_worker(void *context)
{
	int directory = *(int *)context;
	unsigned i;

	for (i = 0; i < RENAMES; i++) {
		REQUIRE(renameat(directory, i % 2 == 0 ? "appended" : "appended-renamed", directory,
			    i % 2 == 0 ? "appended-renamed" : "appended") == 0);
	}
	return NULL;
}

/* Appenders keep their descriptors while the name moves between two names. */
static void
append_rename_contracts(int directory)
{
	struct appender appenders[APPENDERS];
	pthread_t threads[APPENDERS + 1];
	unsigned i;
	int file;

	file = openat(directory, "appended", O_CREAT | O_EXCL | O_WRONLY, 0644);
	REQUIRE(file >= 0 && close(file) == 0);
	for (i = 0; i < APPENDERS; i++) {
		appenders[i] = (struct appender){ directory, (uint8_t)('A' + i) };
		REQUIRE(pthread_create(&threads[i], NULL, append_worker, &appenders[i]) == 0);
	}
	REQUIRE(pthread_create(&threads[APPENDERS], NULL, rename_worker, &directory) == 0);
	for (i = 0; i <= APPENDERS; i++) {
		REQUIRE(pthread_join(threads[i], NULL) == 0);
	}
	file = openat(directory, "appended", O_RDONLY);
	REQUIRE(file >= 0 && fsync(file) == 0 && close(file) == 0);
}

static void
check_appended(int directory)
{
	uint8_t *bytes = malloc(APPENDERS * APPENDS * BLOCK_BYTES);
	unsigned counts[APPENDERS] = { 0 };
	size_t block;
	size_t i;
	int file;

	REQUIRE(bytes != NULL);
	file = openat(directory, "appended", O_RDONLY);
	REQUIRE(file >= 0);
	REQUIRE(pread(file, bytes, APPENDERS * APPENDS * BLOCK_BYTES + 1, 0) ==
	    (ssize_t)(APPENDERS * APPENDS * BLOCK_BYTES));
	REQUIRE(close(file) == 0);
	for (block = 0; block < APPENDERS * APPENDS; block++) {
		uint8_t id = bytes[block * BLOCK_BYTES];

		REQUIRE(id >= 'A' && id < 'A' + APPENDERS);
		for (i = 0; i < BLOCK_BYTES; i++) {
			REQUIRE(bytes[block * BLOCK_BYTES + i] == id);
		}
		counts[id - 'A']++;
	}
	for (i = 0; i < APPENDERS; i++) {
		REQUIRE(counts[i] == APPENDS);
	}
	free(bytes);
}

static void
write_as(int directory, const char *name, uid_t uid, gid_t gid)
{
	pid_t child;
	int status;
	int file;

	child = fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		if (setgid(gid) != 0 || setuid(uid) != 0) {
			_exit(2);
		}
		file = openat(directory, name, O_WRONLY);
		if (file < 0 || write(file, "x", 1) != 1 || fsync(file) != 0) {
			_exit(3);
		}
		_exit(close(file) == 0 ? 0 : 4);
	}
	REQUIRE(waitpid(child, &status, 0) == child);
	REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* The superuser's write keeps S_ISUID; another user's write removes it. */
/* The set-id group is a recorded failure on this mount and is not run. */
static bool skip_set_id;
/* The mount has no F_PUNCHHOLE interface. */
static bool no_punch_hole;

/* native/sparse as written, and with punched as the hole leaves it. */
static void
sparse_data(uint8_t *bytes, bool punched)
{
	size_t i;

	for (i = 0; i < SPARSE_BLOCKS * BLOCK_BYTES; i++) {
		bytes[i] = (uint8_t)(i * 7U + 3U);
	}
	if (punched) {
		memset(bytes + PUNCHED_BLOCK * BLOCK_BYTES, 0, BLOCK_BYTES);
	}
}

/* The file's allocated bytes after the contracts: its data blocks, less the
 * punched one, and the preallocated blocks past its end. */
static off_t
sparse_allocated(void)
{
	return (
	    off_t)((SPARSE_BLOCKS - (no_punch_hole ? 0U : 1U) + PREALLOCATED_BLOCKS) * BLOCK_BYTES);
}

/* F_PREALLOCATE allocates from the physical end and keeps the size;
 * F_PUNCHHOLE zeroes a block and frees it, keeping the size too. */
static void
sparse_contracts(int directory)
{
	uint8_t bytes[SPARSE_BLOCKS * BLOCK_BYTES];
	uint8_t read_back[SPARSE_BLOCKS * BLOCK_BYTES];
	fpunchhole_t hole;
	fstore_t store;
	struct stat status;
	off_t written_blocks;
	int file;

	sparse_data(bytes, false);
	file = openat(directory, "sparse", O_CREAT | O_EXCL | O_RDWR, 0644);
	REQUIRE(file >= 0);
	REQUIRE(pwrite(file, bytes, sizeof(bytes), 0) == (ssize_t)sizeof(bytes));
	REQUIRE(fsync(file) == 0 && fstat(file, &status) == 0);
	written_blocks = status.st_blocks;
	REQUIRE(written_blocks * STAT_BLOCK_BYTES == (off_t)sizeof(bytes));
	memset(&store, 0, sizeof(store));
	store.fst_flags = F_ALLOCATEALL;
	store.fst_posmode = F_PEOFPOSMODE;
	store.fst_length = PREALLOCATED_BLOCKS * BLOCK_BYTES;
	REQUIRE(fcntl(file, F_PREALLOCATE, &store) == 0);
	REQUIRE(store.fst_bytesalloc == (off_t)(PREALLOCATED_BLOCKS * BLOCK_BYTES));
	REQUIRE(fstat(file, &status) == 0 && status.st_size == (off_t)sizeof(bytes));
	REQUIRE(status.st_blocks * STAT_BLOCK_BYTES ==
	    written_blocks * STAT_BLOCK_BYTES + (off_t)(PREALLOCATED_BLOCKS * BLOCK_BYTES));
	memset(&hole, 0, sizeof(hole));
	hole.fp_offset = PUNCHED_BLOCK * BLOCK_BYTES;
	hole.fp_length = BLOCK_BYTES;
	if (no_punch_hole) {
		REQUIRE(fcntl(file, F_PUNCHHOLE, &hole) == -1 &&
		    (errno == ENOTSUP || errno == ENOTTY || errno == EINVAL));
	} else {
		REQUIRE(fcntl(file, F_PUNCHHOLE, &hole) == 0);
	}
	sparse_data(bytes, !no_punch_hole);
	REQUIRE(pread(file, read_back, sizeof(read_back), 0) == (ssize_t)sizeof(read_back));
	REQUIRE(memcmp(read_back, bytes, sizeof(bytes)) == 0);
	REQUIRE(fstat(file, &status) == 0 && status.st_size == (off_t)sizeof(bytes));
	REQUIRE(status.st_blocks * STAT_BLOCK_BYTES == sparse_allocated());
	REQUIRE(fsync(file) == 0 && close(file) == 0);
}

static void
privilege_contracts(int directory)
{
	struct stat status;
	int file;

	file = openat(directory, "suid-root", O_CREAT | O_EXCL | O_WRONLY, 0755);
	REQUIRE(file >= 0 && fchmod(file, SUID_MODE) == 0);
	REQUIRE(write(file, "root", 4) == 4 && fsync(file) == 0 && close(file) == 0);
	REQUIRE(fstatat(directory, "suid-root", &status, 0) == 0);
	REQUIRE((status.st_mode & 07777) == SUID_MODE);
	file = openat(directory, "suid", O_CREAT | O_EXCL | O_WRONLY, 0755);
	REQUIRE(file >= 0 && fchmod(file, SHARED_MODE) == 0 && close(file) == 0);
	write_as(directory, "suid", WRITER_UID, WRITER_GID);
	REQUIRE(fstatat(directory, "suid", &status, 0) == 0);
	REQUIRE((status.st_mode & 07777) == (SHARED_MODE & ~(mode_t)S_ISUID));
}

/* Writes beyond the free space fail with ENOSPC: at fsync for cached writes,
 * at write on a synchronous mount. A short write stores what fits and the next
 * one fails. Deleting the file returns the space and the volume keeps
 * working. */
static void
full_contracts(int directory)
{
	uint8_t *bytes = malloc(FILL_BYTES);
	unsigned round;
	ssize_t written;
	int file;
	int error = 0;

	REQUIRE(bytes != NULL);
	memset(bytes, 'f', FILL_BYTES);
	file = openat(directory, "filler", O_CREAT | O_EXCL | O_WRONLY, 0644);
	REQUIRE(file >= 0);
	for (round = 0; round < FILL_LIMIT && error == 0; round++) {
		written = write(file, bytes, FILL_BYTES);
		REQUIRE(written != 0);
		if (written < 0 || fsync(file) != 0) {
			error = errno;
		}
	}
	REQUIRE(error == ENOSPC);
	REQUIRE(close(file) == 0);
	REQUIRE(unlinkat(directory, "filler", 0) == 0);
	free(bytes);
	file = openat(directory, "after-full", O_CREAT | O_EXCL | O_WRONLY, 0644);
	REQUIRE(file >= 0);
	REQUIRE(
	    write(file, after_full, sizeof(after_full) - 1) == (ssize_t)(sizeof(after_full) - 1));
	REQUIRE(fsync(file) == 0 && close(file) == 0);
}

static void
write_phase(int root)
{
	int directory;

	REQUIRE(mkdirat(root, "native", 0755) == 0);
	directory = openat(root, "native", O_RDONLY | O_DIRECTORY);
	REQUIRE(directory >= 0);
	data_contracts(directory);
	namespace_contracts(directory);
	unlinked_contracts(directory);
	big_contracts(directory);
	append_rename_contracts(directory);
	check_appended(directory);
	if (skip_set_id) {
		fprintf(stderr, "SKIP set-id contracts: recorded failure on this mount\n");
	} else {
		privilege_contracts(directory);
	}
	if (no_punch_hole) {
		fprintf(stderr, "SKIP punch-hole contract: no interface on this mount\n");
	}
	sparse_contracts(directory);
	full_contracts(directory);
	REQUIRE(close(directory) == 0);
	sync();
}

static void
digest(const uint8_t *bytes, size_t size, char text[CC_SHA256_DIGEST_LENGTH * 2 + 1])
{
	unsigned char value[CC_SHA256_DIGEST_LENGTH];
	size_t i;

	CC_SHA256(bytes, (CC_LONG)size, value);
	for (i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
		snprintf(text + 2 * i, 3, "%02x", value[i]);
	}
}

static uint8_t *
read_all(int directory, const char *path, size_t *size)
{
	struct stat status;
	uint8_t *bytes;
	int file;

	file = openat(directory, path, O_RDONLY);
	REQUIRE(file >= 0 && fstat(file, &status) == 0);
	bytes = malloc((size_t)status.st_size + 1);
	REQUIRE(bytes != NULL);
	REQUIRE(pread(file, bytes, (size_t)status.st_size, 0) == status.st_size);
	REQUIRE(close(file) == 0);
	*size = (size_t)status.st_size;
	return bytes;
}

/* Checks a file and prints its manifest line: path, SHA-256, permission
 * bits, owner and link count. */
static void
manifest_file(int directory, const char *path, const uint8_t *expected, size_t expected_size,
    unsigned mode, unsigned uid, unsigned gid, unsigned links)
{
	struct stat status;
	char text[CC_SHA256_DIGEST_LENGTH * 2 + 1];
	uint8_t *bytes;
	size_t size;

	bytes = read_all(directory, path, &size);
	if (expected != NULL) {
		REQUIRE(size == expected_size && memcmp(bytes, expected, size) == 0);
	}
	REQUIRE(fstatat(directory, path, &status, AT_SYMLINK_NOFOLLOW) == 0);
	REQUIRE(S_ISREG(status.st_mode) && (status.st_mode & 07777) == mode);
	REQUIRE(status.st_uid == uid && status.st_gid == gid && status.st_nlink == links);
	digest(bytes, size, text);
	printf("file\tnative/%s\t%s\t%o\t%u\t%u\t%u\n", path, text, mode, uid, gid, links);
	free(bytes);
}

static void
manifest_listing(int directory, const char *path, const char *const *names, size_t count)
{
	char text[CC_SHA256_DIGEST_LENGTH * 2 + 1];
	char joined[1024];
	size_t used = 0;
	size_t i;
	unsigned seen = 0;
	struct dirent *entry;
	DIR *stream;
	int file;

	for (i = 0; i < count; i++) {
		used += (size_t)snprintf(joined + used, sizeof(joined) - used, "%s\n", names[i]);
		REQUIRE(used < sizeof(joined));
	}
	file = openat(directory, path, O_RDONLY | O_DIRECTORY);
	REQUIRE(file >= 0);
	stream = fdopendir(file);
	REQUIRE(stream != NULL);
	while ((entry = readdir(stream)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		for (i = 0; i < count && strcmp(names[i], entry->d_name) != 0; i++) {
		}
		REQUIRE(i < count);
		seen++;
	}
	REQUIRE(seen == count && closedir(stream) == 0);
	digest((const uint8_t *)joined, used, text);
	printf("dir\tnative%s%s\t%s\n", strcmp(path, ".") == 0 ? "" : "/",
	    strcmp(path, ".") == 0 ? "" : path, text);
}

static void
verify_phase(int root)
{
	static const char *const listing[] = { "after-full", "appended", "big", "child",
		"data-link", "data-symlink", "sparse", "suid", "suid-root" };
	static const char *const children[] = { "moved" };
	static const char greeting[] = "hello from Linux Btrfs\n";
	struct stat native;
	struct stat status;
	uint8_t *expected;
	uint8_t *bytes;
	char text[64];
	size_t size;
	size_t i;
	int directory;
	int file;

	/* The fixture's own objects are unchanged. */
	bytes = read_all(root, "greeting", &size);
	REQUIRE(size == sizeof(greeting) - 1 && memcmp(bytes, greeting, size) == 0);
	free(bytes);
	directory = openat(root, "native", O_RDONLY | O_DIRECTORY);
	REQUIRE(directory >= 0 && fstat(directory, &native) == 0);
	/* The set-id files sort last. */
	manifest_listing(directory, ".", listing,
	    sizeof(listing) / sizeof(listing[0]) - (skip_set_id ? SET_ID_FILES : 0));
	manifest_listing(directory, "child", children, 1);
	expected = malloc(BIG_BYTES);
	REQUIRE(expected != NULL);
	final_data(expected);
	manifest_file(
	    directory, "data-link", expected, GROWN_BYTES, LINK_MODE, OWNER_UID, OWNER_GID, 2);
	manifest_file(
	    directory, "child/moved", expected, GROWN_BYTES, LINK_MODE, OWNER_UID, OWNER_GID, 2);
	REQUIRE(fstatat(directory, "child/moved", &status, 0) == 0);
	REQUIRE(status.st_mtimespec.tv_sec == FIXED_SECONDS);
	printf("mtime\tnative/child/moved\t%d\n", FIXED_SECONDS);
	file = openat(directory, "data-link", O_RDONLY);
	REQUIRE(file >= 0);
	REQUIRE(fgetxattr(file, "user.native", text, sizeof(text), 0, 0) ==
	    (ssize_t)(sizeof(native_value) - 1));
	REQUIRE(memcmp(text, native_value, sizeof(native_value) - 1) == 0);
	REQUIRE(close(file) == 0);
	printf("xattr\tnative/data-link\tuser.native\t%s\n", native_value);
	REQUIRE(readlinkat(directory, "data-symlink", text, sizeof(text)) == 9);
	REQUIRE(memcmp(text, "data-link", 9) == 0);
	printf("symlink\tnative/data-symlink\tdata-link\n");
	for (i = 0; i < BIG_BYTES; i++) {
		expected[i] = big_byte(i);
	}
	/* New objects take the directory's group, as BSD creation does. */
	manifest_file(directory, "big", expected, BIG_BYTES, 0644, 0, native.st_gid, 1);
	check_appended(directory);
	manifest_file(directory, "appended", NULL, 0, 0644, 0, native.st_gid, 1);
	if (!skip_set_id) {
		manifest_file(directory, "suid-root", (const uint8_t *)"root", 4, SUID_MODE, 0,
		    native.st_gid, 1);
		manifest_file(directory, "suid", (const uint8_t *)"x", 1,
		    SHARED_MODE & ~(unsigned)S_ISUID, 0, native.st_gid, 1);
	}
	manifest_file(directory, "after-full", (const uint8_t *)after_full, sizeof(after_full) - 1,
	    0644, 0, native.st_gid, 1);
	sparse_data(expected, !no_punch_hole);
	manifest_file(
	    directory, "sparse", expected, SPARSE_BLOCKS * BLOCK_BYTES, 0644, 0, native.st_gid, 1);
	REQUIRE(fstatat(directory, "sparse", &status, 0) == 0);
	REQUIRE(status.st_blocks * STAT_BLOCK_BYTES == sparse_allocated());
	expect_error(fstatat(directory, "ghost", &status, 0), ENOENT);
	expect_error(fstatat(directory, "filler", &status, 0), ENOENT);
	printf("absent\tnative/ghost\nabsent\tnative/filler\n");
	REQUIRE(close(directory) == 0);
	free(expected);
}

int
main(int argc, char **argv)
{
	struct statvfs filesystem;
	int option;
	int root;

	for (option = 3; option < argc; option++) {
		if (strcmp(argv[option], "--skip-set-id") == 0) {
			skip_set_id = true;
		} else if (strcmp(argv[option], "--no-punch-hole") == 0) {
			no_punch_hole = true;
		} else {
			break;
		}
	}
	if (argc < 3 || option != argc || geteuid() != 0 ||
	    (strcmp(argv[1], "write") != 0 && strcmp(argv[1], "verify") != 0)) {
		fprintf(stderr,
		    "usage (guest root): btrfs-mounted-write-test write|verify MOUNT "
		    "[--skip-set-id] [--no-punch-hole]\n");
		return 2;
	}
	root = open(argv[2], O_RDONLY | O_DIRECTORY);
	REQUIRE(root >= 0 && fstatvfs(root, &filesystem) == 0);
	REQUIRE(fchdir(root) == 0);
	if (strcmp(argv[1], "write") == 0) {
		REQUIRE((filesystem.f_flag & ST_RDONLY) == 0);
		write_phase(root);
		fprintf(stderr, "mounted Btrfs write contracts PASS%s%s\n",
		    skip_set_id ? " (set-id SKIPPED)" : "",
		    no_punch_hole ? " (punch-hole SKIPPED)" : "");
	} else {
		verify_phase(root);
		fprintf(stderr, "mounted Btrfs persistence after remount PASS%s%s\n",
		    skip_set_id ? " (set-id SKIPPED)" : "",
		    no_punch_hole ? " (punch-hole SKIPPED)" : "");
	}
	REQUIRE(close(root) == 0);
	return 0;
}
