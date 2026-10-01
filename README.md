# Machlin Btrfs

A standalone Btrfs filesystem for macOS and Machlin: one portable C core, an
FSKit application extension, and an XNU filesystem extension. Linux ABI policy
belongs to LXNU in the XNU fork, not to this repository.

The current implementation has an accepted **read-only XNU mount** and an
experimental portable CoW transaction writer.
It reads single-device CRC32C filesystems with SINGLE/DUP chunks, 4–64 KiB nodes,
inodes, byte-exact names, directories, hard links, symlinks, inline and regular
extents, sparse and preallocated data, raw xattrs, subvolumes and snapshots.
The POSIX image adapter provides zlib and optional Zstd decoding. FSKit supplies
zlib; the XNU adapter also supplies a bounded kernel zlib decoder.

Independent Linux-created images are compared byte-for-byte under ASan/UBSan.
The tests also exercise checksum-correct malformed metadata, allocation/I/O
failures, mirror fallback, subvolume identity and structural I/O budgets.
The XNU guest suite passes on four image profiles, including mmap, native user
xattrs, Unix permissions, directory cookies, snapshots and concurrent reads.

The portable writer writes and truncates regular files as copy-on-write data
with checksums (streaming new data to the device, compressing with zlib or Zstd
and storing small files inline as Linux decides), replaces inline files, and
creates, links, unlinks (with
orphans for open files), renames and sets xattrs and the compression property,
in any writable subvolume or snapshot, many operations per transaction; it
creates, snapshots and deletes subvolumes and drops deleted ones as Linux's
cleaner does. It
updates CoW paths, shared and keyed extent backreferences with Linux's snapshot
rules, data checksum items, block-group accounting, root items and superblock
copies with three persistence barriers. Independent reference, checksum and
namespace audits check every committed state.
Explicit superblock recovery resolves torn or disagreeing copies without rolling
back an acknowledged generation. Linux agrees with the recorded outcome of
reordered and torn crash states on twelve profiles, including namespace facts
and its own cleanup of the orphans left. Native mounts remain read-only: native
writeback and write coherence are remaining work. See
[acceptance](docs/ACCEPTANCE.md) and [the concrete handoff](docs/HANDOFF.md).

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
