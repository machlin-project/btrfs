#!/usr/bin/env python3
"""Format owned C/Objective-C sources with the selected Xcode toolchain."""

import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    formatter = os.environ.get("CLANG_FORMAT")
    if not formatter:
        if sys.platform != "darwin":
            parser.error("set CLANG_FORMAT to a compatible clang-format (version 21)")
        formatter = subprocess.check_output(
            ["xcrun", "--find", "clang-format"], text=True
        ).strip()
    version = subprocess.check_output([formatter, "--version"], text=True).strip()
    print(version, flush=True)
    sources = []
    for directory in ("core", "include", "adapters", "tests", "tools"):
        for path in (root / directory).rglob("*"):
            if path.is_file() and path.suffix in (".c", ".h", ".m", ".mm"):
                sources.append(str(path.relative_to(root)))
    if not sources:
        return 0
    mode = ["--dry-run", "--Werror"] if args.check else ["-i"]
    result = subprocess.run(
        [formatter, "--style=file", *mode, *sorted(sources)], cwd=root
    )
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
