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
build directory when changing toolchains. On macOS the suite also builds the
Objective-C FSKit volume test; `make` sets `OBJC` to the C compiler, and a
manual sanitizer build must do the same (for example `OBJC=/usr/bin/clang` beside
Apple's C compiler), since both must link one sanitizer runtime.

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

`extent-codecs` (`tests/codec.c`) runs the freestanding LZO1X and Zstandard
decoders of `adapters/common/codec.c` against liblzo2 and libzstd when Meson
finds them (`lzo2` and `libzstd` through pkg-config; Homebrew provides both):
streams from lzo1x_1, lzo1x_999 and many libzstd settings (levels, windows,
block sizes, literal modes, checksums, with and without content sizes) must
decode to the input, and mutated streams must agree with the references'
oracles. Without a library it prints a SKIP line for that codec and runs only
hand-written streams; acceptance needs both libraries.

`full-volume-*` (`tests/full_volume.c`) works on an APFS clone of a Linux
image in the build directory, removed afterwards: through the native volume
layer it fills the data and then the metadata until NO_SPACE, deletes the large
file and writes a new one, and a fresh mount checks the result.

`checksum-algorithms` (`tests/checksum.c`) compares the core's XXH64, SHA-256 and
BLAKE2b-256 with published digests and with libxxhash, CommonCrypto and libb2
(`libxxhash` and `libb2` through pkg-config; Homebrew `xxhash` and `libb2`)
over every short length, alignments and sector and node sizes. Without a
library its comparisons are not made and the printed count drops; acceptance
needs all three.

Explicit superblock recovery of an image (dry run unless `--apply`; exit 3 means
RECOVERY_REQUIRED; `tests/check_recovery.py` exercises it on a copy):

```sh
.build/btrfs-inspect IMAGE recover
.build/btrfs-inspect IMAGE recover --apply --acknowledged GENERATION
```

Explicit tree-log replay of an image: without `--apply` the log is replayed
without a commit and nothing is written (exit 3: a log is pending and replays,
4: no log is pending); `--apply` commits the replay. Superblock copies that
disagree beyond the log fields need `recover --apply` first. `walk [--data]`
prints one JSON line per path below the mounted tree's root (attributes,
xattrs and, with `--data`, file and symlink bytes, all names and bytes in
hexadecimal):

```sh
.build/btrfs-inspect IMAGE replay
.build/btrfs-inspect IMAGE replay --apply
.build/btrfs-inspect IMAGE walk --data
.build/btrfs-inspect IMAGE seek PATH data|hole OFFSET
```

`tree-log-replay` (`tests/check_log_replay.py`) replays copies of the `logs`
and `logs-many` fixtures through `btrfs-inspect` and compares the walked
namespace with the manifest Linux printed after its own replay; it also
refuses malformed logs and recovers disagreeing copies before replay.
`tree-log-replay-faults` (`tests/replay.c`) fails every allocation, read, write
and barrier of a replay of `logs` in turn, and cuts power after every write,
then requires the adapters' sequence (replay, superblock recovery, open) to
reach the namespace of an uninterrupted replay. Run it on `logs-many` by hand
(`.build/btrfs-replay-test artifacts/fixtures/logs-many.raw`, about 17 minutes
with sanitizers).

`btrfs-bench IMAGE [--cache MiB] [--write [--durable]]` measures per-operation
wall and process CPU time, backend reads, read bytes and allocations for
sequential and random reads, lookups, directory streams with and without inode
attributes, and (on a copy, which it modifies) single-file commits, an 8 MiB write, and creates
committed one by one or grouped with one sync. Barriers are skipped unless
`--durable` keeps the image's `F_FULLFSYNC`. Build it without sanitizers for
meaningful times:

```sh
meson setup .build-release --buildtype=release -Db_sanitize=none
meson compile -C .build-release btrfs-bench
cp artifacts/fixtures/transactions.raw /tmp/bench.raw
.build-release/btrfs-bench /tmp/bench.raw --cache 64 --write
```

`--volume-only --volume-files 2000` measures just the single-commit and grouped
create series (1 through 100,000 files per series, default 200). It implies
`--write` and does not require `/big` or `/many`. Use a fresh image copy for each
trial, alternate the binaries at least seven times and report CPU time beside
wall time: host file I/O can add large stalls even when barriers are skipped.
Both are image-backend measurements, not native mounted or Linux comparisons.

`btrfs-fsync-bench DIRECTORY FILES BYTES [--samples]` is a separate mounted
POSIX workload, built on both Linux and macOS. Use an existing empty directory
on a disposable filesystem. It exclusively creates each file, writes deterministic
contents, fsyncs the file, closes it and fsyncs its directory. It reports JSON
with each stage's mean/p50/p95/p99, wall time and caller CPU time, then checks
every file's size and contents outside the measured interval. `--samples` adds
per-file timings and monotonic timestamps. Bounds are 100,000 files and 4 MiB
per file. It leaves its files for remount or independent verification; a repeat
in the same directory fails instead of replacing them. Caller CPU excludes the
filesystem extension, daemon and kernel. This workload alone is not a crash
oracle or the complete matched workload matrix.

