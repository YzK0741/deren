#!/usr/bin/env python3
"""Interleaved Bistro performance matrix for Cluster DAG, software raster, and HZB composition.

The scene, resolution, render settings, camera and frame count are held fixed. HZB is enabled through its existing
BUILD/CULL/RECOVERY environment gates. Results and raw captures go to an ignored build directory by default.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time

from analyze_object_lod import parse_log


CAMERAS = {"far": "35,18,140,0,-83.07,0", "near": "35,8,42,0,-88,0"}


CASES = {
    "no_optimizations": {"visibility": True, "cluster_dag": False, "software_raster": False, "hzb": False, "threshold_px": 1.0},
    "cluster_dag": {"visibility": True, "cluster_dag": True, "software_raster": False, "hzb": False, "threshold_px": 1.0},
    "software_raster": {"visibility": True, "cluster_dag": True, "software_raster": True, "hzb": False, "threshold_px": 1.0},
    "hzb": {"visibility": True, "cluster_dag": True, "software_raster": False, "hzb": True, "threshold_px": 1.0},
    "all_enabled": {"visibility": True, "cluster_dag": True, "software_raster": True, "hzb": True, "threshold_px": 1.0},
    "hzb_software_0_25": {"visibility": True, "cluster_dag": True, "software_raster": True, "hzb": True, "threshold_px": 0.25},
    "hzb_software_0_5": {"visibility": True, "cluster_dag": True, "software_raster": True, "hzb": True, "threshold_px": 0.5},
    "hzb_software_2_0": {"visibility": True, "cluster_dag": True, "software_raster": True, "hzb": True, "threshold_px": 2.0},
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def shader_bundle_sha256(directory: Path) -> str:
    digest = hashlib.sha256()
    shaders = sorted(directory.rglob("*.spv"))
    if not shaders:
        raise ValueError(f"no compiled shaders found in {directory}")
    for path in shaders:
        digest.update(path.relative_to(directory).as_posix().encode("utf-8"))
        digest.update(b"\0")
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    return digest.hexdigest()


def parse_run_log(text: str) -> dict:
    metrics = parse_log(text)
    timing_lines = re.findall(r"gpu pass timings \(session avg of (\d+) frames\):([^\n]+)", text)
    if timing_lines:
        line = timing_lines[-1][1]
        for label, key in (("visibility raster", "visibility_raster_ms"), ("software visibility", "software_raster_ms"),
                           ("software merge", "software_merge_ms"), ("hzb cull", "hzb_cull_ms"),
                           ("hzb build", "hzb_build_ms"), ("hzb recovery", "hzb_recovery_ms"),
                           ("visibility recovery", "visibility_recovery_ms")):
            match = re.search(re.escape(label) + r"\s+([0-9.]+)\s+ms", line)
            if match:
                metrics[key] = float(match.group(1))
    metrics["software_raster_active"] = "software raster active:" in text
    metrics["cluster_dag_active"] = bool(re.search(r"subtilis CPU cut: frames \d+", text))
    metrics["hzb_cull_active"] = "hzb: cull lane ARMED" in text and "hzb cull: session " in text
    metrics["hzb_recovery_active"] = "hzb recovery: ARMED" in text and "prediction gate OPEN" in text
    metrics["hzb_culled_percent"] = None
    cull = re.findall(r"hzb cull: session \d+ candidate\(s\) / \d+ culled / \d+ survivor\(s\) \(([0-9.]+)% culled\)", text)
    if cull:
        metrics["hzb_culled_percent"] = float(cull[-1])
    metrics["software_counts"] = None
    software = re.findall(r"software raster active: threshold ([0-9.]+) px, (\d+) software cluster\(s\), (\d+) (?:CSR|work) entries", text)
    if software:
        metrics["software_counts"] = {"threshold_px": float(software[-1][0]), "clusters": int(software[-1][1]), "csr_entries": int(software[-1][2])}
    return metrics


def validate_matrix(data: dict) -> dict:
    rounds = data.get("rounds")
    cameras = data.get("cameras")
    runs = data.get("runs")
    if type(rounds) is not int or rounds < 3 or cameras != sorted(CAMERAS) or not isinstance(runs, list):
        raise ValueError("matrix metadata is incomplete or fewer than three rounds")
    expected = {(camera, case, round_id) for camera in cameras for case in CASES for round_id in range(1, rounds + 1)}
    keys = [(row.get("camera"), row.get("case"), row.get("round")) for row in runs]
    if len(set(keys)) != len(keys) or set(keys) != expected:
        raise ValueError("matrix has missing or duplicate camera/case/round captures")
    required = ("gpu_ms", "cpu_active_ms", "gpu_frames", "exit_code", "image_sha256")
    for row in runs:
        if row.get("exit_code") != 0 or row.get("gpu_frames", 0) < 100 or any(
            not isinstance(row.get(key), (int, float)) or not math.isfinite(row[key]) or row[key] < 0
            for key in required[:3]
        ):
            raise ValueError(f"failed or incomplete capture: {row.get('tag')}")
        expected_case = CASES[row["case"]]
        if expected_case["software_raster"] and not row.get("software_raster_active"):
            raise ValueError(f"software-raster case did not activate: {row['tag']}")
        if expected_case["cluster_dag"] and not row.get("cluster_dag_active"):
            raise ValueError(f"Cluster DAG case did not activate: {row['tag']}")
        if expected_case["hzb"] and not (row.get("hzb_cull_active") and row.get("hzb_recovery_active")):
            raise ValueError(f"HZB cull/recovery case did not activate: {row['tag']}")
    result = {}
    for camera in cameras:
        baseline = statistics.median(row["gpu_ms"] for row in runs if row["camera"] == camera and row["case"] == "cluster_dag")
        unoptimized_baseline = statistics.median(row["gpu_ms"] for row in runs if row["camera"] == camera and row["case"] == "no_optimizations")
        for case in CASES:
            rows = [row for row in runs if row["camera"] == camera and row["case"] == case]
            med_gpu = statistics.median(row["gpu_ms"] for row in rows)
            fields = ("gpu_ms", "cpu_active_ms", "frame_wall_ms", "visibility_raster_ms", "software_raster_ms",
                      "software_merge_ms", "hzb_cull_ms", "hzb_build_ms", "hzb_recovery_ms", "visibility_recovery_ms",
                      "cut_wall_us", "hzb_culled_percent")
            summary = {key: statistics.median(row[key] for row in rows if isinstance(row.get(key), (int, float)))
                       if any(isinstance(row.get(key), (int, float)) for row in rows) else None for key in fields}
            summary["gpu_delta_vs_cluster_dag_percent"] = (med_gpu - baseline) / baseline * 100.0 if baseline else None
            summary["gpu_delta_vs_no_optimizations_percent"] = (
                (med_gpu - unoptimized_baseline) / unoptimized_baseline * 100.0 if unoptimized_baseline else None
            )
            summary["unique_image_hashes"] = len({row["image_sha256"] for row in rows})
            summary["software_raster_effective"] = bool(CASES[case]["software_raster"])
            result[f"{camera}/{case}"] = summary
    return result


def make_config(path: Path, asset: Path, shaders: Path, screenshot_dir: Path, case: str) -> None:
    settings = CASES[case]
    quote = lambda value: json.dumps(str(value).replace("\\", "/"))
    path.write_text(f'''model = {quote(asset)}
[paths]
shaders_dir = {quote(shaders)}
screenshot_dir = {quote(screenshot_dir)}
[render]
window_width = 1280
window_height = 720
vsync = false
max_fps = 0
validation_layers = false
main_render_graph = true
visibility_buffer = {str(settings["visibility"]).lower()}
subtilis = {str(settings["cluster_dag"]).lower()}
subtilis_error_px = 1.0
subtilis_hysteresis = false
software_raster = {str(settings["software_raster"]).lower()}
software_raster_threshold_px = {settings['threshold_px']}
character_forward = false
shadow = false
taa = false
ssao = false
bloom = false
fxaa = false
gpu_timings = true
[gui]
show = false
[lighting]
env_size = 128
env_mip_count = 4
irr_size = 16
lut_size = 128
''', encoding="utf-8")


def run_one(exe: Path, asset: Path, work: Path, case: str, camera: str, round_id: int, frames: int, *, triangles: bool = False, edge_routing: bool = False, gpu_routing: bool = False) -> dict:
    tag = f"{camera}-{case}-r{round_id}"
    run_dir = work / tag
    run_dir.mkdir(parents=True, exist_ok=False)
    config = run_dir / "config.toml"
    make_config(config, asset, exe.parent / "shaders", run_dir, case)
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("DEREN_"):
            env.pop(key, None)
    if os.name == "nt":
        env["PATH"] = "C:/msys64/clang64/bin;" + env.get("PATH", "")
    if triangles:
        env["DEREN_SOFTWARE_RASTER_TRIANGLES"] = "1"
    if edge_routing:
        if not triangles: raise ValueError('edge routing requires triangle producer')
        env["DEREN_SOFTWARE_RASTER_EDGE"] = "1"
    if gpu_routing:
        if not triangles or not CASES[case]["hzb"]: raise ValueError("GPU routing requires triangles and HZB")
        env["DEREN_SOFTWARE_RASTER_GPU"] = "1"
    if CASES[case]["hzb"]:
        env.update(DEREN_HZB_BUILD="1", DEREN_HZB_CULL="1", DEREN_HZB_RECOVERY="1")
    command = [str(exe), "--config", str(config), "--capture-frames", str(frames), f"--capture-camera={CAMERAS[camera]}"]
    started = time.perf_counter()
    completed = subprocess.run(command, cwd=run_dir, env=env, stdout=(run_dir / "stdout.log").open("wb"),
                               stderr=subprocess.STDOUT, timeout=600, check=False,
                               creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    elapsed = time.perf_counter() - started
    log_path = run_dir / "debug.log"
    text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
    images = list(run_dir.glob("screenshot_*.png"))
    if completed.returncode != 0 or len(images) != 1:
        raise RuntimeError(f"{tag} failed (exit={completed.returncode}, screenshots={len(images)}); inspect {log_path}")
    if triangles and "software triangle prototype: recorded clear/raster/unpack" not in text:
        raise RuntimeError(f"{tag}: requested triangle prototype was not recorded")
    if edge_routing and "software routing: triangle-edge metric active" not in text:
        raise RuntimeError(f"{tag}: requested edge routing was not active")
    if gpu_routing and "software GPU routing active: HZB compaction, bounded software list" not in text:
        raise RuntimeError(f"{tag}: requested GPU routing was not recorded")
    metrics = parse_run_log(text)
    row = {"tag": tag, "case": case, "triangle_prototype": triangles, "edge_routing": edge_routing, "gpu_routing": gpu_routing, "camera": camera, "round": round_id, "frames": frames,
           "exit_code": completed.returncode, "process_wall_seconds": elapsed, "config": str(config),
           "log": str(log_path), "image": str(images[0]), "image_sha256": sha256(images[0]), **metrics}
    (run_dir / "run.json").write_text(json.dumps(row, indent=2) + "\n", encoding="utf-8")
    print(f"{tag}: GPU={row.get('gpu_ms')}ms frames={row.get('gpu_frames')} cut={row.get('cut_wall_us')}us "
          f"software={row.get('software_raster_active')} hzb={row.get('hzb_culled_percent')}", flush=True)
    return row


def self_test() -> None:
    rows = []
    for camera in sorted(CAMERAS):
        for round_id in range(1, 4):
            for case in CASES:
                flags = CASES[case]
                rows.append({"camera": camera, "case": case, "round": round_id, "gpu_ms": float(10 + round_id),
                             "cpu_active_ms": 2.0, "gpu_frames": 129, "exit_code": 0, "image_sha256": "same",
                             "cluster_dag_active": flags["cluster_dag"], "software_raster_active": flags["software_raster"],
                             "hzb_cull_active": flags["hzb"], "hzb_recovery_active": flags["hzb"], "hzb_culled_percent": 50.0})
    result = validate_matrix({"rounds": 3, "cameras": sorted(CAMERAS), "runs": rows})
    assert result["far/cluster_dag"]["gpu_ms"] == 12.0
    assert result["far/all_enabled"]["software_raster_effective"] is True
    for invalid in ({"rounds": 2, "cameras": sorted(CAMERAS), "runs": rows},
                    {"rounds": 3, "cameras": sorted(CAMERAS), "runs": rows[:-1]}):
        try:
            validate_matrix(invalid)
        except ValueError:
            continue
        raise AssertionError("invalid benchmark matrix accepted")
    sample = parse_run_log("gpu pass timings (session avg of 129 frames): visibility raster 9.00 ms | software visibility 0.10 ms | total 10.00 ms\nsoftware raster active: threshold 1 px, 100 software cluster(s), 120 CSR entries\n")
    assert sample["visibility_raster_ms"] == 9 and sample["software_raster_active"]
    print("render optimization benchmark self-test: PASS (eight cases including all-off baseline and threshold sweep, two cameras, three rounds, missing/short controls)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asset", type=Path, default=Path("D:/deren-assets/2026-10-21-object-lod/bistro-object-lod.glb"))
    parser.add_argument("--exe", type=Path, default=Path("build-release-dyn-clang64/deren.exe"))
    parser.add_argument("--work", type=Path, default=Path("build-release-dyn-clang64/render-optimization-comparison"))
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--frames", type=int, default=130)
    parser.add_argument("--discard-images", action="store_true", help="delete each screenshot after recording its SHA-256")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.rounds < 3 or args.frames < 130:
        parser.error("formal measurements require at least three rounds and 130 frames")
    exe, asset, work = args.exe.resolve(), args.asset.resolve(), args.work.resolve()
    for path in (exe, asset):
        if not path.is_file():
            parser.error(f"required input does not exist: {path}")
    work.mkdir(parents=True, exist_ok=True)
    data = {"rounds": args.rounds, "frames": args.frames, "cameras": sorted(CAMERAS), "cases": CASES,
            "asset": str(asset), "asset_sha256": sha256(asset), "executable": str(exe),
            "executable_sha256": sha256(exe), "shader_bundle_sha256": shader_bundle_sha256(exe.parent / "shaders"), "runs": []}
    output = work / "measurements.json"
    # Rotate case order by round and camera to reduce thermal/order bias; every cell is still retained.
    names = list(CASES)
    for camera_index, camera in enumerate(sorted(CAMERAS)):
        for round_id in range(1, args.rounds + 1):
            offset = (round_id - 1 + camera_index * 2) % len(names)
            order = names[offset:] + names[:offset]
            for case in order:
                row = run_one(exe, asset, work, case, camera, round_id, args.frames)
                if args.discard_images:
                    Path(row["image"]).unlink()
                data["runs"].append(row)
                output.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    data["summary"] = validate_matrix(data)
    output.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    for key, value in data["summary"].items():
        print(f"{key}: GPU {value['gpu_ms']:.2f} ms ({value['gpu_delta_vs_cluster_dag_percent']:+.1f}% vs Cluster DAG), "
              f"{value['gpu_delta_vs_no_optimizations_percent']:+.1f}% vs all-off, CPU active {value['cpu_active_ms']:.2f} ms, "
              f"unique images {value['unique_image_hashes']}")
    print(f"complete: {output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired, AssertionError) as error:
        print(f"render optimization benchmark: FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
