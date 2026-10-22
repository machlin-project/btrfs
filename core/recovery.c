/* SPDX-License-Identifier: BSD-3-Clause */
#include "encode.h"
#include "space.h"
#include <btrfs/write.h>

/* One device of the volume as recovery finds it: its reads, the copies it
 * holds and their report, and the superblock the selected generation gives
 * it, which names its own device item and is sealed as its primary. */
struct bt_recovery_device {
	void *context;
	enum btrfs_result (*read)(void *context, uint64_t offset, void *buffer, size_t length);
	uint64_t size;
	struct btrfs_super_copy *report;
	struct bt_disk_super copies[BTRFS_SUPER_COPIES];
	struct bt_disk_super expected;
	/* A copy at the selected generation names the device's item; otherwise
	 * its used bytes come from the selection's chunk tree. */
	int current_item;
};

/* A further device under validation: its primary reads as the selection
 * gives it, everything else as the device holds it. */
static enum btrfs_result
bt_recovery_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct bt_recovery_device *device = context;

	if (offset == BT_SUPER_OFFSET && length == sizeof(device->expected)) {
		bt_copy(buffer, &device->expected, length);
		return BTRFS_OK;
	}
	return device->read(device->context, offset, buffer, length);
}

/* The used bytes of each device without a copy at the selected generation,
 * from the selection's chunk tree, whose device item must otherwise agree
 * with the device's own. */
static enum btrfs_result
bt_recovery_items(const struct btrfs_fs *fs, struct bt_recovery_device *devices, size_t count)
{
	const struct bt_disk_device *item;
	struct bt_disk_device *own;
	struct bt_cursor cursor;
	struct bt_record record;
	struct bt_key key;
	size_t i;
	enum btrfs_result error = BTRFS_OK;

	for (i = 0; error == BTRFS_OK && i < count; i++) {
		if (devices[i].current_item) {
			continue;
		}
		own = &devices[i].expected.device;
		key = (struct bt_key){ BT_DEV_ITEMS_OBJECTID, bt_u64(own->id), BT_DEV_ITEM };
		bt_cursor_init(&cursor, fs, fs->chunk_tree);
		error = bt_cursor_seek(&cursor, key, 0);
		if (error == BTRFS_OK) {
			(void)bt_cursor_record(&cursor, &record);
			item = (const void *)record.data;
			if (bt_key_compare(record.key, key) != 0 || record.size != sizeof(*item) ||
			    !bt_equal(item->uuid, own->uuid, BTRFS_UUID_SIZE) ||
			    bt_u64(item->total_bytes) != bt_u64(own->total_bytes)) {
				error = BTRFS_CORRUPT;
			} else {
				own->used_bytes = item->used_bytes;
				bt_super_seal(&devices[i].expected, BT_SUPER_OFFSET);
			}
		}
		bt_cursor_fini(&cursor);
		error = error == BTRFS_NOT_FOUND ? BTRFS_CORRUPT : error;
	}
	return error;
}

/* Opens the selection as a complete committed state on every device: chunk
 * tree, root tree, checksum and top-level roots (through mount), then the
 * device tree root and the extent tree with its block-group accounting. File
 * trees are not scrubbed, nor a tree log the selected primary names: its
 * replay follows recovery. */
static enum btrfs_result
bt_recovery_validate(
    const struct btrfs_environment *environment, struct bt_recovery_device *devices, size_t count)
{
	struct btrfs_environment uncached = *environment;
	struct btrfs_device further[BTRFS_MAX_DEVICES - 1];
	struct btrfs_fs *fs;
	struct bt_space *space = NULL;
	struct bt_root roots = { 0 };
	struct bt_root extents = { 0 };
	uint8_t *node;
	size_t i;
	enum btrfs_result error;

	/* A candidate may still be refused; its nodes never enter the cache. */
	uncached.cache = NULL;
	for (i = 1; i < count; i++) {
		further[i - 1] = environment->devices[i - 1];
		further[i - 1].context = &devices[i];
		further[i - 1].read = bt_recovery_read;
	}
	uncached.devices = count > 1 ? further : NULL;
	error = bt_mount_super(&uncached, &devices[0].expected, BT_SUPER_OFFSET,
	    BTRFS_TOP_LEVEL_TREE, bt_u64(devices[0].expected.log_root) != 0, &fs);
	if (error != BTRFS_OK) {
		return error;
	}
	error = bt_recovery_items(fs, devices, count);
	if (error == BTRFS_OK) {
		error = bt_find_root(fs, BT_DEV_TREE, &roots);
	}
	if (error == BTRFS_OK) {
		error = bt_find_root(fs, BT_EXTENT_TREE, &extents);
	}
	if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		node = environment->allocate(environment->context, fs->info.node_size);
		error = node == NULL ? BTRFS_NO_MEMORY : bt_tree_read(fs, roots, node);
		if (node != NULL) {
			environment->release(environment->context, node, fs->info.node_size);
		}
	}
	if (error == BTRFS_OK) {
		error = bt_space_create(fs, extents, NULL, 1, &space);
	}
	/* A candidate's whole extent tree must verify. */
	if (error == BTRFS_OK) {
		error = bt_space_load_all(space);
	}
	bt_space_destroy(space);
	btrfs_unmount(fs);
	return error;
}

