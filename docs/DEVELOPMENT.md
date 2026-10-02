# Development

Read AGENTS.md first. This is a standalone repository on `development`; the
kernel fork remains in `../lab/xnu`. No kernel hooks were added for this reader.
Keep generated images, archives, binaries, fuzz corpora and logs in ignored
directories. Build/test commands below run from this repository unless marked
as lab commands. VM commands always use the absolute lab working directory.

## Portable build and checks

Required: C11 compiler, Meson >= 1.3, Ninja, Python >= 3.11 and zlib. Install Zstd
development headers/libraries for full image acceptance. On macOS, use the selected
Xcode command-line tools; `make` chooses `xcrun --sdk macosx clang` for new builds.
Meson retains the compiler chosen at initial configuration, so select a fresh
build directory when changing toolchains.

```sh
make build BUILD_JOBS=2
make test BUILD_JOBS=2 TEST_JOBS=2 MESON_OPTIONS='-Dfixtures=artifacts/fixtures'
make check-style
```

The default build enables ASan/UBSan, stops on sanitizer findings and treats
warnings as errors. A second freestanding
core build has no sanitizer and enforces a 2 KiB stack-frame budget. `make test`
uses a minimal environment so Meson does not copy unrelated credentials into its
logs. Without `-Dfixtures`, only self-contained wire/API, identity, native policy and
private CoW editor tests run;
that is not complete acceptance. With fixtures, every image is required and an
unavailable codec causes the image test to fail, not silently skip.

Explicit superblock recovery of an image (dry run unless `--apply`; exit 3 means
RECOVERY_REQUIRED; `tests/check_recovery.py` exercises it on a copy):

```sh
.build/btrfs-inspect IMAGE recover
.build/btrfs-inspect IMAGE recover --apply --acknowledged GENERATION
```

`btrfs-bench IMAGE [--cache MiB] [--write [--durable]]` measures per-operation
wall time, backend reads, read bytes and allocations for sequential and random
reads, lookups, directory streams with and without inode attributes, and (on a
copy, which it modifies) single-file commits, an 8 MiB write, and creates
committed one by one or grouped with one sync. Barriers are skipped unless
`--durable` keeps the image's `F_FULLFSYNC`. Build it without sanitizers for
meaningful times:

```sh
meson setup .build-release --buildtype=release -Db_sanitize=none
meson compile -C .build-release btrfs-bench
cp artifacts/fixtures/transactions.raw /tmp/bench.raw
.build-release/btrfs-bench /tmp/bench.raw --cache 64 --write
```

Useful inspection commands:

```sh
.build/btrfs-inspect artifacts/fixtures/plain.raw info
.build/btrfs-inspect --tree 5 artifacts/fixtures/plain.raw ls /many
.build/btrfs-inspect artifacts/fixtures/plain.raw cat /big 4093 8199
.build/btrfs-inspect artifacts/fixtures/plain.raw xattr /greeting user.binary
```

This tool reads regular image files only. `cat` of a symlink returns its payload;
the image helper is not an implementation of host or Linux namei.

## Linux-authored fixtures

