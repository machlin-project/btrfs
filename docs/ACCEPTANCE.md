# Acceptance

The portable reader, four-profile read-only XNU mount and a narrow CoW writer
have executable acceptance. General writable mounts, installed FSKit, Linux ABI
conformance and a performance win over Linux remain open.

## Established evidence

| Layer | Result | Reproduction / generated evidence |
| --- | --- | --- |
| Linux fixtures | Seven independently created images; Linux contents and read-only fsck pass | `tests/prepare_linux.py`; `logs/linux-reference-*.log` |
| Portable reader | 312 contracts across six read profiles; image hashes unchanged | `tests/check_images.py`; `logs/acceptance-tests.log` |
| Portable acceptance | Eight Meson test processes pass under ASan/UBSan | `make test -Dfixtures=...` via DEVELOPMENT.md |
| Private CoW editor | Independent ordered model; 4/16/64 KiB nodes; root growth/collapse; three-way variable-item split; snapshot isolation; reservation/allocation/read failures | `tests/mutable.c` |
| Inline transaction | 10 writes, 2 barriers, 53 allocations, 26 reads in the accepted small-node fixture; failure sweeps, abort/no-op and zero-length replacement | `tests/transaction.c`; `logs/transaction-accepted-plan.log` |
| Persistence model | Every whole-write prefix; seeded partial metadata persistence and super-mirror subsets; a torn primary is explicitly rejected | `tests/transaction.c`; `logs/transaction-persistence-tests.log` |
| Independent writer oracle | All 11 whole-write prefix images pass Linux fsck and mounted contents; Linux subsequently mounts read-write, writes and passes fsck | `tests/prepare_transactions_linux.py`; `logs/linux-transactions-*.log` |
| Native identities / policy | 65,536 distinct identities, capacity failure, user-xattr filtering and malformed lists pass | `tests/identity.c`, `tests/native_policy.c` |
| Corruption / concurrency | Adversarial/fault/budget tests and eight concurrent readers pass; prior reader TSAN and bounded fuzz runs passed | `tests/adversarial.c`, `tests/concurrent.c`, `logs/fuzz-final.log` |
| Freestanding core | Stack frames bounded to 2 KiB, including writer | Meson freestanding target |
| Native compilation | arm64e and x86_64 kexts, unsigned FSKit application/extension pass; no unresolved owned symbols | `logs/kext-final-*.log`, `logs/fskit-final.log` |
| Loaded XNU and mounted reads | Actual custom guest kernel/module identities verified; plain, 4 KiB nodes, 64 KiB nodes and zlib profiles pass; media hashes unchanged | Lab `artifacts/btrfs-kext/` reports; `tests/run_macos.py`, `tests/mounted.c` |
| Installed FSKit | Not accepted; compilation only | Actual module signing/registration and mounted tests remain |
| LXNU ABI semantics | Not run for Btrfs | No XNU-fork hooks or ABI matrix completion claimed |
| Native durable writes / comparative performance | Not accepted | Native writers remain disabled; no matched Linux benchmark |

Source revisions belong to Git. Kernel/module UUIDs, artifact hashes, precise
commands and individual observations live in generated reports. The 312 reader
contracts are assertions within one test process, not 312 Meson tests. Missing
configured fixtures fail acceptance.

## Read contract

Supported: one device, CRC32C, SINGLE/DUP chunks, primary superblock, default or
explicit subvolume, validated metadata, raw names, hardlinks, symlinks, directory
streams and parent traversal. Data coverage includes inline/regular/shared-offset
extents, holes, preallocation, EOF and files beyond 32 bits. POSIX supplies zlib
and Zstd; native adapters supply zlib. Unsupported codecs fail explicitly.

Native guest tests require user xattr size/get/list/short-buffer behavior, distinct
subvolume identities, directory rewind/seek cookies, mmap after descriptor close,
private mmap isolation, sparse data, eight concurrent readers, allowed and denied
ordinary UID access, and EROFS. Core raw xattrs remain separate from native policy.
Native adapters expose only `user.*` names fitting the Darwin name limit and reject
inodes with POSIX access ACLs until translation/authorization exists. This rejection
is not ACL support or complete multi-user policy.

Data-DUP, NODATASUM, mixed groups, metadata UUID and 64 KiB sectors need a wider
independent fixture matrix. Successful format admission alone is not coverage.
Native tiny-buffer pagein failures, forced unmount/reclaim and FSKit lifetime
stress remain open.

## Writer contract and current boundary

`include/btrfs/write.h` grants write authority separately from the immutable read
environment. The caller owns exclusive resource access and drains readers before
commit. The base mount must be retired after successful or uncertain persistence.
Neither native adapter supplies write callbacks yet.

Admission requires skinny metadata and rejects read-only compatible features
(including the free-space tree), mixed groups, metadata UUID and quotas. Only
existing uncompressed inline regular files in tree 5 can be replaced, up to 2 KiB;
zero length removes their extent. Other trees, external-extent conversion, set-id,
immutable and append-only inodes are unsupported. Adapters still owe credential,
ACL and capability policy. Shared/full-backreference paths are rejected before
media writes; unrelated snapshots remain intact in the independent oracle.

The allocator validates occupied extents against block-group accounting, excludes
all superblock stripes and rejects physical chunk aliases. It pins the committed
allocation map throughout a transaction. CoW extent references, block groups and
root items reach a bounded fixed point before any write. New metadata is emitted
bottom-up, flushed, followed by secondary/primary superblocks and a second flush.
The current root set is recorded in the rotating backup array. Any persistence
failure prevents further use of the transaction.

The crash evidence is deliberately bounded. Linux independently checks every
whole-write prefix for the inline replacement. Seeded partial/reordered metadata
and super-mirror persistence are checked in the portable model. A torn primary
currently returns corruption; automatic mirror selection and recovery are not
implemented. This is rejection coverage, not proof of production crash recovery.
Old backup roots are recovery hints, not permanently pinned snapshots. General
writes must not be enabled until HANDOFF.md's remaining durability gates pass.

## Explicitly unfinished

General data allocation/writeback, shared and delayed references, underfull sibling
merge/rebalance, free-space tree maintenance, quotas, native read/write views and
UBC/FSKit coherence; create/mkdir/link/unlink/rename/xattr mutations and orphan
recovery; superblock recovery and tree-log replay; LZO/alternate checksums/RAID;
full Linux authorization semantics; provisioning and mounted FSKit acceptance;
matched Linux performance measurements.
