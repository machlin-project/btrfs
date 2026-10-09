# Handoff for continued implementation

Continue this repository on `development`. The portable reader and CoW writer,
installed FSKit and loaded XNU mounts have read/write acceptance, including
grouped commits and native power cuts independently checked by Linux. Both
adapters mount read-only by default and enable writes explicitly. Preserve
these implementations and extend their contracts; see ACCEPTANCE.md for the
remaining limitations and PERFORMANCE.md for measured costs and next targets.

Read AGENTS.md, ARCHITECTURE.md, ACCEPTANCE.md and DEVELOPMENT.md. The main agent
owns design, implementation, tests and diagnosis; GPT-6 Luna executes prepared
build/test commands and GPT-6.1 Sol owns VM boot/recovery. Give workers absolute
directories and one VM owner at a time. Use personal Git identity. Generated
reports hold runtime identities and hashes; Git holds source revisions.

## Start by reproducing the accepted behavior

From the Btrfs repository:

```sh
make test MESON_OPTIONS='-Dfixtures=artifacts/fixtures'
make check-style
```

Require fifty passing test processes on macOS (forty-nine elsewhere) and
all seven reader profiles (367 contracts). Thirteen writable images are required
by the transaction suites:
`transactions` (4 KiB single), `transactions-dup` (16 KiB DUP),
`transactions-large` (64 KiB DUP), `transactions-full` (128 MiB with full,
fragmented metadata), `transactions-shared` (snapshots, reflinks, offset
references), `transactions-keyed` (keyed backreferences), `transactions-data`
(file data inputs with snapshots), `transactions-fst` (mkfs defaults with a
free-space tree in extent and bitmap form), `transactions-grow` (nearly full
metadata with unallocated device space), `transactions-namespace` (name-hash
collisions, extended references, compression properties) and
`transactions-holes` (no NO_HOLES, DUP data), `transactions-convert` (1 GiB with
a free-space tree whose conversions Linux measured) and `transactions-copies`
(257 GiB sparse, three superblock copies); the Linux crash-state test needs the
recorded `logwrites.log` and `logwrites-data.raw`, and the replay tests the
pending-log images `logs` and `logs-many` with the manifests Linux printed
after replaying them (`*.expected.tsv`). Missing fixtures
are failures. Recreate them in a disposable Linux VM using DEVELOPMENT.md, which
also describes the exported crash cases and the two-disk Linux oracle.

`tests/mounted.c` and `tests/run_macos.py` establish actual XNU mount behavior on
four profiles. Use the prepared dedicated Btrfs guest and check its loaded kernel
and module UUIDs before running; compilation or a packaged collection is not boot
acceptance. The native test artifact is unsanitized for the guest. All normal
unmounts, detach operations and unchanged-media hash checks must succeed.

## Implementation map

| Component | Existing contract | Main files |
| --- | --- | --- |
| Immutable reader | Validated geometry, chunks, trees, inodes, namespaces, extents/checksums, subvolumes and xattrs | `core/{mount,chunk,tree,inode,directory,parent,read,xattr}.c` |
| Private tree editor | Path CoW; insert/replace/upsert/delete; variable-item splits including three leaves; root growth and collapse; poisoned failures | `core/mutable.c`, `tests/mutable.c` |
| Reservation allocator | Extent-map and block-group reconciliation, physical alias/super-stripe exclusion, pinned committed allocations, bounded free gaps | `core/space.c` |
| Transaction owner | Multi-inode inline replacement, copy agreement/staleness admission, reference/accounting fixed point, root/backup updates, three barriers, terminal failures | `core/transaction.c`, `include/btrfs/write.h` |
| Superblock recovery | Explicit newest-valid-copy selection with acknowledged floor, foreign-copy and non-primary-log refusal, a primary's log kept for replay, selection validation, rewrite of disagreeing copies | `core/recovery.c`, `include/btrfs/write.h` |
| Tree-log replay | Linux's log passes in one transaction through a G+1 log view: withheld log blocks and logged extents, inode overwrite rules, directory and xattr deletions, names, back references, file extents with references, allocation and checksums, link-count fixups with orphans | `core/replay.c`, `include/btrfs/write.h`, `tests/replay.c` |
| Free-space tree | Verification against the extent tree, logged allocation changes applied in the fixed point, extents and bitmaps | `core/fst.c`, `core/space.c` |
| File data / checksums | CoW writes and truncation, drop-extents splitting, data written as extents are created, compression on write, inline small files, private read view, checksum items | `core/data.c`, `core/csum.c` |
| Namespace mutations / audit | Create of every type, link, unlink with orphans, rename with replacement, packed collision items, xattrs and the compression property; independent namespace audit | `core/namespace.c`, `tests/namespace_audit.c` |
| Subvolumes / cleaner | Subvolume and snapshot creation, deletion with root references and UUID tree entries, resumable drop walk with Linux's FULL_BACKREF conversion | `core/subvolume.c`, `core/drop.c` |
| Shared references / audit | Linux CoW reference rules, inline/keyed placement and ordering, FULL_BACKREF conversion; independent whole-filesystem reference audit | `core/backref.c`, `tests/references.c` |
| Persistence model / Linux oracle | Source-controlled scenarios; fault sweeps; prefix, reorder and sector-tear epochs; recovery of every state; exported cases checked by Linux fsck, mount and `btrfs rescue super-recover` | `tests/transaction.c`, `tests/scenario_*.c`, `tests/prepare_transactions_linux.py`, `tests/transaction_oracle.sh` |
| Native boundary | Stable `(tree,inode)` identities, user xattrs, ACL rejection, XNU UBC/strategy, zlib and range device I/O | `adapters/common`, `adapters/xnu`, `adapters/fskit` |

