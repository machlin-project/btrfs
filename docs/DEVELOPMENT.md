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
logs. Without `-Dfixtures`, only self-contained wire/API and identity tests run;
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

The six required profiles are `plain`, `small-nodes`, `large-nodes`, `zlib`, `zstd`
and `default-subvolume`. Each uses a separate disposable 256 MiB raw image and the
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
Linux checks and a completed VM exit before consuming the image. Repeat all six
profiles, then run the portable image suite. It hashes each complete image before
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
