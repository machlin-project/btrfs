#!/usr/bin/env python3
"""Cut the power of a disposable macOS guest while a native mount writes.

--adapter xnu (default) mounts with the loaded module's mount helper; --adapter
fskit lets Disk Arbitration mount the image through the installed FSKit module
(enabled, with its device barrier approved), runs root commands with a sudo
password read from a file (never logged) and needs automatic login in the guest.

The host keeps the authoritative image. Each iteration copies it into the
guest, mounts it read-write (grouped and synchronous commits alternate),
verifies every file acknowledged so far, then runs `btrfs-power-cut write`,
which prints a manifest line only after a file and its directory were synced.
After a random delay the host kills the virtual machine process, boots the
guest again into the btrfs kernel collection and copies the crashed image back.
The host records whether each crash left superblock copies needing recovery
(`btrfs-inspect recover`, which writes nothing) and mounts the crash image as
it is: the adapter's read-write mount must recover it. Finally the guest verifies
the last image, and with --linux the Linux reference kernel checks every crash
image: `btrfs check`, every fact acknowledged before that cut, and a Linux
read-write continuation. Images and logs stay in a new output directory inside
the shared product directory; the source fixture never changes.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import signal
import subprocess
import threading
import time

BOOT_SECONDS = 600
FIRST_ACK_SECONDS = 180
MENU_TIMEOUT = 3


def digest(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def exclusive_copy(source, target):
    with open(source, "rb") as src, open(target, "xb") as dst:
        shutil.copyfileobj(src, dst, 16 << 20)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path, required=True)
    parser.add_argument("--vm", required=True)
    parser.add_argument("--products", type=Path, required=True)
    parser.add_argument("--share", required=True)
    parser.add_argument("--guest-directory", required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--workload", type=Path, required=True)
    parser.add_argument("--adapter", choices=("xnu", "fskit"), default="xnu")
    parser.add_argument("--mount-helper", type=Path)
    parser.add_argument("--inspect", type=Path, required=True)
    parser.add_argument("--kernel-uuid")
    parser.add_argument("--module-uuid")
    parser.add_argument("--sudo-password-file", type=Path)
    parser.add_argument("--app", default="/Applications/Machlin btrfs.app")
    parser.add_argument("--iterations", type=int, default=6)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--min-seconds", type=float, default=2.0)
    parser.add_argument("--max-seconds", type=float, default=15.0)
    parser.add_argument("--linux", action="store_true",
                        help="Check every crash image with the lab's Linux reference kernel")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    lab = args.lab.resolve()
    products = args.products.resolve()
    output = args.output.resolve()
    if output.parent != products or output.exists():
        parser.error("--output must be a new directory directly inside --products")
    if not 0 < args.iterations < 100 or not 0 < args.min_seconds <= args.max_seconds:
        parser.error("Invalid iteration or delay bounds")
    xnu = args.adapter == "xnu"
    if xnu and not (args.mount_helper and args.kernel_uuid and args.module_uuid):
        parser.error("--adapter xnu needs --mount-helper, --kernel-uuid and --module-uuid")
    if not xnu and args.sudo_password_file is None:
        parser.error("--adapter fskit needs --sudo-password-file")
    output.mkdir()
    tart = str(lab / "scripts/tart.sh")
    share = f"/Volumes/My Shared Files/{args.share}/{output.name}"
    guest_dir = args.guest_directory.rstrip("/") + "/" + output.name
    mountpoint = guest_dir + "/mount"
    mounted = {"path": mountpoint, "device": None}
    record = {"vm": args.vm, "adapter": args.adapter, "image": str(args.image),
              "image_sha256": digest(args.image), "seed": args.seed, "iterations": [],
              "commands": []}
    password = args.sudo_password_file.read_bytes() if args.sudo_password_file else None
    log = (output / "runner.log").open("a")

    def save():
        (output / "report.json").write_text(json.dumps(record, indent=2) + "\n")

    def guest(*command, timeout=120, check=True, stdout=None):
        result = subprocess.run([tart, "exec", args.vm, *command], cwd=lab, timeout=timeout,
                                stdout=stdout if stdout is not None else subprocess.PIPE,
                                stderr=subprocess.PIPE)
        entry = {"command": list(command), "status": result.returncode,
                 "stderr": result.stderr.decode(errors="backslashreplace")[-2000:]}
        if stdout is None:
            entry["stdout"] = result.stdout.decode(errors="backslashreplace")[-2000:]
        record["commands"].append(entry)
        if check and result.returncode != 0:
            raise RuntimeError(f"Guest command failed: {command!r}: {entry['stderr']}")
        return result.stdout if stdout is None else b""

    def root_argv(*command):
        if password is None:
            return [tart, "exec", args.vm, "/usr/bin/sudo", "-n", *command]
        return [tart, "exec", "-i", args.vm, "/usr/bin/sudo", "-S", "-p", "", "--", *command]

    def root(*command, timeout=120):
        result = subprocess.run(root_argv(*command), cwd=lab, timeout=timeout, input=password,
                                capture_output=True)
        entry = {"command": ["sudo", *command], "status": result.returncode,
                 "stdout": result.stdout.decode(errors="backslashreplace")[-2000:],
                 "stderr": result.stderr.decode(errors="backslashreplace")[-2000:]}
        record["commands"].append(entry)
        if result.returncode != 0:
            raise RuntimeError(f"Guest root command failed: {command!r}: {entry['stderr']}")
        return entry

    def bootctl(*command):
        return subprocess.run(["python3", str(lab / "scripts/bootctl.py"), "--vm", args.vm,
                               *command], cwd=lab, check=True, capture_output=True, text=True,
                              timeout=300).stdout

    def start_vm(index):
        vm_log = (output / f"vm-{index:02d}.log").open("xb")
        shares = [f"--dir=lxnu-artifacts:{lab / 'artifacts'}:ro",
                  f"--dir=lxnu-kdk:{lab / '.cache/kdk'}:ro"] if xnu else []
        return subprocess.Popen(
            [tart, "run", args.vm, "--no-audio", "--no-clipboard", "--vnc-experimental", *shares,
             f"--dir={args.share}:{products}"], cwd=lab, stdout=vm_log, stderr=subprocess.STDOUT)

    def wait_boot():
        deadline = time.monotonic() + BOOT_SECONDS
        while time.monotonic() < deadline:
            try:
                session = guest("/usr/sbin/sysctl", "-n", "kern.bootsessionuuid", timeout=10)
                break
            except (subprocess.SubprocessError, RuntimeError, OSError):
                time.sleep(3)
        else:
            raise RuntimeError("The guest did not boot")
        if not xnu:
            record.setdefault("kernels", []).append(guest("/usr/bin/uname", "-v").decode().strip())
            control = f"{args.app}/Contents/MacOS/{Path(args.app).stem}"
            modules = json.loads(guest(control, "--control", "modules", timeout=60))
            service = json.loads(guest(control, "--control", "device-service", timeout=60))
            if not modules or not all(module["enabled"] for module in modules) or \
                    service.get("status") != "enabled":
                raise RuntimeError(f"FSKit module or device barrier unavailable: {modules} {service}")
            return session.decode().strip()
        kernel = guest("/usr/sbin/sysctl", "-n", "kern.uuid").decode().strip()
        if kernel != args.kernel_uuid:
            raise RuntimeError(f"Unexpected kernel {kernel}")
        guest("/usr/bin/sudo", "-n", "/usr/bin/kmutil", "load", "-b", "org.machlin.btrfs.kext",
              check=False)
        loaded = guest("/usr/bin/kmutil", "showloaded", "--list-only").decode()
        if not any("org.machlin.btrfs.kext" in line and args.module_uuid in line
                   for line in loaded.splitlines()):
            raise RuntimeError("The expected btrfs module is not loaded (fallback slot?)")
        return session.decode().strip()

    def mount_image(name, local, flags):
        guest("/bin/cp", f"{share}/{name}", f"{guest_dir}/{local}", timeout=600)
        if guest("/usr/bin/shasum", "-a", "256", f"{guest_dir}/{local}",
                 timeout=600).decode().split()[0] != digest(output / name):
            raise RuntimeError("Guest image copy differs")
        attach = ["/usr/bin/hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage"]
        attach += ["-nomount"] if xnu else ["-owners", "on"]
        device = guest(*attach, f"{guest_dir}/{local}").decode().split()[0]
        if not device.startswith("/dev/disk"):
            raise RuntimeError(f"Unexpected device {device}")
        mounted["device"] = device
        if xnu:
            root(f"{guest_dir}/{args.mount_helper.name}", *flags, device, mountpoint)
            return device
        # Disk Arbitration mounts through the FSKit module; writable needs the barrier.
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            for line in guest("/sbin/mount").decode().splitlines():
                if line.startswith(device + " on ") and "machlinbtrfs" in line:
                    if "read-only" in line:
                        raise RuntimeError(f"FSKit mounted read-only: {line}")
                    mounted["path"] = line[len(device) + 4:line.rindex(" (")]
                    return device
            time.sleep(1)
        raise RuntimeError("Disk Arbitration did not mount the image through FSKit")

    def unmount():
        if xnu:
            root("/sbin/umount", mounted["path"])
        else:
            guest("/usr/bin/hdiutil", "detach", mounted["device"])

    def verify(manifest):
        return root(f"{guest_dir}/{args.workload.name}", "verify", mounted["path"],
                    f"{share}/{manifest}", timeout=900)["stderr"].strip()

    # Unattended boots take the btrfs slot; the confirmed fallback slot stays.
    if xnu:
        status = json.loads(bootctl("status"))
        record["menu_timeout_before"] = status["state"]["timeout_seconds"]
        record["default_slot_before"] = status["state"]["default_slot"]
        btrfs_slot = next(i for i, slot in enumerate(status["state"]["slots"])
                          if slot["label"] == "Btrfs")
        bootctl("select", str(btrfs_slot), "--timeout", str(MENU_TIMEOUT))
    subprocess.run([tart, "stop", args.vm], cwd=lab, timeout=300)
    vm = start_vm(0)
    acks = []
    acks_lock = threading.Lock()
    try:
        record["sessions"] = [wait_boot()]
        guest("/bin/mkdir", "-p", mountpoint)
        for binary in (args.workload, args.mount_helper) if xnu else (args.workload,):
            shutil.copy2(binary, output / binary.name)
            guest("/bin/cp", f"{share}/{binary.name}", f"{guest_dir}/{binary.name}")
            guest("/bin/chmod", "755", f"{guest_dir}/{binary.name}")
            if guest("/usr/bin/shasum", "-a", "256",
                     f"{guest_dir}/{binary.name}").decode().split()[0] != digest(binary):
                raise RuntimeError(f"Guest copy of {binary.name} differs")
        current = "image-00.raw"
        exclusive_copy(args.image, output / current)
        for index in range(1, args.iterations + 1):
            mode = "grouped" if index % 2 or not xnu else "synchronous"
            flags = ["-w"] if mode == "grouped" else ["-w", "-s"]
            item = {"index": index, "mode": mode, "image": current, "acked_before": len(acks)}
            record["iterations"].append(item)
            mount_image(current, f"run-{index:02d}.raw", flags)
            if acks:
                item["verified_before"] = verify(f"acks-{index - 1:02d}.tsv")
            stream = subprocess.Popen(
                root_argv(f"{guest_dir}/{args.workload.name}", "write", mounted["path"],
                          str(index)),
                cwd=lab, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=(output / f"workload-{index:02d}.log").open("xb"))
            if password is not None:
                stream.stdin.write(password)
            stream.stdin.close()
            started = threading.Event()

            def read_acks():
                with (output / "acks.tsv").open("ab") as sink:
                    for line in stream.stdout:
                        if line.startswith(b"file\t") and line.endswith(b"\n"):
                            with acks_lock:
                                acks.append(line)
                                sink.write(line)
                                sink.flush()
                                os.fsync(sink.fileno())
                            started.set()

            reader = threading.Thread(target=read_acks)
            reader.start()
            if not started.wait(FIRST_ACK_SECONDS):
                raise RuntimeError("The workload acknowledged nothing")
            delay = random.Random(args.seed * 1000 + index).uniform(args.min_seconds,
                                                                     args.max_seconds)
            time.sleep(delay)
            # The cut: the virtual machine process dies with the guest in it.
            vm.send_signal(signal.SIGKILL)
            vm.wait()
            stream.wait(timeout=120)
            reader.join(timeout=120)
            with acks_lock:
                item.update(delay=round(delay, 3), acked_after=len(acks))
                (output / f"acks-{index:02d}.tsv").write_bytes(b"".join(acks))
            print(f"cut {index} ({mode}) after {delay:.1f}s: "
                  f"{item['acked_after'] - item['acked_before']} new acknowledgements", flush=True)
            subprocess.run([tart, "stop", args.vm], cwd=lab, timeout=300,
                           stdout=log, stderr=subprocess.STDOUT)
            vm = start_vm(index)
            record["sessions"].append(wait_boot())
            crash = f"crash-{index:02d}.raw"
            with (output / crash).open("xb") as sink:
                guest("/bin/cat", f"{guest_dir}/run-{index:02d}.raw", stdout=sink, timeout=1800)
            item["crash"] = crash
            item["crash_sha256"] = digest(output / crash)
            probe = subprocess.run([str(args.inspect), str(output / crash), "recover"],
                                   capture_output=True, text=True)
            item["recovery_check"] = probe.stdout.strip()
            if probe.returncode not in (0, 3):
                raise RuntimeError(f"Recovery check failed: {probe.stdout}{probe.stderr}")
            item["needs_recovery"] = probe.returncode == 3
            current = crash
            save()
        mount_image(current, "final.raw", ["-w"])
        record["final"] = verify(f"acks-{args.iterations:02d}.tsv")
        unmount()
        print(f"final verify: {record['final']}", flush=True)
        if args.linux:
            record["linux"] = linux_checks(args, lab, output, record)
        record["result"] = "PASS" if all(item.get("linux", "PASS") == "PASS"
                                         for item in record["iterations"]) else "FAIL"
    finally:
        save()
        try:
            if xnu:
                bootctl("select", str(record["default_slot_before"]), "--timeout",
                        str(record["menu_timeout_before"]))
        except (subprocess.SubprocessError, OSError) as error:
            print(f"Restore the boot menu manually: {error}", flush=True)
    print(f"{record['result']} power cut: {args.iterations} cuts, {len(acks)} acknowledged files",
          flush=True)


def linux_checks(args, lab, output, record):
    results = []
    for item in record["iterations"]:
        name = output.name + "-" + item["crash"].removesuffix(".raw")
        archive = lab / f"artifacts/btrfs-reference/native-check-{name}.cpio"
        subprocess.run(["python3", str(Path(__file__).with_name("prepare_native_linux.py")),
                        "--root", str(lab / "artifacts/btrfs-reference/root-native"),
                        "--manifest", str(output / f"acks-{item['index']:02d}.tsv"),
                        "--archive", str(archive), "--device-bytes",
                        str((output / item["crash"]).stat().st_size)],
                       cwd=lab, check=True, capture_output=True)
        work = output / f"linux-{item['crash']}"
        exclusive_copy(output / item["crash"], work)
        log = output / f"linux-{item['index']:02d}.log"
        with log.open("xb") as stream:
            subprocess.run([str(lab / ".cache/linux-reference/linux-vm"),
                            str(lab / ".cache/linux-reference/Image"), str(archive), "2", "512",
                            "console=hvc0 rdinit=/init panic=-1 loglevel=4", str(work)],
                           cwd=lab, stdout=stream, stderr=subprocess.STDOUT, timeout=3600)
        text = log.read_text(errors="replace")
        item["linux"] = "PASS" if "BTRFS_NATIVE_PASS" in text else "FAIL"
        results.append({"crash": item["crash"], "result": item["linux"]})
        print(f"linux {item['crash']}: {item['linux']}", flush=True)
    return results


if __name__ == "__main__":
    main()
