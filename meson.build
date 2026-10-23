project('machlin-btrfs', 'c', version: '0.1.0', meson_version: '>=1.3',
  default_options: ['c_std=c11', 'warning_level=3', 'werror=true', 'buildtype=debugoptimized',
    'b_sanitize=address,undefined'])
cc = meson.get_compiler('c')
py = import('python').find_installation('python3')
inc = include_directories('include', 'core')
warnings = ['-Wdeclaration-after-statement', '-Werror=conditional-uninitialized',
  '-fno-sanitize-recover=all']
add_project_arguments(cc.get_supported_arguments(warnings), language: 'c')
sources = files('core/support.c', 'core/chunk.c', 'core/tree.c', 'core/mount.c',
  'core/inode.c', 'core/directory.c', 'core/parent.c', 'core/read.c', 'core/xattr.c',
  'core/mutable.c', 'core/space.c', 'core/transaction.c', 'core/recovery.c', 'core/backref.c', 'core/csum.c',
  'core/data.c', 'core/fst.c', 'core/namespace.c', 'core/rename.c', 'core/subvolume.c', 'core/drop.c',
  'core/group.c', 'core/cache.c', 'core/checksum.c', 'core/replay.c', 'core/qgroup.c',
  'core/verity.c', 'core/heuristic.c', 'core/relocation.c', 'core/balance.c',
  'core/device.c', 'core/scrub.c',
  'core/raid56.c')
fuzz_flags = get_option('fuzzer') ? ['-fsanitize=fuzzer-no-link'] : []
if get_option('fuzzer')
  add_project_link_arguments('-fsanitize=fuzzer-no-link', language: 'c')
endif
core = static_library('btrfs_core', sources, include_directories: inc,
  c_args: ['-ffreestanding', '-fno-builtin'] + fuzz_flags)
identity = static_library('btrfs_identity', 'adapters/common/identity.c', include_directories: inc)
native = static_library('btrfs_native', 'adapters/common/native.c', include_directories: inc)
volume = static_library('btrfs_volume', 'adapters/common/volume.c', include_directories: inc)
staging = static_library('btrfs_staging', 'adapters/common/staging.c', include_directories: inc,
  c_args: ['-ffreestanding', '-fno-builtin'])
staging_test = executable('btrfs-staging-test', 'tests/staging.c', include_directories: inc,
  link_with: staging)
test('device-write-staging', staging_test, suite: 'core')
static_library('btrfs_freestanding', sources, include_directories: inc,
  override_options: ['b_sanitize=none', 'optimization=2'],
  c_args: ['-ffreestanding', '-fno-builtin', '-Wframe-larger-than=2048'])
zlib = dependency('zlib')
zstd = dependency('libzstd', required: false)
codec_flags = zstd.found() ? ['-DBTRFS_HAVE_ZSTD=1'] : []
codec = static_library('btrfs_codec', 'adapters/common/codec.c', include_directories: inc,
  c_args: ['-ffreestanding', '-fno-builtin'])
lzo = dependency('lzo2', required: false, include_type: 'system')
codec_test = executable('btrfs-codec-test', 'tests/codec.c', include_directories: inc,
  link_with: codec, dependencies: [zstd, lzo],
  c_args: codec_flags + (lzo.found()
    ? ['-DBTRFS_HAVE_LZO=1', '-isystem', lzo.get_variable(pkgconfig: 'includedir')] : []))
test('extent-codecs', codec_test, suite: 'core', timeout: 600)
posix = static_library('btrfs_posix', 'adapters/posix/image.c',
  include_directories: inc, dependencies: [zlib, zstd], c_args: codec_flags, link_with: codec)
inspect = executable('btrfs-inspect', 'tools/inspect.c', 'tools/balance_args.c',
  include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd])
executable('btrfs-bench', 'tools/bench.c', include_directories: inc,
  link_with: [volume, posix, core], dependencies: [zlib, zstd, dependency('threads')])
executable('btrfs-fsync-bench', 'tools/fsync_bench.c',
  override_options: ['b_sanitize=none'])
