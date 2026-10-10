#!/usr/bin/env python3
"""Prepare Linux payloads for an interrupted balance; never boot or select a
host device.

create: format the guest's /dev/vda, commit files in the top level, a
subvolume and its snapshot, print their manifest, then balance data and
metadata and power off at once when the committed root tree first holds a
relocation tree, so the image keeps relocation trees that no merge removed.

expect: on a disposable copy of such an image, require the relocation trees,
run `btrfs check --readonly`, mount read-only (Linux merges relocation trees
only on a read-write mount) and print the manifest.

recover: on another disposable copy, mount read-write with skip_balance, which
merges the relocation trees as btrfs_recover_relocation does, print the
manifest, unmount, require that no relocation tree remains and run
`btrfs check --readonly`.

record: write the same files through dm-log-writes (/dev/mapper/logged logs
/dev/vda into /dev/vdb), mark them `filled`, print the manifest, run a full
balance to its end, mark `balanced` and unmount; every state a crash could
leave during the balance can then be replayed from the log.

verify: on an image this implementation recovered, require that no relocation
tree and no data relocation orphan remains and that `btrfs check --readonly`
passes, mount read-write with skip_balance, print the manifest, unmount and
check again.

The manifest names every regular file below the mount with its size and
SHA-256, one line each, sorted by path.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

DEVICE_BYTES = 512 << 20
LOG_BYTES = 1 << 30
NODE_SIZE = 16384
# Files of DATA_BYTES each, cut from the random input at distinct offsets, in
# the subvolume (shared with its snapshot) and the top level; small ones are
# inline.
SUBVOLUME_FILES = 300
TOP_FILES = 100
DATA_BYTES = 65536
DATA_BLOCKS = DATA_BYTES // 4096
SMALL_FILES = 200
SMALL_BYTES = 100
RANDOM_BYTES = (SUBVOLUME_FILES + TOP_FILES) * DATA_BYTES
INPUTS = {"random": hashlib.shake_256(b"Machlin relocation fixture").digest(RANDOM_BYTES)}

PRELUDE = """#!/bin/busybox sh
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
mkdir -p /dev /proc /sys /tmp /mnt
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
trap 'echo BTRFS_RELOC_FAIL; dmesg | tail -60; sync; poweroff -f' EXIT
for module in $(cat /modules/order); do
    insmod /modules/$module.ko
done
exec < /dev/hvc0 > /dev/hvc0 2>&1
uname -r
"""

MANIFEST = r"""
manifest() {
    cd /mnt
    find . -type f | LC_ALL=C sort | while IFS= read -r path; do
        printf 'BTRFS_RELOC_MANIFEST:%s\t%s\t%s\n' "$path" "$(stat -c %s "$path")" \
            "$(sha256sum "$path" | cut -d' ' -f1)"
    done
    cd /
}

relocation_trees() {
    btrfs inspect-internal dump-tree -t root /dev/vda 2>/dev/null |
        grep -c 'key (TREE_RELOC ROOT_ITEM' || true
}

relocation_orphans() {
    btrfs inspect-internal dump-tree -t data_reloc /dev/vda |
        grep -c 'key (ORPHAN ORPHAN_ITEM' || true
}
"""


def fill() -> str:
    """Files in the top level, a subvolume and its snapshot, committed."""
    return f"""
btrfs subvolume create /mnt/sv > /dev/null
mkdir /mnt/sv/data /mnt/sv/small /mnt/top
i=0
while [ "$i" -lt {SUBVOLUME_FILES} ]; do
    dd if=/input/random of=/mnt/sv/data/f$i bs=4096 skip=$((i * {DATA_BLOCKS})) \\
        count={DATA_BLOCKS} 2>/dev/null
    i=$((i + 1))
done
i=0
while [ "$i" -lt {SMALL_FILES} ]; do
    dd if=/input/random of=/mnt/sv/small/s$i bs=1 skip=$i count={SMALL_BYTES} 2>/dev/null
    i=$((i + 1))
done
btrfs subvolume snapshot /mnt/sv /mnt/snap > /dev/null
i=0
while [ "$i" -lt {TOP_FILES} ]; do
    dd if=/input/random of=/mnt/top/f$i bs=4096 \\
        skip=$(( ({SUBVOLUME_FILES} + i) * {DATA_BLOCKS})) count={DATA_BLOCKS} 2>/dev/null
    i=$((i + 1))
done
sync
"""


def create_init() -> str:
    return PRELUDE + MANIFEST + f"""
