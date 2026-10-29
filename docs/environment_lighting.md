# Environment lighting

The environment supplies diffuse and specular IBL independently of the displayed background.
The renderer retains the existing HDR-to-cube, irradiance, GGX prefilter and BRDF LUT pipeline.
The new controls operate on that pipeline; they do not add scene reflections or atmospheric fog.

## Quick start

Fetch an optional Studio Neutral panorama into the build's preset directory:

```text
python scripts/assets/fetch_environment.py --output build-release-dyn-clang64/environments
```

The script pins the Sample Viewer environment revision and verifies SHA256. Studio Neutral is
Copyright 2020 Amazon, LLC, distributed by Khronos Group, CC-BY-4.0. Keep its `.hdr.license` beside
the HDR. An optional `--reference-papermill` additionally downloads Papermill (HDRLabs, sampled by
UX3D, CC-BY-NC-SA-3.0); it is a noncommercial reference asset, not a default distribution dependency.
Assets are not fetched by builds and are not bundled into the executable.

For a controlled DamagedHelmet material preview, use these settings with the model's normal
configuration and a fixed camera:

```toml
[render]
exposure = 1.0
tone_mapping = 1 # 0 existing ACES fit; 1 Khronos PBR Neutral
sun_intensity = 0.0 # isolate IBL for comparison; this does not rotate or disable imported lights

[lighting]
environment_hdr = "environments/neutral.hdr" # relative to the executable
environment_intensity = 1.0 # multiplier baked into the input panorama
environment_strength = 1.0 # live relative gain on the baked IBL
environment_rotation = 0.0 # degrees about world Y
environment_background = 1 # 0 legacy procedural sky; 1 solid; 2 environment; 3 blurred environment
environment_color = [0.18, 0.18, 0.18] # linear RGB, before exposure/tone mapping
environment_blur = 0.5 # mip-range fraction used by background mode 3
```

All background modes retain the same material lighting. A solid grey background therefore still
allows detailed environment reflections. Rotation changes the directions sampled by diffuse IBL,
specular IBL and environment backgrounds together: positive 90 degrees makes world +X sample the
environment's +Z. The manually authored sun and punctual lights keep their own directions.
Strength scales the two IBL lobes and environment background; it does not scale emissive/direct
lights or the solid/legacy background. Existing `environment_intensity` keeps its startup-bake
semantics, including the historical 0.35 default; the new relative strength defaults to 1.

## Live controls

The debug panel's environment menu contains procedural sky, the configured HDR (if any), and
`.hdr` files found in `<executable-directory>/environments` at startup. Add local panoramas there
before launching. Rotation, strength, background mode/colour/blur, exposure and tone mapping are
adjustable while running.

Selecting a preset starts background CPU generation. The old environment keeps rendering while
the new irradiance/prefiltered cube is prepared. The newest selection wins; obsolete results are
discarded. The renderer publishes the new images between frames using its existing wait-idle and
transactional descriptor update. Failed decode/upload keeps the previous environment and reports
status in the panel/log. Publication can briefly pause a frame; fully asynchronous GPU publication
is outside this feature. Successfully changing illumination discards TAA history. Background colour
and strength are sanitized; non-finite configuration inputs fall back to defaults.
Decoded and prepared radiance must be finite, nonnegative and representable in half precision
(at most 65504). An excessively bright panorama is rejected before half conversion; reduce the
baked input multiplier to load it. Live strength does not change this input validation.

The existing ACES/Narkowicz fit stays the default. PBR Neutral is adapted from KhronosGroup's
Apache-2.0 implementation; original source, revision, hashes and license are retained under
`source/third_party/khronos_tonemapping`. Neither choice includes automatic exposure. LUT grading
is still the existing optional post step, so disable it for material-reference comparisons.

## Comparison limits

Match HDR bytes, environment orientation/intensity, camera, exposure, direct lights and tone mapping
before comparing with Sample Viewer. A reference screenshot without those settings does not establish
pixel equivalence. HDR preset/rotation comparisons validate lighting behavior; they do not prove all
material/BRDF or colour-management paths match another renderer.
