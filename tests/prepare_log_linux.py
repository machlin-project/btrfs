#!/usr/bin/env python3
"""Prepare Linux payloads for tree-log replay; never boot or select a host device.

create: format the guest's /dev/vda, commit a base, then fsync a sequence of
changes so that Linux writes them to its tree log, and power off at once
without a commit, leaving the log for replay.

expect: on a disposable copy of such an image, require a pending log, let
Linux replay it by mounting, and print the replayed volume's manifest.

verify: on an image this implementation replayed, require that no log is
pending, run `btrfs check --readonly` before mounting, print the same manifest,
then continue read-write and check again.

The manifest names every path below the mount with its inode number, mode,
owner, link count, size, contents (SHA-256 or symlink target), regular files'
modification time and xattrs, one line each, sorted by path.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

# Linux keeps the transaction open this long, so only fsync writes durably.
COMMIT_SECONDS = 3600
MODULES = ("virtio_blk xor-neon xor raid6_pq crc32c_generic libcrc32c xxhash_generic "
           "blake2b_generic btrfs")
PROFILES = {
    # 16 KiB nodes, DUP metadata, mkfs defaults (no-holes, free-space tree).
    "logs": {"node_size": 16384, "metadata": "dup", "device_bytes": 256 << 20},
    # 4 KiB nodes: the log of one directory spans a multi-level log tree.
    "logs-many": {"node_size": 4096, "metadata": "single", "device_bytes": 256 << 20},
    # The logs workload with quotas enabled before its base: replay accounts
    # the extents it adds and drops, and btrfs check verifies the numbers.
    "logs-quota": {"node_size": 16384, "metadata": "dup", "device_bytes": 256 << 20,
                   "quota": "full"},
    # The logs workload with simple quotas enabled before its base: replay
    # names the owner of each extent it allocates and counts it for that owner.
    "logs-squota": {"node_size": 16384, "metadata": "dup", "device_bytes": 256 << 20,
                    "quota": "simple"},
}
MANY_FILES = 2000
INPUTS = {
    "big": bytes(range(256)) * 16384,
    "random": hashlib.shake_256(b"Machlin tree-log replay fixture").digest(1 << 20),
}

PRELUDE = f"""#!/bin/busybox sh
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
mkdir -p /dev /proc /sys /tmp /mnt
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
trap 'echo BTRFS_LOG_FAIL; dmesg | tail -60; sync; poweroff -f' EXIT
for module in {MODULES}; do
    insmod /modules/$module.ko
done
uname -r
"""

# Every path below /mnt, sorted; fields separated by tabs. Directory sizes are
# Linux's (twice the name lengths); times other than regular files' mtime
# change during replay itself and are left out.
MANIFEST = r"""
manifest() {
    cd /mnt
    find . | LC_ALL=C sort | while IFS= read -r path; do
        set -- $(stat -c '%i %a %u %g %h %s %Y' "$path")
        kind=$(stat -c %F "$path")
        content=-
        mtime=-
        case "$kind" in
        "regular file"|"regular empty file")
            kind=file
            content=$(sha256sum "$path" | cut -d' ' -f1)
            mtime=$7
            ;;
        directory)
            kind=dir
            ;;
        "symbolic link")
            kind=symlink
            content=$(readlink "$path")
            ;;
        esac
        xattrs=$(getfattr -h -d -m - -e hex "$path" 2>/dev/null | grep -v '^#' | grep -v '^$' |
            LC_ALL=C sort | tr '\n' ' ')
        printf 'BTRFS_LOG_MANIFEST:%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$path" \
            "$kind" "$1" "$2" "$3" "$4" "$5" "$6" "$content" "$mtime" "${xattrs:--}"
    done
    cd /
}
"""

BASE = r"""
mkdir -p /mnt/a /mnt/b /mnt/dirdel
i=1
while [ "$i" -le 6 ]; do
    head -c $((i * 7000)) /input/random > /mnt/a/f$i
    i=$((i + 1))
done
head -c 1048576 /input/big > /mnt/big
head -c 300000 /input/random > /mnt/keep-data
printf 'x' > /mnt/xa
setfattr -n user.old -v old /mnt/xa
setfattr -n user.stay -v stay /mnt/xa
ln /mnt/a/f6 /mnt/b/f6-link
i=1
while [ "$i" -le 20 ]; do
    : > /mnt/dirdel/e$i
    i=$((i + 1))
done
btrfs subvolume create /mnt/sub > /dev/null
head -c 50000 /input/random > /mnt/sub/s1
sync
"""

LOGGED = r"""
head -c 100000 /input/big > /mnt/a/new1
fsync /mnt/a/new1
head -c 5000 /input/random >> /mnt/keep-data
fsync /mnt/keep-data
printf 'MIDDLE' | dd of=/mnt/big bs=1 seek=500000 conv=notrunc 2>/dev/null
fsync /mnt/big
truncate -s 1000 /mnt/a/f2
fsync /mnt/a/f2
mv /mnt/a/f3 /mnt/b/f3-moved
fsync /mnt/b/f3-moved
rm /mnt/a/f4
fsync /mnt/a
ln /mnt/a/f5 /mnt/b/f5-link
fsync /mnt/a/f5
setfattr -n user.new -v new /mnt/xa
setfattr -x user.old /mnt/xa
fsync /mnt/xa
rm /mnt/b/f6-link
fsync /mnt/a/f6
i=1
while [ "$i" -le 10 ]; do
    rm /mnt/dirdel/e$i
    i=$((i + 1))