```sh
meson compile -C .build-release btrfs-fsync-bench
.build-release/btrfs-fsync-bench /Volumes/disposable/fsync-run 64 4096 --samples > run.jsonl
```

For FSKit attribution, `scripts/build_fskit.py --profile-io` adds diagnostic
logging to the extension and its device-barrier daemon. Install both in the
dedicated guest, capture `log stream --style ndjson --predicate 'eventMessage
CONTAINS "btrfs-io "'` during the workload, then run:

```sh
python3 tools/analyze_fskit_io.py trace.jsonl --benchmark run.jsonl --output attribution.json
```

The trace records resource reads/writes, staged drains, XPC barriers, the daemon's
flush ioctl and volume-sync callbacks. Monotonic timestamps select the workload's
window and relate callbacks to file/directory fsync. Intervals nest: do not add
drain and resource-write time, or XPC and ioctl time. Unattributed time includes
locking and logging as well as CPU work. Concurrent calls overlap:
`wall_coverage_ns` measures the union of their intervals, while `sum_ns` adds
their individual durations. Profile only one mounted test volume;
diagnostic logging perturbs timing, so performance comparisons use ordinary
builds. Restore the ordinary signed guest build afterwards. Without the flag,
the release extension contains neither the trace strings nor clock calls from
this instrumentation.

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
The checksum profiles repeat the data payload without a free-space tree under
`mkfs.btrfs --csum`: `checksums-xxhash` (16 KiB nodes, DUP metadata),
`checksums-sha256` (4 KiB nodes, single metadata) and `checksums-blake2` (64 KiB
nodes, DUP metadata and data, 512 MiB); Linux also runs `btrfs check
--check-data-csum` and prints the superblock's `csum_type`.
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
their dynamic dependencies. The staged root uses BusyBox and musl from the lab's
pinned `initramfs-virt`, the lab reference Zstd and main's zlib 1.3.2 (the
repository replaced the lab's pinned r0 with r1); additional Alpine v3.22
AArch64 main APKs are btrfs-progs,
eudev-libs, libblkid, libuuid, lzo, libeconf, attr and libattr. Extract packages
into the disposable root with symlinks preserved. The metadata-UUID profile also
needs `btrfstune` from btrfs-progs-extra of the same version, checked against the
APKINDEX checksum, placed at `root/sbin/btrfstune` (its other programs need
libraries the root does not carry). Do not copy dependencies into
tracked driver sources. Standard `unsquashfs` or the sibling ext4 test extractor
can extract the matching module archive. The kernel, initramfs and module archive come from
`alpine-netboot-3.22.5-aarch64.tar.gz` (extracted to `netboot-3.22.5/`), whose
`vmlinuz-virt` and `initramfs-virt` must match the digests of the lab's
`config/linux-reference.json` and whose `modloop-virt` the ext4 project's
`tests/run_linux_quota.py` pins; `Image` is the gzip payload of
`vmlinuz-virt`'s EFI zboot wrapper, and both runners are built from lab
`scripts/linux-vm.swift` and signed with `config/linux-vm.entitlements`, as
`scripts/test-linux-reference.py` does. Put these modules in `root/modules/`:

```text
virtio_blk.ko xor-neon.ko xor.ko raid6_pq.ko crc32c_generic.ko libcrc32c.ko
xxhash_generic.ko blake2b_generic.ko btrfs.ko
```

The fs-verity profile needs `fsverity` from Alpine v3.22 community's
fsverity-utils 1.6 at `root/usr/bin/fsverity`, with its `libfsverity.so.0` and
`libcrypto.so.3` from main's libcrypto3 in `root/usr/lib`, each package checked
against its signed APKINDEX. The reference kernel has fs-verity, SHA-256 and
SHA-512 built in.

The Linux crash-state recording also needs `dm-mod.ko` and `dm-log-writes.ko` in
`root/modules/` and a static `dmsetup` at `root/sbin/dmsetup` (from the
device-mapper-static APK, `usr/sbin/dmsetup.static`).

Every guest init loads the modules `root/modules/order` names, in that order
(`virtio_blk xor-neon xor raid6_pq crc32c_generic libcrc32c xxhash_generic
blake2b_generic btrfs` here), then reopens its console on `/dev/hvc0`; the
crash-state recording adds `dm-mod dm-log-writes`. Modules and the running guest
kernel must match. A fresh checkout requires staging these external tools; the
fixture preparer does not silently download or substitute them.