The six reader profiles are `plain`, `small-nodes`, `large-nodes`, `zlib`, `zstd`
and `default-subvolume`. The transaction suites require eight writable profiles
without a free-space tree and mounted with `nospace_cache`, plus
`transactions-fst` and `transactions-grow` with mkfs and mount defaults (16 KiB
nodes, DUP metadata and a free-space tree), `transactions-namespace` (mkfs
defaults with 4 KiB nodes and single metadata) and `transactions-convert` (mkfs
defaults with 4 KiB nodes, 1 GiB), plus `transactions-copies` (4 KiB nodes,
single metadata, no free-space tree, a 257 GiB sparse device holding all three
superblock copies): `transactions` (4 KiB
nodes, single metadata), `transactions-dup` (16 KiB nodes, DUP metadata),
`transactions-large` (64 KiB nodes, DUP metadata), `transactions-full` (4 KiB
nodes, single metadata, 128 MiB), `transactions-shared`, `transactions-keyed`
and `transactions-data` (4 KiB nodes, single metadata), and `transactions-holes`
(4 KiB nodes, DUP metadata and data, `-O ^no-holes`, 512 MiB). The full profile first fills all unallocated
space with data, then metadata with inline files carrying leaf-sized xattrs until
Linux reports ENOSPC, then removes every seventh filler so the remaining free
metadata is scattered. The shared profile adds a level-2 subvolume with a
writable and a read-only snapshot, data files, a reflink, a partially overwritten
file (two references to one extent at different extent offsets), and a second
subvolume with exactly one writable snapshot whose leaves interleave inline and
data files. Linux reads some files after the snapshots, so the image already
holds parent-named references and FULL_BACKREF blocks. The keyed profile has a
subvolume with 30 writable snapshots and 30 reflinks of one extent, more
references than an extent item lists inline, so Linux also writes keyed
backreference items. The data profile has a subvolume with a 1 MiB file, an
unaligned 10,000-byte file, a 4 MiB sparse file, a 256 KiB preallocation, a zlib
property file, a NODATASUM (`chattr +C`) file, an inline file and a reflink of
the large file, then a writable and a read-only snapshot. The free-space-tree
profile repeats the data payload, then writes 256 small files and removes every
other one so Linux keeps that data block group's free space as bitmaps. The
growth profile fills data, then metadata with leaf-sized xattr files until
ENOSPC, deletes the data and the newest 600 fillers, and runs `btrfs balance
start -dusage=0`, leaving nearly full metadata and unallocated device space.
The namespace profile (`transactions-namespace`, 4 KiB nodes, single metadata,
mkfs defaults with a free-space tree) adds `/ns` with nested directories, a
hard link, a reflinked data file, a FIFO and `/dev/null`'s device number, a
NODATACOW and a COMPRESS directory, directories with `btrfs.compression`
properties `zstd` and `no`, two directory entries and two xattrs whose names
share one CRC32C hash, and a file with 41 links whose 200-byte names exceed its
INODE_REF item, so Linux stores 22 of them as extended references.
The holes profile repeats the data payload without NO_HOLES, so Linux writes
explicit hole items (split around the sparse file's sectors, with their
offsets) and every data sector has two copies; it adds a 1 MiB file grown by
truncation alone and a NODATACOW directory.
The convert profile writes an 8 MiB filler and 480 one-sector files into a
112 MiB data block group, removes every other file, then the rest in order,
syncing after each removal, and requires Linux to convert the group to bitmaps
at 158 free extents and back to extent items at 56 (one past each of the
thresholds 157 and 57); it stops there, leaving the group in extent form.
Other profiles use 256 MiB. Each uses a separate disposable
raw image and the payload in `tests/prepare_linux.py`. The payload formats **guest
`/dev/vda`**, fills files, takes a snapshot, verifies Linux-visible contents,
unmounts, and requires `btrfs check --readonly` to succeed. Never attach a valuable
image to this payload.

The existing lab Linux reference runner supplies the VM transport. The accepted
fixture environment used Alpine 3.22.5 AArch64, Linux 6.12.94 and btrfs-progs 6.14.
Before reproducing, follow lab's Linux reference setup and reserve its runner with
the VM operator. Prepared paths relative to lab are:

| Resource | Path |
| --- | --- |
| Runner | `.cache/linux-reference/linux-vm` |
| Kernel | `.cache/linux-reference/Image` |
| Matching modules archive | `.cache/linux-reference/netboot-3.22.5/boot/modloop-virt` |
| Disposable initrd root | `artifacts/btrfs-reference/root` |
| Cached APK packages | `artifacts/btrfs-reference/packages` |

The initrd needs BusyBox, musl, `mkfs.btrfs`, `btrfs`, `setfattr`, `getfattr`, and
their dynamic dependencies. The staged root uses the lab reference BusyBox,
musl, zlib and Zstd; additional Alpine v3.22 AArch64 main APKs are btrfs-progs,
eudev-libs, libblkid, libuuid, lzo, libeconf, attr and libattr. Extract packages
into the disposable root with symlinks preserved. Do not copy dependencies into
tracked driver sources. Standard `unsquashfs` or the sibling ext4 test extractor
can extract the matching module archive. Put these modules in `root/modules/`:

