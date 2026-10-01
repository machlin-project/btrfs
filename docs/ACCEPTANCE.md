# Acceptance

The portable reader, four-profile read-only XNU mount and a narrow CoW writer
have executable acceptance. General writable mounts, installed FSKit, Linux ABI
conformance and a performance win over Linux remain open.

## Established evidence

| Layer | Result | Reproduction / generated evidence |
| --- | --- | --- |
| Linux fixtures | Sixteen independently created images; Linux contents and read-only fsck pass | `tests/prepare_linux.py`; `logs/linux-reference-*.log` |
| Portable reader | 312 contracts across six read profiles; image hashes unchanged | `tests/check_images.py`; `logs/acceptance-tests.log` |
| Portable acceptance | Eighteen Meson test processes pass under ASan/UBSan | `make test -Dfixtures=...` via DEVELOPMENT.md |
| Private CoW editor | Independent ordered model; 4/16/64 KiB nodes; root growth/collapse; three-way variable-item split; snapshot isolation; reservation/allocation/read failures | `tests/mutable.c` |
| Transactions | Five inline scenarios (replace, zero-length, 2048-byte, 20-inode batch, two repeated commits) on seven writable profiles, plus six shared-block, three keyed-reference, eleven data and eleven namespace scenarios; allocation/read/write/barrier fault points of each last commit (all up to 512 per class, a deterministic stride beyond); three barriers per commit | `tests/transaction.c`; `logs/data-export-*.log` |
| File data | Eleven data scenarios: unaligned overwrite splitting a reflinked extent, append, writes into holes and past EOF, preallocated and zlib extents, NODATASUM, inline conversion, two-step truncation, snapshot overwrites that free an extent, overlapping writes in one transaction; metadata and data exhaustion return NO_SPACE without writes | `tests/transaction.c` (`--data`, `--full`) |
| Namespace mutations | Eleven scenarios on a Linux image with real CRC32C name and xattr collisions, extended references and compression properties: every object type with inherited flags, 100 names splitting leaves, appends to, cuts from and renames within packed collision items, a DIR_ITEM filled to the largest item, hard links (including beside extended references), unlinks of shared and last data references, renames across directories and over files and an empty directory, open unlinks with orphans, eviction and orphan cleanup, the largest xattr, a subvolume tree, and property inheritance recording a new incompat feature; about forty refusals and four numbering/packing limits decided before any change; every state matches its expected namespace facts and passes the independent namespace audit, which agrees with all sixteen Linux images and detects wrong link counts and directory sizes | `tests/scenario_namespace.c` (`--namespace`), `tests/namespace_audit.c` |
| Free-space tree | Default-feature images (16 KiB DUP and 4 KiB nodes, extents and bitmaps): every committed and crash-resolved state is compared with the extent tree at the next admission; freeing between holes merges bitmap runs; a 2 MiB write fills the remaining extent-mode space and then bitmap holes | `tests/transaction.c` (`--fragment`), `core/fst.c` |
| Block-group growth | On a Linux image with nearly full metadata and unallocated space, growing 2,000 leaf-sized files allocates a DUP metadata chunk and a 40 MiB write allocates a data chunk; the next admission re-verifies device extents, chunk items, block groups and the free-space tree | `tests/transaction.c` (`--grow`), `core/space.c` |
| Tree rebalancing | Deleting 90% of 4,000 items merges underfull leaves and nodes: the tree drops from level 2 to 1 and keeps no adjacent pair of underfull edited leaves; disabling the merge fails the test | `tests/mutable.c` |
| Shared references | Owner and snapshot CoW of shared leaves and level-1 nodes, FULL_BACKREF creation, conversion and release, reflinked and offset data references, writes across three trees, inline and keyed references; the independent audit agrees with all sixteen Linux images and every committed state, and rejects damaged references; its checksum audit verifies containment, stored CRC32C values and coverage of written data | `tests/references.c`, `tests/audit.c`, `tests/transaction.c` |
| Persistence and recovery model | 34,865 crash states in the Meson suite: every prefix, 32 seeded reorder/tear states per metadata epoch (data writes included) and six tear patterns per superblock epoch; 1,341 need and pass explicit recovery; all resolve to the acknowledged or new stage and admit the next transaction | `tests/transaction.c` |
| Copies, allocation maps, exhaustion | Stale primary/secondary rejection; disagreeing copies block admission; recovery refuses rollback, foreign copies and pending logs; ten checksum-correct damaged allocation maps (nine with 4 KiB nodes) rejected before writes; full metadata returns NO_SPACE with no write | `tests/transaction.c` |
| Independent writer oracle | 5,166 exported states over twelve profiles (including `plain` and `small-nodes`, which keep Linux's default free-space tree), covering every shared, keyed, data, free-space, growth and namespace scenario; Linux fsck (extent references, checksum items and the fs-tree namespace included), primary generation and exact contents agree for each, with Linux verifying data checksums on read; on the namespace profile Linux also confirms 7,415 namespace facts (listings and sizes, link identity and counts, symlinks, xattrs, device numbers, inode flags via its own tree dump, incompat features), and after each scenario mounts read-write, cleans the orphans left and passes fsck with none remaining; 1,244 recovery states agree with `btrfs rescue super-recover` and with this recovery's writes; Linux then commits read-write | `tests/prepare_transactions_linux.py`, `tests/transaction_oracle.sh`; `logs/linux-transactions-*.log` |
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

Admission requires skinny metadata and at least two superblock copies that agree
with the mounted primary. It accepts a valid free-space tree that agrees with
the extent tree and rejects other read-only compatible features (block-group
tree, verity), mixed groups, metadata UUID and quotas. Only existing
uncompressed inline regular files can be replaced, up to 2 KiB, and regular
files can be written and truncated as copy-on-write data (NO_HOLES filesystems,
at most 64 MiB of new data per transaction), many per transaction, in the top
level, subvolumes and writable snapshots. Read-only snapshots return READ_ONLY.
Set-id, immutable and append-only inodes, dead subvolumes, old backreference
formats, explicit hole items, compression on write and in-place preallocation
conversion are unsupported. Adapters still owe credential, ACL and
capability policy. CoW of shared blocks follows Linux's snapshot reference rules;
inline and keyed references, FULL_BACKREF conversion and release are covered,
and every committed state passes the independent reference audit.
Namespace operations create every object type, link, unlink (keeping open
inodes as orphans), evict, clean orphans, rename and edit xattrs, including the
`btrfs.compression` property, in any writable file tree; every committed state
passes the independent namespace audit. Names held only in extended references,
subvolume and snapshot creation or deletion are unsupported.

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

Native data writeback, relocation trees and snapshot deletion, system-chunk
growth, chunk removal and balance, free-space representation conversion, the v1 space cache, quotas, native read/write views and
UBC/FSKit coherence; extended inode reference edits, subvolume and snapshot
creation/deletion and cross-transaction directory index counters; adapter use of
superblock recovery and tree-log replay; LZO/alternate checksums/RAID;
full Linux authorization semantics; provisioning and mounted FSKit acceptance;
matched Linux performance measurements.