Sectors above 4 KiB need a kernel whose pages are at least as large. The 16 KiB
profiles (`sectors-16k`, `transactions-16k`, `transactions-16k-twins`) run on
CentOS Stream 10's
Hyperscale SIG `kernel-16k` 6.16.4 for Asahi (`6.16.4-0.hs100.hs+asahi.el10`),
which builds in btrfs, fs-verity, SHA-512, xxhash and BLAKE2b and boots in the
Virtualization.framework runner. Fetch its `packages-asahi` repository's
`repomd.xml` with `repomd.xml.asc`, check the signature with the SIG key from
`https://www.centos.org/keys/RPM-GPG-KEY-CentOS-SIG-HyperScale` (fingerprint
`9B04 530E 0ED6 ABC4 B2C3 58DD 2A01 FA2A EB3D AC40`, in a scratch keyring), then
`primary.xml.gz` against `repomd.xml` and the `kernel-16k-core` and
`kernel-16k-modules-core` RPMs against `primary.xml.gz`. Their payloads are
zstd-compressed `newc` archives. `.cache/linux-reference-16k/Image` is the zstd
payload of the core package's EFI zboot `vmlinuz`. `root-16k` beside `root` is
a copy of it whose `modules/` holds `virtio_console.ko`, `virtio_blk.ko`,
`dm-mod.ko` and `dm-log-writes.ko`, decompressed from the modules package's
`.ko.xz` files, with `modules/order` naming `virtio_console virtio_blk`: this
kernel's console is a module, so a guest sees no output before init loads it.
Build those profiles with `--root artifacts/btrfs-reference/root-16k` and the
16 KiB `Image`; `mkfs.btrfs` there formats with `-s 16384`. Apple CPUs implement
no 64 KiB translation granule, so no 64 KiB-sector fixture can be made here.

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
Linux checks and a completed VM exit before consuming the image. Repeat all thirty-three
profiles (512 MiB for `transactions-holes` and `checksums-blake2`, 1 GiB for
`transactions-convert` and `transactions-space-cache`, whose data groups must reach the
100 MiB Linux needs before it writes a v1 cache,
2 GiB for `transactions-scale`, 257 GiB for `transactions-copies`, created with
`truncate` so they stay sparse and never copied byte by byte), then run the portable image and
transaction suites. It hashes each complete image before and after reading,
verifies 2,950 contracts (the seven reader profiles and `sectors-16k`;
`transactions-holes`, whose split hole items it reads; the checksum profiles and
the metadata-UUID, mixed, space-cache, quota, simple-quota, fs-verity and
`transactions-16k` profiles, whose data payload it reads in the subvolume and
both snapshots; and the block-group-tree profile), and fails if any byte
changed. The `transactions-verity` profile enables fs-verity on fourteen
files in a subvolume, then snapshots it read-only; Linux's `fsverity measure`
must print the digest the preparer's independent model computes for each, and
appending, truncating and preallocating must fail. The reader must read both
copies of each file and report the same digests. The `codecs`
profile mounts with `compress-force=lzo` and writes the same 64 files of an
incompressible head and a compressible tail into `lzo`, and, through the
`btrfs.compression` property, `zlib` and `zstd` directories; Linux must report
at least 32 regular extents of each codec. The `transactions-twins` and
`transactions-16k-twins` profiles hold the compression twins: Linux remounts
with `compress=zlib`, then `compress-force=zlib`, then `compress=no` with a zlib
property on each file, and writes every twin in steps (`dd` at an offset, then
`btrfs filesystem sync`, so each step is one delalloc range), keeping each
step's bytes uncompressed in `twins/steps` and the steps in
`twins/manifest.tsv`.

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

## Linux tree logs

