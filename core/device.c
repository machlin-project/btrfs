/* SPDX-License-Identifier: BSD-3-Clause */
/* Devices joining and leaving a volume, as Linux's btrfs device add
 * (btrfs_init_new_device) and btrfs device remove (btrfs_rm_device), and
 * changing size, as btrfs filesystem resize (btrfs_grow_device,
 * btrfs_shrink_device). */
#include "transaction.h"

/* Linux's btrfs_ioctl_resize refuses smaller devices. */
#define BT_RESIZE_MINIMUM (UINT64_C(256) << 20)

enum btrfs_result
bt_tx_add_device(struct btrfs_transaction *transaction, const struct btrfs_device *device,
    const struct btrfs_write_device *output, const uint8_t uuid[BTRFS_UUID_SIZE], uint64_t *id)
{
	const struct bt_disk_super *candidate;
	struct bt_disk_device item;
	struct bt_tx_device *target;
	const struct btrfs_fs *fs;
	struct bt_key key;
	uint64_t next = 0;
	uint64_t size;
	size_t i;
	enum btrfs_result error;

	if (transaction == NULL || device == NULL || device->read == NULL || output == NULL ||
	    output->write == NULL || output->flush == NULL || uuid == NULL || id == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (transaction->failure != BTRFS_OK || transaction->finished) {
		return transaction->failure == BTRFS_OK ? BTRFS_READ_ONLY : transaction->failure;
	}
	fs = &transaction->fs;
	size = device->size_bytes & ~((uint64_t)fs->info.sector_size - 1);
	/* One device at a time, within the volume's limit; every device holds
	 * two superblock copies, as admission requires. */
	if (transaction->adding || transaction->leaving != 0 ||
	    fs->device_count == BT_MAX_DEVICES || !bt_super_present(size, 1)) {
		return BTRFS_UNSUPPORTED;
	}
	if (bt_u64(transaction->super.total_bytes) > UINT64_MAX - size) {
		return BTRFS_RANGE;
	}
	/* A device of this volume is not added again. */
	error = device->read(device->context, BT_SUPER_OFFSET, transaction->scratch, BT_SUPER_SIZE);
	if (error != BTRFS_OK) {
		return error;
	}
	candidate = (const void *)transaction->scratch;
	if (bt_super_check(candidate, BT_SUPER_OFFSET) == BTRFS_OK &&
	    bt_equal(candidate->fsid, transaction->super.fsid, BTRFS_UUID_SIZE)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	/* Linux's find_next_devid: after the highest device. */
	for (i = 0; i < fs->device_count; i++) {
		next = fs->devices[i].id > next ? fs->devices[i].id : next;
	}
	if (next == UINT64_MAX) {
		return BTRFS_RANGE;
	}
	next++;
	/* btrfs_add_dev_item: sector-size alignment, no bytes used yet. */
	bt_zero(&item, sizeof(item));
	bt_put64(&item.id, next);
	bt_put64(&item.total_bytes, size);
	bt_put32(&item.io_align, fs->info.sector_size);
	bt_put32(&item.io_width, fs->info.sector_size);
	bt_put32(&item.sector_size, fs->info.sector_size);
	bt_copy(item.uuid, uuid, BTRFS_UUID_SIZE);
	bt_copy(item.fsid, fs->metadata_uuid, BTRFS_UUID_SIZE);
	key = (struct bt_key){ BT_DEV_ITEMS_OBJECTID, next, BT_DEV_ITEM };
	error = bt_tx_edit(transaction, &transaction->chunks, key, &item, sizeof(item), BT_INSERT);
	if (error != BTRFS_OK) {
		return error;
	}
	bt_put64(&transaction->super.devices, bt_u64(transaction->super.devices) + 1);
	bt_put64(&transaction->super.total_bytes, bt_u64(transaction->super.total_bytes) + size);
	target = &transaction->targets[fs->device_count];
	bt_zero(target, sizeof(*target));
	target->context = output->context;
	target->write = output->write;
	target->flush = output->flush;
	target->item = item;
	target->size = size;
	transaction->target_count++;
	transaction->adding = 1;
	transaction->changed = 1;
	*id = next;
	return BTRFS_OK;
}

enum btrfs_result
bt_tx_remove_device(struct btrfs_transaction *transaction, uint64_t id)
{
	struct bt_disk_device item;
	const struct btrfs_fs *fs;
	const struct bt_chunk *chunk;
	struct bt_key key = { BT_DEV_ITEMS_OBJECTID, id, BT_DEV_ITEM };
	size_t index;
	size_t size;
	size_t i;
	unsigned stripe;
	enum btrfs_result error;

	if (transaction == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (transaction->failure != BTRFS_OK || transaction->finished) {
		return transaction->failure == BTRFS_OK ? BTRFS_READ_ONLY : transaction->failure;
	}
	fs = &transaction->fs;
	index = bt_tx_device_index(transaction, id);
	if (index == fs->device_count) {
		return BTRFS_NOT_FOUND;
	}
	if (fs->device_count < 2 || transaction->adding || transaction->leaving != 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	/* No chunk may keep a stripe there. */
	for (i = 0; i < fs->chunk_count; i++) {
		chunk = &fs->chunks[i];
		for (stripe = 0; stripe < chunk->stripes; stripe++) {
			if (chunk->devices[stripe] == index) {
				return BTRFS_INVALID_ARGUMENT;
			}
		}
	}
	error = bt_mutation_find(
	    transaction->mutation, transaction->chunks, key, &item, sizeof(item), &size);
	if (error != BTRFS_OK) {
		return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
	}
	if (size != sizeof(item) || bt_u64(item.used_bytes) != 0 ||
	    bt_u64(item.total_bytes) > bt_u64(transaction->super.total_bytes)) {
		return BTRFS_CORRUPT;
	}
	error = bt_tx_edit(transaction, &transaction->chunks, key, NULL, 0, BT_DELETE);
	if (error != BTRFS_OK) {
		return error;
	}
	bt_put64(&transaction->super.devices, bt_u64(transaction->super.devices) - 1);
	bt_put64(&transaction->super.total_bytes,
	    bt_u64(transaction->super.total_bytes) - bt_u64(item.total_bytes));
	transaction->leaving = index + 1;
	transaction->changed = 1;
	return BTRFS_OK;
}

enum btrfs_result
btrfs_device_add(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, const struct btrfs_device *device,
    const struct btrfs_write_device *output, const uint8_t uuid[BTRFS_UUID_SIZE], uint64_t *id)
{
	struct btrfs_transaction *transaction = NULL;
	struct btrfs_fs *fs = NULL;
	enum btrfs_result error;

	if (environment == NULL || writer == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	error = btrfs_mount(environment, BTRFS_TOP_LEVEL_TREE, &fs);
	if (error == BTRFS_OK) {
		error = btrfs_transaction_begin(fs, writer, &transaction);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_add_device(transaction, device, output, uuid, id);
	}
	if (error == BTRFS_OK) {
		error = btrfs_transaction_commit(transaction);
	}
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	return error;
}

/* Linux's btrfs_check_raid_min_devices: every profile in use keeps its
 * fewest devices without one of them. */
static enum btrfs_result
bt_device_removable(const struct btrfs_fs *fs, uint64_t id, size_t *index)
{
	unsigned fewest;
	int redundant;
	size_t i;

	for (*index = 0; *index < fs->device_count && fs->devices[*index].id != id; (*index)++) {
	}
	if (*index == fs->device_count) {
		return BTRFS_NOT_FOUND;
	}
	if (fs->device_count < 2) {
		return BTRFS_INVALID_ARGUMENT;
	}
	for (i = 0; i < fs->chunk_count; i++) {
		if (bt_space_profile(fs->chunks[i].type & BT_BLOCK_PROFILES, &fewest, &redundant) !=
			BTRFS_OK ||
		    fewest > fs->device_count - 1) {
			return BTRFS_INVALID_ARGUMENT;
		}
	}
	return BTRFS_OK;
}

/* As btrfs_scratch_superblocks: every superblock copy the leaving device
 * holds loses its magic, so no scan finds it in the volume again. */
static enum btrfs_result
bt_device_scratch(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, size_t index, uint64_t size)
{
	struct bt_disk_super *super;
	void *context = index == 0 ? environment->context : environment->devices[index - 1].context;
	enum btrfs_result (*read)(void *, uint64_t, void *, size_t) =
	    index == 0 ? environment->read : environment->devices[index - 1].read;
	void *output = index == 0 ? writer->context : writer->devices[index - 1].context;
	enum btrfs_result (*write)(void *, uint64_t, const void *, size_t) =
	    index == 0 ? writer->write : writer->devices[index - 1].write;
	enum btrfs_result (*flush)(void *) =
	    index == 0 ? writer->flush : writer->devices[index - 1].flush;
	unsigned mirror;
	enum btrfs_result error = BTRFS_OK;

	super = environment->allocate(environment->context, sizeof(*super));
	if (super == NULL) {
		return BTRFS_NO_MEMORY;
	}
	for (mirror = 0; error == BTRFS_OK && mirror < BT_SUPER_MIRRORS; mirror++) {
		if (!bt_super_present(size, mirror)) {
			continue;
		}
		error = read(context, bt_super_offset(mirror), super, sizeof(*super));
		if (error == BTRFS_OK && bt_equal(super->magic, BT_MAGIC, sizeof(super->magic))) {
			bt_zero(super->magic, sizeof(super->magic));
			error = write(output, bt_super_offset(mirror), super, sizeof(*super));
		}
	}
	if (error == BTRFS_OK) {
		error = flush(output);
	}
	environment->release(environment->context, super, sizeof(*super));
	return error;
}

enum btrfs_result
btrfs_device_remove(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t id, struct btrfs_balance_report *report)
{
	struct btrfs_transaction *transaction = NULL;
	struct btrfs_fs *fs = NULL;
	uint64_t size = 0;
	size_t index = 0;
	enum btrfs_result error;

	if (environment == NULL || writer == NULL || report == NULL ||
	    writer->device_count != environment->device_count) {
		return BTRFS_INVALID_ARGUMENT;
	}
	bt_zero(report, sizeof(*report));
	error = btrfs_mount(environment, BTRFS_TOP_LEVEL_TREE, &fs);
	if (error == BTRFS_OK) {
		error = bt_device_removable(fs, id, &index);
		size = index < fs->device_count ? fs->devices[index].size : 0;
	}
	btrfs_unmount(fs);
	fs = NULL;
	/* The device's groups move to the other devices first. */
	if (error == BTRFS_OK) {
		error = bt_relocate_device(environment, writer, id, 0, BT_BALANCE_WORK, report);
	}
	if (error == BTRFS_OK) {
		error = btrfs_mount(environment, BTRFS_TOP_LEVEL_TREE, &fs);
	}
	if (error == BTRFS_OK) {
		error = btrfs_transaction_begin(fs, writer, &transaction);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_remove_device(transaction, id);
	}
	if (error == BTRFS_OK) {
		error = btrfs_transaction_commit(transaction);
		report->commits += error == BTRFS_OK;
	}
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	/* Only once the others no longer name it. */
	if (error == BTRFS_OK) {
		error = bt_device_scratch(environment, writer, index, size);
	}
	return error;
}

enum btrfs_result
bt_tx_resize_device(struct btrfs_transaction *transaction, uint64_t id, uint64_t size)
{
	struct bt_disk_device item;
	const struct btrfs_fs *fs;
	const struct bt_chunk *chunk;
	struct bt_key key = { BT_DEV_ITEMS_OBJECTID, id, BT_DEV_ITEM };
	uint64_t old;
	uint64_t total;
	size_t index;
	size_t length;
	size_t i;
	unsigned stripe;
	enum btrfs_result error;

	if (transaction == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (transaction->failure != BTRFS_OK || transaction->finished) {
		return transaction->failure == BTRFS_OK ? BTRFS_READ_ONLY : transaction->failure;
	}
	fs = &transaction->fs;
	index = bt_tx_device_index(transaction, id);
	if (index == fs->device_count) {
		return BTRFS_NOT_FOUND;
	}
	if (transaction->adding || transaction->leaving != 0 || size % fs->info.sector_size != 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	/* No stripe may lie past the new end. */
	for (i = 0; i < fs->chunk_count; i++) {
		chunk = &fs->chunks[i];
		for (stripe = 0; stripe < chunk->stripes; stripe++) {
			if (chunk->devices[stripe] == index &&
			    chunk->physical[stripe] + bt_chunk_stripe_length(chunk) > size) {
				return BTRFS_INVALID_ARGUMENT;
			}
		}
	}
	error = bt_mutation_find(
	    transaction->mutation, transaction->chunks, key, &item, sizeof(item), &length);
	if (error != BTRFS_OK) {
		return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
	}
	old = bt_u64(item.total_bytes);
	total = bt_u64(transaction->super.total_bytes);
	if (length != sizeof(item) || old > total || bt_u64(item.used_bytes) > size ||
	    (size > old && size - old > UINT64_MAX - total)) {
		return BTRFS_CORRUPT;
	}
	/* btrfs_update_device and the superblock's total. */
	bt_put64(&item.total_bytes, size);
	error = bt_tx_edit(transaction, &transaction->chunks, key, &item, sizeof(item), BT_REPLACE);
	if (error != BTRFS_OK) {
		return error;
	}
	bt_put64(&transaction->super.total_bytes, total - old + size);
	transaction->targets[index].item.total_bytes = item.total_bytes;
	transaction->targets[index].size = size;
	transaction->changed = 1;
	return BTRFS_OK;
}

enum btrfs_result
btrfs_device_resize(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t id, uint64_t size,
    struct btrfs_balance_report *report)
{
	struct btrfs_transaction *transaction = NULL;
	struct btrfs_fs *fs = NULL;
	uint64_t old = 0;
	uint64_t available = 0;
	size_t index = 0;
	enum btrfs_result error;

	if (environment == NULL || writer == NULL || report == NULL ||
	    writer->device_count != environment->device_count) {
		return BTRFS_INVALID_ARGUMENT;
	}
	bt_zero(report, sizeof(*report));
	error = btrfs_mount(environment, BTRFS_TOP_LEVEL_TREE, &fs);
	if (error == BTRFS_OK) {
		for (index = 0; index < fs->device_count && fs->devices[index].id != id; index++) {
		}
		error = index == fs->device_count ? BTRFS_NOT_FOUND : BTRFS_OK;
	}
	if (error == BTRFS_OK) {
		old = fs->devices[index].size;
		available = index == 0 ? environment->size_bytes
				       : environment->devices[index - 1].size_bytes;
		/* btrfs_ioctl_resize: at least 256 MiB, at most the device, in
		 * whole sectors. */
		error = size < BT_RESIZE_MINIMUM ? BTRFS_INVALID_ARGUMENT
		    : size > available		 ? BTRFS_RANGE
						 : BTRFS_OK;
		size &= ~((uint64_t)fs->info.sector_size - 1);
	}
	btrfs_unmount(fs);
	fs = NULL;
	if (error != BTRFS_OK || size == old) {
		return error;
	}
	/* Shrinking moves the groups past the new end first. */
	if (size < old) {
		error = bt_relocate_device(environment, writer, id, size, BT_BALANCE_WORK, report);
	}
	if (error == BTRFS_OK) {
		error = btrfs_mount(environment, BTRFS_TOP_LEVEL_TREE, &fs);
	}
	if (error == BTRFS_OK) {
		error = btrfs_transaction_begin(fs, writer, &transaction);
	}
	if (error == BTRFS_OK) {
		error = bt_tx_resize_device(transaction, id, size);
	}
	if (error == BTRFS_OK) {
		error = btrfs_transaction_commit(transaction);
		report->commits += error == BTRFS_OK;
	}
	btrfs_transaction_destroy(transaction);
	btrfs_unmount(fs);
	return error;
}

/* Bytes a device replace copies per read and write. */
#define BT_REPLACE_PIECE (UINT64_C(1) << 20)

/* The stripe of a chunk that holds what stripe holds, on another present
 * device: any other copy of RAID1, RAID1C3 and RAID1C4, the partner of a
 * RAID10 pair; none for other profiles (UNSUPPORTED). */
static enum btrfs_result
bt_replace_mirror(
    const struct btrfs_fs *fs, const struct bt_chunk *chunk, unsigned stripe, unsigned *mirror)
{
	uint64_t profile = chunk->type & BT_BLOCK_PROFILES;
	unsigned i;

	if (profile == BT_BLOCK_RAID10) {
		*mirror = stripe ^ 1U;
		return fs->devices[chunk->devices[*mirror]].read != NULL ? BTRFS_OK : BTRFS_IO;
	}
	if (profile != BT_BLOCK_RAID1 && profile != BT_BLOCK_RAID1C3 &&
	    profile != BT_BLOCK_RAID1C4) {
		return BTRFS_UNSUPPORTED;
	}
	for (i = 0; i < chunk->stripes; i++) {
		if (i != stripe && chunk->devices[i] != chunk->devices[stripe] &&
		    fs->devices[chunk->devices[i]].read != NULL) {
			*mirror = i;
			return BTRFS_OK;
		}
	}
	return BTRFS_IO;
}

/* Copies every stripe the device of index holds to output at the same
 * offset: from the device, or where it is missing or cannot be read, from
 * a stripe holding the same bytes on another device. */
static enum btrfs_result
bt_replace_copy(const struct btrfs_fs *fs, size_t index, const struct btrfs_write_device *output,
    uint8_t *buffer)
{
	const struct bt_chunk *chunk;
	uint64_t length;
	uint64_t offset;
	size_t piece;
	size_t i;
	unsigned stripe;
	unsigned mirror;
	enum btrfs_result error = BTRFS_OK;

	for (i = 0; error == BTRFS_OK && i < fs->chunk_count; i++) {
		chunk = &fs->chunks[i];
		length = bt_chunk_stripe_length(chunk);
		for (stripe = 0; error == BTRFS_OK && stripe < chunk->stripes; stripe++) {
			if (chunk->devices[stripe] != index) {
				continue;
			}
			for (offset = 0; error == BTRFS_OK && offset < length; offset += piece) {
				piece = length - offset < BT_REPLACE_PIECE
				    ? (size_t)(length - offset)
				    : (size_t)BT_REPLACE_PIECE;
				error = fs->devices[index].read == NULL
				    ? BTRFS_IO
				    : bt_read_device(fs, (uint32_t)index,
					  chunk->physical[stripe] + offset, buffer, piece);
				if (error == BTRFS_IO) {
					error = bt_replace_mirror(fs, chunk, stripe, &mirror);
					if (error == BTRFS_OK) {
						error = bt_read_device(fs, chunk->devices[mirror],
						    chunk->physical[mirror] + offset, buffer,
						    piece);
					}
				}
				if (error == BTRFS_OK) {
					error = output->write(output->context,
					    chunk->physical[stripe] + offset, buffer, piece);
				}
			}
		}
	}
	return error;
}

/* The device's item in the chunk tree: what a missing device's size and
 * superblocks come from. */
static enum btrfs_result
bt_replace_item(const struct btrfs_fs *fs, size_t index, struct bt_disk_device *item)
{
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key = { BT_DEV_ITEMS_OBJECTID, fs->devices[index].id, BT_DEV_ITEM };
	enum btrfs_result error;

	bt_cursor_init(&cursor, fs, fs->chunk_tree);
	error = bt_cursor_seek(&cursor, key, 0);
	if (error == BTRFS_OK) {
		(void)bt_cursor_record(&cursor, &record);
		error = bt_key_compare(record.key, key) != 0 || record.size != sizeof(*item)
		    ? BTRFS_CORRUPT
		    : BTRFS_OK;
		if (error == BTRFS_OK) {
			bt_copy(item, record.data, sizeof(*item));
			error = bt_equal(item->uuid, fs->devices[index].uuid, BTRFS_UUID_SIZE)
			    ? BTRFS_OK
			    : BTRFS_CORRUPT;
		}
	}
	bt_cursor_fini(&cursor);
	return error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
}

/* The superblock copies the replaced device of size held, onto output: its
 * own, or where it is missing, another device's with its device item, as
 * recovery builds them. */
static enum btrfs_result
bt_replace_supers(const struct btrfs_fs *fs, size_t index, uint64_t size,
    const struct bt_disk_device *item, const struct btrfs_write_device *output,
    struct bt_disk_super *super)
{
	size_t other;
	unsigned mirror;
	enum btrfs_result error = BTRFS_OK;

	/* Secondaries first, the primary last. */
	for (mirror = BT_SUPER_MIRRORS; error == BTRFS_OK && mirror-- != 0;) {
		if (!bt_super_present(size, mirror)) {
			continue;
		}
		if (fs->devices[index].read != NULL) {
			error = bt_read_device(
			    fs, (uint32_t)index, bt_super_offset(mirror), super, sizeof(*super));
		} else {
			for (other = 0; other < fs->device_count && fs->devices[other].read == NULL;
			    other++) {
			}
			error = bt_read_device(
			    fs, (uint32_t)other, BT_SUPER_OFFSET, super, sizeof(*super));
			if (error == BTRFS_OK) {
				super->device = *item;
				bt_put64(&super->log_root, 0);
				super->log_level = 0;
			}
		}
		if (error == BTRFS_OK) {
			error = bt_super_check(super, bt_super_offset(mirror)) == BTRFS_OK ||
				fs->devices[index].read == NULL
			    ? BTRFS_OK
			    : BTRFS_CORRUPT;
		}
		if (error == BTRFS_OK) {
			bt_super_seal(super, bt_super_offset(mirror));
			error = output->write(
			    output->context, bt_super_offset(mirror), super, sizeof(*super));
		}
	}
	return error;
}

enum btrfs_result
btrfs_device_replace(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t id, const struct btrfs_device *target,
    const struct btrfs_write_device *output)
{
	struct btrfs_environment uncached;
	struct bt_disk_super *super = NULL;
	struct bt_disk_device item;
	struct btrfs_fs *fs = NULL;
	uint8_t *buffer = NULL;
	uint64_t size = 0;
	size_t index = 0;
	size_t i;
	int present = 0;
	enum btrfs_result error;

	if (environment == NULL || writer == NULL || target == NULL || target->read == NULL ||
	    output == NULL || output->write == NULL || output->flush == NULL ||
	    writer->device_count != environment->device_count) {
		return BTRFS_INVALID_ARGUMENT;
	}
	uncached = *environment;
	uncached.cache = NULL;
	error = btrfs_mount(&uncached, BTRFS_TOP_LEVEL_TREE, &fs);
	if (error == BTRFS_OK) {
		for (index = 0; index < fs->device_count && fs->devices[index].id != id; index++) {
		}
		error = index == fs->device_count ? BTRFS_NOT_FOUND : BTRFS_OK;
	}
	/* Every other device is present; the target holds the device's size and
	 * is not a device of the volume already. */
	for (i = 0; error == BTRFS_OK && i < fs->device_count; i++) {
		error = i != index && fs->devices[i].read == NULL ? BTRFS_UNSUPPORTED : BTRFS_OK;
	}
	if (error == BTRFS_OK) {
		present = fs->devices[index].read != NULL;
		error = bt_replace_item(fs, index, &item);
	}
	if (error == BTRFS_OK) {
		size = bt_u64(item.total_bytes);
		error = target->size_bytes < size ? BTRFS_INVALID_ARGUMENT : BTRFS_OK;
	}
	if (error == BTRFS_OK) {
		super = fs->env.allocate(fs->env.context, sizeof(*super));
		buffer = fs->env.allocate(fs->env.context, BT_REPLACE_PIECE);
		error = super == NULL || buffer == NULL ? BTRFS_NO_MEMORY : BTRFS_OK;
	}
	if (error == BTRFS_OK) {
		error = target->read(target->context, BT_SUPER_OFFSET, super, sizeof(*super));
	}
	if (error == BTRFS_OK && bt_super_check(super, BT_SUPER_OFFSET) == BTRFS_OK &&
	    bt_equal(super->fsid, fs->info.uuid, BTRFS_UUID_SIZE)) {
		error = BTRFS_INVALID_ARGUMENT;
	}
	/* The stripes, a barrier, the superblocks, a barrier: the target holds
	 * the device whole before it names itself one. */
	if (error == BTRFS_OK) {
		error = bt_replace_copy(fs, index, output, buffer);
	}
	if (error == BTRFS_OK) {
		error = output->flush(output->context);
	}
	if (error == BTRFS_OK) {
		error = bt_replace_supers(fs, index, size, &item, output, super);
	}
	if (error == BTRFS_OK) {
		error = output->flush(output->context);
	}
	if (super != NULL) {
		fs->env.release(fs->env.context, super, sizeof(*super));
	}
	if (buffer != NULL) {
		fs->env.release(fs->env.context, buffer, BT_REPLACE_PIECE);
	}
	btrfs_unmount(fs);
	/* The replaced device leaves once the target holds it. */
	if (error == BTRFS_OK && present) {
		error = bt_device_scratch(environment, writer, index, size);
	}
	return error;
}
