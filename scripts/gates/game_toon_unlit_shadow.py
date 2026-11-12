"""GPU regression for unlit/toon shadow production and isolated toon/TAA timings.

Requires a built application, the Laevatain preview asset, and Vulkan timestamps.
The camera moves each frame so a cached static shadow cannot satisfy the test.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build")
    cases = ("unlit-toon", "pure-unlit", "toon-taa-off", "shadow-disabled")
    parser.add_argument("--case", choices=("all", *cases), default="all")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = (root / args.build_dir).resolve()
    work = Path(tempfile.mkdtemp(prefix="unlit-shadow-", dir=root / "tmp"))
    env = os.environ.copy()
    env["PATH"] = "C:/msys64/clang64/bin;" + env["PATH"]
    results = []
    for case in cases if args.case == "all" else (args.case,):
        folder = work / case
        folder.mkdir()
        toon = case != "pure-unlit"
        unlit = case != "toon-taa-off"
        shadow = case != "shadow-disabled"
        taa = case != "toon-taa-off"
        config = f'''model="{(root / 'asset/laevatain/laevatain_game_toon_preview.glb').as_posix()}"
[paths]
shaders_dir="{(build / 'shaders').as_posix()}"
screenshot_dir="{folder.as_posix()}"
[render]
camera_pose=[-30,0,0.65,0,0.03,-0.85]
window_width=600
window_height=600
vsync=false
max_fps=0
shadow={str(shadow).lower()}
toon_shadow_softness=2
validation_layers=true
unlit={str(unlit).lower()}
character_forward={str(toon).lower()}
game_toon={str(toon).lower()}
taa={str(taa).lower()}
fxaa=true
[gui]
show=false
[lighting]
sun_direction=[0,0.25,1]
environment_hdr=""
env_size=128
env_mip_count=8
irr_size=16
lut_size=64
'''
        path = folder / "config.toml"
        path.write_text(config)
        run = subprocess.run([str(build / "deren.exe"), "--config", str(path),
                              "--capture-frames", "90", "--capture-sweep", "0.75"],
                             cwd=build, env=env, capture_output=True, timeout=180)
        (folder / "run.log").write_bytes(run.stdout + run.stderr)
        run.check_returncode()
        shutil.copyfile(build / "deren-debug.log", folder / "deren-debug.log")
        log = (folder / "deren-debug.log").read_text(errors="replace")
        report = next(line for line in reversed(log.splitlines())
                      if line.startswith("gpu pass timings (session avg"))
        metrics = {name.strip(): float(value) for name, value in
                   re.findall(r"(?:^|\|)\s*([^|:]+?)\s+([0-9.]+) ms", report.split(": ", 1)[1])}
        labels = ("toon surface", "toon outline", "toon eye", "toon overlay", "toon rim", "taa")
        correct_labels = all(label in metrics for label in labels)
        expected_shadow = toon and shadow
        correct_shadow = metrics.get("shadow", 0) > 0.01 if expected_shadow else metrics.get("shadow", 0) <= 0.01
        inactive = labels[:-1] if not toon else (("taa",) if not taa else ())
        correct_skips = all(metrics.get(label, 0) < 0.25 for label in inactive)
        passed = correct_labels and correct_shadow and correct_skips and "VUID-" not in log
        result = dict(case=case, passed=passed, metrics=metrics, log=str(folder / "deren-debug.log"))
        results.append(result)
        print(json.dumps(result), flush=True)
    (work / "results.json").write_text(json.dumps(results, indent=2))
    assert all(result["passed"] for result in results), f"Unlit shadow/timing regression; artifacts: {work}"


if __name__ == "__main__":
    main()
