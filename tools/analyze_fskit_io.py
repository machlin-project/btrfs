#!/usr/bin/env python3
"""Summarize a diagnostic FSKit I/O log inside one fsync-bench time window.

Input is `log stream --style ndjson` (plain log text also works). Both processes
and the benchmark use the guest's CLOCK_MONOTONIC. Run on one mounted test volume
without competing workloads. This is attribution, not an uninstrumented speed
measurement: synchronous logging adds overhead, especially to the XPC residual.
"""

import argparse
import json
import math
from pathlib import Path
import re
import statistics


MAX_EVENTS = 100000
SUPERBLOCK_BYTES = 4096
SUPERBLOCK_OFFSETS = (64 * 1024, 64 * 1024 * 1024, 256 * 1024 * 1024 * 1024)
EVENT = re.compile(
    r"btrfs-io operation=(\w+) start_ns=(\d+) duration_ns=(\d+) "
    r"offset=(\d+) bytes=(\d+) result=(\d+)"
)
KINDS = ("resource_read", "resource_write", "staged_drain", "barrier_xpc",
         "barrier_ioctl", "volume_sync")
PHASES = ("create", "write", "file_fsync", "close", "directory_fsync")


def union_ns(intervals):
    total = 0
    previous_end = 0
    for start, end in sorted(intervals):
        total += max(0, end - max(start, previous_end))
        previous_end = max(previous_end, end)
    return total


def inside(event, start, end):
    return start <= event["start_ns"] and event["end_ns"] <= end


def coverage(events, start, end):
    return union_ns((max(start, event["start_ns"]), min(end, event["end_ns"]))
                    for event in events
                    if event["end_ns"] > start and event["start_ns"] < end)


def distribution(events):
    values = sorted(event["duration_ns"] for event in events)
    if not values:
        return {"calls": 0, "bytes": 0, "failures": 0, "sum_ns": 0, "wall_coverage_ns": 0}
    return {
        "calls": len(values), "bytes": sum(event["bytes"] for event in events),
        "failures": sum(event["result"] != 0 for event in events),
        "sum_ns": sum(values), "mean_us": statistics.mean(values) / 1000,
        "wall_coverage_ns": union_ns((event["start_ns"], event["end_ns"]) for event in events),
        **{f"p{percent}_us": values[math.ceil(len(values) * percent / 100) - 1] / 1000
           for percent in (50, 95, 99)},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--benchmark", type=Path, required=True,
                        help="btrfs-fsync-bench JSONL, preferably with --samples")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    rows = [json.loads(line) for line in args.benchmark.read_text().splitlines() if line.strip()]
    summaries = [row for row in rows if "workload" in row]
    if len(summaries) != 1 or summaries[0].get("workload") != "create-write-fsync-file-and-directory":
        parser.error("benchmark must contain exactly one successful workload summary")
    benchmark = summaries[0]
    start, end = benchmark["start_ns"], benchmark["end_ns"]
    if start <= 0 or end <= start or benchmark["verified_files"] != benchmark["files"]:
        parser.error("benchmark time window or verification is invalid")
    events = []
    seen = set()
    with args.trace.open() as stream:
        for line in stream:
            if line.startswith("Filtering the log data using "):
                continue
            if "btrfs-io " not in line:
                continue
            match = EVENT.search(line)
            if match is None:
                parser.error("malformed or redacted btrfs-io record")
            kind = match[1]
            values = tuple(map(int, match.groups()[1:]))
            if kind not in KINDS or values[0] == 0:
                parser.error("unknown event or unavailable monotonic clock")
            identity = (kind, *values)
            if identity in seen:
                parser.error("duplicate I/O record; use one log capture")
            event = dict(zip(("start_ns", "duration_ns", "offset", "bytes", "result"), values))
            event["end_ns"] = event["start_ns"] + event["duration_ns"]
            if not inside(event, start, end):
                continue
            seen.add(identity)
            event["operation"] = kind
            event["superblock"] = any(
                offset <= event["offset"] < offset + SUPERBLOCK_BYTES
                and event["offset"] + event["bytes"] <= offset + SUPERBLOCK_BYTES
                for offset in SUPERBLOCK_OFFSETS
            )
            events.append(event)
            if len(events) > MAX_EVENTS:
                parser.error("event limit exceeded; use a shorter measurement window")
    if not events:
        parser.error("no timed I/O in benchmark window; check build flag and guest clock")
    by_kind = {kind: [event for event in events if event["operation"] == kind] for kind in KINDS}
    stats = {kind: distribution(items) for kind, items in by_kind.items()}
    for kind in ("resource_read", "resource_write"):
        for superblock in (False, True):
            label = "superblock" if superblock else "other"
            stats[f"{kind}_{label}"] = distribution(
                [event for event in by_kind[kind] if event["superblock"] == superblock]
            )
    leaf_io = by_kind["resource_read"] + by_kind["resource_write"] + by_kind["barrier_xpc"]
    sync_residual = sum(event["duration_ns"] - coverage(leaf_io, event["start_ns"], event["end_ns"])
                        for event in by_kind["volume_sync"])
    xpc_residual = sum(event["duration_ns"] - coverage(by_kind["barrier_ioctl"],
                                                    event["start_ns"], event["end_ns"])
                       for event in by_kind["barrier_xpc"])
    per_call = []
    for row in rows:
        if "sample" not in row:
            continue
        position = row["start_ns"]
        for phase in PHASES:
            next_position = position + row["ns"][phase]
            if phase in ("file_fsync", "directory_fsync"):
                counts = {kind: sum(inside(event, position, next_position) for event in by_kind[kind])
                          for kind in ("volume_sync", "barrier_xpc", "barrier_ioctl")}
                per_call.append({"sample": row["sample"], "operation": phase, **counts})
            position = next_position
    result = {
        "benchmark": benchmark, "trace": str(args.trace), "events": len(events),
        "timing": stats, "per_fsync_calls": per_call,
        "sync_unattributed_ns": sync_residual, "xpc_outside_ioctl_ns": xpc_residual,
        "limitations": [
            "Diagnostic logging perturbs timing; do not publish this as uninstrumented performance.",
            "Intervals are nested: resource writes are inside drains; ioctls are inside XPC barriers.",
            "Concurrent calls overlap: sum_ns is accumulated duration, wall_coverage_ns is their union.",
            "Unattributed sync time includes CPU, lock waits and logging, not CPU time alone.",
            "Missing daemon records make XPC attribution incomplete; inspect ioctl counts.",
            "Live contents verification does not establish power-cut durability.",
        ],
    }
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
