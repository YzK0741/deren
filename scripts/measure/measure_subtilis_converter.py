"""Measure the Subtilis converter on a deterministic asset matrix.

For every asset x --threads value x --rounds iteration it runs one `deren-subtilis build`
with identical (canonical) build options and records the report JSON, the external end-to-end
wall time, the sampled peak working set, the output file size/hash and every per-round value.
Medians are computed over ALL recorded rounds (never best-only) and written to
`measurements.json`; this script does not decide PASS/FAIL (verify_subtilis_converter.py does).

Per-stage `cpuMs` in the report is accumulated worker-task time (thread time, not CPU cycles);
it is recorded separately from the external and reported wall times and must not be added up to
pretend it is a parallel wall clock.

Usage:
  python measure_subtilis_converter.py --converter build-subtilis/deren-subtilis.exe \
      --out-dir <work-dir> --asset small=<small.glb> --asset large=<large.glb> \
      [--asset bistro=<bistro-merged.gltf> --dag-required large --dag-required bistro]
"""

import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path

STAGE_NAMES = [
    "sourceFingerprint",
    "leafCluster",
    "topology",
    "partition",
    "boundary",
    "simplify",
    "recluster",
    "candidate",
    "commit",
    "validate",
    "encode",
]

SCHEMA = "subtilis-converter-measurements/1"


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _windows_peak_sampler():
    if sys.platform != "win32":
        return None
    import ctypes
    from ctypes import wintypes

    class ProcessMemoryCounters(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessMemoryCounters), wintypes.DWORD]
    open_process = kernel32.OpenProcess
    query_limited_information = 0x1000

    def sample(pid):
        handle = open_process(query_limited_information, False, pid)
        if not handle:
            return None
        try:
            counters = ProcessMemoryCounters()
            counters.cb = ctypes.sizeof(counters)
            if psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                return int(counters.PeakWorkingSetSize)
            return None
        finally:
            kernel32.CloseHandle(handle)

    return sample


