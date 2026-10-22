"""Task 1 (GO/NO-GO) measurement for M4 software raster: the subpixel workload upper bound.

WHAT IT MEASURES, AND WHY IT EXISTS. The M4 spec (docs/superpowers/specs/2026-10-22-software-raster-design.md)
proposes to move a cut run's SUBPIXEL clusters from the hardware visibility raster to a tiled software producer.
Before that producer is built, Task 1 measures the ceiling of what the hardware raster can save by NOT dispatching
those clusters at all:

  baseline   - the accepted M1c path (subtilis on, no software raster): every cut cluster is dispatched to hardware;
  holes      - the SAME capture with `DEREN_SOFTWARE_RASTER_PROBE=workload`: every frame's cut is classified and
               split [hardware prefix | software suffix], the hardware raster dispatches the PREFIX ONLY, the suffix
               is not drawn (there is no software pass yet), and the screenshot frame's classification/tile data is
               dumped as JSON. The captured frame therefore has HOLES where the software producer would have
               drawn - the expected measurement state, never a state a default frame can reach.

The upper bound is the `visibility raster` GPU mark difference between the two arms' session averages. It ignores
the cost of the software producer that would have to fill the holes, which is exactly why it is an upper bound and
why Task 1 is only a GATE: GO means "worth implementing", the real ROI is Task 6.

GO/NO-GO (plan Task 1 acceptance / spec §7.4): GO requires the far camera's visibility-raster medians (3 interleaved
rounds, never the best round) to differ by >= 1.5% of M1c's far ON 20.11 ms, i.e. >= ~0.302 ms, with the classified
counts explainable against the M1c probe's proportions; the near camera is recorded but not required. The tile
capacity (spec §9 Q7: 32 vs 64) is decided from the measured per-tile maximum at 1.0 px and 2.0 px, not assumed.

The script is both the capture driver and the analysis gate:
  python scripts/measure/software_raster_roi.py --self-test
  python scripts/measure/software_raster_roi.py --capture --work <dir>          # the formal 12-launch matrix
  python scripts/measure/software_raster_roi.py --capture --work <dir> --smoke  # 2 launches, not a measurement
  python scripts/measure/software_raster_roi.py --analyze <work>/measurements.json
The analysis REQUIRES every recorded metric to equal what the raw log says (a fabricated/copied run JSON is
rejected), every round to be present exactly once, and every holes arm to carry a self-checked probe dump.
"""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time

NUMBER = r"([0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)"

CAMERAS = {"far": "35,18,140,0,-83.07,0", "near": "35,8,42,0,-88,0"}
ROUNDS = 3
FRAMES = 130
EXTENT = (1280, 720)
ASSET = Path("D:/deren-assets/2026-10-21-object-lod/bistro-object-lod.glb")
ASSET_SHA256 = "620f424239909d023b30dd86fe953c579298a39ef2d4706bca59fc0d2982085b"
EXE = Path("build-release-dyn-clang64/deren.exe")

# M1c's accepted numbers this gate is defined against (docs/reviews/2026-10-21-object-lod.md §4/§8):
#   far GPU 24.46 -> 20.11 ms (session average, 130 frames), cut clusters 41,030, cut triangles 3,318,617,
#   <1px 23.28% / <2px 38.23% of clusters, <1px 21.92% / <2px 36.75% of triangles;
#   near GPU 21.33 -> 18.58 ms, cut clusters 35,444, cut triangles 2,860,109, <1px 5.37% / <2px 15.95%,
#   <1px 4.12% / <2px 14.46%.
M1C_REFERENCE = {
    "far": {"visibility_raster_ms": 20.11, "clusters": 41030, "triangles": 3318617,
            "below_1px_pct": 23.28, "below_2px_pct": 38.23, "triangles_below_1px_pct": 21.92,
            "triangles_below_2px_pct": 36.75},
    "near": {"visibility_raster_ms": 18.58, "clusters": 35444, "triangles": 2860109,
             "below_1px_pct": 5.37, "below_2px_pct": 15.95, "triangles_below_1px_pct": 4.12,
             "triangles_below_2px_pct": 14.46},
}
GO_FRACTION = 0.015  # far visibility-raster median delta must reach 1.5% of M1c's far ON raster time


# ====================================================================================================================
# LOG PARSING - the analysis recomputes every recorded metric from the raw capture log (never trusts run.json alone)
# ====================================================================================================================
def parse_log(text: str) -> dict:
    metrics: dict = {
        "validation_clean": not re.search(r"VUID-|Validation Error|\[ERROR\]|\[WARNING\]|panic|recorded out of order", text),
        "probe_armed": "software raster workload probe: ARMED" in text,
        "probe_dumped": "software raster workload probe: frame " in text,
        "probe_self_check_failed": "software raster workload probe: SELF-CHECK FAILED" in text,
    }
    gpu = re.findall(r"gpu pass timings \(session avg of (\d+) frames\):([^\n]+)", text)
    if gpu:
        frames, stages_text = gpu[-1]
        stages = {name: float(value) for name, value in
                  re.findall(r"([A-Za-z][A-Za-z0-9 _-]*?) " + NUMBER + r" ms", stages_text)}
        metrics["gpu_frames"] = int(frames)
        metrics["gpu_stages"] = stages
        if "total" in stages:
            metrics["gpu_total_ms"] = stages["total"]
        if "visibility raster" in stages:
            metrics["visibility_raster_ms"] = stages["visibility raster"]
    cpu = re.findall(r"cpu frame phases \(avg of \d+ frames\):([^\n]+)", text)
    if cpu:
        phases = {name: float(value) for name, value in
                  re.findall(r"([A-Za-z][A-Za-z0-9 _-]*?) " + NUMBER + r" ms", cpu[-1])}
        if "total" in phases and "pace" in phases:
            metrics["cpu_active_ms"] = phases["total"] - phases["pace"]
            metrics["cpu_total_ms"] = phases["total"]
    mesh = re.findall(
        r"mesh culling \(visibility lane\): (\d+) meshlet workgroups, (\d+) emitted their triangles, \d+ were culled "
        r"\([^)]*\), (\d+) triangles from meshlets", text)
    if mesh:
        metrics["mesh_workgroups"] = int(mesh[-1][0])
        metrics["mesh_emitted"] = int(mesh[-1][1])
        metrics["mesh_triangles"] = int(mesh[-1][2])
    publication = re.findall(
        r"subtilis: cut run publication: (\d+) -> (\d+) records, (\d+) -> (\d+) triangles, (\d+) fallback item", text)
    if publication:
        metrics["selected_records"] = int(publication[-1][1])
        metrics["selected_triangles"] = int(publication[-1][3])
        metrics["cut_fallbacks"] = int(publication[-1][4])
    capacity = re.findall(
        r"subtilis capacity: items (\d+) peak (\d+), table (\d+)/(\d+), max run (\d+), refused frames (\d+)", text)
    if capacity:
        metrics["visible_items"] = int(capacity[-1][0])
        metrics["table_records"] = int(capacity[-1][2])
        metrics["capacity_frames"] = int(capacity[-1][5])
    capture = re.search(r"capture mode: (\d+) frames, then screenshot \+ quit", text)
    if capture:
        metrics["capture_frames"] = int(capture.group(1))
    screenshot = re.findall(r"screenshot saved: (.+?) \((\d+)x(\d+)\)", text)
    if screenshot:
        metrics["screenshot_path"] = screenshot[-1][0].strip()
        metrics["screenshot_extent"] = [int(screenshot[-1][1]), int(screenshot[-1][2])]
    camera = re.search(r"capture camera: (.+)", text)
    if camera:
        metrics["capture_camera"] = camera.group(1).strip()
    return metrics


