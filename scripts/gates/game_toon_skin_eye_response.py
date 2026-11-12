"""Probe captured skin roughness/rim inputs and eye MatCap/local lighting on the GPU.

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
    work = Path(tempfile.mkdtemp(prefix="skin-eye-response-", dir=root / "tmp"))
    env = os.environ.copy()
    env["PATH"] = "C:/msys64/clang64/bin;" + env["PATH"]
    results = []
    cases = ("skin", "skin-fallback", "matcap", "eye-light", "eye-modes")
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
        family = "(s.toon_family == VR_FAMILY_FACE || s.toon_family == VR_FAMILY_SKIN)" if case.startswith("skin") else "(s.toon_family == VR_FAMILY_EYE)"
        body = replace_once(body, needle, needle + f"\n    if (!{family}) return float4(0,0,0,1);")
        text = prefix + "float4 game_toon_frag_main" + body
        main_shader.write_text(text)
        header = tree / "toon/game_toon/captured/game_toon_lights.h"
        light_text = header.read_text()
        if case.startswith("skin"):
            target = tree / "toon/game_toon/captured/skin_face.slang"
            text = target.read_text().replace("const float3 base_rgb = s.albedo;", "const float3 base_rgb = float3(0.4,0.2,0.1);")
            check = """
    float expected = material_at(heap_slots_materials, push.material_index).toon_family == VR_FAMILY_FACE ? 1.0 : 0.742;
    bool correct = abs(roughness - expected) < 0.00001
        && all(abs(rim_base - float3(0.384,0.192,0.096)) < 0.00001);
    return correct ? float3(0.1,0.5,0.1) : float3(0.5,0.1,0.1);
"""
            if case == "skin-fallback":
                text = replace_once(text, "const float4 skin_material = game_toon_profile_lane(push.material_index, 15u);",
                                    "const float4 skin_material = float4(-1,0,0,1);")
                check = check.replace("material_at(heap_slots_materials, push.material_index).toon_family == VR_FAMILY_FACE ? 1.0 : 0.742", "0.8")
        else:
            target = tree / "toon/game_toon/captured/eye.slang"
            text = target.read_text()
            if case == "eye-modes":
                needle = "const uint material_index = push.material_index;"
                text = replace_once(text, needle, """
    float3 actual = game_toon_add_eye_lights(float3(0), float3(1,0,0), float3(0,0,1),
        float3(0), float3(0.3,0.2,0.1));
    return all(abs(actual - float3(0.3,0.2,0.1)) < 0.00001)
        ? float3(0.1,0.5,0.1) : float3(0.5,0.1,0.1);
    """ + needle)
                light_text = replace_once(light_text, "const uint base = 352u + index * 64u;", """
    PunctualLight fixture;
    fixture.position = float4(1,0,0,0);
    fixture.color = float4(1,1,1,index == 0u ? 0 : (index == 1u ? 2 : 3));
    fixture.spot_dir = float4(0);
    fixture.params = float4(0,0,1,0);
    return fixture;
    const uint base = 352u + index * 64u;""")
                light_text = replace_once(light_text, "const int count = clamp(int(light_at(heap_light_slot).light_count.x), 0, MAX_PUNCTUAL_LIGHTS);", "const int count = 3;")
                check = ""
            elif case == "matcap":
                text = replace_once(text, "const float3 matcap_brdf =", """
        return all(abs(matcap_tint - float3(1.2755,0.2382,0.733)) < 0.00001)
            ? float3(0.1,0.5,0.1) : float3(0.5,0.1,0.1);
        const float3 matcap_brdf =""")
                check = ""
            else:
                text = replace_once(text, "const float3 light = ambient_light + direct_light;", """
    color = float3(0.3,0.2,0.1);
    const float3 light = ambient_light + direct_light;""")
                check = """
    bool correct = all(abs(F0) < 0.00001)
        && all(abs(diffuse - float3(0.3,0.2,0.1)) < 0.00001);
    return correct ? float3(0.1,0.5,0.1) : float3(0.5,0.1,0.1);
"""
        if check:
            needle = "const int count = clamp(int(light_at(heap_light_slot).light_count.x), 0, MAX_PUNCTUAL_LIGHTS);"
            light_text = replace_once(light_text, needle, check + needle)
        header.write_text(light_text)
        target.write_text(text)
        command = [compiler, str(main_shader), "-I", str(tree), "-target", "spirv",
                   "-profile", "spirv_1_6", "-capability", "spvDescriptorHeapEXT",
                   "-spirv-resource-heap-stride", "64", "-spirv-sampler-heap-stride", "32",
                   "-fvk-use-gl-layout", "-matrix-layout-column-major", "-entry", "game_toon_frag_main",
                   "-stage", "fragment", "-o", str(outputs / "game_toon.frag.spv")]
        compiled = subprocess.run(command, capture_output=True, text=True)
        if compiled.returncode:
            print(compiled.stdout + compiled.stderr)
        compiled.check_returncode()
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
                           "window_width": "600", "window_height": "600",
                           "camera_pose": "[0,0,0.65,0,0.03,-0.85]"}.items():
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
        # Count the bright diagnostic colours, excluding the dark hair fringe.
        green = sum(g > 120 and g > r + 50 and g > b + 50 for r, g, b in pixels)
        red = sum(r > 120 and r > g + 50 and r > b + 50 for r, g, b in pixels)
        results.append(dict(case=case, matching=green, mismatching=red, picture=str(picture),
                            passed=green > 200 and red < 100))
        print(json.dumps(results[-1]), flush=True)
    (work / "results.json").write_text(json.dumps(results, indent=2))
    assert all(result["passed"] for result in results), f"Skin/eye response mismatch; artifacts: {work}"


if __name__ == "__main__":
    main()
