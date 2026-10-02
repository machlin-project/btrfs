/* SPDX-License-Identifier: BSD-3-Clause */
#include "encode.h"
#include "space.h"
#include <btrfs/write.h>

/* Opens the selected copy as a complete committed state: chunk tree, root tree,
 * checksum and top-level roots (through mount), then the device tree root and the
 * extent tree with its block-group accounting. File trees are not scrubbed. */
static enum btrfs_result
bt_recovery_validate(
    const struct btrfs_environment *environment, const struct bt_disk_super *super, uint64_t offset)
{
	struct btrfs_environment uncached = *environment;
	struct btrfs_fs *fs;
	struct bt_space *space = NULL;
	struct bt_root devices = { 0 };
	struct bt_root extents = { 0 };
	uint8_t *node;
	enum btrfs_result error;

	/* A candidate may still be refused; its nodes never enter the cache. */
	uncached.cache = NULL;
	error = bt_mount_super(&uncached, super, offset, BTRFS_TOP_LEVEL_TREE, &fs);
	if (error != BTRFS_OK) {
		return error;
	}
	error = bt_find_root(fs, BT_DEV_TREE, &devices);
	if (error == BTRFS_OK) {
		error = bt_find_root(fs, BT_EXTENT_TREE, &extents);
	}
	if (error == BTRFS_NOT_FOUND) {
		error = BTRFS_CORRUPT;
	}
	if (error == BTRFS_OK) {
		node = environment->allocate(environment->context, fs->info.node_size);
		error = node == NULL ? BTRFS_NO_MEMORY : bt_tree_read(fs, devices, node);
		if (node != NULL) {
			environment->release(environment->context, node, fs->info.node_size);
		}
	}
	if (error == BTRFS_OK) {
		error = bt_space_create(fs, extents, NULL, 1, &space);
	}
	bt_space_destroy(space);
	btrfs_unmount(fs);
	return error;
}

static enum btrfs_result
bt_recovery_select(const struct bt_disk_super *copies, struct btrfs_recovery_report *report)
{
	const struct bt_disk_super *selected;
	struct btrfs_super_copy *copy;
	unsigned best = BTRFS_SUPER_COPIES;
	unsigned i;

	for (i = 0; i < BTRFS_SUPER_COPIES; i++) {
		copy = &report->copies[i];
		if (copy->status == BTRFS_OK &&
		    (best == BTRFS_SUPER_COPIES ||
			copy->generation > report->copies[best].generation)) {
			best = i;
		}
	}
	if (best == BTRFS_SUPER_COPIES) {
		for (i = 0; i < BTRFS_SUPER_COPIES; i++) {
			if (report->copies[i].status == BTRFS_UNSUPPORTED) {
				return BTRFS_UNSUPPORTED;
			}
		}
		return BTRFS_CORRUPT;
	}
	selected = &copies[best];
	report->selected = best;
	report->generation = report->copies[best].generation;
	/* The selection fixes the device size and with it the set of copies Linux
	 * maintains. A copy beyond that set belongs to no current state. */
	for (i = 0; i < BTRFS_SUPER_COPIES; i++) {
		copy = &report->copies[i];
		if (!bt_super_present(bt_u64(selected->device.total_bytes), i)) {
			copy->status = BTRFS_NOT_FOUND;
			continue;
		}
		report->present++;
		/* An unverifiable checksum type may protect a valid copy; never overwrite it. */
		if (copy->status == BTRFS_UNSUPPORTED) {
			return BTRFS_UNSUPPORTED;
		}
		if (copy->status != BTRFS_OK) {
			continue;
		}
		/* Copies of another filesystem or a pending tree log are never resolved by
		 * choosing one: replay is unsupported and a foreign copy is ambiguous. */
		if (!bt_equal(copies[i].fsid, selected->fsid, sizeof(selected->fsid))) {
			return BTRFS_CORRUPT;
		}
		if (copy->generation == report->generation && bt_u64(copies[i].log_root) != 0) {
			return BTRFS_UNSUPPORTED;
		}
		copy->current = bt_super_same(&copies[i], selected);
		if (copy->generation == report->generation && !copy->current) {
			return BTRFS_CORRUPT;
		}
	}
	return report->copies[best].status == BTRFS_OK ? BTRFS_OK : BTRFS_CORRUPT;
}