transaction_test = executable('btrfs-transaction-test', 'tests/transaction.c',
  'tests/scenario_device.c', 'tests/scenario_plan.c', 'tests/scenario_run.c',
  'tests/scenario_sets.c', 'tests/scenario_namespace.c', 'tests/scenario_checks.c',
  'tests/scenario_random.c', 'tests/scenario_subvolume.c', 'tests/scenario_quota.c',
  'tests/scenario_verity.c', 'tests/scenario_twins.c', 'tests/references.c',
  'tests/namespace_audit.c', 'tests/qgroup_audit.c',
  include_directories: inc, link_with: [volume, posix, codec, core], dependencies: [zlib, zstd])
reference_audit = executable('btrfs-reference-audit', 'tests/audit.c', 'tests/references.c',
  'tests/namespace_audit.c', 'tests/qgroup_audit.c', include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd])
unit = executable('btrfs-unit', 'tests/unit.c', include_directories: inc, link_with: core)
test('wire-and-api', unit, suite: 'core')
mutable_test = executable('btrfs-mutable-test', 'tests/mutable.c', include_directories: inc,
  link_with: core)
test('private-cow-trees', mutable_test, suite: 'core', timeout: 180)
adversarial = executable('btrfs-adversarial', 'tests/adversarial.c', include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd])
verity_test = executable('btrfs-verity-test', 'tests/verity.c', include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd])
replay_test = executable('btrfs-replay-test', 'tests/replay.c', include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd])
identity_test = executable('btrfs-identity-test', 'tests/identity.c', include_directories: inc,
  link_with: identity)
test('native-object-identity', identity_test, suite: 'core')
crc_test = executable('btrfs-crc-test', 'tests/crc.c', include_directories: inc,
  link_with: core)
test('crc32c', crc_test, suite: 'core')
crc_portable_test = executable('btrfs-crc-portable-test', 'tests/crc.c', 'core/support.c',
  'core/checksum.c',
  include_directories: inc, c_args: ['-DBT_CRC_PORTABLE'])
test('crc32c-portable', crc_portable_test, suite: 'core')
xxhash = dependency('libxxhash', required: false)
b2 = dependency('libb2', required: false)
checksum_test = executable('btrfs-checksum-test', 'tests/checksum.c', include_directories: inc,
  link_with: core, dependencies: [xxhash, b2],
  c_args: (xxhash.found() ? ['-DBTRFS_HAVE_XXHASH=1'] : []) +
    (b2.found() ? ['-DBTRFS_HAVE_B2=1'] : []))
test('checksum-algorithms', checksum_test, suite: 'core')
native_test = executable('btrfs-native-policy', 'tests/native_policy.c', include_directories: inc,
  link_with: [native, core])
test('native-xattr-policy', native_test, suite: 'core')
executable('btrfs-mounted-test', 'tests/mounted.c', dependencies: dependency('threads'),
  override_options: ['b_sanitize=none'])
if host_machine.system() == 'darwin'
  executable('btrfs-mounted-write-test', 'tests/mounted_write.c',
    dependencies: dependency('threads'), override_options: ['b_sanitize=none'])
  executable('btrfs-power-cut', 'tests/power_cut.c', override_options: ['b_sanitize=none'])
  executable('btrfs-mounted-walk-test', 'tests/mounted_walk.c',
    override_options: ['b_sanitize=none'])
endif
logwrites_test = executable('btrfs-logwrites-test', 'tests/logwrites.c',
  'tests/references.c', 'tests/namespace_audit.c', include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd])
relocation_test = executable('btrfs-relocation-test', 'tests/relocation.c',
  'tests/references.c', 'tests/namespace_audit.c', include_directories: inc,
  link_with: [volume, posix, core], dependencies: [zlib, zstd])
multidev_test = executable('btrfs-multidev-test', 'tests/multidev.c', 'tests/references.c',
  'tests/namespace_audit.c', include_directories: inc, link_with: [posix, core],
  dependencies: [zlib, zstd])
multidev_write_test = executable('btrfs-multidev-write-test', 'tests/multidev_write.c',
  'tests/references.c', 'tests/namespace_audit.c', include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd])
volume_devices_test = executable('btrfs-volume-devices-test', 'tests/volume_devices.c',
  'tests/references.c', 'tests/namespace_audit.c', include_directories: inc,
  link_with: [volume, posix, core], dependencies: [zlib, zstd, dependency('threads')])
volume_balance_test = executable('btrfs-volume-balance-test', 'tests/volume_balance.c',
  'tests/references.c', 'tests/namespace_audit.c', include_directories: inc,
  link_with: [volume, posix, core], dependencies: [zlib, zstd, dependency('threads')])
