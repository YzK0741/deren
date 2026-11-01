"""Render known HDR colours through the real GameToon post chain.

The expected linear values were independently evaluated from the captured
viewer-tonemapping.glsl. Private shader/config copies leave the installed app intact.
Requires the Laevatain preview, Pillow, Vulkan and the configured Slang compiler.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from PIL import Image


def srgb_byte(value):
    encoded = 12.92 * value if value <= .0031308 else 1.055 * value ** (1 / 2.4) - .055
    return round(255 * encoded)


def main():
    root = Path(__file__).resolve().parents[2]
    build = root / "build-release-dyn-clang64"
    work = Path(tempfile.mkdtemp(prefix="game-toon-tonemap-", dir=root / "tmp"))
    shaders = work / "shaders"
    shutil.copytree(build / "shaders", shaders)
    compiler = re.search(r"^VR_SLANGC_EXECUTABLE:FILEPATH=(.+)$", (build / "CMakeCache.txt").read_text(), re.M)[1]
    env = os.environ.copy()
    env["PATH"] = "C:/msys64/clang64/bin;" + env["PATH"]
    samples = [((.18, .18, .18), (.0976206393726227, .0975970300286416, .0975643403088948)),
               ((.8, .3, .1), (.598515905216385, .215432514121701, .0377456197815041))]
    results = []
    for index, (hdr, expected_linear) in enumerate(samples):
        capture = work / str(index)
        capture.mkdir()
        source = capture / "constant.slang"
        source.write_text('[shader("fragment")] float4 main():SV_Target { return float4(' +
                          ','.join(map(str, hdr)) + ',1); }')
        subprocess.run([compiler, str(source), "-target", "spirv", "-profile", "spirv_1_6",
                        "-entry", "main", "-stage", "fragment", "-o", str(shaders / "game_toon.frag.spv")],
                       check=True, capture_output=True)
        config = (build / "config.toml").read_text().replace("show=true", "show=false")
        config = re.sub(r'screenshot_dir="[^"]*"', f'screenshot_dir="{capture.as_posix()}"', config)
        config = re.sub(r'shaders_dir="[^"]*"', f'shaders_dir="{shaders.as_posix()}"', config)
        config = re.sub(r'exposure=[^\r\n]+', 'exposure=1.0', config)
        config = config.replace("bloom=true", "bloom=false").replace("fxaa=true", "fxaa=false")
        path = capture / "config.toml"
        path.write_text(config)
        subprocess.run([str(build / "deren.exe"), "--config", str(path), "--capture-frames", "5",
                        "--capture-camera=0,0,1.8,0,-.98,0"], cwd=build, env=env,
                       check=True, capture_output=True, timeout=120)
        picture = next(capture.glob("screenshot_*.png"))
        with Image.open(picture).convert("RGB") as im:
            actual = im.getpixel((im.width // 2, im.height // 2))
        expected = tuple(map(srgb_byte, expected_linear))
        error = max(abs(a - b) for a, b in zip(actual, expected))
        result = dict(hdr=hdr, expected=expected, actual=actual, max_byte_error=error, picture=str(picture))
        results.append(result)
        (work / "results.json").write_text(json.dumps(results, indent=2))
        print(result, flush=True)
        assert error <= 2, "GameToon mode 2 did not execute the captured tone map exactly once"


if __name__ == "__main__":
    main()
