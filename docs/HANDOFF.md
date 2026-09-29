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

Require eight passing test processes and all six reader profiles (312 contracts).
The seventh image, `transactions.raw`, has 4 KiB nodes, single metadata and no
free-space tree/cache, and is required by `inline-transactions`. Missing fixtures
are failures. Recreate them in a disposable Linux VM using DEVELOPMENT.md.

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
| Transaction owner | Inline replacement, exclusive resource contract, reference/accounting fixed point, root/backup updates, two barriers, terminal failures | `core/transaction.c`, `include/btrfs/write.h` |
| Persistence model / Linux oracle | Read/allocation/write/barrier sweeps, whole-write cuts, partial metadata and mirror subsets; Linux fsck/read/write validation | `tests/transaction.c`, `tests/prepare_transactions_linux.py` |
| Native boundary | Stable `(tree,inode)` identities, user xattrs, ACL rejection, XNU UBC/strategy, zlib and range device I/O | `adapters/common`, `adapters/xnu`, `adapters/fskit` |

The mutation view supports metadata traversal; it does not magically update all
cached root descriptors or provide a live native read/write mount. Reservations
are fresh for the transaction, and released slots stay pinned until its allocator
is destroyed. Never route committed readers into private trees. `accept` is legal
only after successful durable publication; `seal` alone is not a commit.

## Next changes, in dependency order

1. **Broaden transaction and recovery acceptance.** Extend the Linux oracle to
   zero-length replacement, maximum inline size, multi-inode batches, repeated
   commits, DUP metadata and large nodes. Add full/fragmented metadata ENOSPC,
   stale-superblock rejection and independently constructed corrupt allocation
   maps. Current whole-write crash plans are exported, not source-controlled.
   Add device models that tear superblocks and reorder writes between successful
   barriers, then test an explicit recovery policy against Linux. The current
   reader rejects a damaged primary; do not silently select an older mirror or
   call that rejection successful recovery. Preserve an acknowledged generation.
2. **Implement shared/delayed references.** `bt_tx_drop_original` intentionally
   accepts only one inline tree reference with matching owner/generation. Replace
   that admission with correct shared/full backrefs and child/data reference
   changes. Test shared leaves and internal nodes, retained snapshots, reflinks
   and extent-offset references. Linux fsck must report no lost references or
   accounting errors after every fault cut. Never just delete the rejection.
3. **Add data extents and checksums.** Extend allocator reservations beyond
   metadata; support new regular extents, unaligned read-modify-CoW, holes,
   preallocation conversion, truncation and compressed input as separate cases.
   Update inode size/nbytes, checksum ranges and data backrefs together. Keep the
   old extent pinned until publication and reader retirement. Preserve snapshot
   data at every cut. Do not clear integrity or durability options to improve speed.
4. **Maintain allocation features.** Add free-space tree/cache and block-group
   growth with their own Linux fixtures. Writable admission currently rejects
   free-space-tree/quotas/mixed groups/metadata UUID. Keep each rejection until
   the corresponding accounting is implemented and tested. Add underfull sibling
   merge/rebalance to the editor; current deletion removes empty nodes and
   collapses unary roots but leaves underfull siblings.
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

The portable model currently covers 64 seeded metadata persistence subsets and
all four super-mirror subsets, in addition to complete-write cuts. A torn primary
is an explicit error test. The independent Linux oracle currently covers the
whole-write cuts for one inline replacement, followed by a Linux read-write
commit. These are concrete starting tests, not complete crash consistency.

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