```text
virtio_blk.ko xor-neon.ko xor.ko raid6_pq.ko crc32c_generic.ko libcrc32c.ko btrfs.ko
```

The Linux crash-state recording also needs `dm-mod.ko` and `dm-log-writes.ko` in
`root/modules/` and a static `dmsetup` at `root/sbin/dmsetup` (from the
device-mapper-static APK, `usr/sbin/dmsetup.static`).

The payload loads them in dependency order. Modules and the running guest kernel
must match. A fresh checkout requires staging these external tools; the fixture
preparer does not silently download or substitute them.

For each profile, run the following **from the absolute lab directory**, replacing
`plain` consistently and using 128 MiB for `transactions-full`. The image creation
uses exclusive mode to avoid truncating an existing fixture. Archives and
manifests go to generated directories.

```sh
python3 ../btrfs/tests/prepare_linux.py \
  --root artifacts/btrfs-reference/root --profile plain \
  --archive artifacts/btrfs-reference/plain.cpio
mkdir -p ../btrfs/artifacts/fixtures ../btrfs/logs
python3 -c 'from pathlib import Path; p=Path("../btrfs/artifacts/fixtures/plain.raw"); f=p.open("xb"); f.truncate(256*1024*1024); f.close()'
.cache/linux-reference/linux-vm .cache/linux-reference/Image \
  artifacts/btrfs-reference/plain.cpio 2 512 \
  'console=hvc0 rdinit=/init panic=-1 loglevel=4' \
  ../btrfs/artifacts/fixtures/plain.raw > ../btrfs/logs/linux-reference-plain.log 2>&1
cp artifacts/btrfs-reference/plain.json ../btrfs/artifacts/fixtures/plain.json
```

Require the exact `BTRFS_REFERENCE_PASS:plain` marker, no failure marker, successful
Linux checks and a completed VM exit before consuming the image. Repeat all twenty
profiles (512 MiB for `transactions-holes`, 1 GiB for `transactions-convert`,
2 GiB for `transactions-scale`, 257 GiB for `transactions-copies`, created with
`truncate` so they stay sparse and never copied byte by byte), then run the portable image and
transaction suites. It hashes each complete image before and after reading,
verifies 367 contracts (the six reader profiles and `transactions-holes`, whose
split hole items it reads), and fails if any byte changed.

## Linux-written crash states

`tests/prepare_logwrites_linux.py` builds a payload that records every write of a
Linux Btrfs workload with dm-log-writes: `/dev/mapper/logged` logs `/dev/vda`
(256 MiB) into `/dev/vdb` (1 GiB); mkfs (without discards), six steps that each
end with `sync` and a named mark (files, an overwrite, hard links, 50 inline
files, a subvolume with an 8 MiB file and a snapshot, removals, truncation, 300
files splitting the tree, a directory rename, a snapshot deletion), unmount and
`btrfs check`. From the absolute lab directory, create both images without
overwriting existing files and run:

```sh
python3 ../btrfs/tests/prepare_logwrites_linux.py \
  --root artifacts/btrfs-reference/root --archive artifacts/btrfs-reference/logwrites.cpio
.cache/linux-reference/linux-vm-external .cache/linux-reference/Image \
  artifacts/btrfs-reference/logwrites.cpio 2 512 \
  'console=hvc0 rdinit=/init panic=-1 loglevel=4' \
  ../btrfs/artifacts/fixtures/logwrites-data.raw ../btrfs/artifacts/fixtures/logwrites.log \
  > ../btrfs/logs/linux-logwrites.log 2>&1
```

Require `BTRFS_LOGWRITES_PASS`. `btrfs-logwrites-test LOG DATA` (Meson
`linux-crash-states`) replays the log from a zeroed device. Epochs end at a
flush (before its write) and at a FUA write or a mark (after it); every prefix
after mkfs and eight random subsets of each epoch's writes are crash states.
Each must mount, or be recovered explicitly when its superblock copies
disagree (Linux writes the mirror after the FUA primary), and pass both
audits, a transaction admission and full reads of every file; at each mark the
synced files must be exact. Replaying the whole log must reproduce the final
device image byte for byte. The inputs are generated deterministically on both
sides, so no input file is shipped.

