#!/usr/bin/env python3
"""Compile the shared core and XNU adapter into an unsigned kernel extension."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("arm64e", "x86_64"), default="arm64e")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/kext")
    args = parser.parse_args()
    environment = os.environ.copy()
    for name in ("CFLAGS", "CPPFLAGS", "LDFLAGS", "CC", "SDKROOT"):
        environment.pop(name, None)
    clang = subprocess.check_output(["xcrun", "--find", "clang"], text=True).strip()
    sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
    headers = Path(sdk) / "System/Library/Frameworks/Kernel.framework/Headers"
    output = args.output.resolve() / args.arch
    objects = output / "objects"
    bundle = output / "MachlinBtrfs.kext/Contents"
    objects.mkdir(parents=True, exist_ok=True)
    (bundle / "MacOS").mkdir(parents=True, exist_ok=True)
    flags = [
        "-arch", args.arch, "-isysroot", sdk, "-mmacosx-version-min=26.4",
        "-mkernel", "-std=c11", "-O2", "-g", "-DKERNEL", "-DKERNEL_EXTENSION",
        "-isystem", str(headers), "-I", str(ROOT / "include"),
        "-ffreestanding", "-fno-builtin", "-fno-common", "-fno-asynchronous-unwind-tables",
        "-Wall", "-Wextra", "-Werror", "-Werror=conditional-uninitialized",
        "-Wdeclaration-after-statement", "-Wframe-larger-than=2048",
    ]
    sources = (sorted((ROOT / "core").glob("*.c")) +
               sorted((ROOT / "adapters/common").glob("*.c")) +
               sorted((ROOT / "adapters/xnu").glob("*.c")))
    object_paths = []
    for source in sources:
        relative = source.relative_to(ROOT).with_suffix("")
        target = objects / ("_".join(relative.parts) + ".o")
        subprocess.run([clang, *flags, "-c", str(source), "-o", str(target)], check=True, env=environment)
        object_paths.append(str(target))
    subprocess.run(
        [clang, "-arch", args.arch, "-isysroot", sdk, "-mmacosx-version-min=26.4",
         "-nostdlib", "-Wl,-kext", "-Wl,-undefined,dynamic_lookup", "-Wl,-no_fixup_chains",
         "-Wl,-no_adhoc_codesign", *object_paths, "-o", str(bundle / "MacOS/machlin_btrfs")],
        check=True, env=environment,
    )
    shutil.copyfile(ROOT / "adapters/xnu/Info.plist", bundle / "Info.plist")
    subprocess.run([clang, "-arch", "arm64" if args.arch == "arm64e" else args.arch,
                    "-isysroot", sdk, "-mmacosx-version-min=26.4", "-std=c11", "-O2",
                    "-Wall", "-Wextra", "-Werror", "-Wdeclaration-after-statement",
                    str(ROOT / "tools/mount_machlin_btrfs.c"), "-o", str(output / "mount_machlin_btrfs")],
                   check=True, env=environment)
    print(f"Unsigned kernel extension: {bundle.parent}")
    print("Compilation does not establish loadability or mounted filesystem acceptance.")


if __name__ == "__main__":
    main()
