/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_VOLUME_H
#define MACHLIN_BTRFS_VOLUME_H

#include <btrfs/write.h>

/* A mounted volume shared by native adapters. Readers use versioned views:
 * each committed root set is one immutable btrfs_fs, pinned for the duration
 * of an operation. One writer at a time runs a transaction on the current
 * view, with the mount's inode-number and directory-index counters attached;
 * a successful commit publishes the next view, and older views are
 * retired when their last pin goes. The next transaction waits until no view
 * older than its base is pinned, because it may reuse blocks those views still
 * reference. A commit that may have reached media and failed leaves the
 * volume read-only for the rest of the mount (FAILED reports it). */
struct btrfs_volume;
struct btrfs_volume_view;

/* Adapter synchronization: one mutex and sleep/wake channels. wait sleeps
 * with the mutex held and returns with it held. */
struct btrfs_volume_locks {
	void *context;
	void (*lock)(void *context);
	void (*unlock)(void *context);
	void (*wait)(void *context, const void *channel);
	void (*wake)(void *context, const void *channel);
};

/* writer NULL opens the volume read-only. A writable volume admits its first
 * transaction at open (superblock copies, features), so an inadmissible
 * filesystem fails here rather than at the first write. */
enum btrfs_result btrfs_volume_open(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, const struct btrfs_volume_locks *locks,
    uint64_t tree, struct btrfs_volume **result);
/* Every view must be unpinned and no transaction may be open. */
void btrfs_volume_close(struct btrfs_volume *volume);
int btrfs_volume_writable(const struct btrfs_volume *volume);
/* How often a writable volume's transactions loaded and verified the
 * allocator state (scans) and reused the state its last commit left. */
void btrfs_volume_allocation_counts(
    const struct btrfs_volume *volume, uint64_t *scans, uint64_t *reuses);
/* The current committed generation; it changes after each published commit. */
uint64_t btrfs_volume_generation(struct btrfs_volume *volume);
enum btrfs_result btrfs_volume_failure(struct btrfs_volume *volume);

/* Pins the current view; its btrfs_fs stays valid until unpinned. */
const struct btrfs_fs *btrfs_volume_pin(
    struct btrfs_volume *volume, struct btrfs_volume_view **view);
void btrfs_volume_unpin(struct btrfs_volume *volume, struct btrfs_volume_view *view);

/* Exclusive writer. begin waits for any other writer and for older views to
 * drain. commit publishes the transaction and opens its view; abort discards a
 * transaction (only its new data reached media, in space no committed root
 * references). Both end the writer's
 * turn and destroy the transaction. A transaction that changed nothing commits
 * without writes. The caller must not hold a pin while beginning. */
enum btrfs_result btrfs_volume_begin(
    struct btrfs_volume *volume, struct btrfs_transaction **transaction);
enum btrfs_result btrfs_volume_commit(
    struct btrfs_volume *volume, struct btrfs_transaction *transaction);
void btrfs_volume_abort(struct btrfs_volume *volume, struct btrfs_transaction *transaction);

/* Grouped operations (docs/GROUP_COMMIT.md). Operations share one running
 * transaction and return before it commits; their changes are visible to
 * readers at once and durable after the commit that contains them, which
 * btrfs_volume_sync requests. join waits for the writer turn and returns the
 * running transaction, beginning one when none runs. When the running
 * transaction lacks room for an operation that changes at most nodes tree
 * nodes (btrfs_transaction_room), join commits it first; NO_SPACE means the
 * operation does not fit an empty transaction. The caller applies its
 * changes and calls leave. An operation that made the transaction unusable
 * fails the volume when earlier operations' changes are in it: they are lost
 * as at a crash and the volume becomes read-only; alone, it is discarded as an
 * aborted transaction. A begin commits the running transaction first. */
enum btrfs_result btrfs_volume_join(
    struct btrfs_volume *volume, size_t nodes, struct btrfs_transaction **transaction);
void btrfs_volume_leave(struct btrfs_volume *volume, struct btrfs_transaction *transaction);
/* The generation that will publish an operation applied now; record it
 * between join and leave. */
uint64_t btrfs_volume_pending(struct btrfs_volume *volume);
/* Makes every change of generation or earlier durable: commits the running
 * transaction if it may hold them and waits for that commit. A commit that
 * fails fails the volume. */
enum btrfs_result btrfs_volume_sync(struct btrfs_volume *volume, uint64_t generation);
/* A read of the newest state. Between operations of a running transaction it
 * is that transaction's view, shared with other readers and excluding
 * operations and commits until btrfs_volume_unread; otherwise the current
 * committed view, pinned. Readers finish within the call: no directory stream
 * or other state of the view outlives the read. */
const struct btrfs_fs *btrfs_volume_read(
    struct btrfs_volume *volume, struct btrfs_volume_view **view);
void btrfs_volume_unread(struct btrfs_volume *volume, struct btrfs_volume_view *view);

#endif
