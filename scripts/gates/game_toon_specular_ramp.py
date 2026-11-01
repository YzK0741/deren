"""Check the GPU-bound hair specular ramp against the preview GLB image.

Exercises the real application texture callback, detecting the old generated
256x8 threshold ramp replacing the captured 256x256 colour palette.
Uses private shader/config copies. Requires Pillow and the Laevatain preview.
"""
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

from PIL import Image


def main():
    root = Path(__file__).resolve().parents[2]
    build = root / "build-release-dyn-clang64"
    model = root / "asset/laevatain/laevatain_game_toon_preview.glb"
    rows = [row.split("\t") for row in Path(str(model) + ".toon.tsv").read_text().splitlines()]
    name = next(row[3] for row in rows if len(row) == 4 and
                row[:3] == ["M_actor_laevat_hair_01", "slot", "_SpecRampMap"])
    data = model.read_bytes()
    json_size = struct.unpack_from("<I", data, 12)[0]
    gltf = json.loads(data[20:20 + json_size])
    image = next(image for image in gltf["images"] if image.get("name") == name)
    view = gltf["bufferViews"][image["bufferView"]]
    offset = 28 + json_size + view.get("byteOffset", 0)
    with Image.open(io.BytesIO(data[offset:offset + view["byteLength"]])).convert("RGB") as im:
        width, height = im.size
        x, y = width // 2, height // 2
        rgb = im.getpixel((x, y))
    expected = [v / 255 / 12.92 if v / 255 <= .04045 else ((v / 255 + .055) / 1.055) ** 2.4 for v in rgb]
    work = Path(tempfile.mkdtemp(prefix="game-toon-specular-", dir=root / "tmp"))
    shaders = work / "shaders"
    shutil.copytree(build / "shaders", shaders)
    source = (root / "source/shaders/toon/game_toon.slang").read_text()
    needle = "const surface_sample s = gather_surface(v_world_pos, v_normal, v_uv, v_is_front);"
    probe = f"""
    if(s.toon_family != VR_FAMILY_HAIR) return float4(0,0,0,1);
    uint w,h,l;
    heap_at<Texture2D<float4>>(scene_texture_base()+s.toon_indices.z).GetDimensions(0,w,h,l);
    float3 texel=frame_texture_sample(s.toon_indices.z,float2({(x+.5)/width},{(y+.5)/height})).rgb;
    bool ok=w=={width} && h=={height} && all(abs(texel-float3({','.join(map(str, expected))}))<.0001);
    return ok?float4(.1,.5,.1,1):float4(.5,.1,.1,1);
"""
    assert source.count(needle) == 1
    path = work / "probe.slang"
    path.write_text(source.replace(needle, needle + probe))
    compiler = re.search(r"^VR_SLANGC_EXECUTABLE:FILEPATH=(.+)$", (build / "CMakeCache.txt").read_text(), re.M)[1]
    subprocess.run([compiler, str(path), "-I", str(root / "source/shaders"), "-target", "spirv",
                    "-profile", "spirv_1_6", "-capability", "spvDescriptorHeapEXT",
                    "-spirv-resource-heap-stride", "64", "-spirv-sampler-heap-stride", "32",
                    "-fvk-use-gl-layout", "-matrix-layout-column-major", "-entry", "game_toon_frag_main",
                    "-stage", "fragment", "-o", str(shaders / "game_toon.frag.spv")],
                   check=True, capture_output=True)
    # The normal outline has red/brown pixels unrelated to this diagnostic.
    # Discard it in the private copy so only failed hair probes count as red.
    outline = work / "discard-outline.slang"
    outline.write_text('[shader("fragment")] float4 main():SV_Target { discard; return float4(0); }')
    subprocess.run([compiler, str(outline), "-target", "spirv", "-profile", "spirv_1_6",
                    "-entry", "main", "-stage", "fragment", "-o", str(shaders / "outline.frag.spv")],
                   check=True, capture_output=True)
    config = (build / "config.toml").read_text().replace("show=true", "show=false")
    config = re.sub(r'screenshot_dir="[^"]*"', f'screenshot_dir="{work.as_posix()}"', config)
    config = re.sub(r'shaders_dir="[^"]*"', f'shaders_dir="{shaders.as_posix()}"', config)
    cfg = work / "config.toml"
    cfg.write_text(config)
    env = os.environ.copy()
    env["PATH"] = "C:/msys64/clang64/bin;" + env["PATH"]
    subprocess.run([str(build / "deren.exe"), "--config", str(cfg), "--capture-frames", "5",
                    "--capture-camera=0,0,.60,0,-.63,0"], cwd=build, env=env,
                   check=True, capture_output=True, timeout=120)
    picture = next(work.glob("screenshot_*.png"))
    with Image.open(picture).convert("RGB") as im:
        pixels = list(getattr(im, "get_flattened_data", im.getdata)())
    green = sum(g > r + 30 and g > b + 30 for r, g, b in pixels)
    red = sum(r > g + 30 and r > b + 30 for r, g, b in pixels)
    result = dict(texture=name, size=[width, height], sample_rgb=rgb, matching=green, mismatching=red,
                  picture=str(picture))
    (work / "results.json").write_text(json.dumps(result, indent=2))
    print(result, flush=True)
    assert green > 50000 and red < 10000, "GPU specular ramp differs from the captured model image"


if __name__ == "__main__":
    main()
