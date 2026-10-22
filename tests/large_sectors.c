/* SPDX-License-Identifier: BSD-3-Clause */
/* Sectors of 64 KiB, which no kernel an Apple CPU runs can mount. The fixture
 * comes from btrfs-progs (tests/prepare_progs_linux.py): its manifest lists
 * every file and symlink with mode, owner, size and SHA-256 or target, and
 * the reader must return each exactly. Then three transactions write a copy
 * in the ways large sectors make different: unaligned writes across sector
 * boundaries, truncation inside a sector, an inline file grown past one, Zstd
 * extents by property, writes into preallocated space, a hole punched with an
 * unaligned edge, names moved and removed, a symlink, an xattr, and a
 * writable snapshot written to. After each commit every file reads as an
 * independent model holds it, the compressed, preallocated and punched files
 * have the extents Linux's sector rounding gives them, and both audits pass.
 * The written image and the model's manifest go to the output directory, for
 * btrfs-progs to check the image and to extract every file with
 * `btrfs restore`. */
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

#define MANIFEST_LINE 512U
#define MANIFEST_FIELDS 3U
#define MAX_FILES 1024U
#define SHA256_HEX (2U * BT_SHA256_DIGEST)
#define MANIFEST_PREFIX "./"
#define MODE_REGULAR 0100000U
#define MODE_SYMLINK 0120000U
#define MODE_DIRECTORY 0040000U
#define SECONDS INT64_C(1800000000)
#define SECTOR_BYTES 65536U
/* Linux compresses data in pieces of 128 KiB. */
#define COMPRESSION_PIECE 131072U
#define FILE_EXTENT_ITEMS 8U
/* The writes: offsets and lengths that cross 64 KiB sector boundaries. */
#define NEW_BYTES 200000U
#define BIG_PATCH_OFFSET 65000U
#define BIG_PATCH_BYTES 70000U
#define RANDOM_TRUNCATED 100001U
#define SMALL_BYTES 1000U
#define SMALL_GROWN 70000U
#define ZSTD_BYTES 300000U
#define PREALLOC_BYTES (1024U * 1024U)
#define PREALLOC_WRITE_OFFSET 100000U
#define PREALLOC_WRITE_BYTES 70000U
#define PUNCH_OFFSET (65536U + 100U)
#define PUNCH_BYTES 131072U
#define TRUNCATE_STEPS 4096U
#define SNAPSHOT_VALUE "changed in a snapshot\n"
#define COMPRESSION_PROPERTY "btrfs.compression"
#define NOTE_XATTR "user.note"
#define NOTE_VALUE "sixty-four"

struct file {
	char path[MANIFEST_LINE];
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	/* A regular file's bytes, a symlink's target. */
	uint8_t *bytes;
	size_t size;
	/* From the fixture's manifest while the file is untouched. */
	char digest[SHA256_HEX + 1];
	int modelled;
};

/* A file extent item's wire fields. */
struct extent_item {
	uint64_t offset;
	uint64_t disk;
	uint64_t disk_bytes;
	uint64_t extent_offset;
	uint64_t length;
	uint64_t ram;
	uint8_t type;
	uint8_t compression;
};

struct model {
	struct file files[MAX_FILES];
	size_t count;
};

static void
hex_digest(const uint8_t *bytes, size_t size, char hex[SHA256_HEX + 1])
{
	uint8_t digest[BT_SHA256_DIGEST];
	struct bt_sha2 hash;
	size_t i;

	bt_sha2_init(&hash, 0);
	bt_sha2_update(&hash, bytes, size);
	bt_sha2_final(&hash, digest);
	for (i = 0; i < BT_SHA256_DIGEST; i++) {
		snprintf(hex + 2 * i, 3, "%02x", digest[i]);
	}
}

static struct file *
find(struct model *model, const char *path)
{
	size_t i;

	for (i = 0; i < model->count; i++) {
		if (strcmp(model->files[i].path, path) == 0) {
			return &model->files[i];
		}
	}
	return NULL;
}

static struct file *
add(struct model *model, const char *path, uint32_t mode)
{
	struct file *file;

	REQUIRE(find(model, path) == NULL && model->count < MAX_FILES);
	file = &model->files[model->count++];
	memset(file, 0, sizeof(*file));
	REQUIRE(strlen(path) < sizeof(file->path));
	strcpy(file->path, path);
	file->mode = mode;
	file->modelled = 1;
	return file;
}

