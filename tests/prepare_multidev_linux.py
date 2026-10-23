#!/usr/bin/env python3
"""Prepare Linux payloads for multi-device volumes; never boot or select a
host device.

create: format the guest's /dev/vda, /dev/vdb, ... (one image per device, of
256 MiB unless the profile names their sizes) as
one volume with the profile's data and metadata profiles, mount it, have the
kernel write the payload (files larger than a full stripe set on every
device, small and inline files, xattrs, owner and mode, a hard link, a
symlink, a sparse file, Zstd data by property, a subvolume and its read-only
snapshot), unmount, run `btrfs check --readonly --check-data-csum`, print
the devices' chunk profiles and the manifest.

verify: on images this implementation wrote, run the same check, mount,
scrub every copy read-only (reporting whether Linux finds no errors), print
the manifest, write a file, balance every group with Linux's own
allocator (reporting whether it completes, since a full volume may refuse),
print the chunk profiles, unmount and check again.

convert: on a copy of a volume, have Linux convert data and metadata to the
given profiles with its own balance (forced), then balance every group,
reporting whether that completes, print the chunk profiles and check: the
control for conversions this implementation made.

devices: on a copy of a volume with one more, blank image after its devices,
have Linux add that device, print the device and chunk trees' device items
and each device's superblock, check, remove the device --remove names, print
them again with the manifest, and check: the reference for adding and
removing devices.

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
    # Devices of unequal size with unallocated space, for chunks a writer
    # creates across them.
    "multi-grow": (3, "raid1", "raid1c3"),
    # Room to add a device and remove another.
    "multi-spare": (3, "raid1", "raid1"),
}
# Device sizes of profiles whose devices differ.
DEVICE_SIZES = {
    "multi-grow": (512 << 20, 384 << 20, 320 << 20),
    "multi-spare": (512 << 20, 512 << 20, 512 << 20),
}


def device_sizes(profile: str, count: int) -> tuple:
    return DEVICE_SIZES.get(profile, (DEVICE_BYTES,) * count)
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


def create_init(count: int, data: str, metadata: str, sizes: tuple) -> str:
    checks = "".join(f"test \"$(blockdev --getsize64 /dev/vd{chr(ord('a') + i)})\" = {size}\n"
                     for i, size in enumerate(sizes))
    return PRELUDE + MANIFEST + checks + f"""
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
if btrfs scrub start -B -r /mnt > /tmp/scrub 2>&1 && grep -q 'no errors found' /tmp/scrub; then
    echo BTRFS_MULTI_SCRUB_CLEAN
else
    sed 's/^/BTRFS_MULTI_SCRUB:/' /tmp/scrub
fi
manifest
printf 'written after the check\\n' > /mnt/after-check
sync
if btrfs balance start --full-balance /mnt > /tmp/balance 2>&1; then
    echo BTRFS_MULTI_REBALANCED
else
    echo BTRFS_MULTI_REBALANCE_REFUSED
    sed 's/^/BTRFS_MULTI_REBALANCE_OUTPUT:/' /tmp/balance
    btrfs filesystem usage -b /mnt | sed 's/^/BTRFS_MULTI_USAGE:/'
    dmesg | tail -20 | sed 's/^/BTRFS_MULTI_DMESG:/'
fi
btrfs inspect-internal dump-tree -t chunk /dev/vda | grep -E 'type (DATA|METADATA|SYSTEM)' | sort | uniq -c | sed 's/^/BTRFS_MULTI_CHUNKS:/'
umount /mnt
btrfs check --readonly --check-data-csum /dev/vda
echo BTRFS_MULTI_VERIFY_PASS
trap - EXIT
poweroff -f
"""


def convert_init(data: str, metadata: str) -> str:
    return PRELUDE + MANIFEST + f"""
btrfs device scan
mount -t btrfs -o noatime /dev/vda /mnt
manifest
if ! btrfs balance start -f -dconvert={data} -mconvert={metadata} /mnt > /tmp/balance 2>&1; then
    sed 's/^/BTRFS_MULTI_CONVERT_OUTPUT:/' /tmp/balance
    dmesg | tail -20 | sed 's/^/BTRFS_MULTI_DMESG:/'
    false
fi
echo BTRFS_MULTI_CONVERTED
if btrfs balance start --full-balance /mnt > /tmp/balance 2>&1; then
    echo BTRFS_MULTI_REBALANCED
else
    echo BTRFS_MULTI_REBALANCE_REFUSED
    sed 's/^/BTRFS_MULTI_REBALANCE_OUTPUT:/' /tmp/balance
    btrfs filesystem usage -b /mnt | sed 's/^/BTRFS_MULTI_USAGE:/'
    dmesg | tail -20 | sed 's/^/BTRFS_MULTI_DMESG:/'
