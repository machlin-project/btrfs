# Handoff for the next implementer

Continue the Btrfs driver in this repository. Preserve the existing read core and
its independent Linux oracle. Do not restart from an ext4 write path or treat the
native scaffolding as runtime acceptance. The intended handoff is a substantial
read foundation; the remaining work is not honestly measurable as a fixed 40%
of a full production Btrfs driver.

Read AGENTS.md, ARCHITECTURE.md and ACCEPTANCE.md first. Use `development`, focused
commits and personal Git identity. Main agent owns design, code, test contracts,
diagnosis and acceptance. Delegate routine prepared execution to GPT-6 Luna and
VM/UI operations to GPT-6.1 Sol, using explicit model names from AGENTS.md. Give
absolute directories, exact commands, outputs and success criteria; one VM owner
at a time. Do not rerun completed checks without changes or unresolved evidence.

## What is ready to preserve

- A freestanding C read core, independent of XNU, Foundation, errno and allocator.
- Validated superblock/chunk mapping, iterative B-tree cursor, namespace, parent,
  inode, subvolume/snapshot, extent/checksum and raw xattr reading.
- Streaming directory traversal and range reads with deterministic I/O budgets.
- Shared native identity registry preserving full `(tree, inode)` pairs.
- POSIX oracle adapter, compiling FSKit extension and compiling XNU/UBC adapter.
- Six Linux-authored images with 312 independent contracts; malformed metadata,
  fail-point sweeps, concurrency, sanitizer and bounded fuzzing harnesses.

Reproduce `make test ... -Dfixtures=artifacts/fixtures` and `make check-style` using
DEVELOPMENT.md. Inspect the per-suite output, not just the shell exit code. Images
and tool staging are generated state; a new checkout must recreate them. Keep
support tests separate from tests demonstrating safe rejection.

## First: accept the native read path

1. Reserve dedicated disposable macOS guests through the lab operator. Sign and
   install FSKit with its actual filesystem-module capability; do not fake
   provisioning. Load the XNU adapter through the documented guest kernel workflow.
   Verify the loaded guest kernel/module identity, not merely a successful build.
2. Implement native xattr access and namespace/authorization policy. Preserve raw
   Linux names in the core. Keep native credentials and Linux credentials at their
   owning boundaries. Decide explicitly how native callers see Linux ACLs,
   security attributes and inode flags before claiming multi-user acceptance.
3. Complete the bounded codec providers: zlib, Zstd and LZO with malformed-stream,
   padded-tail, exact-output and allocation-failure tests. Never decode unchecked
   stored data or pull userspace libc into the kernel.
4. Run `tests/mounted_contracts.py --suite readonly --disposable-guest` as root on
   the top-level plain fixture. It requires byte-exact reads, mmap/pread coherence,
   eight concurrent readers, hardlinks, native identity across snapshots, raw
   names, xattrs, holes and EROFS. Missing operations are failures, not skips.
5. Add platform-specific tests for tiny getdirentries buffers, saved/reopened
   cookies, failed pagein without stale-page publication, vnode/FSItem reclaim,
   low memory and forced unmount with in-flight I/O. Reclaim and remount must not
   alias objects; mount-local IDs are not persistent file handles.
6. In the XNU fork, connect Btrfs backing objects to LXNU at the established VFS
   boundary. Keep Linux namei, permissions and retry semantics in `bsd/lxnu/vfs`,
   native glue in `bsd/lxnu/xnu`. Run native regression plus the applicable existing
   Linux conformance suites; update the ABI matrix with actual results.

Acceptance: each native mount passes its own suite in a guest, independently of
portable tests. This work must not change host boot policy or install host kexts.

## Then: establish a transaction engine before mutation APIs

Use explicit components with single owners:

| Component | Owns | Required invariant |
| --- | --- | --- |
| Device persistence interface | Exact reads/writes, flush/barrier, geometry | Failure and partial/torn writes are representable; no assumed persistence |
| Committed view | Immutable root set and generation, active-reader pins | Readers keep a consistent view until release |
| Transaction | Private roots, dirty blocks, reservations, state | No uncommitted pointer is visible through the committed view |
| Allocator | Chunk/block-group free ranges, reservations, ENOSPC | Live roots and pinned readers prevent reuse |
| Reference accounting | Extent and shared backrefs, delayed reference changes | Snapshot/reflink ownership remains correct across overwrite/free |
| Commit publisher | Bottom-up persistence, barriers, superblock generation | A recoverable committed root set always exists |
| Recovery | Super mirrors, log policy, interrupted publication | Never choose a root simply because its generation number is largest |

Proposed transaction states are preparing, writing, publishing, committed and
failed. Define legal transitions and lock order before code. A pre-publication
allocation failure may abort privately; ambiguous persistence failures poison the
writable mount until recovery. Existing read-only callbacks must not grow silent
write side effects. Reject unknown read-only-compatible bits for writable mounts.

Start with single-device CRC32C and full transactions for fsync. Implement tree
insertion/split, deletion/merge and CoW path replacement with reference accounting,
then allocation and publication. Do not claim tree-log recovery until it exists.
The read parser remains the oracle for your writer, but cannot be the only oracle.

