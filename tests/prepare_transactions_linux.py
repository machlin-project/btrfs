#!/usr/bin/env python3
"""Prepare independent btrfs-progs/Linux checks of exported transaction crash states.

The plan directory holds one subdirectory per scenario, produced by
`btrfs-transaction-test --export`. Each crash case lists the visible sector runs
of the issued writes and the superblock writes chosen by explicit recovery. The
Linux guest receives a disposable working copy as /dev/vda and a pristine copy of
the same fixture as /dev/vdb; it never writes /dev/vdb.
"""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

SECTOR = 512
MAX_WRITE = 1024 * 1024
MAX_DEVICE = 1 << 30
NAME = re.compile(r"^[a-z0-9-]+\.(bin|tsv)$")
SCENARIO = re.compile(r"^[a-z0-9-]+$")
STAGE_PATH = re.compile(r"^(/[A-Za-z0-9._-]+)+$")


def fields(line, count):
    parts = line.split("\t")
    if len(parts) != count:
        raise ValueError(f"Malformed record: {line!r}")
    return parts


def check_name(name):
    if not NAME.match(name):
        raise ValueError(f"Unexpected file name {name!r}")
    return name


def load_fragments(directory, name, device_bytes):
    """Validate one fragment list and return the touched byte ranges."""
    ranges = []
    for line in (directory / check_name(name)).read_text().splitlines():
        seek, count, payload, skip = fields(line, 4)
        seek, count, skip = int(seek), int(count), int(skip)
        size = (directory / check_name(payload)).stat().st_size
        if (count <= 0 or skip < 0 or (skip + count) * SECTOR > size or
                size > MAX_WRITE or (seek + count) * SECTOR > device_bytes):
            raise ValueError(f"Invalid fragment in {name}: {line!r}")
        ranges.append((seek * SECTOR, count * SECTOR))
    return ranges


def load_scenario(directory, device_bytes):
    writes = []
    for line in (directory / "writes.tsv").read_text().splitlines():
        index, offset, length, name, commit, epoch = fields(line, 6)
        offset, length = int(offset), int(length)
        if (offset % SECTOR or length % SECTOR or not 0 < length <= MAX_WRITE or
                offset + length > device_bytes or
                (directory / check_name(name)).stat().st_size != length):
            raise ValueError(f"Invalid write record {line!r}")
        writes.append((offset, length, name, int(commit), int(epoch)))
    stages = {}
    for line in (directory / "stages.tsv").read_text().splitlines():
        stage, generation, path, name = fields(line, 4)
        if not STAGE_PATH.match(path) or ".." in path:
            raise ValueError(f"Unexpected tracked path {path!r}")
        check_name(name)
        stages.setdefault(int(stage), {"generation": int(generation), "files": []})
        stages[int(stage)]["files"].append((path, name))
    cases = []
    touched = [(offset, length) for offset, length, _, _, _ in writes]
    for line in (directory / "cases.tsv").read_text().splitlines():
        case, commit, kind, mounted, resolved, recovered = fields(line, 6)
        if mounted != "-" and int(mounted) not in stages:
            raise ValueError(f"Unknown mounted stage in {line!r}")
        if int(resolved) not in stages or recovered not in ("0", "1"):
            raise ValueError(f"Invalid case {line!r}")
        touched += load_fragments(directory, f"case-{case}.tsv", device_bytes)
        touched += load_fragments(directory, f"recover-{case}.tsv", device_bytes)
        cases.append({"case": case, "commit": int(commit), "kind": kind, "mounted": mounted,
                      "resolved": int(resolved), "recovered": recovered == "1"})
    if not writes or not cases or not 0 < len(cases) <= 4096:
        raise ValueError(f"Empty or oversized scenario {directory.name}")
    return writes, stages, cases, touched


def merge(ranges):
    merged = []
    for offset, length in sorted(ranges):
        if merged and offset <= merged[-1][0] + merged[-1][1]:
            end = max(merged[-1][0] + merged[-1][1], offset + length)
            merged[-1] = (merged[-1][0], end - merged[-1][0])
        else:
            merged.append((offset, length))
    return merged


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--device-bytes", type=int, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    plan = args.plan.resolve()
    archive = args.archive.resolve()
    if not 0 < args.device_bytes <= MAX_DEVICE or args.device_bytes % SECTOR:
        raise ValueError("Invalid device size")
    target = root / "transaction"
    if target.exists():
        shutil.rmtree(target)
    target.mkdir()
    summary = {}
    total = 0
    scenarios = sorted(path for path in plan.iterdir() if path.is_dir())
    if not scenarios:
        raise ValueError("No exported scenarios")
    for directory in scenarios:
        if not SCENARIO.match(directory.name):
            raise ValueError(f"Unexpected scenario name {directory.name!r}")
        writes, stages, cases, touched = load_scenario(directory, args.device_bytes)
        # Superblock copies may be rewritten by Linux recovery as well.
        touched += [(65536, 4096), (64 * 1024 * 1024, 4096)]
        shutil.copytree(directory, target / directory.name)
        (target / directory.name / "restore.tsv").write_text("".join(
            f"{offset // SECTOR}\t{length // SECTOR}\n" for offset, length in merge(touched)))
        final = max(stages)
        (target / directory.name / "final.txt").write_text(f"{final}\n")
        summary[directory.name] = {"writes": len(writes), "cases": len(cases),
                                   "recoveries": sum(case["recovered"] for case in cases),
                                   "stages": len(stages)}
        total += len(cases)
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
test "$(blockdev --getsize64 /dev/vda)" = {args.device_bytes}
test "$(blockdev --getsize64 /dev/vdb)" = {args.device_bytes}
cmp /dev/vda /dev/vdb

