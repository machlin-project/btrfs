#!/usr/bin/env python3
"""Prepare a bounded Linux oracle payload; never boot or select a host device."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

PROFILES = {"plain": (16384, "dup", ""), "small-nodes": (4096, "single", ""),
            "large-nodes": (65536, "dup", ""), "zlib": (16384, "dup", "zlib"),
            "zstd": (16384, "dup", "zstd"), "codecs": (16384, "dup", "lzo"),
            "default-subvolume": (16384, "dup", ""),
            "transactions": (4096, "single", ""), "transactions-dup": (16384, "dup", ""),
            "transactions-large": (65536, "dup", ""), "transactions-full": (4096, "single", ""),
            "transactions-shared": (4096, "single", ""), "transactions-keyed": (4096, "single", ""),
            "transactions-data": (4096, "single", ""), "transactions-fst": (16384, "dup", ""),
            "transactions-grow": (16384, "dup", ""),
            "transactions-namespace": (4096, "single", ""),
            "transactions-holes": (4096, "dup", ""),
            "transactions-convert": (4096, "single", ""),
            "transactions-copies": (4096, "single", ""),
            "transactions-scale": (4096, "single", ""),
            "checksums-xxhash": (16384, "dup", ""), "checksums-sha256": (4096, "single", ""),
            "checksums-blake2": (65536, "dup", ""),
            "transactions-metadata-uuid": (4096, "single", ""),
            "transactions-block-group-tree": (16384, "dup", ""),
            "transactions-mixed": (4096, "single", ""),
            "transactions-space-cache": (4096, "single", ""),
            "transactions-quota": (16384, "dup", "")}
# Writable profiles without a free-space tree; transactions-fst keeps mkfs
# defaults and therefore maintains one.
WRITABLE = {"transactions", "transactions-dup", "transactions-large", "transactions-full",
            "transactions-shared", "transactions-keyed", "transactions-data",
            "transactions-holes", "transactions-copies", "checksums-xxhash",
            "checksums-sha256", "checksums-blake2"}
# The holes profile repeats the data payload without NO_HOLES, so Linux writes
# explicit hole items, and with DUP data, so every data sector has two copies;
# it adds a file grown by truncation alone and a NODATACOW directory.
DATA_PROFILES = {"transactions-holes": "dup", "checksums-blake2": "dup"}
MKFS_FEATURES = {"transactions-holes": "-O ^no-holes",
                 "transactions-block-group-tree": "-O block-group-tree",
                 "transactions-mixed": "--mixed",
                 "transactions-space-cache": "-R ^free-space-tree"}
# Format features beyond mkfs defaults, each confirmed in Linux's own dumps: a
# metadata UUID set by btrfstune after Linux wrote the volume (tree blocks keep
# the old fsid, the superblock names a new one), mixed data and metadata
# groups, a v1 free-space cache that Linux keeps current and quotas with a
# limited subvolume carry the data payload. The block-group tree keeps the
# base payload, whose emptied data group the group scenarios remove.
FEATURE_PROFILES = ("transactions-metadata-uuid", "transactions-mixed",
                    "transactions-space-cache", "transactions-quota")
MOUNT_OPTIONS = {"transactions-space-cache": "space_cache=v1"}
# Linux writes no v1 cache for a block group below 100 MiB (cache_save_setup);
# on 1 GiB its data groups are 112 MiB.
SPACE_CACHE_DEVICE_BYTES = 1024 * 1024 * 1024
QUOTA_LIMIT_BYTES = 64 * 1024 * 1024
# A level-1 qgroup holding the data subvolume and its writable snapshot, with an
# exclusive limit of its own.
QUOTA_GROUP = "1/100"
QUOTA_GROUP_LIMIT_BYTES = 96 * 1024 * 1024
# The checksum profiles repeat the data payload under each algorithm other than
# CRC32C, with 16, 4 and 64 KiB nodes; Linux also verifies every data checksum.
CHECKSUMS = {"checksums-xxhash": "xxhash", "checksums-sha256": "sha256",
             "checksums-blake2": "blake2"}
DATA_PAYLOAD = ("transactions-data", "transactions-fst", "transactions-holes", *CHECKSUMS,
                *FEATURE_PROFILES)
HOLES_GROWN_BYTES = 1048576
# The convert profile keeps mkfs defaults (a free-space tree) on 1 GiB, so data
# block groups span enough bitmaps for both of Linux's conversion thresholds.
# Linux removes every other one of these one-sector files, then the rest in
# order, syncing after each removal, and records the free extent count at which
# it converts the group to bitmaps and back.
CONVERT_FILES = 480
CONVERT_FILLER_MIB = 8
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
# Leaf-sized xattrs for 16 KiB nodes in the growth profile.
GROW_XATTR_BYTES = 3800
GROW_ROOM_FILES = 600
# Namespace-writer inputs. Four names share one CRC32C name hash (as directory
# entries, and with the "user." prefix as xattrs): equal-length CRC collisions
# survive equal-length suffixes, so the pairs of one collision form four names.
# Linux writes the first two; the writer adds and removes the others. Two
# directories carry compression properties ("zstd", inherited by new files and
# directories, and "no", which only sets NOCOMPRESS).
NAMESPACE_COLLISIONS = ("ethvq997ethvq997", "ethvq997wdkjavbx", "wdkjavbxethvq997",
                        "wdkjavbxwdkjavbx")
NAMESPACE_DATA_BYTES = 65536
# With 4 KiB nodes one INODE_REF item holds 18 of these names; Linux stores the
# remaining links of the inode as extended references.
EXTREF_NAME_BYTES = 200
EXTREF_LINKS = 40
# The scale profile measures writable admission on a volume with many block
# groups and a large extent tree: many 4 KiB files (one extent each) and a large
# file that fills the low data groups. mkfs defaults keep a free-space tree.
SCALE_DEVICE_BYTES = 2 * 1024 * 1024 * 1024
SCALE_DIRECTORIES = 100
SCALE_FILES = 1000
SCALE_FILE_BYTES = 4096
SCALE_LARGE_MIB = 1024
# Past 256 GiB a device holds the third superblock copy; the image is sparse.
COPIES_DEVICE_BYTES = 257 * 1024 * 1024 * 1024
THIRD_SUPER_OFFSET = 256 * 1024 * 1024 * 1024
DEVICE_BYTES = {"transactions-full": 128 * 1024 * 1024,
                "transactions-holes": 512 * 1024 * 1024,
                "checksums-blake2": 512 * 1024 * 1024,
                "transactions-convert": 1024 * 1024 * 1024,
                "transactions-space-cache": SPACE_CACHE_DEVICE_BYTES,
                "transactions-copies": COPIES_DEVICE_BYTES,
                "transactions-scale": SCALE_DEVICE_BYTES}
# A leaf-sized xattr gives each metadata filler inode its own 4 KiB leaf.
# The codecs profile adds files of an incompressible head and a compressible
# tail in many sizes, under the mount's LZO and, by property, zlib and Zstd: LZO
# segments of varied lengths with headers at many offsets within a sector,
# inline extents, and regular extents whose stream ends with an unaligned file.
CODEC_DIRECTORIES = ("lzo", "zlib", "zstd")
LZO_FILES = 64
LZO_HEAD_STEP = 37
LZO_HEAD_BASE = 50
LZO_TAIL_STEP = 1531
LZO_TAIL_BASE = 100
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
    features = " ".join(part for part in ("-R ^free-space-tree" if profile in WRITABLE else "",
                                          MKFS_FEATURES.get(profile, ""),
                                          f"--csum {CHECKSUMS[profile]}"
                                          if profile in CHECKSUMS else "") if part)
    check_data = "--check-data-csum" if profile in CHECKSUMS else ""
    data_profile = DATA_PROFILES.get(profile, "single")
    if profile in WRITABLE:
        options += ",nospace_cache"
    if profile in MOUNT_OPTIONS:
        options += f",{MOUNT_OPTIONS[profile]}"
    set_default = "btrfs subvolume set-default /mnt/subvol" if profile == "default-subvolume" else ":"
    device_bytes = DEVICE_BYTES.get(profile, 256 * 1024 * 1024)
    len_random = len(contents["random"])
    extra = {}
    fill = ":"
    if profile == "codecs":
        for directory in CODEC_DIRECTORIES:
            for i in range(LZO_FILES):
                extra[f"{directory}/f{i}"] = (
                    contents["random"][:i * LZO_HEAD_STEP + LZO_HEAD_BASE] +
                    contents["big"][:i * LZO_TAIL_STEP + LZO_TAIL_BASE])
        fill = f'''mkdir /mnt/lzo /mnt/zlib /mnt/zstd
btrfs property set /mnt/zlib compression zlib
btrfs property set /mnt/zstd compression zstd
for directory in {" ".join(CODEC_DIRECTORIES)}; do
    i=0
    while [ "$i" -lt {LZO_FILES} ]; do
        {{ head -c $((i * {LZO_HEAD_STEP} + {LZO_HEAD_BASE})) /input/random
           head -c $((i * {LZO_TAIL_STEP} + {LZO_TAIL_BASE})) /input/big; }} > /mnt/$directory/f$i
        i=$((i + 1))
    done
done
btrfs filesystem sync /mnt
btrfs inspect-internal dump-tree -t 5 /dev/vda > /tmp/codecs.txt
for codec in '1 (zlib)' '2 (lzo)' '3 (zstd)'; do
    count=$(grep -c "extent compression $codec" /tmp/codecs.txt || true)
    echo "BTRFS_REFERENCE_REGULAR_EXTENTS:$codec:$count"
    test "$count" -ge {LZO_FILES // 2}
done'''
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
    if profile in DATA_PAYLOAD:
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
    if profile == "transactions-holes":
        fill += f'''
truncate -s {HOLES_GROWN_BYTES} /mnt/data/grown
mkdir /mnt/data/nocow
chattr +C /mnt/data/nocow
lsattr -d /mnt/data/nocow
btrfs filesystem sync /mnt
if btrfs inspect-internal dump-super /dev/vda | grep -qw NO_HOLES; then
    exit 1
fi
btrfs filesystem df /mnt | grep 'Data, DUP'
btrfs inspect-internal dump-tree /dev/vda > /tmp/tree.txt
holes=$(grep -c 'extent data disk byte 0 nr 0' /tmp/tree.txt || true)
echo BTRFS_REFERENCE_HOLE_ITEMS:$holes
test "$holes" -gt 0'''
    if profile == "transactions-convert":
        fill = f'''mkdir /mnt/convert
# The free extent count and flags of the block group holding disk byte $1, and
# its length, from Linux's own free-space tree.
group_of() {{
    btrfs filesystem sync /mnt
    blockdev --flushbufs /dev/vda
    btrfs inspect-internal dump-tree -t free-space /dev/vda | awk -v address="$1" '
        $1 == "item" && $5 == "FREE_SPACE_INFO" {{
            start = substr($4, 2) + 0; size = $6 + 0
            inside = address + 0 >= start && address + 0 < start + size; next }}
        inside && $1 == "free" && $2 == "space" && $3 == "info" {{ print $6, $8, size; exit }}'
}}
dd if=/dev/zero of=/mnt/convert/filler bs=1048576 count={CONVERT_FILLER_MIB} 2>/dev/null
btrfs filesystem sync /mnt
head -c 4096 /input/random > /mnt/convert/anchor
i=0
while [ "$i" -lt {CONVERT_FILES} ]; do
    head -c 4096 /input/random > /mnt/convert/f$(printf '%03d' "$i")
    i=$((i + 1))
done
btrfs filesystem sync /mnt
blockdev --flushbufs /dev/vda
inode=$(stat -c '%i' /mnt/convert/anchor)
address=$(btrfs inspect-internal dump-tree -t 5 /dev/vda | awk -v inode="$inode" '
    $1 == "item" && $3 == "key" {{ inside = substr($4, 2) == inode && $5 == "EXTENT_DATA"; next }}
    inside && $1 == "extent" && $2 == "data" && $3 == "disk" && $4 == "byte" {{ print $5; exit }}')
set -- $(group_of "$address")
bitmaps=$((($3 + 8388607) / 8388608))
high=$((bitmaps * 281 / 25))
low=0
if [ "$high" -gt 100 ]; then
    low=$((high - 100))
fi
echo BTRFS_REFERENCE_CONVERT_GROUP:$address:$3:$high:$low
test "$low" -gt 0
to_bitmaps=
i=1
while [ "$i" -lt {CONVERT_FILES} ]; do
    rm /mnt/convert/f$(printf '%03d' "$i")
    set -- $(group_of "$address")
    if [ -z "$to_bitmaps" ] && [ "$2" = 1 ]; then
        to_bitmaps=$1
    fi
    i=$((i + 2))
done
to_extents=
i=0
while [ -z "$to_extents" ] && [ "$i" -lt {CONVERT_FILES} ]; do
    rm /mnt/convert/f$(printf '%03d' "$i")
    set -- $(group_of "$address")
    if [ "$2" = 0 ]; then
        to_extents=$1
    fi
    i=$((i + 2))
done
echo BTRFS_REFERENCE_FST_TO_BITMAPS:$to_bitmaps
echo BTRFS_REFERENCE_FST_TO_EXTENTS:$to_extents
test "$to_bitmaps" = $((high + 1))
test "$to_extents" = $((low - 1))'''
    if profile == "transactions-scale":
        fill = f'''test "$(blockdev --getsize64 /dev/vda)" = {SCALE_DEVICE_BYTES}
dd if=/dev/zero of=/mnt/large bs=1M count={SCALE_LARGE_MIB} 2>/dev/null
d=0
while [ "$d" -lt {SCALE_DIRECTORIES} ]; do
    mkdir /mnt/d$d
    i=0
    while [ "$i" -lt {SCALE_FILES} ]; do
        head -c {SCALE_FILE_BYTES} /input/random > /mnt/d$d/f$i
        i=$((i + 1))
    done
    d=$((d + 1))
done
btrfs filesystem sync /mnt
echo BTRFS_REFERENCE_SCALE_EXTENTS:$(btrfs inspect-internal dump-tree -t extent /dev/vda | grep -c ' EXTENT_ITEM ')
echo BTRFS_REFERENCE_SCALE_GROUPS:$(btrfs inspect-internal dump-tree -t extent /dev/vda | grep -c ' BLOCK_GROUP_ITEM ')'''
    if profile == "transactions-copies":
        fill = f'''test "$(blockdev --getsize64 /dev/vda)" = {COPIES_DEVICE_BYTES}
btrfs filesystem sync /mnt
btrfs inspect-internal dump-super -s 2 /dev/vda | grep -q '^bytenr[[:space:]]*{THIRD_SUPER_OFFSET}$'
echo BTRFS_REFERENCE_SUPER_COPIES:$(btrfs inspect-internal dump-super -a /dev/vda | grep -c '^superblock: bytenr=')'''
    if profile == "transactions-grow":
        # Fill data, then metadata until Linux reports ENOSPC; delete the data and
        # the newest fillers (whole leaves of room for balance's own transaction),
        # and let balance drop the empty data block groups. Metadata stays nearly full
        # while the device regains unallocated space.
        fill = f'''dd if=/dev/zero of=/mnt/data-fill bs=1M 2>/dev/null || true
btrfs filesystem sync /mnt
mkdir /mnt/meta
pad=$(head -c {GROW_XATTR_BYTES} /dev/zero | tr '\\0' q)
i=0
while printf m > /mnt/meta/f$i 2>/dev/null &&
      setfattr -n user.pad -v "$pad" /mnt/meta/f$i 2>/dev/null; do
    i=$((i + 1))
done
echo BTRFS_REFERENCE_METADATA_FILES:$i
rm -f /mnt/meta/f$i /mnt/data-fill
j=$((i - {GROW_ROOM_FILES}))
while [ "$j" -lt "$i" ]; do
    rm /mnt/meta/f$j
    j=$((j + 1))
done
btrfs filesystem sync /mnt
btrfs balance start -dusage=0 /mnt
btrfs filesystem sync /mnt
btrfs filesystem usage -b /mnt'''
    if profile == "transactions-namespace":
        first, second = NAMESPACE_COLLISIONS[:2]
        fill = f'''mkdir -p /mnt/ns/tree/a/b /mnt/ns/empty /mnt/ns/full /mnt/ns/collide /mnt/ns/nocow \\
    /mnt/ns/compress /mnt/ns/extref
printf 'one\\n' > /mnt/ns/one
ln /mnt/ns/one /mnt/ns/tree/one-link
head -c {NAMESPACE_DATA_BYTES} /input/random > /mnt/ns/data
cp --reflink=always /mnt/ns/data /mnt/ns/clone
printf 'victim\\n' > /mnt/ns/victim
printf 'inner\\n' > /mnt/ns/full/inner
printf 'deep\\n' > /mnt/ns/tree/a/b/deep
printf 'first\\n' > /mnt/ns/collide/{first}
printf 'second\\n' > /mnt/ns/collide/{second}
setfattr -n user.{first} -v first /mnt/ns/one
setfattr -n user.{second} -v second /mnt/ns/one
chattr +C /mnt/ns/nocow
chattr +c /mnt/ns/compress
mkdir /mnt/ns/zstd /mnt/ns/nocompress
setfattr -n btrfs.compression -v zstd /mnt/ns/zstd
setfattr -n btrfs.compression -v no /mnt/ns/nocompress
mkfifo /mnt/ns/fifo
mknod /mnt/ns/null c 1 3
printf 'target\\n' > /mnt/ns/extref/target
long=$(head -c {EXTREF_NAME_BYTES} /dev/zero | tr '\\0' x)
i=0
while [ "$i" -lt {EXTREF_LINKS} ]; do
    ln /mnt/ns/extref/target /mnt/ns/extref/l$(printf '%03d' "$i")$long
    i=$((i + 1))
done
test "$(stat -c '%h' /mnt/ns/extref/target)" = {EXTREF_LINKS + 1}
lsattr -d /mnt/ns/nocow /mnt/ns/compress /mnt/ns/zstd /mnt/ns/nocompress
getfattr -n btrfs.compression /mnt/ns/zstd /mnt/ns/nocompress
btrfs filesystem sync /mnt
btrfs inspect-internal dump-tree -t 5 /dev/vda > /tmp/fs.txt
echo BTRFS_REFERENCE_EXTREF_ITEMS:$(grep -c 'INODE_EXTREF' /tmp/fs.txt || true)'''
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
    after = ":"
    if profile == "transactions-metadata-uuid":
        after = '''old=$(btrfs inspect-internal dump-super /dev/vda | awk '$1 == "fsid" {print $2}')
btrfstune -m /dev/vda
btrfs inspect-internal dump-super /dev/vda > /tmp/super.txt
grep -E '^(fsid|metadata_uuid|incompat_flags)' /tmp/super.txt
grep -qw METADATA_UUID /tmp/super.txt
test "$(awk '$1 == "metadata_uuid" {print $2}' /tmp/super.txt)" = "$old"
test "$(awk '$1 == "fsid" {print $2}' /tmp/super.txt)" != "$old"
btrfs check --readonly /dev/vda
mount -t btrfs -o ro /dev/vda /mnt
(cd /mnt && sha256sum -c /input/SHA256SUMS)
umount /mnt'''
    if profile == "transactions-block-group-tree":
        after = '''btrfs inspect-internal dump-super /dev/vda | grep -w BLOCK_GROUP_TREE
echo BTRFS_REFERENCE_GROUP_TREE_ITEMS:$(btrfs inspect-internal dump-tree -t 11 /dev/vda | grep -c ' BLOCK_GROUP_ITEM ')
test "$(btrfs inspect-internal dump-tree -t extent /dev/vda | grep -c ' BLOCK_GROUP_ITEM ' || true)" = 0'''
    if profile == "transactions-mixed":
        fill += '''
btrfs filesystem df /mnt | grep -F Data+Metadata'''
        after = '''btrfs inspect-internal dump-super /dev/vda | grep -w MIXED_GROUPS'''
    if profile == "transactions-space-cache":
        after = '''btrfs inspect-internal dump-super /dev/vda > /tmp/super.txt
grep -E '^(generation|cache_generation|compat_ro_flags)' /tmp/super.txt
test "$(awk '$1 == "cache_generation" {print $2}' /tmp/super.txt)" = \\
     "$(awk '$1 == "generation" {print $2}' /tmp/super.txt)"
caches=$(btrfs inspect-internal dump-tree -t root /dev/vda | grep -c 'key (FREE_SPACE UNTYPED' || true)
echo BTRFS_REFERENCE_SPACE_CACHES:$caches
test "$caches" -gt 0'''
    if profile == "transactions-quota":
        fill += f'''
btrfs quota enable /mnt
btrfs qgroup create {QUOTA_GROUP} /mnt
btrfs qgroup assign --no-rescan 0/$(btrfs inspect-internal rootid /mnt/data) {QUOTA_GROUP} /mnt
btrfs qgroup assign --no-rescan 0/$(btrfs inspect-internal rootid /mnt/data-snap) {QUOTA_GROUP} /mnt
btrfs qgroup limit {QUOTA_LIMIT_BYTES} /mnt/data
btrfs qgroup limit -e {QUOTA_GROUP_LIMIT_BYTES} {QUOTA_GROUP} /mnt
btrfs filesystem sync /mnt
btrfs quota rescan -w /mnt
btrfs qgroup show -pcre --raw /mnt'''
        after = '''btrfs inspect-internal dump-tree -t quota /dev/vda > /tmp/quota.txt
grep -c 'QGROUP_INFO' /tmp/quota.txt
grep -q 'QGROUP_LIMIT' /tmp/quota.txt
test "$(grep -c 'QGROUP_RELATION' /tmp/quota.txt)" = 4
grep -A1 'QGROUP_STATUS' /tmp/quota.txt
! grep -q INCONSISTENT /tmp/quota.txt'''
    init = f'''#!/bin/busybox sh
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
mkdir -p /dev /proc /sys /tmp /mnt
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
trap 'echo BTRFS_REFERENCE_FAIL; dmesg | tail -60; sync; poweroff -f' EXIT
for module in virtio_blk xor-neon xor raid6_pq crc32c_generic libcrc32c xxhash_generic blake2b_generic btrfs; do
    insmod /modules/$module.ko
done
uname -r
mkfs.btrfs --version
mkfs.btrfs -f -s 4096 -n {node_size} -m {metadata} -d {data_profile} {features} -L machlin-btrfs /dev/vda
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
btrfs check --readonly {check_data} /dev/vda
{after}
btrfs inspect-internal dump-super /dev/vda | grep '^csum_type'
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
                "data": data_profile, "checksum": CHECKSUMS.get(profile, "crc32c"),
                "compression": compression, "device_bytes": device_bytes, "files": {
                    name: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                    for name, data in {**contents, **extra}.items()}}
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
