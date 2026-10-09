/* SPDX-License-Identifier: BSD-3-Clause */
#include <btrfs/volume.h>

/* Pinning the current view takes no lock: a pinner marks itself in one of
 * these counters, chosen by its stack address, while it reads the current
 * view and raises the view's pins. Freeing views waits for every counter to
 * drain after publishing a new current view, so no pinner still holds an older
 * one unpinned. */
#define VOLUME_STRIPES 16U
#define VOLUME_LINE 64U
/* Inodes one operation can leave to eviction steps: a transaction keeps room
 * for at most this many (btrfs_transaction_room). */
#define VOLUME_DEFERRED 32U

struct volume_active {
	uint32_t count;
	uint8_t padding[VOLUME_LINE - sizeof(uint32_t)];
};

#ifdef BTRFS_VOLUME_TEST_HOOKS
/* Tests widen the window between reading the current view and pinning it. */
void (*btrfs_volume_test_pin_hook)(void);
#endif

struct btrfs_volume_view {
	struct btrfs_fs *fs;
	uint64_t generation;
	uint32_t pins;
	struct btrfs_volume_view *older;
};

struct btrfs_volume {
	struct volume_active active[VOLUME_STRIPES];
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
	/* Grouped operations: the running transaction, its view after the last
	 * operation, the operations it holds and the readers of that view. */
	struct btrfs_transaction *running;
	const struct btrfs_fs *running_view;
	/* Set while the detached running transaction commits, until its view is
	 * published (atomic; written with the lock held). */
	int committing;
	size_t operations;
	uint32_t readers;
	/* Threads waiting for the writer turn; running-view readers yield to them,
	 * except that the readers waiting when a turn ends (admitting) enter
	 * before the next writer, so neither side starves. */
	uint32_t waiting;
	uint32_t read_waiting;
	uint32_t admitting;
	uint64_t issued;
	enum btrfs_result failure;
	int writable;
	/* The writer turn: a begin's transaction, an operation, a sync. */
	int writer;
	/* A committed transaction released space: the next one removes the block
	 * groups that became empty, as Linux's cleaner does. */
	int released;
	/* The running transaction released space. */
	int releasing;
	/* Deleted subvolumes or a quota rescan may wait for maintenance. */
	int maintenance;
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
volume_compress(void *context, enum btrfs_compression codec, const void *input, size_t input_size,
    void *output, size_t capacity, size_t *size)
{
	struct btrfs_volume *volume = context;

	return volume->device.compress(
	    volume->device.context, codec, input, input_size, output, capacity, size);
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
	__atomic_store_n(&view->pins, 0, __ATOMIC_RELAXED);
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

/* Reserve the version wrapper before the commit can issue writes. The core
 * prepares its owned reader state before publication and returns it only
 * after the last barrier; a successful commit needs no remount or allocation. */
static enum btrfs_result
volume_commit_view(struct btrfs_volume *volume, struct btrfs_transaction *transaction,
    struct btrfs_volume_view **result)
{
	struct btrfs_volume_view *view;
	struct btrfs_info info;
	enum btrfs_result error;

	*result = NULL;
	view = volume->environment.allocate(volume->environment.context, sizeof(*view));
	if (view == NULL) {
		return BTRFS_NO_MEMORY;
	}
	__atomic_store_n(&view->pins, 0, __ATOMIC_RELAXED);
	view->older = NULL;
	error = btrfs_transaction_commit_view(transaction, volume->tree, &view->fs);
	if (error != BTRFS_OK || view->fs == NULL) {
		volume->environment.release(volume->environment.context, view, sizeof(*view));
		return error;
	}
	btrfs_get_info(view->fs, &info);
	view->generation = info.generation;
	*result = view;
	return BTRFS_OK;
}

/* Unlinks unpinned views older than the current one; called with the lock
 * held, returning the list to release after unlocking. */
static struct btrfs_volume_view *
volume_retire(struct btrfs_volume *volume)
{
	struct btrfs_volume_view **link = &volume->current->older;
	struct btrfs_volume_view *retired = NULL;
	struct btrfs_volume_view *view;
	unsigned stripe;

	/* A pinner that read an older current view has raised its pins once its
	 * counter drains; one that starts later reads the current view. */
	for (stripe = 0; stripe < VOLUME_STRIPES; stripe++) {
		while (__atomic_load_n(&volume->active[stripe].count, __ATOMIC_SEQ_CST) != 0) {
		}
	}
	while (*link != NULL) {
		view = *link;
		if (__atomic_load_n(&view->pins, __ATOMIC_ACQUIRE) == 0) {
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
	unsigned stripe;
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
	for (stripe = 0; stripe < VOLUME_STRIPES; stripe++) {
		volume->active[stripe].count = 0;
	}
	volume->environment = *environment;
	volume->locks = *locks;
	volume->tree = tree;
	volume->current = NULL;
	volume->counters = NULL;
	volume->map = NULL;
	volume->open = NULL;
	__atomic_store_n(&volume->running, NULL, __ATOMIC_SEQ_CST);
	volume->running_view = NULL;
	volume->committing = 0;
	volume->operations = 0;
	volume->readers = 0;
	volume->waiting = 0;
	volume->read_waiting = 0;
	volume->admitting = 0;
	volume->issued = 0;
	volume->failure = BTRFS_OK;
	volume->writable = writer != NULL;
	volume->maintenance = volume->writable;
	volume->writer = 0;
	if (writer != NULL) {
		volume->device = *writer;
		volume->counted.context = volume;
		volume->counted.write = volume_write;
		volume->counted.flush = volume_flush;
		volume->counted.compress = writer->compress != NULL ? volume_compress : NULL;
		volume->counted.compression = writer->compression;
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
	/* Changes no sync committed are discarded. */
	btrfs_transaction_destroy(volume->running);
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

static struct volume_active *
volume_stripe(struct btrfs_volume *volume, const void *hint)
{
	uint64_t page = (uint64_t)(uintptr_t)hint >> 12;

	return &volume->active[(page * UINT64_C(0x9E3779B97F4A7C15)) >> 60 & (VOLUME_STRIPES - 1)];
}

/* Pins the current view without the lock; NULL when unpinned is required
 * (a running transaction's readers take the lock path). */
static struct btrfs_volume_view *
volume_pin_current(struct btrfs_volume *volume, int committed_only, const void *hint)
{
	struct volume_active *active = volume_stripe(volume, hint);
	struct btrfs_volume_view *view = NULL;

	__atomic_fetch_add(&active->count, 1, __ATOMIC_SEQ_CST);
	/* A commit stores committing before it detaches the running transaction
	 * and clears it after publishing current. */
	if (committed_only ||
	    (__atomic_load_n(&volume->running, __ATOMIC_SEQ_CST) == NULL &&
		__atomic_load_n(&volume->committing, __ATOMIC_SEQ_CST) == 0)) {
		view = __atomic_load_n(&volume->current, __ATOMIC_SEQ_CST);
#ifdef BTRFS_VOLUME_TEST_HOOKS
		if (btrfs_volume_test_pin_hook != NULL) {
			btrfs_volume_test_pin_hook();
		}
#endif
		__atomic_fetch_add(&view->pins, 1, __ATOMIC_RELAXED);
	}
	__atomic_fetch_sub(&active->count, 1, __ATOMIC_RELEASE);
	return view;
}

const struct btrfs_fs *
btrfs_volume_pin(struct btrfs_volume *volume, struct btrfs_volume_view **view)
{
	*view = volume_pin_current(volume, 1, view);
	return (*view)->fs;
}

void
btrfs_volume_unpin(struct btrfs_volume *volume, struct btrfs_volume_view *view)
{
	struct btrfs_volume_view *retired;

	/* The view may be freed by others once its pins reach zero. */
	if (__atomic_sub_fetch(&view->pins, 1, __ATOMIC_ACQ_REL) != 0 ||
	    view == __atomic_load_n(&volume->current, __ATOMIC_ACQUIRE)) {
		return;
	}
	volume->locks.lock(volume->locks.context);
	retired = volume_retire(volume);
	volume->locks.wake(volume->locks.context, &volume->current);
	volume->locks.unlock(volume->locks.context);
	volume_release_list(volume, retired);
}

/* Takes the writer turn, with the lock held; readers of the running view
 * finish first, and new ones wait while a writer does. */
static void
volume_take_turn(struct btrfs_volume *volume)
{
	volume->waiting++;
	while (volume->writer || volume->readers != 0 || volume->admitting != 0) {
		volume->locks.wait(volume->locks.context, &volume->writer);
	}
	volume->waiting--;
	volume->writer = 1;
}

/* With the lock held. */
static void
volume_give_turn(struct btrfs_volume *volume)
{
	volume->writer = 0;
	if (volume->running != NULL) {
		volume->admitting = volume->read_waiting;
	}
	volume->locks.wake(volume->locks.context, &volume->writer);
	volume->locks.wake(volume->locks.context, &volume->readers);
}

/* Begins a transaction on the current view, with the turn held. */
static enum btrfs_result
volume_start(struct btrfs_volume *volume, struct btrfs_transaction **transaction)
{
	size_t removed;
	enum btrfs_result error;

	*transaction = NULL;
	volume->locks.lock(volume->locks.context);
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
	}
	if (error == BTRFS_OK && volume->released) {
		error = btrfs_transaction_remove_unused_groups(*transaction, &removed);
		volume->released = 0;
	}
	if (error != BTRFS_OK && *transaction != NULL) {
		btrfs_transaction_destroy(*transaction);
		*transaction = NULL;
	}
	return error;
}

/* Publishes view (when not NULL) and records failure, with the lock held;
 * returns the views to release after unlocking. */
static struct btrfs_volume_view *
volume_publish(
    struct btrfs_volume *volume, struct btrfs_volume_view *view, enum btrfs_result failure)
{
	struct btrfs_volume_view *retired = NULL;

	if (view != NULL) {
		view->older = volume->current;
		__atomic_store_n(&volume->current, view, __ATOMIC_SEQ_CST);
		retired = volume_retire(volume);
	}
	if (failure != BTRFS_OK && volume->failure == BTRFS_OK) {
		volume->failure = failure;
	}
	/* Begins wait for older views to drain or for a failure. */
	volume->locks.wake(volume->locks.context, &volume->current);
	return retired;
}

/* Commits the running transaction, with the turn held. Its operations were
 * acknowledged, so any failure fails the volume. */
static enum btrfs_result
volume_commit_running(struct btrfs_volume *volume)
{
	struct btrfs_transaction *transaction = volume->running;
	struct btrfs_volume_view *view = NULL;
	struct btrfs_volume_view *retired;
	enum btrfs_result error;

	if (transaction == NULL) {
		return BTRFS_OK;
	}
	/* Until the commit publishes its view, readers wait for this turn: the
	 * committed view lacks the transaction's acknowledged operations, and
	 * nothing reads the transaction while it commits. */
	volume->locks.lock(volume->locks.context);
	__atomic_store_n(&volume->committing, 1, __ATOMIC_SEQ_CST);
	__atomic_store_n(&volume->running, NULL, __ATOMIC_SEQ_CST);
	volume->running_view = NULL;
	volume->operations = 0;
	volume->locks.unlock(volume->locks.context);
	volume->issued = 0;
	error = volume_commit_view(volume, transaction, &view);
	/* Before publication: the transaction refers to the view it began on. */
	btrfs_transaction_destroy(transaction);
	if (error == BTRFS_OK && volume->releasing) {
		volume->released = 1;
	}
	volume->releasing = 0;
	volume->locks.lock(volume->locks.context);
	retired = volume_publish(volume, view, error);
	__atomic_store_n(&volume->committing, 0, __ATOMIC_SEQ_CST);
	volume->locks.unlock(volume->locks.context);
	volume_release_list(volume, retired);
	return error;
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
	volume_take_turn(volume);
	volume->locks.unlock(volume->locks.context);
	error = volume_commit_running(volume);
	if (error == BTRFS_OK) {
		error = volume_start(volume, transaction);
	}
	if (error != BTRFS_OK) {
		volume->locks.lock(volume->locks.context);
		volume_give_turn(volume);
		volume->locks.unlock(volume->locks.context);
		return error;
	}
	volume->open = *transaction;
	return BTRFS_OK;
}

static void
volume_end(struct btrfs_volume *volume, struct btrfs_volume_view *view, enum btrfs_result failure)
{
	struct btrfs_volume_view *retired;

	volume->locks.lock(volume->locks.context);
	retired = volume_publish(volume, view, failure);
	volume->open = NULL;
	volume_give_turn(volume);
	volume->locks.unlock(volume->locks.context);
	volume_release_list(volume, retired);
}

enum btrfs_result
btrfs_volume_commit(struct btrfs_volume *volume, struct btrfs_transaction *transaction)
{
	struct btrfs_volume_view *view = NULL;
	struct btrfs_object_id deferred[VOLUME_DEFERRED];
	size_t count = 0;
	size_t i;
	enum btrfs_result error;
	enum btrfs_result failure = BTRFS_OK;

	if (transaction == NULL || transaction != volume->open) {
		return BTRFS_INVALID_ARGUMENT;
	}
	while (count < VOLUME_DEFERRED &&
	    btrfs_transaction_take_deferred(transaction, &deferred[count]) == BTRFS_OK) {
		count++;
	}
	/* Data written while the transaction ran went to space no committed root
	 * references; only the commit's own writes can make the medium
	 * uncertain. */
	volume->issued = 0;
	error = volume_commit_view(volume, transaction, &view);
	btrfs_transaction_destroy(transaction);
	if (error != BTRFS_OK && volume->issued != 0) {
		/* Some of the commit may be durable: the medium needs explicit
		 * recovery before another transaction. */
		failure = error;
	}
	volume_end(volume, view, failure);
	/* Inodes the transaction left to eviction steps are deleted now and
	 * committed before returning; one without room keeps its orphan. */
	for (i = 0; error == BTRFS_OK && i < count; i++) {
		if (btrfs_volume_evict(volume, deferred[i]) != BTRFS_OK) {
			break;
		}
	}
	if (error == BTRFS_OK && count != 0) {
		(void)btrfs_volume_sync(volume, btrfs_volume_pending(volume));
	}
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

/* Makes the running transaction able to take one more operation of nodes,
 * with the turn held: a running transaction without room commits first, and
 * a new one begins when none runs. */
static enum btrfs_result
volume_room(struct btrfs_volume *volume, size_t nodes, int releasing)
{
	struct btrfs_transaction *running = NULL;
	enum btrfs_result error = BTRFS_OK;

	if (volume->running != NULL &&
	    (releasing ? btrfs_transaction_room_releasing(volume->running, nodes)
		       : btrfs_transaction_room(volume->running, nodes)) != BTRFS_OK) {
		error = volume_commit_running(volume);
	}
	if (error == BTRFS_OK && volume->running == NULL) {
		error = volume_start(volume, &running);
		if (error == BTRFS_OK) {
			error = releasing ? btrfs_transaction_room_releasing(running, nodes)
					  : btrfs_transaction_room(running, nodes);
		}
		if (error != BTRFS_OK) {
			btrfs_transaction_destroy(running);
		} else {
			volume->locks.lock(volume->locks.context);
			__atomic_store_n(&volume->running, running, __ATOMIC_SEQ_CST);
			volume->locks.unlock(volume->locks.context);
		}
	}
	if (error == BTRFS_OK && releasing) {
		volume->releasing = 1;
	}
	return error;
}

static enum btrfs_result
volume_join(struct btrfs_volume *volume, size_t nodes, int releasing,
    struct btrfs_transaction **transaction)
{
	enum btrfs_result error;

	*transaction = NULL;
	if (!volume->writable) {
		return BTRFS_READ_ONLY;
	}
	volume->locks.lock(volume->locks.context);
	volume_take_turn(volume);
	error = volume->failure;
	volume->locks.unlock(volume->locks.context);
	if (error == BTRFS_OK) {
		error = volume_room(volume, nodes, releasing);
	}
	if (error != BTRFS_OK) {
		volume->locks.lock(volume->locks.context);
		volume_give_turn(volume);
		volume->locks.unlock(volume->locks.context);
		return error;
	}
	*transaction = volume->running;
	return BTRFS_OK;
}

enum btrfs_result
btrfs_volume_join(struct btrfs_volume *volume, size_t nodes, struct btrfs_transaction **transaction)
{
	return volume_join(volume, nodes, 0, transaction);
}

enum btrfs_result
btrfs_volume_join_releasing(
    struct btrfs_volume *volume, size_t nodes, struct btrfs_transaction **transaction)
{
	return volume_join(volume, nodes, 1, transaction);
}

/* Evicts an orphan in releasing steps, with the turn held; the running
 * transaction commits whenever it lacks room for the next step. */
static enum btrfs_result
volume_evict(struct btrfs_volume *volume, struct btrfs_object_id id)
{
	enum btrfs_result error = BTRFS_OK;
	int done = 0;

	while (error == BTRFS_OK && !done) {
		error = volume_room(volume, BTRFS_RELEASE_STEP_NODES, 1);
		if (error == BTRFS_OK) {
			error = btrfs_transaction_evict(
			    volume->running, id, BTRFS_RELEASE_STEP_NODES, &done);
			volume->operations++;
		}
	}
	return error;
}

/* Ends an operation, with the turn held, after evicting the inodes it left to
 * eviction steps; as Linux's eviction, one that finds no room even in a new
 * transaction keeps its orphan for the next mount's cleanup. A transaction
 * that failed after acknowledged operations fails the volume; an operation
 * that failed alone is discarded as an aborted transaction. */
static void
volume_finish(struct btrfs_volume *volume)
{
	struct btrfs_transaction *transaction = volume->running;
	struct btrfs_object_id deferred[VOLUME_DEFERRED];
	const struct btrfs_fs *view = NULL;
	size_t count = 0;
	size_t i;
	enum btrfs_result failure = btrfs_transaction_failure(transaction);
	enum btrfs_result error = BTRFS_OK;

	if (failure == BTRFS_OK) {
		volume->operations++;
		while (count < VOLUME_DEFERRED &&
		    btrfs_transaction_take_deferred(transaction, &deferred[count]) == BTRFS_OK) {
			count++;
		}
		for (i = 0; error == BTRFS_OK && i < count; i++) {
			error = volume_evict(volume, deferred[i]);
		}
		transaction = volume->running;
		failure = transaction != NULL ? btrfs_transaction_failure(transaction) : BTRFS_OK;
	}
	if (failure == BTRFS_OK && transaction != NULL) {
		view = btrfs_transaction_reader(transaction);
		/* Readers cannot see the changes in place: publish them now. */
		if (view == NULL) {
			(void)volume_commit_running(volume);
		}
	}
	volume->locks.lock(volume->locks.context);
	if (failure != BTRFS_OK) {
		__atomic_store_n(&volume->running, NULL, __ATOMIC_SEQ_CST);
		volume->running_view = NULL;
		if (volume->operations != 0 && volume->failure == BTRFS_OK) {
			volume->failure = failure;
		}
		volume->operations = 0;
	} else if (volume->running != NULL) {
		volume->running_view = view;
	}
	volume_give_turn(volume);
	volume->locks.unlock(volume->locks.context);
	if (failure != BTRFS_OK) {
		btrfs_transaction_destroy(transaction);
	}
}

void
btrfs_volume_leave(struct btrfs_volume *volume, struct btrfs_transaction *transaction)
{
	if (transaction == NULL || transaction != volume->running) {
		return;
	}
	volume_finish(volume);
}

enum btrfs_result
btrfs_volume_evict(struct btrfs_volume *volume, struct btrfs_object_id id)
{
	enum btrfs_result error;

	if (!volume->writable) {
		return BTRFS_READ_ONLY;
	}
	volume->locks.lock(volume->locks.context);
	volume_take_turn(volume);
	error = volume->failure;
	volume->locks.unlock(volume->locks.context);
	if (error == BTRFS_OK) {
		error = volume_evict(volume, id);
	}
	if (volume->running != NULL) {
		volume_finish(volume);
	} else {
		volume->locks.lock(volume->locks.context);
		volume_give_turn(volume);
		volume->locks.unlock(volume->locks.context);
	}
	return error == BTRFS_OK ? btrfs_volume_failure(volume) : error;
}

/* Extent-tree items a maintenance step's rescan examines. */
#define VOLUME_RESCAN_ITEMS 4096U

enum btrfs_result
btrfs_volume_maintain(struct btrfs_volume *volume, int *pending)
{
	size_t dropped = 0;
	int partial = 0;
	int done = 1;
	enum btrfs_result error;

	*pending = 0;
	if (!volume->writable) {
		return BTRFS_OK;
	}
	volume->locks.lock(volume->locks.context);
	volume_take_turn(volume);
	error = volume->failure;
	volume->locks.unlock(volume->locks.context);
	if (error == BTRFS_OK && volume->maintenance) {
		error = volume_room(volume, BTRFS_RELEASE_STEP_NODES, 1);
		if (error == BTRFS_OK) {
			error = btrfs_transaction_clean_subvolumes(
			    volume->running, BTRFS_RELEASE_STEP_NODES, &dropped, &partial);
			volume->operations++;
		}
		if (error == BTRFS_OK && btrfs_transaction_quota_rescanning(volume->running)) {
			error = btrfs_transaction_quota_rescan(
			    volume->running, VOLUME_RESCAN_ITEMS, &done);
		}
		/* A finished drop may leave another deleted subvolume. */
		volume->maintenance = error == BTRFS_OK && (partial || dropped != 0 || !done);
		*pending = volume->maintenance;
	}
	if (volume->running != NULL) {
		volume_finish(volume);
	} else {
		volume->locks.lock(volume->locks.context);
		volume_give_turn(volume);
		volume->locks.unlock(volume->locks.context);
	}
	return error == BTRFS_OK ? btrfs_volume_failure(volume) : error;
}

uint64_t
btrfs_volume_pending(struct btrfs_volume *volume)
{
	uint64_t generation;

	volume->locks.lock(volume->locks.context);
	generation = volume->current->generation + 1;
	volume->locks.unlock(volume->locks.context);
	return generation;
}

enum btrfs_result
btrfs_volume_sync(struct btrfs_volume *volume, uint64_t generation)
{
	enum btrfs_result error;

	if (!volume->writable) {
		return BTRFS_OK;
	}
	volume->locks.lock(volume->locks.context);
	if (volume->current->generation >= generation) {
		volume->locks.unlock(volume->locks.context);
		return BTRFS_OK;
	}
	/* Nothing runs and no writer could be starting a transaction: the
	 * changes of generation were published or discarded. */
	if (volume->running == NULL && !volume->writer) {
		error = volume->failure;
		volume->locks.unlock(volume->locks.context);
		return error;
	}
	volume_take_turn(volume);
	volume->locks.unlock(volume->locks.context);
	/* A sync that held the turn before this one may have committed it. */
	error =
	    volume->current->generation >= generation ? BTRFS_OK : volume_commit_running(volume);
	volume->locks.lock(volume->locks.context);
	if (error == BTRFS_OK && volume->current->generation < generation) {
		/* Nothing of that generation remains: its operations failed. */
		error = volume->failure;
	}
	volume_give_turn(volume);
	volume->locks.unlock(volume->locks.context);
	return error;
}

const struct btrfs_fs *
btrfs_volume_read(struct btrfs_volume *volume, struct btrfs_volume_view **view)
{
	const struct btrfs_fs *fs;

	/* Without a running transaction the newest state is the current view. */
	*view = volume_pin_current(volume, 0, view);
	if (*view != NULL) {
		return (*view)->fs;
	}
	volume->locks.lock(volume->locks.context);
	volume->read_waiting++;
	while ((volume->running != NULL || volume->committing) &&
	    (volume->writer || (volume->waiting != 0 && volume->admitting == 0))) {
		volume->locks.wait(volume->locks.context, &volume->readers);
	}
	volume->read_waiting--;
	if (volume->admitting != 0) {
		volume->admitting--;
		if (volume->admitting == 0 && volume->readers == 0 &&
		    volume->running_view == NULL) {
			volume->locks.wake(volume->locks.context, &volume->writer);
		}
	}
	if (volume->running_view != NULL) {
		volume->readers++;
		*view = NULL;
		fs = volume->running_view;
	} else {
		*view = volume->current;
		__atomic_fetch_add(&(*view)->pins, 1, __ATOMIC_RELAXED);
		fs = (*view)->fs;
	}
	volume->locks.unlock(volume->locks.context);
	return fs;
}

void
btrfs_volume_unread(struct btrfs_volume *volume, struct btrfs_volume_view *view)
{
	if (view != NULL) {
		btrfs_volume_unpin(volume, view);
		return;
	}
	volume->locks.lock(volume->locks.context);
	volume->readers--;
	if (volume->readers == 0) {
		volume->locks.wake(volume->locks.context, &volume->writer);
	}
	volume->locks.unlock(volume->locks.context);
}
