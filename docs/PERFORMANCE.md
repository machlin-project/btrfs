# Performance acceptance

The product target is higher throughput than Linux Btrfs on matched filesystem
workloads, without losing integrity, durability, authorization or concurrency
semantics. This is an open release gate. A portable image read, checksum benchmark
or unsigned adapter build cannot establish mounted filesystem performance.

## Existing architecture gates

`btrfs-adversarial` counts backend reads and allocations for a single verified
4 MiB read on the Linux-authored plain image. The budget is at most 64 I/O calls
and 16 allocations. The initial accepted run uses 7 reads and 4 allocations.
These are deterministic structural costs, not speed measurements. ASan/UBSan
are enabled for this check, so wall-clock timing would be misleading.

A 700-entry directory stream must use at most 32 reads and 8 allocations; the
accepted run uses 5 reads and 3 allocations. Both native adapters use this stream.
Attribute requests may require additional inode lookups; the budget measures
name/type/identity enumeration only.

Retained cursor paths avoid repeated metadata I/O within a read; checksum records
are consumed across sectors; data reads use up to 1 MiB windows. ARM CRC32C uses
general-register hardware instructions. Sparse gaps do not iterate over each
missing block. Subvolume identity uses separate hash indexes by object and native
number instead of scans over all issued IDs.

Before optimizing further, measure these remaining costs: non-selected subvolume
operations re-resolve their root; enumeration with attributes fetches each inode;
each convenience read creates a new operation context. The XNU adapter now
coalesces aligned device requests through private I/O buffers, with one-block
bounce storage only at unaligned edges.
Do not conceal these costs behind hot-cache numbers. A bounded,
generation-aware node cache exists and both native adapters enable it (see
ARCHITECTURE.md). Reusable read sessions remain follow-up work; optional native
I/O instrumentation is available for attributing device and barrier costs.

The private editor reuses its dirty paths; repeated fixed-size replacement in an
already modified path allocates nothing. It updates payloads and child pointers
without repacking the whole node, and an insertion, resize or deletion that fits
a leaf it has already packed moves only the item headers and data after the
edited slot. The reservation gap vector begins at 4 KiB and
grows only when needed. A writable transaction reads every block group's item
and loads a block group's extents (verifying them and its free-space items)
only when allocation reaches it or a change touches it, so admission costs a
few node reads per block group instead of a pass over the whole extent tree
(on the full-metadata fixture 20 node reads instead of 44; on the small
fixtures about the same). On `transactions-scale`, Linux-written with 102,042
extents in 15 block groups on 2 GiB, admission reads 35 nodes (0.14 MiB)
instead of 2,270 (8.9 MiB); the first data write and its commit then load the
groups they touch, so mount, admission, one create, a 64 KiB write and the
commit read 541 nodes instead of 2,380. Full block groups are passed over
without loading. The scale fixture is a measurement input, not part of the
suite. A volume keeps the loaded state across transactions,
and a transaction copies a class's free ranges only when it changes them.
Verified tree nodes can be cached across operations (below). Each commit still
pays three barriers; grouping operations into one commit is described in
[group commit](GROUP_COMMIT.md). Each backreference edit copies its
extent item into a node-sized buffer and probes for keyed items with a new
cursor; a transaction keeps up to 16 released node-sized buffers for reuse, so
these cost allocations only when more are live at once (a create commit takes
44 allocations instead of 97); the test output prints the counts per commit.
New file data is written to the device as its extents are created, from rewrite
pieces of at most 8 MiB; unaligned edges are read back through the private view.
Compression costs one codec call per 128 KiB. Extent-item caching is unmeasured
follow-up work.

## Measured implementation costs

`btrfs-bench` (see DEVELOPMENT.md) measures this implementation on an image file
through the host page cache: CPU and backend-call costs per operation, not a
mounted filesystem and not a comparison with Linux. On `transactions` (4 KiB
nodes) in a release build, without and with a 64 MiB node cache:

| Operation | Without cache | With cache |
| --- | --- | --- |
| 4 KiB random read of a 4 MiB file | 4.0 us, 5 reads | 1.0 us, 1 read, no allocations |
| Path lookup in a 700-entry directory | 6.5 us, 8 reads | 0.6 us, no reads or allocations |
| 700-entry directory stream | 24 us, 16 reads | 12 us, 0.1 reads |
| 700-entry stream with each inode | 77 us, 77 reads | 40 us, 1 allocation |
| 1 MiB sequential read | 140-160 us, 5.2 reads | 60-95 us, 1 read, no allocations |
| Mount, create one file, commit | 90-110 us, 61 reads | 55 us, 5 reads |
| Create and write an 8 MiB file, commit | 1.8 ms, 71 reads | 1.7 ms, 5 reads |

Against the first cached measurement (lookup 1.2 us, stat during enumeration
203 us, a create commit 185 us), the gains came from cursors reading cached
nodes in place, inlined key and integer decoding, a commit adding its published
nodes to the cache, and a transaction reading its own nodes without checksumming
them twice or rescanning their items on every read (it checks each node's
items once, when it seals them) and, on its own views, without copying them,
and from reusing the primary superblock a transaction has just read
instead of reading it again. Sequential reads gained from checksumming four data
sectors in independent CRC instruction chains and from reading aligned sectors
into the caller's buffer instead of copying them out of a window (the table's
first sequential figures predate both). A directory stream reads the inodes its
entries name through a second tree path it keeps, so an inode next to the
previous one costs a search of the leaf already held and no device read (the
row took 1,100 us and 1,416 reads before, and 90 us with the cache). Node checksums run in three such chains
over thirds of the node, joined by an operator computed at mount that advances
a CRC over a third of zero bytes: on the development host, 33 GB/s instead of 11
for every node size. The remaining reads are superblocks: the primary at
mount, and at both begin and commit the primary and each secondary copy present
on the device. The written images are byte-identical to those before these
changes. Writes in
this harness depend on host file I/O and vary between runs; compare them only
within one run. Generated benchmark reports keep the exact figures and build
identities.

The volume series create 200 empty files, each in its own transaction and
commit, then as grouped operations followed by one sync
([group commit](GROUP_COMMIT.md)). With `--durable`, every barrier is the
image file's `F_FULLFSYNC` on the host SSD:

| Create | Barriers skipped | `--durable` |
| --- | --- | --- |
| One transaction and commit each | 52 us, 5 reads, 51 allocations | 16 ms |
| Grouped, one sync for all | 3.1 us, 3.7 allocations | 0.09 ms |

A per-operation commit pays three barriers, which dominate durable latency;
grouping amortizes them over the running transaction. A commit writes its node
copies in physical order, merging contiguous ones into writes of at most
256 KiB: on `transactions`, a single create's commit issues 10.9 writes for
15.2 nodes on average, a commit of 100 grouped creates 23 for 56, and one of
1,000 grouped creates 19 for 211. Both native adapters use the volume's grouping;
matched mounted performance remains a separate gate.

A cache with locks also keeps sixteen decompressed extents, so small reads
within one compressed extent decompress it once. Sequential 4 KiB reads of the
4 MiB file on the `zstd` and `zlib` fixtures take 0.5 and 1.3 us per read
instead of 4.8 and 32 us (8.1 instead of 0.85 GB/s, and 3.1 instead of
0.13 GB/s). Random reads across a file far larger than those 2 MiB gain
little; there the native page cache keeps the pages already read.

Concurrent cached lookups of names in one directory, each walking the same
tree roots, scale with lock-free hits and striped pin counters: on the
`transactions` image, 1.6, 3.1, 4.9 and 5.1 million lookups per second with one,
two, four and eight threads, where a cache lock gave 1.6, 1.0, 0.4 and 0.5
million. Reads through the native volume pin its current view without a lock
as well: eight threads reach 5.1 million there, where the volume lock limited
them to 1.1 million.

### Commit accounting and private-tree edits

Seven alternating before/after trials of `--cache 64 --volume-only
--volume-files 2000`, each on a fresh clone of its Linux-authored fixture, give
the following median process CPU times. These are release builds through the
image-file backend, with barriers skipped, not durable or mounted filesystem
latencies. The grouped column includes its final sync, amortized over 2,000
creates. Wall time is recorded separately because host I/O stalls especially
affect the scale image.