mkfs.btrfs --version
mkfs.btrfs -f -s 4096 -n {NODE_SIZE} -m dup -d single -L machlin-btrfs /dev/vda
mount -t btrfs -o noatime /dev/vda /mnt
{fill()}
manifest
test "$(relocation_trees)" = 0
btrfs balance start --full-balance /mnt > /tmp/balance.txt 2>&1 &
balance=$!
# Each poll reads the committed root tree from the device, past the block
# device's cache; the balance commits on its own.
while :; do
    blockdev --flushbufs /dev/vda
    if [ "$(relocation_trees)" != 0 ]; then
        break
    fi
    if ! kill -0 "$balance" 2>/dev/null; then
        echo BTRFS_RELOC_MISSED
        exit 1
    fi
done
echo BTRFS_RELOC_CREATED
trap - EXIT
# Power off now: no merge, no unmount, only what the balance committed.
echo o > /proc/sysrq-trigger
"""


def expect_init() -> str:
    return PRELUDE + MANIFEST + """
btrfs inspect-internal dump-super /dev/vda | grep -E '^(generation|root)[[:space:]]'
trees=$(relocation_trees)
echo BTRFS_RELOC_TREES:$trees
test "$trees" != 0
btrfs inspect-internal dump-tree -t root /dev/vda | grep -E 'key \\((TREE_RELOC|BALANCE)'
btrfs check --readonly /dev/vda
mount -t btrfs -o ro /dev/vda /mnt
manifest
umount /mnt
test "$(relocation_trees)" = "$trees"
echo BTRFS_RELOC_EXPECT_PASS
trap - EXIT
poweroff -f
"""


def recover_init() -> str:
    return PRELUDE + MANIFEST + """
test "$(relocation_trees)" != 0
mount -t btrfs -o noatime,skip_balance /dev/vda /mnt
manifest
umount /mnt
test "$(relocation_trees)" = 0
btrfs check --readonly /dev/vda
echo BTRFS_RELOC_RECOVER_PASS
trap - EXIT
poweroff -f
"""


def record_init() -> str:
    # dm-log-writes needs its modules; no discards, which a replay would have
    # to zero.
    prelude = PRELUDE.replace("for module in $(cat /modules/order); do",
                              "for module in $(cat /modules/order) dm-mod dm-log-writes; do")
    return prelude + MANIFEST + f"""
test "$(blockdev --getsize64 /dev/vda)" = {DEVICE_BYTES}
test "$(blockdev --getsize64 /dev/vdb)" = {LOG_BYTES}
dmsetup create logged --table "0 $(blockdev --getsz /dev/vda) log-writes /dev/vda /dev/vdb"
mark() {{
    dmsetup message logged 0 mark "$1"
}}
mkfs.btrfs -f -K -s 4096 -n {NODE_SIZE} -m dup -d single -L machlin-btrfs /dev/mapper/logged
mark mkfs
mount -t btrfs -o noatime,nodiscard /dev/mapper/logged /mnt
{fill()}
mark filled
manifest
btrfs balance start --full-balance /mnt
sync
mark balanced
umount /mnt
mark unmounted
dmsetup remove logged
btrfs check --readonly /dev/vda
echo BTRFS_RELOC_RECORD_PASS
trap - EXIT
poweroff -f
"""


def verify_init() -> str:
    return PRELUDE + MANIFEST + """
test "$(relocation_trees)" = 0
test "$(relocation_orphans)" = 0
btrfs inspect-internal dump-tree -t root /dev/vda | grep -E 'key \\((TREE_RELOC|BALANCE)' || true
btrfs check --readonly /dev/vda
mount -t btrfs -o noatime,skip_balance /dev/vda /mnt
manifest
printf 'after relocation recovery\\n' > /mnt/after-recovery
sync
umount /mnt
btrfs check --readonly /dev/vda
echo BTRFS_RELOC_VERIFY_PASS
trap - EXIT
poweroff -f
"""


def prepare(root: Path, phase: str, archive: Path) -> None:
    inputs = root / "input"
    inputs.mkdir(parents=True, exist_ok=True)
    for name, data in INPUTS.items():
        (inputs / name).write_bytes(data)
    init = {"create": create_init, "expect": expect_init, "recover": recover_init,
            "record": record_init, "verify": verify_init}[phase]()
    (root / "init").write_text(init)
    (root / "init").chmod(0o755)
    paths = subprocess.run(["find", ".", "-print"], cwd=root, check=True,
                           capture_output=True).stdout
    archive.parent.mkdir(parents=True, exist_ok=True)
    with archive.open("xb") as output:
        subprocess.run(["/usr/bin/cpio", "-o", "-H", "newc"], cwd=root, input=paths,
                       stdout=output, check=True)
    if phase in ("create", "record"):
        report = {"device_bytes": DEVICE_BYTES, "log_bytes": LOG_BYTES, "node_size": NODE_SIZE}
        archive.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True,
                        help="Disposable staged Alpine initrd root")
    parser.add_argument("--phase", choices=("create", "expect", "recover", "record", "verify"),
                        required=True)
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.root.resolve(), args.phase, args.archive.resolve())


if __name__ == "__main__":
    main()
