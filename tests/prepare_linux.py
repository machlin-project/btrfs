#!/usr/bin/env python3
"""Prepare a bounded Linux oracle payload; never boot or select a host device."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

PROFILES = {"plain": (16384, "dup", ""), "small-nodes": (4096, "single", ""),
            "large-nodes": (65536, "dup", ""), "zlib": (16384, "dup", "zlib"),
            "zstd": (16384, "dup", "zstd"), "default-subvolume": (16384, "dup", ""),
            "transactions": (4096, "single", ""), "transactions-dup": (16384, "dup", ""),
            "transactions-large": (65536, "dup", ""), "transactions-full": (4096, "single", ""),
            "transactions-shared": (4096, "single", ""), "transactions-keyed": (4096, "single", ""),
            "transactions-data": (4096, "single", ""), "transactions-fst": (16384, "dup", "")}
# Writable profiles without a free-space tree; transactions-fst keeps mkfs
# defaults and therefore maintains one.
WRITABLE = {"transactions", "transactions-dup", "transactions-large", "transactions-full",
            "transactions-shared", "transactions-keyed", "transactions-data"}
# Enough inline files for a level-2 subvolume tree with 4 KiB nodes, so that a
# snapshot shares internal nodes as well as leaves.
SHARED_INLINE_FILES = 1600
SHARED_DATA_FILES = 64
SHARED_DATA_BYTES = 8192
SHARED_SPAN_BYTES = 262144
SHARED_SPAN_MIDDLE = 131072
PAIR_FILES = 120
# More referencing snapshots and reflinks than one 4 KiB extent item can list
# inline, so Linux also stores keyed backreference items.
KEYED_FILES = 40
KEYED_SNAPSHOTS = 30
KEYED_REFLINKS = 30
# Data-writer inputs: one multi-sector extent, an unaligned tail, holes, an
# unwritten preallocation, a compressed extent, a no-checksum file, an inline
# file and a reflink, then a writable and a read-only snapshot.
DATA_BIG_BYTES = 1048576
DATA_SMALL_BYTES = 10000
DATA_SPARSE_BYTES = 4194304
DATA_PREALLOC_BYTES = 262144
DATA_ZLIB_BYTES = 262144
DATA_NODATASUM_BYTES = 65536
# Enough freed holes in one data block group that Linux switches its free
# space to bitmaps.
FRAGMENT_FILES = 256
FRAGMENT_BYTES = 4096
DEVICE_BYTES = {"transactions-full": 128 * 1024 * 1024}
# A leaf-sized xattr gives each metadata filler inode its own 4 KiB leaf.
FILL_XATTR_BYTES = 3800
FILL_REMOVE_STRIDE = 7


def prepare(root: Path, profile: str, archive: Path) -> None:
    node_size, metadata, compression = PROFILES[profile]
    inputs = root / "input"
    inputs.mkdir(parents=True, exist_ok=True)
    contents = {
        "greeting": b"hello from Linux Btrfs\n",
        "big": bytes(range(256)) * 16384,
        "random": hashlib.shake_256(b"Machlin independent Btrfs fixture").digest(262144),
    }
    for name, data in contents.items():
        (inputs / name).write_bytes(data)
    (inputs / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in contents.items()))
    options = f"compress-force={compression}" if compression else "compress=no"
    features = "-R ^free-space-tree" if profile in WRITABLE else ""
    if profile in WRITABLE:
        options += ",nospace_cache"
    set_default = "btrfs subvolume set-default /mnt/subvol" if profile == "default-subvolume" else ":"
    device_bytes = DEVICE_BYTES.get(profile, 256 * 1024 * 1024)
    len_random = len(contents["random"])
    fill = ":"
    if profile == "transactions-full":
        # Exhaust unallocated space with data, then metadata with inline files,
        # then free every seventh filler so the remaining free space is scattered.
        fill = f'''test "$(blockdev --getsize64 /dev/vda)" = {device_bytes}
dd if=/dev/zero of=/mnt/data-fill bs=1M 2>/dev/null || true
btrfs filesystem sync /mnt
mkdir /mnt/meta
pad=$(head -c {FILL_XATTR_BYTES} /dev/zero | tr '\\0' p)
i=0
while printf m > /mnt/meta/f$i 2>/dev/null &&
      setfattr -n user.pad -v "$pad" /mnt/meta/f$i 2>/dev/null; do
    i=$((i + 1))
done
echo BTRFS_REFERENCE_METADATA_FILES:$i
test "$i" -gt {FILL_REMOVE_STRIDE * 16}
rm -f /mnt/meta/f$i
j=0
while [ "$j" -lt "$i" ]; do
    if [ $((j % {FILL_REMOVE_STRIDE})) -eq 0 ]; then
        rm /mnt/meta/f$j
    fi
    j=$((j + 1))
done
btrfs filesystem sync /mnt
btrfs filesystem df /mnt'''
    if profile == "transactions-shared":
        # Snapshots share leaves and internal nodes; a reflink shares a data
        # extent between inodes; a partial overwrite leaves two references to
        # one extent at different extent offsets. The inline tail file is the
        # newest inode, so its leaf also holds those file extent items. The pair
        # subvolume has exactly one writable snapshot and interleaves inline and
        # data files, so leaves shared by two trees also hold data references.
        # Nothing reads the pair after its snapshot, so access times stay put.
        fill = f'''btrfs subvolume create /mnt/shared
mkdir /mnt/shared/inline /mnt/shared/data
i=0
while [ "$i" -lt {SHARED_INLINE_FILES} ]; do
    printf 'inline %s\\n' "$i" > /mnt/shared/inline/f$(printf '%04d' "$i")
    i=$((i + 1))
done
i=0
while [ "$i" -lt {SHARED_DATA_FILES} ]; do
    dd if=/input/random of=/mnt/shared/data/d$(printf '%02d' "$i") bs={SHARED_DATA_BYTES} count=1 \\
        skip=$((i % ({len_random} / {SHARED_DATA_BYTES}))) 2>/dev/null
    i=$((i + 1))
done
dd if=/input/random of=/mnt/shared/span bs={SHARED_SPAN_BYTES} count=1 2>/dev/null
btrfs filesystem sync /mnt
printf MIDDLE | dd of=/mnt/shared/span bs=1 seek={SHARED_SPAN_MIDDLE} conv=notrunc 2>/dev/null
btrfs filesystem sync /mnt
cp --reflink=always /mnt/shared/data/d00 /mnt/shared/reflink
printf 'tail\\n' > /mnt/shared/tail
btrfs filesystem sync /mnt
btrfs subvolume create /mnt/pair
i=0
while [ "$i" -lt {PAIR_FILES} ]; do
    dd if=/input/random of=/mnt/pair/d$(printf '%03d' "$i") bs={SHARED_DATA_BYTES} count=1 \\
        skip=$((i % ({len_random} / {SHARED_DATA_BYTES}))) 2>/dev/null
    printf 'pair %s\\n' "$i" > /mnt/pair/i$(printf '%03d' "$i")
    i=$((i + 1))
done
btrfs filesystem sync /mnt
btrfs subvolume snapshot /mnt/pair /mnt/pair-snap
btrfs subvolume snapshot /mnt/shared /mnt/shared-snap
btrfs subvolume snapshot -r /mnt/shared /mnt/shared-ro
cmp /mnt/shared/reflink /mnt/shared/data/d00
test "$(dd if=/mnt/shared-ro/span bs=1 skip={SHARED_SPAN_MIDDLE} count=6 2>/dev/null)" = MIDDLE
test "$(cat /mnt/shared-snap/inline/f1599)" = 'inline 1599'
btrfs filesystem sync /mnt
btrfs inspect-internal dump-tree -t extent /dev/vda > /tmp/extent.txt
echo BTRFS_REFERENCE_SHARED_BLOCK_REFS:$(grep -c 'shared block backref' /tmp/extent.txt || true)
echo BTRFS_REFERENCE_SHARED_DATA_REFS:$(grep -c 'shared data backref' /tmp/extent.txt || true)
echo BTRFS_REFERENCE_FULL_BACKREF:$(grep -c 'FULL_BACKREF' /tmp/extent.txt || true)'''
    if profile == "transactions-keyed":
        fill = f'''btrfs subvolume create /mnt/keyed
i=0
while [ "$i" -lt {KEYED_FILES} ]; do
    dd if=/input/random of=/mnt/keyed/d$(printf '%02d' "$i") bs={SHARED_DATA_BYTES} count=1 \\
        skip=$((i % ({len_random} / {SHARED_DATA_BYTES}))) 2>/dev/null
    printf 'keyed %s\\n' "$i" > /mnt/keyed/i$(printf '%02d' "$i")
    i=$((i + 1))
done
dd if=/input/random of=/mnt/keyed/origin bs={SHARED_DATA_BYTES} count=1 2>/dev/null
btrfs filesystem sync /mnt
i=0
while [ "$i" -lt {KEYED_REFLINKS} ]; do
    cp --reflink=always /mnt/keyed/origin /mnt/keyed/r$(printf '%02d' "$i")
    i=$((i + 1))
done
printf 'last\\n' > /mnt/keyed/last
btrfs filesystem sync /mnt
i=0
while [ "$i" -lt {KEYED_SNAPSHOTS} ]; do
    btrfs subvolume snapshot /mnt/keyed /mnt/keyed-$(printf '%02d' "$i") > /dev/null
    i=$((i + 1))
done
btrfs filesystem sync /mnt
cmp /mnt/keyed-29/r29 /mnt/keyed/origin
btrfs inspect-internal dump-tree -t extent /dev/vda > /tmp/extent.txt
echo BTRFS_REFERENCE_KEYED_REFS:$(grep -cE 'key \\([0-9]+ (TREE_BLOCK_REF|EXTENT_DATA_REF|SHARED_BLOCK_REF|SHARED_DATA_REF) ' /tmp/extent.txt || true)'''
    if profile in ("transactions-data", "transactions-fst"):
        fill = f'''btrfs subvolume create /mnt/data
head -c {DATA_BIG_BYTES} /input/big > /mnt/data/big
head -c {DATA_SMALL_BYTES} /input/random > /mnt/data/small
truncate -s {DATA_SPARSE_BYTES} /mnt/data/sparse
printf HOLE-A | dd of=/mnt/data/sparse bs=1 seek=1048576 conv=notrunc 2>/dev/null
printf HOLE-B | dd of=/mnt/data/sparse bs=1 seek=3145728 conv=notrunc 2>/dev/null
fallocate -l {DATA_PREALLOC_BYTES} /mnt/data/prealloc
touch /mnt/data/zlib
btrfs property set /mnt/data/zlib compression zlib
head -c {DATA_ZLIB_BYTES} /input/big > /mnt/data/zlib
touch /mnt/data/nodatasum
chattr +C /mnt/data/nodatasum
head -c {DATA_NODATASUM_BYTES} /input/random > /mnt/data/nodatasum
printf 'inline data\\n' > /mnt/data/inline
btrfs filesystem sync /mnt
cp --reflink=always /mnt/data/big /mnt/data/big-clone
btrfs filesystem sync /mnt
btrfs subvolume snapshot /mnt/data /mnt/data-snap > /dev/null
btrfs subvolume snapshot -r /mnt/data /mnt/data-ro > /dev/null
btrfs filesystem sync /mnt
btrfs inspect-internal dump-tree /dev/vda > /tmp/tree.txt
echo BTRFS_REFERENCE_DATA_COMPRESSED:$(grep -c 'compression 1 (zlib)' /tmp/tree.txt || true)
echo BTRFS_REFERENCE_DATA_PREALLOC:$(grep -c 'prealloc' /tmp/tree.txt || true)
lsattr /mnt/data/nodatasum'''
    if profile == "transactions-fst":
        fill += f'''
mkdir /mnt/fragment
i=0
while [ "$i" -lt {FRAGMENT_FILES} ]; do
    head -c {FRAGMENT_BYTES} /input/random > /mnt/fragment/f$(printf '%03d' "$i")
    i=$((i + 1))
done
btrfs filesystem sync /mnt
i=0
while [ "$i" -lt {FRAGMENT_FILES} ]; do
    rm /mnt/fragment/f$(printf '%03d' "$i")
    i=$((i + 2))
done
btrfs filesystem sync /mnt
btrfs inspect-internal dump-tree -t 10 /dev/vda > /tmp/free-space.txt
echo BTRFS_REFERENCE_FREE_SPACE_BITMAPS:$(grep -c 'FREE_SPACE_BITMAP' /tmp/free-space.txt || true)
echo BTRFS_REFERENCE_FREE_SPACE_EXTENTS:$(grep -c 'FREE_SPACE_EXTENT' /tmp/free-space.txt || true)'''
    init = f'''#!/bin/busybox sh
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
mkdir -p /dev /proc /sys /tmp /mnt
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
trap 'echo BTRFS_REFERENCE_FAIL; dmesg | tail -60; sync; poweroff -f' EXIT
for module in virtio_blk xor-neon xor raid6_pq crc32c_generic libcrc32c btrfs; do
    insmod /modules/$module.ko
done
uname -r
mkfs.btrfs --version
mkfs.btrfs -f -s 4096 -n {node_size} -m {metadata} -d single {features} -L machlin-btrfs /dev/vda
mount -t btrfs -o {options} /dev/vda /mnt
cp /input/greeting /input/big /input/random /mnt/
chmod 0640 /mnt/greeting
chown 1001:1002 /mnt/greeting
ln /mnt/greeting /mnt/hardlink
ln -s greeting /mnt/symlink
setfattr -n user.text -v 'Linux xattr' /mnt/greeting
setfattr -n user.binary -v 0x00017fff /mnt/greeting
mkdir /mnt/many
i=0
while [ "$i" -lt 700 ]; do
    printf '%s\\n' "$i" > /mnt/many/entry-$(printf '%04d' "$i")
    i=$((i + 1))
done
truncate -s 8388608 /mnt/sparse
printf LEFT | dd of=/mnt/sparse bs=1 seek=17 conv=notrunc 2>/dev/null
printf RIGHT | dd of=/mnt/sparse bs=1 seek=7340035 conv=notrunc 2>/dev/null
fallocate -l 1048576 /mnt/preallocated
truncate -s 17179869191 /mnt/huge
printf 'raw name\\n' > /mnt/$(printf 'raw-\\377')
btrfs subvolume create /mnt/subvol
printf 'snapshot original\\n' > /mnt/subvol/value
btrfs subvolume snapshot -r /mnt/subvol /mnt/snapshot
printf 'subvolume changed\\n' > /mnt/subvol/value
{set_default}
{fill}
cd /mnt
sha256sum -c /input/SHA256SUMS
test "$(stat -c '%a:%u:%g:%h' greeting)" = '640:1001:1002:2'
test "$(cat snapshot/value)" = 'snapshot original'
test "$(cat subvol/value)" = 'subvolume changed'
test "$(readlink symlink)" = greeting
getfattr -d greeting
btrfs filesystem sync /mnt
cd /
umount /mnt
btrfs check --readonly /dev/vda
btrfs inspect-internal dump-tree -t fs /dev/vda > /tmp/tree.txt
grep 'compression' /tmp/tree.txt | sort | uniq -c
echo BTRFS_REFERENCE_PASS:{profile}
trap - EXIT
poweroff -f
'''
    (root / "init").write_text(init)
    (root / "init").chmod(0o755)
    paths = subprocess.run(["find", ".", "-print"], cwd=root, check=True, capture_output=True).stdout
    archive.parent.mkdir(parents=True, exist_ok=True)
    with archive.open("wb") as output:
        subprocess.run(["/usr/bin/cpio", "-o", "-H", "newc"], cwd=root,
                       input=paths, stdout=output, check=True)
    manifest = {"profile": profile, "node_size": node_size, "metadata": metadata,
                "compression": compression, "device_bytes": device_bytes, "files": {
                    name: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                    for name, data in contents.items()}}
    archive.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="Disposable staged Alpine initrd root")
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.root.resolve(), args.profile, args.archive.resolve())


if __name__ == "__main__":
    main()
