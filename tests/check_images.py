#!/usr/bin/env python3
"""Compare the portable reader with independent Linux-authored fixture contracts."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def check(tool: Path, image: Path, manifest: dict) -> int:
    cases = 0

    def run(*args: str, tree: int = 5, success: bool = True) -> bytes:
        result = subprocess.run([str(tool), "--tree", str(tree), str(image), *args],
                                capture_output=True, timeout=60)
        if success:
            assert result.returncode == 0, (image.name, args, result.stderr.decode(errors="replace"))
        else:
            assert result.returncode != 0, (image.name, args, "unexpected success")
        return result.stdout

    def stat(path: str) -> dict:
        return json.loads(run("stat", path))

    info = json.loads(run("info"))
    assert info["node_size"] == manifest["node_size"] and info["sector_size"] == 4096
    cases += 1
    for name, expected in manifest["files"].items():
        data = run("cat", "/" + name)
        assert len(data) == expected["size"]
        assert hashlib.sha256(data).hexdigest() == expected["sha256"]
        for offset, length in [(0, 1), (17, 3001), (4093, 8199), (131067, 41), (len(data), 100)]:
            assert run("cat", name, str(offset), str(length)) == data[offset:offset + length]
        cases += 6
    greeting = stat("greeting")
    hardlink = stat("hardlink")
    assert (greeting["uid"], greeting["gid"], greeting["mode"] & 0o7777, greeting["links"]) == (1001, 1002, 0o640, 2)
    assert (greeting["tree"], greeting["inode"]) == (hardlink["tree"], hardlink["inode"])
    assert run("cat", "symlink") == b"greeting"
    assert run("xattr", "greeting", "user.text") == b"Linux xattr"
    assert run("xattr", "greeting", "user.binary") == bytes([0, 1, 127, 255])
    names = set(run("listxattr", "greeting").rstrip(b"\0").split(b"\0"))
    assert {b"user.text", b"user.binary"} <= names
    cases += 6
    entries = run("ls", "many").splitlines()
    assert len(entries) == 700
    names = [bytes.fromhex(line.split(b"\t")[-1].decode()) for line in entries]
    assert set(names) == {f"entry-{i:04}".encode() for i in range(700)}
    cookies = [int(line.split(b"\t")[3]) for line in entries]
    assert cookies == sorted(set(cookies))
    for i in [0, 1, 199, 349, 698, 699]:
        assert run("cat", f"many/entry-{i:04}") == f"{i}\n".encode()
    run("stat", "many/not-present", success=False)
    cases += 8
    sparse = bytearray(8388608)
    sparse[17:21] = b"LEFT"
    sparse[7340035:7340040] = b"RIGHT"
    assert run("cat", "sparse") == sparse
    assert run("cat", "sparse", "4095", "5001") == sparse[4095:9096]
    assert run("cat", "preallocated") == bytes(1048576)
    assert stat("huge")["size"] == 17179869191
    assert run("cat", "huge", "17179869180", "64") == bytes(11)
    assert run("cat", os.fsdecode(b"raw-\xff")) == b"raw name\n"
    cases += 6
    assert run("cat", "subvol/value") == b"subvolume changed\n"
    assert run("cat", "snapshot/value") == b"snapshot original\n"
    sub = stat("subvol")
    snap = stat("snapshot")
    assert sub["inode"] == snap["inode"] == 256 and sub["tree"] != snap["tree"]
    assert run("cat", "value", tree=sub["tree"]) == b"subvolume changed\n"
    assert run("cat", "value", tree=snap["tree"]) == b"snapshot original\n"
    selected = json.loads(run("info", tree=0))["tree"]
    assert selected == (sub["tree"] if manifest["profile"] == "default-subvolume" else 5)
    cases += 6
    for path in [".", "..", "many/..", "subvol/..", "snapshot/.."]:
        assert stat(path)["tree"] == 5 and stat(path)["inode"] == 256
    assert run("cat", "../value", tree=sub["tree"]) == b"subvolume changed\n"
    cases += 6
    return cases


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    args = parser.parse_args()
    profiles = ["plain", "small-nodes", "large-nodes", "zlib", "zstd", "default-subvolume"]
    total = 0
    for profile in profiles:
        image = args.fixtures / f"{profile}.raw"
        manifest = json.loads((args.fixtures / f"{profile}.json").read_text())
        before = hashlib.file_digest(image.open("rb"), "sha256").hexdigest()
        cases = check(args.tool.resolve(), image.resolve(), manifest)
        after = hashlib.file_digest(image.open("rb"), "sha256").hexdigest()
        assert before == after, "reader changed the image"
        total += cases + 1
        print(f"{profile}: {cases + 1} contract checks PASS; image unchanged", flush=True)
    print(f"{total} independent image contract checks PASS")


if __name__ == "__main__":
    main()
