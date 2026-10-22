/* SPDX-License-Identifier: BSD-3-Clause */
/* Writes to a volume Linux made across several devices
 * (tests/prepare_multidev_linux.py), on a recording of every device. Five
 * transactions create, write, overwrite, snapshot, remove, move, truncate,
 * preallocate and punch; after each commit every file reads as an
 * independent model holds it (Linux's manifest for untouched files) and the
 * reference audit, which reads every copy of every block and checksummed
 * sector on its device, and the namespace audit pass. Every state a power
 * cut leaves at a barrier or superblock write of a commit, with all, only
 * the durable, or a random part of the writes since each device's last
 * barrier on media, reads as the commit before or the commit itself after
 * superblock recovery, which never selects anything else, and passes the
 * reference audit; some of them take a further transaction. The volume
 * reads the same mounted from each device. A writer without each device is
 * an invalid argument and a degraded mount is refused; volumes with RAID5/6
 * chunks are refused before any write. Then a balance relocates every group,
 * the last of each kind after a chunk placed across the devices as Linux's
 * btrfs_create_chunk places it takes its place (on a full volume a group may
 * be refused for space): the files read the same, every state a power cut
 * leaves at its barriers reads the same, and some of those states recover
 * the relocation and balance again. Conversions follow: metadata without a
 * second copy is refused unless forced, as are a profile needing more
 * devices and RAID5, before any write; then data and metadata convert to
 * other profiles the devices allow, every group ends with them, and the
 * conversion's crash states are checked as the balance's. With --grow, a sixth transaction
 * preallocates beyond the free data space instead and writes into the new
 * chunks, which are placed across the devices as Linux places them; its crash
 * states are checked as the others. With an export directory, the written
 * devices and the model's manifest go there for Linux. */
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
#define MANIFEST_LINE 512U
#define MANIFEST_FIELDS 3U
#define MAX_FILES 1024U
#define SHA256_HEX (2U * BT_SHA256_DIGEST)
#define MANIFEST_PREFIX "./"
#define MODE_TYPE 0170000U
#define MODE_REGULAR 0100000U
#define MODE_SYMLINK 0120000U
#define MODE_DIRECTORY 0040000U
#define SECONDS INT64_C(1800000000)
#define EVENT_GROWTH 4096U
#define EXPORT_PATH 4096U
#define TRUNCATE_STEPS 4096U
/* The writes: a file over many 64 KiB stripes, an overwrite inside a Linux
 * extent, an inline file grown past inline size, preallocation. */
#define LARGE_BYTES (1024U * 1024U + 12345U)
#define LARGE_TRUNCATED 100001U
#define OVERWRITE_OFFSET (3U * 1024U * 1024U + 1000U)
#define OVERWRITE_BYTES 200000U
#define SMALL_BYTES 1000U
#define SMALL_GROWN 6000U
#define SNAPSHOT_OFFSET (5U * 1024U * 1024U)
#define SNAPSHOT_BYTES 70000U
#define SNAPSHOT_VALUE "changed in a writable snapshot\n"
#define PREALLOC_BYTES (1024U * 1024U)
#define PREALLOC_WRITE_OFFSET 100000U
#define PREALLOC_WRITE_BYTES 70000U
#define PUNCH_OFFSET 4096U
#define PUNCH_BYTES 65536U
#define REMOVED_NAMES 50U
#define NOTE_XATTR "user.note"
#define NOTE_VALUE "several devices"
#define COMMITS 5U
/* With --grow: a preallocation this much beyond the free data space, which
 * new data chunks across the devices must hold, and a write into its end. */
#define GROW_BEYOND (UINT64_C(96) << 20)
#define GROW_WRITE_BYTES (1024U * 1024U)
#define GROW_WRITE_BEFORE_END (2U * 1024U * 1024U)
#define MAX_GROUPS 256U
/* Random parts of the pending writes tried at each crash position. */
#define RANDOM_STATES 1U
/* Crash states of the balance that recover it and balance again. */
#define RESUMED_BALANCES 3U
/* Linux's usage filter above 100 selects every group. */
#define BALANCE_EVERY_GROUP 101U

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

struct model {
	struct file files[MAX_FILES];
	size_t count;
};

/* A write to one device, or a barrier (no bytes). */
struct event {
	uint64_t offset;
	uint8_t *bytes;
	size_t length;
	uint32_t device;
};

struct volume;

struct port {
	struct volume *volume;
	uint32_t device;
};

/* The devices under recorded writes: each reads its image with the visible
 * writes to it on top, in order. The first shared events belong to the
 * volume this one was forked from. */
struct volume {
	struct btrfs_image *image;
	size_t devices;
	struct event *events;
	uint8_t *visible;
	size_t count;
	size_t capacity;
	size_t shared;
	struct port ports[MAX_DEVICES];
	struct btrfs_device readers[MAX_DEVICES - 1];
	struct btrfs_write_device writers[MAX_DEVICES - 1];
};

/* The committed states a crash may leave. */
struct commit {
	uint64_t generation;
	struct namespace_digest digest;
	size_t first;
	size_t end;
};

static enum btrfs_result
volume_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct port *port = context;
	struct volume *volume = port->volume;
	const struct btrfs_environment *base = &volume->image->environment;
	const struct event *event;
	uint64_t start;
	uint64_t end;
	size_t i;
	enum btrfs_result error;

	error = port->device == 0
	    ? base->read(base->context, offset, buffer, length)
	    : base->devices[port->device - 1].read(
		  base->devices[port->device - 1].context, offset, buffer, length);
	for (i = 0; error == BTRFS_OK && i < volume->count; i++) {
		event = &volume->events[i];
		if (event->bytes == NULL || event->device != port->device || !volume->visible[i]) {
			continue;
		}
		start = offset > event->offset ? offset : event->offset;
		end = offset + length < event->offset + event->length
		    ? offset + length
		    : event->offset + event->length;
		if (start < end) {
			memcpy((uint8_t *)buffer + (start - offset),
			    event->bytes + (start - event->offset), (size_t)(end - start));
		}
	}
	return error;
}

