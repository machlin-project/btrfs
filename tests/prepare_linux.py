#!/usr/bin/env python3
"""Prepare a bounded Linux oracle payload; never boot or select a host device."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

PROFILES = {"plain": (16384, "dup", ""), "small-nodes": (4096, "single", ""),
            "large-nodes": (65536, "dup", ""), "zlib": (16384, "dup", "zlib"),
            "zstd": (16384, "dup", "zstd"), "default-subvolume": (16384, "dup", "")}


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
    set_default = "btrfs subvolume set-default /mnt/subvol" if profile == "default-subvolume" else ":"
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
mkfs.btrfs -f -s 4096 -n {node_size} -m {metadata} -d single -L machlin-btrfs /dev/vda
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
                "compression": compression, "files": {
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
