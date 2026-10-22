#!/usr/bin/env python3
"""Prepare a Linux payload that checks an image this implementation
balanced; never boot or select a host device.

verify: on /dev/vda, run `btrfs check --readonly --check-data-csum`, mount it
read-write and print the manifest, then balance every chunk with Linux's own
relocation (`btrfs balance start --full-balance`), unmount, check again, mount
and print the manifest a second time, write a file, unmount and check once
more. Run it on a clone of the image before and after the balance here: the
manifests of both must be equal, and Linux's balance must leave them so.

squota: format /dev/vda, write data in the top level and a subvolume, enable
simple quotas, write more, and print the qgroups before and after a full
balance: how Linux counts moved extents from before and after the enable.

The manifest names every regular file and symlink below the mount with its
mode, owner, size and SHA-256 (or symlink target), one line each, sorted;
subvolumes appear as their directories. Under quotas, `btrfs qgroup show
--raw` is printed before and after Linux's balance.
"""

import argparse
from pathlib import Path
import subprocess

PRELUDE = """#!/bin/busybox sh
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
mkdir -p /dev /proc /sys /tmp /mnt
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
trap 'echo BTRFS_BALANCE_FAIL; dmesg | tail -60; sync; poweroff -f' EXIT
for module in $(cat /modules/order); do
    insmod /modules/$module.ko
done
exec < /dev/hvc0 > /dev/hvc0 2>&1
uname -r
btrfs --version
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
        printf 'BTRFS_BALANCE_MANIFEST_%s:%s\t%s\t%s\n' "$1" "$path" \
            "$(stat -c '%f:%u:%g:%s' "$path")" "$content"
    done
    cd /
}

qgroups() {
    btrfs qgroup show --raw /mnt 2>/dev/null | sed "s/^/BTRFS_BALANCE_QGROUP_$1:/" || true
}
"""


def verify_init() -> str:
    return PRELUDE + MANIFEST + """
btrfs check --readonly --check-data-csum /dev/vda
mount -t btrfs -o noatime /dev/vda /mnt
manifest BEFORE
qgroups BEFORE
btrfs balance start --full-balance /mnt
umount /mnt
btrfs check --readonly --check-data-csum /dev/vda
mount -t btrfs -o noatime /dev/vda /mnt
manifest AFTER
qgroups AFTER
printf 'written after the balance\\n' > /mnt/after-balance
sync
umount /mnt
btrfs check --readonly --check-data-csum /dev/vda
echo BTRFS_BALANCE_VERIFY_PASS
trap - EXIT
poweroff -f
"""


# Bytes written before and after simple quotas are enabled (squota phase).
SQUOTA_BEFORE_BYTES = 1 << 20
SQUOTA_AFTER_BYTES = 1 << 19
SQUOTA_BLOCK = 65536


def squota_init() -> str:
    """How Linux's balance counts extents under simple quotas: data and tree
    blocks from before the quotas were enabled and from after."""
    return PRELUDE + MANIFEST + f"""
mkfs.btrfs -f -s 4096 -n 16384 -m dup -d single /dev/vda
mount -t btrfs -o noatime /dev/vda /mnt
dd if=/dev/zero of=/mnt/before bs={SQUOTA_BLOCK} count={SQUOTA_BEFORE_BYTES // SQUOTA_BLOCK} 2>/dev/null
btrfs subvolume create /mnt/sv > /dev/null
dd if=/dev/zero of=/mnt/sv/before bs={SQUOTA_BLOCK} count={SQUOTA_BEFORE_BYTES // SQUOTA_BLOCK} 2>/dev/null
sync
btrfs quota enable -s /mnt
dd if=/dev/zero of=/mnt/after bs={SQUOTA_BLOCK} count={SQUOTA_AFTER_BYTES // SQUOTA_BLOCK} 2>/dev/null
sync
qgroups BEFORE
btrfs balance start --full-balance /mnt
sync
qgroups AFTER
umount /mnt
btrfs check --readonly /dev/vda
echo BTRFS_BALANCE_SQUOTA_PASS
trap - EXIT
poweroff -f
"""


def prepare(root: Path, phase: str, archive: Path) -> None:
    (root / "init").write_text(verify_init() if phase == "verify" else squota_init())
    (root / "init").chmod(0o755)
    paths = subprocess.run(["find", ".", "-path", "./transaction", "-prune", "-o", "-path",
                            "./input", "-prune", "-o", "-print"],
                           cwd=root, check=True, capture_output=True).stdout
    archive.parent.mkdir(parents=True, exist_ok=True)
    with archive.open("xb") as output:
        subprocess.run(["/usr/bin/cpio", "-o", "-H", "newc"], cwd=root, input=paths,
                       stdout=output, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True,
                        help="Disposable staged Alpine initrd root")
    parser.add_argument("--phase", choices=("verify", "squota"), default="verify")
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.root.resolve(), args.phase, args.archive.resolve())


if __name__ == "__main__":
    main()