static void
volume_append(
    struct volume *volume, uint32_t device, uint64_t offset, const void *bytes, size_t length)
{
	struct event *event;

	if (volume->count == volume->capacity) {
		volume->capacity += EVENT_GROWTH;
		volume->events = realloc(volume->events, volume->capacity * sizeof(*event));
		volume->visible = realloc(volume->visible, volume->capacity);
		REQUIRE(volume->events != NULL && volume->visible != NULL);
	}
	event = &volume->events[volume->count];
	event->device = device;
	event->offset = offset;
	event->length = length;
	event->bytes = NULL;
	if (bytes != NULL) {
		event->bytes = malloc(length);
		REQUIRE(event->bytes != NULL);
		memcpy(event->bytes, bytes, length);
	}
	volume->visible[volume->count++] = 1;
}

static enum btrfs_result
volume_write(void *context, uint64_t offset, const void *bytes, size_t length)
{
	struct port *port = context;

	REQUIRE(length != 0 &&
	    offset + length <= (port->device == 0
				       ? port->volume->image->environment.size_bytes
				       : port->volume->readers[port->device - 1].size_bytes));
	volume_append(port->volume, port->device, offset, bytes, length);
	return BTRFS_OK;
}

static enum btrfs_result
volume_flush(void *context)
{
	struct port *port = context;

	volume_append(port->volume, port->device, 0, NULL, 0);
	return BTRFS_OK;
}

static void *
volume_allocate(void *context, size_t size)
{
	struct port *port = context;

	return port->volume->image->environment.allocate(port->volume->image, size);
}

static void
volume_release(void *context, void *bytes, size_t size)
{
	struct port *port = context;

	port->volume->image->environment.release(port->volume->image, bytes, size);
}

static void
volume_init(struct volume *volume, struct btrfs_image *image)
{
	size_t i;

	memset(volume, 0, sizeof(*volume));
	volume->image = image;
	volume->devices = image->environment.device_count + 1;
	for (i = 0; i < volume->devices; i++) {
		volume->ports[i] = (struct port){ volume, (uint32_t)i };
	}
	for (i = 1; i < volume->devices; i++) {
		volume->readers[i - 1] = (struct btrfs_device){ &volume->ports[i],
			image->environment.devices[i - 1].size_bytes, volume_read };
		volume->writers[i - 1] =
		    (struct btrfs_write_device){ &volume->ports[i], volume_write, volume_flush };
	}
}

/* The environments of the volume; first names the device it is mounted
 * from, the others follow in turn. */
static void
volume_environment(struct volume *volume, size_t first, struct btrfs_environment *environment,
    struct btrfs_write_environment *writer, struct btrfs_device *readers,
    struct btrfs_write_device *writers)
{
	size_t device;
	size_t i;

	*environment = volume->image->environment;
	environment->context = &volume->ports[first];
	environment->size_bytes = first == 0 ? volume->image->environment.size_bytes
					     : volume->readers[first - 1].size_bytes;
	environment->read = volume_read;
	environment->allocate = volume_allocate;
	environment->release = volume_release;
	*writer = (struct btrfs_write_environment){ &volume->ports[first], volume_write,
		volume_flush, btrfs_image_compress, BTRFS_COMPRESSION_NONE, 0, NULL, 0 };
	for (i = 1; i < volume->devices; i++) {
		device = (first + i) % volume->devices;
		readers[i - 1] = (struct btrfs_device){ &volume->ports[device],
			device == 0 ? volume->image->environment.size_bytes
				    : volume->readers[device - 1].size_bytes,
			volume_read };
		writers[i - 1] = (struct btrfs_write_device){ &volume->ports[device], volume_write,
			volume_flush };
	}
	environment->devices = volume->devices > 1 ? readers : NULL;
	environment->device_count = volume->devices - 1;
	writer->devices = volume->devices > 1 ? writers : NULL;
	writer->device_count = volume->devices - 1;
}

/* A crash state: the first count events of source, those visible marks on
 * media, then fresh writes. */
static void
volume_fork(struct volume *fork, const struct volume *source, size_t count, const uint8_t *visible)
{
	volume_init(fork, source->image);
	fork->capacity = count + EVENT_GROWTH;
	fork->events = malloc(fork->capacity * sizeof(*fork->events));
	fork->visible = malloc(fork->capacity);
	REQUIRE(fork->events != NULL && fork->visible != NULL);
	memcpy(fork->events, source->events, count * sizeof(*fork->events));
	memcpy(fork->visible, visible, count);
	fork->count = count;
	fork->shared = count;
}

