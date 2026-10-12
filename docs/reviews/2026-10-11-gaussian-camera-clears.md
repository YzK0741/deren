# Gaussian camera ghosting and missing scene clears, 2026-10-11

Rotating the Gaussian viewer could retain old geometry and brighten the background. Starting directly at the same camera angle could instead produce a black frame. This was a separate defect from the swapchain acquire synchronization fix in `1287055e`.

## Cause and change

The viewer still imports an opaque placeholder triangle. When camera culling removed every opaque mesh, `scene_pass::record` returned before opening its rendering scope. Consequently, none of its five color targets or its depth target were cleared. Deferred lighting adds into the existing scene color; the Gaussian pass loads scene color and depth. Their inputs could therefore contain previous-frame contents or uninitialized contents on startup.

The scene pass now opens and closes a clear-only scope when its visible leaf list is empty. This needs neither a draw environment nor secondary command buffers, and sets `secondary_contents = false`. Frames with no targets, no primary command buffer, or a zero extent still do nothing. Nonempty frames retain their existing drawing path and prerequisites. The existing runtime attachment transitions and downstream barriers cover these clear writes.

This clear-only change did not itself remove the placeholder or establish that scenes with no imported geometry work throughout the engine. A subsequent [shared-resource preparation fix](2026-10-11-empty-scene-resources.md) removed the helper and made the normal frame lifecycle initialize these resources independently of primitives.

## Regression evidence

`test_pass` calls the real scene pass with a recording RHI test double. It checks five zero-cleared/stored color attachments, depth cleared to 1 and stored, the frame extent, a balanced clear-only scope without secondary contents or execution, zero-extent rejection, and a refused begin that must not be ended.

The test was run against the committed, defective scene pass: 11 assertions failed. Restoring the fix produced **198 checks, 0 failures**. The varying check count comes from the attachment loop, which never executes when the defective pass skips rendering.

The GPU comparison used Bouquet of Flowers (562,974 splats), 1280x720, TAA off, GUI off, and synchronization validation on. For each angle, a 41-frame camera sweep was compared with a fresh run held at the screenshot's **actual logged camera pose**. Captures are served on the following frame, so `--capture-frames 41 --capture-sweep 2` starting at yaw 0 captured yaw 82, not 80. Comparisons made before correcting that alignment were discarded.

| Implementation | Actual final yaw | Moving versus stationary RGB result |
| --- | --- | --- |
| Committed defective scene pass | 82 degrees | 921,567 of 921,600 pixels differ by more than 2 channel levels; mean absolute channel difference 228.106 |
| Fixed scene pass | 82 degrees | Exact pixel equality |
| Fixed scene pass | 162 degrees | Exact pixel equality |
| Fixed scene pass | 242 degrees | Exact pixel equality |
| Fixed scene pass | 322 degrees | Exact pixel equality |

Both defective captures were inspected: moving retained a bright background and a stale triangle, while stationary was black. The fixed sweep captures were also inspected. The fixed stationary captures are pixel-identical to those sweeps. All runs were free of Vulkan validation errors, including the defective runs: synchronization validation alone cannot detect this missing initialization.

Local evidence is under `build-release-dyn-clang64/resize-investigation/`: `camera-history-aligned-red/`, `camera-history-aligned-green/`, `camera-red-unit.log`, and `camera-green-build.log`. These generated assets and logs are not tracked.

The restored Release build and `clang-format-check` passed. The final full CTest run passed **23/23**. An earlier sandboxed run failed two hard-link creation cases inside `test_backend_boundary` with Windows access denied; rerunning with the required filesystem access passed, without changing the tests. Independent read-only review found no blocking issues in the clear-only scope, its callers, or its resource dependencies.

Full render regression also passed: fourteen scenarios, two runs each, zero nondeterminism, and **14/14 actual hashes match the frozen reference list**. The older local PNG reference set differs in twelve scenarios, as documented by the existing harness; `-Compare frozen` is the acceptance criterion and no references were updated. Set `DEREN_BASELINE_DIR` to the existing local reference directory when running in a sandbox that redirects `LOCALAPPDATA`. The final log is `camera-full-render-final.log` in the evidence directory above.

## Repeating the camera check

Use a viewer TOML with an absolute flower PLY path, `taa = false`, `validation_layers = true`, and `[gui] show = false`. Set an absolute screenshot directory for each run. Enable synchronization validation with `VK_LAYER_VALIDATE_SYNC=1`.

Run the viewer with:

```text
--config <viewer.toml> --capture-frames 41 --capture-camera 0,0,4.6685,0.1190,0.3818,0.3433 --capture-sweep 2
```

Read `camera pose (F12 screenshot)` from its log, then capture again with that six-value pose and `--capture-sweep 0`. Compare decoded RGB pixels at equal dimensions and inspect the images. Repeat with starting yaw 80, 160, and 240. Do not compare different camera poses or differently sized sky regions as a corruption metric.

The check covers a reproducible camera-history defect on the tested GPU. It is not evidence about every possible Gaussian artifact or every visible-window interaction.