`tests/prepare_log_linux.py` builds payloads in three phases. `create` formats
`/dev/vda` (the `logs` profile: 16 KiB nodes, DUP metadata; `logs-many`: 4 KiB
nodes, single metadata; `logs-quota`: `logs` with quotas enabled before the base
and rescanned after it; `logs-squota`: `logs` with simple quotas enabled before
the base), mounts with `commit=3600`, commits a base with `sync`,
then makes fsync durable: a new file, an append, an overwrite, a truncation, a
rename, an unlink with its directory fsynced, a new hard link, an xattr set and
removed, a removed hard link, ten removed directory entries, a preallocation,
mode and ownership changes, an append in a subvolume, an fsynced file removed
again, a symlink and a new directory; `logs-many` adds a 2,000-entry directory
and a file with 64 one-byte overwrites. It then powers off with
`echo o > /proc/sysrq-trigger`, so the image keeps a pending log. `expect`
requires the log, lets Linux replay it by mounting a disposable copy, and prints
the manifest (every path with its number, mode, owner, links, size, content
hash, regular files' mtime and xattrs). `verify` requires no pending log, runs
`btrfs check --readonly`, prints the same manifest, writes a file and checks
again. From the absolute lab directory, with `p` set to `logs`, `logs-many`, `logs-quota` or `logs-squota`
and `S` a private scratch directory:

```sh
python3 ../btrfs/tests/prepare_log_linux.py --root artifacts/btrfs-reference/root \
  --phase create --profile $p --archive artifacts/btrfs-reference/$p-create.cpio
python3 -c "from pathlib import Path; f=Path('../btrfs/artifacts/fixtures/$p.raw').open('xb'); f.truncate(256 << 20); f.close()"
.cache/linux-reference/linux-vm .cache/linux-reference/Image \
  artifacts/btrfs-reference/$p-create.cpio 2 512 \
  'console=hvc0 rdinit=/init panic=-1 loglevel=4' \
  ../btrfs/artifacts/fixtures/$p.raw > ../btrfs/logs/linux-log-create-$p.log 2>&1
cp -n artifacts/btrfs-reference/$p-create.json ../btrfs/artifacts/fixtures/$p.json
cp -n ../btrfs/artifacts/fixtures/$p.raw $S/$p-expect.raw
python3 ../btrfs/tests/prepare_log_linux.py --root artifacts/btrfs-reference/root \
  --phase expect --archive artifacts/btrfs-reference/$p-expect.cpio
.cache/linux-reference/linux-vm .cache/linux-reference/Image \
  artifacts/btrfs-reference/$p-expect.cpio 2 512 \
  'console=hvc0 rdinit=/init panic=-1 loglevel=4' \
  $S/$p-expect.raw > ../btrfs/logs/linux-log-expect-$p.log 2>&1
grep '^BTRFS_LOG_MANIFEST:' ../btrfs/logs/linux-log-expect-$p.log | \
  sed 's/^BTRFS_LOG_MANIFEST://' > ../btrfs/artifacts/fixtures/$p.expected.tsv
```

Require `BTRFS_LOG_CREATED:$p`, then `BTRFS_LOG_PENDING` and
`BTRFS_LOG_EXPECT_PASS`. The manifest keeps the serial console's CRLF line
ends; the checks read it line by line. To verify an image this implementation
replayed, run the `verify` phase on a copy of it the same way and require
`BTRFS_LOG_VERIFY_PASS`; its manifest must equal the expected one, apart from
`.fseventsd` and the root directory's size after a macOS writable mount.

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

`btrfs-fuzz-codec` drives the same decoders: an input's first byte selects the
codec, the next two the output capacity. Seed it with compressed streams of
both codecs (any generator works; libzstd and liblzo2 output in that framing),
keep every finding under `artifacts/fuzz-corpus-codec/regression-*`, and run
it with an RSS limit, since the reference decoders allocate windows:

```sh
.build-fuzz-llvm/btrfs-fuzz-codec -max_total_time=1200 -timeout=5 -rss_limit_mb=4096 \
  -max_len=65536 -jobs=6 -workers=6 -artifact_prefix=artifacts/fuzz-findings-codec/ \
  artifacts/fuzz-corpus-codec
```

A bounded smoke campaign is not exhaustive fuzzing. Preserve and minimize every
crashing input; turn the cause into a deterministic regression before fixing it.

## Native builds and guest acceptance

FSKit requires macOS 26.5 SDK or later and XcodeGen; the deployment target is
macOS 26.4, the release that added `requestedMountOptions`, which a read-only
volume uses to ask FSKit for a read-only mount. Kext builds use Kernel.framework
headers from the selected SDK and enforce the same core stack budget.
The adapter uses the public `page_size` KPI: the SDK 27 `PAGE_SIZE` macro instead
imports `PAGE_SHIFT_CONST`, which the supported 26.5 kernel does not export.

```sh
python3 scripts/build_fskit.py
python3 scripts/build_kext.py --arch arm64e
python3 scripts/build_kext.py --arch x86_64
```

Outputs are unsigned by default. Builds do not install, register, load, attach
devices or change boot policy. FSKit signed builds accept `--team` and optionally
`--provision`; use only the personal identity and provision the filesystem-module
capability in a dedicated guest workflow. When automatic profiles omit the test
guest, create a Mac App Development profile for `org.machlin.btrfs` and for
`org.machlin.btrfs.filesystem` that include the guest's Provisioning UDID (not
its Hardware UUID; read it again after a restore) and pass both, with `--clean`
after a profile change:

```sh
python3 scripts/build_fskit.py --team YOUR_TEAM_ID --provision --clean \
  --app-profile 'Machlin btrfs development devices' \
  --extension-profile 'Machlin btrfs filesystem development devices' \
  --configuration Release --build-number N \
  --derived-data artifacts/fskit-signed/DerivedData
```

A signed build ends with `codesign --verify --deep --strict --all-architectures`.
`--build-number` gives every bundle the same positive build number; increase it
for each replacement. `--archive-path` creates a new Xcode archive (Release by
default) and refuses an existing path. The app, extension, setup utility and
device barrier use hardened runtime; the app and extension share the App Group
`group.org.machlin.btrfs`, which prefixes the barrier's Mach service. No signing
or installation acceptance has been established for this project. Do not use a
different entitlement to pretend filesystem-module authorization exists.

A development profile authorizes only the devices it lists, by Provisioning
UDID (`system_profiler SPHardwareDataType`), and a guest can acquire a new one
while keeping its Tart configuration; AMFI then refuses to launch the app and
extension ("No matching profile found"), and FSKit reports no mountable file
system. A Developer ID export runs on any Mac: archive, then export with an
`ExportOptions.plist` whose `method` is `developer-id`, `teamID` the personal
team and `signingStyle` `automatic`; Xcode selects the Developer ID identity
and all-device profiles, including the extension's FSKit entitlement:

```sh
python3 scripts/build_fskit.py --team TEAM --provision --configuration Release \
  --build-number N --derived-data artifacts/fskit-distribution/DerivedData \
  --archive-path artifacts/fskit-distribution/Machlin-btrfs-N.xcarchive \
  --export-path artifacts/fskit-distribution/export-N \
  --export-options artifacts/fskit-distribution/ExportOptions.plist
```

The export is not notarized; Gatekeeper rejects it after a download, but copies
made with `tart exec` carry no quarantine and run. A replaced barrier daemon
needs `BtrfsDeviceSetup --refresh` and, after a change of signing identity, the
administrator's approval again before writable loads.

Install into `/Applications` of a dedicated guest, enable the module in System
Settings → General → Login Items & Extensions → **By Category → File System
Extensions**, and check FSKit's own state before mounting:

```sh
app='/Applications/Machlin btrfs.app/Contents/MacOS/Machlin btrfs'
"$app" --control modules
"$app" --control device-service setup
"$app" --control device-service
```

Register the copy with `lsregister -f -R -trusted` and `pluginkit -a` on the
extension. After replacing an installed bundle, restart the user's `fskit_agent`:
it keeps the old extension identity, and every probe then fails with
ExtensionKit error 2 until it restarts. Enabling the module takes the toggle in
the category's info sheet; its state shows only in `--control modules`.

`setup` opens the embedded setup utility (`Contents/Helpers/BtrfsDeviceSetup.app`),
which registers the barrier daemon; approve it in System Settings. The utility also
takes `--register`, `--unregister`, `--refresh` and `--status`, refuses to
unregister while a `machlinbtrfs` volume is mounted, and after a bundle update
`--refresh` unregisters and registers again, since registering an enabled service
does not replace its code. Only the app's `device-service` query, which asks the
daemon over its authenticated connection, shows a usable service; registration
status alone does not.