static void
volume_free(struct volume *volume)
{
	size_t i;

	for (i = volume->shared; i < volume->count; i++) {
		free(volume->events[i].bytes);
	}
	free(volume->events);
	free(volume->visible);
}

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
		if ((file->mode & MODE_TYPE) == MODE_SYMLINK) {
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

static void
audit(const struct btrfs_fs *fs, const char *when, size_t *copies)
{
	struct reference_audit references;

	if (reference_audit(fs, &references) != 0) {
		fprintf(stderr, "%s: reference audit: %s\n", when, references.failure);
		exit(1);
	}
	*copies = references.checked_copies;
}

/* Every file reads as the model holds it; untouched ones as the manifest. */
static void
check_model(struct btrfs_fs *fs, struct model *model, const char *when, size_t *copies)
{
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
	audit(fs, when, copies);
	if (namespace_audit(fs, &names) != 0) {
		fprintf(stderr, "%s: namespace audit: %s\n", when, names.failure);
		exit(1);
	}
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
    const char *name, uint32_t mode)
{
	struct btrfs_new_inode attributes = { mode, 0, 0, 0, { SECONDS, 0 }, NULL, 0 };
	struct btrfs_object_id created;
	char path[MANIFEST_LINE];

	REQUIRE(btrfs_transaction_create(transaction, object(transaction, parent), name,
		    strlen(name), &attributes, &created) == BTRFS_OK);
	if ((mode & MODE_TYPE) != MODE_DIRECTORY) {
		REQUIRE(snprintf(path, sizeof(path), "%s/%s",
			    strcmp(parent, "/") == 0 ? "" : parent, name) < (int)sizeof(path));
		(void)add(model, path, mode);
	}
}

/* Copies the model's files below source to below target, as a snapshot. */
static void
copy_files(struct btrfs_fs *fs, struct model *model, const char *source, const char *target)
{
	char path[MANIFEST_LINE];
	struct file *original;
	struct file *copy;
	size_t count = model->count;
	size_t i;

	for (i = 0; i < count; i++) {
		original = &model->files[i];
		if (strncmp(original->path, source, strlen(source)) != 0 ||
		    original->path[strlen(source)] != '/') {
			continue;
		}
		original = take(fs, model, original->path);
		REQUIRE(snprintf(path, sizeof(path), "%s%s", target,
			    original->path + strlen(source)) < (int)sizeof(path));
		copy = add(model, path, original->mode);
		copy->uid = original->uid;
		copy->gid = original->gid;
		copy->size = original->size;
		copy->bytes = malloc(original->size + 1);
		REQUIRE(copy->bytes != NULL);
		memcpy(copy->bytes, original->bytes, original->size);
	}
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
		if ((file->mode & MODE_TYPE) == MODE_SYMLINK) {
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

static void
mount_volume(struct volume *volume, size_t first, struct btrfs_fs **fs)
{
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_device readers[MAX_DEVICES - 1];
	struct btrfs_write_device writers[MAX_DEVICES - 1];

	volume_environment(volume, first, &environment, &writer, readers, writers);
	REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, fs) == BTRFS_OK);
}

static void
begin(struct volume *volume, struct btrfs_fs *fs, struct btrfs_transaction **transaction)
{
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_device readers[MAX_DEVICES - 1];
	struct btrfs_write_device writers[MAX_DEVICES - 1];
	enum btrfs_result result;

	volume_environment(volume, 0, &environment, &writer, readers, writers);
	result = btrfs_transaction_begin(fs, &writer, transaction);
	if (result != BTRFS_OK) {
		fprintf(stderr, "begin: %s\n", btrfs_result_string(result));
		exit(1);
	}
}

/* Commits, mounts the result and checks it against the model. */
static void
commit(struct volume *volume, struct btrfs_transaction *transaction, struct btrfs_fs **fs,
    struct model *model, struct commit *record, const char *when, size_t *copies)
{
	struct btrfs_info info;

	REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(*fs);
	mount_volume(volume, 0, fs);
	check_model(*fs, model, when, copies);
	btrfs_get_info(*fs, &info);
	record->generation = info.generation;
	record->end = volume->count;
	REQUIRE(namespace_content_digest(*fs, &record->digest) == 0);
}

/* Mounts a state a crash left, recovering the superblocks first when their
 * copies disagree, as a writable mount does. */
static void
mount_state(struct volume *state, struct btrfs_fs **fs, size_t *recoveries)
{
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_device readers[MAX_DEVICES - 1];
	struct btrfs_write_device writers[MAX_DEVICES - 1];
	struct btrfs_recovery_report report;
	enum btrfs_result result;

	volume_environment(state, 0, &environment, &writer, readers, writers);
	result = btrfs_recover_supers(&environment, NULL, 0, &report);
	if (result == BTRFS_RECOVERY_REQUIRED) {
		REQUIRE(btrfs_recover_supers(&environment, &writer, 0, &report) == BTRFS_OK &&
		    report.rewritten != 0);
		REQUIRE(btrfs_recover_supers(&environment, NULL, 0, &report) == BTRFS_OK &&
		    report.rewritten == 0);
		(*recoveries)++;
	} else {
		REQUIRE(result == BTRFS_OK);
	}
	REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, fs) == BTRFS_OK);
}

static int
superblock_write(const struct event *event)
{
	unsigned mirror;

	for (mirror = 0; mirror < BT_SUPER_MIRRORS; mirror++) {
		if (event->bytes != NULL && event->offset == bt_super_offset(mirror) &&
		    event->length == BT_SUPER_SIZE) {
			return 1;
		}
	}
	return 0;
}

/* Marks the writes of the first count events a crash leaves on media: those
 * a later barrier of their device made durable, and of the others all
 * (policy 0), none (1) or a random part (2). */
static void
crash_visibility(
    const struct volume *volume, size_t count, unsigned policy, uint32_t *seed, uint8_t *visible)
{
	uint8_t flushed[MAX_DEVICES] = { 0 };
	const struct event *event;
	size_t i;

	for (i = count; i-- != 0;) {
		event = &volume->events[i];
		if (event->bytes == NULL) {
			flushed[event->device] = 1;
			visible[i] = 0;
			continue;
		}
		if (flushed[event->device]) {
			visible[i] = 1;
			continue;
		}
		*seed ^= *seed << 13;
		*seed ^= *seed >> 17;
		*seed ^= *seed << 5;
		visible[i] = policy == 0 ? 1 : policy == 1 ? 0 : (uint8_t)(*seed & 1U);
	}
}

/* Every state a crash leaves during commit k at a barrier or superblock
 * write reads as the commit before it or as the commit. */
