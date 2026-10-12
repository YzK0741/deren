# Shared frame resources without placeholder geometry, 2026-10-11

The Gaussian viewer now renders with zero mesh primitives. It no longer imports an opaque placeholder triangle to initialize the renderer, so the triangle cannot appear as scenery or affect depth.

## Failure and cause

With the earlier clear-only scene-pass fix in place, removing the triangle still made a zero-mesh frame fail submission with error 11 (`device_lost`). Both the Bouquet of Flowers viewer and a zero-particle PLY reproduced this; no screenshot was produced. A zero process exit code was insufficient because the render loop could stop after a refused submission.

Shared scene descriptors were written only when `create_primitive` or `make_instanced_primitive` called `ensure_scene_heap_slots`. That lazy path allocated shadow maps, light/head buffers and cluster buffers, then published their bindings, including the camera and other shared scene buffers. A scene with no primitive never took that path, even though its lighting and frame passes still consumed those descriptors.

## Changes

- `pace_and_acquire` calls `prepare_scene_resources` after configuration and valid presentation extent checks, before frame acquisition, per-frame light updates and resource publication. Later frames reuse the initialized resources.
- Primitive creation no longer performs shared scene preparation.
- Names follow their responsibilities: `ensure_scene_heap_slots` became `prepare_scene_resources`, `ensure_shadow_resources` became `create_shadow_resources`, and `ensure_cluster_buffers` became `create_cluster_buffers`. Declarations, calls, comments and naming conventions were updated. Other unrelated `ensure_*` helpers retain their existing names.
- Startup `enable_shadows` initializes the CPU lighting state without requiring mapped light buffers. The first frame uploads that state after allocating its buffers. A zero-radius empty scene uses radius 1 for its initial light frustum, avoiding degenerate matrices; positive scene radii retain their previous behavior.
- The triangle helper, its import adapters, its declaration and its application call were removed. The earlier clear-only scene pass supplies initialized color and depth when no mesh is visible.

Independent review caught the startup shadow guard during this work. The new regression check first failed with the old guard (startup shadow configuration skipped) and passed after removing its allocation dependency. Final review found no remaining blockers or other startup settings with the same dependency.

## Verification

`source/scripts/windows/check_empty_scene.ps1` is a manual Windows GPU regression. It generates a valid zero-particle PLY, runs eight capture frames in a zero-mesh scene with synchronization validation, and requires a fresh screenshot, clean rendering logs, and successful startup shadow configuration. It checks real renderer output rather than only process termination.

- Before shared frame preparation: submission was refused and the script failed.
- After preparation and shadow setup correction: empty scenes passed with one and four shadow cascades. The background capture was visually inspected.
- Release build and `clang-format-check`: passed.
- Full CTest: 23/23 passed. The hard-link boundary tests require execution outside the restricted filesystem sandbox.
- Full ordinary-model render regression: fourteen scenarios, two runs each, no nondeterminism, all 14 frozen hashes matched. No reference images were changed.

The earlier no-placeholder camera run matched moving and stationary images exactly at actual yaw 82, 162, 242 and 322 degrees. After the startup-shadow correction, the final build repeated yaw 82 and 322: moving and stationary RGB images matched exactly, synchronization validation was clean, and the captures were visually inspected with no placeholder triangle present.

The final build also passed three window resizes (938×584, 1050×654 and 1162×724) with clean synchronization validation. The final 1162×724 capture was visually inspected: the cloud and background remained intact.

Evidence lives under `build-release-dyn-clang64/resize-investigation`: `no-placeholder-red`, `empty-resources-red`, `empty-shadow-setup-red`, `empty-shadow-setup-green`, `empty-shadow-four-green`, `no-placeholder-final/results.json`, `no-placeholder-resize`, `no-placeholder-render.log`, and `empty-scene-ctest.log`. Generated images, logs and PLY files remain untracked.

## Repeat

```powershell
pwsh -File source/scripts/windows/check_empty_scene.ps1 -BuildDir build-release-dyn-clang64
pwsh -File source/scripts/windows/check_empty_scene.ps1 -BuildDir build-release-dyn-clang64 -ShadowCascades 4
```

Use the camera-pose protocol in [the clear-only report](2026-10-11-gaussian-camera-clears.md) for a real cloud, and `check_gaussian_resize.ps1` with a viewer TOML for resizing. These are local GPU checks; they do not add a device requirement to CPU-only CI.
