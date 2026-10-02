#!/usr/bin/env python3
"""Prepare the Linux check of an image written by a native macOS mount.

The manifest is the output of `btrfs-mounted-write-test verify`. The Linux guest
receives a disposable copy of the written image as /dev/vda.
"""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

FIELDS = {"file": 7, "dir": 3, "inodes": 3, "symlink": 3, "xattr": 4, "mtime": 3,
          "absent": 2}
PATH = re.compile(r"^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$")
VALUE = re.compile(r"^[A-Za-z0-9 ._-]*$")
MAX_DEVICE = 1 << 33


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--device-bytes", type=int, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    archive = args.archive.resolve()
    if not 0 < args.device_bytes <= MAX_DEVICE or args.device_bytes % 512:
        raise ValueError("Invalid device size")
    lines = args.manifest.read_text().splitlines()
    for line in lines:
        fields = line.split("\t")
        if (fields[0] not in FIELDS or len(fields) != FIELDS[fields[0]] or
                not PATH.match(fields[1]) or ".." in fields[1].split("/") or
                not all(VALUE.match(field) for field in fields[2:])):
            raise ValueError(f"Invalid manifest line {line!r}")
    if not lines:
        raise ValueError("Empty manifest")
    target = root / "native"
    if target.exists():
        shutil.rmtree(target)
    target.mkdir()
    (target / "manifest.tsv").write_text("\n".join(lines) + "\n")
    init = (Path(__file__).with_name("native_oracle.sh").read_text()
            .replace("@DEVICE_BYTES@", str(args.device_bytes))
            .replace("@CHECKS@", str(len(lines))))
    (root / "init").write_text(init)
    (root / "init").chmod(0o755)
    paths = subprocess.run(["find", ".", "-print"], cwd=root, check=True, capture_output=True).stdout
    archive.parent.mkdir(parents=True, exist_ok=True)
    with archive.open("wb") as output:
        subprocess.run(["/usr/bin/cpio", "-o", "-H", "newc"], cwd=root,
                       input=paths, stdout=output, check=True)
    report = {"checks": len(lines)}
    archive.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