enum btrfs_result
btrfs_recover_supers(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, uint64_t acknowledged,
    struct btrfs_recovery_report *report)
{
	struct bt_disk_super *copies;
	struct bt_disk_super *output;
	struct btrfs_super_copy *copy;
	size_t size = (BTRFS_SUPER_COPIES + 1) * sizeof(*copies);
	unsigned stale = 0;
	unsigned i;
	enum btrfs_result error = BTRFS_OK;

	if (report == NULL) {
		return BTRFS_INVALID_ARGUMENT;
	}
	bt_zero(report, sizeof(*report));
	report->selected = BTRFS_SUPER_COPIES;
	if (environment == NULL || environment->read == NULL || environment->allocate == NULL ||
	    environment->release == NULL ||
	    (writer != NULL && (writer->write == NULL || writer->flush == NULL))) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (environment->size_bytes < BT_SUPER_OFFSET + BT_SUPER_SIZE) {
		return BTRFS_NOT_BTRFS;
	}
	copies = environment->allocate(environment->context, size);
	if (copies == NULL) {
		return BTRFS_NO_MEMORY;
	}
	output = &copies[BTRFS_SUPER_COPIES];
	/* Every copy inside the resource is read before deciding. An unreadable copy
	 * could hold the newest state, so an I/O failure stops the decision. */
	for (i = 0; i < BTRFS_SUPER_COPIES && error == BTRFS_OK; i++) {
		copy = &report->copies[i];
		copy->offset = bt_super_offset(i);
		copy->status = BTRFS_NOT_FOUND;
		if (!bt_super_present(environment->size_bytes, i)) {
			continue;
		}
		error = environment->read(
		    environment->context, copy->offset, &copies[i], sizeof(copies[i]));
		if (error == BTRFS_OK) {
			copy->status = bt_super_check(&copies[i], copy->offset);
			copy->generation =
			    copy->status == BTRFS_OK ? bt_u64(copies[i].generation) : 0;
		}
	}
	if (error == BTRFS_OK) {
		error = bt_recovery_select(copies, report);
	}
	if (error == BTRFS_OK && report->generation < acknowledged) {
		error = BTRFS_STALE;
	}
	for (i = 0; i < BTRFS_SUPER_COPIES; i++) {
		if (report->copies[i].status != BTRFS_NOT_FOUND && !report->copies[i].current) {
			stale++;
		}
	}
	if (error == BTRFS_OK && stale != 0) {
		error = bt_recovery_validate(environment, &copies[report->selected],
		    report->copies[report->selected].offset);
		if (error == BTRFS_OK && writer == NULL) {
			error = BTRFS_RECOVERY_REQUIRED;
		}
	}
	/* Secondary copies first, primary last. The selected copy is never written, so
	 * a crash during recovery leaves the selected generation recoverable again. */
	for (i = BTRFS_SUPER_COPIES; error == BTRFS_OK && stale != 0 && i-- != 0;) {
		copy = &report->copies[i];
		if (copy->status == BTRFS_NOT_FOUND || copy->current) {
			continue;
		}
		*output = copies[report->selected];
		bt_super_seal(output, copy->offset);
		error = writer->write(writer->context, copy->offset, output, sizeof(*output));
		report->rewritten++;
	}
	if (error == BTRFS_OK && stale != 0) {
		error = writer->flush(writer->context);
		for (i = 0; error == BTRFS_OK && i < BTRFS_SUPER_COPIES; i++) {
			report->copies[i].current = report->copies[i].status != BTRFS_NOT_FOUND;
		}
	}
	environment->release(environment->context, copies, size);
	return error;
}