The volume publishes each completed operation's private view to live mount
readers under its reader/writer protocol. Stable committed-view consumers retain
their own immutable roots; never redirect them into private trees. Reservations
are fresh for the transaction, and released slots stay pinned until its allocator
is destroyed. `accept` is legal only after successful durable publication;
`seal` alone is not a commit.

## Next changes, in dependency order

1. **Finish transaction and recovery acceptance.** The scenario suite, epoch
   device model, explicit superblock recovery, metadata exhaustion and damaged
   allocation maps are accepted (see ACCEPTANCE.md), including three copies
   with every tear combination across two secondaries, NO_SPACE from the commit's
   accounting fixed point, and explicit recovery of image files
   (`btrfs-inspect IMAGE recover`), and crash states Linux wrote (recorded
   with dm-log-writes, replayed and recovered here). Read-write native mounts
   recover disagreeing superblock copies before they open, and both adapters
   pass native power cuts; read-only mounts write nothing. Linux tree logs are
   replayed in one transaction (`core/replay.c`) as Linux's mount does, and
   writable mounts replay before they open; malformed logs, faults, power cuts
   and Linux's checks of the replayed images pass. Remaining: writing a tree
   log for fsync, which needs its own writer, a full-commit fallback and Linux
   replaying the logs written here. Extend every new writer feature with scenarios in `tests/scenario_*.c`,
   add its operations to the randomized model in `tests/scenario_random.c`, and
   export both to the Linux oracle.
2. **Extend shared references.** CoW of shared blocks follows Linux's
   `update_ref_for_cow` with inline and keyed references (`core/backref.c`), and
   the independent audit in `tests/references.c` checks every committed state.
   Deleted subvolumes are dropped as `btrfs_drop_snapshot` does (`core/drop.c`).
   Remaining: relocation trees stay unsupported; data reference edits from file
   writes must join the same
   ordered pass with additions before drops. Keep running the audit after every
   new writer feature and keep the Linux oracle on the shared and keyed profiles.
3. **Extend file data.** `btrfs_transaction_write`/`_truncate` write CoW data
   with checksums, holes, preallocated and compressed input, inline conversion
   and snapshot-safe frees (`core/data.c`, `core/csum.c`), in place into
   preallocated and unshared NODATACOW extents, with hole items on filesystems
   without NO_HOLES and both copies of DUP data, and fallocate as Linux's
   `btrfs_fallocate` (allocation, zeroing and punching; `F_PREALLOCATE` and
   `F_PUNCHHOLE` natively). The kernel adapter compresses with the kernel's
   deflate and the shared freestanding Zstd encoder. New data is written as
   its extents are created, and files compress on write as Linux decides.
   fs-verity files are verified on every read, keep their data on writable
   volumes, and fs-verity is enabled in bounded steps whose interruption orphan
   cleanup undoes, as Linux does (`core/verity.c`). Remaining: a 64 KiB-sector
   fixture, and LXNU's FS_IOC_ENABLE_VERITY and keyring policy for builtin
   signatures.
