#!/usr/bin/env python3
"""Tree-log replay through btrfs-inspect on copies of Linux images whose last
fsyncs were never committed (tests/prepare_log_linux.py).

A replayed copy must hold exactly the namespace Linux replays from the same
image, in the manifest format of that script. An inspection and every refused
replay leave the copy byte-identical; read-only access keeps refusing a volume
with a pending log; malformed logs are rejected.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

PROFILES = ("logs", "logs-many")
SUPER_OFFSET = 65536
SECONDARY_OFFSET = 64 * 1024 * 1024
SUPER_SIZE = 4096
CSUM_SIZE = 32
GENERATION_OFFSET = 72
ROOT_OFFSET = 80
LOG_ROOT_OFFSET = 96
LOG_ROOT_LEVEL_OFFSET = 200
MAX_LEVEL = 8
# Node header fields.
HEADER_BYTENR_OFFSET = 48
HEADER_OWNER_OFFSET = 88
HEADER_SIZE = 101
TREE_LOG_OBJECTID = 2**64 - 6
SECTOR = 4096
MODE_TYPE = 0o170000
MODE_DIRECTORY = 0o040000
MODE_REGULAR = 0o100000
MODE_SYMLINK = 0o120000
REPLAYED = 0
PENDING = 3
NO_LOG = 4


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


def read(path: Path, offset: int, size: int) -> bytes:
    with path.open("rb") as image:
        image.seek(offset)
        return image.read(size)


def patch(path: Path, offset: int, data: bytes) -> None:
    with path.open("r+b") as image:
        image.seek(offset)
        image.write(data)


def patch_super(path: Path, offset: int, field: int, data: bytes) -> None:
    """Changes one field of a CRC32C superblock copy and keeps it checksum-valid."""
    body = bytearray(read(path, offset, SUPER_SIZE))
    body[field:field + len(data)] = data
    struct.pack_into("<I", body, 0, crc32c(bytes(body[CSUM_SIZE:])))
    body[4:CSUM_SIZE] = bytes(CSUM_SIZE - 4)
    patch(path, offset, bytes(body))


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def log_root_copies(path: Path, log_root: int) -> list[int]:
    """Physical offsets of the log root node's copies, found by their headers."""
    found = []
    with path.open("rb") as image:
        offset = 0
        while True:
            block = image.read(SECTOR)
            if len(block) < HEADER_SIZE:
                return found
            if (u64(block, HEADER_BYTENR_OFFSET) == log_root and
                    u64(block, HEADER_OWNER_OFFSET) == TREE_LOG_OBJECTID):
                found.append(offset)
            offset += len(block)


def scratch_copy(source: Path, path: Path) -> Path:
    with source.open("rb") as input_file, path.open("xb") as output:
        while chunk := input_file.read(1 << 24):
            output.write(chunk)
    return path


def manifest_line(entry: dict) -> str:
    """The manifest line prepare_log_linux.py prints for one walked path."""
    path = bytes.fromhex(entry["path"]).decode("utf-8", "surrogateescape")
    kind = {MODE_DIRECTORY: "dir", MODE_REGULAR: "file",
            MODE_SYMLINK: "symlink"}.get(entry["mode"] & MODE_TYPE, "other")
    data = bytes.fromhex(entry.get("data", ""))
    content = "-"
    mtime = "-"
    if kind == "file":
        content = hashlib.sha256(data).hexdigest()
        mtime = str(entry["mtime"])
    elif kind == "symlink":
        content = data.decode("utf-8", "surrogateescape")
    xattrs = "".join(
        f"{bytes.fromhex(name).decode('utf-8', 'surrogateescape')}=0x{value} "
        for name, value in sorted(entry["xattrs"], key=lambda item: bytes.fromhex(item[0])))
    fields = [path, kind, entry["inode"], f"{entry['mode'] & 0o7777:o}", entry["uid"],
              entry["gid"], entry["links"], entry["size"], content, mtime, xattrs or "-"]
    return "\t".join(str(field) for field in fields)


