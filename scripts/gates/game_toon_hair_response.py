"""Probe captured hair punctual-light material inputs on the GPU.

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
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = (root / args.build_dir).resolve()
    cache = (build / "CMakeCache.txt").read_text()
    compiler = re.search(r"^VR_SLANGC_EXECUTABLE:FILEPATH=(.+)$", cache, re.M)[1]
    work = Path(tempfile.mkdtemp(prefix="hair-response-", dir=root / "tmp"))
    env = os.environ.copy()
    env["PATH"] = "C:/msys64/clang64/bin;" + env["PATH"]
    results = []
    cases = ("punctual", "anisotropic-wrap", "anisotropic-specular")
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
        body = replace_once(body, needle, needle + "\n    if (s.toon_family != VR_FAMILY_HAIR) return float4(0,0,0,1);")
        text = prefix + "float4 game_toon_frag_main" + body
        main_shader.write_text(text)
        hair = tree / "toon/game_toon/captured/hair.slang"
        text = hair.read_text()
        needle = "const float p_map_occlusion = has_p_map ? p_map.b : 1.0;"
        text = replace_once(text, "const float4 p_map = has_p_map", "float4 p_map = has_p_map")
        text = replace_once(text, needle, "p_map = float4(0.5, 0.25, 1.0, 0.8);\n    " + needle)
        text = replace_once(text, needle, "s.albedo = float3(0.4, 0.2, 0.1);\n    " + needle)
        # The surface sample is a value parameter, so the synthetic albedo stays local.
        header = tree / "toon/game_toon/captured/game_toon_lights.h"
        light_text = header.read_text()
        needle = "const int count = clamp(int(light_at(heap_light_slot).light_count.x), 0, MAX_PUNCTUAL_LIGHTS);"
        light_text = replace_once(light_text, needle, """
    bool correct = all(abs(F0 - float3(0.01)) < 0.00001)
        && all(abs(diffuse - float3(0.384,0.192,0.096)) < 0.00001)
        && all(abs(rim_base - float3(0.4,0.2,0.1)) < 0.00001)
        && hair_material.w == 1.0 && abs(hair_material.x - 0.25) < 0.00001;
    return correct ? float3(0.1,0.5,0.1) : float3(0.5,0.1,0.1);
    """ + needle)
        if case.startswith("anisotropic"):
            light_text = header.read_text()
            light_text = replace_once(light_text, "const uint base = 352u + index * 64u;", """
    PunctualLight fixture;
    fixture.position = float4(0,0,1,0);
    fixture.color = float4(1,1,1,0);
    fixture.spot_dir = float4(0);
    fixture.params = float4(0,0,1,0);
    return fixture;
    const uint base = 352u + index * 64u;""")
            if case == "anisotropic-specular":
                light_text = light_text.replace("fixture.color = float4(1,1,1,0);", "fixture.color = float4(1,1,1,2);")
            light_text = replace_once(light_text, needle, "const int count = 1;")
            # Independent scalar expectations from PS 37900 DXBC 2042-2069.
            # No ramp: sin(theta)^200 * P.g * fade * F0 * strength * 5 * line gain.
            probe = """
    bool correct = true;
    for (int probe = 0; probe < 8; ++probe) {
        float3 axis = probe == 1 ? float3(0,0,1) : float3(1,0,0);
        if (probe == 7) axis = float3(0.9949874371, 0, 0.1);
        float fade = probe == 2 ? 0.0 : 0.5;
        float reflectivity = probe == 3 ? 0.0 : 0.25;
        float strength = probe == 4 ? 0.0 : 2.0;
        float gain = probe == 5 ? 0.0 : 0.75;
        float3 normal = probe == 6 ? float3(0,0,-1) : float3(0,0,1);
        float3 actual = game_toon_add_lights(float3(0), normal, float3(0,0,1),
            float3(0), float3(0), float3(0.01), 0.2, float3(0),
            float4(axis, fade), float4(reflectivity, strength, gain, 1), 0u);
        float expected = probe == 0 ? 0.009375 : (probe == 7 ? 0.003431553199 : 0.0);
        correct = correct && all(abs(actual - expected) < 0.00001);
    }
    float3 non_hair = game_toon_add_lights(float3(0), float3(0,0,1), float3(0,0,1),
        float3(0), float3(0), float3(0.01), 0.2, float3(0));
    correct = correct && all(abs(non_hair - NON_HAIR_EXPECTED) < 0.00001);
    return correct ? float3(0.1,0.5,0.1) : float3(0.5,0.1,0.1);
"""
            probe = probe.replace("NON_HAIR_EXPECTED", "0.0" if case == "anisotropic-specular" else "0.2")
            text = replace_once(text, "const bool has_p_map = toon_lanes.y != 0u;", probe + "\n    const bool has_p_map = toon_lanes.y != 0u;")
        header.write_text(light_text)
        hair.write_text(text)
        command = [compiler, str(main_shader), "-I", str(tree), "-target", "spirv",
                   "-profile", "spirv_1_6", "-capability", "spvDescriptorHeapEXT",
                   "-spirv-resource-heap-stride", "64", "-spirv-sampler-heap-stride", "32",
                   "-fvk-use-gl-layout", "-matrix-layout-column-major", "-entry", "game_toon_frag_main",
                   "-stage", "fragment", "-o", str(outputs / "game_toon.frag.spv")]
        compile_result = subprocess.run(command, capture_output=True, text=True)
        if compile_result.returncode:
            print(compile_result.stdout + compile_result.stderr)
        compile_result.check_returncode()
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
                            passed=green > 1000 and red < 1000))
        print(json.dumps(results[-1]), flush=True)
    (work / "results.json").write_text(json.dumps(results, indent=2))
    assert all(result["passed"] for result in results), f"Hair response mismatch; artifacts: {work}"


if __name__ == "__main__":
    main()
