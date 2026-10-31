# README screenshots

## PBR helmets (updated 2026-10-29)

`DamagedHelmet.png` and `FlightHelmet.png` are actual 1080x960 renderer captures at frame 120,
using deferred PBR, Studio Neutral HDR IBL, a solid linear grey background [0.26, 0.26, 0.26],
exposure 1 and Khronos PBR Neutral tone mapping. These are explicit material-preview settings,
not the default configuration. Sun, shadows, SSAO, TAA, bloom and FXAA are disabled, as is the
GUI overlay. No removed traced-GI path is used. Both runs exited normally with Vulkan validation
enabled and no validation errors reported.

| Image | Camera yaw/pitch/distance/target | Relative environment strength |
| --- | --- | --- |
| DamagedHelmet | 0, 12, 3.7, 0, -1.6446, 0 | 0.45 |
| FlightHelmet | 25, 8, 1.25, 0, -0.443, 0 | 0.9 |

Both use input HDR intensity 1, environment rotation 0 degrees, a 256-pixel cube with nine mips,
16-pixel irradiance and a 128-pixel BRDF LUT. The different camera and relative strength values
are deliberate presentation choices; these two images are not an A/B material comparison.

The compressed [capture manifest](pbr-captures-2026-10-29.json.gz) retains the complete TOML,
command, executable and final PNG hashes, resolved camera logs and HDR provenance for each image.
To reproduce, extract a model's `config` string, adapt its absolute model/shader/HDR/output paths
to your checkout, then run the current executable with `--config <file> --capture-frames 120`
and the corresponding `--capture-camera=yaw,pitch,distance,target.x,target.y,target.z` above.
The local originals and logs are under `tmp/readme-pbr-20261029/`. See
[environment lighting](../environment_lighting.md) for the pinned HDR fetch command.

PNG compression is lossless: decoded bytes were checked against each original renderer PNG.
No crop, resize, retouching or colour adjustment was applied. Compressed sizes are 456159 bytes
for DamagedHelmet and 319816 bytes for FlightHelmet.

Studio Neutral: Copyright 2020 Amazon, LLC, distributed by Khronos Group, CC-BY-4.0; retained
asset hashes/license provenance are in the manifest and the fetched `neutral.hdr.license`.
FlightHelmet model-associated files are CC0-1.0 in glTF-Sample-Assets; its terms are kept in
`asset/FlightHelmet/LICENSE.md`. DamagedHelmet retains the terms in `asset/DamagedHelmet/LICENSE.md`. These material previews do not establish pixel equivalence
to Sample Viewer.

## CharacterForward.png

`CharacterForward.png` (1197x1494) is the exception to the rule above: a HAND-TAKEN screenshot of the
character-forward pipeline on a posed anime model, not a reproducible capture. The model, the pose and the
camera come from outside this repository, and the session's `toon shading` combo was at `off (plain pbr)`,
so this is the pipeline with the smooth shading response - the banded cel looks are that combo's other
values, and a banded example would have to be captured on purpose.

It was cropped from a 2560x1600 desktop screenshot (ShareX, 2026-10-02). The ImGui debug overlay, the window
title bar and the Windows taskbar are cut away, leaving the render alone; the client-area edges were measured
rather than guessed (`build-release-clang64/docs_review/toon_shot_final.py`, which finds the overlay's right
edge from its text density and the taskbar's top edge from the first row that is dark across the whole
width). Pixels inside the crop are the renderer's own; the crop was re-encoded with Pillow
(`optimize=True`) and NOT resampled.


## GameToon.png

`GameToon.png` (1140x1030) is the current README character shot. It is a HAND-TAKEN desktop
screenshot of the `game_toon` profile-on path rendering GameToon: captured skin/face, hair, cloth,
eye and outline shader branches, the captured light rig and the viewer's display transform. The
model and its Toon sidecar are checked in under `asset/laevatain/`; all rights to that asset belong
to Hypergryph Network Technology Co., Ltd., see `asset/laevatain/LICENSE.md`.

It was cropped from a 2560x1494 desktop screenshot (2026-10-31). The ImGui debug overlay and the
Windows taskbar are cut away, leaving the render alone. Pixels inside the crop are the renderer's
own; the crop was re-encoded with Pillow (`optimize=True`) and NOT resampled.

## gaussian-viewer.jpg

The Gaussian viewer renders NVIDIA's Bouquet of Flowers (CC BY 4.0), with
562,974 splats. The cloud is checked in at `asset/flowers/flowers_1.ply`; see
[Gaussian splatting](../gaussian_splatting.md) for the loader and viewer details.