static void
drop(struct model *model, const char *path)
{
	struct file *file = find(model, path);

	REQUIRE(file != NULL);
	free(file->bytes);
	*file = model->files[--model->count];
}

/* The fixture's manifest: PATH, %f:%u:%g:%s and SHA-256 or target. */
static void
read_manifest(const char *path, struct model *model)
{
	char line[MANIFEST_LINE];
	char *fields[MANIFEST_FIELDS];
	unsigned long mode;
	unsigned long uid;
	unsigned long gid;
	unsigned long long size;
	struct file *file;
	FILE *stream;
	size_t i;

	stream = fopen(path, "r");
	REQUIRE(stream != NULL);
	model->count = 0;
	while (fgets(line, sizeof(line), stream) != NULL) {
		REQUIRE(strlen(line) > 0 && line[strlen(line) - 1] == '\n');
		line[strlen(line) - 1] = '\0';
		fields[0] = line;
		for (i = 1; i < MANIFEST_FIELDS; i++) {
			fields[i] = strchr(fields[i - 1], '\t');
			REQUIRE(fields[i] != NULL);
			*fields[i]++ = '\0';
		}
		REQUIRE(strncmp(fields[0], MANIFEST_PREFIX, strlen(MANIFEST_PREFIX)) == 0);
		REQUIRE(sscanf(fields[1], "%lx:%lu:%lu:%llu", &mode, &uid, &gid, &size) == 4);
		REQUIRE(model->count < MAX_FILES);
		file = &model->files[model->count++];
		memset(file, 0, sizeof(*file));
		REQUIRE(snprintf(file->path, sizeof(file->path), "/%s",
			    fields[0] + strlen(MANIFEST_PREFIX)) < (int)sizeof(file->path));
		file->mode = (uint32_t)mode;
		file->uid = (uint32_t)uid;
		file->gid = (uint32_t)gid;
		file->size = (size_t)size;
		if ((file->mode & 0170000U) == MODE_SYMLINK) {
			file->bytes = (uint8_t *)strdup(fields[2]);
			REQUIRE(file->bytes != NULL && strlen(fields[2]) == file->size);
			file->modelled = 1;
		} else {
			REQUIRE(strlen(fields[2]) == SHA256_HEX);
			strcpy(file->digest, fields[2]);
		}
	}
	REQUIRE(fclose(stream) == 0);
	REQUIRE(model->count > 0);
}

static uint8_t *
read_all(struct btrfs_fs *fs, const struct btrfs_inode *inode)
{
	uint8_t *bytes = malloc((size_t)inode->size + 1);
	size_t completed = 0;

	REQUIRE(bytes != NULL);
	REQUIRE(btrfs_read(fs, inode, 0, bytes, (size_t)inode->size, &completed) == BTRFS_OK);
	REQUIRE(completed == inode->size);
	return bytes;
}

/* Every file reads as the model holds it; untouched ones as the manifest. */
static void
check_model(struct btrfs_fs *fs, struct model *model, const char *when)
{
	struct reference_audit references;
	struct namespace_audit names;
	struct btrfs_inode inode;
	struct file *file;
	char hex[SHA256_HEX + 1];
	uint8_t *bytes;
	size_t i;

	for (i = 0; i < model->count; i++) {
		file = &model->files[i];
		if (btrfs_image_lookup(fs, file->path, &inode) != BTRFS_OK ||
		    inode.mode != file->mode || inode.uid != file->uid || inode.gid != file->gid ||
		    inode.size != file->size) {
			fprintf(stderr, "%s: %s: mode %o uid %u gid %u size %llu\n", when,
			    file->path, inode.mode, inode.uid, inode.gid,
			    (unsigned long long)inode.size);
			exit(1);
		}
		bytes = read_all(fs, &inode);
		if (file->modelled) {
			REQUIRE(memcmp(bytes, file->bytes, file->size) == 0);
		} else {
			hex_digest(bytes, file->size, hex);
			REQUIRE(strcmp(hex, file->digest) == 0);
		}
		free(bytes);
	}
	if (reference_audit(fs, &references) != 0) {
		fprintf(stderr, "%s: reference audit: %s\n", when, references.failure);
		exit(1);
	}
	if (namespace_audit(fs, &names) != 0) {
		fprintf(stderr, "%s: namespace audit: %s\n", when, names.failure);
		exit(1);
	}
}