static void
check_crashes(struct volume *volume, const struct commit *commits, size_t k, uint32_t *seed,
    size_t *states, size_t *recoveries, size_t *resumed)
{
	struct volume crash;
	struct btrfs_transaction *transaction;
	struct btrfs_new_inode attributes = { MODE_REGULAR | 0644, 0, 0, 0, { SECONDS, 0 }, NULL,
		0 };
	struct btrfs_object_id created;
	struct btrfs_info info;
	struct namespace_digest digest;
	struct btrfs_fs *fs;
	const struct commit *expected;
	uint8_t *visible = malloc(commits[k].end + 1);
	size_t copies;
	size_t position;
	size_t first_super = commits[k].end;
	size_t resumes = 0;
	unsigned policy;

	REQUIRE(visible != NULL);
	for (position = commits[k].first; position < commits[k].end; position++) {
		if (superblock_write(&volume->events[position])) {
			first_super = position;
			break;
		}
	}
	for (position = commits[k].first; position <= commits[k].end; position++) {
		if (position != commits[k].first && position != commits[k].end &&
		    volume->events[position].bytes != NULL &&
		    !superblock_write(&volume->events[position])) {
			continue;
		}
		for (policy = 0; policy < 2 + RANDOM_STATES; policy++) {
			crash_visibility(volume, position, policy, seed, visible);
			volume_fork(&crash, volume, position, visible);
			mount_state(&crash, &fs, recoveries);
			btrfs_get_info(fs, &info);
			expected = info.generation == commits[k].generation ? &commits[k]
			    : info.generation == commits[k - 1].generation  ? &commits[k - 1]
									    : NULL;
			REQUIRE(expected != NULL);
			/* Before the first superblock write only the old commit is
			 * on media; after the last barrier only the new one. */
			REQUIRE(position > first_super || expected == &commits[k - 1]);
			REQUIRE(position < commits[k].end || expected == &commits[k]);
			REQUIRE(namespace_content_digest(fs, &digest) == 0 &&
			    digest.hash == expected->digest.hash &&
			    digest.objects == expected->digest.objects &&
			    digest.bytes == expected->digest.bytes);
			audit(fs, "crash state", &copies);
			/* A state at a superblock write takes a further transaction. */
			if (policy == 2 && position < commits[k].end &&
			    superblock_write(&volume->events[position]) && resumes == 0) {
				begin(&crash, fs, &transaction);
				REQUIRE(btrfs_transaction_create(transaction,
					    (struct btrfs_object_id){
						BTRFS_TOP_LEVEL_TREE, BTRFS_ROOT_INODE },
					    "resumed", strlen("resumed"), &attributes,
					    &created) == BTRFS_OK);
				REQUIRE(btrfs_transaction_commit(transaction) == BTRFS_OK);
				btrfs_transaction_destroy(transaction);
				btrfs_unmount(fs);
				mount_volume(&crash, 0, &fs);
				audit(fs, "resumed crash state", &copies);
				resumes++;
				(*resumed)++;
			}
			btrfs_unmount(fs);
			volume_free(&crash);
			(*states)++;
		}
	}
	free(visible);
}

/* Every group of every kind, each keeping its profile. */
static void
balance_everything(struct btrfs_balance_args *args)
{
	struct btrfs_balance_kind every = { 1, BALANCE_EVERY_GROUP, BTRFS_BALANCE_KEEP, 0 };

	args->data = every;
	args->metadata = every;
	args->system = every;
}

/* Every state a crash leaves at a barrier or superblock write of the
 * balance reads as the volume did before it, after superblock recovery, and
 * passes the reference audit; some of them recover the relocation and
 * balance again to the same files. */
static void
check_balance_crashes(struct volume *volume, size_t first, size_t end,
    const struct btrfs_balance_args *args, const struct namespace_digest *expected, uint32_t *seed,
    size_t *states, size_t *recoveries, size_t *resumed)
{
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_device readers[MAX_DEVICES - 1];
	struct btrfs_write_device writers[MAX_DEVICES - 1];
	struct btrfs_relocation_report relocation;
	struct btrfs_balance_report report;
	struct namespace_digest digest;
	struct volume crash;
	struct btrfs_fs *fs;
	uint8_t *visible = malloc(end + 1);
	size_t copies;
	size_t position;
	size_t positions = 0;
	size_t taken = 0;
	unsigned policy;
	enum btrfs_result result;

	REQUIRE(visible != NULL);
	for (position = first; position <= end; position++) {
		positions +=
		    position == first || position == end || volume->events[position].bytes == NULL;
	}
	for (position = first; position <= end; position++) {
		if (position != first && position != end &&
		    volume->events[position].bytes != NULL) {
			continue;
		}
		taken++;
		for (policy = 0; policy < 2 + RANDOM_STATES; policy++) {
			crash_visibility(volume, position, policy, seed, visible);
			volume_fork(&crash, volume, position, visible);
			mount_state(&crash, &fs, recoveries);
			REQUIRE(namespace_content_digest(fs, &digest) == 0 &&
			    digest.hash == expected->hash && digest.objects == expected->objects &&
			    digest.bytes == expected->bytes);
			audit(fs, "balance crash state", &copies);
			btrfs_unmount(fs);
			if (policy == 2 && taken % (positions / RESUMED_BALANCES + 1) == 0) {
				volume_environment(
				    &crash, 0, &environment, &writer, readers, writers);
				result =
				    btrfs_recover_relocation(&environment, &writer, &relocation);
				REQUIRE(result == BTRFS_OK || result == BTRFS_NOT_FOUND);
				result = btrfs_balance(&environment, &writer, args, &report);
				REQUIRE(result == BTRFS_OK || result == BTRFS_NO_SPACE);
				mount_volume(&crash, 0, &fs);
				REQUIRE(namespace_content_digest(fs, &digest) == 0 &&
				    digest.hash == expected->hash &&
				    digest.objects == expected->objects);
				audit(fs, "balanced again", &copies);
				btrfs_unmount(fs);
				(*resumed)++;
			}
			volume_free(&crash);
			(*states)++;
		}
	}
	free(visible);
}

