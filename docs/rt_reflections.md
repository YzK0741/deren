# Ray-traced reflections

Ray-traced reflections are an opt-in, per-frame effect for the deferred PBR path and the
GameToon cloth path. The default is off; with `rt_reflections = false` the renderer records
no reflection pass and the deferred lighting result is byte-identical to the pre-feature
output.

## Enabling it

```toml
[render]
rt_reflections = true
rt_reflection_cloth = true
rt_reflection_strength = 1.0
rt_reflection_materials = ["Material_MR"]
rt_reflection_samples = 1
```

| Key | Default | Meaning |
| --- | --- | --- |
| `[render] rt_reflections` | `false` | Trace reflection rays for visible pixels. A device without ray queries ignores the key and continues with the normal deferred lighting result. |
| `[render] rt_reflection_cloth` | `true` | Grant the composite to the loader's `cloth` material family. |
| `[render] rt_reflection_strength` | `0.35` | Multiplier on the Fresnel/roughness-weighted reflection term; `0` disables it, `1` is the full add. |
| `[render] rt_reflection_samples` | `1` | Rays per visible pixel, clamped to 1-4. `1` keeps the mirror trace and its roughness fallback; `2`-`4` importance-sample the GGX lobe and average the radiance. |
| `[render] rt_reflection_materials` | `[]` | Extra material names, matched as case-insensitive substrings (for example `"Material_MR"` or `"leather"`). The whitelist is resolved at scene import, so changing it requires a reload. |
| `[[scene.objects]] ray_tracing_reflection` | `cast_shadow` | Per-object opt-out. `false` keeps that object out of the traced reflection while leaving its raster result unchanged. |

A cel-shaded character is mostly flat, rough shading, so a mirror term over the whole frame
fights the intended look. The default therefore restricts the traced reflection to cloth,
plus any material named in `rt_reflection_materials`. Set `rt_reflection_cloth = false` to
use the name list alone.

## What is traced

The pass traces one to four reflection rays per visible pixel through the same scene
acceleration structures used by ray-traced shadows. With the default one sample it traces the
mirror direction. With two to four samples it importance-samples the GGX lobe (Heitz's
visible-normal distribution, so the sampled directions stay above the surface), averages the
radiance over the accepted samples, and averages the hit distance over real hits only: a miss is
not a hit at infinity, and reporting one would make the denoiser reconstruct an invalid hit
point. Surfaces rougher than 0.55 take a roughness-selected environment mip in either mode,
because four rays are no longer a useful estimate of a lobe that wide. A multi-sample ray that
escapes the scene also takes that mip, at the material roughness. The closest-hit shader shades
the hit's base color with the sun and a low-mip environment lookup. The deferred lighting stage
composites the result with a Fresnel and roughness weight, scaled by `rt_reflection_strength`.

The pipeline has four shader stages: ray generation, miss, closest hit, and any hit. Closest hit
and any hit form one hit group, so the shader binding table has three records (ray generation,
miss, hit group). The any-hit stage applies only to materials whose `alphaMode` is `MASK`. It
reads the base-color alpha at explicit LOD 0 and calls `IgnoreHit()` when
`base_color_factor.a * albedo.a < alpha_cutoff`, so a reflection ray sees the same cut-out
silhouette as a shadow ray. The predicate lives in
`source/shaders/common/rt_alpha_mask.slang`; editing it invalidates both ray-tracing pipelines.

The traced radiance then runs the AMD FidelityFX Reflection Denoiser: reprojection against the
previous frame, a spatial prefilter, and a temporal resolve that clips history to the local
color bounds. The three stages filter a glossy band whose upper bound is 0.2 for the one-sample
path and 0.55 for two to four samples: the multi-sample tracer reaches the wider band, so the
extra samples are filtered instead of left raw. One sample keeps AMD's 0.2 and the pre-sampling
output byte for byte. History is per swapchain image, so a newly enabled or resized frame starts
from the current sample instead of blending stale pixels.

## Capture

The README material preview uses this configuration with a 1080x960 offscreen capture at frame
30; the exact camera, HDR and tone-mapping settings are recorded in
[the screenshot notes](screenshots/README.md). A minimal run is:

```powershell
./build/deren --config reflection.toml --capture-frames 30
```

## Source layout

- `source/engine/pass/ray_traced_reflection.cppm` and `source/engine/pass/reflection_denoise.cppm`
  own the recorded passes.
- `source/engine/render_graph/rt_reflection.cppm` and
  `source/engine/render_graph/reflection_denoise.cppm` own their graph declarations.
- `source/shaders/lighting/rt_reflection.slang` holds the ray-generation, closest-hit, miss
  and alpha-mask any-hit stages; `source/shaders/common/rt_alpha_mask.slang` holds the predicate
  shared with ray-traced shadows. The denoiser stages are `reflection_reproject.slang`,
  `reflection_prefilter.slang`, `reflection_resolve.slang` and
  `reflection_copy_history.slang` under the same directory.

See [the configuration example](../config.example.toml) for the surrounding render keys and
the reasoning behind the defaults.
