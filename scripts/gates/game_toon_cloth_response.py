"""Probe captured cloth diffuse energy and punctual-light material inputs on the GPU.

Uses a private shader/config tree and synthetic material texels. Requires the
Laevatain preview asset, Pillow, Vulkan, and a configured build directory.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from PIL import Image


def replace_once(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--case", choices=("diffuse", "punctual", "all"), default="all")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = (root / args.build_dir).resolve()
    cache = (build / "CMakeCache.txt").read_text()
    compiler = re.search(r"^VR_SLANGC_EXECUTABLE:FILEPATH=(.+)$", cache, re.M)[1]
    work = Path(tempfile.mkdtemp(prefix="cloth-response-", dir=root / "tmp"))
    env = os.environ.copy()
    env["PATH"] = "C:/msys64/clang64/bin;" + env["PATH"]
    results = []
    cases = ("diffuse", "punctual") if args.case == "all" else (args.case,)
    for case in cases:
        case_dir = work / case
        case_dir.mkdir()
        tree = case_dir / "source"
        outputs = case_dir / "shaders"
        shutil.copytree(root / "source/shaders", tree)
        shutil.copytree(build / "shaders", outputs)
        main_shader = tree / "toon/game_toon.slang"
        text = main_shader.read_text()
        needle = "const surface_sample s = gather_surface(v_world_pos, v_normal, v_uv, v_is_front);"
        prefix, body = text.split("float4 game_toon_frag_main", 1)
        body = replace_once(body, needle, needle + "\n    if (s.toon_family != VR_FAMILY_CLOTH) return float4(0,0,0,1);")
        text = prefix + "float4 game_toon_frag_main" + body
        main_shader.write_text(text)
        cloth = tree / "toon/game_toon/captured/cloth.slang"
        text = cloth.read_text()
        text = replace_once(text, "float3 base = s.albedo;", "float3 base = float3(0.4, 0.2, 0.1);")
        texel = "float4(1.0, 0.25, 1.0, 0.8)" if case == "diffuse" else "float4(0.5, 0.25, 1.0, 0.8)"
        text = replace_once(text, "float3 shadow_colour;", f"metallic_gloss = {texel};\n    float3 shadow_colour;")
        if case == "diffuse":
            text = replace_once(text, "const float3 pre_light_colour = colour;", """
    return all(abs(colour) < 0.00001) ? float3(0.1,0.5,0.1) : float3(0.5,0.1,0.1);
    const float3 pre_light_colour = colour;""")
        else:
            header = tree / "toon/game_toon/captured/game_toon_lights.h"
            light_text = header.read_text()
            needle = "const int count = clamp(int(light_at(heap_light_slot).light_count.x), 0, MAX_PUNCTUAL_LIGHTS);"
            light_text = replace_once(light_text, needle, """
    bool correct = all(abs(F0 - float3(0.205,0.105,0.055)) < 0.00001)
        && abs(roughness - 0.2) < 0.00001;
    return correct ? float3(0.1,0.5,0.1) : float3(0.5,0.1,0.1);
    """ + needle)
            header.write_text(light_text)
        cloth.write_text(text)
        command = [compiler, str(main_shader), "-I", str(tree), "-target", "spirv",
                   "-profile", "spirv_1_6", "-capability", "spvDescriptorHeapEXT",
                   "-spirv-resource-heap-stride", "64", "-spirv-sampler-heap-stride", "32",
                   "-fvk-use-gl-layout", "-matrix-layout-column-major", "-entry", "game_toon_frag_main",
                   "-stage", "fragment", "-o", str(outputs / "game_toon.frag.spv")]
        subprocess.run(command, check=True, capture_output=True)
        discard = case_dir / "discard.slang"
        discard.write_text('[shader("fragment")] float4 main():SV_Target { discard; return float4(0); }')
        for filename in ("outline.frag.spv", "overlay.frag.spv"):
            subprocess.run([compiler, str(discard), "-target", "spirv", "-profile", "spirv_1_6",
                            "-entry", "main", "-stage", "fragment", "-o", str(outputs / filename)],
                           check=True, capture_output=True)
        config = (root / "config.toml").read_text()
        overrides = {"model": (root / "asset/laevatain/laevatain_game_toon_preview.glb").as_posix(),
                     "shaders_dir": outputs.as_posix(), "screenshot_dir": case_dir.as_posix(),
                     "environment_hdr": ""}
        for key, value in overrides.items():
            config, count = re.subn(rf'(?m)^{key}\s*=.*$', f'{key}="{value}"', config)
            assert count == 1, key
        for key, value in {"show": "false", "taa": "false", "fxaa": "false",
                           "window_width": "600", "window_height": "600"}.items():
            config = re.sub(rf'(?m)^{key}\s*=.*$', f"{key}={value}", config)
        cfg = case_dir / "config.toml"
        cfg.write_text(config)
        run = subprocess.run([str(build / "deren.exe"), "--config", str(cfg), "--capture-frames", "4"],
                             cwd=build, env=env, capture_output=True, timeout=120)
        (case_dir / "run.log").write_bytes(run.stdout + run.stderr)
        run.check_returncode()
        picture = next(case_dir.glob("screenshot_*.png"))
        with Image.open(picture).convert("RGB") as im:
            pixels = list(getattr(im, "get_flattened_data", im.getdata)())
        green = sum(g > r + 30 and g > b + 30 for r, g, b in pixels)
        red = sum(r > g + 30 and r > b + 30 for r, g, b in pixels)
        results.append(dict(case=case, matching=green, mismatching=red, picture=str(picture),
                            passed=green > 10000 and red < 1000))
        print(json.dumps(results[-1]), flush=True)
    (work / "results.json").write_text(json.dumps(results, indent=2))
    assert all(result["passed"] for result in results), f"Cloth response mismatch; artifacts: {work}"


if __name__ == "__main__":
    main()
