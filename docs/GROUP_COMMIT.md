# Group commit design

Status: the portable volume and both native adapters group operations. Mounted
write suites and native power cuts pass for XNU and FSKit; see ACCEPTANCE.md.
Every full commit still pays three device barriers (metadata, secondary
superblocks, primary). Grouping amortizes that cost over a running transaction;
fsync of a pending change still waits for its full publication. XNU's `-o sync`
mode commits each namespace and attribute operation before returning. This
document records the contracts, implementation and remaining reservation proof.

## What Linux does

A Linux Btrfs transaction stays open while system calls add changes to it. It
commits when the commit interval expires (30 s by default), when `sync` or a
full-filesystem `fsync` asks, when memory or metadata pressure requires it, or at
unmount. Namespace calls return before their transaction commits; POSIX makes
them durable only through `fsync` of the object or its directory, or `sync`.
Linux makes a single `fsync` cheap with the tree log, which this implementation
neither writes nor replays (a pending log is refused at admission). Metadata
space is reserved when an operation starts, so the commit itself does not run
out of space; ENOSPC there aborts the transaction and turns the filesystem
read-only.

## Contracts to keep

1. **Crash model.** A commit stays exactly one publication: new metadata,
   barrier, secondary copies, barrier, primary, barrier. Grouping changes how many
   operations one publication carries, never its order or its barriers.
2. **Acknowledgement.** An operation returns once it is applied to the running
   transaction. It is durable after the commit that contains it. `fsync` of an
   object, `fsync` of a directory and `sync` wait for the commit whose generation
   is at least the one recorded for that object (Linux's `last_trans`) or for the
   whole running transaction. The volume records each object's last changing
   generation; a commit that fails after I/O reports the failure to every waiter
   and makes the volume read-only, as today.
3. **Visibility.** Later calls must see earlier ones before they commit. Readers
   of the mount therefore read the running transaction's private view, not only
   the last committed view: lookups, directory streams, attributes and data
   reads go through the transaction's mutation view under a shared lock, which
   writers take exclusively. Pinned committed views remain for consumers that
   need a stable generation (snapshots in progress, background verification).
4. **No commit-time ENOSPC for accepted work.** Once an operation is
   acknowledged, its commit must not fail for space. Each operation reserves its
   worst-case metadata (the nodes its edits may copy on write, as
   `btrfs_calc_insert_metadata_size` counts them) plus its share of the commit's
   accounting fixed point (extent items, block groups, free space and root items
   for the copied nodes) before it changes anything, and is refused with
   NO_SPACE when the reserve does not fit, leaving the running transaction
   usable. The fixed point that `tests/scenario_checks.c` drives into NO_SPACE
   today (edits that fit, a commit that does not) must become impossible for
   accepted operations; a commit that would still exceed the reserve is a bug,
   reported as a failed transaction, never as a partial one.
5. **Bounded transactions.** The running transaction commits before it reaches
   the editor's limits (4,096 dirty nodes, 16 file trees, the reference queue,
   the per-transaction directory bound) instead of refusing an operation. An
   operation that would cross a limit first waits for that commit.
6. **Data pages.** Native caches keep their current ownership: XNU pageout,
   `fsync`, `sync` and unmount push ranges into the running transaction; FSKit
   write callbacks apply their ranges there. Core data writes go to the adapter
   as their extents are created; device staging drains them before the next
   barrier. A commit includes every range pushed before it starts.

## Proposed structure

- The volume owns one running transaction, its allocation map and its counters
  (both already kept per mount). Operations take the writer lock, apply their
  edits and release it without committing.
- A committer issues the commit when a durability waiter exists, when the
  running transaction approaches a limit or its reserve, when a commit interval
  configured by the adapter expires, or at unmount. One commit at a time; new
  operations wait while it publishes, then start the next running transaction
  from the published view.
- NO_SPACE from a reservation first commits the running transaction (frees in
  it become reusable only after a commit) and retries once, as Linux flushes
  delayed work before failing.