fi
btrfs inspect-internal dump-tree -t chunk /dev/vda | grep -E 'type (DATA|METADATA|SYSTEM)' | sort | uniq -c | sed 's/^/BTRFS_MULTI_CHUNKS:/'
umount /mnt
btrfs check --readonly --check-data-csum /dev/vda
echo BTRFS_MULTI_CONVERT_PASS
trap - EXIT
poweroff -f
"""


DEVICE_STATE = r"""
# $1 names the state, $2 a device of the volume.
device_state() {
    btrfs inspect-internal dump-tree -t dev $2 | grep -E 'DEV_STATS|PERSISTENT' | sed "s/^/BTRFS_MULTI_DEV_TREE_$1:/"
    btrfs inspect-internal dump-tree -t chunk $2 | grep -A 8 'DEV_ITEM' | sed "s/^/BTRFS_MULTI_CHUNK_TREE_$1:/"
    for device in /dev/vd?; do
        for offset in 65536 67108864; do
            printf 'BTRFS_MULTI_MAGIC_%s:%s:%s:' $1 $device $offset
            dd if=$device bs=1 skip=$((offset + 64)) count=8 2>/dev/null | od -An -c | tr -d ' \n'
            echo
        done
        btrfs inspect-internal dump-super $device 2>/dev/null | grep -E '^(magic|generation|total_bytes|bytes_used|num_devices|dev_item)|^dev_item' | sed "s|^|BTRFS_MULTI_SUPER_$1:$device:|" || true
    done
}
"""


def devices_init(count: int, remove: int) -> str:
    # The added device stays: the states after removal are read from it.
    added = f"/dev/vd{chr(ord('a') + count)}"
    return PRELUDE + MANIFEST + DEVICE_STATE + f"""
btrfs device scan
mount -t btrfs -o noatime /dev/vda /mnt
btrfs device add -f {added} /mnt
sync
umount /mnt
device_state ADDED {added}
btrfs check --readonly --check-data-csum {added}
mount -t btrfs -o noatime {added} /mnt
btrfs device remove {remove} /mnt
sync
manifest
umount /mnt
device_state REMOVED {added}
btrfs check --readonly --check-data-csum {added}
echo BTRFS_MULTI_DEVICES_PASS
trap - EXIT
poweroff -f
"""


def prepare(root: Path, phase: str, profile: str, archive: Path, data: str = "",
            metadata: str = "", remove: int = 0) -> None:
    count, profile_data, profile_metadata = PROFILES[profile]
    inputs = root / "input"
    inputs.mkdir(parents=True, exist_ok=True)
    for name, content in INPUTS.items():
        (inputs / name).write_bytes(content)
    sizes = device_sizes(profile, count)
    if phase == "create":
        init = create_init(count, profile_data, profile_metadata, sizes)
    elif phase == "convert":
        init = convert_init(data, metadata)
    elif phase == "devices":
        init = devices_init(count, remove)
    else:
        init = verify_init(count)
    (root / "init").write_text(init)
    (root / "init").chmod(0o755)
    paths = subprocess.run(["find", ".", "-path", "./transaction", "-prune", "-o", "-print"],
                           cwd=root, check=True, capture_output=True).stdout
    archive.parent.mkdir(parents=True, exist_ok=True)
    with archive.open("xb") as output:
        subprocess.run(["/usr/bin/cpio", "-o", "-H", "newc"], cwd=root, input=paths,
                       stdout=output, check=True)
    if phase == "create":
        report = {"profile": profile, "devices": count, "device_bytes": list(sizes),
                  "node_size": NODE_SIZE, "data": profile_data, "metadata": profile_metadata}
        archive.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True,
                        help="Disposable staged Alpine initrd root")
    parser.add_argument("--phase", choices=("create", "verify", "convert", "devices"),
                        required=True)
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--data", default="", help="convert: the data target profile")
    parser.add_argument("--metadata", default="", help="convert: the metadata target profile")
    parser.add_argument("--remove", type=int, default=0,
                        help="devices: the device id to remove after adding one")
    args = parser.parse_args()
    if args.phase == "convert" and not (args.data and args.metadata):
        parser.error("convert needs --data and --metadata")
    if args.phase == "devices" and args.remove <= 0:
        parser.error("devices needs --remove")
    prepare(args.root.resolve(), args.phase, args.profile, args.archive.resolve(), args.data,
            args.metadata, args.remove)


if __name__ == "__main__":
    main()