def run_measured(command, log_path, sample, timeout):
    """Run `command`, returning (exit code, external wall ms, sampled peak working set bytes)."""
    started = time.perf_counter()
    with open(log_path, "wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        peak = None
        try:
            while process.poll() is None:
                if sample is not None:
                    value = sample(process.pid)
                    if value is not None:
                        peak = value if peak is None else max(peak, value)
                if time.perf_counter() - started > timeout:
                    process.kill()
                    process.wait()
                    return (-1, (time.perf_counter() - started) * 1000.0, peak)
                time.sleep(0.02)
        finally:
            code = process.wait()
    return (code, (time.perf_counter() - started) * 1000.0, peak)


def load_report(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return {"_parse_error": str(error)}


def aggregate(report):
    """Compact structural aggregate of one build report (never drops a stage or a level)."""
    primitives = report.get("primitives", [])
    stages = {}
    for stage_name in STAGE_NAMES:
        cpu = 0.0
        count = 0
        seen = 0
        inclusive = None
        for primitive in primitives:
            for stage in primitive.get("stages", []):
                if stage.get("name") == stage_name:
                    seen += 1
                    cpu += float(stage.get("cpuMs", 0.0))
                    count += int(stage.get("count", 0))
                    inclusive = bool(stage.get("inclusive", False))
        stages[stage_name] = {"primitives_with_stage": seen, "cpu_ms": cpu, "count": count, "inclusive": inclusive}
    levels = []
    for primitive in primitives:
        primitive_levels = primitive.get("levels", [])
        while len(levels) < len(primitive_levels):
            levels.append({"triangles": 0, "clusters": 0, "groups": 0, "roots": 0})
        for index, level in enumerate(primitive_levels):
            for key in ("triangles", "clusters", "groups", "roots"):
                levels[index][key] += int(level.get(key, 0))
    rejected = {}
    known_work_peak_bytes_max = 0
    known_work_limit_bytes_min = None
    cross_group_repartitions = 0
    rounds = 0
    accumulated_ms = 0.0
    wall_ms = 0.0
    groups = 0
    for primitive in primitives:
        cross_group_repartitions += int(primitive.get("crossGroupRepartitions", 0))
        rounds = max(rounds, int(primitive.get("rounds", 0)))
        accumulated_ms += float(primitive.get("accumulatedMs", 0.0))
        wall_ms += float(primitive.get("wallMs", 0.0))
        known_work_peak_bytes_max = max(known_work_peak_bytes_max, int(primitive.get("knownWorkPeakBytes", 0)))
        limit = int(primitive.get("knownWorkLimitBytes", 0))
        known_work_limit_bytes_min = limit if known_work_limit_bytes_min is None else min(known_work_limit_bytes_min, limit)
        level_groups = primitive.get("levels", [])
        if level_groups:
            groups += int(level_groups[-1].get("groups", 0))
        for name, value in primitive.get("rejected", {}).items():
            if name != "none":
                rejected[name] = rejected.get(name, 0) + int(value)
    return {
        "converted": int(report.get("converted", 0)),
        "reused": int(report.get("reused", 0)),
        "skipped": int(report.get("skipped", 0)),
        "lod0Only": int(report.get("lod0Only", 0)),
        "threads": int(report.get("threads", 0)),
        "primitives": len(primitives),
        "rounds_max": rounds,
        "cross_group_repartitions": cross_group_repartitions,
        "groups_final": groups,
        "rejected": rejected,
        "accumulated_ms": accumulated_ms,
        "wall_ms": wall_ms,
        "known_work_peak_bytes_max": known_work_peak_bytes_max,
        "known_work_limit_bytes_min": known_work_limit_bytes_min if known_work_limit_bytes_min is not None else 0,
        "stages": stages,
        "levels": levels,
    }



def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--converter", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--asset", action="append", required=True, metavar="NAME=PATH",
                        help="measurement asset (repeat for the small fixture, synthetic large grid and real Bistro)")
    parser.add_argument("--dag-required", action="append", default=[], metavar="NAME",
                        help="asset that must show a real cross-group DAG before the gate may PASS")
    parser.add_argument("--threads", default="1,12")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=3600.0, help="per-run timeout in seconds")
    parser.add_argument("--no-memory", action="store_true", help="skip the sampled peak-working-set measurement")
    args = parser.parse_args()
    threads = [int(item) for item in args.threads.split(",") if item.strip()]
    assets = []
    for spec in args.asset:
        name, _, path = spec.partition("=")
        if not name or not path:
            parser.error(f"--asset needs NAME=PATH, got {spec!r}")
        if name in [asset["name"] for asset in assets]:
            parser.error(f"duplicate --asset name {name!r}")
        assets.append({"name": name, "path": str(Path(path).resolve()), "dag_required": name in args.dag_required})
    for name in args.dag_required:
        if name not in [asset["name"] for asset in assets]:
            parser.error(f"--dag-required names an unknown asset {name!r}")
    converter = args.converter.resolve()
    if not converter.is_file():
        parser.error(f"converter not found: {converter}")
    out_dir = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    sample = None if args.no_memory else _windows_peak_sampler()
    if sample is None and not args.no_memory:
        print("note: peak working set is only sampled on Windows; recorded as null on this platform")
    measurements = {
        "schema": SCHEMA,
        "converter": {"path": str(converter), "bytes": converter.stat().st_size, "sha256": sha256_file(converter)},
        "environment": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "cpu_count": os.cpu_count(),
            "python": platform.python_version(),
            "memory_measurement": "sampled peak working set (20 ms polling, lower bound)" if sample else "not available",
        },
        "parameters": {
            "threads": threads,
            "rounds": args.rounds,
            "build_options": "converter canonical defaults (no option overrides; only --threads differs between arms)",
            "command_line": "deren-subtilis build <asset> -o <out> --threads <N> --report <report>",
        },
        "assets": [],
    }
    for asset in assets:
        source = Path(asset["path"])
        if not source.is_file():
            parser.error(f"asset not found: {source}")
        asset_entry = {
            "name": asset["name"],
            "path": str(source),
            "dag_required": asset["dag_required"],
            "source_bytes": source.stat().st_size,
            "source_sha256": sha256_file(source),
            "runs": [],
            "medians": {},
            "identity": {},
        }
        for thread_count in threads:
            for round_index in range(1, args.rounds + 1):
                stem = f"{asset['name']}-t{thread_count}-r{round_index}"
                output = out_dir / (stem + ".glb")
                report_path = out_dir / (stem + ".report.json")
                log_path = out_dir / (stem + ".log")
                for stale in (output, report_path, log_path):
                    if stale.exists():
                        stale.unlink()
                command = [
                    str(converter), "build", str(source), "-o", str(output),
                    "--threads", str(thread_count), "--report", str(report_path),
                ]
                exit_code, wall_ms, peak = run_measured(command, log_path, sample, args.timeout)
                run = {
                    "threads": thread_count,
                    "round": round_index,
                    "command": command,
                    "exit": exit_code,
                    "external_wall_ms": wall_ms,
                    "peak_working_set_bytes": peak,
                    "output_bytes": output.stat().st_size if output.exists() else 0,
                    "output_sha256": sha256_file(output) if output.exists() else None,
                    "output_path": str(output) if output.exists() else None,
                    "report_path": str(report_path) if report_path.exists() else None,
                }
                if report_path.exists():
                    report = load_report(report_path)
                    run["report"] = aggregate(report) if "_parse_error" not in report else {"_parse_error": report["_parse_error"]}
                else:
                    run["report"] = {"_missing": True}
                asset_entry["runs"].append(run)
                print(f"{asset['name']}: threads={thread_count} round={round_index} exit={exit_code} "
                      f"wall={wall_ms:.1f}ms peak={peak} out={run['output_bytes']}B")
        for thread_count in threads:
            thread_runs = [run for run in asset_entry["runs"] if run["threads"] == thread_count]
            successful = [run for run in thread_runs if run["exit"] == 0 and isinstance(run.get("report"), dict) and "_missing" not in run["report"] and "_parse_error" not in run["report"]]
            if not successful:
                continue
            stage_medians = {}
            for stage_name in STAGE_NAMES:
                stage_medians[stage_name] = {
                    "cpu_ms": statistics.median([float(run["report"]["stages"][stage_name]["cpu_ms"]) for run in successful]),
                    "count": statistics.median([int(run["report"]["stages"][stage_name]["count"]) for run in successful]),
                }
            asset_entry["medians"][str(thread_count)] = {
                "runs": len(successful),
                "external_wall_ms": statistics.median([run["external_wall_ms"] for run in successful]),
                "peak_working_set_bytes": statistics.median([run["peak_working_set_bytes"] for run in successful if run["peak_working_set_bytes"] is not None]) if any(run["peak_working_set_bytes"] is not None for run in successful) else None,
                "output_bytes": statistics.median([run["output_bytes"] for run in successful]),
                "converted": statistics.median([run["report"]["converted"] for run in successful]),
                "skipped": statistics.median([run["report"]["skipped"] for run in successful]),
                "lod0Only": statistics.median([run["report"]["lod0Only"] for run in successful]),
                "rounds_max": statistics.median([run["report"]["rounds_max"] for run in successful]),
                "cross_group_repartitions": statistics.median([run["report"]["cross_group_repartitions"] for run in successful]),
                "groups_final": statistics.median([run["report"]["groups_final"] for run in successful]),
                "report_wall_ms": statistics.median([float(run["report"]["wall_ms"]) for run in successful]),
                "accumulated_ms": statistics.median([float(run["report"]["accumulated_ms"]) for run in successful]),
                "stages": stage_medians,
            }
        hashes = sorted({run["output_sha256"] for run in asset_entry["runs"] if run.get("output_sha256")})
        asset_entry["identity"] = {
            "distinct_output_sha256": [h for h in hashes if h],
            "byte_identical_across_threads_and_rounds": len([h for h in hashes if h]) == 1,
        }
        measurements["assets"].append(asset_entry)
    measurements_path = out_dir / "measurements.json"
    measurements_path.write_text(json.dumps(measurements, indent=1))
    print(f"measurements: {measurements_path} ({measurements_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