/* Every copy inside each device is read before deciding. An unreadable copy
 * could hold the newest state, so an I/O failure stops the decision. */
static enum btrfs_result
bt_recovery_read_copies(struct bt_recovery_device *device)
{
	struct btrfs_super_copy *copy;
	unsigned i;
	enum btrfs_result error = BTRFS_OK;

	for (i = 0; i < BTRFS_SUPER_COPIES && error == BTRFS_OK; i++) {
		copy = &device->report[i];
		copy->offset = bt_super_offset(i);
		copy->status = BTRFS_NOT_FOUND;
		if (!bt_super_present(device->size, i)) {
			continue;
		}
		error = device->read(
		    device->context, copy->offset, &device->copies[i], sizeof(device->copies[i]));
		if (error == BTRFS_OK) {
			copy->status = bt_super_check(&device->copies[i], copy->offset);
			copy->generation =
			    copy->status == BTRFS_OK ? bt_u64(device->copies[i].generation) : 0;
		}
	}
	return error;
}

/* The newest checksum-valid copy on any device, the first found of equal
 * ones. */
static enum btrfs_result
bt_recovery_select(
    const struct bt_recovery_device *devices, size_t count, struct btrfs_recovery_report *report)
{
	const struct btrfs_super_copy *copy;
	size_t best_device = count;
	size_t i;
	unsigned best = BTRFS_SUPER_COPIES;
	unsigned j;

	for (i = 0; i < count; i++) {
		for (j = 0; j < BTRFS_SUPER_COPIES; j++) {
			copy = &devices[i].report[j];
			if (copy->status == BTRFS_OK &&
			    (best_device == count ||
				copy->generation > devices[best_device].report[best].generation)) {
				best_device = i;
				best = j;
			}
		}
	}
	if (best_device == count) {
		for (i = 0; i < count; i++) {
			for (j = 0; j < BTRFS_SUPER_COPIES; j++) {
				if (devices[i].report[j].status == BTRFS_UNSUPPORTED) {
					return BTRFS_UNSUPPORTED;
				}
			}
		}
		return BTRFS_CORRUPT;
	}
	/* Linux writes a tree log into primary copies alone. */
	if (bt_u64(devices[best_device].copies[best].log_root) != 0 && best != 0) {
		return BTRFS_UNSUPPORTED;
	}
	report->selected = best;
	report->selected_device = (unsigned)best_device;
	report->generation = devices[best_device].report[best].generation;
	return BTRFS_OK;
}

/* Each device's item: from its copy at the selected generation, else its
 * newest copy, with the used bytes the selection's chunk tree gives it
 * (bt_recovery_items). That copy lies among those the item's size keeps,
 * every valid one of them names the selection's filesystem and the same
 * device, and no two devices are one; otherwise the copies are ambiguous. */