/* The file's extent items, at most FILE_EXTENT_ITEMS; returns their count. */
static size_t
extent_items(struct btrfs_fs *fs, const char *path, struct extent_item *items)
{
	const struct bt_disk_extent_header *header;
	const struct bt_disk_extent *extent;
	struct btrfs_inode inode;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_root root;
	struct bt_key key;
	size_t count = 0;
	enum btrfs_result result;

	REQUIRE(btrfs_image_lookup(fs, path, &inode) == BTRFS_OK);
	key.objectid = inode.id.inode;
	key.offset = 0;
	key.type = BT_EXTENT_DATA;
	REQUIRE(bt_find_root(fs, inode.id.tree, &root) == BTRFS_OK);
	bt_cursor_init(&cursor, fs, root);
	result = bt_cursor_seek(&cursor, key, 0);
	while (result == BTRFS_OK) {
		REQUIRE(bt_cursor_record(&cursor, &record) == BTRFS_OK);
		if (record.key.objectid != inode.id.inode || record.key.type != BT_EXTENT_DATA) {
			break;
		}
		REQUIRE(count < FILE_EXTENT_ITEMS && record.size >= sizeof(*header));
		header = (const void *)record.data;
		memset(&items[count], 0, sizeof(items[count]));
		items[count].offset = record.key.offset;
		items[count].ram = bt_u64(header->ram_bytes);
		items[count].type = header->type;
		items[count].compression = header->compression;
		if (header->type != BT_EXTENT_INLINE) {
			REQUIRE(record.size == sizeof(*extent));
			extent = (const void *)record.data;
			items[count].disk = bt_u64(extent->disk_bytenr);
			items[count].disk_bytes = bt_u64(extent->disk_bytes);
			items[count].extent_offset = bt_u64(extent->offset);
			items[count].length = bt_u64(extent->length);
		}
		count++;
		result = bt_cursor_next(&cursor);
	}
	bt_cursor_fini(&cursor);
	return count;
}

/* Linux's layouts with 64 KiB sectors (and NO_HOLES). Each 128 KiB piece of
 * /zstd compresses into one sector, while its last sector cannot shrink and
 * stays uncompressed. The write into /prealloc covers sectors one and two of
 * the preallocated extent in place, splitting it into three items. The punch
 * in /new rewrites its two partial edge sectors as new extents and drops the
 * sector between them. Seeks find that sector a hole and preallocated space
 * unwritten, at sector granularity. */
static void
check_layouts(struct btrfs_fs *fs)
{
	struct extent_item items[FILE_EXTENT_ITEMS];
	struct btrfs_inode inode;
	uint64_t offset;
	size_t count;

	count = extent_items(fs, "/zstd", items);
	REQUIRE(count == 3);
	REQUIRE(items[0].offset == 0 && items[0].compression == BTRFS_COMPRESSION_ZSTD &&
	    items[0].ram == COMPRESSION_PIECE && items[0].disk_bytes == SECTOR_BYTES);
	REQUIRE(items[1].offset == COMPRESSION_PIECE &&
	    items[1].compression == BTRFS_COMPRESSION_ZSTD && items[1].ram == COMPRESSION_PIECE &&
	    items[1].disk_bytes == SECTOR_BYTES);
	REQUIRE(items[2].offset == 2 * COMPRESSION_PIECE &&
	    items[2].compression == BTRFS_COMPRESSION_NONE && items[2].length == SECTOR_BYTES);
	count = extent_items(fs, "/prealloc", items);
	REQUIRE(count == 3);
	REQUIRE(items[0].offset == 0 && items[0].type == BT_EXTENT_PREALLOC &&
	    items[0].disk_bytes == PREALLOC_BYTES && items[0].length == SECTOR_BYTES);
	REQUIRE(items[1].offset == SECTOR_BYTES && items[1].type == BT_EXTENT_REGULAR &&
	    items[1].disk == items[0].disk && items[1].extent_offset == SECTOR_BYTES &&
	    items[1].length == 2 * SECTOR_BYTES);
	REQUIRE(items[2].offset == 3 * SECTOR_BYTES && items[2].type == BT_EXTENT_PREALLOC &&
	    items[2].disk == items[0].disk && items[2].extent_offset == 3 * SECTOR_BYTES &&
	    items[2].length == PREALLOC_BYTES - 3 * SECTOR_BYTES);
	count = extent_items(fs, "/new", items);
	REQUIRE(count == 3);
	REQUIRE(items[0].offset == 0 && items[0].length == SECTOR_BYTES);
	REQUIRE(items[1].offset == SECTOR_BYTES && items[1].length == SECTOR_BYTES &&
	    items[1].disk != items[0].disk);
	REQUIRE(items[2].offset == 3 * SECTOR_BYTES && items[2].length == SECTOR_BYTES &&
	    items[2].disk != items[0].disk && items[2].disk != items[1].disk);
	REQUIRE(btrfs_image_lookup(fs, "/new", &inode) == BTRFS_OK);
	REQUIRE(btrfs_seek(fs, &inode, 0, 1, &offset) == BTRFS_OK && offset == 2 * SECTOR_BYTES);
	REQUIRE(btrfs_seek(fs, &inode, 2 * SECTOR_BYTES, 0, &offset) == BTRFS_OK &&
	    offset == 3 * SECTOR_BYTES);
	REQUIRE(btrfs_image_lookup(fs, "/prealloc", &inode) == BTRFS_OK);
	REQUIRE(btrfs_seek(fs, &inode, 0, 1, &offset) == BTRFS_OK && offset == 0);
	REQUIRE(btrfs_seek(fs, &inode, 0, 0, &offset) == BTRFS_OK && offset == SECTOR_BYTES);
	REQUIRE(btrfs_seek(fs, &inode, SECTOR_BYTES, 1, &offset) == BTRFS_OK &&
	    offset == 3 * SECTOR_BYTES);
}