| Fixture | Single create + commit, before / after | Grouped create, before / after |
| --- | --- | --- |
| `transactions`, 4 KiB nodes | 39.94 / 31.03 us | 2.30 / 1.55 us |
| `transactions-namespace`, free-space tree | 54.45 / 37.33 us | 2.39 / 1.55 us |
| `transactions-fst`, 16 KiB DUP, bitmaps | 70.44 / 50.73 us | 2.17 / 1.41 us |
| `transactions-scale`, 102,042 starting extents | 173.26 / 131.97 us | 2.67 / 1.80 us |

Single-commit CPU cost falls by 22–31%; grouped create cost by 33–35% (up to
1.54 times the throughput). The changes normalize a copy of each round's
allocation log, cancel exact allocation/release pairs and merge adjacent ranges,
read a group's free-space info once and avoid repeated bitmap descents. The
editor recognizes packed source leaves before their first edit, computes packed
occupancy in constant time, and finds exact items on entirely private paths
without creating a cursor. Small memory primitives are inline so the compiler
can specialize fixed-size copies. Accepted editors transfer their node buffers
to the cache at destruction, and reuse up to eight retired cache buffers in the
next transaction, within the configured node-byte budget.

The common metadata CoW with one inline implicit owner moves its loaded extent
item to the replacement block instead of dropping and recreating it. Equal-size
key changes within a packed leaf move just the interval between the two slots;
cross-leaf moves retain the ordinary delete/insert path. Free-space extent
trimming and neighbour merging use the same operation, often changing only a
key. Other ownership cases keep the existing conversion and reference order.
The volume receives an independently owned committed view prepared before
publication, eliminating its post-commit mount and the associated root reads.
Source nodes and the allocator's original ordered log remain immutable; all
three persistence barriers remain in force.

Backend reads fall from 5 to 4 and read bytes from 20 to 16 KiB per single commit
(5.2 to 4.1 reads and 20.6 to 16.6 KiB on scale). Despite temporary pending-log
copies, the complete batch removes approximately seven to nine allocations per
single commit. A pending-log copy is bounded by 4 MiB on 64-bit targets.
Three additional alternating pairs compare the preceding buffer-transfer and
reference-lookup checkpoint with the final view-handoff and key-move changes:
single-commit CPU falls from 34.62 to 31.25 us on `transactions`, 40.81 to
37.32 us on namespace and 58.49 to 50.13 us on the bitmap fixture (9–14%).
Scale changes from 137.40 to 135.67 us (1%), with grouped costs broadly flat.
These incremental trials and the full-batch comparison above are separate runs.

A three-pair read-only control at the buffer-transfer checkpoint on `plain`
shows sequential reads, random reads
and lookup close to the baseline. Enumerating 700 entries with stat falls from
40.00 to 28.36 us CPU. Enumeration without stat rises from 10.76 to 11.87 us CPU
(wall time 12.05 to 12.24 us); these small control samples do not establish a
general read-path improvement. Read I/O and allocation counts are unchanged.

CRC32C already uses general-register hardware
instructions and parallel lanes. Kernel builds use `-mgeneral-regs-only` on
arm64e, with a compile guard and zero SIMD/FP operands in the emitted core and
adapter objects; `-mkernel` alone permits compiler-generated NEON copies (x86_64
kernel builds already avoid implicit SIMD). The final changes pass the full
portable suite, the Linux oracle on all nineteen profiles and the native mounted
suites (ACCEPTANCE.md). Raw trials, sampling profiles and binary identities
live under the ignored `logs/perf-commit-20261002/` and
`artifacts/perf-commit-20261002/` directories.
These results do not establish a 2–4 times faster full commit or a win over
Linux. Tree-log fsync remains separate work. The later mounted FSKit change
below avoids superblock device rereads under its exclusive-resource contract.

### Native numbering and whole-volume walks

A mount could earlier number only 65,536 objects in its lifetime, each one an
allocation in a fixed hash table, and XNU readdir numbered every entry it
returned: listing more objects than that failed. Objects of the mounted
subvolume now carry their inode numbers, computed without allocation or lock
contention beyond the adapter's table lock; the XNU node hash has one bucket per
eight vnodes the system keeps instead of 128 per mount.