class Checker:
    def __init__(self, tool: Path) -> None:
        self.tool = tool
        self.failures = 0

    def run(self, image: Path, *args: str) -> tuple[int, str]:
        result = subprocess.run([str(self.tool), str(image), *args], capture_output=True,
                                text=True, check=False)
        return result.returncode, result.stdout

    def replay(self, image: Path, apply: bool) -> tuple[int, dict]:
        status, output = self.run(image, "replay", *(["--apply"] if apply else []))
        report = json.loads(output) if output.strip() else {}
        return status, report

    def expect(self, name: str, condition: bool, detail: str = "") -> None:
        suffix = f": {detail}" if detail and not condition else ""
        print(f"{'PASS' if condition else 'FAIL'} {name}{suffix}")
        self.failures += 0 if condition else 1

    def refused(self, name: str, image: Path, result: str) -> None:
        """Neither an inspection nor an applied replay changes the image."""
        before = digest(image)
        for apply in (False, True):
            status, report = self.replay(image, apply)
            self.expect(f"{name} {'apply' if apply else 'inspect'} refused",
                        status != REPLAYED and report.get("result") == result and
                        report.get("logs") == 0, f"status {status} {report}")
        self.expect(f"{name} unchanged", digest(image) == before)

    def profile(self, fixtures: Path, profile: str, scratch: Path) -> None:
        source = fixtures / f"{profile}.raw"
        expected = (fixtures / f"{profile}.expected.tsv").read_text().splitlines()
        image = scratch_copy(source, scratch / f"{profile}.raw")
        before = digest(image)
        primary = read(image, SUPER_OFFSET, SUPER_SIZE)
        generation = u64(primary, GENERATION_OFFSET)
        log_root = u64(primary, LOG_ROOT_OFFSET)
        self.expect(f"{profile} fixture has a pending log", log_root != 0)

        status, _ = self.run(image, "info")
        self.expect(f"{profile} read-only mount refuses a pending log", status != 0)
        status, report = self.replay(image, False)
        self.expect(f"{profile} inspection replays", status == PENDING, f"{status} {report}")
        self.expect(f"{profile} inspection writes nothing", digest(image) == before)

        status, report = self.replay(image, True)
        self.expect(f"{profile} replay", status == REPLAYED, f"{status} {report}")
        print(f"{profile} report {json.dumps(report, sort_keys=True)}")
        status, report = self.replay(image, False)
        self.expect(f"{profile} no log after replay", status == NO_LOG, f"{status} {report}")
        status, output = self.run(image, "info")
        info = json.loads(output) if status == 0 else {}
        self.expect(f"{profile} replay commits one generation",
                    info.get("generation") == generation + 1, f"{status} {info}")
        self.namespace(f"{profile}", image, expected)

        self.malformed(source, profile, scratch, log_root)
        self.recovery(source, profile, image, expected, generation)

    def namespace(self, name: str, image: Path, expected: list[str]) -> None:
        status, output = self.run(image, "walk", "--data")
        actual = sorted((manifest_line(json.loads(line)) for line in output.splitlines()),
                        key=lambda line: line.split("\t")[0].encode("utf-8", "surrogateescape"))
        self.expect(f"{name} walk", status == 0)
        missing = sorted(set(expected) - set(actual))
        extra = sorted(set(actual) - set(expected))
        self.expect(f"{name} namespace equals Linux's replay ({len(expected)} paths)",
                    actual == expected,
                    f"{len(missing)} missing, {len(extra)} extra; "
                    f"first missing {missing[:3]}, first extra {extra[:3]}")

    def recover(self, name: str, image: Path, selected: int, generation: int,
                rewritten: int) -> None:
        status, output = self.run(image, "recover", "--apply")
        report = json.loads(output) if output.strip() else {}
        self.expect(f"{name} recovery", status == 0 and report.get("selected") == selected and
                    report.get("generation") == generation and
                    report.get("rewritten") == rewritten, f"{status} {report}")

    def recovery(self, source: Path, profile: str, replayed: Path, expected: list[str],
                 generation: int) -> None:
        """Copies that disagree beyond the log are recovered before replay."""
        name = f"{profile} older secondary"
        image = scratch_copy(source, replayed.parent / f"{profile}-secondary.raw")
        secondary = read(image, SECONDARY_OFFSET, SUPER_SIZE)
        patch_super(image, SECONDARY_OFFSET, GENERATION_OFFSET,
                    struct.pack("<Q", u64(secondary, GENERATION_OFFSET) - 1))
        self.refused(name, image, "recovery required")
        # The primary keeps its log; the rewritten secondary names none.
        self.recover(name, image, 0, generation, 1)
        repaired = read(image, SECONDARY_OFFSET, SUPER_SIZE)
        self.expect(f"{name} rewritten without the log",
                    u64(repaired, LOG_ROOT_OFFSET) == 0 and repaired[LOG_ROOT_LEVEL_OFFSET] == 0 and
                    u64(repaired, GENERATION_OFFSET) == generation)
        status, report = self.replay(image, True)
        self.expect(f"{name} replay", status == REPLAYED, f"{status} {report}")
        self.namespace(name, image, expected)
        image.unlink()

        # A replay commit interrupted before its primary: the secondaries hold
        # the replayed generation, the primary still names the log.
        name = f"{profile} interrupted replay commit"
        patch(replayed, SUPER_OFFSET, read(source, SUPER_OFFSET, SUPER_SIZE))
        self.refused(name, replayed, "recovery required")
        self.recover(name, replayed, 1, generation + 1, 1)
        status, report = self.replay(replayed, False)
        self.expect(f"{name} no log after recovery", status == NO_LOG, f"{status} {report}")
        self.namespace(name, replayed, expected)

    def malformed(self, source: Path, profile: str, scratch: Path, log_root: int) -> None:
        def copy(name: str) -> Path:
            return scratch_copy(source, scratch / f"{profile}-{name}.raw")

        image = copy("level")
        patch_super(image, SUPER_OFFSET, LOG_ROOT_LEVEL_OFFSET, bytes([MAX_LEVEL]))
        self.refused(f"{profile} log level beyond the tree bound", image,
                     "corrupt filesystem")
        image.unlink()

        image = copy("owner")
        tree_root = u64(read(image, SUPER_OFFSET, SUPER_SIZE), ROOT_OFFSET)
        patch_super(image, SUPER_OFFSET, LOG_ROOT_OFFSET, struct.pack("<Q", tree_root))
        self.refused(f"{profile} log root naming another tree's node", image,
                     "corrupt filesystem")
        image.unlink()

        image = copy("node")
        copies = log_root_copies(image, log_root)
        self.expect(f"{profile} log root node copies found", len(copies) >= 1, f"{copies}")
        for offset in copies:
            byte = read(image, offset + HEADER_SIZE, 1)[0]
            patch(image, offset + HEADER_SIZE, bytes([byte ^ 0xFF]))
        self.refused(f"{profile} log root failing its checksum", image,
                     "corrupt filesystem")
        image.unlink()

        # A log is Linux's only in the primary.
        image = copy("secondary-log")
        primary = read(image, SUPER_OFFSET, SUPER_SIZE)
        patch_super(image, SECONDARY_OFFSET, LOG_ROOT_OFFSET,
                    primary[LOG_ROOT_OFFSET:LOG_ROOT_OFFSET + 8])
        self.refused(f"{profile} log named by a secondary copy", image, "recovery required")
        before = digest(image)
        status, output = self.run(image, "recover", "--apply")
        self.expect(f"{profile} recovery refuses a log outside the primary",
                    status == 1 and json.loads(output).get("result") ==
                    "unsupported format or operation" and digest(image) == before,
                    f"{status} {output}")
        image.unlink()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    args = parser.parse_args()
    checker = Checker(args.tool.resolve())
    with tempfile.TemporaryDirectory(prefix="btrfs-log-replay-") as directory:
        for profile in PROFILES:
            source = args.fixtures / f"{profile}.raw"
            if not source.exists():
                print(f"FAIL {profile} fixture missing: {source}")
                checker.failures += 1
                continue
            checker.profile(args.fixtures, profile, Path(directory))
            for image in Path(directory).glob(f"{profile}*.raw"):
                image.unlink()
    print(f"{'PASS' if checker.failures == 0 else 'FAIL'} log replay: {checker.failures} failures")
    raise SystemExit(1 if checker.failures else 0)


if __name__ == "__main__":
    main()