static enum btrfs_result
bt_recovery_identify(struct bt_recovery_device *devices, size_t count,
    const struct bt_disk_super *selected, uint64_t generation)
{
	struct bt_recovery_device *device;
	const struct bt_disk_super *copy;
	const struct bt_disk_super *newest;
	size_t i;
	size_t k;
	unsigned j;

	if (count != bt_u64(selected->devices)) {
		return count > bt_u64(selected->devices) ? BTRFS_INVALID_ARGUMENT
							 : BTRFS_UNSUPPORTED;
	}
	for (i = 0; i < count; i++) {
		device = &devices[i];
		newest = NULL;
		for (j = 0; j < BTRFS_SUPER_COPIES; j++) {
			copy = &device->copies[j];
			if (device->report[j].status != BTRFS_OK) {
				continue;
			}
			if (newest == NULL ||
			    device->report[j].generation > bt_u64(newest->generation) ||
			    (device->report[j].generation == generation && !device->current_item)) {
				newest = copy;
				device->current_item = device->report[j].generation == generation;
			}
		}
		if (newest == NULL ||
		    !bt_super_present(
			bt_u64(newest->device.total_bytes), (unsigned)(newest - device->copies))) {
			return BTRFS_CORRUPT;
		}
		for (j = 0; j < BTRFS_SUPER_COPIES; j++) {
			copy = &device->copies[j];
			if (device->report[j].status == BTRFS_OK &&
			    bt_super_present(bt_u64(newest->device.total_bytes), j) &&
			    (!bt_equal(copy->fsid, selected->fsid, sizeof(selected->fsid)) ||
				bt_u64(copy->device.id) != bt_u64(newest->device.id) ||
				!bt_equal(
				    copy->device.uuid, newest->device.uuid, BTRFS_UUID_SIZE))) {
				return BTRFS_CORRUPT;
			}
		}
		for (k = 0; k < i; k++) {
			if (bt_u64(devices[k].expected.device.id) == bt_u64(newest->device.id)) {
				return BTRFS_CORRUPT;
			}
		}
		device->expected = *selected;
		device->expected.device = newest->device;
		bt_super_seal(&device->expected, BT_SUPER_OFFSET);
	}
	return BTRFS_OK;
}

/* Which copies hold the selection, and how many do not. The selection fixes
 * each device's size and with it the set of copies Linux maintains; a copy
 * beyond that set belongs to no current state. A primary of another device
 * may name the selection's tree log, as Linux's fsync writes it into every
 * primary; no other copy may. */
static enum btrfs_result
bt_recovery_compare(struct bt_recovery_device *devices, size_t count,
    struct btrfs_recovery_report *report, unsigned *stale)
{
	struct bt_recovery_device *device;
	struct btrfs_super_copy *copy;
	const struct bt_disk_super *candidate;
	int logged = bt_u64(devices[report->selected_device].expected.log_root) != 0;
	size_t i;
	unsigned j;

	*stale = 0;
	for (i = 0; i < count; i++) {
		device = &devices[i];
		for (j = 0; j < BTRFS_SUPER_COPIES; j++) {
			copy = &device->report[j];
			candidate = &device->copies[j];
			if (!bt_super_present(bt_u64(device->expected.device.total_bytes), j)) {
				copy->status = BTRFS_NOT_FOUND;
				continue;
			}
			report->present++;
			/* An unverifiable checksum type may protect a valid copy; never
			 * overwrite it. */
			if (copy->status == BTRFS_UNSUPPORTED) {
				return BTRFS_UNSUPPORTED;
			}
			if (copy->status == BTRFS_OK && j != 0 &&
			    copy->generation == report->generation &&
			    bt_u64(candidate->log_root) != 0) {
				return BTRFS_UNSUPPORTED;
			}
			copy->current = copy->status == BTRFS_OK &&
			    ((i == report->selected_device && j == report->selected) ||
				(logged && j != 0
					? bt_super_same_but_log(candidate, &device->expected)
					: bt_super_same(candidate, &device->expected)));
			if (copy->status == BTRFS_OK && copy->generation == report->generation &&
			    !copy->current) {
				return BTRFS_CORRUPT;
			}
			/* A rewritten copy names no log: a primary must already
			 * name the selection's. */
			if (!copy->current && logged && j == 0) {
				return BTRFS_UNSUPPORTED;
			}
			*stale += !copy->current;
		}
	}
	return BTRFS_OK;
}

/* Writes every copy that does not hold the selection, from the highest
 * offset down on each device, then a barrier on every device. The selected
 * copy is never written, so a crash during recovery leaves the selected
 * generation recoverable again. */
static enum btrfs_result
bt_recovery_rewrite(const struct btrfs_write_environment *writer,
    struct bt_recovery_device *devices, size_t count, struct bt_disk_super *output,
    struct btrfs_recovery_report *report)
{
	struct btrfs_super_copy *copy;
	void *context;
	enum btrfs_result (*write)(void *context, uint64_t offset, const void *bytes, size_t size);
	enum btrfs_result (*flush)(void *context);
	size_t i;
	unsigned j;
	enum btrfs_result error = BTRFS_OK;

