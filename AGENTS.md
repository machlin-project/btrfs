# Machlin Btrfs development

This repository owns the standalone Btrfs filesystem, including its FSKit and
XNU adapters. Follow the workspace lab policy for personal Git identity, VM
safety, source history and agent roles. Work on `development`. Read README.md,
docs/ARCHITECTURE.md, docs/ACCEPTANCE.md and docs/HANDOFF.md before changes.

The portable C core owns the disk format and filesystem algorithms. It has no
Foundation, FSKit, LXNU, Darwin errno, libc allocation or native page-cache
dependencies. Adapters own resource lifetime, authorization, synchronization,
codec services and the native page cache. Btrfs identity is (subvolume, inode),
never inode number alone. Preserve that pair through all platform interfaces.

Every disk byte is untrusted. Verify checksums, arithmetic, tree parent generation,
key bounds and ownership before following pointers. Use named wire fields and
constants. All traversal and allocation must have documented finite bounds.
Read-only mounts cannot replay a log, repair mirrors or write any byte.
Unknown features fail explicitly; safe rejection is not feature support.

Use the selected Xcode clang-format and the provided profile. C declarations go
at block starts, with a blank line before statements. Use braces and one statement
per line. English source, comments and documentation only.

The main agent owns architecture, implementation, test design, diagnosis, review
and acceptance. Delegate prepared build/test/fixture runs to GPT-6 Luna
(`gpt-6-luna`), and VM operations to GPT-6.1 Sol (`gpt-6.1-sol`) with explicit absolute
directories, commands, bounds and success criteria. Only one operator per VM.
Standalone commands may run here; lab/VM commands run from the absolute lab
directory. Installed native tests use dedicated disposable guests only.

Keep Linux-authored fixtures and independent Linux verification. Extend the
malformed-input, allocation/I/O fault and sanitizer suites with each feature.
Run focused checks during implementation; full applicable acceptance at a batch
boundary. Never turn missing fixtures, pending tests or unsupported contracts
into a pass. Tests of rejection and tests of support are separate.

Writes require the transaction architecture and power-cut oracle in the handoff;
do not add in-place shortcuts, fake fsync success, post-operation security repairs
or special cases for applications. A future writer must preserve shared extents,
backreferences and the last committed root set under every failed write/barrier.

Report portable tests, native compilation, installed FSKit, loaded XNU and LXNU
semantics separately. Generated images, tools, reports and credentials stay
ignored. Git owns revisions; generated reports own hashes and build identities.