done
fsync /mnt/dirdel
fallocate -l 262144 /mnt/a/pre
fsync /mnt/a/pre
chmod 600 /mnt/a/f1
chown 1001:1002 /mnt/a/f1
fsync /mnt/a/f1
head -c 7000 /input/random >> /mnt/sub/s1
fsync /mnt/sub/s1
head -c 3000 /input/random > /mnt/a/gone
fsync /mnt/a/gone
rm /mnt/a/gone
ln -s a/f1 /mnt/sym
fsync /mnt
mkdir /mnt/c
head -c 9000 /input/random > /mnt/c/x
fsync /mnt/c/x
"""

MANY = f"""
mkdir /mnt/many
i=0
while [ "$i" -lt {MANY_FILES} ]; do
    printf '%s\\n' "$i" > /mnt/many/entry-$(printf '%05d' "$i")
    i=$((i + 1))
done
head -c 1048576 /input/random > /mnt/many-extents
i=0
while [ "$i" -lt 64 ]; do
    printf 'P' | dd of=/mnt/many-extents bs=1 seek=$((i * 16384 + 7)) conv=notrunc 2>/dev/null
    i=$((i + 1))
done
fsync /mnt/many-extents
fsync /mnt/many
"""


def create_init(profile: str) -> str:
    settings = PROFILES[profile]
    logged = LOGGED + (MANY if profile == "logs-many" else "")
    return PRELUDE + f"""
mkfs.btrfs --version
mkfs.btrfs -f -s 4096 -n {settings["node_size"]} -m {settings["metadata"]} -d single \\
    -L machlin-btrfs /dev/vda
mount -t btrfs -o noatime,commit={COMMIT_SECONDS} /dev/vda /mnt
{"btrfs quota enable /mnt" if settings.get("quota") == "full" else
 "btrfs quota enable -s /mnt" if settings.get("quota") == "simple" else ":"}
{BASE}
{"btrfs quota rescan -w /mnt && btrfs qgroup show --raw /mnt" if settings.get("quota") == "full" else
 "btrfs qgroup show --raw /mnt" if settings.get("quota") == "simple" else ":"}
{logged}
echo BTRFS_LOG_CREATED:{profile}
trap - EXIT
# Power off now: no commit, no unmount, only what fsync wrote.
echo o > /proc/sysrq-trigger
"""


def expect_init() -> str:
    return PRELUDE + MANIFEST + """
btrfs inspect-internal dump-super /dev/vda | grep -E '^(generation|log_root|log_root_level)[[:space:]]'
log_root=$(btrfs inspect-internal dump-super /dev/vda | awk '$1 == "log_root" {print $2}')
test "$log_root" != 0
echo BTRFS_LOG_PENDING:$log_root
mount -t btrfs -o noatime /dev/vda /mnt
manifest
umount /mnt
btrfs check --readonly /dev/vda
echo BTRFS_LOG_EXPECT_PASS
trap - EXIT
poweroff -f
"""


def verify_init() -> str:
    return PRELUDE + MANIFEST + """
log_root=$(btrfs inspect-internal dump-super /dev/vda | awk '$1 == "log_root" {print $2}')
test "$log_root" = 0
btrfs check --readonly /dev/vda
mount -t btrfs -o noatime /dev/vda /mnt
manifest
printf 'after replay\\n' > /mnt/after-replay
sync
umount /mnt
btrfs check --readonly /dev/vda
echo BTRFS_LOG_VERIFY_PASS
trap - EXIT
poweroff -f
"""


def prepare(root: Path, phase: str, profile: str, archive: Path) -> None:
    inputs = root / "input"
    inputs.mkdir(parents=True, exist_ok=True)
    for name, data in INPUTS.items():
        (inputs / name).write_bytes(data)
    init = {"create": lambda: create_init(profile), "expect": expect_init,
            "verify": verify_init}[phase]()
    (root / "init").write_text(init)
    (root / "init").chmod(0o755)
    paths = subprocess.run(["find", ".", "-print"], cwd=root, check=True,
                           capture_output=True).stdout
    archive.parent.mkdir(parents=True, exist_ok=True)
    with archive.open("xb") as output:
        subprocess.run(["/usr/bin/cpio", "-o", "-H", "newc"], cwd=root, input=paths,
                       stdout=output, check=True)
    if phase == "create":
        report = {"profile": profile, **PROFILES[profile]}
        archive.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True,
                        help="Disposable staged Alpine initrd root")
    parser.add_argument("--phase", choices=("create", "expect", "verify"), required=True)
    parser.add_argument("--profile", choices=PROFILES, default="logs")
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.root.resolve(), args.phase, args.profile, args.archive.resolve())


if __name__ == "__main__":
    main()