space_caches_test = executable('btrfs-space-caches-test', 'tests/space_caches.c',
  'tests/references.c', 'tests/namespace_audit.c', include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd])
scrub_test = executable('btrfs-scrub-test', 'tests/scrub.c', 'tests/references.c',
  'tests/namespace_audit.c', include_directories: inc, link_with: [posix, core],
  dependencies: [zlib, zstd])
balance_test = executable('btrfs-balance-test', 'tests/balance.c', 'tests/references.c',
  'tests/namespace_audit.c', 'tests/qgroup_audit.c', include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd])
large_sectors_test = executable('btrfs-large-sectors-test', 'tests/large_sectors.c',
  'tests/references.c', 'tests/namespace_audit.c', include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd])
relocation_log_test = executable('btrfs-relocation-log-test', 'tests/relocation_log.c',
  'tests/references.c', 'tests/namespace_audit.c', 'tests/qgroup_audit.c',
  include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd])
volume_test = executable('btrfs-volume-test', 'tests/volume.c', include_directories: inc,
  link_with: [volume, posix, core], dependencies: [zlib, zstd, dependency('threads')])
full_volume_test = executable('btrfs-full-volume-test', 'tests/full_volume.c',
  include_directories: inc, link_with: [volume, posix, core],
  dependencies: [zlib, zstd, dependency('threads')])
volume_window_test = executable('btrfs-volume-window-test', 'tests/volume.c',
  'adapters/common/volume.c', include_directories: inc, c_args: ['-DBTRFS_VOLUME_TEST_HOOKS'],
  link_with: [posix, core], dependencies: [zlib, zstd, dependency('threads')])
concurrent = executable('btrfs-concurrent', 'tests/concurrent.c', include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd, dependency('threads')])
extents_test = executable('btrfs-extents-test', 'tests/extents.c', include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd, dependency('threads')])
# The FSKit volume against stand-ins for the framework's resource and packer.
fskit_available = host_machine.system() == 'darwin' and add_languages('objc', required: false,
  native: false)
if fskit_available
  fskit_volume_test = executable('btrfs-fskit-volume-test', 'tests/fskit_volume.m',
    'adapters/fskit/BtrfsFileSystem.m', 'adapters/fskit/BtrfsVolumeMutation.m',
    'adapters/fskit/BtrfsDeviceBarrier.m', 'adapters/fskit/BtrfsPeerIdentity.m',
    'tests/namespace_audit.c', 'tests/references.c', include_directories: inc,
    objc_args: ['-fobjc-arc', '-fblocks', '-mmacosx-version-min=26.4'],
    link_args: ['-framework', 'Foundation', '-framework', 'FSKit', '-framework', 'Security',
      '-mmacosx-version-min=26.4'],
    link_with: [posix, identity, native, volume, core], dependencies: [zlib, zstd])
endif
if get_option('fuzzer')
  executable('btrfs-fuzz-codec', 'tests/fuzz_codec.c', include_directories: inc,
    link_with: codec, dependencies: [zstd, lzo],
    c_args: ['-fsanitize=fuzzer'] + codec_flags + (lzo.found()
      ? ['-DBTRFS_HAVE_LZO=1', '-isystem', lzo.get_variable(pkgconfig: 'includedir')] : []),
    link_args: ['-fsanitize=fuzzer'])
  executable('btrfs-fuzz', 'tests/fuzz_metadata.c', include_directories: inc,
    link_with: [posix, core], dependencies: [zlib, zstd],
    c_args: ['-fsanitize=fuzzer'], link_args: ['-fsanitize=fuzzer'])
