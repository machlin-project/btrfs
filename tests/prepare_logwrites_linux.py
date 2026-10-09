#!/usr/bin/env python3
"""Prepare a Linux payload that records every write of a Btrfs workload with
dm-log-writes; never boot or select a host device.

The guest formats /dev/mapper/logged (log-writes of /dev/vda into /dev/vdb),
runs four steps that each end with `sync` and a named mark, unmounts and
removes the target. tests/logwrites.c replays the log from a zeroed device and
recovers every crash state this implementation can reach."""

import argparse
import json
from pathlib import Path
import subprocess

DEVICE_BYTES = 256 * 1024 * 1024
LOG_BYTES = 1024 * 1024 * 1024
SMALL_FILES = 50
MANY_FILES = 300
# Input files: name -> (seed, size). tests/logwrites.c generates the same bytes.
INPUTS = {"a": (1, 300 * 1024), "b": (2, 64 * 1024), "c": (3, 5000), "small": (4, 100),
          "big": (5, 8 * 1024 * 1024)}


def generate(seed: int, size: int) -> bytes:
    """xorshift64* bytes, low byte of each step."""
    state = (seed * 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF or 1
    out = bytearray(size)
    for i in range(size):
        state ^= state >> 12
        state ^= (state << 25) & 0xFFFFFFFFFFFFFFFF
        state ^= state >> 27
        out[i] = ((state * 0x2545F4914F6CDD1D) & 0xFFFFFFFFFFFFFFFF) >> 56
    return bytes(out)


def prepare(root: Path, archive: Path) -> None:
    inputs = root / "input"
    inputs.mkdir(parents=True, exist_ok=True)
    for name, (seed, size) in INPUTS.items():
        (inputs / name).write_bytes(generate(seed, size))
    smalls = " ".join(f"s{i:02d}" for i in range(SMALL_FILES))
    removed = " ".join(f"s{i:02d}" for i in range(SMALL_FILES // 2))
    init = f'''#!/bin/busybox sh
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
mkdir -p /dev /proc /sys /tmp /mnt
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
trap 'echo BTRFS_LOGWRITES_FAIL; dmesg | tail -60; sync; poweroff -f' EXIT
for module in $(cat /modules/order) dm-mod dm-log-writes; do
    insmod /modules/$module.ko
done
exec < /dev/hvc0 > /dev/hvc0 2>&1
uname -r
test "$(blockdev --getsize64 /dev/vda)" = {DEVICE_BYTES}
test "$(blockdev --getsize64 /dev/vdb)" = {LOG_BYTES}
dmsetup create logged --table "0 $(blockdev --getsz /dev/vda) log-writes /dev/vda /dev/vdb"
mark() {{
    dmsetup message logged 0 mark "$1"
}}
# No discards: the log would record them and a replay would have to zero them.
mkfs.btrfs -f -K -s 4096 -n 4096 -m single -d single -L logwrites /dev/mapper/logged
mark mkfs
mount -t btrfs -o nodiscard,compress=no /dev/mapper/logged /mnt
cp /input/a /mnt/a
mkdir /mnt/d
cp /input/c /mnt/d/c
sync
mark step-1
cp /input/b /mnt/a
mv /mnt/d/c /mnt/c2
for name in {smalls}; do
    cp /input/small /mnt/d/$name
done
ln /mnt/a /mnt/d/a-link
sync
mark step-2
btrfs subvolume create /mnt/sv
cp /input/big /mnt/sv/big
btrfs subvolume snapshot /mnt/sv /mnt/snap
cd /mnt/d
rm {removed}
cd /
sync
mark step-3
cp /input/a /mnt/sv/big
truncate -s 0 /mnt/c2
sync
mark step-4
mkdir /mnt/many
i=0
while [ "$i" -lt {MANY_FILES} ]; do
    cp /input/c /mnt/many/f$(printf '%03d' "$i")
    i=$((i + 1))
done
mv /mnt/d /mnt/e
sync
mark step-5
btrfs subvolume delete /mnt/snap
i=0
while [ "$i" -lt {MANY_FILES} ]; do
    rm /mnt/many/f$(printf '%03d' "$i")
    i=$((i + 2))
done
sync
mark step-6
umount /mnt
mark unmounted
dmsetup remove logged
btrfs check --readonly /dev/vda
echo BTRFS_LOGWRITES_PASS
trap - EXIT
poweroff -f
'''
    (root / "init").write_text(init)
    (root / "init").chmod(0o755)
    paths = subprocess.run(["find", ".", "-print"], cwd=root, check=True,
                           capture_output=True).stdout
    archive.parent.mkdir(parents=True, exist_ok=True)
    with archive.open("wb") as output:
        subprocess.run(["/usr/bin/cpio", "-o", "-H", "newc"], cwd=root, input=paths,
                       stdout=output, check=True)
    manifest = {"device_bytes": DEVICE_BYTES, "log_bytes": LOG_BYTES,
                "inputs": {name: {"seed": seed, "size": size}
                           for name, (seed, size) in INPUTS.items()}}
    archive.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True,
                        help="Disposable staged Alpine initrd root")
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.root.resolve(), args.archive.resolve())


if __name__ == "__main__":
    main()
