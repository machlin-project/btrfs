# Acceptance

The portable reader, four-profile read-only XNU mount and a narrow CoW writer
have executable acceptance. General writable mounts, installed FSKit, Linux ABI
conformance and a performance win over Linux remain open.

## Established evidence

| Layer | Result | Reproduction / generated evidence |
| --- | --- | --- |
| Linux fixtures | Eighteen independently created images; Linux contents and read-only fsck pass | `tests/prepare_linux.py`; `logs/linux-reference-*.log` |
| Portable reader | 367 contracts across the six read profiles and the holes profile, whose split hole items Linux wrote with nonzero offsets; image hashes unchanged | `tests/check_images.py`; `logs/acceptance-tests.log` |
| Portable acceptance | Twenty-seven Meson test processes pass under ASan/UBSan | `make test -Dfixtures=...` via DEVELOPMENT.md |
| Private CoW editor | Independent ordered model; 4/16/64 KiB nodes; root growth/collapse; three-way variable-item split; snapshot isolation; reservation/allocation/read failures | `tests/mutable.c` |
| Transactions | Five inline scenarios (replace, zero-length, 2048-byte, 20-inode batch, two repeated commits) on eight writable profiles, plus six shared-block, three keyed-reference, eleven data (on two profiles), three hole, fourteen namespace and four subvolume scenarios; allocation/read/write/barrier fault points of each last commit (all up to 512 per class, a deterministic stride beyond); three barriers per commit | `tests/transaction.c`; `logs/data-export-*.log` |
| File data | Eleven data scenarios: unaligned overwrite splitting a reflinked extent, append, writes into holes and past EOF, preallocated and zlib extents, NODATASUM, inline conversion, two-step truncation, snapshot overwrites that free an extent, overlapping writes in one transaction; compression on write by property and by the zstd mount option (128 KiB extents kept when they save a sector, compressed and plain inline files, split and truncated compressed extents, the ZSTD feature, refusals by NOCOMPRESS and NODATASUM) and an inherited zstd property; writes in place into Linux's preallocated file (item split into preallocated, written and preallocated parts of one disk extent) and into an unshared NODATACOW extent whose crash states may hold old or new sectors, copied on write once a snapshot or another reference shares the extent (dropping either check fails the tests); on a Linux image without NO_HOLES and with DUP data, all data scenarios pass again and three hole scenarios cover growth by truncation and by writes past EOF with hole items, writes and truncation inside our and Linux's hole items (a 16 GiB one included), an inline file written far beyond its sector, and in-place writes on both copies (omitting the hole items, counting them in `nbytes` or skipping a copy fails the tests); 80 MiB written in one transaction holding under 16 MiB; metadata and data exhaustion return NO_SPACE without writes | `tests/transaction.c` (`--data`, `--full`) |
| Namespace mutations | Fourteen scenarios on a Linux image with real CRC32C name and xattr collisions, extended references and compression properties: every object type with inherited flags, 100 names splitting leaves, appends to, cuts from and renames within packed collision items, a DIR_ITEM filled to the largest item, hard links (including beside extended references), names beyond a full INODE_REF in new, colliding and Linux-written INODE_EXTREF items with unlinks, renames into and out of the INODE_REF item and an INODE_EXTREF item filled to the largest item, unlinks of shared and last data references, renames across directories and over files and an empty directory, open unlinks with orphans, eviction and orphan cleanup, the largest xattr, a subvolume tree, property inheritance recording a new incompat feature, attribute changes, and set-id files written with kept or dropped privileges; about forty namespace refusals, the immutable, append-only and set-id refusals, and five numbering/packing limits decided before any change; inode numbers and directory indexes stay unique across transactions with a mount's counters (and repeat without them, as after Linux evicts an inode); every state matches its expected namespace facts and passes the independent namespace audit, which agrees with all sixteen Linux images and detects wrong link counts and directory sizes | `tests/scenario_namespace.c` (`--namespace`), `tests/namespace_audit.c` |
| Subvolumes and snapshots | Four scenarios on the namespace image: subvolumes at the top level, in a directory and in another subvolume, with files, inheriting the parent subvolume's compression property; writable and read-only snapshots of a subvolume, of the multi-level top level, of a read-only snapshot and of a snapshot, edited on either side without leaking, with copied subvolume entries as empty stubs; deletion of subvolumes, snapshots and a stub entry with NOT_EMPTY, INVALID_ARGUMENT and NOT_FOUND refusals; the cleaner resuming a partial drop across commits, and fully dropping an unshared subvolume with data and one whose leaves a snapshot shares (23 blocks converted to FULL_BACKREF and 120 data references to parent references first); refusals, edits in the creating transaction, and deleted subvolumes in the reader (NOT_FOUND, stubs). Both audits run on every state and now check root references with their entries, the UUID tree against live roots, orphan items of deleted roots, references of partly dropped trees, and that keyed references name existing trees; skipping the FULL_BACKREF conversion or a leaf's file references fails them | `tests/scenario_subvolume.c` (`--subvolume`), `core/subvolume.c`, `core/drop.c` |
| Free-space tree | Default-feature images (16 KiB DUP and 4 KiB nodes, extents and bitmaps): every committed and crash-resolved state is compared with the extent tree at the next admission; freeing between holes merges bitmap runs; a 2 MiB write fills the remaining extent-mode space and then bitmap holes; groups convert at Linux's thresholds, which Linux itself crossed at exactly 158 and 56 free extents in a 112 MiB group of its fixture: an 8 MiB group becomes bitmaps when 100 alternate one-sector files go, and the 112 MiB group goes to bitmaps and back to extent items; every state keeps each group within the thresholds (dropping either conversion fails the tests) | `tests/transaction.c` (`--fragment`, `--convert`), `core/fst.c` |
| Block groups | On a Linux image with nearly full metadata and unallocated space, growing 2,000 leaf-sized files allocates a DUP metadata chunk and a 40 MiB write allocates a data chunk; on 4 KiB single and 16 KiB DUP images an emptied data group is removed as Linux's cleaner does (not in the transaction that emptied it or freed into it, never the last of its type), a later data chunk takes its device space back, and a chunk tree that finds no system space grows a system chunk through the superblock's system array, after which the emptied old system group leaves both; the next admission re-verifies device extents, chunk items, block groups and the free-space tree (dropping the device-extent deletion, either array update or the freed-group check fails the tests) | `tests/transaction.c` (`--grow`, `--groups`), `core/space.c`, `core/group.c` |
| Tree rebalancing | Deleting 90% of 4,000 items merges underfull leaves and nodes: the tree drops from level 2 to 1 and keeps no adjacent pair of underfull edited leaves; disabling the merge fails the test | `tests/mutable.c` |
| Shared references | Owner and snapshot CoW of shared leaves and level-1 nodes, FULL_BACKREF creation, conversion and release, reflinked and offset data references, writes across three trees, inline and keyed references; the independent audit agrees with all sixteen Linux images and every committed state, and rejects damaged references; its checksum audit verifies containment, stored CRC32C values and coverage of written data | `tests/references.c`, `tests/audit.c`, `tests/transaction.c` |
| Persistence and recovery model | 70,813 crash states in the Meson suite: every prefix, 32 seeded reorder/tear states per metadata epoch (data writes included) and six tear patterns per superblock epoch; 2,931 need and pass explicit recovery; all resolve to the acknowledged or new stage and admit the next transaction | `tests/transaction.c` |
| Randomized differential writer | Sixteen seeded plans of 24 operations in three commits (namespace, xattr, data, attribute operations and predicted refusals over real CRC32C name collisions) with sampled crash states on `transactions-namespace`, and 500 quick plans checking every committed stage on `transactions-dup`; a separate model predicts every stage, and both audits run in every state; removing the directory time update from removals fails the test | `tests/scenario_random.c` (`--random`, `--random-quick`) |
| Copies, allocation maps, exhaustion | Stale primary/secondary rejection; disagreeing copies block admission; recovery refuses rollback, foreign copies and pending logs; ten checksum-correct damaged allocation maps (nine with 4 KiB nodes) rejected before writes; full metadata returns NO_SPACE with no write | `tests/transaction.c` |
| Independent writer oracle | 5,356 exported states over twelve profiles (including `plain` and `small-nodes`, which keep Linux's default free-space tree), covering every shared, keyed, data, free-space, growth and namespace scenario; Linux fsck (extent references, checksum items and the fs-tree namespace included), primary generation and exact contents agree for each, with Linux verifying data checksums on read; on the namespace profile Linux also confirms 9,490 namespace facts (listings and sizes, link identity and counts, symlinks, xattrs, device numbers, inode flags and the INODE_REF or INODE_EXTREF item holding a name via its own tree dump, incompat features, file capabilities, owners and times), and after each scenario mounts read-write, cleans the orphans left and passes fsck with none remaining; the sixteen random plans (with the inline scenarios on the same profile) add 1,793 states and 66,464 namespace facts; the four subvolume scenarios add 666 states and 6,374 facts (flags and parent UUIDs from `btrfs subvolume show`, `btrfs subvolume list` with and without `-d`), and before its read-write check Linux's cleaner finishes the drops left partial (`btrfs subvolume sync`); with data written as extents are created and compression on write, the data profile (883 states, 1,242 facts) and the namespace profile (1,218 states, 9,818 facts) pass again, Linux decompressing our zlib and Zstd extents, inline ones included, and its own tree dump agreeing on their counts; with writes in place, the namespace profile (1,332 states, 10,540 facts) passes again, Linux's tree dump agreeing on the regular, preallocated and distinct disk extents of the preallocated and NODATACOW files, and every crash state of the in-place overwrite reading each 512-byte sector of the volatile range as old or new; on a Linux image without NO_HOLES and with DUP data, every data scenario and the three hole scenarios (1,216 states, 2,756 facts) pass, Linux's tree dump agreeing on hole item counts and `btrfs check --check-data-csum` verifying both copies after each scenario; with free-space conversions, the convert (342 states), namespace (1,408 states, 10,624 facts), free-space-tree (1,021) and growth (304) profiles pass, Linux's own dumps agreeing on which block groups keep bitmaps; with block group removal and system chunk growth, the 4 KiB single and 16 KiB DUP images (381 and 386 states) pass, Linux agreeing on chunk items, block group items and system array entries; 1,262 recovery states agree with `btrfs rescue super-recover` and with this recovery's writes; Linux then commits read-write | `tests/prepare_transactions_linux.py`, `tests/transaction_oracle.sh`; `logs/linux-transactions-*.log` |
| Native identities / policy | 65,536 distinct identities, capacity failure, user-xattr filtering and malformed lists pass | `tests/identity.c`, `tests/native_policy.c` |
| Corruption / concurrency | Adversarial/fault/budget tests and eight concurrent readers pass; prior reader TSAN and bounded fuzz runs passed | `tests/adversarial.c`, `tests/concurrent.c`, `logs/fuzz-final.log` |
| Native volume views | Pins keep committed root sets, the next writer waits for older views, empty/aborted transactions publish nothing, an uncertain commit fails the volume; under ASan/UBSan and TSan, four readers check every pinned view against the writer's per-generation model through 1,000 transactions with aborts, injected allocation failures and a failed final commit; removing the drain wait or the pin lock fails the test | `tests/volume.c` (`--stress`) |
| Freestanding core | Stack frames bounded to 2 KiB, including writer | Meson freestanding target |
| Native compilation | arm64e and x86_64 kexts, unsigned FSKit application/extension pass; no unresolved owned symbols | `logs/kext-final-*.log`, `logs/fskit-final.log` |
| Loaded XNU and mounted reads | Actual custom guest kernel/module identities verified; plain, 4 KiB nodes, 64 KiB nodes and zlib profiles pass; media hashes unchanged | Lab `artifacts/btrfs-kext/` reports; `tests/run_macos.py`, `tests/mounted.c` |
| Installed FSKit | Not accepted; compilation only | Actual module signing/registration and mounted tests remain |
| LXNU ABI semantics | Not run for Btrfs | No XNU-fork hooks or ABI matrix completion claimed |
| Native durable writes / comparative performance | Not accepted | Native writers remain disabled; no matched Linux benchmark |

Source revisions belong to Git. Kernel/module UUIDs, artifact hashes, precise
commands and individual observations live in generated reports. The 367 reader
contracts are assertions within one test process, not 367 Meson tests. Missing
configured fixtures fail acceptance.

## Read contract

Supported: one device, CRC32C, SINGLE/DUP chunks, primary superblock, default or
explicit subvolume, validated metadata, raw names, hardlinks, symlinks, directory
streams and parent traversal. A subvolume entry without its ROOT_REF (copied by a
snapshot, or deleted) is an empty stub directory and a deleted subvolume cannot be
opened, as on Linux. Data coverage includes inline/regular/shared-offset
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
files can be written and truncated (with hole items where NO_HOLES is absent;
new data reaches the device as its extents are created, with no bound per
transaction), compressed with zlib or Zstd as Linux decides, and small files
are stored inline, many per transaction, in the top
level, subvolumes and writable snapshots. Read-only snapshots return READ_ONLY.
Old backreference formats and LZO on write are unsupported. Preallocated extents and unshared NODATACOW extents are written in
place as Linux does. Set-id writes need
the caller's privilege decision; immutable and append-only inodes refuse changes
as Linux does. Adapters still owe credential, ACL and
capability policy. CoW of shared blocks follows Linux's snapshot reference rules;
inline and keyed references, FULL_BACKREF conversion and release are covered,
and every committed state passes the independent reference audit.
Namespace operations create every object type, link, unlink (keeping open
inodes as orphans), evict, clean orphans, rename and edit xattrs, including the
`btrfs.compression` property, in any writable file tree; every committed state
passes the independent namespace audit. Names beyond a full INODE_REF item use
extended references as Linux does. Subvolumes and snapshots are created and
deleted as Linux does, and deleted ones are dropped with resumable progress.

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

Native data writeback, relocation trees, system-chunk growth, chunk removal and
balance, free-space representation conversion, the v1 space cache, quotas,
accepted native writable mounts with a cleaner, and UBC/FSKit coherence; adapter
use of superblock recovery and tree-log replay; LZO/alternate checksums/RAID;
full Linux authorization semantics; provisioning and mounted FSKit acceptance;
matched Linux performance measurements.
