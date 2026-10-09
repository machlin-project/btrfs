#!/usr/bin/env python3
"""Compare the portable reader with independent Linux-authored fixture contracts."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

# Linux's checksum algorithm numbers by mkfs.btrfs name.
CHECKSUM_TYPES = {"crc32c": 0, "xxhash": 1, "sha256": 2, "blake2": 3}
# The data payload of tests/prepare_linux.py: prefixes of its inputs, a sparse
# file, a preallocation, a zlib property file, a NODATACOW file without
# checksums, an inline file, a reflink and two snapshots.
DATA_PAYLOAD_PROFILES = {"checksums-xxhash", "checksums-sha256", "checksums-blake2",
                         "transactions-metadata-uuid", "transactions-mixed",
                         "transactions-space-cache", "transactions-quota",
                         "transactions-squota", "transactions-verity", "transactions-16k"}
DATA_BIG_BYTES = 1048576
DATA_SMALL_BYTES = 10000
DATA_SPARSE_BYTES = 4194304
DATA_HOLE_A = 1048576
DATA_HOLE_B = 3145728
DATA_PREALLOC_BYTES = 262144
DATA_ZLIB_BYTES = 262144
DATA_NODATASUM_BYTES = 65536
# FS_VERITY_FL and Btrfs's RO_VERITY inode flag.
VERITY_FSFLAG = 0x00100000
VERITY_INODE_FLAG = 1 << 32


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

    def seek(path: str, whence: str, offset: int) -> int | None:
        """The offset btrfs-inspect seeks to, or None for ENXIO."""
        result = subprocess.run([str(tool), "--tree", "5", str(image), "seek", path, whence,
                                 str(offset)], capture_output=True, timeout=60)
        if result.returncode != 0:
            assert result.stderr.strip() == b"not found", (path, whence, offset, result.stderr)
            return None
        return int(result.stdout)

    info = json.loads(run("info"))
    sector = manifest.get("sector_size", 4096)
    assert info["node_size"] == manifest["node_size"] and info["sector_size"] == sector
    assert info["checksum_type"] == CHECKSUM_TYPES[manifest.get("checksum", "crc32c")]
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
    # lseek's SEEK_DATA and SEEK_HOLE as Linux answers them: the sectors Linux
    # wrote are data, never-written and preallocated ranges holes, the end of
    # the file the last hole; at or past the size there is nothing. "RIGHT" is
    # written at 7340035, in the sector from 7340032 for every sector size.
    for path, whence, offset, expected in [
            ("sparse", "data", 0, 0), ("sparse", "hole", 0, sector),
            ("sparse", "data", sector, 7340032), ("sparse", "hole", 7340032, 7340032 + sector),
            ("sparse", "data", 7340032 + sector, None), ("sparse", "hole", 8388607, 8388607),
            ("sparse", "hole", 8388608, None), ("preallocated", "data", 0, None),
            ("preallocated", "hole", 0, 0), ("huge", "data", 0, None),
            ("huge", "hole", 17179869190, 17179869190), ("greeting", "data", 0, 0),
            ("greeting", "hole", 0, 23), ("greeting", "data", 23, None)]:
        assert seek(path, whence, offset) == expected, (path, whence, offset)
        cases += 1
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
    if manifest["profile"] in DATA_PAYLOAD_PROFILES:
        big = run("cat", "big")
        random = run("cat", "random")
        sparse = bytearray(DATA_SPARSE_BYTES)
        sparse[DATA_HOLE_A:DATA_HOLE_A + 6] = b"HOLE-A"
        sparse[DATA_HOLE_B:DATA_HOLE_B + 6] = b"HOLE-B"
        expected = {"big": big[:DATA_BIG_BYTES], "small": random[:DATA_SMALL_BYTES],
                    "sparse": bytes(sparse), "prealloc": bytes(DATA_PREALLOC_BYTES),
                    "zlib": big[:DATA_ZLIB_BYTES], "nodatasum": random[:DATA_NODATASUM_BYTES],
                    "inline": b"inline data\n", "big-clone": big[:DATA_BIG_BYTES]}
        for directory in ("data", "data-snap", "data-ro"):
            for name, data in expected.items():
                assert run("cat", f"{directory}/{name}") == data, (directory, name)
                cases += 1
        assert run("cat", "data/big", "4093", "8199") == big[4093:12292]
        cases += 1
    # fs-verity files read only what their Merkle trees authenticate (the
    # manifest's reads above); their digests equal the independent model's,
    # which Linux's measure matched.
    for path, model in manifest.get("verity", {}).items():
        attributes = stat(path)
        assert attributes["flags"] & VERITY_INODE_FLAG and attributes["fsflags"] & VERITY_FSFLAG
        assert run("verity", path).decode().strip() == model["digest"], path
        cases += 2
    if manifest.get("verity"):
        assert not stat("greeting")["fsflags"] & VERITY_FSFLAG
        run("verity", "greeting", success=False)
        cases += 1
    if manifest["profile"] == "transactions-holes":
        # Without NO_HOLES Linux splits hole items around written sectors and
        # keeps their offsets; a file grown by truncation is one hole item.
        sparse = bytearray(4194304)
        sparse[1048576:1048582] = b"HOLE-A"
        sparse[3145728:3145734] = b"HOLE-B"
        assert run("cat", "data/sparse") == sparse
        assert run("cat", "data/sparse", "1048570", "4200") == sparse[1048570:1052770]
        assert run("cat", "data/grown") == bytes(1048576)
        cases += 3
    return cases


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    args = parser.parse_args()
    profiles = ["plain", "small-nodes", "large-nodes", "zlib", "zstd", "codecs", "sectors-16k",
                "default-subvolume", "transactions-holes", "transactions-block-group-tree",
                *sorted(DATA_PAYLOAD_PROFILES)]
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