4. **Finish allocation features.** The free-space tree is verified and kept in
   step with every allocation (`core/fst.c`), the editor merges underfull
   siblings, and data and metadata chunks grow from unallocated device space
   (`core/space.c`, `bt_tx_publish_chunks`); groups convert between free extent
   items and bitmaps at Linux's thresholds; system chunks grow through the
   superblock's system array and unused groups are removed by an explicit
   cleaner pass (`core/group.c`), which the native volume runs after a
   commit that released space. Admission keeps metadata for the commit, for
   data written afterwards and, as Linux's global reserve, for deletions;
   truncation, eviction and orphan cleanup run in bounded steps.
   Mixed groups, the block-group tree and a metadata UUID are written, a v1
   space cache is left stale for Linux to rebuild (ARCHITECTURE.md, Format
   features), and qgroups are accounted as Linux's full mode accounts them
   (`core/qgroup.c`, ARCHITECTURE.md, Quotas), including rescans, Linux's
   metadata reservations against limits and simple quotas. Remaining:
   fixtures with sectors above 4 KiB.
5. **Extend namespace mutations.** Create of every type, link, unlink/rmdir with
   orphan items for open inodes, eviction and orphan cleanup, atomic rename
   (replacement, cross-directory, between names of one inode), xattrs and the
   `btrfs.compression` property update all coupled records in one transaction
   (`core/namespace.c`); real CRC32C collisions, numbering limits, refusals and
   crash states are covered and Linux agrees (see ACCEPTANCE.md), including
   names held in extended inode references. `btrfs_counters` keeps inode numbers
   and directory indexes unique across a mount's transactions; the native volume
   layer attaches them. Subvolumes and snapshots are created and deleted
   (`core/subvolume.c`), and the reader resolves unreferenced subvolume entries
   to stubs; native mounts drop deleted subvolumes and continue a running quota
   rescan in bounded steps from their commit ticks (`btrfs_volume_maintain`).
   O_TMPFILE files, RENAME_EXCHANGE
   and RENAME_WHITEOUT follow Linux, subvolume entries rename and exchange
   across subvolumes as Linux moves them, and inode flags change as Linux's
   `FS_IOC_SETFLAGS` (`btrfs_transaction_set_fsflags`; `chflags` natively).
   Truncation orphans of pre-3.12 kernels are dropped as Linux does. Native writers supply time, mode, owner,
   set-id and ACL decisions.
6. **Connect native writers.** Define versioned operation views, read pins,
   publication locks and UBC/FSKit dirty-page ownership first. Supply real exact
   write and durable flush callbacks, order pageout/truncate/invalidate/fsync,
   and authorize using actual credentials. Clear security metadata only through
   the owning transaction contract; no root impersonation or post-write repairs.
   Existing EROFS paths stay until mounted write suites pass.
7. **Complete FSKit/LXNU policy and release acceptance.** The signed module and its
   device barrier pass the mounted suites on stock macOS 26.5.2 except set-id
   metadata (ACCEPTANCE.md); a distribution (Developer ID, notarized) build and
   macOS 27 remain.
   Add native/LXNU ACL, capability, immutable/append and set-id contracts at the
   owning boundary. Linux namei and object provenance belong to the XNU fork's
   `bsd/lxnu/vfs` and `bsd/lxnu/xnu`. Run existing Linux conformance and native
   regressions before changing the ABI matrix.

## Executable gates for writable mounts

`tests/mounted_write.c` (run by `tests/run_macos.py` in both commit modes)
requires exclusive create, unaligned overwrite, mmap/pread/pwrite coherence,
`F_PREALLOCATE` and `F_PUNCHHOLE` (sizes, contents and allocated blocks),
shrink/grow zeroing, metadata/xattr mutations, links, rename replacement,
cross-directory rename, nonempty-directory failure, open-unlink lifetime,
file/directory fsync, remount persistence, concurrent append/rename, set-id
writes and full-disk behavior. It passes on the loaded XNU module with Linux
verification of every written image; `tests/mounted_contracts.py --suite write`
has not run for this driver. Missing contracts must fail, not be removed or
blanket-skipped. `tests/power_cut_macos.py` kills the guest while the loaded
module writes and requires every acknowledged fsync to survive; it passes for
the XNU adapter and, with `--adapter fskit` on stock macOS, for FSKit.

For each operation sequence and each device fault:

- Start with a Linux-verified baseline and an expected logical manifest.
- Retain possibly durable states, including ambiguous errors and torn writes.
- Recover with both implementations using a documented recovery choice. Run
  `btrfs check --readonly` and compare data, links, xattrs, snapshots and space
  accounting. Never use repair to manufacture a pass.
- Require every acknowledged fsync to survive. An unacknowledged transaction may
  resolve to an allowed old/new state; it must not mix roots or free live extents.