/* A file the model is about to change: its bytes from the volume. */
static struct file *
take(struct btrfs_fs *fs, struct model *model, const char *path)
{
	struct btrfs_inode inode;
	struct file *file = find(model, path);

	REQUIRE(file != NULL);
	if (!file->modelled) {
		REQUIRE(btrfs_image_lookup(fs, path, &inode) == BTRFS_OK);
		file->bytes = read_all(fs, &inode);
		file->modelled = 1;
	}
	return file;
}

static struct btrfs_object_id
object(struct btrfs_transaction *transaction, const char *path)
{
	struct btrfs_inode inode;

	REQUIRE(btrfs_image_lookup((struct btrfs_fs *)btrfs_transaction_reader(transaction), path,
		    &inode) == BTRFS_OK);
	return inode.id;
}

static void
fill(uint8_t *bytes, size_t size, unsigned seed, int text)
{
	uint32_t state = 0x9e3779b9U * (seed + 1);
	size_t i;

	for (i = 0; i < size; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		bytes[i] = text ? (uint8_t)("abcdefgh \n"[state % 10]) : (uint8_t)state;
	}
}

/* Writes bytes at offset in the volume and the model. */
static void
write_file(struct btrfs_transaction *transaction, struct file *file, uint64_t offset,
    const uint8_t *bytes, size_t size)
{
	struct btrfs_time time = { SECONDS, 0 };
	size_t end = (size_t)offset + size;

	REQUIRE(btrfs_transaction_write(transaction, object(transaction, file->path), offset, bytes,
		    size, time) == BTRFS_OK);
	if (end > file->size) {
		file->bytes = realloc(file->bytes, end);
		REQUIRE(file->bytes != NULL);
		memset(file->bytes + file->size, 0, end - file->size);
		file->size = end;
	}
	memcpy(file->bytes + offset, bytes, size);
}

static void
create(struct btrfs_transaction *transaction, struct model *model, const char *parent,
    const char *name, uint32_t mode, const char *target)
{
	struct btrfs_new_inode attributes = { mode, 0, 0, 0, { SECONDS, 0 }, target,
		target != NULL ? strlen(target) : 0 };
	struct btrfs_object_id created;
	char path[MANIFEST_LINE];
	struct file *file;

	REQUIRE(btrfs_transaction_create(transaction, object(transaction, parent), name,
		    strlen(name), &attributes, &created) == BTRFS_OK);
	if ((mode & 0170000U) == MODE_DIRECTORY) {
		return;
	}
	REQUIRE(snprintf(path, sizeof(path), "%s/%s", strcmp(parent, "/") == 0 ? "" : parent,
		    name) < (int)sizeof(path));
	file = add(model, path, mode);
	if (target != NULL) {
		file->bytes = (uint8_t *)strdup(target);
		REQUIRE(file->bytes != NULL);
		file->size = strlen(target);
	}
}

static void
commit(struct btrfs_image *image, struct btrfs_transaction *transaction, struct btrfs_fs **fs,
    struct model *model, const char *when)
{
	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(*fs);
	REQUIRE(btrfs_mount(&image->environment, BTRFS_TOP_LEVEL_TREE, fs) == BTRFS_OK);
	check_model(*fs, model, when);
}

