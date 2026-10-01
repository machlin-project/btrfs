# Acceptance

The portable reader, four-profile read-only XNU mount and a narrow CoW writer
have executable acceptance. General writable mounts, installed FSKit, Linux ABI
conformance and a performance win over Linux remain open.

## Established evidence

| Layer | Result | Reproduction / generated evidence |
| --- | --- | --- |
| Linux fixtures | Ten independently created images; Linux contents and read-only fsck pass | `tests/prepare_linux.py`; `logs/linux-reference-*.log` |
| Portable reader | 312 contracts across six read profiles; image hashes unchanged | `tests/check_images.py`; `logs/acceptance-tests.log` |
| Portable acceptance | Eleven Meson test processes pass under ASan/UBSan | `make test -Dfixtures=...` via DEVELOPMENT.md |
| Private CoW editor | Independent ordered model; 4/16/64 KiB nodes; root growth/collapse; three-way variable-item split; snapshot isolation; reservation/allocation/read failures | `tests/mutable.c` |
| Inline transactions | Five scenarios (replace, zero-length, 2048-byte, 20-inode batch, two repeated commits) on 4 KiB single, 16 KiB DUP, 64 KiB DUP and full/fragmented 4 KiB profiles; every allocation/read/write/barrier fault point of each last commit; three barriers per commit | `tests/transaction.c`; `logs/transaction-export-*.log` |
| Persistence and recovery model | 4,443 crash states: every prefix, 32 seeded reorder/tear states per metadata epoch and six tear patterns per superblock epoch; 255 need and pass explicit recovery; all resolve to the acknowledged or new stage and admit the next transaction | `tests/transaction.c` |
| Copies, allocation maps, exhaustion | Stale primary/secondary rejection; disagreeing copies block admission; recovery refuses rollback, foreign copies and pending logs; ten checksum-correct damaged allocation maps (nine with 4 KiB nodes) rejected before writes; full metadata returns NO_SPACE with no write | `tests/transaction.c` |
| Independent writer oracle | 848 exported states over four profiles; Linux fsck, primary generation and exact contents agree for each; 212 recovery states agree with `btrfs rescue super-recover` and with this recovery's writes; Linux then commits read-write | `tests/prepare_transactions_linux.py`; `logs/linux-transactions-*.log` |
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

Admission requires skinny metadata, at least two superblock copies that agree
with the mounted primary, and rejects read-only compatible features (including
the free-space tree), mixed groups, metadata UUID and quotas. Only existing
uncompressed inline regular files in tree 5 can be replaced, up to 2 KiB, many
per transaction; zero length removes their extent. Other trees, external-extent conversion, set-id,
immutable and append-only inodes are unsupported. Adapters still owe credential,
ACL and capability policy. Shared/full-backreference paths are rejected before
media writes; unrelated snapshots remain intact in the independent oracle.

The allocator validates in one ordered pass that every extent lies inside a chunk
and that block-group totals match, excludes all superblock stripes and rejects
physical chunk aliases. It pins the committed allocation map throughout a
transaction. CoW extent references, block groups and root items reach a bounded
fixed point before any write. New metadata is emitted bottom-up and flushed;
secondary copies are written and flushed; the primary is written and flushed.
The current root set is recorded in the rotating backup array. Any persistence
failure prevents further use of the transaction.

Recovery is explicit: mount still reads only the primary and fails on a torn
copy. `btrfs_recover_supers` selects the newest valid copy at or above the
caller's acknowledged generation and rewrites only disagreeing copies. The crash
model assumes a device that persists issued writes in any order and any sector
subset between successful barriers, and nothing after a failed barrier. It
covers only these inline scenarios and devices with two copies; actual native
flush semantics, three-copy devices, tree-log replay and shared references are
outside this evidence. Old backup roots are recovery hints, not permanently
pinned snapshots. General writes must not be enabled until HANDOFF.md's
remaining durability gates pass.

## Explicitly unfinished

General data allocation/writeback, shared and delayed references, underfull sibling
merge/rebalance, free-space tree maintenance, quotas, native read/write views and
UBC/FSKit coherence; create/mkdir/link/unlink/rename/xattr mutations and orphan
recovery; adapter use of superblock recovery and tree-log replay; LZO/alternate checksums/RAID;
full Linux authorization semantics; provisioning and mounted FSKit acceptance;
matched Linux performance measurements.