## Fuzzing and concurrency

Apple's selected compiler lacks libFuzzer on this machine. Use a separate upstream
LLVM userland build; native builds still use Xcode. The fuzzer overlays mutations
in memory across superblock and tree regions, optionally repairs their CRCs, and
exercises mount, traversal, read, xattrs and directory streams. Both DUP copies
receive the mutation so fallback cannot hide it. The seed image stays read-only.

```sh
CC=/opt/homebrew/opt/llvm/bin/clang make build BUILD_DIR=.build-fuzz-llvm \
  MESON_OPTIONS='-Dfuzzer=true -Dfixtures=artifacts/fixtures'
mkdir -p artifacts/fuzz-corpus artifacts/fuzz-findings
python3 -c 'from pathlib import Path; d=Path("artifacts/fuzz-corpus"); [(d/f"{i}-{r}").write_bytes(bytes([i,r,0,0,0])) for i in range(16) for r in range(2)]'
env -i PATH="$PATH" BTRFS_FUZZ_IMAGE="$PWD/artifacts/fixtures/plain.raw" \
  .build-fuzz-llvm/btrfs-fuzz -max_total_time=30 -timeout=3 -max_len=1024 \
  -artifact_prefix=artifacts/fuzz-findings/ artifacts/fuzz-corpus
CC=/opt/homebrew/opt/llvm/bin/clang meson setup .build-tsan \
  -Db_sanitize=thread -Dfixtures=artifacts/fixtures
meson compile -C .build-tsan btrfs-concurrent btrfs-volume-test btrfs-extents-test
env -i PATH="$PATH" meson test -C .build-tsan --no-rebuild concurrent-readers \
  native-volume-views native-volume-stress decompressed-extents --print-errorlogs
```

`native-volume-stress` runs four readers against the shared native volume layer
(`adapters/common/volume.c`) while one writer runs 1,000 transactions of
creates, rewrites, unlinks and replacing renames. Before each commit the writer
records the expected names, sizes and contents for the generation it may
publish; every reader checks its pinned view against the record for that view's
generation (lookups, full reads and the directory listing), and one reader
checks its view again after a pause, so later transactions must not reuse its
blocks. The writer also aborts transactions, fails allocations inside them, and
finally fails a commit write: the volume must refuse the next writer, keep
serving the published view, and a reopened volume must find that generation.
Its allocation map must have been loaded once, at admission, and reused by
every later transaction, aborted and failed ones included.

A bounded smoke campaign is not exhaustive fuzzing. Preserve and minimize every
crashing input; turn the cause into a deterministic regression before fixing it.

## Native builds and guest acceptance

FSKit requires macOS 26.4 SDK or later and XcodeGen. Kext builds use Kernel.framework
headers from the selected SDK and enforce the same core stack budget.

```sh
python3 scripts/build_fskit.py
python3 scripts/build_kext.py --arch arm64e
python3 scripts/build_kext.py --arch x86_64
```

Outputs are unsigned by default. Builds do not install, register, load, attach
devices or change boot policy. FSKit signed builds accept `--team` and optionally
`--provision`; use only the personal identity and provision the filesystem-module
capability in a dedicated guest workflow. No signing or installation acceptance
has been established for this project. Do not use a different entitlement to
pretend filesystem-module authorization exists.

In an isolated guest with Python, mount a disposable copy of the plain fixture
at its top-level subvolume, then run as root:

```sh
python3 tests/mounted_contracts.py --mount /mnt/btrfs \
  --suite readonly --disposable-guest
```

The future writer runs the same script with `--suite write` on a disposable writable
mount. These mounted suites are prepared contracts, currently **unexecuted** for
Machlin Btrfs. They are not part of portable acceptance and cannot substitute for
Linux fsck, unmount/remount, fault injection, crash recovery, native authorization
or LXNU tests. See HANDOFF.md for those mandatory gates.

## Transaction persistence and independent Linux oracle

