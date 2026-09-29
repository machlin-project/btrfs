# Acceptance

This repository is a working portable read-only foundation with compiling native
adapters. It is not yet an accepted mounted filesystem, a writable driver or a
completed LXNU backend. A percentage of completed files would hide the remaining
transaction, recovery and native-policy work, so readiness is recorded by contract.

## Evidence established

| Layer | Result | Evidence |
| --- | --- | --- |
| Linux fixture creation | Six profiles passed Linux content checks and read-only btrfs check | Generated `logs/linux-reference-*.log`; `tests/prepare_linux.py` |
| Portable image reader | 312 independent contract checks passed; all six images unchanged | `tests/check_images.py`; `.build/meson-logs/testlog.txt` |
| Wire/API and native identity | Passed, including 65,536 distinct subvolume identities and exhaustion | `tests/unit.c`, `tests/identity.c` |
| Corruption and failure injection | Passed under ASan/UBSan | `tests/adversarial.c` |
| Concurrent immutable readers | Eight threads passed; separate ThreadSanitizer run passed | `tests/concurrent.c`; generated TSAN logs |
| Bounded fuzz smoke test | Passed with LLVM ASan/UBSan; see generated log for run count and coverage | `tests/fuzz_metadata.c`; `logs/fuzz-final.log` |
| Freestanding core | Compiled with a 2 KiB frame warning promoted to error | Meson freestanding target and both kext builds |
| FSKit | Unsigned application/extension compiled | `logs/fskit.log` |
| XNU arm64e and x86_64 | Unsigned kexts and mount helpers compiled; no unresolved owned core symbols | `logs/kext-arm64e.log`, `logs/kext-x86_64.log` |
| Installed FSKit / loaded XNU | **Not run** | Guest installation and mounted tests remain open |
| LXNU semantic acceptance | **Not run** | No LXNU hooks or ABI-matrix completion claimed |
| Writes and crash recovery | **Not implemented** | No core write callback exists |
| Matched Linux performance comparison | **Not run** | Structural I/O budgets only; see PERFORMANCE.md |

The portable suite has five test processes: wire/API, identities, independent
images, adversarial/fault/budget checks and concurrent readers. The 312 image
contracts are assertions within one of those processes, not 312 Meson tests.
No configured image is silently skipped. Source revisions belong to Git; detailed
runtime identities, logs and any artifact hashes belong in generated reports.

## Supported and bounded read contracts

| Contract | Current coverage and boundary |
| --- | --- |
| Mount and feature admission | Primary CRC32C superblock, one device, SINGLE/DUP mapping; default or explicit subvolume |
| Geometry | Independent images have 4 KiB sectors and 4/16/64 KiB nodes; larger admitted sectors need additional oracles |
| Metadata | CRC, UUID, bytenr, generation, tree levels, key ranges, payload bounds; DUP fallback exercised |
| Namespace | Raw byte names, hardlinks, symlink payload, 700-entry multi-leaf directory, resumable streams, dot/parent lookup |
| Identity | `(subvolume, inode)` preserved; native mount-local IDs cannot alias or be reused after reclaim |
| File data | Inline, regular, shared extents with offset, holes, preallocation, unaligned/EOF reads and files beyond 32 bits |
| Compression | Linux zlib and Zstd images pass through POSIX codec providers; FSKit supplies zlib; XNU providers pending |
| Xattrs | Raw binary values and size/list/get contracts; slash allowed in xattr keys, rejected in directory components; native mapping pending |
| Subvolumes/snapshots | Default selection, explicit roots, distinct identity, cross-root parent, shared original data after source overwrite |
| Resource safety | Read and allocation fail-point sweeps, allocation balance, finite traversal and allocation bounds |
| Cost budgets | 4 MiB read: 7 calls / 4 allocations; 700-entry stream: 5 calls / 3 allocations on accepted plain image |

Data-DUP fallback, NODATASUM, mixed block groups, metadata UUID and less common
admitted layouts need a wider independent Linux fixture matrix before claiming
complete feature coverage. Admission of their format bits is not a passing test.

## Rejection is not feature support

Unknown incompatible features, alternate checksum algorithms, multi-device
layouts, unsupported RAID profiles and missing codecs return unsupported. A
pending tree log returns recovery-required; no log replay or read-only repair is
performed. Corrupt metadata, failed checksums and invalid arithmetic fail with
explicit errors. Unknown read-only compatible flags are admitted only because
the core has no write interface. Do not reuse that decision in a future writer.

Primary-superblock failure has no automatic rollback to older super mirrors.
RAID recovery, LZO, XXHash/SHA256/BLAKE2, zoned devices, extent-tree-v2 and block
group-tree layouts have no release acceptance. Capacity limits are documented in
ARCHITECTURE.md and must remain explicit errors.

## Native and Linux-policy gaps

FSKit and XNU still require real mount, read, mmap/pagein, short-buffer readdir,
I/O error, reclaim, concurrent unmount and codec tests. The XNU adapter retains
UBC and routes its strategy reads through verified core reads; compilation alone
does not prove that page-cache integration works. FSKit registration, provisioning
and filesystem-module signing have not been tested for this repository.

Raw xattrs are available to the image core, but native xattr access and Linux ACL,
security/capability and immutable/append-only policy are not complete. Do not
expose security-sensitive volumes as an accepted multi-user filesystem until the
owning native/LXNU authorization contracts are implemented and tested. Preserve
native credentials; broad authorization and root impersonation are not solutions.

`tests/mounted_contracts.py` contains prepared guest-only read-only and future
writable acceptance. It currently has no Machlin execution result. The read suite
deliberately requires native xattrs and identity correctness; the writer suite
deliberately requires real mutation, mmap coherence and fsync. Missing features
must fail these suites. They do not replace the crash oracle in HANDOFF.md.
