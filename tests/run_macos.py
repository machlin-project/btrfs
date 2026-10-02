#!/usr/bin/env python3
"""Run mounted contracts in an already prepared, identified disposable guest.

Read-only images are attached read-only and must stay byte-identical. A write
image is copied, attached writable, mounted read-write for the write contracts,
mounted again for the persistence check, and copied back for the Linux check;
the source fixture never changes."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess

MODULE = "org.machlin.btrfs.kext"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path, required=True)
    parser.add_argument("--vm", required=True)
    parser.add_argument("--products", type=Path, required=True)
    parser.add_argument("--guest-directory", required=True)
    parser.add_argument("--share", required=True)
    parser.add_argument("--expected-session", required=True)
    parser.add_argument("--expected-module-uuid", required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--mount-helper", type=Path, required=True)
    parser.add_argument("--image", type=Path, action="append", default=[])
    parser.add_argument("--write-image", type=Path, action="append", default=[])
    parser.add_argument("--write-probe", type=Path)
    parser.add_argument("--write-mode", dest="write_modes", action="append",
                        choices=["grouped", "synchronous"],
                        help="Commit modes for each --write-image (default: both)")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.image and not args.write_image:
        parser.error("name at least one --image or --write-image")
    if not args.write_modes:
        args.write_modes = ["grouped", "synchronous"]
    if args.write_image and args.write_probe is None:
        parser.error("--write-image needs --write-probe")
    lab = args.lab.resolve()
    products = args.products.resolve()
    output = args.output.resolve()
    if Path.cwd().resolve() != lab:
        parser.error("run from the explicit Machlin lab directory")
    relative = output.relative_to(products)
    output.mkdir(parents=True, exist_ok=False)
    identity = json.loads((products / "kc-identity.json").read_text())
    if (digest(products / identity["kc"]) != identity["sha256"] or
            digest(products / "MachlinBtrfs.kext/Contents/MacOS/machlin_btrfs") !=
            identity["btrfs_executable_sha256"]):
        raise RuntimeError("Prepared kernel collection or module changed")
    record = {"vm": args.vm, "cases": [], "commands": [], "result": "FAIL",
              "kc_sha256": identity["sha256"]}
    guest_dir = args.guest_directory + "/" + output.name
    share = f"/Volumes/My Shared Files/{args.share}/{relative}"

    def save():
        (output / "report.json").write_text(json.dumps(record, indent=2) + "\n")

    def guest(*command, timeout=120):
        result = subprocess.run([str(lab / "scripts/tart.sh"), "exec", args.vm, *command],
                                cwd=lab, capture_output=True, timeout=timeout)
        record["commands"].append({"command": list(command), "status": result.returncode,
                                   "stdout": result.stdout.decode(errors="backslashreplace"),
                                   "stderr": result.stderr.decode(errors="backslashreplace")})
        save()
        if result.returncode:
            raise RuntimeError(f"Guest command failed: {command!r}: "
                               + result.stderr.decode(errors="backslashreplace"))
        return result.stdout

    def check_identity():
        kernel = guest("/usr/sbin/sysctl", "-n", "kern.uuid").decode().strip()
        session = guest("/usr/sbin/sysctl", "-n", "kern.bootsessionuuid").decode().strip()
        module = guest("/usr/sbin/kextstat", "-l", "-b", MODULE).decode()
        if kernel.upper() not in [value.upper() for value in identity["kernel_uuids"]]:
            raise RuntimeError("Unexpected loaded kernel")
        if session != args.expected_session or args.expected_module_uuid.upper() not in module.upper():
            raise RuntimeError("Unexpected boot session or module")
        record.update(kernel_uuid=kernel, boot_session=session, loaded_module=module)

    mountpoint = guest_dir + "/mount"

    def write_case(source, mode):
        """mode: "grouped" (operations share a running transaction) or
        "synchronous" (mount -s: each namespace operation and each write
        commits before it returns)."""
        expected = digest(source)
        copy = output / source.name
        if not copy.exists():
            shutil.copyfile(source, copy)
        guest_image = guest_dir + "/" + mode + "-" + source.name
        flags = ["-w", "-s"] if mode == "synchronous" else ["-w"]
        guest("/bin/cp", share + "/" + source.name, guest_image)
        attached = plistlib.loads(guest("/usr/bin/hdiutil", "attach", "-nomount", "-imagekey",
                                        "diskimage-class=CRawDiskImage", "-plist", guest_image))
        devices = [entry["dev-entry"] for entry in attached["system-entities"] if "dev-entry" in entry]
        if len(devices) != 1 or not re.fullmatch(r"/dev/disk[0-9]+", devices[0]):
            raise RuntimeError(f"Unexpected attached raw device: {devices}")
        device = devices[0]
        item = {"image": str(source), "sha256": expected, "device": device, "mode": "write",
                "commit": mode, "result": "FAIL"}
        record["cases"].append(item)
        probe = guest_dir + "/" + args.write_probe.name
        helper = guest_dir + "/" + args.mount_helper.name
        mounted = False
        try:
            info = plistlib.loads(guest("/usr/sbin/diskutil", "info", "-plist", device))
            if (not info["WholeDisk"] or info["TotalSize"] != source.stat().st_size or
                    info.get("MountPoint") or not info["WritableMedia"]):
                raise RuntimeError("Device is not the expected writable image copy")
            raw = "/dev/r" + device.removeprefix("/dev/")
            if guest("/usr/bin/sudo", "-n", "/usr/bin/shasum", "-a", "256", raw).decode().split()[0] != expected:
                raise RuntimeError("Guest raw device differs from source")
            guest("/usr/bin/sudo", "-n", helper, *flags, device, mountpoint)
            mounted = True
            item["write"] = guest("/usr/bin/sudo", "-n", probe, "write", mountpoint,
                                  timeout=3600).decode()
            guest("/usr/bin/sudo", "-n", "/sbin/umount", mountpoint)
            mounted = False
            guest("/usr/bin/sudo", "-n", helper, *flags, device, mountpoint)
            mounted = True
            manifest = guest("/usr/bin/sudo", "-n", probe, "verify", mountpoint, timeout=600)
            (output / (source.stem + "-" + mode + ".manifest.tsv")).write_bytes(manifest)
            guest("/usr/bin/sudo", "-n", "/sbin/umount", mountpoint)
            mounted = False
        finally:
            if mounted:
                guest("/usr/bin/sudo", "-n", "/sbin/umount", mountpoint)
            guest("/usr/bin/hdiutil", "detach", device)
        written = output / (source.stem + "-" + mode + "-written.raw")
        guest("/bin/cp", guest_image, share + "/" + written.name, timeout=600)
        item["written_sha256"] = digest(written)
        if digest(source) != expected:
            raise RuntimeError("The write case changed its source fixture")
        item["result"] = "PASS"
        save()
        print(f"PASS {source.name} ({mode}): write contracts, remount persistence; "
              f"image {written}", flush=True)

    try:
        check_identity()
        guest("/bin/mkdir", "-p", mountpoint)
        binaries = [args.probe.resolve(), args.mount_helper.resolve()]
        if args.write_probe is not None:
            binaries.append(args.write_probe.resolve())
        for source in binaries:
            target = output / source.name
            shutil.copyfile(source, target)
            target.chmod(0o755)
            guest("/bin/cp", share + "/" + source.name, guest_dir + "/" + source.name)
            actual = guest("/usr/bin/shasum", "-a", "256", guest_dir + "/" + source.name).decode().split()[0]
            if actual != digest(source):
                raise RuntimeError("Native binary changed in transit")
            record[source.name + "_sha256"] = actual
        for source in args.image:
            source = source.resolve()
            expected = digest(source)
            target = output / source.name
            shutil.copyfile(source, target)
            guest_image = guest_dir + "/" + source.name
            guest("/bin/cp", share + "/" + source.name, guest_image)
            attached = plistlib.loads(guest("/usr/bin/hdiutil", "attach", "-nomount", "-readonly",
                                            "-imagekey", "diskimage-class=CRawDiskImage", "-plist", guest_image))
            devices = [entry["dev-entry"] for entry in attached["system-entities"] if "dev-entry" in entry]
            if len(devices) != 1 or not re.fullmatch(r"/dev/disk[0-9]+", devices[0]):
                raise RuntimeError(f"Unexpected attached raw device: {devices}")
            device = devices[0]
            item = {"image": str(source), "sha256": expected, "device": device, "result": "FAIL"}
            record["cases"].append(item)
            mounted = False
            try:
                info = plistlib.loads(guest("/usr/sbin/diskutil", "info", "-plist", device))
                if (not info["WholeDisk"] or info["TotalSize"] != source.stat().st_size or
                        info.get("MountPoint") or info["WritableMedia"]):
                    raise RuntimeError("Device is not the expected read-only image")
                raw = "/dev/r" + device.removeprefix("/dev/")
                if guest("/usr/bin/sudo", "-n", "/usr/bin/shasum", "-a", "256", raw).decode().split()[0] != expected:
                    raise RuntimeError("Guest raw device differs from source")
                guest("/usr/bin/sudo", "-n", guest_dir + "/" + args.mount_helper.name, device, mountpoint)
                mounted = True
                item["probe"] = guest("/usr/bin/sudo", "-n", guest_dir + "/" + args.probe.name, mountpoint).decode()
                if "EROFS PASS" not in item["probe"]:
                    raise RuntimeError("Missing mounted contract success evidence")
                guest("/usr/bin/sudo", "-n", "/sbin/umount", mountpoint)
                mounted = False
                if guest("/usr/bin/sudo", "-n", "/usr/bin/shasum", "-a", "256", raw).decode().split()[0] != expected:
                    raise RuntimeError("Read-only operations changed the raw device")
            finally:
                if mounted:
                    guest("/usr/bin/sudo", "-n", "/sbin/umount", mountpoint)
                guest("/usr/bin/hdiutil", "detach", device)
            if digest(source) != expected or digest(target) != expected:
                raise RuntimeError("Native checks changed a fixture")
            item["result"] = "PASS"
            save()
            print(f"PASS {source.name}: mounted contracts and unchanged media", flush=True)
        for source in args.write_image:
            for mode in args.write_modes:
                write_case(source.resolve(), mode)
        check_identity()
        record["result"] = "PASS"
    except Exception as error:
        record["error"] = str(error)
        raise
    finally:
        record["checked_at"] = datetime.now(timezone.utc).isoformat()
        save()


if __name__ == "__main__":
    main()