`btrfs-mounted-walk-test` reads a whole volume with readdir and lstat per name,
as `find` or `ls -l` does. On the stock macOS 26.5.2 guest, installed FSKit
build 7 walks `transactions-scale` (100,815 names in 104 directories,
read-only mount) at 7,439 to 7,845 names per second in four runs (two of them
while sampled), and the plain profiles at 6,178 to 8,661. A sample of the extension during the walk
finds it busy for about a fifth of the time, most of that in B-tree descents
for lookup and inode reads and in device reads for nodes outside the cache;
the remainder is FSKit's per-name lookup and attribute requests. Build 6
cannot finish the scale walk.

On the same guest the same tree (100 directories of 1,000 files of 4 KiB,
created by one program, walked cold by `walkbench`, which does readdir and
lstat of each name) gives:

| Filesystem | Create 100,000 files | Walk, names per second (three cold mounts) |
| --- | --- | --- |
| APFS (in-kernel) | 22 s | 92,780; 72,760; 56,213 |
| exFAT (Apple's FSKit module) | 168 s | 3,692; 3,748; 3,749 |
| Btrfs (FSKit build 9, 1 GiB image) | 110 s with the final sync | 7,670; 7,754; 7,696 |
| Btrfs (FSKit build 8, writes issued at once) | 843 s with the final sync | 7,665; 7,644; 7,648 |

Build 8 spent 84% of the extension's time during creation in one synchronous
device write per file's data sector. Build 9 keeps device writes until the
next barrier and merges adjacent ones (on the host, 64 one-sector files reach
the device in 14 writes): creation became 7.7 times faster. Through FSKit,
Btrfs now walks twice as fast and creates 1.5 times as fast as Apple's exFAT
module. These are single-guest measurements; the loaded kernel module and
Linux are not compared yet.

### XNU writeback and normal unmount

Outgoing cluster buffers now use `buf_map_range_with_prot(..., VM_PROT_READ)`.
The previous `buf_map` creates a writable kernel mapping: on arm64, XNU marks
its physical pages modified even when the adapter only reads them. Each push
therefore dirtied its own pages again. Reclaim's later fsync wrote the data
again and published a separate transaction for each file.

Three fresh-image runs before and three after on the same 8-vCPU, 16-GiB
macOS guest give these create-plus-normal-unmount times for 1,000 files of
4 KiB, including both guest command round trips:

| Build | Individual durations | Median |
| --- | --- | --- |
| Before this native I/O batch | 33.274, 30.520, 30.344 s | 30.520 s |
| With write combining and read-only outgoing mappings | 0.676, 0.375, 0.697 s | 0.676 s |

The median improves by 45.1 times. Every run remounts read-only and verifies
every byte. One 100,000-file run completes creation and normal unmount in
13.314 s, then verifies every file on a fresh read-only mount. The old
100,000-file unmount was interrupted after exceeding its time bound and is
not a completed baseline; no speed ratio is claimed at that size. Runs are
sequential before/after, not alternating, and no samples are discarded.

A separate DTrace comparison isolates the mapping fix after write combining
was already present. For 1,000 bulk files plus 96 individually synced files,
strategy writes fall from 4,772 to 1,129 and commits from 1,196 to 97; all
three volume-sync calls succeed. The timing table uses uninstrumented runs.
Single-file durability remains barrier-bound: median throughput is 39.341
versus 38.988 files/s at 4 KiB and 38.105 versus 38.248 at 128 KiB. This batch
does not establish a single-fsync improvement in XNU or a win over Linux.

### Mounted FSKit superblock reads

An exclusive writable mount now retains the superblock copies it reads and
updates them with its own writes. The core still checks every checksum,
generation and identity on admission and commit. A new load reads the device
again; failed writes/barriers invalidate the copies and resource revocation is
checked even on hits. The cache occupies 12 KiB and changes none of the three
persistence barriers.

Seven alternating pairs of ordinary signed builds on the same stock macOS
26.5.2 guest compare build 9 with build 17. Each run starts with a fresh clone
of the same Linux-authored 1 GiB image, creates 64 files of 4 KiB and 32 files
of 128 KiB in separate directories, and verifies all contents. Every operation
includes create, write, file fsync, close and directory fsync. These are mounted
end-to-end latencies, not the portable core's CPU cost.

| File size | Median throughput before / after | Median paired throughput gain | Median mean file-fsync latency before / after |
| --- | --- | --- | --- |
| 4 KiB | 6.823 / 8.443 files/s | 23.7% | 128.23 / 114.20 ms |
| 128 KiB | 6.036 / 7.344 files/s | 21.7% | 146.27 / 132.15 ms |

The gain is the median of each pair's ratio, not a ratio of the two medians.
All seven pairs and their tails are retained. File fsync alone improves by
about 11%; eliminating the admission reads also makes create much cheaper,
which contributes to the larger gain for the whole operation.

A separate diagnostic build attributes roughly 27 ms per 4 KiB operation to
four superblock reads, 66 ms to device writes and 41 ms to the three device
flush ioctls. XPC overhead outside those ioctls is under 1 ms in that sample.
The resource already uses a raw character device. Concurrent resource writes
did not give a repeatable improvement and are not part of the implementation.
The ordinary builds used for comparison contain no diagnostic logging.
Raw trials and identities are in the lab's ignored
`artifacts/btrfs-kext/fsync-profile-20261003/` directory. This is a comparison
between two versions of the same FSKit driver; Linux remains unmeasured.

## Remaining opportunities after the native I/O changes

The following findings come from inspecting the current implementation. Their
speedups have not been measured. The next two local core changes should target
block-group accounting and checksum-item packing; the larger adapter and
durability changes follow separately.

### Update only changed block groups

`bt_tx_prepare` in `core/transaction.c` replaces every active block group's item
in every accounting round, even when its used-byte count did not change.
`bt_tx_edit` forwards directly to the private editor; `bt_mutation_apply` copies
the path before `bt_mut_leaf` replaces the payload, with no equality check.
Consequently a small transaction can CoW extent-tree leaves for untouched
groups, adding allocations, reference updates, free-space edits and writes to
its own fixed point. Already-private paths avoid another copy, but still pay
the traversal and replacement in subsequent rounds.

This is a dependency on the number of block groups, which a benchmark varying
only files per directory does not isolate. First retain each group's last
published used-byte count and skip unchanged replacements. Then let allocator
accounting enqueue changed groups, including changes caused by the commit
itself, and maintain the total used count incrementally. New and removed groups
must participate, and convergence must include changes made while processing
that queue. Chunk-map copies and admission still depend on group count; this
does not make the entire transaction constant-time.

Measure one create/commit on Linux-authored images with increasing group counts
and comparable occupancy, recording unique CoW nodes, accounting rounds and
metadata bytes as well as CPU time. Keep the reference/free-space audits and
growth, removal and exhausted-metadata cases when implementing the change.

### Pack adjacent checksums before adding delayed allocation

`bt_csum_insert` in `core/csum.c` creates new items for each call and never
extends an adjacent item. For 32 consecutive checksummed 4 KiB allocations it
therefore creates 32 items. If the logical device ranges are contiguous, one
item can hold all 32 checksums: 153 bytes of item header plus payload instead
of 928 bytes, excluding the leaf header and unused space. This is an on-disk
representation calculation, not a sixfold throughput prediction. Append and
merge paths should respect the existing per-item limit, reject overlaps and
retain correct partial deletion and shared-extent behavior. Reusing checksum
scratch storage can also remove a node-sized allocation from each insert.

Separately, each FSKit write callback calls `btrfs_transaction_write`, which
creates file extent and extent-reference items for its new allocations. Device
write staging combines I/O only after those records exist. Buffering adjacent
file ranges before allocation could reduce all three trees' item counts; it
requires bounded memory and space reservations, coherent reads and correct
truncate, fsync and failure handling. Measure callback sizes and resulting
extent counts first. XNU already combines file writes through UBC.

### Reduce repeated FSKit metadata work and callbacks

On the current adapter, lookup reads the inode and saves it in `BtrfsItem`, but
the following `getAttributes` calls `refreshItem`, which reads it again.
Mutation callbacks also refresh records after updating them. A coherent inode
cache can remove repeated traversals on macOS 26. Its validity must follow
operation publication, not only committed generation: many mutations share a
running transaction. Preserve `(tree,inode)`, orphan lifetime, parent changes
and ordering between concurrent readers and writers.

The selected macOS 27 SDK exposes `FSVolumeHandler` and result objects carrying
attributes with lookup, create and rename. `FSVolumeHandlerResult` specifies
which attributes to populate and cache; create results can also supply free
space, avoiding the fallback statistics query documented by the SDK. Adopting
these protocols is a separate way to reduce FSKit callbacks, with a macOS 26
fallback. It needs a macOS 27 guest for mounted acceptance; compiling against
that SDK or timing the existing 26.5.2 guest cannot establish the gain.

### Let reads progress while a sealed transaction persists

`volume_commit_running` in `adapters/common/volume.c` holds the writer turn
across persistence, and `btrfs_volume_read` waits for it. Consequently metadata
and data reads that reach the volume can wait through all three barriers.
This affects mixed-workload latency even when their data is already in the
core cache; reads satisfied entirely by the native page cache need not enter
the volume.

A possible design exposes a pinned, immutable view of the sealed transaction
while it persists. It must retain visibility of previously acknowledged
operations, prevent block reuse underneath readers and define failure
behavior. Returning the old committed view would restore the stale-read bug
covered by the volume stress test. Measure read p95/p99 during concurrent fsync;
this change targets stalls, not the cost of an individual durable commit.

### Treat durable fsync as a separate architecture project

The measured core CPU times above are tens to hundreds of microseconds, while
mounted fsync takes tens to hundreds of milliseconds. Faster CRC or another
private-tree micro-optimization cannot remove the measured device and barrier
latency. Hardware CRC and independent instruction chains are already present;
the arm64e kernel build deliberately emits no SIMD/FP instructions.

A tree log can reduce the work persisted by a single-object fsync, but needs
its own writer, mount replay, full-commit fallback and power-cut/Linux oracle.
That scope remains separate from the local changes above. Three barriers are
still the accepted publication protocol; a two-barrier design requires its own
crash analysis and acceptance. Caching exclusive-mount superblocks in XNU is
also possible, but the FSKit cache's measured gain cannot be transferred to
XNU's cheaper in-kernel read path without a new measurement.

## Matched benchmark protocol

1. Freeze binaries, fixture images and mount options in generated reports. Record
   OS/kernel/driver identity, CPU allocation, RAM, cache state, block geometry,
   compression and checksum algorithms. Source revisions remain in Git.
2. Use equal dedicated VM resources and the same virtual disk transport for Linux
   Btrfs and the XNU adapter. FSKit is a separate reported series. Do not compare
   a host image reader with a VM mounted filesystem as if they were equivalent.
3. Run correctness and durability oracles first. Benchmarks fail if the reader
   skips checksums or a writer returns success without the required flush.
4. Alternate at least seven Linux/driver runs. Report every sample, median,
   variability, throughput, p50/p95/p99 latency, CPU time, peak memory, backend
   I/O bytes/calls, allocations and flush count. No unrelated VM/test workloads.
5. Separate cold data, warm metadata, warm file cache, tiny datasets and datasets
   larger than guest RAM. Remount alone is not proof of a cold host device cache.
6. For writes include identical fsync cadence, crash verification, snapshots and
   equivalent compression. Report write amplification and metadata cost, not just
   user-byte throughput. Never trade away CoW isolation for a winning number.

| Workload | Sizes/concurrency | Mandatory accompanying checks |
| --- | --- | --- |
| Sequential read | 4 KiB/64 KiB/1 MiB calls; 1/4/16 readers | Hash, allocated bytes, cold/warm series |
| Random read | 4 KiB/16 KiB; queue depths 1/4/16/64 | Same seed, identical working set and checksum policy |
| Namespace | 1k/100k/1M entries; hit/miss/collision lookup | Full enumeration, stable resume, bounded memory |
| Compressed read | zlib/LZO/Zstd, low/high entropy | Same image and exact decoded bytes |
| Shared extents | snapshots, reflink, overwritten middle | Isolation and backreference validation |
| Mixed read/write | readers + append/overwrite/truncate | No stale pages, torn publication or lock starvation |
| Durable write | 4 KiB fsync; batched fsync; directory fsync | Every acknowledged boundary survives power cut |
| Full/fragmented filesystem | 90/99/100% fill | ENOSPC rollback and recovery |

An overall claim requires the agreed representative workload set and no material
correctness or tail-latency regression. A win in one row is reported as that row's
win. This repository currently makes no measured claim of outperforming Linux.
