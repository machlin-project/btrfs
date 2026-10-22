#!/usr/bin/env python3
"""Prepare btrfs-progs payloads for sector sizes no available Linux kernel
mounts; never boot or select a host device.

Sectors of 64 KiB need a kernel whose pages are at least as large, which no
Apple CPU runs. btrfs-progs writes and checks such filesystems in user space:

create: stage the base payload of tests/prepare_linux.py in a directory (files
with xattrs, an owner and a mode, a hard link, a symlink, 700 entries, a
sparse file, a raw name) with a writable subvolume `subvol` and a read-only
subvolume `snapshot`, format /dev/vda from it with `mkfs.btrfs --rootdir`,
run `btrfs check --readonly --check-data-csum` and print the staged manifest.

verify: on an image this implementation wrote, run the same check, print the
top level's file extents, extract every file with `btrfs restore` (xattrs and
symlinks included) and print its manifest.

The manifest names every regular file and symlink below the root with its
mode, owner, size and SHA-256 (or symlink target), one line each, sorted.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

DEVICE_BYTES = 512 << 20
SECTOR_SIZES = {"sectors-64k": 65536}
NODE_SIZE = 65536
MANY_ENTRIES = 700
SPARSE_BYTES = 8388608
SPARSE_LEFT = 17
SPARSE_RIGHT = 7340035
INPUTS = {
    "greeting": b"hello from Linux Btrfs\n",
    "big": bytes(range(256)) * 16384,
    "random": hashlib.shake_256(b"Machlin independent Btrfs fixture").digest(262144),
}

PRELUDE = """#!/bin/busybox sh
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
mkdir -p /dev /proc /sys /tmp /mnt
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
trap 'echo BTRFS_PROGS_FAIL; dmesg | tail -60; sync; poweroff -f' EXIT
for module in $(cat /modules/order); do
    insmod /modules/$module.ko
done
exec < /dev/hvc0 > /dev/hvc0 2>&1
uname -r
mkfs.btrfs --version
mount -t tmpfs -o size=256m tmpfs /tmp
"""

MANIFEST = r"""
manifest() {
    cd "$1"
    find . \( -type f -o -type l \) | LC_ALL=C sort | while IFS= read -r path; do
        if [ -L "$path" ]; then
            content=$(readlink "$path")
        else
            content=$(sha256sum "$path" | cut -d' ' -f1)
        fi
        printf 'BTRFS_PROGS_MANIFEST:%s\t%s\t%s\n' "$path" "$(stat -c '%f:%u:%g:%s' "$path")" \
            "$content"
    done
    cd /
}
"""


def create_init(sector: int) -> str:
    return PRELUDE + MANIFEST + f"""
root=/tmp/root
mkdir -p $root/many $root/subvol $root/snapshot
cp /input/greeting /input/big /input/random $root/
chmod 0640 $root/greeting
chown 1001:1002 $root/greeting
ln $root/greeting $root/hardlink
ln -s greeting $root/symlink
setfattr -n user.text -v 'Linux xattr' $root/greeting
setfattr -n user.binary -v 0x00017fff $root/greeting
i=0
while [ "$i" -lt {MANY_ENTRIES} ]; do
    printf '%s\\n' "$i" > $root/many/entry-$(printf '%04d' "$i")
    i=$((i + 1))
done
truncate -s {SPARSE_BYTES} $root/sparse
printf LEFT | dd of=$root/sparse bs=1 seek={SPARSE_LEFT} conv=notrunc 2>/dev/null
printf RIGHT | dd of=$root/sparse bs=1 seek={SPARSE_RIGHT} conv=notrunc 2>/dev/null
printf 'raw name\\n' > $root/$(printf 'raw-\\377')
printf 'subvolume changed\\n' > $root/subvol/value
printf 'snapshot original\\n' > $root/snapshot/value
mkfs.btrfs -f -s {sector} -n {NODE_SIZE} -m dup -d single -L machlin-btrfs \\
    --rootdir $root --subvol rw:subvol --subvol ro:snapshot /dev/vda
btrfs check --readonly --check-data-csum /dev/vda
btrfs inspect-internal dump-super /dev/vda | grep -E '^(sectorsize|nodesize|incompat_flags)'
manifest $root
echo BTRFS_PROGS_CREATED
trap - EXIT
poweroff -f
"""


def verify_init() -> str:
    return PRELUDE + MANIFEST + """
btrfs check --readonly --check-data-csum /dev/vda
btrfs inspect-internal dump-tree -t fs /dev/vda | grep -E 'EXTENT_DATA|type [0-9] [(]|extent (data|compression)|inline'
mkdir /tmp/restore
btrfs restore -s -m -x -S /dev/vda /tmp/restore > /dev/null
for subvolume in $(btrfs inspect-internal dump-tree -t root /dev/vda |
        awk '$1 == "item" && $5 == "ROOT_ITEM" && substr($4, 2) + 0 >= 256 { print substr($4, 2) }' |
        sort -u); do
    echo BTRFS_PROGS_SUBVOLUME:$subvolume
done
manifest /tmp/restore
echo BTRFS_PROGS_VERIFY_PASS
trap - EXIT
poweroff -f
"""


def prepare(root: Path, phase: str, profile: str, archive: Path) -> None:
    inputs = root / "input"
    inputs.mkdir(parents=True, exist_ok=True)
    for name, data in INPUTS.items():
        (inputs / name).write_bytes(data)
    init = create_init(SECTOR_SIZES[profile]) if phase == "create" else verify_init()
    (root / "init").write_text(init)
    (root / "init").chmod(0o755)
    paths = subprocess.run(["find", ".", "-path", "./transaction", "-prune", "-o", "-print"],
                           cwd=root, check=True, capture_output=True).stdout
    archive.parent.mkdir(parents=True, exist_ok=True)
    with archive.open("xb") as output:
        subprocess.run(["/usr/bin/cpio", "-o", "-H", "newc"], cwd=root, input=paths,
                       stdout=output, check=True)
    if phase == "create":
        report = {"profile": profile, "device_bytes": DEVICE_BYTES,
                  "sector_size": SECTOR_SIZES[profile], "node_size": NODE_SIZE}
        archive.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True,
                        help="Disposable staged Alpine initrd root")
    parser.add_argument("--phase", choices=("create", "verify"), required=True)
    parser.add_argument("--profile", choices=SECTOR_SIZES, default="sectors-64k")
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.root.resolve(), args.phase, args.profile, args.archive.resolve())


if __name__ == "__main__":
    main()
