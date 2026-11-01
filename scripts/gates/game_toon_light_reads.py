"""GPU regression for the captured Laevatain rig's first punctual light.

Uses a private shader tree and restores the installed shader even if a capture fails.
Requires the preview asset, Pillow, Vulkan, and the configured Slang compiler.
Run with the application closed: python scripts/gates/game_toon_light_reads.py
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from PIL import Image


def main():
    root = Path(__file__).resolve().parents[2]
    build = root / "build-release-dyn-clang64"
    cache = (build / "CMakeCache.txt").read_text()
    compiler = re.search(r"^VR_SLANGC_EXECUTABLE:FILEPATH=(.+)$", cache, re.M)[1]
    output = build / "shaders/game_toon.frag.spv"
    backup = output.read_bytes()
    work = Path(tempfile.mkdtemp(prefix="light-read-", dir=root / "tmp"))
    tree = work / "shaders"
    shutil.copytree(root / "source/shaders", tree)
    header = tree / "toon/game_toon/captured/game_toon_lights.h"
    text = header.read_text()
    line = re.search(r"const PunctualLight pl = [^;]+;", text)[0]
    # Compare GPU values with the app's captured rig, including the unused NPR lanes.
    probe = """
        bool correct = all(abs(pl.position - float4(1.135, .942, .138, .194)) < .0001)
            && all(abs(pl.color - float4(13.5108, 16.8156, 18, 3)) < .0001)
            && all(abs(pl.spot_dir - float4(normalize(float3(-.8698, 0, -.4935)), .368)) < .0001)
            && all(abs(pl.params - float4(1.15, 1, cos(71.6229*.00872664626), cos(62.2358*.00872664626))) < .0001);
        return correct ? float3(.1, .5, .1) : float3(.5, .1, .1);
"""
    header.write_text(text.replace(line, line + probe))
    command = [compiler, str(tree / "toon/game_toon.slang"), "-I", str(tree),
               "-target", "spirv", "-profile", "spirv_1_6", "-capability", "spvDescriptorHeapEXT",
               "-spirv-resource-heap-stride", "64", "-spirv-sampler-heap-stride", "32",
               "-fvk-use-gl-layout", "-matrix-layout-column-major", "-entry", "game_toon_frag_main",
               "-stage", "fragment", "-o", str(output)]
    env = os.environ.copy()
    env["PATH"] = "C:/msys64/clang64/bin;" + env["PATH"]
    try:
        subprocess.run(command, check=True, capture_output=True)
        for frames in (4, 5):
            capture = work / str(frames)
            capture.mkdir()
            config = (build / "config.toml").read_text()
            config = re.sub(r'screenshot_dir="[^"]*"', f'screenshot_dir="{capture.as_posix()}"', config)
            config = config.replace("show=true", "show=false")
            config_path = capture / "config.toml"
            config_path.write_text(config)
            subprocess.run([str(build / "deren.exe"), "--config", str(config_path),
                            "--capture-frames", str(frames), "--capture-camera=0,0,1.8,0,-.98,0"],
                           cwd=build, env=env, check=True, capture_output=True, timeout=120)
            picture = next(capture.glob("screenshot_*.png"))
            with Image.open(picture).convert("RGB") as im:
                pixels = list(getattr(im, "get_flattened_data", im.getdata)())
                green = sum(g > r + 30 and g > b + 30 for r, g, b in pixels)
                red = sum(r > g + 30 and r > b + 30 for r, g, b in pixels)
            print(f"frames={frames}: matching pixels={green}, mismatching pixels={red}; {picture}", flush=True)
            assert green > 100000 and red < 10000, "GPU punctual light fields differ from the captured rig"
    finally:
        output.write_bytes(backup)


if __name__ == "__main__":
    main()
