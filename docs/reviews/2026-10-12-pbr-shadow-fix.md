# PBR shadow fixes

This fixes the three defects recorded in the same-day shader audit. Production synchronization
and resource ownership were not changed.

## Changes

- Forward PBR explicitly initializes `shadow_override = -1.0` and `punctual_replaced = 0.0`.
  Zero-initializing the first would select a zero-visibility override rather than normal shadows.
- The next cascade's near bound includes the shader's blend band, using the actual configured
  `cascade_blend` and the shader's minimum 1e-4 width. The runtime passes that setting to the fit
  and invalidates its cached fit when the blend changes.
- The projection's XY size now encloses the cascade slice in a camera-space sphere. Its radius
  does not depend on camera orientation, and is quantized using a scene-fixed world increment
  (`scene_radius / 1024`, with a small positive floor). The light-space centre is snapped to the
  resulting texel grid; caster merges continue to extend depth only. Five texels of margin cover
  centre snapping, a two-texel normal offset, and the basic PBR 3x3/hardware comparison footprint.
  Map sizes <=10 are rejected because that padding cannot fit.

## Verification

The initial regression tests failed before the fix: 16 shadow-fit checks and two forward shader
initialization checks. The tests passed after it. Coverage was then broadened:

- `test_shadow_fit`: 1626 checks pass. Rotation checks cover 1..4 cascades, including a 0.0001-radian
  orbit. Band coverage checks use 2..4 cascades, blend 0/0.1/0.5, band start/mid/end, all four receiver
  corners, seven unit normal directions, and the basic PCF footprint.
- `test_shader_sources`: 1584 checks pass. The rebuilt forward SPIR-V no longer reads any unwritten
  scalar float local; the two undefined data-flow lanes from the audit are gone.
- Release dynamic build passed after retrying a transient Ninja access violation with lower build
  parallelism. Final full CTest passed 24/24.
- Four real render scenarios (`deferred`, `shadow_single`, `transparent_blend`, `sponza`) each completed
  two matching captures without validation/log problems. The gate's reference comparison was NOT
  passed: its sandbox-local baseline directory has no references and therefore reported four
  unseeded scenarios, exit 1. No references were seeded or updated.
- Sponza orbit captures at frames 40 and 41, 0.5 degrees per frame, were captured before/after using
  the frozen earlier runtime and the current runtime. All four runs used synchronization validation
  and were clean. The images were inspected: floor shadows, thin bars and contact detail remain visible,
  without a gross visual regression. This is two adjacent frames, not proof that every old model's
  entire orbit is now flicker-free.

The sphere is more conservative than the previous tight light-space AABB. At the initial Sponza
pose, reported texel world sizes changed from 0.0019/0.0053/0.0184 to 0.0025/0.0064/0.0226. This is a
resolution-density tradeoff for rotational stability; the map resolution setting is unchanged.
Image changes are intentional and are not constrained to pixel identity. The measured per-channel
RGB RMSE against the frozen frame-40 image is [1.4636,1.2915,1.0530] on a 0..255 scale. It is not a
flicker metric, since a camera and image registration are involved in a temporal comparison.

Independent review found no blocking issue. The five-texel guarantee is for basic PBR PCF; optional
wide toon/area-light kernels retain their preexisting boundary considerations and are not claimed
fully covered by this margin. Receiver depth padding remains the preexisting world-scale policy.

Evidence: `docs/reviews/2026-10-12-pbr-shadow-fix/` contains paired orbit images, logs/configurations,
reproduction script, comparison data and source fingerprints. Transient SPIR-V and runs remain in
`build-release-dyn-clang64/shadow-fix-check/`.
