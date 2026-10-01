# Handoff for continued implementation

Continue this repository on `development`. The portable reader and read-only XNU
mount work; a real CoW transaction implementation can replace inline files and
produce images independently accepted by Linux. Preserve these implementations
and extend their contracts. Native writable mounts are deliberately still disabled.

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

Require sixteen passing test processes and all six reader profiles (312
contracts). Eight writable images are required by the transaction suites:
`transactions` (4 KiB single), `transactions-dup` (16 KiB DUP),
`transactions-large` (64 KiB DUP), `transactions-full` (128 MiB with full,
fragmented metadata), `transactions-shared` (snapshots, reflinks, offset
references), `transactions-keyed` (keyed backreferences), `transactions-data`
(file data inputs with snapshots) and `transactions-fst` (mkfs defaults with a
free-space tree in extent and bitmap form). Missing fixtures
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
| Superblock recovery | Explicit newest-valid-copy selection with acknowledged floor, log/foreign-copy refusal, selection validation, rewrite of disagreeing copies | `core/recovery.c`, `include/btrfs/write.h` |
| Free-space tree | Verification against the extent tree, logged allocation changes applied in the fixed point, extents and bitmaps | `core/fst.c`, `core/space.c` |
| File data / checksums | CoW writes and truncation, drop-extents splitting, staged data, private read view, checksum items | `core/data.c`, `core/csum.c` |
| Shared references / audit | Linux CoW reference rules, inline/keyed placement and ordering, FULL_BACKREF conversion; independent whole-filesystem reference audit | `core/backref.c`, `tests/references.c` |
| Persistence model / Linux oracle | Source-controlled scenarios; fault sweeps; prefix, reorder and sector-tear epochs; recovery of every state; exported cases checked by Linux fsck, mount and `btrfs rescue super-recover` | `tests/transaction.c`, `tests/prepare_transactions_linux.py` |
| Native boundary | Stable `(tree,inode)` identities, user xattrs, ACL rejection, XNU UBC/strategy, zlib and range device I/O | `adapters/common`, `adapters/xnu`, `adapters/fskit` |

The mutation view supports metadata traversal; it does not magically update all
cached root descriptors or provide a live native read/write mount. Reservations
are fresh for the transaction, and released slots stay pinned until its allocator
is destroyed. Never route committed readers into private trees. `accept` is legal
only after successful durable publication; `seal` alone is not a commit.

## Next changes, in dependency order

1. **Finish transaction and recovery acceptance.** The scenario suite, epoch
   device model, explicit superblock recovery, metadata exhaustion and damaged
   allocation maps are accepted (see ACCEPTANCE.md). Remaining: devices large
   enough for a third copy (tear combinations across two secondaries), NO_SPACE
   raised inside the commit's accounting fixed point rather than an edit, a
   Linux-written crash state (Linux as writer, this implementation recovering),
   and adapter use of recovery: report RECOVERY_REQUIRED with the dry-run
   decision, persist the acknowledged generation, and never recover implicitly at
   mount. Extend every new writer feature with scenarios in `tests/transaction.c`
   and export them to the Linux oracle.
2. **Extend shared references.** CoW of shared blocks follows Linux's
   `update_ref_for_cow` with inline and keyed references (`core/backref.c`), and
   the independent audit in `tests/references.c` checks every committed state.
   Remaining: relocation trees and snapshot deletion (dead roots, drop progress)
   stay unsupported; data reference edits from file writes must join the same
   ordered pass with additions before drops. Keep running the audit after every
   new writer feature and keep the Linux oracle on the shared and keyed profiles.
3. **Extend file data.** `btrfs_transaction_write`/`_truncate` write CoW data
   with checksums, holes, preallocated and compressed input, inline conversion
   and snapshot-safe frees (`core/data.c`, `core/csum.c`). Remaining: in-place
   preallocation conversion and NODATACOW overwrite for unshared extents (both
   need their own crash cases), compression on write, data DUP and 64 KiB-sector
   fixtures, explicit hole items for filesystems without NO_HOLES, and lifting
   the 64 MiB staging bound with streaming writeback once native writers exist.
4. **Finish allocation features.** The free-space tree is verified and kept in
   step with every allocation (`core/fst.c`), and the editor merges underfull
   siblings. Remaining: block-group growth (chunk, device-extent, device-item,
   block-group and free-space info creation, with system-array updates for
   system chunks) with its own fixture; extent/bitmap conversion at Linux's
   thresholds; the v1 space cache (`cache_generation` is currently invalidated)
   needs its own fixture; quotas, mixed groups, metadata UUID and the
   block-group tree stay rejected until each is implemented and tested.
5. **Add namespace mutations.** Create/mkdir, link/unlink, symlink, atomic rename
   and xattrs must update all coupled inode refs, DIR_ITEM collision records,
   DIR_INDEX cookies, link counts, parent metadata and orphan state in one
   transaction. Cover duplicate keys, real name-hash collisions, index exhaustion,
   rename replacement and cross-directory operations, open-unlink and crash replay.
6. **Connect native writers.** Define versioned operation views, read pins,
   publication locks and UBC/FSKit dirty-page ownership first. Supply real exact
   write and durable flush callbacks, order pageout/truncate/invalidate/fsync,
   and authorize using actual credentials. Clear security metadata only through
   the owning transaction contract; no root impersonation or post-write repairs.
   Existing EROFS paths stay until mounted write suites pass.
7. **Complete FSKit/LXNU policy and release acceptance.** Provision/register the
   actual FSKit module and run its mounted suite; an unsigned build is insufficient.
   Add native/LXNU ACL, capability, immutable/append and set-id contracts at the
   owning boundary. Linux namei and object provenance belong to the XNU fork's
   `bsd/lxnu/vfs` and `bsd/lxnu/xnu`. Run existing Linux conformance and native
   regressions before changing the ABI matrix.

## Executable gates for writable mounts

`tests/mounted_contracts.py --suite write --disposable-guest` already requires
exclusive create, unaligned overwrite, mmap/pread/pwrite coherence, shrink/grow
zeroing, metadata/xattr mutations, links, rename replacement, cross-directory
rename, nonempty-directory failure, open-unlink lifetime and file/directory fsync.
It has not passed for this driver. Missing contracts must fail, not be removed or
blanket-skipped. Extend with remount persistence, concurrent append/rename and
full-disk behavior.

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

The portable model covers every prefix, seeded reorder/tear states of the


## Performance work

Preserve read budgets and the absence of a mount-wide read lock. Private fixed-size
replacements update one payload/pointer without repacking the whole node; repeated
edits reuse dirty paths. The allocator starts with 256 gap records and grows within
an explicit bound. Keep allocation and I/O counts visible in tests.

Follow PERFORMANCE.md for matched Linux comparisons. Remaining likely costs are
repeated subvolume-root resolution, per-call cursors, attribute-rich enumeration,
metadata caching, full-extent-map scanning at transaction begin, and rebuilding
allocation state for each transaction. Optimize after measurement. Add a bounded,
generation-aware cache with documented lifetime rules if it pays for itself.
Do not claim a speed win from a userspace image reader versus a mounted guest.

At delivery, report portable contracts, actual loaded native mounts, FSKit,
LXNU policy, recovery/durability and comparative performance separately. Preserve
all unresolved tests and keep this document focused on remaining work.
