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
# Up to the sparse device that holds a third superblock copy (past 256 GiB).
MAX_DEVICE = 512 << 30
NAME = re.compile(r"^[a-z0-9-]+\.(bin|tsv)$")
SCENARIO = re.compile(r"^[a-z0-9-]+$")
STAGE_PATH = re.compile(r"^(/[A-Za-z0-9._-]+)+$")
# Namespace facts: kind -> pattern of the argument field.
NAMESPACE_PATH = re.compile(r"^(/[A-Za-z0-9._-]{1,255})+$")
NAMESPACE_ARGUMENT = {
    "absent": re.compile(r"^-$"), "file": re.compile(r"^-$"), "symlink": re.compile(r"^-$"),
    "dir": re.compile(r"^[0-9]+$"), "same": NAMESPACE_PATH,
    "xattr": re.compile(r"^(user|trusted|security|btrfs)\.[A-Za-z0-9._-]{1,248}$"),
    "noxattr": re.compile(r"^(user|trusted|security|btrfs)\.[A-Za-z0-9._-]{1,248}$"),
    "stat": re.compile(r"^[0-9a-f]+:[0-9]+:[0-9]+:[0-9]+$"),
    "device": re.compile(r"^[0-9a-f]+:[0-9a-f]+$"), "flags": re.compile(r"^0x[0-9a-f]+:0x[0-9a-f]+$"),
    "feature": re.compile(r"^COMPRESS_(LZO|ZSTD)$"),
    "times": re.compile(r"^-?[0-9]+:-?[0-9]+$"),
    "reference": re.compile(r"^(inode|extended)$"),
    "subvolume": re.compile(r"^(ro|rw):(-|/|(/[A-Za-z0-9._-]{1,255})+)$"),
    "subvolumes": re.compile(r"^-$"),
    "deleted": re.compile(r"^[0-9]+$"),
    "compressed": re.compile(r"^(zlib|zstd):[0-9]+:[0-9]+$"),
    "extents": re.compile(r"^[0-9]+:[0-9]+:[0-9]+$"),
    "holes": re.compile(r"^[0-9]+$"),
    "bitmaps": re.compile(r"^[01]$"),
    "groups": re.compile(r"^[0-9]+:[0-9]+$"),
    "quota": re.compile(r"^(consistent|inconsistent|rescan):[0-9]+$")}
PAYLOAD_KINDS = {"file", "symlink", "dir", "xattr", "subvolumes"}
# Facts that may name the top-level directory itself.
ROOT_KINDS = {"dir", "stat", "xattr", "noxattr", "flags", "times", "feature", "subvolume",
              "subvolumes", "deleted", "groups", "quota"}


def safe_path(path):
    return NAMESPACE_PATH.match(path) and all(
        part not in (".", "..") for part in path.split("/")[1:])


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
    facts = {}
    namespace = directory / "namespace.tsv"
    if namespace.exists():
        for line in namespace.read_text().splitlines():
            stage, kind, path, argument, payload = fields(line, 5)
            if (int(stage) not in stages or kind not in NAMESPACE_ARGUMENT or
                    not (safe_path(path) or (kind in ROOT_KINDS and path == "/")) or
                    not NAMESPACE_ARGUMENT[kind].match(argument) or
                    (kind == "same" and not safe_path(argument))):
                raise ValueError(f"Invalid namespace record {line!r}")
            if (payload == "-") == (kind in PAYLOAD_KINDS):
                raise ValueError(f"Invalid namespace payload {line!r}")
            if payload != "-":
                (directory / check_name(payload)).stat()
            facts[int(stage)] = facts.get(int(stage), 0) + 1
    volatile = directory / "volatile.tsv"
    if volatile.exists():
        for line in volatile.read_text().splitlines():
            commit, path, offset, length = fields(line, 4)
            if (int(commit) not in stages or not safe_path(path) or int(offset) % 4096 != 0 or
                    int(length) == 0 or int(length) % 4096 != 0):
                raise ValueError(f"Invalid volatile range {line!r}")
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
    # The guest checks a stage's facts at every verification of that stage:
    # the mounted state, twice per recovery, and the final state.
    checks = facts.get(max(stages), 0)
    for case in cases:
        if case["mounted"] != "-":
            checks += facts.get(int(case["mounted"]), 0)
        if case["recovered"]:
            checks += 2 * facts.get(case["resolved"], 0)
    if not writes or not cases or not 0 < len(cases) <= 4096:
        raise ValueError(f"Empty or oversized scenario {directory.name}")
    return writes, stages, cases, touched, checks


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
    namespace_checks = 0
    scenarios = sorted(path for path in plan.iterdir() if path.is_dir())
    if not scenarios:
        raise ValueError("No exported scenarios")
    for directory in scenarios:
        if not SCENARIO.match(directory.name):
            raise ValueError(f"Unexpected scenario name {directory.name!r}")
        writes, stages, cases, touched, checks = load_scenario(directory, args.device_bytes)
        namespace_checks += checks
        # Superblock copies may be rewritten by Linux recovery as well.
        touched += [(65536, 4096), (64 * 1024 * 1024, 4096)]
        shutil.copytree(directory, target / directory.name)
        (target / directory.name / "restore.tsv").write_text("".join(
            f"{offset // SECTOR}\t{length // SECTOR}\n" for offset, length in merge(touched)))
        final = max(stages)
        (target / directory.name / "final.txt").write_text(f"{final}\n")
        summary[directory.name] = {"writes": len(writes), "cases": len(cases),
                                   "recoveries": sum(case["recovered"] for case in cases),
                                   "stages": len(stages), "namespace_checks": checks}
        total += len(cases)
    init = (Path(__file__).with_name("transaction_oracle.sh").read_text()
            .replace("@DEVICE_BYTES@", str(args.device_bytes))
            .replace("@NAMESPACE_CHECKS@", str(namespace_checks)))
    (root / "init").write_text(init)
    (root / "init").chmod(0o755)
    paths = subprocess.run(["find", ".", "-print"], cwd=root, check=True, capture_output=True).stdout
    archive.parent.mkdir(parents=True, exist_ok=True)
    with archive.open("wb") as output:
        subprocess.run(["/usr/bin/cpio", "-o", "-H", "newc"], cwd=root,
                       input=paths, stdout=output, check=True)
    report = {"cases": total, "namespace_checks": namespace_checks, "scenarios": summary}
    archive.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