The public writer interface is `include/btrfs/write.h`; both native adapters
currently stay read-only. `btrfs-transaction-test` runs source-controlled
scenarios on a recorded device. It never writes the source fixture. Its
recorded device, plan model and runner are `tests/scenario_{device,plan,run}.c`;
the scenario sets are `tests/scenario_{sets,namespace,checks}.c`. The Linux
oracle's guest script is `tests/transaction_oracle.sh`.

| Scenario | Operations |
| --- | --- |
| `replace` | Replace `/greeting` |
| `empty` | Zero-length replacement removes the inline extent |
| `maximum` | 2048-byte inline payload |
| `batch` | `/greeting` and 19 `/many` entries, growing items in several leaves |
| `repeated` | Two commits; the second starts from the first Machlin root set |
| `shared-*`, `pair-convert` (`--shared`) | Writes in a snapshot source, its writable snapshot, alternately, and across three trees; leaves with reflinked and offset data references; FULL_BACKREF conversion and release |
| `keyed-*` (`--keyed`) | The same decisions on blocks and extents whose references are partly keyed items |
| `grow-*` (`--grow`) | Metadata and data chunk growth from unallocated device space |
| `fst-*` (`--fragment`) | Frees between bitmap holes and a write that spills from extent-mode free space into bitmap holes |
| `fst-round-trip` (`--convert`) | One-sector files in the 112 MiB group: removing every other one converts it to bitmaps, removing the rest converts it back |
| `groups-*` (`--groups`) | Removing `/sparse` empties a data group, which the cleaner pass keeps in that commit and removes in the next; a later write takes the device space back with a new data chunk; a chunk tree that finds no system space grows a system chunk through the superblock's array, and the emptied old system group is removed from both; a group freed within the transaction stays |
| `holes-*` (`--holes`) | Without NO_HOLES and on DUP data: truncation and writes past EOF covered by hole items, a write splitting a hole, truncation inside a hole, an inline file written far beyond its sector, Linux's split hole items and a file grown by truncation alone, a write into a 16 GiB hole item, and writes in place into Linux's preallocated file and a new NODATACOW file on both copies |
| `data-*` (`--data`) | Unaligned overwrite of a reflinked extent, append, holes and past-EOF writes, preallocation, zlib, NODATASUM, inline conversion, truncation, snapshot overwrites, overlapping writes; compression on write by property (128 KiB zlib extents, incompressible data, compressed and plain inline files, an overwrite splitting a compressed extent, truncation) and by the zstd mount option (ZSTD feature, `no` property, NODATASUM) |
| `namespace-*` (`--namespace`) | Every object type with inherited flags and data, 100 names splitting leaves; appends to, cuts from and renames within colliding DIR_ITEM and xattr items; hard links (across directories, to a device, beside extended references); names beyond a full INODE_REF item in new INODE_EXTREF items, a colliding one and one Linux wrote, unlinked from packed and last entries, renamed into and out of the INODE_REF item and across directories, and an INODE_EXTREF item filled to the largest item; unlinks of shared and last data references; renames across directories, over files, over an empty directory and between names of one inode; open unlinks left as orphans, eviction and orphan cleanup; the largest xattr; a subvolume tree; compression properties and their inheritance; a DIR_ITEM filled to the largest item; zstd by an inherited property; writes in place into Linux's preallocated file (item split) and into an unshared NODATACOW extent, copied on write once a snapshot shares it or another reference exists; 200 one-sector files of which every other one is removed, converting their block group to bitmaps (`fst-bitmaps`); O_TMPFILE files inheriting COMPRESS, linked in or left as orphans for cleanup, with both linking refusals; RENAME_EXCHANGE of files across directories and of a file with two names and a directory, a no-op between two names of one inode and a directory refused below itself; RENAME_WHITEOUT leaving 0:0 whiteouts, also over a replaced target |
| `subvolume-*` (`--subvolume`) | Subvolumes at the top level, in a directory and in another subvolume, inheriting the parent subvolume's compression property; writable and read-only snapshots of a subvolume, of the multi-level top level, of a read-only snapshot and of a snapshot, edited on either side; copied subvolume entries as stubs; deletion of subvolumes, snapshots and a stub entry; the cleaner resuming a partial drop across commits, and fully dropping a subvolume whose leaves a snapshot shares and an unshared one with data |
| `random-N` (`--random FIRST COUNT`, `--random-quick FIRST COUNT`) | Seeded differential sequences: 24 operations in three commits drawn from create of every type, link, unlink, rename (also over files and between names of one inode), xattr set/remove, write, truncation, attribute changes, orphan cleanup and expected refusals under `/fuzz`, over a name pool with real CRC32C collisions, plus RENAME_EXCHANGE and RENAME_WHITEOUT; a separate model predicts each stage's namespace facts |

