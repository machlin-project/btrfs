# Machlin Btrfs

A standalone Btrfs filesystem for macOS and Machlin: one portable C core, an
FSKit application extension, and an XNU filesystem extension. Linux ABI policy
belongs to LXNU in the XNU fork, not to this repository.

Both native adapters mount read-only by default and read-write on request, on a
portable CoW transaction writer: the XNU mount (`mount_machlin_btrfs -w`) and
the FSKit extension, behind a privileged device barrier, pass the mounted read
and write suites, and Linux verifies the images they write. Both keep every
acknowledged fsync through power cuts of their guest; FSKit set-id metadata is
not accepted.
It reads single-device filesystems with CRC32C, XXH64, SHA-256 or BLAKE2b
checksums, SINGLE/DUP chunks, 4–64 KiB nodes, inodes, byte-exact names,
directories, hard links, symlinks, inline and regular extents, sparse and
preallocated data, raw xattrs, subvolumes and snapshots, and verifies fs-verity
files against their Merkle trees on every read. Every adapter reads
zlib, LZO and Zstd extents: the platform's zlib (a bounded kernel decoder in the
XNU adapter) and the shared freestanding LZO and Zstd decoders. Every adapter
writes zlib and Zstd extents; the XNU adapter uses the kernel's deflate and a
shared freestanding Zstd encoder.

Independent Linux-created images are compared byte-for-byte under ASan/UBSan.
The tests also exercise checksum-correct malformed metadata, allocation/I/O
failures, mirror fallback, subvolume identity and structural I/O budgets.
The XNU guest suite passes on four image profiles, including mmap, native user
xattrs, Unix permissions, directory cookies, snapshots and concurrent reads.

The portable writer writes and truncates regular files as copy-on-write data
with checksums (streaming new data to the device, compressing with zlib or Zstd
and storing small files inline as Linux decides), preallocates, zeroes and punches
ranges as Linux's fallocate does, replaces inline files, and
creates, links, unlinks (with
orphans for open files), renames and sets xattrs, the compression property and
inode flags (as `chattr`), in any writable subvolume or snapshot, many
operations per transaction; it creates, snapshots, renames and deletes
subvolumes and drops deleted ones as Linux's cleaner does, and enables
fs-verity as FS_IOC_ENABLE_VERITY does, keeping verity files' data unchanged.
It writes filesystems with mixed groups, a block-group tree or a metadata UUID, leaves a
v1 space cache stale for Linux to rebuild, and accounts full and simple quotas
as Linux does: it rescans qgroups and refuses operations past their limits
(EDQUOT). Native mounts finish Linux's background drops and rescans. It
updates CoW paths, shared and keyed extent backreferences with Linux's snapshot
rules, data checksum items, block-group accounting, root items and superblock
copies with three persistence barriers. Independent reference, checksum and
namespace audits check every committed state.
Explicit superblock recovery resolves torn or disagreeing copies without rolling
back an acknowledged generation, and a Linux tree log left by fsync without a
commit is replayed as Linux's mount replays it; writable mounts do both before
they open, while read-only mounts refuse a pending log and write nothing. Linux agrees with the recorded outcome of
reordered and torn crash states on twelve profiles, including namespace facts
and its own cleanup of the orphans left. The XNU adapter writes through the
unified buffer cache with grouped or synchronous commits; FSKit's block resource
has no device cache flush, so the extension writes only while a root service,
approved by the administrator, flushes that one disk for each barrier. See
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
