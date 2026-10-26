#!/usr/bin/env python3
"""Run/validate the opt-in image-upload benchmark and preserve raw samples and summary statistics."""
from __future__ import annotations
import argparse
import collections
import csv
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess

METHODS = ("host_current", "host_native", "staging_shared_usage", "staging")
TIMES = ("allocation_us", "prepare_us", "host_copy_us", "submit_us", "wait_us", "upload_us", "total_us")
KEYS = ("case", "format", "side", "layers", "mips", "payload_bytes", "mode", "method")


def percentile95(values):
    return sorted(values)[math.ceil(len(values) * .95) - 1]


def read_samples(path, samples, validation):
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 24 * 2 * 4 * samples:
        raise RuntimeError(f"{path}: expected {24 * 2 * 4 * samples} rows, got {len(rows)}")
    seen = set()
    for row in rows:
        key = tuple(row[k] for k in KEYS)
        identity = (*key, int(row["sample"]))
        if identity in seen:
            raise RuntimeError(f"{path}: duplicate {identity}")
        seen.add(identity)
        if not 0 <= int(row["sample"]) < samples or row["method"] not in METHODS:
            raise RuntimeError(f"{path}: invalid sample/method")
        for metric in TIMES:
            value = float(row[metric])
            if not math.isfinite(value) or value < 0:
                raise RuntimeError(f"{path}: invalid {metric}")
        expected_submits = 2 if row["method"] == "host_current" else 0 if row["method"] == "host_native" else 1
        if int(row["submits"]) != expected_submits:
            raise RuntimeError(f"{path}: incorrect submission count")
        if int(row["sample"]) == samples - 1 and row["validated"] != "1":
            raise RuntimeError(f"{path}: final sample missing byte validation")
        if float(row["total_us"]) + 1 < float(row["allocation_us"]) + float(row["upload_us"]):
            raise RuntimeError(f"{path}: total does not include allocation and upload")
        row["validation_layers"] = str(int(validation))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=21)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()
    if not args.exe.is_file() or not all(1 <= n <= 1000 for n in (args.samples, args.warmups, args.rounds)):
        parser.error("executable must exist and all counts must be 1..1000")
    executable = args.exe.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["PATH"] = str(executable.parent) + os.pathsep + "C:/msys64/clang64/bin" + os.pathsep + env.get("PATH", "")
    all_rows, runs = [], []
    for index in range(args.rounds + 1):
        validation = index == 0
        directory = out / ("validation" if validation else f"round-{index}")
        directory.mkdir(exist_ok=False)
        samples, warmups = (2, 2) if validation else (args.samples, args.warmups)
        command = [str(executable), "--out", "raw.csv", "--samples", str(samples), "--warmups", str(warmups)]
        if validation:
            command.append("--validation")
        print("Running", directory.name, flush=True)
        with (directory / "run.log").open("wb") as log:
            result = subprocess.run(command, cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=300)
        if result.returncode:
            raise RuntimeError(f"{directory}: exit {result.returncode}; inspect run.log and deren-crash.log")
        diagnostic = directory / "deren-debug.log"
        if not diagnostic.is_file():
            raise RuntimeError(f"{directory}: missing backend diagnostic log")
        text = diagnostic.read_text(encoding="utf-8", errors="replace")
        if re.search(r"VUID-|Validation Error|SYNC-HAZARD|\[ERROR\]", text):
            raise RuntimeError(f"{directory}: validation/backend errors")
        run_text = (directory / "run.log").read_text()
        match = re.search(r"verified=(\d+) skipped_iterations=0 cases=24", run_text)
        if not match or int(match[1]) != 24 * 2 * 4 * 2:
            raise RuntimeError(f"{directory}: incomplete byte validation")
        rows = read_samples(directory / "raw.csv", samples, validation)
        runs.append({"directory": str(directory), "validation": validation, "rows": len(rows),
                     "verified": int(match[1]), "device": (directory / "raw.csv.device.txt").read_text()})
        if not validation:
            for row in rows:
                row["round"] = str(index)
                all_rows.append(row)
    grouped = collections.defaultdict(list)
    for row in all_rows:
        grouped[tuple(row[k] for k in KEYS)].append(row)
    summary = []
    for key, group in sorted(grouped.items(), key=lambda item: (item[0][0], int(item[0][2]), item[0][6], item[0][7])):
        if len(group) != args.samples * args.rounds:
            raise RuntimeError("unequal samples per configuration")
        item = dict(zip(KEYS, key))
        item["samples"] = len(group)
        for metric in TIMES:
            values = [float(row[metric]) for row in group]
            item[metric + "_median"] = statistics.median(values)
            item[metric + "_p95"] = percentile95(values)
        for field in ("image_allocation_bytes", "staging_capacity_bytes", "memory_type", "optimal_device_access", "identical_layout"):
            item[field] = sorted({int(row[field]) for row in group})
        summary.append(item)
    with (out / "raw.csv.gz").open("wb") as compressed:
        with gzip.open(compressed, "wt", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(all_rows[0]))
            writer.writeheader()
            writer.writerows(all_rows)
    with (out / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)
    document = {"executable": str(executable), "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                "counts": {"cases": 24, "methods": 4, "modes": 2, "samples": args.samples, "rounds": args.rounds,
                           "timed_rows": len(all_rows)}, "runs": runs, "summary": summary}
    (out / "summary.json").write_text(json.dumps(document, indent=2), encoding="utf-8")
    print("Saved", out / "summary.json")
    for side in (16, 64, 256, 1024, 2048, 4096):
        selected = {row["method"]: row for row in summary if row["case"] == "rgba8" and int(row["side"]) == side and row["mode"] == "warm"}
        print(f"RGBA8 {side}: " + ", ".join(f"{method}={selected[method]['upload_us_median']:.2f}us" for method in METHODS))


if __name__ == "__main__":
    main()
