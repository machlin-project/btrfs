#!/usr/bin/env python3
"""Prepare independent btrfs-progs/Linux checks of exported transaction writes."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    plan = args.plan.resolve()
    archive = args.archive.resolve()
    writes = []
    for line in (plan / "writes.tsv").read_text().splitlines():
        offset, size, name = line.split("\t")
        offset, size = int(offset), int(size)
        if (Path(name).name != name or offset % 4096 or size % 4096 or
                offset < 0 or not 0 < size <= 65536 or offset + size > 268435456):
            raise ValueError("Invalid bounded write record")
        data = (plan / name).read_bytes()
        if len(data) != size:
            raise ValueError("Incomplete write payload")
        writes.append({"offset": offset, "size": size, "name": name,
                       "sha256": hashlib.sha256(data).hexdigest()})
    if not 0 < len(writes) <= 1024:
        raise ValueError("Invalid transaction write count")
    target = root / "transaction"
    target.mkdir(exist_ok=True)
    for write in writes:
        shutil.copyfile(plan / write["name"], target / write["name"])
    (target / "writes.tsv").write_text("".join(
        f"{write['offset'] // 4096} {write['size'] // 4096} {write['name']}\n"
        for write in writes))
    init = f'''#!/bin/busybox sh
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
mkdir -p /dev /proc /sys /tmp /mnt
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
trap 'echo BTRFS_TRANSACTION_FAIL; dmesg | tail -60; sync; poweroff -f' EXIT
for module in virtio_blk xor-neon xor raid6_pq crc32c_generic libcrc32c btrfs; do
    insmod /modules/$module.ko
done
uname -r
btrfs --version
while read offset blocks name; do
    dd if=/dev/vda of=/tmp/old-$name bs=4096 skip=$offset count=$blocks 2>/dev/null
done < /transaction/writes.tsv
cut=0
while [ "$cut" -le {len(writes)} ]; do
    while read offset blocks name; do
        dd if=/tmp/old-$name of=/dev/vda bs=4096 seek=$offset conv=notrunc 2>/dev/null
    done < /transaction/writes.tsv
    index=0
    while read offset blocks name; do
        if [ "$index" -lt "$cut" ]; then
            dd if=/transaction/$name of=/dev/vda bs=4096 seek=$offset conv=notrunc 2>/dev/null
        fi
        index=$((index + 1))
    done < /transaction/writes.tsv
    sync
    echo BTRFS_TRANSACTION_CUT:$cut
    btrfs check --readonly /dev/vda
    mount -t btrfs -o ro,nologreplay /dev/vda /mnt
    expected='hello from Linux Btrfs'
    if [ "$cut" -eq {len(writes)} ]; then
        expected='written by Machlin CoW transaction'
    fi
    test "$(cat /mnt/greeting)" = "$expected"
    test "$(cat /mnt/hardlink)" = "$expected"
    test "$(stat -c '%a:%u:%g:%h' /mnt/greeting)" = '640:1001:1002:2'
    test "$(cat /mnt/snapshot/value)" = 'snapshot original'
    test "$(cat /mnt/subvol/value)" = 'subvolume changed'
    test "$(getfattr --only-values -n user.text /mnt/greeting 2>/dev/null)" = 'Linux xattr'
    umount /mnt
    cut=$((cut + 1))
done
mount -t btrfs -o nospace_cache /dev/vda /mnt
printf 'Linux accepted the new root\\n' > /mnt/after-machlin
btrfs filesystem sync /mnt
umount /mnt
btrfs check --readonly /dev/vda
echo BTRFS_TRANSACTION_PASS:{len(writes) + 1}
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
    archive.with_suffix(".json").write_text(json.dumps({"writes": writes, "cuts": len(writes) + 1}, indent=2) + "\n")


if __name__ == "__main__":
    main()
