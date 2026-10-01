#!/usr/bin/env python3
"""Explicit superblock recovery through btrfs-inspect on a copy of a Linux image."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

SUPER_OFFSETS = (65536, 64 * 1024 * 1024)
SUPER_SIZE = 4096
CSUM_SIZE = 32
GENERATION_OFFSET = 72
REQUIRED = 3


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def digest(path: Path) -> str:
    with path.open("rb") as image:
        return hashlib.file_digest(image, "sha256").hexdigest()


def patch(path: Path, offset: int, data: bytes) -> None:
    with path.open("r+b") as image:
        image.seek(offset)
        image.write(data)


def read(path: Path, offset: int) -> bytes:
    with path.open("rb") as image:
        image.seek(offset)
        return image.read(SUPER_SIZE)


def with_generation(copy: bytes, generation: int) -> bytes:
    """A checksum-valid copy recording another generation."""
    body = bytearray(copy)
    struct.pack_into("<Q", body, GENERATION_OFFSET, generation)
    struct.pack_into("<I", body, 0, crc32c(bytes(body[CSUM_SIZE:])))
    body[4:CSUM_SIZE] = bytes(CSUM_SIZE - 4)
    return bytes(body)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    args = parser.parse_args()
    tool = args.tool.resolve()
    checks = 0

    def run(image: Path, *arguments: str) -> tuple[int, dict]:
        result = subprocess.run([str(tool), str(image), "recover", *arguments],
                                capture_output=True, timeout=60)
        return result.returncode, json.loads(result.stdout) if result.stdout else {}

    def mounts(image: Path) -> bool:
        return subprocess.run([str(tool), str(image), "info"], capture_output=True,
                              timeout=60).returncode == 0

    with tempfile.TemporaryDirectory() as scratch:
        image = Path(scratch) / "recovery.raw"
        shutil.copyfile(args.fixture, image)
        primary, mirror = (read(image, offset) for offset in SUPER_OFFSETS)
        generation = struct.unpack_from("<Q", primary, GENERATION_OFFSET)[0]

        status, report = run(image)
        assert status == 0 and report["rewritten"] == 0 and report["generation"] == generation
        checks += 1

        # A destroyed primary: nothing mounts, the dry run names the mirror and
        # writes nothing, the applied recovery rewrites only the primary.
        patch(image, SUPER_OFFSETS[0], bytes(SUPER_SIZE))
        before = digest(image)
        assert not mounts(image)
        status, report = run(image)
        assert status == REQUIRED and report["selected"] == 1, report
        assert digest(image) == before
        status, report = run(image, "--apply")
        assert status == 0 and report["rewritten"] == 1, report
        assert mounts(image) and read(image, SUPER_OFFSETS[0]) == primary
        checks += 4

        # A damaged mirror still mounts but must be recovered before a write.
        patch(image, SUPER_OFFSETS[1] + CSUM_SIZE + 1, b"\xff")
        assert mounts(image)
        status, report = run(image)
        assert status == REQUIRED and report["selected"] == 0, report
        status, report = run(image, "--apply")
        assert status == 0 and report["rewritten"] == 1 and read(image, SUPER_OFFSETS[1]) == mirror
        checks += 3

        # An older valid mirror, as after a crash before the secondaries were
        # written: the newest copy wins and the older one is rewritten.
        patch(image, SUPER_OFFSETS[1], with_generation(mirror, generation - 1))
        status, report = run(image)
        assert status == REQUIRED and report["selected"] == 0 and report["generation"] == generation
        status, report = run(image, "--apply")
        assert status == 0 and read(image, SUPER_OFFSETS[1]) == mirror
        checks += 2

        # Never below the acknowledged generation: refused, nothing written.
        patch(image, SUPER_OFFSETS[0], bytes(SUPER_SIZE))
        before = digest(image)
        status, report = run(image, "--apply", "--acknowledged", str(generation + 1))
        assert status == 1 and report["rewritten"] == 0 and digest(image) == before, report
        checks += 1

        # No valid copy at all: refused, nothing written.
        patch(image, SUPER_OFFSETS[1], bytes(SUPER_SIZE))
        before = digest(image)
        status, report = run(image, "--apply")
        assert status == 1 and report["rewritten"] == 0 and digest(image) == before, report
        checks += 1

        result = subprocess.run([str(tool), str(image), "recover", "--bogus"],
                                capture_output=True, timeout=60)
        assert result.returncode == 2
        checks += 1
    print(f"explicit recovery: {checks} checks PASS")


if __name__ == "__main__":
    main()