- A failed operation that already changed the running transaction poisons it, as
  today; with grouping that would discard other callers' acknowledged work, so
  every refusal must stay decided before the first change (already the rule for
  namespace operations) and reservations must cover every later failure point
  except I/O.

## Implemented in the volume

`include/btrfs/volume.h` adds grouped operations beside the existing
transaction-per-operation calls:

- `btrfs_volume_join(volume, nodes, &transaction)` takes the writer turn and
  returns the running transaction, beginning one when none runs. The caller
  bounds the tree nodes its operation may change; `btrfs_transaction_room`
  keeps half of every per-transaction limit (changed nodes, file trees,
  directory index and privilege slots, queued references) and free metadata
  space for twice the changed nodes plus a fixed allowance for the commit's
  accounting. A running transaction without room commits first; an operation
  that does not fit an empty transaction is refused with NO_SPACE before it
  changes anything. The allowance is a measured margin, not a proven bound: on
  the full-metadata fixture, inline rewrites run to refusal without a failed
  commit, also with half the allowance, and fail an operation after its first
  change without the space check.
- `btrfs_volume_leave` publishes the operation's changes to readers. An
  operation that made the transaction unusable is discarded with it when it
  was alone in it (as an aborted transaction); with earlier operations in it,
  the volume fails and becomes read-only, losing them as at a crash, which is
  Linux's transaction abort.
- `btrfs_volume_pending` names the generation that will publish an operation
  applied now; `btrfs_volume_sync(volume, generation)` commits the running
  transaction when it may hold changes of that generation and waits. Any
  failure of a grouped commit fails the volume. `btrfs_volume_begin` commits a
  running transaction before its own.
- `btrfs_volume_read` returns the running transaction's view between
  operations, shared by readers; otherwise it pins the committed view as
  before. Operations and commits wait for readers of the running view, and
  readers wait for them, including while a commit writes the detached running
  transaction: until its view is published the committed view lacks
  acknowledged operations, so a read then would go back in time. The turn is
  phase-fair: the readers waiting when a turn ends enter before the next
  writer, so a continuous writer does not starve readers. A read of the
  running view must finish within the call; no directory stream outlives it.

`tests/volume.c` checks visibility before commit, one commit per sync, commits
for a begin and for room, refusals that keep the transaction, and both failure
cases; its grouped stress test writes a sequence number with every operation,
and four readers (one also syncing) check that each view they read equals the
model of the sequence number it shows and is not older than the last operation
acknowledged before the read began, under ASan and TSan. Letting readers in
during an operation makes them see torn state; letting them read the committed
view during a commit makes them see an operation disappear (both fail).

## Implementation status and remaining work

1. Metadata reservations per operation and a commit reserve, with tests that
   accepted batches never fail at commit for space (extending the exhaustion
   bisection), and that refusals leave the running transaction usable. The room
   check and adapter call budgets exist. The accounting allowance remains a
   measured margin rather than a proven bound for every supported topology.
2. Read paths over the running transaction's view with a reader/writer lock,
   and TSan stress of concurrent readers against a running writer. Implemented
   in the volume and adapters: `btrfs_transaction_reader` resolves the
   transaction's own trees to their private roots, copies private nodes by
   logical address and reads unchanged nodes from the device and cache with the
   base allocator. The
   scenario harness requires every transaction's view, read through the public
   interface before commit, to equal the published view.
3. Durability waits: per-object last-changing generation, `fsync`/`sync`
   semantics, failure propagation to waiters. The volume provides
   `btrfs_volume_pending` and `btrfs_volume_sync`; both adapters record changing
   generations and sync from their durability and unmount paths.
4. Commit triggers and limits, then adapter integration and the mounted write
   suite, including power-cut checks of every acknowledged `fsync` boundary.
   Implemented and accepted on the native guests; see ACCEPTANCE.md.
5. Measurements per PERFORMANCE.md: operations per barrier, fsync latency and
   throughput against Linux on matched workloads.

A tree log for single-object `fsync` without a full commit is a later, separate
format feature; it needs its own writer, replay and Linux oracle.
