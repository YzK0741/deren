# BC texture cooking

Build `deren_texture_export` with the normal project build, then install the offline encoder
with `python -m pip install -r tools/textures/requirements.txt`.

```powershell
python tools/textures/cook_bc7.py asset/bistro/bistro.gltf build/texture-cache --exporter build/deren_texture_export.exe
$env:DEREN_TEXTURE_CACHE = (Resolve-Path build/texture-cache).Path
```

The next normal `deren run` uses matching entries for PBR albedo, metallic/roughness and normal maps. Remove the environment
variable to compare against source textures. The game needs no Python or encoder installation.
The CPU exporter uses the engine's existing colour-aware mip generator. Cache keys fingerprint
all source mip bytes, dimensions, mip count, colour space and encoder profile. Generated DDS files contain native BC7 sRGB albedo, BC7 UNORM material data and BC5 UNORM normals, with the original mip chain. Normal maps containing negative Z use BC7 UNORM to preserve all three components. Missing, invalid or stale entries use
the source texture. Devices without support for the selected compression format use the source texture as well.

Alpha-cutout and transparent albedo retain their existing path. Their PBR material and normal maps can use compressed entries. Classified toon materials retain their existing texture path.
Cooking preserves the original assets. Put generated caches in an ignored build directory.

Valid existing entries are reused when cooking again. The runtime reports unique compressed texture count and RGBA/compressed payload bytes at import. Compare
the same camera, render resolution and mip residency when measuring GPU time. Payload bytes exclude
driver allocation alignment, render targets, geometry and CPU-decoded source pixels.

Validate the offline encoder after installing its dependency:

```powershell
python tools/textures/test_cook_bc7.py
```

The BC5 encoder takes packed RG8 input (two bytes per texel). The cooker explicitly
packs the red and green channels before calling it; passing RGBA bytes to this kernel
introduces alternating false normals. The cache profile includes the input layout so
older incorrectly encoded BC5 entries are ignored and recooked.