`btrfs-reference-audit` is an independent reference oracle in the portable
suite. It walks every tree from the superblock and root items, derives the
backreference every parent pointer and file extent item requires (shared forms
under FULL_BACKREF parents), and compares the multiset and totals exactly with
the extent tree, including leaks. Its checksum pass verifies every copy of
each checksummed sector, so a DUP write that misses one copy fails. It runs on
every Linux fixture, after every scenario commit, and against deliberately
damaged references.
`tests/namespace_audit.c` is the matching namespace oracle: in every file tree
each name must exist exactly once as DIR_ITEM, DIR_INDEX and inode or extended
reference with equal index, inode and type; link counts must equal names,
directory sizes twice their name lengths, and unlinked inodes must have orphan
items. As `btrfs check` does, the file extent items of a regular file or
symlink must not overlap and must count its `nbytes` (hole items excluded), and
without NO_HOLES they must cover every byte below its size. It runs on every
Linux fixture, after every commit, in every crash state of the namespace
scenarios, and against a committed wrong link count, directory size and
`nbytes`; dropping the hole items fails it on `transactions-holes`.

Each scenario's last commit runs allocation, read, write and barrier fault
points: every point up to 512 per class, and a deterministic stride that keeps
the first and last point for larger commits. Every recorded commit is then cut
at each issue-order prefix (the growth scenarios check 64 evenly spaced prefixes
and every 50th of their 2,000 files) and in each barrier epoch: 32 seeded
metadata states persist arbitrary subsets of whole, missing, sector-subset or
torn writes; each superblock epoch tries six tear patterns per copy, and every
combination across the two secondaries of a device past 256 GiB (36). Every state is classified by mounting the primary and by explicit
recovery. It must resolve to the acknowledged or new stage with exact contents,
invariant snapshots/xattrs/links, every free-space-tree block group on the right
side of Linux's conversion thresholds (derived independently in the test), and
admit the next transaction. Finally every plan is replayed from its base with one
allocation map kept across its commits: each replayed commit must issue exactly
the recorded writes, and afterwards the map must equal a fresh load and
verification of the committed state. The suite also
checks admission, stale copies, recovery refusals and checksum-correct damaged
allocation maps. `--full` adds the metadata-exhaustion case: the full profile's
remaining metadata cannot hold a batch of all inline files, so the edit returns
NO_SPACE without any write. `--namespace` also checks that every refusal (about
forty: names, types, read-only snapshots, subvolume entries, xattr limits,
properties) and numbering or packed-item limit (last directory index, last inode
number, the per-transaction directory bound, full colliding DIR_ITEM and
INODE_EXTREF items) is decided before the first change and leaves the transaction
able to commit. It also checks numbering across committed transactions: without
counters a removed highest inode number and directory index are handed out
again; with a mount's `btrfs_counters` both continue, also past an aborted
transaction, until the full directory table forgets a directory, and a stale
counter never numbers a new directory that reuses an inode number.

`--data` also writes 80 MiB in one transaction and requires the transaction to
hold less than 16 MiB afterwards (new data reaches the device as its extents
are created), the file to read back exactly and both audits to pass. With
`--namespace`, files in a directory with the zstd property inherit it.
Allocation and read faults may leave writes issued before the commit, but only
of data (no superblock copy, metadata or system chunk); the state must still
resolve to the previous stage. A plan declares the bytes a commit writes in
place (`plan_volatile`): crash and fault states of that commit that resolve to
the previous stage may hold, per 512-byte device sector, the old or the new
bytes there; the export adds `volatile.tsv`, and the Linux oracle applies the
same rule with `cmp -l`. While a transaction runs, the recorded
device returns every issued write, as a block device does before a flush.

