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
and `default-subvolume`. The transaction suites require seven writable profiles
without a free-space tree and mounted with `nospace_cache`, plus
`transactions-fst` with mkfs and mount defaults (16 KiB nodes, DUP metadata and a
free-space tree): `transactions` (4 KiB
nodes, single metadata), `transactions-dup` (16 KiB nodes, DUP metadata),
`transactions-large` (64 KiB nodes, DUP metadata), `transactions-full` (4 KiB
nodes, single metadata, 128 MiB), and `transactions-shared`, `transactions-keyed`
and `transactions-data` (4 KiB nodes, single metadata). The full profile first fills all unallocated
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
other one so Linux keeps that data block group's free space as bitmaps. Other
profiles use 256 MiB. Each uses a separate disposable
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
Linux checks and a completed VM exit before consuming the image. Repeat all fourteen
profiles, then run the portable image and transaction suites. It hashes each complete image before
and after reading, verifies 312 contracts, and fails if any byte changed.

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
meson compile -C .build-tsan btrfs-concurrent
env -i PATH="$PATH" meson test -C .build-tsan --no-rebuild concurrent-readers --print-errorlogs
```

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
scenarios on a recorded device. It never writes the source fixture.

| Scenario | Operations |
| --- | --- |
| `replace` | Replace `/greeting` |
| `empty` | Zero-length replacement removes the inline extent |
| `maximum` | 2048-byte inline payload |
| `batch` | `/greeting` and 19 `/many` entries, growing items in several leaves |
| `repeated` | Two commits; the second starts from the first Machlin root set |
| `shared-*`, `pair-convert` (`--shared`) | Writes in a snapshot source, its writable snapshot, alternately, and across three trees; leaves with reflinked and offset data references; FULL_BACKREF conversion and release |
| `keyed-*` (`--keyed`) | The same decisions on blocks and extents whose references are partly keyed items |
| `fst-*` (`--fragment`) | Frees between bitmap holes and a write that spills from extent-mode free space into bitmap holes |
| `data-*` (`--data`) | Unaligned overwrite of a reflinked extent, append, holes and past-EOF writes, preallocation, zlib, NODATASUM, inline conversion, truncation, snapshot overwrites, overlapping writes |

`btrfs-reference-audit` is an independent reference oracle in the portable
suite. It walks every tree from the superblock and root items, derives the
backreference every parent pointer and file extent item requires (shared forms
under FULL_BACKREF parents), and compares the multiset and totals exactly with
the extent tree, including leaks. It runs on every Linux fixture, after every
scenario commit, and against deliberately damaged references.

Each scenario's last commit runs allocation, read, write and barrier fault
points: every point up to 512 per class, and a deterministic stride that keeps
the first and last point for larger commits. Every recorded commit is then cut at each issue-order prefix and in each
barrier epoch: 32 seeded metadata states persist arbitrary subsets of whole,
missing, sector-subset or torn writes; each superblock epoch tries six tear
patterns. Every state is classified by mounting the primary and by explicit
recovery. It must resolve to the acknowledged or new stage with exact contents,
invariant snapshots/xattrs/links, and admit the next transaction. The suite also
checks admission, stale copies, recovery refusals and checksum-correct damaged
allocation maps. `--full` adds the metadata-exhaustion case: the full profile's
remaining metadata cannot hold a batch of all inline files, so the edit returns
NO_SPACE without any write.

Export a profile's crash cases into a new generated directory. Pass the same
profile flag Meson uses: `--full` for `transactions-full`, `--shared` for
`transactions-shared`, `--keyed` for `transactions-keyed`, `--data` for
`transactions-data` and `--data --fragment` for `transactions-fst`. Reader
profiles with a free-space tree (`plain`, `small-nodes`) also run the default
scenarios:

```sh
mkdir artifacts/transaction-plan-transactions
.build/btrfs-transaction-test --export artifacts/transaction-plan-transactions \
  artifacts/fixtures/transactions.raw
```

Each scenario directory holds write payloads (`writes.tsv`), per-stage expected
contents (`stages.tsv`), and one sector-run list per case plus the superblock
writes this implementation's recovery chose. These are generated test inputs, not
a source ledger. About sixteen prefixes, ten metadata states and all superblock
tear patterns are exported per commit; the portable test checks all of them.

Reserve the Linux runner and stop the macOS test guest first. The oracle needs the
two-disk runner built from lab `scripts/linux-vm.swift` (the lab caches it as
`.cache/linux-reference/linux-vm-external`). From the absolute lab directory,
create a disposable working copy and a pristine copy without overwriting existing
files, then run (use 134217728 bytes for `transactions-full`):

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
`btrfs check --readonly`, the primary generation and exact tracked contents plus
invariants for a valid primary, or a failed mount for a torn primary. For each
recovery case it runs `btrfs rescue super-recover -y`, requires status 2 and the
same resolved generation and contents, then repeats from the crash state with
this implementation's recovery writes and requires Linux to find every copy
valid. Finally Linux mounts the last scenario's newest root read-write, writes,
syncs and passes `btrfs check`. Require `BTRFS_TRANSACTION_PASS:N` with the case
count printed by the preparer and in its JSON, and an unchanged pristine copy and
fixture. The oracle never runs `btrfs check --repair`. It proves on-disk
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