Attach a disposable copy as an ordinary user with
`hdiutil attach -owners on -imagekey diskimage-class=CRawDiskImage IMAGE`
(add `-readonly` for the read suite); Disk Arbitration mounts it through the
module at `/Volumes/LABEL`. Run `btrfs-mounted-test MOUNT --no-seek-hole` (FSKit
26.x has no interface for SEEK_HOLE; the option accepts a refusal or no holes
before the end, as POSIX allows) and, as root,
`btrfs-mounted-write-test write MOUNT --skip-set-id --no-punch-hole`, then
detach, attach again and run `verify` with the same flags; the first skip names
the set-id group, a recorded failure on FSKit 26.x (see ARCHITECTURE.md), and
the manifest omits its files; the second requires `F_PUNCHHOLE` to be refused,
since FSKit has no interface for it, and expects the block it would punch.
`diskutil unmount` is refused while Spotlight holds the volume root; `hdiutil
detach` unmounts it. Check the written image with Linux as for the XNU runs.

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
| `data-*` (`--data`) | Unaligned overwrite of a reflinked extent, append, holes and past-EOF writes, preallocation, zlib, NODATASUM, inline conversion, truncation, snapshot overwrites, overlapping writes; compression on write by property (128 KiB zlib extents, incompressible data, compressed and plain inline files, an overwrite splitting a compressed extent, truncation) and by the zstd mount option (ZSTD feature, `no` property, NODATASUM); `release-*`: a file rewritten as 128 extents shrinks by one release step, committed as an exact prefix whose size the committed state decides within its bounds, then completes, and its unlink leaves an orphan that the next commit's cleanup finishes |
| `namespace-*` (`--namespace`) | Every object type with inherited flags and data, 100 names splitting leaves; appends to, cuts from and renames within colliding DIR_ITEM and xattr items; hard links (across directories, to a device, beside extended references); names beyond a full INODE_REF item in new INODE_EXTREF items, a colliding one and one Linux wrote, unlinked from packed and last entries, renamed into and out of the INODE_REF item and across directories, and an INODE_EXTREF item filled to the largest item; unlinks of shared and last data references; renames across directories, over files, over an empty directory and between names of one inode; open unlinks left as orphans, eviction and orphan cleanup; the largest xattr; a subvolume tree; compression properties and their inheritance; a DIR_ITEM filled to the largest item; zstd by an inherited property; writes in place into Linux's preallocated file (item split) and into an unshared NODATACOW extent, copied on write once a snapshot shares it or another reference exists; 200 one-sector files of which every other one is removed, converting their block group to bitmaps (`fst-bitmaps`); O_TMPFILE files inheriting COMPRESS, linked in or left as orphans for cleanup, with both linking refusals; RENAME_EXCHANGE of files across directories and of a file with two names and a directory, a no-op between two names of one inode and a directory refused below itself; RENAME_WHITEOUT leaving 0:0 whiteouts, also over a replaced target; inode flags as `chattr` sets them on a file, a directory and a symlink, NOCOW on an empty and a written file, COMPR and NOCOMP with their property, and an immutable file refusing a write until its flag goes, with four refusals (`fsflags`) |
| `squota-*` (`--squota`, on `transactions-squota`) | Simple quotas: a subvolume and its snapshot inside `/data` joining 1/100, the subvolume deleted and dropped while the snapshot keeps extents it owns (its qgroup stays); a snapshot into the top level after another change, joining no qgroup, whose writes count for it; a shared extent freed last by the snapshot and taken back from its owner; an extent from before simple quotas freed without effect; a rescan (INVALID_ARGUMENT) and a preallocation past the limit (QUOTA_EXCEEDED) refused; two snapshots dropped with their qgroups. Every state's numbers equal the audit's count of each extent from the enabling generation for its owner, and every such data extent names its owner |
| `twins-*` (`--twins`, on `transactions-twins` and `transactions-16k-twins`) | Compression twins: a copy of every file Linux wrote under `compress=zlib`, `compress-force=zlib` and a zlib property, written in the same steps from Linux's step bytes with the same mount option and property; in every state from the copy's last step its inode flags and extent layout equal its twin's (compressed pieces, inline extents and uncompressed runs), and Linux compares the same layouts in its own tree dumps |
| `verity-*` (`--verity`, on `transactions-verity`; on `transactions` only `verity-feature`) | fs-verity: writes, an inline write, truncation, preallocation and punching of Linux's verity files refused (NOT_PERMITTED), a second enable refused (EXISTS) and enables refused in Linux's order (READ_ONLY in read-only subvolumes, IS_DIRECTORY, INVALID_ARGUMENT for a symlink, block sizes and an unknown algorithm, RANGE for the salt and signature sizes, NOT_PERMITTED for immutable and append-only files); a rename, a link, an xattr, inode flags and an unlink of verity files; enables of new, empty, NODATACOW and Zstd files and of Linux's files, the inline one through its second name, with SHA-256 and SHA-512, 1 KiB and 4 KiB blocks, salts, a signature and steps of 1 to 64 blocks, after which writes and a second enable are refused; two enables interrupted after storing some tree blocks, whose items and orphan items orphan cleanup drops, then an enable and a rollback; on a volume without fs-verity, the first enable sets the feature. Every state's digests and item counts equal a model that builds the tree one level at a time, and every file reads through its verification |
| `quota-*` (`--quota`, on `transactions-quota`) | A new subvolume's qgroup, deleted and dropped with it; a snapshot into the top level and one inside its own source (each its transaction's first change), then files on both sides; a snapshot of a subvolume in 1/100, which leaves quotas inconsistent as Linux does; a snapshot after another change (UNSUPPORTED) and a preallocation past the referenced limit (QUOTA_EXCEEDED), refused before any change beside a write that fits; two shared snapshots dropped, one leaving 1/100; a quota rescan begun after such a snapshot with partial steps, writes between them and a snapshot waiting for it, then finished (`quota-rescan`), the native volume's maintenance finishing a drop and a rescan; and metadata reservations against a nearly full qgroup within one transaction (`quota-metadata`). Every audited state's qgroup numbers equal the independent count of `tests/qgroup_audit.c`, which computes them as `btrfs check` does, during a rescan over the extents below its progress |
| `subvolume-*` (`--subvolume`) | Subvolumes at the top level, in a directory and in another subvolume, inheriting the parent subvolume's compression property; writable and read-only snapshots of a subvolume, of the multi-level top level, of a read-only snapshot and of a snapshot, edited on either side; copied subvolume entries as stubs; deletion of subvolumes, snapshots and a stub entry; the cleaner resuming a partial drop across commits, and fully dropping a subvolume whose leaves a snapshot shares and an unshared one with data; subvolume entries renamed within and across directories and subvolumes, over an empty directory, and exchanged with each other and with inodes, with Linux's refusals (`subvolume-rename`) |
| `random-N` (`--random FIRST COUNT`, `--random-quick FIRST COUNT`) | Seeded differential sequences: 24 operations in three commits drawn from create of every type, link, unlink, rename (also over files and between names of one inode), xattr set/remove, write, truncation, attribute changes, inode flags as `FS_IOC_SETFLAGS` sets them (with the compression property, inheritance and an immutable file refusing a write), subvolumes (created, snapshotted and deleted as a commit's first operation, renamed and exchanged across subvolumes, dropped by the cleaner, with stubs and read-only snapshots), orphan cleanup and expected refusals under `/fuzz`, over a name pool with real CRC32C collisions, plus RENAME_EXCHANGE and RENAME_WHITEOUT; a separate model predicts each stage's namespace facts |

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
`transactions-dup`, `--namespace` or `--subvolume` for
`transactions-namespace`, `--quota` for `transactions-quota`, `--squota` for
`transactions-squota`, `--verity` for `transactions-verity` and `transactions`,
`--twins` for `transactions-twins` and `transactions-16k-twins`, and
`--data --kernel-codecs` for `transactions-data` with the kernel adapter's
encoders writing its compressed extents. Reader
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
absence, mode/owner/link counts, device numbers, inode flags, incompat and
read-only compatible features, fs-verity digests (or their absence, as
`fsverity measure` reports them), whether INODE_REF or INODE_EXTREF holds a name, how many of a file's regular and
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
references (one dump per tree and case), `btrfs subvolume show` for flags and parent UUIDs, and
`btrfs subvolume list` with and without `-d`, and the quota tree's dump for the
status flags, its generation and the number of qgroups; listings use shell
globbing, which keeps every byte of a name),
or a failed mount for a torn primary. With quotas, `btrfs check` also counts
every qgroup's referenced and exclusive bytes from the extent tree and fails on
any difference (a deliberately wrong info item fails it with "Counts for qgroup
id ... are different"). For each
recovery case it runs `btrfs rescue super-recover -y`, requires status 2 and the
same resolved generation and contents, then repeats from the crash state with
this implementation's recovery writes and requires Linux to find every copy
valid. After each scenario's cases Linux mounts its newest root read-write
(cleaning any orphans the scenario left; with a v1 cache, `space_cache=v1`,
which rebuilds the cache this implementation left stale), finishes every subvolume drop this
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
  --mount-helper "$PWD/../btrfs/artifacts/kext/arm64e/mount_machlin_btrfs" \
  --write-probe "$PWD/../btrfs/.build/btrfs-mounted-write-test" \
  --walk-probe "$PWD/../btrfs/.build/btrfs-mounted-walk-test" \
  --image "$PWD/../btrfs/artifacts/fixtures/plain.raw" \
  --image "$PWD/../btrfs/artifacts/fixtures/small-nodes.raw" \
  --image "$PWD/../btrfs/artifacts/fixtures/large-nodes.raw" \
  --image "$PWD/../btrfs/artifacts/fixtures/zlib.raw" \
  --walk-image "$PWD/../btrfs/artifacts/fixtures/transactions-scale.raw" \
  --write-image "$PWD/../btrfs/artifacts/fixtures/plain.raw" \
  --write-image "$PWD/../btrfs/artifacts/fixtures/small-nodes.raw" \
  --output "$PWD/artifacts/btrfs-kext/mounted-new-run"
```

The output directory must be new and inside the shared product directory. The
probe starts as guest root solely to run tests under specified ordinary UIDs;
the filesystem's authorization uses their actual credentials. Every image is
attached read-only, verified by raw-device hash, mounted, tested, normally
unmounted, hashed again and detached. A failed cleanup or missing image is not a
pass. Keep FSKit and LXNU acceptance separate from these native XNU results.

`btrfs-mounted-walk-test MOUNT` walks a whole mounted volume and only reads. Each
name's `d_ino` and `d_type` must match `lstat`, `.` and `..` must name the
directory and its parent, and a number may be shared only by links of one file,
by no more names than its link count. It prints one `inodes` manifest line per
directory: the SHA-256 of the sorted `name<TAB>number` lines, with `-` for an
object of another subvolume. With `--walk-probe`, the runner walks every
`--image` after its contracts and saves `NAME-walk.manifest.tsv`, walks each
`--walk-image` alone, and appends to each `--write-image` manifest the walk of
the final image on a read-only mount, which must leave the raw device unchanged.
A writable mount is not walked for the manifest: macOS daemons may still change
it before unmount (fseventsd removes its own `.fseventsd` then).
`transactions-scale` holds about 100,000 objects, beyond what an earlier fixed
table could number. Linux checks a read-only walk manifest on a disposable copy
of the unchanged fixture as below.

A `--write-image` is copied into the guest and attached writable; its source
fixture must not change. Each `--write-mode` (default: both) uses its own copy:
`grouped` mounts it read-write (`mount_machlin_btrfs -w`), where operations
share a running transaction until `fsync`, `sync`, the commit interval or
unmount commits it, and `synchronous` adds `-s` (`MNT_SYNCHRONOUS`, one commit
per operation). The runner runs `btrfs-mounted-write-test write` (exclusive create, unaligned
overwrite, mmap/pread/pwrite coherence, shrink and grow zeroing, chmod, xattrs,
hard and symbolic links, rename over a file and across directories, a nonempty
rmdir, open-unlink lifetime, file and directory fsync, an 8 MiB file, four
concurrent O_APPEND writers while the name is renamed 200 times, set-id writes
by the superuser and by another user, and filling the device until fsync, or
the write itself on a synchronous mount, reports ENOSPC, then deleting and
writing again), unmounts, mounts again and runs
`verify`, which checks the same facts after the remount and prints a manifest.
The runner saves it as `NAME-MODE.manifest.tsv` and copies the written image
back as `NAME-MODE-written.raw`. Then Linux checks the image macOS wrote, from the lab
directory, on a disposable copy:

```sh
python3 ../btrfs/tests/prepare_native_linux.py \
  --root artifacts/btrfs-reference/root \
  --manifest artifacts/btrfs-kext/mounted-new-run/plain-grouped.manifest.tsv \
  --archive artifacts/btrfs-reference/native-check-plain.cpio --device-bytes 268435456
python3 -c 'import shutil,sys; from pathlib import Path; shutil.copyfileobj(Path(sys.argv[1]).open("rb"), Path(sys.argv[2]).open("xb"))' \
  artifacts/btrfs-kext/mounted-new-run/plain-grouped-written.raw ../btrfs/artifacts/native-plain-work.raw
.cache/linux-reference/linux-vm .cache/linux-reference/Image \
  artifacts/btrfs-reference/native-check-plain.cpio 2 512 \
  'console=hvc0 rdinit=/init panic=-1 loglevel=4' \
  ../btrfs/artifacts/native-plain-work.raw > ../btrfs/logs/linux-native-plain.log 2>&1
```

Repeat with the `synchronous` manifest and image. Require `BTRFS_NATIVE_CHECKS:N`
with the manifest's line count and
`BTRFS_NATIVE_PASS`: `btrfs check --readonly`, every manifest fact (SHA-256,
mode, owner, links, listings, inode numbers of the mounted subvolume and the
subvolume boundary, symlink, xattr, mtime, absent names, inode flags from
Linux's tree dump), then a Linux
read-write mount, write, `btrfs check` again and no orphan item left.

### Native tree-log replay

`run_macos.py` does not drive replay yet; the accepted runs followed these
steps on each pending-log fixture, copied into the guest under a new name and
checked by hash. Through the loaded module: attach with `hdiutil attach
-nomount`, require `mount_machlin_btrfs DEVICE MOUNT` (read-only) to fail with
the image unchanged, mount with `-w`, take a native manifest (path, kind,
`stat -f '%i %Lp %u %g %l %z %m'`, SHA-256 or link target) and run
`btrfs-mounted-walk-test`, unmount and detach. Through installed FSKit: require
`hdiutil attach -readonly` to report no mountable file system with the image
unchanged, then attach read-write, which Disk Arbitration mounts after its
repair request, and take the same manifest and walk. Copy each image back,
compare the manifest with `artifacts/fixtures/*.expected.tsv` (macOS adds
`.fseventsd` and the root's native number is 2), and run the Linux `verify`
phase on a copy. The kernel's replay line is in the unified log
(`log show --predicate 'eventMessage CONTAINS "tree-log replay"'`); the
kernel message buffer is too small to keep it.

### Native power cuts

`tests/power_cut_macos.py` cuts the power of the same prepared guest while the
loaded module writes. It keeps the authoritative image on the host, selects the
Btrfs slot with a short menu timeout (restoring the menu afterwards) and, per
iteration, mounts a new guest copy read-write (grouped and `-s` alternate),
verifies every file acknowledged so far, runs `btrfs-power-cut write`, kills the
virtual machine process after a random delay, boots it again (retrying a start
that finds the killed process still holding the machine's auxiliary storage)
and copies the crash image back. The workload prints a manifest line only after a file and its
directory were synced, and churns unsynchronized renames, unlinks and open
unlinked files around them. `btrfs-inspect IMAGE recover` (no writes) records
whether a cut left disagreeing superblock copies; the next read-write mount must
recover them itself. From the absolute lab directory, with the module loaded:

```sh
python3 ../btrfs/tests/power_cut_macos.py \
  --lab "$PWD" --vm lxnu-btrfs-kext-lab \
  --products "$PWD/artifacts/btrfs-kext" --share btrfs-kext \
  --guest-directory /var/tmp/machlin-btrfs-powercut \
  --image "$PWD/../btrfs/artifacts/fixtures/transactions-convert.raw" \
  --workload "$PWD/../btrfs/.build/btrfs-power-cut" \
  --mount-helper "$PWD/../btrfs/artifacts/kext/arm64e/mount_machlin_btrfs" \
  --inspect "$PWD/../btrfs/.build/btrfs-inspect" \
  --kernel-uuid "$btrfs_test_kernel_uuid" --module-uuid "$btrfs_test_module_uuid" \
  --iterations 8 --seed 2 --linux \
  --output "$PWD/artifacts/btrfs-kext/powercut-new-run"
```

`--linux` checks every crash image with the reference kernel: `btrfs check`,
every fact acknowledged before that cut and a Linux read-write continuation.
Require `PASS power cut` and a passing Linux check for every cut. Each cut
reboots the guest, so allow about two minutes per iteration.

`--adapter fskit` cuts a stock guest with the signed app installed, its module
enabled and its device barrier approved, logged in automatically: Disk
Arbitration mounts each copy through the module (`hdiutil attach -owners on`),
the runner refuses a read-only mount, and root commands read the guest's sudo
password from `--sudo-password-file` through stdin, never from arguments or
logs. It takes no mount helper or kernel identities, and every iteration uses
grouped commits.

For a durability-only run without guest administrator credentials, use
`--adapter fskit --unprivileged` instead of the password option. The runner
attaches its disposable copies with `-owners off` and runs the same workload as
the logged-in user. Acknowledgements still require both fsyncs; the manifest
still records and verifies contents, mode, owner, group and link count in the
guest and Linux. This mode does not test ownership enforcement or set-id
semantics. The guest must still have its signed module and device service
installed and enabled.

Each crash image remains in the host output directory. After its hash matches
the guest file, the runner removes that redundant guest copy before the next
iteration, keeping guest disk use bounded.
