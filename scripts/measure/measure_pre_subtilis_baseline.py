#!/usr/bin/env python3
"""Measure the pre-subtilis renderer on the Bistro scene with an interleaved 3-round capture."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import time


CAMERAS = {
    "far": "35,18,140,0,-83.07,0",
    "near": "35,8,42,0,-88,0",
}
GPU_WINDOW = re.compile(r"gpu pass timings \(avg of (\d+) frames\):([^\r\n]+)")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def make_config(path: Path, asset: Path, shaders: Path, screenshot_dir: Path) -> None:
    quote = lambda value: json.dumps(str(value).replace("\\", "/"))
    path.write_text(
        f'''model = {quote(asset)}
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
visibility_buffer = true
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
''',
        encoding="utf-8",
    )


def run_one(exe: Path, asset: Path, work: Path, camera: str, round_id: int, frames: int, discard_images: bool) -> dict:
    tag = f"{camera}-pre-subtilis-r{round_id}"
    run_dir = work / tag
    run_dir.mkdir(parents=True, exist_ok=False)
    config = run_dir / "config.toml"
    make_config(config, asset, exe.parent / "shaders", run_dir)
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("DEREN_"):
            env.pop(key, None)
    if os.name == "nt":
        env["PATH"] = f"{exe.parent};C:/msys64/clang64/bin;" + env.get("PATH", "")
    command = [str(exe), "--config", str(config), "--capture-frames", str(frames),
               "--capture-camera", CAMERAS[camera]]
    started = time.perf_counter()
    with (run_dir / "stdout.log").open("wb") as output:
        completed = subprocess.run(command, cwd=run_dir, env=env, stdout=output, stderr=subprocess.STDOUT,
                                   timeout=600, check=False,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    elapsed = time.perf_counter() - started
    log_path = run_dir / "debug.log"
    text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
    images = list(run_dir.glob("screenshot_*.png"))
    if completed.returncode != 0 or len(images) != 1:
        raise RuntimeError(f"{tag} failed (exit={completed.returncode}, screenshots={len(images)}); inspect {log_path}")
    windows = []
    for count, body in GPU_WINDOW.findall(text):
        total = re.search(r"\btotal\s+([0-9.]+)\s+ms", body)
        visibility = re.search(r"\bvisibility raster\s+([0-9.]+)\s+ms", body)
        if total:
            windows.append({"frames": int(count), "gpu_ms": float(total.group(1)),
                            "visibility_raster_ms": float(visibility.group(1)) if visibility else None})
    timed_frames = sum(row["frames"] for row in windows)
    if timed_frames < 100:
        raise RuntimeError(f"{tag} has only {timed_frames} GPU-timed frames; inspect {log_path}")
    image_hash = sha256(images[0])
    row = {"tag": tag, "camera": camera, "round": round_id, "frames": frames,
           "exit_code": completed.returncode, "process_wall_seconds": elapsed,
           "config": str(config), "log": str(log_path), "image_sha256": image_hash,
           "gpu_timed_frames": timed_frames,
           "gpu_ms": sum(item["frames"] * item["gpu_ms"] for item in windows) / timed_frames,
           "visibility_raster_ms": (sum(item["frames"] * item["visibility_raster_ms"] for item in windows
                                         if item["visibility_raster_ms"] is not None)
                                     / sum(item["frames"] for item in windows
                                           if item["visibility_raster_ms"] is not None)),
           "gpu_windows": windows}
    (run_dir / "run.json").write_text(json.dumps(row, indent=2) + "\n", encoding="utf-8")
    if discard_images:
        images[0].unlink()
    print(f"{tag}: GPU={row['gpu_ms']:.2f} ms, visibility={row['visibility_raster_ms']:.2f} ms, "
          f"timed frames={timed_frames}, image={image_hash[:12]}", flush=True)
    return row


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--asset", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--frames", type=int, default=130)
    parser.add_argument("--discard-images", action="store_true")
    args = parser.parse_args()
    if args.rounds < 3 or args.frames < 130:
        parser.error("formal measurements require at least three rounds and 130 frames")
    exe, asset, work = args.exe.resolve(), args.asset.resolve(), args.work.resolve()
    for path in (exe, asset):
        if not path.is_file():
            parser.error(f"required input does not exist: {path}")
    work.mkdir(parents=True, exist_ok=True)
    data = {"rounds": args.rounds, "frames": args.frames, "cameras": sorted(CAMERAS),
            "asset": str(asset), "asset_sha256": sha256(asset), "executable": str(exe),
            "executable_sha256": sha256(exe), "pre_subtilis_commit": "344c3ef82af40714abc2f91d789243c44ce4885f",
            "benchmark_adjustments": ["visibility ID capacities raised to 4096 items and 4096 meshlets",
                                       "Bistro light importer capped to its four GUI light slots to avoid historical out-of-bounds write",
                                       "full diagnostic scene-tree dump omitted; rendering path unchanged"],
            "runs": []}
    output = work / "measurements.json"
    for camera_index, camera in enumerate(sorted(CAMERAS)):
        for round_id in range(1, args.rounds + 1):
            row = run_one(exe, asset, work, camera, round_id, args.frames, args.discard_images)
            data["runs"].append(row)
            output.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    summaries = {}
    for camera in sorted(CAMERAS):
        rows = [row for row in data["runs"] if row["camera"] == camera]
        summaries[camera] = {"gpu_ms": statistics.median(row["gpu_ms"] for row in rows),
                             "visibility_raster_ms": statistics.median(row["visibility_raster_ms"] for row in rows),
                             "range_gpu_ms": [min(row["gpu_ms"] for row in rows), max(row["gpu_ms"] for row in rows)],
                             "unique_image_hashes": len({row["image_sha256"] for row in rows})}
    data["summary"] = summaries
    output.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    for camera, summary in summaries.items():
        print(f"{camera}: median GPU {summary['gpu_ms']:.2f} ms, visibility raster "
              f"{summary['visibility_raster_ms']:.2f} ms, range {summary['range_gpu_ms']}, "
              f"unique images {summary['unique_image_hashes']}")
    print(f"complete: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