def sha256(path: Path) -> str:
    import hashlib
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


# ====================================================================================================================
# CAPTURE - one interleaved arm of the matrix
# ====================================================================================================================
def write_config(path: Path, asset: Path, shaders: Path, screenshots: Path, validation: bool) -> None:
    quote = lambda value: json.dumps(str(value).replace("\\", "/"))
    path.write_text(
        f"""model = {quote(asset)}
[paths]
shaders_dir = {quote(shaders)}
screenshot_dir = {quote(screenshots)}
[render]
window_width = {EXTENT[0]}
window_height = {EXTENT[1]}
vsync = false
max_fps = 0
validation_layers = {str(validation).lower()}
visibility_buffer = true
subtilis = true
subtilis_error_px = 1.0
subtilis_hysteresis = false
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
""",
        encoding="utf-8")


def capture(exe: Path, asset: Path, work: Path, tag: str, camera: str, camera_pose: str, arm: str, validation: bool = False) -> dict:
    run = work / tag
    run.mkdir(parents=True, exist_ok=False)
    config = run / "config.toml"
    write_config(config, asset, exe.parent / "shaders", run, validation)
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("DEREN_"):
            del env[key]
    if os.name == "nt":
        env["PATH"] = "C:/msys64/clang64/bin;" + env.get("PATH", "")
    dump = None
    if arm == "holes":
        dump = run / "workload.json"
        env["DEREN_SOFTWARE_RASTER_PROBE"] = "workload"
        env["DEREN_SOFTWARE_RASTER_PROBE_DUMP"] = str(dump)
    command = [str(exe), "--config", str(config), "--capture-frames", str(FRAMES), f"--capture-camera={camera_pose}"]
    started = time.perf_counter()
    with (run / "stdout.log").open("wb") as stdout:
        process = subprocess.Popen(command, cwd=run, env=env, stdout=stdout, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        while process.poll() is None:
            if time.perf_counter() - started > 600:
                process.kill()
                process.wait()
                raise SystemExit(f"{tag}: timed out")
            time.sleep(0.05)
    elapsed = time.perf_counter() - started
    log = run / "debug.log"
    text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
    images = list(run.glob("screenshot_*.png"))
    metrics = parse_log(text)
    result = {"tag": tag, "camera": camera, "camera_pose": camera_pose, "arm": arm, "validation": validation, "frames": FRAMES,
              "exit_code": process.returncode, "process_wall_seconds": elapsed,
              "log": str(log), "image": str(images[0]) if len(images) == 1 else None,
              "workload_json": str(dump) if dump is not None else None, "command": command, **metrics}
    (run / "run.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if process.returncode != 0 or not result["image"] or not result["validation_clean"]:
        raise SystemExit(f"failed capture: {tag}; inspect {log}")
    print(f"{tag}: {elapsed:.2f}s, GPU {result.get('gpu_total_ms')}ms, "
          f"visibility raster {result.get('visibility_raster_ms')}ms", flush=True)
    return result


def capture_matrix(exe: Path, asset: Path, work: Path, rounds: int, smoke: bool) -> None:
    exe, asset, work = exe.resolve(), asset.resolve(), work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    if sha256(asset) != ASSET_SHA256:
        raise SystemExit(f"asset SHA-256 mismatch: {asset} is not M1c's accepted cooked GLB")
    data = {"kind": "software-raster-roi", "version": 1, "rounds": rounds, "frames": FRAMES, "extent": list(EXTENT),
            "asset": str(asset), "asset_sha256": sha256(asset), "exe": str(exe), "exe_sha256": sha256(exe),
            "camera_parameters": CAMERAS, "m1c_reference": M1C_REFERENCE, "go_fraction": GO_FRACTION,
            "runs": []}
    save = lambda: (work / "measurements.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    if smoke:
        data["runs"].append(capture(exe, asset, work, "smoke-far-baseline", "far", CAMERAS["far"], "baseline"))
        save()
        data["runs"].append(capture(exe, asset, work, "smoke-far-holes", "far", CAMERAS["far"], "holes"))
        save()
        print("smoke captures complete (not a measurement matrix)", flush=True)
        return
    for camera in CAMERAS:
        for round_index in range(1, rounds + 1):
            order = ("baseline", "holes") if round_index % 2 else ("holes", "baseline")
            for arm in order:
                tag = f"{camera}-{arm}-{round_index}"
                row = capture(exe, asset, work, tag, camera, CAMERAS[camera], arm)
                row["round"] = round_index
                data["runs"].append(row)
                save()
    data["validation_smoke"] = capture(exe, asset, work, "validation-far-holes", "far", CAMERAS["far"], "holes", validation=True)
    save()
    print("formal matrix complete; run --analyze on measurements.json", flush=True)


# ====================================================================================================================
# PROBE DUMP VALIDATION - the dump is the CPU-side evidence; a malformed/self-checked-failed dump is a failed run
# ====================================================================================================================
def validate_workload(path: Path, camera: str) -> dict:
    if not path.is_file():
        raise ValueError(f"holes control has no workload dump: {path}")
    data = json.loads(path.read_text(encoding="utf-8"))
    expected = {"far": M1C_REFERENCE["far"], "near": M1C_REFERENCE["near"]}[camera]
    if data.get("kind") != "software-raster-workload" or data.get("version") != 1:
        raise ValueError(f"{path}: not a software-raster workload dump")
    if list(data.get("extent", {}).values()) != list(EXTENT):
        raise ValueError(f"{path}: wrong extent")
    thresholds = data.get("thresholds_px")
    if thresholds != [1, 2]:
        raise ValueError(f"{path}: expected thresholds [1, 2], got {thresholds}")
    if not data.get("self_check", {}).get("primary_split_matches_phase2"):
        raise ValueError(f"{path}: the probe's phase-2 split self-check failed")
    totals = data["totals"]
    for key in ("items", "classified_items", "clusters", "triangles", "hardware_clusters", "software_clusters",
                "hardware_triangles", "software_triangles", "software_clusters_2_0", "software_triangles_2_0"):
        value = totals.get(key)
        if not isinstance(value, int) or value < 0:
            raise ValueError(f"{path}: missing/invalid total {key}")
    if totals["hardware_clusters"] + totals["software_clusters"] != totals["clusters"]:
        raise ValueError(f"{path}: hardware + software clusters do not partition the total")
    if totals["clusters"] > expected["clusters"]:
        raise ValueError(f"{path}: classified more clusters than M1c's cut ({totals['clusters']} > {expected['clusters']})")
    if not 0.0 <= totals["software_clusters"] / max(1, totals["clusters"]) <= 1.0:
        raise ValueError(f"{path}: software cluster share is not a fraction")
    for band in ("1.0", "2.0"):
        capacity = data.get("capacity", {}).get(band, {})
        for key in ("tile_count", "entry_count", "max_tile_entries", "overflow_tiles_at_32", "overflow_tiles_at_64"):
            value = capacity.get(key)
            if not isinstance(value, int) or value < 0:
                raise ValueError(f"{path}: missing/invalid capacity.{band}.{key}")
        if capacity["overflow_tiles_at_32"] > capacity["tile_count"] or capacity["overflow_tiles_at_64"] > capacity["tile_count"]:
            raise ValueError(f"{path}: overflow tile count exceeds the tile count")
    counts = data.get("tile_counts_1_0")
    if not isinstance(counts, list) or len(counts) != data["capacity"]["1.0"]["tile_count"]:
        raise ValueError(f"{path}: tile_counts_1_0 does not match the tile count")
    if sum(counts) != data["capacity"]["1.0"]["entry_count"]:
        raise ValueError(f"{path}: tile_counts_1_0 does not sum to entry_count")
    if max(counts, default=0) != data["capacity"]["1.0"]["max_tile_entries"]:
        raise ValueError(f"{path}: max_tile_entries is not the measured max")
    counts_two = data["tile_counts_2_0"]
    if len(counts_two) != data["capacity"]["2.0"]["tile_count"] or max(counts_two, default=0) != data["capacity"]["2.0"]["max_tile_entries"]:
        raise ValueError(f"{path}: the 2.0 tile counts do not match their header")
    cost = data.get("cost_us", {})
    for key in ("classify_frame", "classify_session_avg", "bin_frame"):
        value = cost.get(key)
        if not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
            raise ValueError(f"{path}: missing/invalid cost {key}")
    return data


# ====================================================================================================================
# MATRIX VALIDATION + ANALYSIS
# ====================================================================================================================
def verify_matrix(data: dict) -> None:
    if data.get("kind") != "software-raster-roi" or data.get("version") != 1:
        raise ValueError("not a software-raster ROI measurement file")
    rounds = data.get("rounds")
    if not isinstance(rounds, int) or rounds < ROUNDS:
        raise ValueError("at least three interleaved rounds are required")
    expected = {(camera, arm, round_index) for camera in CAMERAS for arm in ("baseline", "holes")
                for round_index in range(1, rounds + 1)}
    keys = [(run["camera"], run["arm"], run.get("round")) for run in data["runs"]]
    if len(set(keys)) != len(keys) or set(keys) != expected:
        raise ValueError("incomplete or duplicate camera/arm/round matrix")
    for run in data["runs"]:
        tag = run["tag"]
        if run.get("exit_code") != 0:
            raise ValueError(f"{tag}: capture failed")
        if not run.get("validation_clean"):
            raise ValueError(f"{tag}: validation/log findings")
        if run.get("gpu_frames") != FRAMES - 1:
            raise ValueError(f"{tag}: short GPU timing session ({run.get('gpu_frames')} frames)")
        for key in ("gpu_total_ms", "visibility_raster_ms", "mesh_workgroups", "mesh_triangles", "cpu_active_ms"):
            value = run.get(key)
            if not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise ValueError(f"{tag}: missing/invalid metric {key}")
        if not run.get("image") or not Path(run["image"]).is_file():
            raise ValueError(f"{tag}: missing screenshot")
        if run.get("camera_pose") != CAMERAS.get(run["camera"]):
            raise ValueError(f"{tag}: camera pose does not match the camera name")
        if run["arm"] == "baseline":
            if run.get("probe_armed"):
                raise ValueError(f"{tag}: baseline arm armed the workload probe")
        else:
            if not run.get("probe_armed") or not run.get("probe_dumped"):
                raise ValueError(f"{tag}: holes arm did not arm/dump the workload probe")
            validate_workload(Path(run["workload_json"]), run["camera"])
    validation_smoke = data.get("validation_smoke")
    if validation_smoke is not None:
        if validation_smoke.get("exit_code") != 0 or not validation_smoke.get("validation_clean"):
            raise ValueError("the validation-on holes smoke capture failed")
        validate_workload(Path(validation_smoke["workload_json"]), validation_smoke["camera"])


def verify_against_logs(data: dict) -> None:
    """Every recorded metric must equal what the raw debug log says (the fabricated-run guard)."""
    for run in data["runs"] + ([data["validation_smoke"]] if data.get("validation_smoke") else []):
        log = Path(run["log"])
        if not log.is_file():
            raise ValueError(f"{run['tag']}: raw log is missing")
        actual = parse_log(log.read_text(encoding="utf-8", errors="replace"))
        for key in ("gpu_total_ms", "visibility_raster_ms", "gpu_frames", "mesh_workgroups", "mesh_triangles",
                    "cpu_active_ms", "probe_armed", "probe_dumped", "capture_frames", "validation_clean"):
            if key in actual and run.get(key) != actual[key]:
                raise ValueError(f"{run['tag']}: recorded {key}={run.get(key)!r} differs from the raw log ({actual[key]!r})")


def median(values):
    return statistics.median(values)


def analyze(data: dict) -> dict:
    verify_matrix(data)
    verify_against_logs(data)
    rounds = data["rounds"]
    results: dict = {"kind": "software-raster-roi-result", "version": 1, "rounds": rounds,
                     "m1c_reference": M1C_REFERENCE, "go_fraction": GO_FRACTION, "cameras": {},
                     "capacity_decision": {}, "go": False, "reasons": []}
    go_required = {}
    for camera in CAMERAS:
        reference = M1C_REFERENCE[camera]
        rows = [run for run in data["runs"] if run["camera"] == camera]
        rows.sort(key=lambda run: (run["round"], run["arm"]))
        def arm(arm_name: str):
            return [run for run in rows if run["arm"] == arm_name]
        baseline, holes = arm("baseline"), arm("holes")
        if len(baseline) != rounds or len(holes) != rounds:
            raise ValueError(f"{camera}: wrong arm counts")
        probe_by_round = {run["round"]: validate_workload(Path(run["workload_json"]), camera) for run in holes}
        per_round = []
        for round_index in range(1, rounds + 1):
            base = next(run for run in baseline if run["round"] == round_index)
            hole = next(run for run in holes if run["round"] == round_index)
            probe = probe_by_round[round_index]
            workgroups_removed = base["mesh_workgroups"] - hole["mesh_workgroups"]
            triangles_removed = base["mesh_triangles"] - hole["mesh_triangles"]
            if workgroups_removed <= 0:
                raise ValueError(f"{camera} round {round_index}: holes control removed no hardware workgroups")
            # explainability: the removed session workgroups/triangles must equal the probe frame's software suffix
            # scaled by the session's own frame count. The multiplier is DERIVED from the baseline arm
            # (baseline session workgroups / the probe frame's cluster count) instead of assumed, because the mesh
            # stats session covers one more frame than the GPU timing session in this renderer.
            frame_factor = base["mesh_workgroups"] / max(1, probe["totals"]["clusters"])
            expected_workgroups = probe["totals"]["software_clusters"] * frame_factor
            # triangles are the CANDIDATE triangles in the probe (a meshlet's window), while the session's emitted
            # triangle count already drops faces the mesh stage culls (back-facing/frustum) - the far camera emits
            # every candidate triangle, the near camera culls ~1.8% of them, so the removal is bounded above by the
            # candidate count and must not exceed it; the workgroup count is exact (one group per cluster).
            candidate_triangles = probe["totals"]["software_triangles"] * frame_factor
            workgroup_error = abs(workgroups_removed - expected_workgroups) / max(1, expected_workgroups)
            if workgroup_error > 0.01 or triangles_removed > candidate_triangles * 1.000001 or triangles_removed < candidate_triangles * 0.90:
                raise ValueError(f"{camera} round {round_index}: removed work does not match the probe's software suffix "
                                 f"(workgroups {workgroups_removed} vs {expected_workgroups}, triangles {triangles_removed} vs candidate "
                                 f"{candidate_triangles})")
            per_round.append({
                "round": round_index,
                "baseline_visibility_raster_ms": base["visibility_raster_ms"],
                "holes_visibility_raster_ms": hole["visibility_raster_ms"],
                "visibility_raster_delta_ms": base["visibility_raster_ms"] - hole["visibility_raster_ms"],
                "baseline_gpu_total_ms": base["gpu_total_ms"],
                "holes_gpu_total_ms": hole["gpu_total_ms"],
                "gpu_total_delta_ms": base["gpu_total_ms"] - hole["gpu_total_ms"],
                "mesh_workgroups_removed": workgroups_removed,
                "mesh_workgroups_removed_pct": 100.0 * workgroups_removed / base["mesh_workgroups"],
                "mesh_triangles_removed": triangles_removed,
                "probe_software_clusters": probe["totals"]["software_clusters"],
                "probe_software_triangles": probe["totals"]["software_triangles"],
                "probe_software_clusters_2_0": probe["totals"]["software_clusters_2_0"],
                "probe_classify_frame_us": probe["cost_us"]["classify_frame"],
                "probe_classify_session_avg_us": probe["cost_us"]["classify_session_avg"],
                "probe_bin_frame_us": probe["cost_us"]["bin_frame"],
                "probe_bin_items_us": probe["cost_us"].get("bin_items"),
                "probe_bin_primary_us": probe["cost_us"].get("bin_primary"),
                "probe_bin_secondary_us": probe["cost_us"].get("bin_secondary"),
                "tile_max_1_0": probe["capacity"]["1.0"]["max_tile_entries"],
                "tile_max_2_0": probe["capacity"]["2.0"]["max_tile_entries"],
                "overflow_tiles_at_32_1_0": probe["capacity"]["1.0"]["overflow_tiles_at_32"],
                "overflow_tiles_at_64_1_0": probe["capacity"]["1.0"]["overflow_tiles_at_64"],
                "overflow_tiles_at_32_2_0": probe["capacity"]["2.0"]["overflow_tiles_at_32"],
                "overflow_tiles_at_64_2_0": probe["capacity"]["2.0"]["overflow_tiles_at_64"],
            })
        medians = {
            "baseline_visibility_raster_ms": median([row["baseline_visibility_raster_ms"] for row in per_round]),
            "holes_visibility_raster_ms": median([row["holes_visibility_raster_ms"] for row in per_round]),
            "visibility_raster_delta_ms": median([row["visibility_raster_delta_ms"] for row in per_round]),
            "baseline_gpu_total_ms": median([row["baseline_gpu_total_ms"] for row in per_round]),
            "holes_gpu_total_ms": median([row["holes_gpu_total_ms"] for row in per_round]),
            "gpu_total_delta_ms": median([row["gpu_total_delta_ms"] for row in per_round]),
            "mesh_workgroups_removed": median([row["mesh_workgroups_removed"] for row in per_round]),
            "probe_software_clusters": median([row["probe_software_clusters"] for row in per_round]),
            "probe_classify_frame_us": median([row["probe_classify_frame_us"] for row in per_round]),
            "probe_classify_session_avg_us": median([row["probe_classify_session_avg_us"] for row in per_round]),
            "probe_bin_frame_us": median([row["probe_bin_frame_us"] for row in per_round]),
            "probe_bin_primary_us": median([row["probe_bin_primary_us"] for row in per_round if row["probe_bin_primary_us"] is not None]),
            "probe_bin_secondary_us": median([row["probe_bin_secondary_us"] for row in per_round if row["probe_bin_secondary_us"] is not None]),
        }
        medians["visibility_raster_delta_pct_of_m1c"] = 100.0 * medians["visibility_raster_delta_ms"] / reference["visibility_raster_ms"]
        medians["visibility_raster_delta_pct_of_measured"] = 100.0 * medians["visibility_raster_delta_ms"] / medians["baseline_visibility_raster_ms"]
        medians["software_cluster_share_pct"] = 100.0 * medians["probe_software_clusters"] / max(1, reference["clusters"])
        counts = {
            "clusters": max(statistics.median([probe["totals"]["clusters"] for probe in probe_by_round.values()]), 1),
            "software_clusters_1_0": medians["probe_software_clusters"],
            "software_clusters_2_0": statistics.median([probe["totals"]["software_clusters_2_0"] for probe in probe_by_round.values()]),
            "triangles": statistics.median([probe["totals"]["triangles"] for probe in probe_by_round.values()]),
            "software_triangles_1_0": statistics.median([probe["totals"]["software_triangles"] for probe in probe_by_round.values()]),
            "software_triangles_2_0": statistics.median([probe["totals"]["software_triangles_2_0"] for probe in probe_by_round.values()]),
        }
        shares = {
            "software_cluster_share_pct_1_0": 100.0 * counts["software_clusters_1_0"] / counts["clusters"],
            "software_cluster_share_pct_2_0": 100.0 * counts["software_clusters_2_0"] / counts["clusters"],
            "software_triangle_share_pct_1_0": 100.0 * counts["software_triangles_1_0"] / max(1, counts["triangles"]),
            "software_triangle_share_pct_2_0": 100.0 * counts["software_triangles_2_0"] / max(1, counts["triangles"]),
        }
        # explainable vs M1c's probe: the conservative criterion must classify FEWER clusters than the probe's centre
        # approximation (a bigger count would mean the criterion is not the conservative one the report claims)
        if shares["software_cluster_share_pct_1_0"] > reference["below_1px_pct"]:
            raise ValueError(f"{camera}: conservative classification share {shares['software_cluster_share_pct_1_0']:.2f}% "
                             f"exceeds M1c's probe {reference['below_1px_pct']}% (not a conservative criterion)")
        if shares["software_cluster_share_pct_2_0"] > reference["below_2px_pct"]:
            raise ValueError(f"{camera}: the 2.0 px conservative share exceeds M1c's probe")
        gate_ms = GO_FRACTION * reference["visibility_raster_ms"]
        go = medians["visibility_raster_delta_ms"] >= gate_ms
        if camera == "far":
            go_required = {"go": go, "gate_ms": gate_ms,
                           "delta_ms": medians["visibility_raster_delta_ms"],
                           "delta_pct_of_m1c": medians["visibility_raster_delta_pct_of_m1c"]}
        results["cameras"][camera] = {"rounds": per_round, "medians": medians, "counts": counts, "shares": shares,
                                      "gate_ms": gate_ms, "go": go}
    results["go"] = bool(go_required.get("go"))
    results["far_gate"] = go_required
    if not results["go"]:
        results["reasons"].append("far visibility-raster median delta is below 1.5% of M1c's far ON raster time")
    # ---- Q7: the tile entry capacity decision from the measured maxima, not from an assumption ----
    hole_probes = [validate_workload(Path(run["workload_json"]), run["camera"])
                   for run in data["runs"] if run["arm"] == "holes"]
    max_1_0 = max((probe["capacity"]["1.0"]["max_tile_entries"] for probe in hole_probes), default=0)
    max_2_0 = max((probe["capacity"]["2.0"]["max_tile_entries"] for probe in hole_probes), default=0)
    overflow_tiles_32_1_0 = max((probe["capacity"]["1.0"]["overflow_tiles_at_32"] for probe in hole_probes), default=0)
    overflow_tiles_64_1_0 = max((probe["capacity"]["1.0"]["overflow_tiles_at_64"] for probe in hole_probes), default=0)
    overflow_tiles_32_2_0 = max((probe["capacity"]["2.0"]["overflow_tiles_at_32"] for probe in hole_probes), default=0)
    overflow_tiles_64_2_0 = max((probe["capacity"]["2.0"]["overflow_tiles_at_64"] for probe in hole_probes), default=0)
    primary_overflow_frames_32 = sum(1 for probe in hole_probes if probe["capacity"]["1.0"]["overflow_tiles_at_32"] > 0)
    primary_overflow_frames_64 = sum(1 for probe in hole_probes if probe["capacity"]["1.0"]["overflow_tiles_at_64"] > 0)
    secondary_overflow_frames_32 = sum(1 for probe in hole_probes if probe["capacity"]["2.0"]["overflow_tiles_at_32"] > 0)
    secondary_overflow_frames_64 = sum(1 for probe in hole_probes if probe["capacity"]["2.0"]["overflow_tiles_at_64"] > 0)
    # Q7 says "32 or 64"; the measurement decides FROM THE DATA: the smallest power of two that holds the measured
    # per-tile maximum in the arm that matters (1.0 px primary; the 2.0 px band is the optional second configuration).
    needed = 1
    while needed < max_1_0:
        needed *= 2
    needed_secondary = 1
    while needed_secondary < max_2_0:
        needed_secondary *= 2
    if max_1_0 == 0:
        decision = 32
        reason = "no subpixel cluster was classified, so the design default 32 stands"
    elif needed <= 32:
        decision = 32
        reason = f"measured per-tile maximum at 1.0 px is {max_1_0} entries, which fits the design default 32"
    elif needed <= 64:
        decision = 64
        reason = f"measured per-tile maximum at 1.0 px is {max_1_0} entries, which exceeds 32 and fits 64"
    else:
        decision = needed
        reason = (f"measured per-tile maximum at 1.0 px is {max_1_0} entries: NEITHER 32 NOR 64 holds it (Q7's two "
                  f"candidates are both insufficient); the smallest power of two that does not overflow every frame is "
                  f"{needed}. The 2.0 px band would need {needed_secondary} (max {max_2_0}).")
    results["capacity_decision"] = {
        "max_tile_entries_1_0": max_1_0,
        "max_tile_entries_2_0": max_2_0,
        "overflow_tiles_at_32_1_0": overflow_tiles_32_1_0,
        "overflow_tiles_at_64_1_0": overflow_tiles_64_1_0,
        "overflow_tiles_at_32_2_0": overflow_tiles_32_2_0,
        "overflow_tiles_at_64_2_0": overflow_tiles_64_2_0,
        "primary_overflow_frames_at_32": primary_overflow_frames_32,
        "primary_overflow_frames_at_64": primary_overflow_frames_64,
        "secondary_overflow_frames_at_32": secondary_overflow_frames_32,
        "secondary_overflow_frames_at_64": secondary_overflow_frames_64,
        "decision": decision,
        "decision_2_0": needed_secondary,
        "q7_premise_holds": max(max_1_0, max_2_0) <= 64,
        "reason": reason,
    }
    return results


def report(results: dict) -> str:
    lines = ["# Task 1 software-raster workload upper bound (GO/NO-GO)", ""]
    lines.append(f"GO: **{'YES' if results['go'] else 'NO'}**  (far gate: {results['far_gate']})")
    lines.append("")
    lines.append("| camera | round | baseline raster ms | holes raster ms | delta ms | delta % M1c | GPU total delta ms | workgroups removed | probe sw clusters | tile max 1.0/2.0 |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|")
    for camera, payload in results["cameras"].items():
        for row in payload["rounds"]:
            lines.append(f"| {camera} | {row['round']} | {row['baseline_visibility_raster_ms']:.2f} | "
                         f"{row['holes_visibility_raster_ms']:.2f} | {row['visibility_raster_delta_ms']:.2f} | "
                         f"{100.0 * row['visibility_raster_delta_ms'] / results['m1c_reference'][camera]['visibility_raster_ms']:.2f}% | "
                         f"{row['gpu_total_delta_ms']:.2f} | {row['mesh_workgroups_removed']} | {row['probe_software_clusters']} | "
                         f"{row['tile_max_1_0']}/{row['tile_max_2_0']} |")
        medians = payload["medians"]
        lines.append(f"| **{camera} median** |  | {medians['baseline_visibility_raster_ms']:.2f} | "
                     f"{medians['holes_visibility_raster_ms']:.2f} | {medians['visibility_raster_delta_ms']:.2f} | "
                     f"{medians['visibility_raster_delta_pct_of_m1c']:.2f}% | {medians['gpu_total_delta_ms']:.2f} | "
                     f"{medians['mesh_workgroups_removed']} | {medians['probe_software_clusters']} |  |")
    lines.append("")
    for camera, payload in results["cameras"].items():
        medians, counts, shares = payload["medians"], payload["counts"], payload["shares"]
        lines.append(f"- **{camera}**: classify 1.0 px share {shares['software_cluster_share_pct_1_0']:.2f}% of clusters "
                     f"(M1c probe {results['m1c_reference'][camera]['below_1px_pct']}%), 2.0 px {shares['software_cluster_share_pct_2_0']:.2f}% "
                     f"(probe {results['m1c_reference'][camera]['below_2px_pct']}%); triangle shares {shares['software_triangle_share_pct_1_0']:.2f}%/"
                     f"{shares['software_triangle_share_pct_2_0']:.2f}% (probe {results['m1c_reference'][camera]['triangles_below_1px_pct']}%/"
                     f"{results['m1c_reference'][camera]['triangles_below_2px_pct']}%); CPU classify {medians['probe_classify_frame_us']:.1f} us work/frame "
                     f"(session avg {medians['probe_classify_session_avg_us']:.1f} us), capture-frame binning {medians['probe_bin_frame_us']:.1f} us (primary {medians['probe_bin_primary_us']:.1f} + secondary {medians['probe_bin_secondary_us']:.1f}; capture frame only, host copies).")
    capacity = results["capacity_decision"]
    lines.append("")
    lines.append(f"tile entry capacity (Q7): max entries 1.0 px = {capacity['max_tile_entries_1_0']}, 2.0 px = "
                 f"{capacity['max_tile_entries_2_0']}; overflow tiles@32/64 = {capacity['overflow_tiles_at_32_1_0']}/{capacity['overflow_tiles_at_64_1_0']} (1.0 px) -> **capacity {capacity['decision']}** ({capacity['reason']})")
    return "\n".join(lines)


# ====================================================================================================================
# SELF-TEST - the negative controls the gate must refuse (a green run on fabricated input would be worthless)
# ====================================================================================================================
def _synthetic_matrix(work: Path) -> dict:
    work.mkdir(parents=True, exist_ok=True)
    runs = []
    for camera in CAMERAS:
        for round_index in range(1, ROUNDS + 1):
            for arm in ("baseline", "holes"):
                run_dir = work / f"{camera}-{arm}-{round_index}"
                run_dir.mkdir(exist_ok=True)
                probe = validate_fixture(camera)
                workload = run_dir / "workload.json"
                workload.write_text(json.dumps(probe) + "\n", encoding="utf-8")
                log = run_dir / "debug.log"
                raster = 20.11 if camera == "far" else 18.58
                holes = arm == "holes"
                raster_time = raster - 1.0 if holes else raster
                workgroups = (41_030 if camera == "far" else 35_444) * (FRAMES - 1)
                triangles = (3_318_617 if camera == "far" else 2_860_109) * (FRAMES - 1)
                removal = probe["totals"]["software_clusters"] * (FRAMES - 1)
                triangle_removal = probe["totals"]["software_triangles"] * (FRAMES - 1)
                probe_lines = ""
                if holes:
                    probe_lines = ("software raster workload probe: ARMED by DEREN_SOFTWARE_RASTER_PROBE=workload\n"
                                   f"software raster workload probe: frame {FRAMES - 1}: items 1, clusters 1 -> '{workload}'\n")
                log.write_text(
                    f"capture mode: {FRAMES} frames, then screenshot + quit\n"
                    f"capture camera: {CAMERAS[camera]}\n"
                    f"gpu pass timings (session avg of {FRAMES - 1} frames): rt 0.00 ms | visibility raster {raster_time:.2f} ms | "
                    f"total {raster_time + 1.0:.2f} ms\n"
                    f"cpu frame phases (avg of 60 frames): pace 15.0 ms | scene 0.15 ms | post 5.0 ms | total 21.0 ms\n"
                    f"mesh culling (visibility lane): {workgroups - (removal if holes else 0)} meshlet workgroups, "
                    f"{workgroups - (removal if holes else 0)} emitted their triangles, 0 were culled (0%), "
                    f"{triangles - (triangle_removal if holes else 0)} triangles from meshlets\n"
                    f"subtilis: cut run publication: 50808 -> {41030 if camera == 'far' else 35444} records, "
                    f"4208625 -> {3318617 if camera == 'far' else 2860109} triangles, 0 fallback item(s)\n"
                    f"screenshot saved: {run_dir}/screenshot.png (1280x720)\n" + probe_lines,
                    encoding="utf-8")
                (run_dir / "screenshot.png").write_bytes(b"fake")
                parsed = parse_log(log.read_text(encoding="utf-8"))
                runs.append({"tag": run_dir.name, "camera": camera, "camera_pose": CAMERAS[camera], "arm": arm, "validation": False,
                             "frames": FRAMES, "round": round_index, "exit_code": 0, "log": str(log),
                             "image": str(run_dir / "screenshot.png"), "workload_json": str(workload),
                             "process_wall_seconds": 20.0, **parsed})
    return {"kind": "software-raster-roi", "version": 1, "rounds": ROUNDS, "frames": FRAMES, "extent": list(EXTENT),
            "runs": runs, "m1c_reference": M1C_REFERENCE, "go_fraction": GO_FRACTION}


def validate_fixture(camera: str) -> dict:
    reference = M1C_REFERENCE[camera]
    clusters = reference["clusters"]
    software = int(clusters * (0.10 if camera == "far" else 0.03))
    tiles_x = EXTENT[0] // 16
    tiles_y = EXTENT[1] // 16
    tile_count = tiles_x * tiles_y
    counts = [0] * tile_count
    for index in range(software):
        counts[index % tile_count] += 1
    counts_2 = [count * 2 for count in counts]
    return {
        "kind": "software-raster-workload", "version": 1,
        "frame": {"index": FRAMES - 1, "ring_slot": 0},
        "extent": {"width": EXTENT[0], "height": EXTENT[1]},
        "camera": {"pose": {"yaw": 35.0}, "view": [0.0] * 16, "proj": [0.0] * 16},
        "thresholds_px": [1, 2],
        "tiles": {"size": 16, "tiles_x": tiles_x, "tiles_y": tiles_y, "entry_capacity": 32},
        "totals": {"items": 2906, "classified_items": 2906, "clusters": clusters,
                   "triangles": reference["triangles"],
                   "hardware_clusters": clusters - software, "software_clusters": software,
                   "hardware_triangles": reference["triangles"] - software * 3, "software_triangles": software * 3,
                   "software_clusters_2_0": software * 2, "software_triangles_2_0": software * 6},
        "capacity": {"1.0": {"tile_count": tile_count, "entry_count": software, "max_tile_entries": max(counts),
                             "overflow_tiles_at_32": 0, "overflow_tiles_at_64": 0, "overflow_at_32": False,
                             "overflow_at_64": False, "band": "1.0"},
                     "2.0": {"tile_count": tile_count, "entry_count": software * 2, "max_tile_entries": max(counts_2),
                             "overflow_tiles_at_32": 0, "overflow_tiles_at_64": 0, "overflow_at_32": False,
                             "overflow_at_64": False, "band": "2.0"}},
        "cost_us": {"classify_frame": 120.0, "classify_session_avg": 110.0, "bin_frame": 30.0, "bin_items": 5.0, "bin_primary": 12.5, "bin_secondary": 12.5, "probe_frame_index": FRAMES - 1},
        "self_check": {"primary_split_matches_phase2": True},
        "tile_counts_1_0": counts, "tile_counts_2_0": counts_2,
        "items": [{"index": 0, "source": "visible", "selected": software, "hardware": 0, "software": software,
                   "hardware_2_0": 0, "software_2_0": software * 2, "triangles": software * 3, "hardware_triangles": 0,
                   "software_triangles": software * 3, "model": [0.0] * 16}],
    }


def self_test() -> None:
    import copy
    import tempfile
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        data = _synthetic_matrix(work)
        results = analyze(data)
        assert results["go"] is True, "valid synthetic far delta (1.0 ms) must be GO"
        assert results["capacity_decision"]["decision"] == 32

        files = [Path(run["log"]) for run in data["runs"]] + [Path(run["workload_json"]) for run in data["runs"]]
        snapshot = {path: path.read_bytes() for path in files}

        def restore() -> None:
            for path, payload in snapshot.items():
                path.write_bytes(payload)

        def rejected(case: str, mutate) -> None:
            bad = copy.deepcopy(data)
            mutate(bad)
            try:
                analyze(bad)
            except (ValueError, KeyError):
                return
            finally:
                restore()
            raise AssertionError(f"self-test: invalid input was accepted ({case})")

        rejected("missing round", lambda bad: bad["runs"].pop())
        rejected("duplicate round", lambda bad: bad["runs"].append(copy.deepcopy(bad["runs"][0])))
        rejected("failed capture", lambda bad: bad["runs"][0].__setitem__("exit_code", 1))
        rejected("short capture", lambda bad: bad["runs"][1].__setitem__("gpu_frames", 3))
        rejected("validation findings", lambda bad: bad["runs"][2].__setitem__("validation_clean", False))
        rejected("non-finite metric", lambda bad: bad["runs"][0].__setitem__("visibility_raster_ms", float("nan")))
        rejected("baseline armed the probe", lambda bad: bad["runs"][0].__setitem__("probe_armed", True))
        rejected("holes without probe armed", lambda bad: bad["runs"][1].__setitem__("probe_armed", False))
        rejected("holes without dump", lambda bad: bad["runs"][1].__setitem__("probe_dumped", False))
        # fabricated run JSON: the recorded metric no longer equals the raw log
        rejected("fabricated metric", lambda bad: bad["runs"][0].__setitem__("gpu_total_ms", 1.0))
        # a dump whose phase-2 self check failed, or whose partitioning contradicts itself
        def break_self_check(bad):
            path = Path(bad["runs"][1]["workload_json"])
            payload = json.loads(path.read_text(encoding="utf-8"))
            payload["self_check"]["primary_split_matches_phase2"] = False
            path.write_text(json.dumps(payload), encoding="utf-8")
        rejected("probe self-check failed", break_self_check)

        def break_totals(bad):
            path = Path(bad["runs"][1]["workload_json"])
            payload = json.loads(path.read_text(encoding="utf-8"))
            payload["totals"]["hardware_clusters"] += 1
            path.write_text(json.dumps(payload), encoding="utf-8")
        rejected("probe totals do not partition", break_totals)

        def break_explainability(bad):
            # the holes arm removed MORE workgroups than the probe's software suffix accounts for, with the raw log
            # re-synced so the fabricated-metric guard is not what rejects it
            for run in bad["runs"]:
                if run["arm"] == "holes":
                    run["mesh_workgroups"] += 100_000
                    log = Path(run["log"])
                    text = log.read_text(encoding="utf-8")
                    text = re.sub(r"mesh culling \(visibility lane\): \d+ meshlet workgroups",
                                  f"mesh culling (visibility lane): {run['mesh_workgroups']} meshlet workgroups", text)
                    log.write_text(text, encoding="utf-8")
        rejected("removed work not explainable by the probe", break_explainability)

        def not_go(bad):
            for run in bad["runs"]:
                if run["camera"] == "far" and run["arm"] == "holes":
                    run["visibility_raster_ms"] = round(run["visibility_raster_ms"] + 0.9, 2)
                    log = Path(run["log"])
                    text = log.read_text(encoding="utf-8")
                    text = re.sub(r"visibility raster [0-9.]+ ms", f"visibility raster {run['visibility_raster_ms']:.2f} ms", text)
                    log.write_text(text, encoding="utf-8")
        restore()
        no_go = copy.deepcopy(data)
        not_go(no_go)
        assert analyze(no_go)["go"] is False, "self-test: a sub-threshold delta must be NO-GO"
    print("software raster ROI analysis gate: valid matrix + 13 negative controls passed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--analyze", type=Path)
    parser.add_argument("--work", type=Path, help="capture directory (also where measurements.json lands)")
    parser.add_argument("--rounds", type=int, default=ROUNDS)
    parser.add_argument("--smoke", action="store_true", help="two launches only; not a measurement matrix")
    parser.add_argument("--exe", type=Path, default=EXE)
    parser.add_argument("--asset", type=Path, default=ASSET)
    parser.add_argument("--out", type=Path, help="write the analysis result JSON here")
    parser.add_argument("--md", type=Path, help="write the markdown summary here")
    arguments = parser.parse_args()
    if arguments.self_test:
        self_test()
        return 0
    if arguments.capture:
        if arguments.work is None:
            parser.error("--capture requires --work")
        if arguments.rounds < ROUNDS and not arguments.smoke:
            parser.error("formal measurements require at least three rounds")
        capture_matrix(arguments.exe, arguments.asset, arguments.work, arguments.rounds, arguments.smoke)
        return 0
    if arguments.analyze:
        data = json.loads(arguments.analyze.read_text(encoding="utf-8"))
        results = analyze(data)
        text = json.dumps(results, indent=2) + "\n"
        if arguments.out:
            arguments.out.write_text(text, encoding="utf-8")
        rendered = report(results)
        if arguments.md:
            arguments.md.write_text(rendered + "\n", encoding="utf-8")
        print(rendered)
        return 0 if results["go"] else 1
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
