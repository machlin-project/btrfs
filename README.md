# Machlin Btrfs

A standalone Btrfs filesystem for macOS and Machlin: one portable C core, an
FSKit application extension, and an XNU filesystem extension. Linux ABI policy
belongs to LXNU in the XNU fork, not to this repository.

The current implementation is a **read-only foundation for continued development**.
It reads single-device CRC32C filesystems with SINGLE/DUP chunks, 4–64 KiB nodes,
inodes, byte-exact names, directories, hard links, symlinks, inline and regular
extents, sparse and preallocated data, raw xattrs, subvolumes and snapshots.
The POSIX image adapter provides zlib and optional Zstd decoding. FSKit supplies
zlib; XNU codec providers remain a separate integration requirement.

Independent Linux-created images are compared byte-for-byte under ASan/UBSan.
The tests also exercise checksum-correct malformed metadata, allocation/I/O
failures, mirror fallback, subvolume identity and structural I/O budgets.
Native build evidence and mounted acceptance are tracked separately in
[acceptance](docs/ACCEPTANCE.md). Writes, recovery, multi-device RAID, complete
native authorization and Linux ABI policy are still open requirements.

The performance goal is to outperform Linux Btrfs on matched filesystem
workloads. Range I/O and reusable traversal paths are present; the goal has **not
been established by a comparative benchmark**. See [performance](docs/PERFORMANCE.md).

```sh
make build
make test MESON_OPTIONS="-Dfixtures=$PWD/artifacts/fixtures"
.build/btrfs-inspect artifacts/fixtures/plain.raw ls /
.build/btrfs-inspect --tree 5 artifacts/fixtures/default-subvolume.raw cat /big
make fskit
make kext
```

`make test` without a fixture option runs only the self-contained tests. It does
not establish image acceptance. The image suite requires every configured
fixture; missing images fail rather than skip. Never use native development
builds on valuable media or install them on the development host.

- [Architecture](docs/ARCHITECTURE.md): ownership, format and concurrency contracts.
- [Development](docs/DEVELOPMENT.md): tools, fixture generation and exact commands.
- [Acceptance](docs/ACCEPTANCE.md): supported/rejected/pending contracts and evidence.
- [Handoff](docs/HANDOFF.md): ordered tasks, test oracles and release gates for the next implementer.

The core and adapters are BSD-3-Clause licensed. Linux and btrfs-progs are external
test oracles; no Linux filesystem implementation is linked into the driver.
The ext4 sibling supplied the native adapter scaffolding and build conventions;
Btrfs disk algorithms and object identity are independently implemented here.
