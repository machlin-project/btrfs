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
generation-aware node cache exists (see ARCHITECTURE.md); reusable read sessions
and native I/O instrumentation are explicit follow-up work, as is enabling the
cache in the native adapters.

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
adapter objects; `-mkernel` alone permits compiler-generated NEON copies.
The first checkpoint passed full portable acceptance and thirteen independent
Linux oracle profiles. Subsequent changes passed focused ASan/UBSan checks;
the full suite and Linux oracle were not repeated for them. See ACCEPTANCE.md
for the final concurrency and native-compilation evidence boundary.
Raw trials, sampling profiles, binary identities and acceptance logs live under
the ignored
`logs/perf-commit-20261002/` and `artifacts/perf-commit-20261002/` directories.
These results do not establish a 2–4 times faster full commit or a win over
Linux. Tree-log fsync and avoiding mounted-volume superblock rereads are
separate changes with separate durability contracts.

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
