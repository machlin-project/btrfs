/* SPDX-License-Identifier: BSD-3-Clause */
#include <btrfs/volume.h>

struct btrfs_volume_view {
	struct btrfs_fs *fs;
	uint64_t generation;
	uint32_t pins;
	struct btrfs_volume_view *older;
};

struct btrfs_volume {
	struct btrfs_environment environment;
	struct btrfs_write_environment device;
	/* Counts what the open transaction issued, to tell an unchanged medium
	 * from an uncertain one when a commit fails. */
	struct btrfs_write_environment counted;
	struct btrfs_volume_locks locks;
	/* Inode numbers and directory indexes stay unique for the mount. */
	struct btrfs_counters *counters;
	/* The last committed allocator state, so a begin need not reload it. */
	struct btrfs_allocation_map *map;
	uint64_t tree;
	struct btrfs_volume_view *current;
	struct btrfs_transaction *open;
	uint64_t issued;
	enum btrfs_result failure;
	int writable;
	int writer;
};

static enum btrfs_result
volume_write(void *context, uint64_t offset, const void *bytes, size_t size)
{
	struct btrfs_volume *volume = context;

	volume->issued++;
	return volume->device.write(volume->device.context, offset, bytes, size);
}

static enum btrfs_result
volume_flush(void *context)
{
	struct btrfs_volume *volume = context;

	volume->issued++;
	return volume->device.flush(volume->device.context);
}

static enum btrfs_result
volume_view(struct btrfs_volume *volume, struct btrfs_volume_view **result)
{
	struct btrfs_volume_view *view;
	struct btrfs_info info;
	enum btrfs_result error;

	view = volume->environment.allocate(volume->environment.context, sizeof(*view));
	if (view == NULL) {
		return BTRFS_NO_MEMORY;
	}
	view->pins = 0;
	view->older = NULL;
	error = btrfs_mount(&volume->environment, volume->tree, &view->fs);
	if (error != BTRFS_OK) {
		volume->environment.release(volume->environment.context, view, sizeof(*view));
		return error;
	}
	btrfs_get_info(view->fs, &info);
	view->generation = info.generation;
	*result = view;
	return BTRFS_OK;
}

static void
volume_release_view(struct btrfs_volume *volume, struct btrfs_volume_view *view)
{
	btrfs_unmount(view->fs);
	volume->environment.release(volume->environment.context, view, sizeof(*view));
}

/* Unlinks unpinned views older than the current one; called with the lock
 * held, returning the list to release after unlocking. */
static struct btrfs_volume_view *
volume_retire(struct btrfs_volume *volume)
{
	struct btrfs_volume_view **link = &volume->current->older;
	struct btrfs_volume_view *retired = NULL;
	struct btrfs_volume_view *view;

	while (*link != NULL) {
		view = *link;
		if (view->pins == 0) {
			*link = view->older;
			view->older = retired;
			retired = view;
		} else {
			link = &view->older;
		}
	}
	return retired;
}

static void
volume_release_list(struct btrfs_volume *volume, struct btrfs_volume_view *list)
{
	struct btrfs_volume_view *next;

	while (list != NULL) {
		next = list->older;
		volume_release_view(volume, list);
		list = next;
	}
}

enum btrfs_result
btrfs_volume_open(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, const struct btrfs_volume_locks *locks,
    uint64_t tree, struct btrfs_volume **result)
{
	struct btrfs_volume *volume;
	struct btrfs_transaction *transaction = NULL;
	enum btrfs_result error;

	if (environment == NULL || locks == NULL || result == NULL ||
	    (writer != NULL && (writer->write == NULL || writer->flush == NULL))) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*result = NULL;
	volume = environment->allocate(environment->context, sizeof(*volume));
	if (volume == NULL) {
		return BTRFS_NO_MEMORY;
	}
	volume->environment = *environment;
	volume->locks = *locks;
	volume->tree = tree;
	volume->current = NULL;
	volume->counters = NULL;
	volume->map = NULL;
	volume->open = NULL;
	volume->issued = 0;
	volume->failure = BTRFS_OK;
	volume->writable = writer != NULL;
	volume->writer = 0;
	if (writer != NULL) {
		volume->device = *writer;
		volume->counted.context = volume;
		volume->counted.write = volume_write;
		volume->counted.flush = volume_flush;
	}
	error = volume_view(volume, &volume->current);
	if (error == BTRFS_OK && volume->writable) {
		error = btrfs_allocation_map_create(environment, &volume->map);
	}
	/* The admission transaction also verifies and keeps the allocator state. */
	if (error == BTRFS_OK && volume->writable) {
		error = btrfs_transaction_begin_mapped(
		    volume->current->fs, &volume->counted, volume->map, &transaction);
		btrfs_transaction_destroy(transaction);
	}
	if (error == BTRFS_OK && volume->writable) {
		error = btrfs_counters_create(environment, &volume->counters);
	}
	if (error != BTRFS_OK) {
		if (volume->current != NULL) {
			volume_release_view(volume, volume->current);
		}
		btrfs_allocation_map_destroy(volume->map);
		environment->release(environment->context, volume, sizeof(*volume));
		return error;
	}
	*result = volume;
	return BTRFS_OK;
}

void
btrfs_volume_close(struct btrfs_volume *volume)
{
	if (volume == NULL) {
		return;
	}
	volume_release_list(volume, volume->current);
	btrfs_counters_destroy(volume->counters);
	btrfs_allocation_map_destroy(volume->map);
	volume->environment.release(volume->environment.context, volume, sizeof(*volume));
}

