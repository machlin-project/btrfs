#!/usr/bin/env python3
"""Prepare Linux payloads for multi-device volumes; never boot or select a
host device.

create: format the guest's /dev/vda, /dev/vdb, ... (one image per device) as
one volume with the profile's data and metadata profiles, mount it, have the
kernel write the payload (files larger than a full stripe set on every
device, small and inline files, xattrs, owner and mode, a hard link, a
symlink, a sparse file, Zstd data by property, a subvolume and its read-only
snapshot), unmount, run `btrfs check --readonly --check-data-csum`, print
the devices' chunk profiles and the manifest.

verify: on images this implementation wrote, run the same check, mount,
print the manifest, write a file, unmount and check again.

The manifest names every regular file and symlink below the mount with its
mode, owner, size and SHA-256 (or symlink target), one line each, sorted;
subvolumes appear as their directories.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

DEVICE_BYTES = 256 << 20
NODE_SIZE = 16384
# Profile: devices, data profile, metadata profile.
PROFILES = {
    "multi-raid1": (2, "raid1", "raid1"),
    "multi-raid1c3": (3, "raid1c3", "raid1c3"),
    "multi-raid1c4": (4, "raid1c4", "raid1c4"),
    "multi-raid10": (4, "raid10", "raid10"),
    "multi-raid0": (3, "raid0", "raid1"),
    "multi-raid5": (3, "raid5", "raid1"),
    "multi-raid6": (4, "raid6", "raid1c3"),
    "multi-single": (2, "single", "raid1"),
}
MANY_ENTRIES = 300
SPARSE_BYTES = 8388608
SPARSE_LEFT = 17
SPARSE_RIGHT = 7340035
INPUTS = {
    "greeting": b"hello from Linux Btrfs\n",
    "big": bytes(range(256)) * 16384,
    # Larger than a full stripe set of 64 KiB stripes on every device.
    "random": hashlib.shake_256(b"Machlin multi-device fixture").digest(12 << 20),
}

PRELUDE = """#!/bin/busybox sh
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
mkdir -p /dev /proc /sys /tmp /mnt
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
trap 'echo BTRFS_MULTI_FAIL; dmesg | tail -60; sync; poweroff -f' EXIT
for module in $(cat /modules/order); do
    insmod /modules/$module.ko
done
exec < /dev/hvc0 > /dev/hvc0 2>&1
uname -r
mkfs.btrfs --version
"""

MANIFEST = r"""
manifest() {
    cd /mnt
    find . \( -type f -o -type l \) | LC_ALL=C sort | while IFS= read -r path; do
        if [ -L "$path" ]; then
            content=$(readlink "$path")
        else
            content=$(sha256sum "$path" | cut -d' ' -f1)
        fi
        printf 'BTRFS_MULTI_MANIFEST:%s\t%s\t%s\n' "$path" \
            "$(stat -c '%f:%u:%g:%s' "$path")" "$content"
    done
    cd /
}
"""


def devices(count: int) -> str:
    return " ".join(f"/dev/vd{chr(ord('a') + i)}" for i in range(count))


def create_init(count: int, data: str, metadata: str) -> str:
    return PRELUDE + MANIFEST + f"""
for device in {devices(count)}; do
    test "$(blockdev --getsize64 $device)" = {DEVICE_BYTES}
done
mkfs.btrfs -f -n {NODE_SIZE} -d {data} -m {metadata} -L machlin-btrfs {devices(count)}
btrfs device scan
mount -t btrfs -o noatime /dev/vda /mnt
cp /input/greeting /input/big /input/random /mnt/
chmod 0640 /mnt/greeting
chown 1001:1002 /mnt/greeting
ln /mnt/greeting /mnt/hardlink
ln -s greeting /mnt/symlink
setfattr -n user.text -v 'Linux xattr' /mnt/greeting
mkdir /mnt/many
i=0
while [ "$i" -lt {MANY_ENTRIES} ]; do
    printf '%s\\n' "$i" > /mnt/many/entry-$(printf '%04d' "$i")
    i=$((i + 1))
done
truncate -s {SPARSE_BYTES} /mnt/sparse
printf LEFT | dd of=/mnt/sparse bs=1 seek={SPARSE_LEFT} conv=notrunc 2>/dev/null
printf RIGHT | dd of=/mnt/sparse bs=1 seek={SPARSE_RIGHT} conv=notrunc 2>/dev/null
mkdir /mnt/zstd
btrfs property set /mnt/zstd compression zstd
cp /input/big /mnt/zstd/big
btrfs subvolume create /mnt/subvol > /dev/null
cp /input/random /mnt/subvol/random
printf 'subvolume value\\n' > /mnt/subvol/value
btrfs subvolume snapshot -r /mnt/subvol /mnt/snapshot > /dev/null
printf 'changed after the snapshot\\n' > /mnt/subvol/value
sync
btrfs filesystem usage -b /mnt | sed 's/^/BTRFS_MULTI_USAGE:/'
manifest
umount /mnt
btrfs check --readonly --check-data-csum /dev/vda
btrfs inspect-internal dump-tree -t chunk /dev/vda | grep -E 'type (DATA|METADATA|SYSTEM)' | sort | uniq -c | sed 's/^/BTRFS_MULTI_CHUNKS:/'
echo BTRFS_MULTI_CREATED
trap - EXIT
poweroff -f
"""


def verify_init(count: int) -> str:
    return PRELUDE + MANIFEST + f"""
btrfs device scan
btrfs check --readonly --check-data-csum /dev/vda
mount -t btrfs -o noatime /dev/vda /mnt
manifest
printf 'written after the check\\n' > /mnt/after-check
sync
umount /mnt
btrfs check --readonly --check-data-csum /dev/vda
echo BTRFS_MULTI_VERIFY_PASS
trap - EXIT
poweroff -f
"""


def prepare(root: Path, phase: str, profile: str, archive: Path) -> None:
    count, data, metadata = PROFILES[profile]
    inputs = root / "input"
    inputs.mkdir(parents=True, exist_ok=True)
    for name, content in INPUTS.items():
        (inputs / name).write_bytes(content)
    init = create_init(count, data, metadata) if phase == "create" else verify_init(count)
    (root / "init").write_text(init)
    (root / "init").chmod(0o755)
    paths = subprocess.run(["find", ".", "-path", "./transaction", "-prune", "-o", "-print"],
                           cwd=root, check=True, capture_output=True).stdout
    archive.parent.mkdir(parents=True, exist_ok=True)
    with archive.open("xb") as output:
        subprocess.run(["/usr/bin/cpio", "-o", "-H", "newc"], cwd=root, input=paths,
                       stdout=output, check=True)
    if phase == "create":
        report = {"profile": profile, "devices": count, "device_bytes": DEVICE_BYTES,
                  "node_size": NODE_SIZE, "data": data, "metadata": metadata}
        archive.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True,
                        help="Disposable staged Alpine initrd root")
    parser.add_argument("--phase", choices=("create", "verify"), required=True)
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.root.resolve(), args.phase, args.profile, args.archive.resolve())


if __name__ == "__main__":
    main()
