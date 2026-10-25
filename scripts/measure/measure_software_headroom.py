"""E1+E2 measurement for the M4 software-raster headroom question (measurement only; default OFF untouched).

E1 (shipping-configuration headroom, textured Bistro compatibility asset, DAG+HZB build/cull/recovery on):
  hardware-only vs tiled software vs triangle software, 3 interleaved rounds, far/near at 1280x720, with the
  per-pass GPU decomposition the existing timing labels expose (CPU routing cost, per-pass ms, counts).

E2 (subpixel-share sweep): resolution 640x360 / 1280x720 / 1920x1080 at the same far/near poses, plus one farther
camera at 1280x720, each software OFF vs TRIANGLE software on, 3 interleaved rounds, 130 capture frames
(129 timed GPU samples), fixed executable/shader hashes, per-round data and the candidate-count metrics needed to
define a subpixel-share ratio from what the runtime actually logs.

Every row records the log-derived pass decomposition and counts, so no second capture is needed to analyze a run.
The raw run directories (config/log/screenshot) stay under the work dir and are archived by the caller.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

from measure_render_optimizations import parse_run_log, sha256, shader_bundle_sha256

CAMERAS = {"far": "35,18,140,0,-83.07,0",
           "near": "35,8,42,0,-88,0",
           # one farther pose: the far camera's yaw/pitch/target with a much larger orbit distance, so the same
           # scene covers fewer pixels and is expected to raise the subpixel share.
           "far_farther": "35,18,240,0,-83.07,0"}
RESOLUTIONS = {"640x360": (640, 360), "1280x720": (1280, 720), "1920x1080": (1920, 1080)}
MODES = ("off", "tiled", "triangles")


def make_config(path: Path, asset: Path, exe: Path, screenshot_dir: Path, width: int, height: int, software: bool) -> None:
    quote = lambda value: json.dumps(str(value).replace("\\", "/"))
    path.write_text(f'''model = {quote(asset)}
[paths]
shaders_dir = {quote(exe.parent / "shaders")}
screenshot_dir = {quote(screenshot_dir)}
[render]
window_width = {width}
window_height = {height}
vsync = false
max_fps = 0
validation_layers = false
main_render_graph = true
visibility_buffer = true
subtilis = true
subtilis_error_px = 1.0
subtilis_hysteresis = false
software_raster = {str(software).lower()}
software_raster_threshold_px = 1.0
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


def environment(mode: str) -> dict:
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("DEREN_"):
            env.pop(key, None)
    if os.name == "nt":
        env["PATH"] = "C:/msys64/clang64/bin;" + env.get("PATH", "")
    env.update(DEREN_HZB_BUILD="1", DEREN_HZB_CULL="1", DEREN_HZB_RECOVERY="1")
    if mode == "triangles":
        env["DEREN_SOFTWARE_RASTER_TRIANGLES"] = "1"
    return env


PASS_LABELS = ("rt", "shadow", "hzb cull", "visibility raster", "hzb build", "hzb recovery", "visibility recovery",
               "software visibility", "scene", "rt shadow", "software merge", "lighting", "gaussian prepare",
               "gaussian sort", "gaussian finalize", "gaussian raster", "transparent", "taa", "debug", "bloom",
               "composite", "fxaa", "tail")


def parse_pass_timings(text: str) -> dict:
    lines = re.findall(r"gpu pass timings \(session avg of (\d+) frames\):([^\n]+)", text)
    if not lines:
        return {}
    frames, body = lines[-1]
    passes = {"gpu_total_ms": 0.0, "gpu_frames": int(frames)}
    for label in PASS_LABELS:
        match = re.search(re.escape(label) + r"\s+([0-9.]+)\s+ms", body)
        if match:
            passes[label.replace(" ", "_") + "_ms"] = float(match.group(1))
    total = re.search(r"\btotal\s+([0-9.]+)\s+ms", body)
    if total:
        passes["gpu_total_ms"] = float(total.group(1))
    return passes


def parse_counts(text: str) -> dict:
    counts = {}
    software = re.findall(r"software raster: (\d+) frames; ([0-9.]+) software clusters/frame; ([0-9.]+) work entries/frame; max tile (\d+); budget (\d+)", text)
    if software:
        counts["software_frames"] = int(software[-1][0])
        counts["software_clusters_per_frame"] = float(software[-1][1])
        counts["software_entries_per_frame"] = float(software[-1][2])
        counts["software_max_tile_entries"] = int(software[-1][3])
        counts["software_entry_budget"] = int(software[-1][4])
    activation = re.findall(r"software raster active: threshold ([0-9.]+) px, (\d+) software cluster\(s\), (\d+) (?:CSR|work) entries", text)
    if activation:
        counts["software_threshold_px"] = float(activation[-1][0])
        counts["software_clusters_first_frame"] = int(activation[-1][1])
        counts["software_entries_first_frame"] = int(activation[-1][2])
    hzb = re.findall(r"hzb cull: session (\d+) candidate\(s\) / (\d+) culled / (\d+) survivor\(s\) \(([0-9.]+)% culled\) over (\d+) culling frame\(s\), ([0-9.]+) candidate\(s\)/frame", text)
    if hzb:
        counts["hzb_candidates"] = int(hzb[-1][0])
        counts["hzb_culled"] = int(hzb[-1][1])
        counts["hzb_survivors"] = int(hzb[-1][2])
        counts["hzb_culled_percent"] = float(hzb[-1][3])
        counts["hzb_culling_frames"] = int(hzb[-1][4])
        counts["hzb_candidates_per_culling_frame"] = float(hzb[-1][5])
        if counts["hzb_culling_frames"]:
            counts["hzb_survivors_per_culling_frame"] = counts["hzb_survivors"] / counts["hzb_culling_frames"]
    recovery = re.findall(r"hzb recovery: session (\d+) retested / (\d+) recovered / (\d+) still occluded", text)
    if recovery:
        counts["hzb_retested"] = int(recovery[-1][0])
        counts["hzb_recovered"] = int(recovery[-1][1])
        counts["hzb_still_occluded"] = int(recovery[-1][2])
    return counts


def marker_check(mode: str, text: str) -> list[str]:
    """The per-mode activation markers, so a row cannot silently measure the wrong lane."""
    failures = []
    if mode == "off":
        # The OFF row is expected NOT to activate the lane; activation is checked in run_one.
        return ["software lane active in an OFF run"] if "software raster active:" in text else []
    if "software raster active:" not in text:
        return ["the software lane never logged an activation line"]
    triangle_marker = "software triangle prototype: recorded clear/raster/unpack" in text
    if mode == "triangles" and not triangle_marker:
        failures.append("triangle producer requested but not recorded")
    if mode == "tiled" and triangle_marker:
        failures.append("tiled mode unexpectedly recorded the triangle producer")
    return failures


def run_one(exe: Path, asset: Path, work: Path, key: str, resolution: str, camera: str, mode: str, round_id: int,
            frames: int) -> dict:
    width, height = RESOLUTIONS[resolution]
    tag = f"{key}-{mode}-r{round_id}"
    run_dir = work / key / mode / f"r{round_id}"
    run_dir.mkdir(parents=True, exist_ok=False)
    config = run_dir / "config.toml"
    make_config(config, asset, exe, run_dir, width, height, mode != "off")
    command = [str(exe), "--config", str(config), "--capture-frames", str(frames), f"--capture-camera={CAMERAS[camera]}"]
    started = time.perf_counter()
    completed = subprocess.run(command, cwd=run_dir, env=environment(mode), stdout=(run_dir / "stdout.log").open("wb"),
                               stderr=subprocess.STDOUT, timeout=900, check=False,
                               creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    elapsed = time.perf_counter() - started
    log_path = run_dir / "deren-debug.log"
    text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
    images = sorted(run_dir.glob("screenshot_*.png"))
    metrics = parse_run_log(text)
    passes = parse_pass_timings(text)
    counts = parse_counts(text)
    row = {"tag": tag, "key": key, "resolution": resolution, "width": width, "height": height, "camera": camera,
           "mode": mode, "round": round_id, "frames": frames, "exit_code": completed.returncode,
           "process_wall_seconds": elapsed, "config": str(config), "log": str(log_path),
           "image": str(images[0]) if images else "", "image_sha256": sha256(images[0]) if images else "",
           **{key_name: value for key_name, value in metrics.items() if key_name not in ("software_counts",)},
           **passes, **counts}
    failures = []
    if completed.returncode != 0:
        failures.append(f"exit code {completed.returncode}")
    if len(images) != 1:
        failures.append(f"{len(images)} screenshots (expected one)")
    if metrics.get("gpu_frames") != frames - 1:
        failures.append(f"gpu frames {metrics.get('gpu_frames')} (expected {frames - 1})")
    if not row.get("hzb_cull_active"):
        failures.append("HZB cull lane was not active")
    if mode == "off" and row.get("software_raster_active"):
        failures.append("software lane active in an OFF run")
    if mode != "off" and not row.get("software_raster_active"):
        failures.append("software lane inactive in a software run")
    if not row.get("hzb_culled_percent"):
        failures.append("missing HZB cull statistics")
    if mode != "off" and not row.get("software_clusters_per_frame"):
        failures.append("missing per-frame software cluster statistics")
    failures.extend(marker_check(mode, text))
    row["failures"] = failures
    row["passed"] = not failures
    (run_dir / "run.json").write_text(json.dumps(row, indent=2) + "\n", encoding="utf-8")
    print(f"{tag}: GPU={row.get('gpu_ms')}ms frames={row.get('gpu_frames')} "
          f"software_clusters={row.get('software_clusters_per_frame')} hzb_cull={row.get('hzb_culled_percent')}% "
          f"pass={'PASS' if row['passed'] else 'FAIL ' + str(failures)}", flush=True)
    return row


def matrix_plan(rounds: int, only: str | None) -> list[tuple[int, str, str, str, str]]:
    """(round, key, resolution, camera, mode) in a rotated interleaved order - the order is the anti-bias device."""
    configs = []
    for resolution in ("640x360", "1280x720", "1920x1080"):
        for camera in ("far", "near"):
            modes = MODES if resolution == "1280x720" else ("off", "triangles")
            configs.append((f"{resolution}-{camera}", resolution, camera, modes))
    configs.append(("1280x720-far_farther", "1280x720", "far_farther", ("off", "triangles")))
    if only:
        configs = [config for config in configs if config[0].startswith(only)]
        if not configs:
            raise ValueError(f"--only matched no configuration: {only}")
    plan = []
    for round_id in range(1, rounds + 1):
        rotated = configs[round_id - 1:] + configs[:round_id - 1]
        for config_key, resolution, camera, modes in rotated:
            for mode in modes[round_id - 1:] + modes[:round_id - 1]:
                plan.append((round_id, config_key, resolution, camera, mode))
    return plan


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--work", type=Path)
    parser.add_argument("--asset", type=Path)
    parser.add_argument("--exe", type=Path, default=Path("build-release-dyn-clang64/deren.exe"))
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--frames", type=int, default=130)
    parser.add_argument("--only", default=None, help="run only configurations whose key starts with this prefix (smoke)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        # The plan shape and the per-mode marker checks are the two things a later edit can silently break.
        plan = matrix_plan(3, None)
        assert len(plan) == 48, len(plan)
        assert sum(1 for row in plan if row[4] == "tiled") == 6
        assert len({(row[0], row[1], row[4]) for row in plan}) == 48, "round/config/mode cells must be unique"
        assert {row[0] for row in plan} == {1, 2, 3}
        assert len(matrix_plan(3, "1920x1080")) == 12
        # every round rotates the configuration order: the first configuration of each round differs.
        assert len({matrix_plan(3, None)[index][1] for index in (0, 16, 32)}) == 3
        assert not marker_check("triangles", "software raster active: threshold 1 px, 4 software cluster(s)\n"
                                              "software triangle prototype: recorded clear/raster/unpack\n")
        assert marker_check("triangles", "software raster active: threshold 1 px, 4 software cluster(s)\n")
        assert marker_check("tiled", "software raster active: threshold 1 px, 4 software cluster(s)\n"
                                      "software triangle prototype: recorded clear/raster/unpack\n")
        config = Path(__file__).resolve().parents[2] / "tmp" / "headroom-config-self-test.toml"
        config.parent.mkdir(exist_ok=True)
        make_config(config, Path("asset.gltf"), Path("build/deren.exe"), Path("shots"), 640, 360, True)
        text = config.read_text(encoding="utf-8")
        assert "window_width = 640" in text and "window_height = 360" in text and "software_raster = true" in text
        config.unlink()
        sample = ("gpu pass timings (session avg of 129 frames): visibility raster 7.11 ms | software visibility 0.11 ms | "
                  "software merge 1.66 ms | hzb cull 0.07 ms | hzb build 1.32 ms | scene 5.04 ms | total 9.48 ms\n"
                  "software raster: 120 frames; 7146.5 software clusters/frame; 7146.5 work entries/frame; max tile 1; budget 65536\n"
                  "hzb cull: session 628979 candidate(s) / 451811 culled / 177168 survivor(s) (71.8% culled) over 15 culling frame(s), 41931.9 candidate(s)/frame\n")
        passes = parse_pass_timings(sample)
        assert passes["visibility_raster_ms"] == 7.11 and passes["scene_ms"] == 5.04 and passes["gpu_total_ms"] == 9.48
        counts = parse_counts(sample)
        assert counts["software_clusters_per_frame"] == 7146.5
        assert counts["hzb_survivors_per_culling_frame"] == 177168 / 15
        print("software-raster headroom measurement: plan/markers/config/parser self-test PASS")
        return 0
    if args.work is None or args.asset is None:
        parser.error("--work and --asset are required unless --self-test is used")
    work, asset, exe = args.work.resolve(), args.asset.resolve(), args.exe.resolve()
    for name, path in (("asset", asset), ("executable", exe)):
        if not path.is_file():
            parser.error(f"{name} does not exist: {path}")
    if args.rounds < 1:
        parser.error("rounds must be positive")
    work.mkdir(parents=True, exist_ok=False)
    result = {"complete": False, "rounds": args.rounds, "frames": args.frames,
              "executable_sha256": sha256(exe), "asset_sha256": sha256(asset),
              "shader_bundle_sha256": shader_bundle_sha256(exe.parent / "shaders"), "runs": []}
    output = work / "results.json"
    for round_id, config_key, resolution, camera, mode in matrix_plan(args.rounds, args.only):
        row = run_one(exe, asset, work, config_key, resolution, camera, mode, round_id, args.frames)
        result["runs"].append(row)
        output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if sha256(exe) != result["executable_sha256"] or shader_bundle_sha256(exe.parent / "shaders") != result["shader_bundle_sha256"]:
        raise SystemExit("executable or shader bundle changed during measurement")
    failures = [row["tag"] for row in result["runs"] if not row["passed"]]
    if failures:
        raise SystemExit(f"failed runs: {failures}")
    result["complete"] = True
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"complete": True, "runs": len(result["runs"])}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"software raster headroom measurement: FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