- Reduce every failure to a deterministic test. Record skipped unsupported
  recovery modes separately from successful recovery.

The portable model covers every prefix and seeded reorder/tear states between
persistence barriers, followed by explicit recovery where needed. The native
power-cut runner checks both adapters' flush and recovery on real guests.

## Performance work

Preserve read budgets and the absence of a mount-wide read lock. Private fixed-size
replacements update one payload/pointer without repacking the whole node, and
edits that fit an already packed leaf move only what follows the edited slot;
the first CoW recognizes a packed source leaf without rewriting it. Packed
equal-size key changes move only the interval between old and new slots, leaving
occupancy constant; cross-leaf or resized changes retain delete/insert semantics.
Free-space edge trims and adjoining releases often change just the existing key.
Packed occupancy is constant-time, and exact lookups on entirely private paths avoid
cursor setup while keeping identity and ancestor-bound checks. Repeated edits
reuse dirty paths. Free-space accounting normalizes a bounded copy of each
round's allocation log, cancels exact allocation/release pairs, merges adjacent
ranges and updates group info once per batch. Preserve the original ordered
allocator log and the additions-before-drops order of shared references.
The allocator starts with 256 gap records and grows within an explicit bound.
Keep allocation and I/O counts visible in tests.

Follow PERFORMANCE.md for matched Linux comparisons; `btrfs-bench` measures
per-operation costs portably. The core has a bounded, generation-aware node
cache that cursors read in place and that commits fill with their published
nodes; native adapters must create one per mounted device (with their own lock)
and drop it when anything but their commits changes the device. Data checksums
run in parallel CRC lanes, and aligned reads land in the caller's buffer. The
volume groups operations into one running transaction
([group commit](GROUP_COMMIT.md)); a native adapter must use it to avoid three
barriers per operation, and its acceptance needs mounted runs in both commit
modes plus power-cut checks of `fsync` boundaries. Writable admission loads
block groups as allocation reaches them, and the chunk table follows the
chunks present, so large volumes mount without a full extent-tree pass;
large-volume mount scaling still needs separate measurements. Repeated
subvolume-root resolution and attribute-rich enumeration also need profiling.
Native numbers of the mounted subvolume are its inode numbers and cost no
allocation; the XNU node hash grows with the system's vnode limit. On installed
FSKit a whole-volume walk (readdir and lstat of each name) reads about 7,500
names per second; the extension is busy for about a fifth of it, the rest being
FSKit's lookup and attribute requests per name (PERFORMANCE.md).
Do not claim a speed win from a userspace image reader versus a mounted guest.

The commit-cost batch in PERFORMANCE.md reduces image-backend CPU per single
create/commit by 22–31% and per grouped create by 33–35% on four profiles, including
102,042 starting extents. It does not establish mounted or durable latency.
Accepted editors now transfer buffers into the cache at destruction and reuse
retired buffers. Sole-owner metadata CoW moves its inline extent record directly
to the replacement block. A volume prepares the next immutable view before
publication and receives it from commit without mounting again, saving one
superblock read per commit. General extent-reference accounting and free-space
fixed-point rounds remain candidates. Writable FSKit mounts now cache their
own superblock copies under exclusive device ownership, eliminating repeated
device reads at begin/commit while retaining the core checks. The mounted
file+directory-fsync workload improves by 22–24% across seven paired trials.
XNU now combines adjacent device writes before each barrier, and volume sync
drains both the cluster-write queue and UBC pages before a group commit. Write
strategy maps outgoing UPL ranges read-only: writable kernel mappings on arm64
mark pages modified during writeback itself, causing repeated writes and full
commits at later fsync and reclaim. Creating 1,000 files and unmounting normally
now takes a median 0.676 s instead of 30.520 s across three runs per build;
100,000 files take 13.314 s in one verified run. Single-file fsync throughput
in XNU is essentially unchanged; barriers still dominate. Preserve all three
publication barriers;
the current sector-tear model permits every superblock copy to tear if their
writes share one unbarriered epoch.
Do not cancel reference additions/drops without preserving snapshot and
FULL_BACKREF transitions.
Writing a tree log for fsync needs its own writer, power-cut model and a Linux
oracle that replays logs written here; replay already follows Linux. Hardware CRC uses
general registers; arm64e kernel builds enforce `-mgeneral-regs-only`, since
the adapter does not own SIMD state even for compiler-generated copies.

At delivery, report portable contracts, actual loaded native mounts, FSKit,
LXNU policy, recovery/durability and comparative performance separately. Preserve
all unresolved tests and keep this document focused on remaining work.
