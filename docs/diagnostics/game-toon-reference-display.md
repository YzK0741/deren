# GameToon reference-look alignment: this round

Date: 2026-11-01.

## Changes

1. Post-processing previously sent tone mode 2 through PBR Neutral as well.
   Mode 2 now actually runs the transcribed reference-site tone map; the
   output still goes through the existing display encoding exactly once.
2. The reference material's `_SpecRampMap` was previously replaced by the
   generic 256x8 threshold ramp. When `_GameToon0.x > 0.5` and the model
   provides the texture, the model's own highlight-colour map is now bound;
   other materials keep the previous fallback.

Result: the hair's broad pale highlight becomes the finer, cooler pink
variation of the reference; skin and iris display separation is closer; the
nose bridge is easier to read. No exposure or material colour was retuned.

## Verification

- The application and affected shaders build.
- 22 independent reference values pass, maximum absolute error `2.63025e-7`.
- `scripts/gates/game_toon_tonemap.py`: two known HDR inputs through the real
  post chain match the reference output byte for byte (display-byte error 0).
  Before the fix, the grey input expected 88 and read 105.
- `scripts/gates/game_toon_specular_ramp.py`: GPU-verifies the bound texture is
  256x256 and that a given sample matches the GLB image; the diagnostic image
  matched 423208 pixels and 0 wrong pixels. Before the fix it read the
  substitute ramp.
- `test_shader_sources` and `test_toon_material_sidecar`: 2/2 pass.
- Front and side capture logs contain no validation error.

Both GPU tools use private shader/config directories, require the built
Laevatain preview, Pillow and Vulkan, and never overwrite the installed shader.

## Captures and records

Local working directory: `tmp/reference-look-alignment/`.

- Before: `before-face/screenshot_20261101_075550.png`
- After, same pose: `after-same-pose/screenshot_20261101_080009.png`
- Final close-up: `final-face/screenshot_20261101_080822.png`
- Final side: screenshots under `final-side/`

## Remaining items

The thin hair outlines still have breaks; the camera angle and head pose are
not yet matched exactly to the user's reference screenshots. The highlight map
still uses the sRGB-upload / captured-branch re-encode bridge, so its filtering
order differs slightly from the reference's raw-data sampling. These are left
for a separate pass and are not expanded here.