`tests/scenario_random.c` adds differential coverage. Seed N draws operations
with fixed weights against a model of `/fuzz` (inodes, names, link counts,
owners, modes, times, data, symlink targets and xattrs), including refusals the
model predicts (existing and missing names and xattrs under create/replace
flags, non-empty directories, a directory renamed below itself, a linked
directory); an operation that must be refused and succeeds fails the test.
After every commit the model's state becomes the stage's expectations: the
reader, the reference audit and the namespace audit check them, and `--random`
samples twelve prefixes and 24 fault points per commit plus the reorder and tear
epochs, while `--random-quick` checks each committed stage only. `BTRFS_RANDOM_TRACE=1` prints each drawn
operation. Meson runs seeds 1-16 on `transactions-namespace` and quick seeds
1-500 on `transactions-dup`; export seeds with `--random 1 16 --export DIR`
on `transactions-namespace` for the Linux oracle.

Export a profile's crash cases into a new generated directory. Pass the same
profile flag Meson uses: `--full` for `transactions-full`, `--shared` for
`transactions-shared`, `--keyed` for `transactions-keyed`, `--data` for
`transactions-data`, `--data --fragment` for `transactions-fst`, `--grow`
for `transactions-grow`, `--data --holes` for `transactions-holes`,
`--convert` for `transactions-convert`, `--groups` for `transactions` and
`transactions-dup`, and `--namespace` or `--subvolume` for
`transactions-namespace`. Reader
profiles with a free-space tree (`plain`, `small-nodes`) also run the default
scenarios:

```sh
mkdir artifacts/transaction-plan-transactions
.build/btrfs-transaction-test --export artifacts/transaction-plan-transactions \
  artifacts/fixtures/transactions.raw
```

Each scenario directory holds write payloads (`writes.tsv`), per-stage expected
contents (`stages.tsv`), and one sector-run list per case plus the superblock
writes this implementation's recovery chose. Namespace scenarios add
`namespace.tsv`: per stage, absent paths, file contents, exact sorted directory
listings with their sizes, symlink targets, hard-link identity, xattr values and
absence, mode/owner/link counts, device numbers, inode flags, incompat features
whether INODE_REF or INODE_EXTREF holds a name, how many of a file's regular and
inline extents a codec compressed, a file's regular and preallocated extent
items and distinct disk extents, a subvolume's read-only flag and
snapshot source, the list of subvolumes and the number of deleted ones waiting
for the cleaner, with payload files for expected bytes. These are generated
test inputs, not
a source ledger. About sixteen prefixes, ten metadata states and all superblock
tear patterns are exported per commit; the portable test checks all of them.

Reserve the Linux runner and stop the macOS test guest first. The oracle needs the
two-disk runner built from lab `scripts/linux-vm.swift` (the lab caches it as
`.cache/linux-reference/linux-vm-external`). From the absolute lab directory,
create a disposable working copy and a pristine copy without overwriting existing
files, then run (use 134217728 bytes for `transactions-full`). For the 257 GiB
`transactions-copies` image, make both copies as APFS clones (`/bin/cp -c`,
after checking that neither exists) and pass 275951648768 bytes: the guest then
compares only the ranges the cases wrote instead of whole disks and runs
Linux's read-write continuation after the last scenario only, since restoring
whole disks would read every byte.

```sh
python3 -c 'from pathlib import Path; import shutil; s=Path("../btrfs/artifacts/fixtures/transactions.raw"); [shutil.copyfileobj(s.open("rb"), Path(d).open("xb")) for d in ("../btrfs/artifacts/oracle-transactions-work.raw", "../btrfs/artifacts/oracle-transactions-pristine.raw")]'
python3 ../btrfs/tests/prepare_transactions_linux.py \
  --root artifacts/btrfs-reference/root --plan ../btrfs/artifacts/transaction-plan-transactions \
  --archive artifacts/btrfs-reference/transaction-check-transactions.cpio --device-bytes 268435456
.cache/linux-reference/linux-vm-external .cache/linux-reference/Image \
  artifacts/btrfs-reference/transaction-check-transactions.cpio 2 512 \
  'console=hvc0 rdinit=/init panic=-1 loglevel=4' \
  ../btrfs/artifacts/oracle-transactions-work.raw ../btrfs/artifacts/oracle-transactions-pristine.raw \
  > ../btrfs/logs/linux-transactions-transactions.log 2>&1
```

