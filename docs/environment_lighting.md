# Environment lighting

The environment supplies diffuse and specular IBL independently of the displayed background.
The renderer accepts either an HDR source (decoded and convolved at startup) or a pre-baked
`DERENIBL` file, which skips the HDR decode, GGX prefilter, irradiance map and BRDF LUT. The new
controls operate on the uploaded IBL; they do not add atmospheric fog. Scene reflections are a
separate opt-in pass: see [ray-traced reflections](rt_reflections.md).

## Quick start

The example configuration uses `environments/neutral.ibl`, copied next to `deren.exe` from
`asset/environments/neutral.ibl`. It was baked once from Studio Neutral:

```text
deren.exe --bake-environment asset/environments/neutral.hdr asset/environments/neutral.ibl
```

The command is CPU-only and writes a single file containing the header plus the prefiltered mip
chain, irradiance cube and BRDF LUT. Bake it again only when the HDR or the bake parameters change.
The runtime still accepts a raw `.hdr` path and performs the old convolution at every startup; that
path is useful while tuning an environment, not as the shipped default.

The source Studio Neutral panorama is Copyright 2020 Amazon, LLC, distributed by Khronos Group,
CC-BY-4.0; `asset/environments/neutral.hdr.license` carries the terms. An optional fetched Papermill
panorama (`python scripts/assets/fetch_environment.py --output build/environments`) is a
noncommercial reference asset (HDRLabs, sampled by UX3D, CC-BY-NC-SA-3.0), not a default distribution
dependency. Assets are not bundled into the executable.

For a controlled DamagedHelmet material preview, use these settings with the model's normal
configuration and a fixed camera:

```toml
[render]
exposure = 1.0
tone_mapping = 1 # 0 existing ACES fit; 1 Khronos PBR Neutral
sun_intensity = 0.0 # isolate IBL for comparison; this does not rotate or disable imported lights

[lighting]
environment_hdr = "environments/neutral.ibl" # relative to the executable
profile = "preview-lighting.toml" # environment calibration; relative to this file's directory
```

The calibration file carries the IBL/background keys (`environment_intensity`, `environment_strength`,
`environment_rotation`, `environment_background`, `environment_color`, `environment_blur`):

```toml
environment_intensity = 1.0 # bake-time multiplier; must match the .ibl header
environment_strength = 1.0 # live relative gain on the baked IBL
environment_rotation = 0.0 # degrees about world Y
environment_background = 1 # 0 legacy procedural sky; 1 solid; 2 environment; 3 blurred environment
environment_color = [0.18, 0.18, 0.18] # linear RGB, before exposure/tone mapping
environment_blur = 0.5 # mip-range fraction used by background mode 3
```

All background modes keep the same material lighting. A solid grey background therefore still
allows detailed environment reflections. Rotation changes the directions sampled by diffuse IBL,
specular IBL and environment backgrounds together: positive 90 degrees makes world +X sample the
environment's +Z. The manually authored sun and punctual lights keep their directions.
Strength scales the two IBL lobes and environment background; it does not scale emissive/direct
lights or the solid/legacy background. Existing `environment_intensity` keeps its startup-bake
semantics, including the historical 0.35 default; the new relative strength defaults to 1.

In a scripted project, `Deren.Managed` reads the same `[lighting] profile` before the project's
`initialize` (exposed as `project_config.lighting`) and applies it through the runtime setters, so
one profile drives both the native startup pass and the project's own lighting.

## Live controls

The debug panel's environment menu contains procedural sky, the configured entry (a raw `.hdr` or a
baked `.ibl`), and `.hdr` files found in `<executable-directory>/environments` at startup. Add local
panoramas there before launching; selecting a baked entry reuses its prefiltered and irradiance
data without another convolution. Rotation, strength, background mode/colour/blur, exposure and tone mapping are
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
Apache-2.0 implementation; original source, revision, hashes and license are kept under
`source/third_party/khronos_tonemapping`. Neither choice includes automatic exposure. LUT grading
is still the existing optional post step, so disable it for material-reference comparisons.

## Comparison limits

Match HDR bytes, environment orientation/intensity, camera, exposure, direct lights and tone mapping
before comparing with Sample Viewer. A reference screenshot without those settings does not establish
pixel equivalence. HDR preset/rotation comparisons validate lighting behavior; they do not prove all
material/BRDF or colour-management paths match another renderer.