	for (i = 0; error == BTRFS_OK && i < count; i++) {
		context = i == 0 ? writer->context : writer->devices[i - 1].context;
		write = i == 0 ? writer->write : writer->devices[i - 1].write;
		for (j = BTRFS_SUPER_COPIES; error == BTRFS_OK && j-- != 0;) {
			copy = &devices[i].report[j];
			if (copy->status == BTRFS_NOT_FOUND || copy->current) {
				continue;
			}
			*output = devices[i].expected;
			bt_put64(&output->log_root, 0);
			output->log_level = 0;
			bt_super_seal(output, copy->offset);
			error = write(context, copy->offset, output, sizeof(*output));
			report->rewritten++;
		}
	}
	for (i = 0; error == BTRFS_OK && i < count; i++) {
		context = i == 0 ? writer->context : writer->devices[i - 1].context;
		flush = i == 0 ? writer->flush : writer->devices[i - 1].flush;
		error = flush(context);
	}
	for (i = 0; error == BTRFS_OK && i < count; i++) {
		for (j = 0; j < BTRFS_SUPER_COPIES; j++) {
			devices[i].report[j].current =
			    devices[i].report[j].status != BTRFS_NOT_FOUND;
		}
	}
	return error;
}

enum btrfs_result
btrfs_recover_supers(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t acknowledged,
    struct btrfs_recovery_report *report)
{
	struct bt_recovery_device *devices;
	struct bt_recovery_device *device;
	struct bt_disk_super *output;
	size_t count;
	size_t size;
	size_t i;
	unsigned stale = 0;
	enum btrfs_result error = BTRFS_OK;

	if (report == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	bt_zero(report, sizeof(*report));
	report->selected = BTRFS_SUPER_COPIES;
	if (environment == NULL || environment->read == NULL || environment->allocate == NULL ||
	    environment->release == NULL || environment->device_count > BTRFS_MAX_DEVICES - 1 ||
	    (environment->device_count != 0 && environment->devices == NULL) ||
	    (writer != NULL &&
		(writer->write == NULL || writer->flush == NULL ||
		    writer->device_count != environment->device_count ||
		    (writer->device_count != 0 && writer->devices == NULL)))) {
		return BTRFS_INVALID_ARGUMENT;
	}
	for (i = 0; i < environment->device_count; i++) {
		if (environment->devices[i].read == NULL ||
		    (writer != NULL &&
			(writer->devices[i].write == NULL || writer->devices[i].flush == NULL))) {
			return BTRFS_INVALID_ARGUMENT;
		}
	}
	if (environment->size_bytes < BT_SUPER_OFFSET + BT_SUPER_SIZE) {
		return BTRFS_NOT_BTRFS;
	}
	count = environment->device_count + 1;
	size = count * sizeof(*devices) + sizeof(*output);
	devices = environment->allocate(environment->context, size);
	if (devices == NULL) {
		return BTRFS_NO_MEMORY;
	}
	bt_zero(devices, size);
	output = (struct bt_disk_super *)(devices + count);
	for (i = 0; error == BTRFS_OK && i < count; i++) {
		device = &devices[i];
		device->context =
		    i == 0 ? environment->context : environment->devices[i - 1].context;
		device->read = i == 0 ? environment->read : environment->devices[i - 1].read;
		device->size =
		    i == 0 ? environment->size_bytes : environment->devices[i - 1].size_bytes;
		device->report = i == 0 ? report->copies : report->device_copies[i - 1];
		error = bt_recovery_read_copies(device);
	}
	if (error == BTRFS_OK) {
		error = bt_recovery_select(devices, count, report);
	}
	if (error == BTRFS_OK) {
		error = bt_recovery_identify(devices, count,
		    &devices[report->selected_device].copies[report->selected], report->generation);
	}
	if (error == BTRFS_OK) {
		error = bt_recovery_compare(devices, count, report, &stale);
	}
	if (error == BTRFS_OK && report->generation < acknowledged) {
		error = BTRFS_STALE;
	}
	if (error == BTRFS_OK && stale != 0) {
		error = bt_recovery_validate(environment, devices, count);
		if (error == BTRFS_OK && writer == NULL) {
			error = BTRFS_RECOVERY_REQUIRED;
		}
	}
	if (error == BTRFS_OK && stale != 0) {
		error = bt_recovery_rewrite(writer, devices, count, output, report);
	}
	environment->release(environment->context, devices, size);
	return error;
}