void
btrfs_volume_allocation_counts(const struct btrfs_volume *volume, uint64_t *scans, uint64_t *reuses)
{
	*scans = 0;
	*reuses = 0;
	if (volume->map != NULL) {
		btrfs_allocation_map_counts(volume->map, scans, reuses);
	}
}

int
btrfs_volume_writable(const struct btrfs_volume *volume)
{
	return volume->writable;
}

uint64_t
btrfs_volume_generation(struct btrfs_volume *volume)
{
	uint64_t generation;

	volume->locks.lock(volume->locks.context);
	generation = volume->current->generation;
	volume->locks.unlock(volume->locks.context);
	return generation;
}

enum btrfs_result
btrfs_volume_failure(struct btrfs_volume *volume)
{
	enum btrfs_result failure;

	volume->locks.lock(volume->locks.context);
	failure = volume->failure;
	volume->locks.unlock(volume->locks.context);
	return failure;
}

const struct btrfs_fs *
btrfs_volume_pin(struct btrfs_volume *volume, struct btrfs_volume_view **view)
{
	volume->locks.lock(volume->locks.context);
	*view = volume->current;
	(*view)->pins++;
	volume->locks.unlock(volume->locks.context);
	return (*view)->fs;
}

void
btrfs_volume_unpin(struct btrfs_volume *volume, struct btrfs_volume_view *view)
{
	struct btrfs_volume_view *retired = NULL;

	volume->locks.lock(volume->locks.context);
	view->pins--;
	if (view != volume->current && view->pins == 0) {
		retired = volume_retire(volume);
		volume->locks.wake(volume->locks.context, &volume->current);
	}
	volume->locks.unlock(volume->locks.context);
	volume_release_list(volume, retired);
}

enum btrfs_result
btrfs_volume_begin(struct btrfs_volume *volume, struct btrfs_transaction **transaction)
{
	enum btrfs_result error;

	*transaction = NULL;
	if (!volume->writable) {
		return BTRFS_READ_ONLY;
	}
	volume->locks.lock(volume->locks.context);
	while (volume->writer) {
		volume->locks.wait(volume->locks.context, &volume->writer);
	}
	volume->writer = 1;
	/* Blocks freed by the current view's commit may be reused now. */
	while (volume->current->older != NULL && volume->failure == BTRFS_OK) {
		volume->locks.wait(volume->locks.context, &volume->current);
	}
	error = volume->failure;
	volume->locks.unlock(volume->locks.context);
	if (error == BTRFS_OK) {
		volume->issued = 0;
		error = btrfs_transaction_begin_mapped(
		    volume->current->fs, &volume->counted, volume->map, transaction);
	}
	if (error == BTRFS_OK) {
		error = btrfs_transaction_use_counters(*transaction, volume->counters);
		if (error != BTRFS_OK) {
			btrfs_transaction_destroy(*transaction);
			*transaction = NULL;
		}
	}
	if (error != BTRFS_OK) {
		volume->locks.lock(volume->locks.context);
		volume->writer = 0;
		volume->locks.wake(volume->locks.context, &volume->writer);
		volume->locks.unlock(volume->locks.context);
		return error;
	}
	volume->open = *transaction;
	return BTRFS_OK;
}

static void
volume_end(struct btrfs_volume *volume, struct btrfs_volume_view *view, enum btrfs_result failure)
{
	struct btrfs_volume_view *retired = NULL;

	volume->locks.lock(volume->locks.context);
	if (view != NULL) {
		view->older = volume->current;
		volume->current = view;
		retired = volume_retire(volume);
	}
	if (failure != BTRFS_OK && volume->failure == BTRFS_OK) {
		volume->failure = failure;
	}
	volume->open = NULL;
	volume->writer = 0;
	volume->locks.wake(volume->locks.context, &volume->writer);
	volume->locks.wake(volume->locks.context, &volume->current);
	volume->locks.unlock(volume->locks.context);
	volume_release_list(volume, retired);
}

enum btrfs_result
btrfs_volume_commit(struct btrfs_volume *volume, struct btrfs_transaction *transaction)
{
	struct btrfs_volume_view *view = NULL;
	enum btrfs_result error;
	enum btrfs_result failure = BTRFS_OK;

	if (transaction == NULL || transaction != volume->open) {
		return BTRFS_INVALID_ARGUMENT;
	}
	/* Data written while the transaction ran went to space no committed root
	 * references; only the commit's own writes can make the medium
	 * uncertain. */
	volume->issued = 0;
	error = btrfs_transaction_commit(transaction);
	btrfs_transaction_destroy(transaction);
	if (error == BTRFS_OK && volume->issued != 0) {
		error = volume_view(volume, &view);
		failure = error;
	} else if (error != BTRFS_OK && volume->issued != 0) {
		/* Some of the commit may be durable: the medium needs explicit
		 * recovery before another transaction. */
		failure = error;
	}
	volume_end(volume, view, failure);
	return error;
}

void
btrfs_volume_abort(struct btrfs_volume *volume, struct btrfs_transaction *transaction)
{
	if (transaction == NULL || transaction != volume->open) {
		return;
	}
	btrfs_transaction_destroy(transaction);
	volume_end(volume, NULL, BTRFS_OK);
}
