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
  'core/mutable.c', 'core/space.c', 'core/transaction.c')
fuzz_flags = get_option('fuzzer') ? ['-fsanitize=fuzzer-no-link'] : []
if get_option('fuzzer')
  add_project_link_arguments('-fsanitize=fuzzer-no-link', language: 'c')
endif
core = static_library('btrfs_core', sources, include_directories: inc,
  c_args: ['-ffreestanding', '-fno-builtin'] + fuzz_flags)
identity = static_library('btrfs_identity', 'adapters/common/identity.c', include_directories: inc)
native = static_library('btrfs_native', 'adapters/common/native.c', include_directories: inc)
static_library('btrfs_freestanding', sources, include_directories: inc,
  override_options: ['b_sanitize=none', 'optimization=2'],
  c_args: ['-ffreestanding', '-fno-builtin', '-Wframe-larger-than=2048'])
zlib = dependency('zlib')
zstd = dependency('libzstd', required: false)
codec_flags = zstd.found() ? ['-DBTRFS_HAVE_ZSTD=1'] : []
posix = static_library('btrfs_posix', 'adapters/posix/image.c',
  include_directories: inc, dependencies: [zlib, zstd], c_args: codec_flags)
inspect = executable('btrfs-inspect', 'tools/inspect.c', include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd])
transaction_test = executable('btrfs-transaction-test', 'tests/transaction.c', include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd])
unit = executable('btrfs-unit', 'tests/unit.c', include_directories: inc, link_with: core)
test('wire-and-api', unit, suite: 'core')
mutable_test = executable('btrfs-mutable-test', 'tests/mutable.c', include_directories: inc,
  link_with: core)
test('private-cow-trees', mutable_test, suite: 'core', timeout: 180)
adversarial = executable('btrfs-adversarial', 'tests/adversarial.c', include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd])
identity_test = executable('btrfs-identity-test', 'tests/identity.c', include_directories: inc,
  link_with: identity)
test('native-object-identity', identity_test, suite: 'core')
native_test = executable('btrfs-native-policy', 'tests/native_policy.c', include_directories: inc,
  link_with: [native, core])
test('native-xattr-policy', native_test, suite: 'core')
executable('btrfs-mounted-test', 'tests/mounted.c', dependencies: dependency('threads'),
  override_options: ['b_sanitize=none'])
concurrent = executable('btrfs-concurrent', 'tests/concurrent.c', include_directories: inc,
  link_with: [posix, core], dependencies: [zlib, zstd, dependency('threads')])
if get_option('fuzzer')
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
  test('malformed-faults-and-io-budgets', adversarial,
    args: [join_paths(fixtures, 'plain.raw')], timeout: 180, suite: 'images')
  test('concurrent-readers', concurrent,
    args: [join_paths(fixtures, 'plain.raw')], timeout: 180, suite: 'images')
  test('inline-transactions', transaction_test,
    args: [join_paths(fixtures, 'transactions.raw')], timeout: 180, suite: 'images')
endif
run_target('format', command: [py, files('scripts/format.py')])
run_target('check-style', command: [py, files('scripts/format.py'), '--check'])