static int
path_order(const void *left, const void *right)
{
	return strcmp(((const struct file *)left)->path, ((const struct file *)right)->path);
}

/* The model's manifest, in the fixture's format and byte order. */
static void
write_manifest(struct model *model, const char *path)
{
	char hex[SHA256_HEX + 1];
	struct file *file;
	FILE *stream;
	size_t i;

	qsort(model->files, model->count, sizeof(model->files[0]), path_order);
	stream = fopen(path, "wx");
	REQUIRE(stream != NULL);
	for (i = 0; i < model->count; i++) {
		file = &model->files[i];
		if ((file->mode & 0170000U) == MODE_SYMLINK) {
			snprintf(
			    hex, sizeof(hex), "%.*s", (int)file->size, (const char *)file->bytes);
		} else if (file->modelled) {
			hex_digest(file->bytes, file->size, hex);
		} else {
			strcpy(hex, file->digest);
		}
		fprintf(stream, ".%s\t%x:%u:%u:%zu\t%s\n", file->path, file->mode, file->uid,
		    file->gid, file->size, hex);
	}
	REQUIRE(fclose(stream) == 0);
}

int
main(int argc, char **argv)
{
	static uint8_t data[ZSTD_BYTES];
	static struct model model;
	struct btrfs_time time = { SECONDS, 0 };
	struct btrfs_write_environment writer;
	struct btrfs_transaction *transaction;
	struct btrfs_image image;
	struct btrfs_info info;
	struct btrfs_fs *fs;
	struct file *file;
	char copy[4096];
	char manifest[4096];
	uint8_t uuid[BTRFS_UUID_SIZE] = { 0x64, 0x6b };
	uint64_t snapshot;
	int done = 0;
	size_t steps;

	REQUIRE(argc == 4);
	read_manifest(argv[2], &model);
	REQUIRE(btrfs_image_open(argv[1], &image) == 0);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	btrfs_get_info(fs, &info);
	REQUIRE(info.sector_size == SECTOR_BYTES);
	check_model(fs, &model, "fixture");
	btrfs_unmount(fs);
	btrfs_image_close(&image);

	REQUIRE(snprintf(copy, sizeof(copy), "%s/large-sectors-%ld.raw", argv[3], (long)getpid()) <
	    (int)sizeof(copy));
	REQUIRE(snprintf(manifest, sizeof(manifest), "%s/large-sectors-%ld.tsv", argv[3],
		    (long)getpid()) < (int)sizeof(manifest));
	REQUIRE(clonefile(argv[1], copy, 0) == 0);
	REQUIRE(btrfs_image_open_writable(copy, &image) == 0);
	btrfs_image_writer(&image, &writer);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);

	/* Unaligned data across sector boundaries, a truncation inside a
	 * sector, an inline file, names. */
	REQUIRE(btrfs_transaction_begin(fs, &writer, &transaction) == BTRFS_OK);
	create(transaction, &model, "/", "new", MODE_REGULAR | 0644, NULL);
	fill(data, NEW_BYTES, 1, 0);
	write_file(transaction, find(&model, "/new"), 0, data, NEW_BYTES);
	fill(data, BIG_PATCH_BYTES, 2, 0);
	write_file(transaction, take(fs, &model, "/big"), BIG_PATCH_OFFSET, data, BIG_PATCH_BYTES);
	file = take(fs, &model, "/random");
	for (steps = 0; !done; steps++) {
		REQUIRE(steps < TRUNCATE_STEPS);
		REQUIRE(btrfs_transaction_truncate(transaction, object(transaction, "/random"),
			    RANDOM_TRUNCATED, time, BTRFS_RELEASE_STEP_NODES, &done) == BTRFS_OK);
	}
	file->size = RANDOM_TRUNCATED;
	create(transaction, &model, "/", "small", MODE_REGULAR | 0600, NULL);
	fill(data, SMALL_BYTES, 3, 1);
	write_file(transaction, find(&model, "/small"), 0, data, SMALL_BYTES);
	create(transaction, &model, "/", "dir", MODE_DIRECTORY | 0755, NULL);
	create(transaction, &model, "/dir", "link", MODE_SYMLINK | 0777, "../new");
	REQUIRE(btrfs_transaction_set_xattr(transaction, object(transaction, "/new"), NOTE_XATTR,
		    strlen(NOTE_XATTR), NOTE_VALUE, strlen(NOTE_VALUE), 0, time) == BTRFS_OK);
	REQUIRE(btrfs_transaction_unlink(transaction, object(transaction, "/many"), "entry-0005",
		    strlen("entry-0005"), time, 0) == BTRFS_OK);
	drop(&model, "/many/entry-0005");
	REQUIRE(btrfs_transaction_rename(transaction, object(transaction, "/many"), "entry-0006",
		    strlen("entry-0006"), object(transaction, "/dir"), "moved", strlen("moved"),
		    time, 0) == BTRFS_OK);
	file = find(&model, "/many/entry-0006");
	REQUIRE(file != NULL);
	strcpy(file->path, "/dir/moved");
	commit(&image, transaction, &fs, &model, "first commit");

	/* The inline file past a sector, Zstd extents by property, writes into
	 * preallocated space and a punched hole with an unaligned edge. */
	REQUIRE(btrfs_transaction_begin(fs, &writer, &transaction) == BTRFS_OK);
	fill(data, SMALL_GROWN - SMALL_BYTES, 4, 1);
	write_file(
	    transaction, find(&model, "/small"), SMALL_BYTES, data, SMALL_GROWN - SMALL_BYTES);
	create(transaction, &model, "/", "zstd", MODE_REGULAR | 0644, NULL);
	REQUIRE(btrfs_transaction_set_xattr(transaction, object(transaction, "/zstd"),
		    COMPRESSION_PROPERTY, strlen(COMPRESSION_PROPERTY), "zstd", 4, 0,
		    time) == BTRFS_OK);
	fill(data, ZSTD_BYTES, 5, 1);
	write_file(transaction, find(&model, "/zstd"), 0, data, ZSTD_BYTES);
	create(transaction, &model, "/", "prealloc", MODE_REGULAR | 0644, NULL);
	REQUIRE(btrfs_transaction_fallocate(transaction, object(transaction, "/prealloc"), 0, 0,
		    PREALLOC_BYTES, time) == BTRFS_OK);
	file = find(&model, "/prealloc");
	file->bytes = calloc(1, PREALLOC_BYTES);
	REQUIRE(file->bytes != NULL);
	file->size = PREALLOC_BYTES;
	fill(data, PREALLOC_WRITE_BYTES, 6, 0);
	write_file(transaction, file, PREALLOC_WRITE_OFFSET, data, PREALLOC_WRITE_BYTES);
	REQUIRE(btrfs_transaction_fallocate(transaction, object(transaction, "/new"),
		    BTRFS_FALLOCATE_PUNCH_HOLE | BTRFS_FALLOCATE_KEEP_SIZE, PUNCH_OFFSET,
		    PUNCH_BYTES, time) == BTRFS_OK);
	memset(find(&model, "/new")->bytes + PUNCH_OFFSET, 0, PUNCH_BYTES);
	commit(&image, transaction, &fs, &model, "second commit");
	check_layouts(fs);

	/* A writable snapshot of the subvolume, written to. */
	REQUIRE(btrfs_transaction_begin(fs, &writer, &transaction) == BTRFS_OK);
	REQUIRE(btrfs_transaction_snapshot(transaction, object(transaction, "/subvol").tree,
		    object(transaction, "/"), "snap2", strlen("snap2"), 0, time, uuid,
		    &snapshot) == BTRFS_OK);
	file = take(fs, &model, "/subvol/value");
	file = add(&model, "/snap2/value", file->mode);
	file->uid = find(&model, "/subvol/value")->uid;
	file->gid = find(&model, "/subvol/value")->gid;
	REQUIRE(btrfs_transaction_write_inline(transaction, object(transaction, "/snap2/value"),
		    SNAPSHOT_VALUE, strlen(SNAPSHOT_VALUE), time) == BTRFS_OK);
	file->bytes = (uint8_t *)strdup(SNAPSHOT_VALUE);
	REQUIRE(file->bytes != NULL);
	file->size = strlen(SNAPSHOT_VALUE);
	commit(&image, transaction, &fs, &model, "third commit");
	check_layouts(fs);
	btrfs_unmount(fs);
	REQUIRE(image.live_allocations == 0);
	btrfs_image_close(&image);
	write_manifest(&model, manifest);
	printf("64 KiB sectors: %zu files read as btrfs-progs wrote them; three transactions of "
	       "unaligned, inline, Zstd, preallocated, punched, namespace and snapshot writes read "
	       "back with Linux's extent layouts and pass both audits; written image %s and "
	       "manifest %s PASS\n",
	    model.count, copy, manifest);
	return 0;
}
