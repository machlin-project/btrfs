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
and `default-subvolume`. The transaction suite also requires `transactions`, which
uses 4 KiB nodes, single metadata, no free-space tree and `nospace_cache`. Each uses a separate disposable 256 MiB raw image and the
payload in `tests/prepare_linux.py`. The payload formats **guest `/dev/vda`**, fills
files, takes a snapshot, verifies Linux-visible contents, unmounts, and requires
`btrfs check --readonly` to succeed. Never attach a valuable image to this payload.

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
`plain` consistently. The image creation uses exclusive mode to avoid truncating
an existing fixture. Archives and manifests go to generated directories.

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
Linux checks and a completed VM exit before consuming the image. Repeat all seven
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
currently stay read-only. The image transaction test uses a recorded write device
with explicit volatile/durable state. It never writes the source fixture. Export
its accepted transaction into a new generated directory:

```sh
mkdir artifacts/transaction-plan
.build/btrfs-transaction-test artifacts/fixtures/transactions.raw artifacts/transaction-plan
```

The export is a TSV of physical offsets/lengths and separate binary write payloads.
These are test artifacts, not a source revision ledger. The test requires all
allocation/read/write/flush fault points to satisfy its contract, then verifies
whole-write prefixes, seeded partial metadata, independent mirror persistence,
abort/no-op, and zero-length replacement. A torn primary is rejected, not repaired.

Reserve the Linux runner and stop the macOS test guest first. From the absolute
lab directory, create a disposable copy without overwriting an existing artifact:

```sh
python3 -c 'from pathlib import Path; import shutil; s=Path("../btrfs/artifacts/fixtures/transactions.raw"); d=Path("../btrfs/artifacts/transaction-linux.raw"); shutil.copyfileobj(s.open("rb"), d.open("xb"))'
python3 ../btrfs/tests/prepare_transactions_linux.py \
  --root artifacts/btrfs-reference/root --plan ../btrfs/artifacts/transaction-plan \
  --archive artifacts/btrfs-reference/transaction-check.cpio
.cache/linux-reference/linux-vm .cache/linux-reference/Image \
  artifacts/btrfs-reference/transaction-check.cpio 2 512 \
  'console=hvc0 rdinit=/init panic=-1 loglevel=4' \
  ../btrfs/artifacts/transaction-linux.raw > ../btrfs/logs/linux-transactions.log 2>&1
```

Require the exact pass marker with the exported number of writes plus one, each
Linux read-only fsck, each mounted old/new data check and the final Linux
read-write commit/fsck. Check the original fixture hash before and after.
The oracle writes only the disposable VM disk; it restores affected ranges
between crash cases. It does not use `btrfs check --repair`. It proves on-disk
compatibility for that transaction; actual native flush durability is a separate
gate once native write callbacks exist.

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
