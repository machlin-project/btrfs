#!/usr/bin/env python3
"""Build or archive the macOS FSKit app, unsigned unless a personal signing team is
supplied, with an optional Xcode distribution export of the archive."""

import argparse
import os
from pathlib import Path
import pwd
import subprocess


ROOT = Path(__file__).resolve().parents[1]
APP_NAME = "Machlin btrfs.app"


def verify_app(app: Path, environment: dict[str, str]) -> None:
    subprocess.run(
        ["codesign", "--verify", "--deep", "--strict", "--all-architectures", str(app)],
        check=True, cwd=ROOT, env=environment,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--team", help="Personal Apple Developer team for signing")
    parser.add_argument("--provision", action="store_true", help="Allow Xcode to fetch signing profiles")
    parser.add_argument("--app-profile", help="App development profile name or UUID (requires --extension-profile)")
    parser.add_argument("--extension-profile", help="Extension development profile name or UUID (requires --app-profile)")
    parser.add_argument("--clean", action="store_true", help="Clean products before building, for example after changing profiles")
    parser.add_argument("--configuration", choices=("Debug", "Release"),
                        help="Defaults to Release for archives, Debug for builds")
    parser.add_argument("--build-number", type=int, help="Positive bundle build number for every bundle")
    parser.add_argument("--profile-io", action="store_true",
                        help="Diagnostic build: log timed device I/O, barriers and volume syncs")
    parser.add_argument("--derived-data", type=Path, default=ROOT / "artifacts/fskit/DerivedData")
    parser.add_argument("--archive-path", type=Path, help="Create a new Xcode archive instead of a build")
    parser.add_argument("--export-path", type=Path,
                        help="Export the archive to a new distribution directory")
    parser.add_argument("--export-options", type=Path,
                        help="Xcode ExportOptions.plist for --export-path")
    args = parser.parse_args()
    if args.export_path or args.export_options:
        if not (args.archive_path and args.export_path and args.export_options and args.team):
            parser.error("export requires --archive-path, --export-path, --export-options and --team")
        if not args.export_options.is_file():
            parser.error("--export-options must name an existing plist file")
    if args.build_number is not None and args.build_number <= 0:
        parser.error("--build-number must be positive")
    if args.provision and not args.team:
        parser.error("--provision requires --team")
    manual = bool(args.app_profile or args.extension_profile)
    if manual and not (args.team and args.app_profile and args.extension_profile):
        parser.error("manual signing requires --team, --app-profile and --extension-profile")
    archive_path = args.archive_path.resolve() if args.archive_path else None
    export_path = args.export_path.resolve() if args.export_path else None
    for output in (archive_path, export_path):
        if output and output.exists():
            parser.error(f"output already exists: {output}")
    configuration = args.configuration or ("Release" if archive_path else "Debug")
    derived_data = args.derived_data.resolve()
    project = ROOT / "adapters/fskit"
    environment = os.environ.copy()
    username = pwd.getpwuid(os.getuid()).pw_name
    environment.setdefault("USER", username)
    environment.setdefault("LOGNAME", username)
    for name in ("CFLAGS", "CPPFLAGS", "CXXFLAGS", "LDFLAGS", "CC", "CXX", "SDKROOT"):
        environment.pop(name, None)
    clang = subprocess.check_output(["xcrun", "--find", "clang"], text=True).strip()
    subprocess.run(
        ["xcodegen", "generate", "--spec", str(project / "project.yml"), "--project", str(project)],
        check=True, cwd=ROOT, env=environment,
    )
    command = [
        "xcodebuild", "-project", str(project / "BtrfsFSKit.xcodeproj"),
        "-scheme", "BtrfsFSKitApp", "-configuration", configuration, "-sdk", "macosx",
        "-destination", "generic/platform=macOS", "-derivedDataPath", str(derived_data),
        "-jobs", "4", "CLANG_ENABLE_EXPLICIT_MODULES=NO", f"CC={clang}",
    ]
    if args.build_number is not None:
        command.append(f"CURRENT_PROJECT_VERSION={args.build_number}")
    if args.profile_io:
        command.append("GCC_PREPROCESSOR_DEFINITIONS=$(inherited) BTRFS_PROFILE_IO=1")
    if args.team:
        command.extend([
            f"CODE_SIGN_STYLE={'Manual' if manual else 'Automatic'}",
            "CODE_SIGN_IDENTITY=Apple Development",
            f"DEVELOPMENT_TEAM={args.team}",
        ])
        if manual:
            command.extend([
                f"BTRFS_APP_PROFILE={args.app_profile}",
                f"BTRFS_EXTENSION_PROFILE={args.extension_profile}",
            ])
        if args.provision:
            command.append("-allowProvisioningUpdates")
    else:
        command.append("CODE_SIGNING_ALLOWED=NO")
    if archive_path:
        command.extend(["-archivePath", str(archive_path)])
    if args.clean:
        command.append("clean")
    command.append("archive" if archive_path else "build")
    subprocess.run(command, check=True, cwd=ROOT, env=environment)
    if args.team:
        app = (archive_path / "Products/Applications" / APP_NAME if archive_path else
               derived_data / "Build/Products" / configuration / APP_NAME)
        verify_app(app, environment)
    if export_path:
        # Export selects the distribution identity and profiles, including those
        # for the extension's restricted entitlements.
        command = [
            "xcodebuild", "-exportArchive", "-archivePath", str(archive_path),
            "-exportPath", str(export_path), "-exportOptionsPlist",
            str(args.export_options.resolve()),
        ]
        if args.provision:
            command.append("-allowProvisioningUpdates")
        subprocess.run(command, check=True, cwd=ROOT, env=environment)
        if (export_path / APP_NAME).is_dir():
            verify_app(export_path / APP_NAME, environment)


if __name__ == "__main__":
    main()