/* The profile of the volume's groups of kind (data, or else metadata). */
static uint64_t
profile_of(const struct btrfs_fs *fs, uint64_t kind)
{
	size_t i;

	for (i = 0; i < fs->chunk_count; i++) {
		if ((fs->chunks[i].type & BT_BLOCK_DATA) == (kind & BT_BLOCK_DATA)) {
			return fs->chunks[i].type & BT_BLOCK_PROFILES;
		}
	}
	return 0;
}

/* Conversions the device count allows, to a profile each kind does not have
 * yet: data to RAID1 or RAID0 on two devices, RAID1C3 or RAID1 on three,
 * RAID10 or RAID1C4 on four; metadata and system groups to DUP or RAID1, to
 * RAID1 or RAID1C3, to RAID1C4 or RAID10. */
static void
conversion_args(
    struct volume *volume, struct btrfs_balance_args *args, uint64_t *data, uint64_t *metadata)
{
	static const uint64_t targets[3][4] = {
		{ BTRFS_GROUP_RAID1, BTRFS_GROUP_RAID0, BTRFS_GROUP_DUP, BTRFS_GROUP_RAID1 },
		{ BTRFS_GROUP_RAID1C3, BTRFS_GROUP_RAID1, BTRFS_GROUP_RAID1, BTRFS_GROUP_RAID1C3 },
		{ BTRFS_GROUP_RAID10, BTRFS_GROUP_RAID1C4, BTRFS_GROUP_RAID1C4,
		    BTRFS_GROUP_RAID10 },
	};
	struct btrfs_fs *fs;
	const uint64_t *choice;

	REQUIRE(volume->devices >= 2 && volume->devices <= 4);
	choice = targets[volume->devices - 2];
	mount_volume(volume, 0, &fs);
	*data = profile_of(fs, BT_BLOCK_DATA) != choice[0] ? choice[0] : choice[1];
	*metadata = profile_of(fs, BT_BLOCK_METADATA) != choice[2] ? choice[2] : choice[3];
	btrfs_unmount(fs);
	memset(args, 0, sizeof(*args));
	args->data = (struct btrfs_balance_kind){ 1, BALANCE_EVERY_GROUP, *data, 0 };
	args->metadata = (struct btrfs_balance_kind){ 1, BALANCE_EVERY_GROUP, *metadata, 0 };
	args->system = args->metadata;
}

/* Conversions Linux's btrfs_balance refuses, before any write: metadata
 * without its second copy unless forced, a profile needing more devices,
 * and RAID5, which this writer does not write. */
static void
check_conversion_refusals(struct volume *volume)
{
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_device readers[MAX_DEVICES - 1];
	struct btrfs_write_device writers[MAX_DEVICES - 1];
	struct btrfs_balance_report report;
	struct btrfs_balance_args args;
	struct btrfs_balance_kind keep = { 1, BALANCE_EVERY_GROUP, BTRFS_BALANCE_KEEP, 0 };
	size_t count = volume->count;

	volume_environment(volume, 0, &environment, &writer, readers, writers);
	memset(&args, 0, sizeof(args));
	args.data = keep;
	args.metadata =
	    (struct btrfs_balance_kind){ 1, BALANCE_EVERY_GROUP, BTRFS_BALANCE_SINGLE, 0 };
	args.system = args.metadata;
	REQUIRE(btrfs_balance(&environment, &writer, &args, &report) == BTRFS_INVALID_ARGUMENT);
	args.metadata = keep;
	args.system = keep;
	if (volume->devices < 4) {
		args.data =
		    (struct btrfs_balance_kind){ 1, BALANCE_EVERY_GROUP, BTRFS_GROUP_RAID1C4, 0 };
		REQUIRE(
		    btrfs_balance(&environment, &writer, &args, &report) == BTRFS_INVALID_ARGUMENT);
	}
	args.data = (struct btrfs_balance_kind){ 1, BALANCE_EVERY_GROUP, BTRFS_GROUP_RAID5, 0 };
	REQUIRE(btrfs_balance(&environment, &writer, &args, &report) == BTRFS_UNSUPPORTED);
	REQUIRE(volume->count == count);
}

/* Applies the volume's writes to clones of its device images. */
static void
export_volume(const struct volume *volume, char **paths, const char *directory, const char *name)
{
	char path[EXPORT_PATH];
	const struct event *event;
	size_t device;
	size_t i;
	int file;

	for (device = 0; device < volume->devices; device++) {
		REQUIRE(snprintf(path, sizeof(path), "%s/%s-%zu.raw", directory, name, device) <
		    (int)sizeof(path));
		REQUIRE(clonefile(paths[device], path, 0) == 0);
		file = open(path, O_WRONLY);
		REQUIRE(file >= 0);
		for (i = 0; i < volume->count; i++) {
			event = &volume->events[i];
			if (event->bytes != NULL && event->device == device && volume->visible[i]) {
				REQUIRE(pwrite(file, event->bytes, event->length,
					    (off_t)event->offset) == (ssize_t)event->length);
			}
		}
		REQUIRE(fsync(file) == 0 && close(file) == 0);
	}
}

/* Exports the volume and the model's manifest when asked, and releases
 * them. */
static void
finish(struct volume *volume, struct model *model, char **argv, struct btrfs_image *image)
{
	char manifest[EXPORT_PATH];

	if (strcmp(argv[3], "-") != 0) {
		export_volume(volume, argv + 4, argv[3], argv[1]);
		REQUIRE(snprintf(manifest, sizeof(manifest), "%s/%s.expected.tsv", argv[3],
			    argv[1]) < (int)sizeof(manifest));
		write_manifest(model, manifest);
	}
	volume_free(volume);
	REQUIRE(image->live_allocations == 0);
	btrfs_image_close(image);
}