The guest restores the working disk from the pristine `/dev/vdb` before every
case and compares both disks after each scenario. For each case it applies the
exported sector runs, then requires Linux to agree with the recorded outcome:
`btrfs check --readonly`, the primary generation, exact tracked contents,
invariants and the stage's namespace facts for a valid primary (with Linux's
`stat`, `readlink`, `getfattr`, its own `dump-tree` for inode flags and back
references, `btrfs subvolume show` for flags and parent UUIDs, and
`btrfs subvolume list` with and without `-d`; listings use shell globbing, which
keeps every byte of a name),
or a failed mount for a torn primary. For each
recovery case it runs `btrfs rescue super-recover -y`, requires status 2 and the
same resolved generation and contents, then repeats from the crash state with
this implementation's recovery writes and requires Linux to find every copy
valid. After each scenario's cases Linux mounts its newest root read-write
(cleaning any orphans the scenario left), finishes every subvolume drop this
implementation left (`btrfs subvolume sync`, then no deleted subvolume
remains), writes, syncs and passes `btrfs check --check-data-csum`, which
reads every copy of checksummed data, with no orphan item left in the
top-level tree; the guest then copies the
pristine disk back whole. Require `BTRFS_TRANSACTION_NAMESPACE_CHECKS:M` and
`BTRFS_TRANSACTION_PASS:N` with the counts printed by the preparer and in its
JSON (the guest also compares the namespace count itself), and an unchanged
pristine copy and fixture. The oracle never runs `btrfs check --repair`. It proves on-disk
compatibility and Linux agreement for these states; actual native flush
durability is a separate gate once native write callbacks exist.

## Identified macOS mounted acceptance

Use a dedicated guest through the lab's normal native kernel collection workflow.
The prepared Btrfs guest is `lxnu-btrfs-kext-lab`; preserve its fallback boot slot.
Do not install a host kext or change host boot policy. Capture the kext bundle,
matching mount helper and unsanitized `.build/btrfs-mounted-test` before packaging.
The generated product directory must hold `MachlinBtrfs.kext`, its collection and
`kc-identity.json`, including the collection/module hashes and kernel UUIDs.
Verify the actual loaded guest module with `kmutil showloaded` or `kextstat`;
activate the packed bundle with `kmutil load -b org.machlin.btrfs.kext` if needed.

`tests/run_macos.py` checks these captured identities and transfers only fixture
copies and probes. Run from the absolute lab directory, passing the observed boot
session and module UUID (do not copy stale values from old reports):

```sh
python3 ../btrfs/tests/run_macos.py \
  --lab "$PWD" --vm lxnu-btrfs-kext-lab \
  --products "$PWD/artifacts/btrfs-kext" --share btrfs-kext \
  --guest-directory /var/tmp/machlin-btrfs-tests \
  --expected-session "$btrfs_test_session" --expected-module-uuid "$btrfs_test_module_uuid" \
  --probe "$PWD/../btrfs/.build/btrfs-mounted-test" \
  --mount-helper "$PWD/../btrfs/artifacts/kext-final/arm64e/mount_machlin_btrfs" \
  --image "$PWD/../btrfs/artifacts/fixtures/plain.raw" \
  --image "$PWD/../btrfs/artifacts/fixtures/small-nodes.raw" \
  --image "$PWD/../btrfs/artifacts/fixtures/large-nodes.raw" \
  --image "$PWD/../btrfs/artifacts/fixtures/zlib.raw" \
  --output "$PWD/artifacts/btrfs-kext/mounted-new-run"
```

The output directory must be new and inside the shared product directory. The
probe starts as guest root solely to run tests under specified ordinary UIDs;
the filesystem's authorization uses their actual credentials. Every image is
attached read-only, verified by raw-device hash, mounted, tested, normally
unmounted, hashed again and detached. A failed cleanup or missing image is not a
pass. Keep FSKit and LXNU acceptance separate from these native XNU results.
