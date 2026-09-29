#!/usr/bin/env python3
"""Guest-only acceptance for a mounted disposable Btrfs fixture; never mounts devices."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import errno
import mmap
import os
from pathlib import Path
import stat
import tempfile


def expect_error(expected: int, operation) -> None:
    try:
        operation()
    except OSError as error:
        assert error.errno == expected, (expected, error)
    else:
        raise AssertionError(f"operation succeeded; expected errno {expected}")


def read_contracts(root: Path) -> None:
    greeting = root / "greeting"
    assert greeting.read_bytes() == b"hello from Linux Btrfs\n"
    info = greeting.stat()
    assert (info.st_uid, info.st_gid, stat.S_IMODE(info.st_mode), info.st_nlink) == (1001, 1002, 0o640, 2)
    assert info.st_ino == (root / "hardlink").stat().st_ino
    assert os.readlink(root / "symlink") == "greeting"
    assert (root / "symlink").read_bytes() == greeting.read_bytes()
    assert os.getxattr(greeting, "user.text") == b"Linux xattr"
    assert os.getxattr(greeting, "user.binary") == bytes([0, 1, 127, 255])
    assert {"user.text", "user.binary"} <= set(os.listxattr(greeting))
    assert set(os.listdir(root / "many")) == {f"entry-{i:04}" for i in range(700)}
    assert Path(os.fsdecode(os.fsencode(root) + b"/raw-\xff")).read_bytes() == b"raw name\n"
    assert (root / "subvol/value").read_bytes() == b"subvolume changed\n"
    assert (root / "snapshot/value").read_bytes() == b"snapshot original\n"
    for suffix in ("", "/value"):
        original = (root / ("subvol" + suffix)).stat()
        snapshot = (root / ("snapshot" + suffix)).stat()
        assert (original.st_dev, original.st_ino) != (snapshot.st_dev, snapshot.st_ino)
    assert (root / "subvol/..").stat().st_ino == root.stat().st_ino

    def reader(index: int) -> None:
        with (root / "big").open("rb") as file:
            with mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as mapping:
                assert len(mapping) == 4 * 1024 * 1024
                for step in range(32):
                    offset = (index * 65537 + step * 32003) % (len(mapping) - 65539)
                    expected = bytes((offset + i) & 255 for i in range(65539))
                    assert os.pread(file.fileno(), len(expected), offset) == expected
                    assert mapping[offset:offset + len(expected)] == expected
                assert os.pread(file.fileno(), 100, len(mapping)) == b""
        assert len(list((root / "many").iterdir())) == 700

    with ThreadPoolExecutor(max_workers=8) as executor:
        list(executor.map(reader, range(8)))
    sparse = bytearray(8388608)
    sparse[17:21] = b"LEFT"
    sparse[7340035:7340040] = b"RIGHT"
    assert (root / "sparse").read_bytes() == sparse
    assert (root / "preallocated").read_bytes() == bytes(1048576)
    with (root / "huge").open("rb") as file:
        assert os.fstat(file.fileno()).st_size == 17179869191
        assert os.pread(file.fileno(), 64, 17179869180) == bytes(11)


def readonly_contract(root: Path) -> None:
    # Root is required so permission failures cannot masquerade as a read-only mount.
    assert os.geteuid() == 0, "run inside the disposable guest as root"
    probe = root / ".machlin-readonly-probe"
    assert not probe.exists(), "reserved probe path already exists"

    def create() -> None:
        descriptor = os.open(probe, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(descriptor)
        probe.unlink()

    expect_error(errno.EROFS, create)

    def open_for_write() -> None:
        descriptor = os.open(root / "big", os.O_WRONLY)
        os.close(descriptor)

    expect_error(errno.EROFS, open_for_write)


def write_contracts(root: Path) -> None:
    # All names created by this suite live in one uniquely allocated directory.
    with tempfile.TemporaryDirectory(prefix=".machlin-write-contract-", dir=root) as temporary:
        directory = Path(temporary)
        path = directory / "file"
        descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        try:
            expect_error(errno.EEXIST, lambda: exclusive_create(path))
            data = bytes(range(256)) * 4096
            assert os.write(descriptor, data) == len(data)
            replacement = b"unaligned CoW replacement" * 4096
            assert os.pwrite(descriptor, replacement, 4093) == len(replacement)
            expected = bytearray(data)
            expected[4093:4093 + len(replacement)] = replacement
            assert os.pread(descriptor, len(expected), 0) == expected
            os.fsync(descriptor)
            with mmap.mmap(descriptor, 0, access=mmap.ACCESS_WRITE) as mapping:
                mapping[8191:8197] = b"mmap!!"
                mapping.flush()
                expected[8191:8197] = b"mmap!!"
                assert os.pread(descriptor, len(expected), 0) == expected
                assert os.pwrite(descriptor, b"read!!", 8191) == 6
                assert mapping[8191:8197] == b"read!!"
                expected[8191:8197] = b"read!!"
            os.ftruncate(descriptor, 4097)
            os.ftruncate(descriptor, 8193)
            assert os.pread(descriptor, 8193, 0) == expected[:4097] + bytes(4096)
            os.fchmod(descriptor, 0o640)
            assert stat.S_IMODE(path.stat().st_mode) == 0o640
            os.setxattr(path, "user.binary", bytes([0, 1, 127, 255]))
            assert os.getxattr(path, "user.binary") == bytes([0, 1, 127, 255])
            os.removexattr(path, "user.binary")
            assert "user.binary" not in os.listxattr(path)
            linked = directory / "link"
            os.link(path, linked)
            assert linked.stat().st_ino == path.stat().st_ino
            assert path.stat().st_nlink == 2
            symlink = directory / "symlink"
            symlink.symlink_to("link")
            assert symlink.read_bytes() == linked.read_bytes()
            target = directory / "target"
            target.write_bytes(b"old destination")
            os.replace(path, target)
            assert not path.exists() and target.stat().st_ino == linked.stat().st_ino
            child = directory / "child"
            child.mkdir()
            os.rename(target, child / "moved")
            assert (child / "..").stat().st_ino == directory.stat().st_ino
            expect_error(errno.ENOTEMPTY, lambda: child.rmdir())
            linked.unlink()
            (child / "moved").unlink()
            assert os.fstat(descriptor).st_nlink == 0
            assert os.pread(descriptor, 4097, 0) == expected[:4097]
            assert os.pwrite(descriptor, b"open after unlink", 0) == 17
            os.fsync(descriptor)
            child.rmdir()
            directory_fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
        finally:
            os.close(descriptor)


def exclusive_create(path: Path) -> None:
    descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(descriptor)


def main() -> None:
    if not __debug__:
        raise RuntimeError("assertions must be enabled")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mount", required=True, type=Path)
    parser.add_argument("--suite", choices=("readonly", "write"), required=True)
    parser.add_argument("--disposable-guest", action="store_true", required=True,
                        help="Acknowledge that this is a disposable guest fixture, not host media")
    args = parser.parse_args()
    root = args.mount.resolve()
    assert root != Path("/") and os.path.ismount(root), "must name a dedicated mounted fixture"
    if args.suite == "readonly":
        read_contracts(root)
        readonly_contract(root)
    else:
        write_contracts(root)
    print(f"mounted {args.suite} contracts: PASS")


if __name__ == "__main__":
    main()