/* Writable admission refuses what it cannot write: a writer without each
 * device, a degraded mount and RAID5/6 chunks, before any write. */
static int
check_refusals(struct volume *volume, struct btrfs_fs *fs)
{
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_device readers[MAX_DEVICES - 1];
	struct btrfs_write_device writers[MAX_DEVICES - 1];
	struct btrfs_transaction *transaction = NULL;
	struct btrfs_fs *degraded;
	size_t i;
	int striped_parity = 0;

	volume_environment(volume, 0, &environment, &writer, readers, writers);
	writer.device_count--;
	REQUIRE(btrfs_transaction_begin(fs, &writer, &transaction) == BTRFS_INVALID_ARGUMENT);
	writer.device_count++;
	environment.device_count--;
	environment.degraded = 1;
	REQUIRE(btrfs_mount(&environment, BTRFS_TOP_LEVEL_TREE, &degraded) == BTRFS_OK);
	writer.device_count--;
	REQUIRE(btrfs_transaction_begin(degraded, &writer, &transaction) == BTRFS_UNSUPPORTED);
	btrfs_unmount(degraded);
	for (i = 0; i < fs->chunk_count; i++) {
		striped_parity |= (fs->chunks[i].type & (BT_BLOCK_RAID5 | BT_BLOCK_RAID6)) != 0;
	}
	volume_environment(volume, 0, &environment, &writer, readers, writers);
	if (striped_parity) {
		REQUIRE(btrfs_transaction_begin(fs, &writer, &transaction) == BTRFS_UNSUPPORTED);
	}
	REQUIRE(transaction == NULL && volume->count == 0);
	return striped_parity;
}

