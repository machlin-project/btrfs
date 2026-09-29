#!/usr/bin/env python3
"""Build the macOS FSKit app, unsigned unless a personal signing team is supplied."""

import argparse
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--team", help="Personal Apple Developer team for automatic signing")
    parser.add_argument("--provision", action="store_true", help="Allow Xcode to fetch signing profiles")
    parser.add_argument("--derived-data", type=Path, default=ROOT / "artifacts/fskit/DerivedData")
    args = parser.parse_args()
    if args.provision and not args.team:
        parser.error("--provision requires --team")
    project = ROOT / "adapters/fskit"
    environment = os.environ.copy()
    for name in ("CFLAGS", "CPPFLAGS", "CXXFLAGS", "LDFLAGS", "CC", "CXX", "SDKROOT"):
        environment.pop(name, None)
    clang = subprocess.check_output(["xcrun", "--find", "clang"], text=True).strip()
    subprocess.run(
        ["xcodegen", "generate", "--spec", str(project / "project.yml"), "--project", str(project)],
        check=True, cwd=ROOT, env=environment,
    )
    command = [
        "xcodebuild", "-project", str(project / "BtrfsFSKit.xcodeproj"),
        "-scheme", "BtrfsFSKitApp", "-configuration", "Debug", "-sdk", "macosx",
        "-destination", "generic/platform=macOS", "-derivedDataPath", str(args.derived_data.resolve()),
        "-jobs", "4", "CLANG_ENABLE_EXPLICIT_MODULES=NO", f"CC={clang}",
    ]
    if args.team:
        command.extend(["CODE_SIGN_STYLE=Automatic", f"DEVELOPMENT_TEAM={args.team}"])
        if args.provision:
            command.append("-allowProvisioningUpdates")
    else:
        command.append("CODE_SIGNING_ALLOWED=NO")
    command.append("build")
    subprocess.run(command, check=True, cwd=ROOT, env=environment)


if __name__ == "__main__":
    main()