refresh() {{
    sync
    blockdev --flushbufs /dev/vda
}}

restore() {{
    while IFS="$(printf '\\t')" read -r seek count; do
        dd if=/dev/vdb of=/dev/vda bs=512 skip=$seek seek=$seek count=$count conv=notrunc 2>/dev/null
    done < "$1/restore.tsv"
    refresh
}}

apply() {{
    while IFS="$(printf '\\t')" read -r seek count payload skip; do
        dd if="$1/$payload" of=/dev/vda bs=512 seek=$seek skip=$skip count=$count conv=notrunc 2>/dev/null
    done < "$1/$2"
    refresh
}}

generation() {{
    awk -v stage="$2" -F '\\t' '$1 == stage {{ print $2; exit }}' "$1/stages.tsv"
}}

primary_generation() {{
    btrfs inspect-internal dump-super /dev/vda | awk '$1 == "generation" {{ print $2; exit }}'
}}

verify() {{
    btrfs check --readonly /dev/vda < /dev/null
    test "$(primary_generation)" = "$(generation "$1" "$2")"
    mount -t btrfs -o ro,nologreplay /dev/vda /mnt
    while IFS="$(printf '\\t')" read -r stage generation path expected; do
        if [ "$stage" = "$2" ]; then
            cmp "/mnt$path" "$1/$expected"
        fi
    done < "$1/stages.tsv"
    test "$(stat -c '%a:%u:%g:%h' /mnt/greeting)" = '640:1001:1002:2'
    test "$(stat -c '%i' /mnt/greeting)" = "$(stat -c '%i' /mnt/hardlink)"
    test "$(cat /mnt/snapshot/value)" = 'snapshot original'
    test "$(cat /mnt/subvol/value)" = 'subvolume changed'
    test "$(getfattr --only-values -n user.text /mnt/greeting 2>/dev/null)" = 'Linux xattr'
    umount /mnt
}}

total=0
for scenario in /transaction/*; do
    name=${{scenario##*/}}
    while IFS="$(printf '\\t')" read -r case commit kind mounted resolved recovered; do
        restore "$scenario"
        apply "$scenario" "case-$case.tsv"
        echo "BTRFS_TRANSACTION_CASE:$name:$case:$kind:$mounted:$resolved:$recovered"
        if [ "$mounted" = - ]; then
            if mount -t btrfs -o ro,nologreplay /dev/vda /mnt 2>/dev/null; then
                umount /mnt
                echo "Linux mounted a primary superblock this implementation rejects"
                exit 1
            fi
        else
            verify "$scenario" "$mounted"
        fi
        if [ "$recovered" = 1 ]; then
            status=0
            btrfs rescue super-recover -y /dev/vda < /dev/null || status=$?
            test "$status" -eq 2
            refresh
            verify "$scenario" "$resolved"
            restore "$scenario"
            apply "$scenario" "case-$case.tsv"
            apply "$scenario" "recover-$case.tsv"
            verify "$scenario" "$resolved"
            status=0
            btrfs rescue super-recover -y /dev/vda < /dev/null || status=$?
            test "$status" -eq 0
        fi
        total=$((total + 1))
    done < "$scenario/cases.tsv"
    restore "$scenario"
    cmp /dev/vda /dev/vdb
    echo "BTRFS_TRANSACTION_SCENARIO:$name"
done
for scenario in /transaction/*; do
    restore "$scenario"
done
cmp /dev/vda /dev/vdb
# Linux continues read-write from the newest root of the last scenario.
scenario=$(ls -d /transaction/* | tail -1)
awk -F '\\t' '{{ printf "%d\\t%d\\t%s\\t0\\n", $2 / 512, $3 / 512, $4 }}' "$scenario/writes.tsv" > /tmp/all.tsv
cp /tmp/all.tsv "$scenario/all.tsv"
apply "$scenario" all.tsv
verify "$scenario" "$(cat "$scenario/final.txt")"
# A free-space tree must stay enabled; the other profiles keep no space cache.
options=nospace_cache
if btrfs inspect-internal dump-super /dev/vda | grep -q FREE_SPACE_TREE_VALID; then
    options=defaults
fi
mount -t btrfs -o "$options" /dev/vda /mnt
printf 'Linux accepted the new root\\n' > /mnt/after-machlin
btrfs filesystem sync /mnt
umount /mnt
btrfs check --readonly /dev/vda < /dev/null
echo BTRFS_TRANSACTION_PASS:$total
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
    archive.with_suffix(".json").write_text(json.dumps(
        {"cases": total, "scenarios": summary}, indent=2) + "\n")
    print(json.dumps({"cases": total, "scenarios": summary}))


if __name__ == "__main__":
    main()