int
main(int argc, char **argv)
{
	static uint8_t data[LARGE_BYTES];
	static struct model model;
	struct btrfs_time time = { SECONDS, 0 };
	struct commit commits[COMMITS + 2];
	struct btrfs_group groups[MAX_GROUPS];
	struct btrfs_transaction *transaction;
	struct btrfs_image image;
	struct btrfs_fs *fs;
	struct volume volume;
	struct namespace_digest digest;
	struct btrfs_info info;
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_device readers[MAX_DEVICES - 1];
	struct btrfs_write_device writers[MAX_DEVICES - 1];
	struct btrfs_balance_args args;
	struct btrfs_balance_report report;
	struct file *file;
	const char *paths[MAX_DEVICES];
	char name[MANIFEST_LINE];
	uint8_t uuid[BTRFS_UUID_SIZE] = { 0x6d, 0x64 };
	uint64_t snapshot;
	uint32_t seed = 0x2545f491U;
	size_t devices;
	size_t copies = 0;
	size_t removed = 0;
	size_t states = 0;
	size_t recoveries = 0;
	size_t resumed = 0;
	size_t balance_first;
	size_t balance_end;
	size_t balance_states = 0;
	size_t rebalanced = 0;
	size_t grown = 0;
	size_t kept = 0;
	size_t convert_first;
	size_t convert_end;
	size_t convert_states = 0;
	uint64_t data_target;
	uint64_t metadata_target;
	uint64_t converted;
	uint64_t balanced_groups;
	size_t steps;
	size_t group_count;
	size_t commit_count = COMMITS;
	uint64_t highest;
	uint64_t free_data = 0;
	uint64_t grown_bytes;
	int grow = 0;
	enum btrfs_result balanced;
	size_t i;
	int done = 0;

	if (argc > 1 && strcmp(argv[1], "--grow") == 0) {
		grow = 1;
		commit_count = COMMITS + 1;
		argv++;
		argc--;
	}
	REQUIRE(argc >= 6);
	devices = (size_t)(argc - 4);
	REQUIRE(devices <= MAX_DEVICES);
	for (i = 0; i < devices; i++) {
		paths[i] = argv[4 + i];
	}
	read_manifest(argv[2], &model);
	REQUIRE(btrfs_image_open_devices(paths, devices, &image) == 0);
	volume_init(&volume, &image);
	mount_volume(&volume, 0, &fs);
	check_model(fs, &model, "fixture", &copies);
	if (check_refusals(&volume, fs)) {
		btrfs_unmount(fs);
		REQUIRE(image.live_allocations == 0);
		btrfs_image_close(&image);
		printf("multi-device writes: %zu devices with RAID5/6 chunks; a writer without "
		       "each device, a degraded mount and the volume itself are refused before "
		       "any write PASS\n",
		    devices);
		return 0;
	}
	btrfs_get_info(fs, &info);
	commits[0] = (struct commit){ info.generation, { 0 }, 0, 0 };
	REQUIRE(namespace_content_digest(fs, &commits[0].digest) == 0);

	/* A directory, an inline file, a file over many stripes, an xattr. */
	commits[1].first = volume.count;
	begin(&volume, fs, &transaction);
	create(transaction, &model, "/", "written", MODE_DIRECTORY | 0755);
	create(transaction, &model, "/written", "small", MODE_REGULAR | 0600);
	fill(data, SMALL_BYTES, 1, 1);
	write_file(transaction, find(&model, "/written/small"), 0, data, SMALL_BYTES);
	create(transaction, &model, "/written", "large", MODE_REGULAR | 0644);
	fill(data, LARGE_BYTES, 2, 0);
	write_file(transaction, find(&model, "/written/large"), 0, data, LARGE_BYTES);
	REQUIRE(btrfs_transaction_set_xattr(transaction, object(transaction, "/written/large"),
		    NOTE_XATTR, strlen(NOTE_XATTR), NOTE_VALUE, strlen(NOTE_VALUE), 0,
		    time) == BTRFS_OK);
	commit(&volume, transaction, &fs, &model, &commits[1], "first commit", &copies);

	/* An overwrite inside Linux's extent; the inline file grows past it. */
	commits[2].first = volume.count;
	begin(&volume, fs, &transaction);
	fill(data, OVERWRITE_BYTES, 3, 0);
	write_file(
	    transaction, take(fs, &model, "/random"), OVERWRITE_OFFSET, data, OVERWRITE_BYTES);
	fill(data, SMALL_GROWN - SMALL_BYTES, 4, 1);
	write_file(transaction, find(&model, "/written/small"), SMALL_BYTES, data,
	    SMALL_GROWN - SMALL_BYTES);
	commit(&volume, transaction, &fs, &model, &commits[2], "second commit", &copies);

	/* A writable snapshot of the subvolume, written to. */
	commits[3].first = volume.count;
	begin(&volume, fs, &transaction);
	REQUIRE(btrfs_transaction_snapshot(transaction, object(transaction, "/subvol").tree,
		    object(transaction, "/written"), "snap", strlen("snap"), 0, time, uuid,
		    &snapshot) == BTRFS_OK);
	copy_files(fs, &model, "/subvol", "/written/snap");
	file = find(&model, "/written/snap/value");
	REQUIRE(file != NULL);
	REQUIRE(btrfs_transaction_write_inline(transaction, object(transaction, file->path),
		    SNAPSHOT_VALUE, strlen(SNAPSHOT_VALUE), time) == BTRFS_OK);
	free(file->bytes);
	file->bytes = (uint8_t *)strdup(SNAPSHOT_VALUE);
	REQUIRE(file->bytes != NULL);
	file->size = strlen(SNAPSHOT_VALUE);
	fill(data, SNAPSHOT_BYTES, 5, 0);
	write_file(transaction, find(&model, "/written/snap/random"), SNAPSHOT_OFFSET, data,
	    SNAPSHOT_BYTES);
	commit(&volume, transaction, &fs, &model, &commits[3], "third commit", &copies);

	/* Names removed and moved; the large file truncated in steps. */
	commits[4].first = volume.count;
	begin(&volume, fs, &transaction);
	for (i = 0; i < REMOVED_NAMES; i++) {
		REQUIRE(snprintf(name, sizeof(name), "entry-%04zu", i) < (int)sizeof(name));
		REQUIRE(btrfs_transaction_unlink(transaction, object(transaction, "/many"), name,
			    strlen(name), time, 0) == BTRFS_OK);
		REQUIRE(snprintf(name, sizeof(name), "/many/entry-%04zu", i) < (int)sizeof(name));
		drop(&model, name);
	}
	REQUIRE(btrfs_transaction_rename(transaction, object(transaction, "/many"), "entry-0050",
		    strlen("entry-0050"), object(transaction, "/written"), "moved", strlen("moved"),
		    time, 0) == BTRFS_OK);
	file = find(&model, "/many/entry-0050");
	REQUIRE(file != NULL);
	strcpy(file->path, "/written/moved");
	for (steps = 0; !done; steps++) {
		REQUIRE(steps < TRUNCATE_STEPS);
		REQUIRE(
		    btrfs_transaction_truncate(transaction, object(transaction, "/written/large"),
			LARGE_TRUNCATED, time, BTRFS_RELEASE_STEP_NODES, &done) == BTRFS_OK);
	}
	find(&model, "/written/large")->size = LARGE_TRUNCATED;
	commit(&volume, transaction, &fs, &model, &commits[4], "fourth commit", &copies);

	/* Preallocation written into, a punched hole, unused groups removed. */
	commits[5].first = volume.count;
	begin(&volume, fs, &transaction);
	create(transaction, &model, "/written", "prealloc", MODE_REGULAR | 0644);
	REQUIRE(btrfs_transaction_fallocate(transaction, object(transaction, "/written/prealloc"),
		    0, 0, PREALLOC_BYTES, time) == BTRFS_OK);
	file = find(&model, "/written/prealloc");
	file->bytes = calloc(1, PREALLOC_BYTES);
	REQUIRE(file->bytes != NULL);
	file->size = PREALLOC_BYTES;
	fill(data, PREALLOC_WRITE_BYTES, 6, 0);
	write_file(transaction, file, PREALLOC_WRITE_OFFSET, data, PREALLOC_WRITE_BYTES);
	REQUIRE(btrfs_transaction_fallocate(transaction, object(transaction, "/written/large"),
		    BTRFS_FALLOCATE_PUNCH_HOLE | BTRFS_FALLOCATE_KEEP_SIZE, PUNCH_OFFSET,
		    PUNCH_BYTES, time) == BTRFS_OK);
	memset(find(&model, "/written/large")->bytes + PUNCH_OFFSET, 0, PUNCH_BYTES);
	REQUIRE(btrfs_transaction_remove_unused_groups(transaction, &removed) == BTRFS_OK);
	commit(&volume, transaction, &fs, &model, &commits[5], "fifth commit", &copies);

	highest = fs->chunks[fs->chunk_count - 1].logical;
	/* Preallocation beyond the free data space, written into at its end. */
	if (grow) {
		REQUIRE(btrfs_get_groups(fs, groups, MAX_GROUPS, &group_count) == BTRFS_OK);
		for (i = 0; i < group_count; i++) {
			if ((groups[i].flags & BTRFS_GROUP_DATA) != 0) {
				free_data += groups[i].length - groups[i].used;
			}
		}
		grown_bytes = free_data + GROW_BEYOND;
		commits[COMMITS + 1].first = volume.count;
		begin(&volume, fs, &transaction);
		create(transaction, &model, "/written", "grown", MODE_REGULAR | 0644);
		REQUIRE(
		    btrfs_transaction_fallocate(transaction, object(transaction, "/written/grown"),
			0, 0, grown_bytes, time) == BTRFS_OK);
		file = find(&model, "/written/grown");
		file->bytes = calloc(1, (size_t)grown_bytes);
		REQUIRE(file->bytes != NULL);
		file->size = (size_t)grown_bytes;
		fill(data, GROW_WRITE_BYTES, 7, 0);
		write_file(
		    transaction, file, grown_bytes - GROW_WRITE_BEFORE_END, data, GROW_WRITE_BYTES);
		commit(&volume, transaction, &fs, &model, &commits[COMMITS + 1], "growth commit",
		    &copies);
		for (i = 0; i < fs->chunk_count; i++) {
			grown += fs->chunks[i].logical > highest &&
			    (fs->chunks[i].type & BT_BLOCK_DATA) != 0;
		}
		REQUIRE(grown > 0);
	}

	/* Mounted from each device, the volume reads the same. */
	btrfs_unmount(fs);
	for (i = 1; i < devices; i++) {
		mount_volume(&volume, i, &fs);
		REQUIRE(namespace_content_digest(fs, &digest) == 0 &&
		    digest.hash == commits[commit_count].digest.hash &&
		    digest.objects == commits[commit_count].digest.objects);
		btrfs_unmount(fs);
	}
	for (i = 1; i <= commit_count; i++) {
		check_crashes(&volume, commits, i, &seed, &states, &recoveries, &resumed);
	}
	if (grow) {
		finish(&volume, &model, argv, &image);
		printf("multi-device growth: %zu devices, %zu files; six commits read back as the "
		       "model holds them and pass both audits (%zu checksummed copies), the last "
		       "preallocating %llu bytes beyond the free data space into %zu new data "
		       "chunks; the volume reads the same from each device; %zu crash states read "
		       "as a commit (%zu after superblock recovery) and pass the reference audit, "
		       "%zu taking a further transaction PASS\n",
		    devices, model.count, copies, (unsigned long long)GROW_BEYOND, grown, states,
		    recoveries, resumed);
		return 0;
	}

	/* A balance of every group. */
	balance_first = volume.count;
	volume_environment(&volume, 0, &environment, &writer, readers, writers);
	balance_everything(&args);
	balanced = btrfs_balance(&environment, &writer, &args, &report);
	REQUIRE(balanced == BTRFS_OK || balanced == BTRFS_NO_SPACE);
	balance_end = volume.count;
	mount_volume(&volume, 0, &fs);
	check_model(fs, &model, "balanced", &copies);
	REQUIRE(namespace_content_digest(fs, &digest) == 0 &&
	    digest.hash == commits[COMMITS].digest.hash &&
	    digest.objects == commits[COMMITS].digest.objects &&
	    digest.bytes == commits[COMMITS].digest.bytes);
	for (i = 0; i < fs->chunk_count; i++) {
		grown += fs->chunks[i].logical > highest;
		kept += fs->chunks[i].logical <= highest;
	}
	REQUIRE(report.groups > 0 && (balanced == BTRFS_NO_SPACE || kept == 0));
	balanced_groups = report.groups;
	btrfs_unmount(fs);

	check_balance_crashes(&volume, balance_first, balance_end, &args, &commits[COMMITS].digest,
	    &seed, &balance_states, &recoveries, &rebalanced);

	/* Conversions: refusals first, then data and metadata to other
	 * profiles the device count allows. */
	check_conversion_refusals(&volume);
	convert_first = volume.count;
	conversion_args(&volume, &args, &data_target, &metadata_target);
	volume_environment(&volume, 0, &environment, &writer, readers, writers);
	REQUIRE(btrfs_balance(&environment, &writer, &args, &report) == BTRFS_OK);
	convert_end = volume.count;
	mount_volume(&volume, 0, &fs);
	check_model(fs, &model, "converted", &copies);
	REQUIRE(namespace_content_digest(fs, &digest) == 0 &&
	    digest.hash == commits[COMMITS].digest.hash &&
	    digest.objects == commits[COMMITS].digest.objects &&
	    digest.bytes == commits[COMMITS].digest.bytes);
	for (i = 0; i < fs->chunk_count; i++) {
		REQUIRE((fs->chunks[i].type & BT_BLOCK_PROFILES) ==
		    ((fs->chunks[i].type & BT_BLOCK_DATA) != 0 ? data_target : metadata_target));
	}
	converted = report.groups;
	btrfs_unmount(fs);
	check_balance_crashes(&volume, convert_first, convert_end, &args, &commits[COMMITS].digest,
	    &seed, &convert_states, &recoveries, &rebalanced);
	finish(&volume, &model, argv, &image);
	printf("multi-device writes: %zu devices, %zu files; five commits read back as the model "
	       "holds them and pass both audits (%zu checksummed copies), %zu unused groups "
	       "removed; the volume reads the same from each device; %zu crash states read as a "
	       "commit and pass the reference audit, %zu taking a further transaction; a balance "
	       "relocated %llu groups (%s) into %zu new chunks, %zu old ones kept, reading the "
	       "same, as do %zu of its crash states; converting data to profile %llx and "
	       "metadata to %llx relocated %llu groups, reading the same, as do %zu of its "
	       "crash states; %zu balances resumed (%zu states after superblock recovery in "
	       "all); refusals before any write PASS\n",
	    devices, model.count, copies, removed, states, resumed,
	    (unsigned long long)balanced_groups, btrfs_result_string(balanced), grown, kept,
	    balance_states, (unsigned long long)data_target, (unsigned long long)metadata_target,
	    (unsigned long long)converted, convert_states, rebalanced, recoveries);
	return 0;
}
