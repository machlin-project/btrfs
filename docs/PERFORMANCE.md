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
without repacking the whole node. The reservation gap vector begins at 4 KiB and
grows only when needed. A volume keeps its allocation state across transactions:
only the admission transaction loads the extent tree and verifies the free-space
and device trees, and later begins copy the kept state (on the small Linux
fixtures a full load costs 18 to 39 reads and 72 to 588 KiB, which grows with
the extent tree). Verified tree nodes can be cached across operations (below).
Each commit still pays three barriers; grouping operations into one commit is
designed in [group commit](GROUP_COMMIT.md). Each backreference edit copies its
extent item into a fresh node-sized buffer and probes for keyed items with a new
cursor, so a shared-subvolume commit costs several hundred allocations; the test
output prints these counts per commit. New file data is written to the device
as its extents are created, from rewrite pieces of at most 8 MiB; unaligned
edges are read back through the private view. Compression costs one codec call
per 128 KiB. Buffer reuse and extent-item caching are unmeasured follow-up
work.

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
| 700-entry stream with each inode | 1,100 us, 1,416 reads | 90 us, 1 allocation |
| 1 MiB sequential read | 140-160 us, 5.2 reads | 60-95 us, 1 read, no allocations |
| Mount, create one file, commit | 90-110 us, 61 reads | 55 us, 5 reads |
| Create and write an 8 MiB file, commit | 1.8 ms, 71 reads | 1.7 ms, 5 reads |

Against the first cached measurement (lookup 1.2 us, stat during enumeration
203 us, a create commit 185 us), the gains came from cursors reading cached
nodes in place, inlined key and integer decoding, a commit adding its published
nodes to the cache, and a transaction reading its own nodes without checksumming
them twice, and from reusing the primary superblock a transaction has just read
instead of reading it again. Sequential reads gained from checksumming four data
sectors in independent CRC instruction chains and from reading aligned sectors
into the caller's buffer instead of copying them out of a window (the table's
first sequential figures predate both). The remaining reads are superblocks: the primary at
mount, and at both begin and commit the primary and each secondary copy present
on the device. The written images are byte-identical to those before these
changes. Writes in
this harness depend on host file I/O and vary between runs; compare them only
within one run. A commit still writes and barriers per operation. Generated
benchmark reports keep the exact figures and build identities.

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