endif
fixtures = get_option('fixtures')
if fixtures != ''
  if not import('fs').is_absolute(fixtures)
    fixtures = join_paths(meson.project_source_root(), fixtures)
  endif
  test('linux-images', py, args: [files('tests/check_images.py'),
    '--tool', inspect, '--fixtures', fixtures], timeout: 300, suite: 'images')
  test('explicit-recovery', py, args: [files('tests/check_recovery.py'), '--tool', inspect,
    '--fixture', join_paths(fixtures, 'transactions.raw')], timeout: 120, suite: 'images')
  test('tree-log-replay', py, args: [files('tests/check_log_replay.py'), '--tool', inspect,
    '--fixtures', fixtures], timeout: 300, suite: 'images')
  test('tree-log-replay-faults', replay_test, args: [join_paths(fixtures, 'logs.raw')],
    timeout: 1800, suite: 'images')
  # A balance Linux left unmerged, and every crash state of a recorded
  # balance: readable, recovered as Linux's mount recovers them.
  test('interrupted-balance', relocation_test, args: [join_paths(fixtures, 'relocation.raw'),
    join_paths(fixtures, 'relocation.expected.tsv')], timeout: 600, suite: 'images')
  test('interrupted-balance-states', relocation_log_test,
    args: [join_paths(fixtures, 'relocation-log.raw'),
      join_paths(fixtures, 'relocation-log-data.raw'),
      join_paths(fixtures, 'relocation.expected.tsv')], timeout: 3600, suite: 'images')
  test('interrupted-balance-fsync-states', relocation_log_test,
    args: [join_paths(fixtures, 'relocation-fsync-log.raw'),
      join_paths(fixtures, 'relocation-fsync-log-data.raw'),
      join_paths(fixtures, 'relocation.expected.tsv')], timeout: 3600, suite: 'images')
  test('interrupted-balance-squota-states', relocation_log_test,
    args: [join_paths(fixtures, 'relocation-squota-log.raw'),
      join_paths(fixtures, 'relocation-squota-log-data.raw'),
      join_paths(fixtures, 'relocation.expected.tsv')], timeout: 3600, suite: 'images')
  test('interrupted-balance-quota-states', relocation_log_test,
    args: [join_paths(fixtures, 'relocation-quota-log.raw'),
      join_paths(fixtures, 'relocation-quota-log-data.raw'),
      join_paths(fixtures, 'relocation.expected.tsv')], timeout: 3600, suite: 'images')
  test('linux-crash-states', logwrites_test, args: [join_paths(fixtures, 'logwrites.log'),
    join_paths(fixtures, 'logwrites-data.raw')], timeout: 600, suite: 'images')
  test('verity-reads', verity_test, args: [join_paths(fixtures, 'transactions-verity.raw')],
    timeout: 300, suite: 'images')
  test('malformed-faults-and-io-budgets', adversarial,
    args: [join_paths(fixtures, 'plain.raw')], timeout: 180, suite: 'images')
  test('malformed-faults-16k', adversarial, args: [join_paths(fixtures, 'sectors-16k.raw')],
    timeout: 180, suite: 'images')
  test('malformed-faults-64k', adversarial, args: [join_paths(fixtures, 'sectors-64k.raw')],
    timeout: 180, suite: 'images')
  foreach algorithm : ['xxhash', 'blake2']
    test('malformed-faults-' + algorithm, adversarial,
      args: [join_paths(fixtures, 'checksums-' + algorithm + '.raw')], timeout: 180,
      suite: 'images')
  endforeach
  test('concurrent-readers', concurrent,
    args: [join_paths(fixtures, 'plain.raw')], timeout: 180, suite: 'images')
  test('decompressed-extents', extents_test, args: [join_paths(fixtures, 'zstd.raw'),
    join_paths(fixtures, 'zlib.raw')], timeout: 180, suite: 'images')
  if fskit_available
    test('fskit-volume', fskit_volume_test, args: [join_paths(fixtures, 'plain.raw'),
      join_paths(fixtures, 'transactions.raw')], timeout: 180, suite: 'images')
  endif
  test('inline-transactions', transaction_test,
    args: [join_paths(fixtures, 'transactions.raw')], timeout: 180, suite: 'images')
  test('inline-transactions-dup', transaction_test,
    args: [join_paths(fixtures, 'transactions-dup.raw')], timeout: 180, suite: 'images')
  test('three-superblock-copies', transaction_test,
    args: [join_paths(fixtures, 'transactions-copies.raw')], timeout: 300, suite: 'images')
  test('inline-transactions-large', transaction_test,
    args: [join_paths(fixtures, 'transactions-large.raw')], timeout: 180, suite: 'images')
  test('shared-references', transaction_test,
    args: ['--shared', join_paths(fixtures, 'transactions-shared.raw')], timeout: 300, suite: 'images')
  test('data-extents', transaction_test,
    args: ['--data', join_paths(fixtures, 'transactions-data.raw')], timeout: 300, suite: 'images')
  foreach algorithm : ['xxhash', 'sha256', 'blake2']
    # BLAKE2b with 64 KiB nodes and DUP data takes about ten minutes, more under
    # load.
    test('data-extents-' + algorithm, transaction_test,
      args: ['--data', join_paths(fixtures, 'checksums-' + algorithm + '.raw')], timeout: 1200,
      suite: 'images')
  endforeach
  # The kernel adapter's encoders write the compressed extents.
  test('data-extents-kernel-codecs', transaction_test,
    args: ['--data', '--kernel-codecs', join_paths(fixtures, 'transactions-data.raw')],
    timeout: 600, suite: 'images')
  test('holes-and-dup-data', transaction_test,
    args: ['--data', '--holes', join_paths(fixtures, 'transactions-holes.raw')], timeout: 600,
    suite: 'images')
  test('free-space-conversion', transaction_test,
    args: ['--convert', join_paths(fixtures, 'transactions-convert.raw')], timeout: 600,
    suite: 'images')
  test('block-group-removal', transaction_test,
    args: ['--groups', join_paths(fixtures, 'transactions.raw')], timeout: 300, suite: 'images')
  test('block-group-removal-dup', transaction_test,
    args: ['--groups', join_paths(fixtures, 'transactions-dup.raw')], timeout: 300,
    suite: 'images')
  test('free-space-tree', transaction_test,
    args: ['--data', '--fragment', join_paths(fixtures, 'transactions-fst.raw')], timeout: 600,
    suite: 'images')
  test('block-group-growth', transaction_test,
    args: ['--grow', join_paths(fixtures, 'transactions-grow.raw')], timeout: 900,
    suite: 'images')
  test('keyed-references', transaction_test,
    args: ['--keyed', join_paths(fixtures, 'transactions-keyed.raw')], timeout: 300, suite: 'images')
  # Format features: a metadata UUID, mixed groups and a v1 space cache under the
  # inline and data scenarios, the block-group tree under the inline and group
  # scenarios.
  foreach profile : ['metadata-uuid', 'mixed', 'space-cache']
    test('inline-transactions-' + profile, transaction_test,
      args: [join_paths(fixtures, 'transactions-' + profile + '.raw')], timeout: 300,
      suite: 'images')
    test('data-extents-' + profile, transaction_test,
      args: ['--data', join_paths(fixtures, 'transactions-' + profile + '.raw')], timeout: 900,
      suite: 'images')
  endforeach
  # Quotas: every audited state's qgroup numbers equal an independent count.
  test('inline-transactions-quota', transaction_test,
    args: [join_paths(fixtures, 'transactions-quota.raw')], timeout: 300, suite: 'images')
  test('data-extents-quota', transaction_test,
    args: ['--data', join_paths(fixtures, 'transactions-quota.raw')], timeout: 900,
    suite: 'images')
  test('quota-transactions', transaction_test,
    args: ['--quota', join_paths(fixtures, 'transactions-quota.raw')], timeout: 600,
    suite: 'images')
  # Simple quotas: each extent counts for its owner from the enabling
  # generation on, as btrfs check counts it.
  test('inline-transactions-squota', transaction_test,
    args: [join_paths(fixtures, 'transactions-squota.raw')], timeout: 300, suite: 'images')
  test('data-extents-squota', transaction_test,
    args: ['--data', join_paths(fixtures, 'transactions-squota.raw')], timeout: 900,
    suite: 'images')
  test('squota-transactions', transaction_test,
    args: ['--squota', join_paths(fixtures, 'transactions-squota.raw')], timeout: 600,
    suite: 'images')
  # fs-verity: Linux's verity files keep their data, enables store the tree
  # and descriptor as Linux does, and the first enable sets the feature.
  test('inline-transactions-verity', transaction_test,
    args: [join_paths(fixtures, 'transactions-verity.raw')], timeout: 300, suite: 'images')
  test('data-extents-verity', transaction_test,
    args: ['--data', join_paths(fixtures, 'transactions-verity.raw')], timeout: 900,
    suite: 'images')
  test('verity-transactions', transaction_test,
    args: ['--verity', join_paths(fixtures, 'transactions-verity.raw')], timeout: 900,
    suite: 'images')
  # The same on 64 KiB sectors, which a 64 KiB-page Linux kernel wrote.
  test('inline-transactions-verity-64k', transaction_test,
    args: [join_paths(fixtures, 'verity-64k.raw')], timeout: 300, suite: 'images')
  test('verity-transactions-64k', transaction_test,
    args: ['--verity', join_paths(fixtures, 'verity-64k.raw')], timeout: 900,
    suite: 'images')
  test('verity-feature', transaction_test,
    args: ['--verity', join_paths(fixtures, 'transactions.raw')], timeout: 300,
    suite: 'images')
  # 16 KiB sectors, written by a 16 KiB-page Linux kernel: Linux's sector
  # rounding decides the zeroed, preallocated and compressed ranges.
  test('inline-transactions-16k', transaction_test,
    args: [join_paths(fixtures, 'transactions-16k.raw')], timeout: 300, suite: 'images')
  test('data-extents-16k', transaction_test,
    args: ['--data', join_paths(fixtures, 'transactions-16k.raw')], timeout: 900,
    suite: 'images')
  # Compression twins: copies of the files Linux wrote under compress=zlib,
  # compress-force=zlib and a zlib property, written in the same steps, have
  # their twins' extent layouts and NOCOMPRESS flags, with 4 KiB and 16 KiB
  # sectors.
  test('compression-twins', transaction_test,
    args: ['--twins', join_paths(fixtures, 'transactions-twins.raw')], timeout: 900,
    suite: 'images')
  test('compression-twins-16k', transaction_test,
    args: ['--twins', join_paths(fixtures, 'transactions-16k-twins.raw')], timeout: 900,
    suite: 'images')
  # 64 KiB sectors, which no kernel on an Apple CPU mounts, written by
  # btrfs-progs: every file reads as its manifest names it, and the crash
  # states and written files of transactions read back.
  test('transactions-64k', transaction_test,
    args: [join_paths(fixtures, 'sectors-64k.raw')], timeout: 600, suite: 'images')
  test('large-sectors', large_sectors_test, args: [join_paths(fixtures, 'sectors-64k.raw'),
    join_paths(fixtures, 'sectors-64k.expected.tsv'), meson.current_build_dir()],
    timeout: 300, suite: 'images')
  # Volumes Linux wrote across several devices: every profile reads as
  # Linux's manifest says, from any of its devices, and the kernel's chunks
  # are planned again as it placed them; writes, a balance and conversions
  # with their crash states; on unequal devices, new data chunks; devices
  # added, resized, replaced and removed.
  foreach profile : [['multi-raid1', 2, []], ['multi-raid1c3', 3, []],
      ['multi-raid1c4', 4, []], ['multi-raid10', 4, []], ['multi-raid0', 3, []],
      ['multi-raid5', 3, []], ['multi-raid6', 4, []], ['multi-single', 2, []],
      ['multi-grow', 3, ['--grow']], ['multi-spare', 3, ['--devices', meson.current_build_dir()]]]
    images = []
    foreach index : range(profile[1])
      images += join_paths(fixtures, profile[0] + '-' + index.to_string() + '.raw')
    endforeach
    test(profile[0], multidev_test,
      args: [join_paths(fixtures, profile[0] + '.expected.tsv'),
        join_paths(fixtures, 'plain.raw')] + images, timeout: 300, suite: 'images')
    test(profile[0] + '-writes', multidev_write_test,
      args: profile[2] + [profile[0], join_paths(fixtures, profile[0] + '.expected.tsv'),
        '-'] + images, timeout: 1200, suite: 'images')
    # On RAID5, the growth's new chunk is a RAID5 one, and the write into
    # the committed preallocation goes in place into rows holding nothing
    # else committed.
    if profile[0] == 'multi-raid5'
      test('multi-raid5-growth-writes', multidev_write_test,
        args: ['--grow', profile[0], join_paths(fixtures, profile[0] + '.expected.tsv'),
          '-'] + images, timeout: 1200, suite: 'images')
    endif
    # The device changes again on RAID6 data and RAID5 metadata: the
    # missing device is rebuilt from parity.
    if profile[0] == 'multi-spare'
      test('multi-spare-parity-writes', multidev_write_test,
        args: profile[2] + ['--parity', profile[0],
          join_paths(fixtures, profile[0] + '.expected.tsv'), '-'] + images,
        timeout: 2400, suite: 'images')
    endif
  endforeach
  # Scrub: one copy of every seventh tree block and checksummed data sector
  # corrupted, found, and rewritten from a good copy where there is one.
  foreach profile : [['multi-raid1', 2], ['multi-raid10', 4], ['multi-raid1c4', 4],
      ['multi-raid0', 3], ['multi-raid5', 3], ['multi-raid6', 4], ['checksums-blake2', 1],
      ['transactions', 1]]
    images = []
    foreach index : range(profile[1])
      images += join_paths(fixtures, profile[1] == 1 ? profile[0] + '.raw'
        : profile[0] + '-' + index.to_string() + '.raw')
    endforeach
    test('scrub-' + profile[0], scrub_test,
      args: (profile[0] == 'transactions' ? ['--faults'] : []) +
        [profile[0], meson.current_build_dir()] + images, timeout: 600, suite: 'images')
  endforeach
  # The native volume layer on several devices: grouped operations write
  # every device; the volume reads the same from each and degraded.
  foreach profile : [['multi-raid1', 2, 'redundant'], ['multi-raid5', 3, 'redundant'],
      ['multi-raid6', 4, 'redundant'], ['multi-raid0', 3, 'striped']]
    images = []
    foreach index : range(profile[1])
      images += join_paths(fixtures, profile[0] + '-' + index.to_string() + '.raw')
    endforeach
    test('volume-devices-' + profile[0], volume_devices_test,
      args: [profile[2], meson.current_build_dir()] + images, timeout: 300, suite: 'images')
  endforeach
  # Balance and scrub on a mounted volume while readers digest its views.
  test('volume-balance-transactions-data', volume_balance_test,
    args: [meson.current_build_dir(), join_paths(fixtures, 'transactions-data.raw')],
    timeout: 600, suite: 'images')
  test('volume-balance-multi-raid5', volume_balance_test,
    args: [meson.current_build_dir()] + [join_paths(fixtures, 'multi-raid5-0.raw'),
      join_paths(fixtures, 'multi-raid5-1.raw'), join_paths(fixtures, 'multi-raid5-2.raw')],
    timeout: 600, suite: 'images')
  # v1 space caches: an emptied group goes with its cache.
  test('space-caches', space_caches_test,
    args: [join_paths(fixtures, 'space-caches.raw'), meson.current_build_dir()],
    timeout: 300, suite: 'images')
  # Balance: every block group relocated on its own, with its crash states,
  # then all in turn; the files read as before and sharing stays. A full
  # volume refuses for space; v1 space caches go with their groups.
  foreach profile : ['transactions', 'transactions-shared', 'transactions-namespace',
      'transactions-dup', 'transactions-data', 'transactions-quota', 'transactions-squota',
      'transactions-mixed',
      'transactions-block-group-tree', 'transactions-twins', 'transactions-holes',
      'transactions-verity', 'transactions-keyed', 'transactions-fst',
      'transactions-metadata-uuid', 'transactions-grow', 'transactions-full',
      'transactions-16k', 'sectors-16k', 'sectors-64k', 'checksums-xxhash', 'checksums-blake2',
      'codecs', 'default-subvolume', 'transactions-space-cache', 'space-caches']
    test('balance-' + profile, balance_test,
      args: [join_paths(fixtures, profile + '.raw'), '64'], timeout: 900, suite: 'images')
  endforeach
  # Conversions of every kind to DUP and of data back to single, with the
  # usage and soft filters.
  foreach profile : ['transactions', 'transactions-mixed', 'transactions-shared',
      'transactions-quota', 'sectors-64k']
    test('balance-convert-' + profile, balance_test,
      args: ['--convert', join_paths(fixtures, profile + '.raw'), '64'], timeout: 900,
      suite: 'images')
  endforeach
  test('balance-faults', balance_test,
    args: ['--faults', join_paths(fixtures, 'transactions.raw'), '64'], timeout: 900,
    suite: 'images')
  test('inline-transactions-block-group-tree', transaction_test,
    args: [join_paths(fixtures, 'transactions-block-group-tree.raw')], timeout: 300,
    suite: 'images')
  test('block-group-removal-tree', transaction_test,
    args: ['--groups', join_paths(fixtures, 'transactions-block-group-tree.raw')], timeout: 300,
    suite: 'images')
  test('linux-reference-audit', reference_audit, args: [
    join_paths(fixtures, 'plain.raw'), join_paths(fixtures, 'small-nodes.raw'),
    join_paths(fixtures, 'large-nodes.raw'), join_paths(fixtures, 'zlib.raw'),
    join_paths(fixtures, 'zstd.raw'), join_paths(fixtures, 'default-subvolume.raw'),
    join_paths(fixtures, 'transactions.raw'), join_paths(fixtures, 'transactions-dup.raw'),
    join_paths(fixtures, 'transactions-large.raw'), join_paths(fixtures, 'transactions-full.raw'),
    join_paths(fixtures, 'transactions-shared.raw'), join_paths(fixtures, 'transactions-keyed.raw'),
    join_paths(fixtures, 'transactions-data.raw'), join_paths(fixtures, 'transactions-fst.raw'),
    join_paths(fixtures, 'transactions-grow.raw'),
    join_paths(fixtures, 'transactions-namespace.raw'),
    join_paths(fixtures, 'transactions-holes.raw'),
    join_paths(fixtures, 'transactions-convert.raw'),
    join_paths(fixtures, 'transactions-copies.raw'),
    join_paths(fixtures, 'checksums-xxhash.raw'), join_paths(fixtures, 'checksums-sha256.raw'),
    join_paths(fixtures, 'checksums-blake2.raw'),
    join_paths(fixtures, 'transactions-metadata-uuid.raw'),
    join_paths(fixtures, 'transactions-block-group-tree.raw'),
    join_paths(fixtures, 'transactions-mixed.raw'),
    join_paths(fixtures, 'transactions-space-cache.raw'),
    join_paths(fixtures, 'transactions-quota.raw'),
    join_paths(fixtures, 'transactions-squota.raw'),
    join_paths(fixtures, 'transactions-verity.raw'),
    join_paths(fixtures, 'sectors-16k.raw'), join_paths(fixtures, 'transactions-16k.raw'),
    join_paths(fixtures, 'transactions-twins.raw'),
    join_paths(fixtures, 'transactions-16k-twins.raw'), join_paths(fixtures, 'relocation.raw'),
    join_paths(fixtures, 'sectors-64k.raw')],
    suite: 'images')
  test('namespace-transactions', transaction_test,
    args: ['--namespace', join_paths(fixtures, 'transactions-namespace.raw')], timeout: 600,
    suite: 'images')
  test('subvolume-transactions', transaction_test,
    args: ['--subvolume', join_paths(fixtures, 'transactions-namespace.raw')], timeout: 600,
    suite: 'images')
  test('random-writer', transaction_test,
    args: ['--random', '1', '16', join_paths(fixtures, 'transactions-namespace.raw')],
    timeout: 900, suite: 'images')
  test('random-writer-dup', transaction_test,
    args: ['--random-quick', '1', '500', join_paths(fixtures, 'transactions-dup.raw')],
    timeout: 900, suite: 'images')
  foreach profile : ['transactions-data', 'checksums-xxhash', 'checksums-sha256',
      'checksums-blake2', 'transactions-metadata-uuid', 'transactions-mixed',
      'transactions-space-cache']
    test('full-volume-' + profile, full_volume_test,
      args: [join_paths(fixtures, profile + '.raw'), meson.current_build_dir()], timeout: 600,
      suite: 'images')
  endforeach
  test('native-volume-views', volume_test,
    args: [join_paths(fixtures, 'transactions.raw')], timeout: 120, suite: 'images')
  test('native-volume-stress', volume_test,
    args: ['--stress', '1000', join_paths(fixtures, 'transactions.raw')], timeout: 300,
    suite: 'images')
  test('native-volume-pin-window', volume_window_test,
    args: ['--window', join_paths(fixtures, 'transactions.raw')], timeout: 120, suite: 'images')
  test('native-volume-grouped', volume_test,
    args: ['--grouped', '2000', join_paths(fixtures, 'transactions.raw')], timeout: 300,
    suite: 'images')
  test('metadata-exhaustion', transaction_test,
    args: ['--full', join_paths(fixtures, 'transactions-full.raw')], timeout: 180, suite: 'images')
endif
run_target('format', command: [py, files('scripts/format.py')])
run_target('check-style', command: [py, files('scripts/format.py'), '--check'])