Tests before publishing the first writable volume:

- Force leaf/internal splits and merges at each implemented level; after each
  operation verify numeric key order, exact parent generation, bounds and reachability.
- Snapshot a tree, mutate every depth, then prove original leaves and file extents
  are unchanged. Include shared backrefs and extent-offset references.
- Fail every reservation and allocation point; neither leaked allocations nor
  published dangling references are allowed. Test full and fragmented metadata
  space separately from data space.
- Test duplicate keys, name-hash collisions, inode-reference collisions, directory
  index exhaustion and generation overflow explicitly; no magic test-only bypasses.
- After successful commit, reopen with Linux, run `btrfs check --readonly`, mount
  and compare the complete namespace/data/xattr manifest. Never use `--repair` to
  make the acceptance run pass.

## Power-cut oracle: a required test harness, not implemented yet

Implement a test-only block backend with separate durable and volatile images.
Record writes and flushes; support an acknowledged write residing only in volatile
state, delayed/reordered persistence between barriers, torn sectors, failed writes,
failed flushes and abrupt termination. Seed all permutations and bound each run.
The model must match the declared virtual-device persistence contract; dropping
only the last whole write is insufficient.

For every mutation/fsync sequence and every write/flush cut point:

1. Begin from a Linux-verified immutable baseline and retain an expected logical
   manifest of operations and acknowledged durability boundaries.
2. Execute through the fault backend. Keep each possibly durable disk state as a
   disposable artifact, including ambiguous error outcomes.
3. Recover with both the driver and Linux. Run Linux read-only fsck, then compare
   namespace, contents, links, xattrs, shared extents and free-space accounting.
4. Every acknowledged fsync must survive. Unacknowledged operations may resolve
   according to the specified transaction contract, but never leave a torn root
   set, cross-linked allocation, freed live extent or corrupted prior snapshot.
5. Minimize every failure into a deterministic reproducer. Record the cut, device
   geometry, flush result and recovery choice in generated reports, not source
   revision ledgers.

Include data writes, tree splits, rename replacement, parent-directory updates,
truncate, orphan/open-unlink recovery, xattrs, snapshot/reflink and ENOSPC. Check
all relevant superblock mirrors and their backup roots. Native fsync acceptance
must use the actual resource flush interface, not just the simulated backend.

## Mutations and coherence

After the transaction gate, implement create/mkdir, data write/append, truncate,
link/unlink, symlink, atomic rename and xattrs. Keep multi-object lock ordering
explicit. Reuse the core through both adapters. Native UBC remains the file-page
cache; define ordering for dirty pages, truncation, invalidate, mmap, fsync and
transaction publication. Add append/concurrent-rename stress and reference lifetime
tests before broadening concurrency.

The prepared `tests/mounted_contracts.py --suite write --disposable-guest` requires
exclusive creation, unaligned overwrite, mmap/pread/pwrite coherence, shrink/grow
zeroing, permissions, xattr mutation, links, replacement rename, cross-directory
rename, nonempty-directory failure, open-unlink lifetime and file/directory fsync.
It is a starting executable contract, currently unexecuted for this driver. Extend
it with remount persistence and the power-cut oracle; a single live mount pass
does not establish durable writing.

Before LXNU acceptance, extend its conformance suite for sticky/setgid directories,
umask, ACL inheritance, capability-preserving operations, immutable/append flags,
O_PATH, openat2 resolution and Linux-owned description provenance. No post-operation
security repair or elevated native authorization is an acceptable workaround.

## Broaden format support only with its own oracle

Add LZO, alternate checksums, data-DUP, NODATASUM, mixed groups, metadata UUID and
64 KiB-sector fixtures. Add large/fragmented directories, actual name-hash collisions,
inode extrefs and deeper multi-level trees. Raise capacity bounds only after measured
memory/stack coverage. Then address multi-device profiles with explicit device-set,
degraded-read and recovery contracts. Safe rejection of a format remains valuable
but is never counted as implementing that format.

## Performance remains a release target

Follow PERFORMANCE.md. Preserve the existing deterministic budgets when adding
features. Measure before introducing a shared metadata cache: immutable generation
keys, bounded memory, single-flight fills and lifetime/eviction rules must be clear.
Prioritize coalesced native device reads and reusable per-open read contexts; avoid
a mount-wide read mutex. Test non-selected subvolume workloads and attribute-rich
directory enumeration so their costs remain visible.

Claim faster-than-Linux results only after comparable mounted workloads, equal
device transport/resources, identical checksum/compression/durability policy,
alternating runs and correctness/crash gates. Report each workload and tail latency.
Do not infer a win from low core callback counts or a userspace image benchmark.

## Final delivery criteria

Report portable correctness, native compilation, native mounted acceptance, loaded
custom-kernel evidence, LXNU behavior, persistence and performance separately. List
remaining failures and justified filesystem-specific skips by contract. Update
ACCEPTANCE.md with real evidence; remove completed work from this handoff rather
than accumulating contradictory historical claims. A clean build or a focused
commit is a checkpoint, not completion of the driver.
