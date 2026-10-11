# Resize synchronization, 2026-10-11

The user's symptom is colored blocks, corruption or misplaced content after resizing the Gaussian viewer. This round fixes a confirmed swapchain synchronization error. The earlier visual corruption was not reproduced on the rebuilt baseline, so its cause remains unconfirmed.

## Confirmed fault and change

`core::submit_frame` waits for the acquired image at `VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT`. The initial color layout transition used `TOP_OF_PIPE` as its source stage, which did not chain that wait. Discarding old contents with an `UNDEFINED` layout does not remove the need to wait for presentation's use of the image. This is the dependency described in [Khronos's synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html).

In `source/backends/vulkan/constant_init/constant_init.cppm`, both `color_attachment_transition` and the `undefined_to_present_transition` fallback now use `COLOR_ATTACHMENT_OUTPUT` as their source stage. Access masks, layouts and destination stages are unchanged. No Gaussian upload or RHI interface changes were needed.

## Evidence

Test device: NVIDIA GeForce RTX 4060 Laptop GPU, driver 617.42. Viewer asset: Bouquet of Flowers, 562,974 splats. The debug GUI was enabled for the synchronization regression runs.

Before the fix, enabling synchronization validation on the rebuilt `fb6c5b02` baseline reproducibly reported:

```text
vkQueueSubmit(): WRITE_AFTER_READ hazard detected.
vkCmdPipelineBarrier2 ... writes to VkImage ... previously accessed by vkAcquireNextImageKHR.
```

The layer's diagnostic specifically identified the missing execution dependency between the semaphore wait stages and the barrier's source stage. The regression check failed on these errors. After changing the two source masks, the same resize run exited successfully with no synchronization errors. Logs are under `build-release-dyn-clang64/resize-investigation/red-acquire/` and `green-acquire/`.

Additional checks:

- Early screenshot after the first resize: clean validation and visually inspected output, 618x384 physical client pixels. This captures the request as soon as the application serves it, without claiming an exact first submitted frame.
- Three resizes with rendered frames between them: clean validation and visually inspected output, final extent 1162x724.
- Rapid repeated resizing: twenty window size changes, including shrinking and growing, with synchronization validation enabled. Vsync off and on were exercised.
- A one-splat cloud also passed three spaced resizes. The script waits sixteen new logged frames after each resize rather than assuming the first poll occurs at exactly frame eight.
- Release build: passed.
- CTest: 23/23 passed.
- `clang-format-check`: passed.
- Full render regression: fourteen scenarios, two runs each, no nondeterminism; all fourteen hashes match the frozen reference list. The script's older local PNG references differ in twelve cases, so the explicit `-Compare frozen` verdict is the relevant comparison. No references were updated.

Do not infer that the old corruption is resolved solely from these results. Several genuine resizes were already visually clean before this patch; the reproducible before/after difference is the validation error. The hidden-window checks also do not cover every interaction with a visible desktop window.

Independent read-only review found the two barrier changes sound and no blocking issues. Its minor observation about fast clouds and spaced resize thresholds was fixed and checked with the one-splat cloud. The user subsequently reported that resizing appears fixed, then reported model ghosting and an abnormal background when rotating the camera; that camera-dependent issue is a separate follow-up investigation.

## Repeating the GPU check

`source/scripts/windows/check_gaussian_resize.ps1` launches a hidden viewer and enables `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`. Supply a viewer TOML with an absolute PLY path in `model` and `[render] validation_layers = true`. It copies the config into the test directory and redirects screenshots there.

```powershell
pwsh -File source/scripts/windows/check_gaussian_resize.ps1 `
    -BuildDir build-release-dyn-clang64 -Config path/to/viewer.toml

# Capture soon after the first resize.
pwsh -File source/scripts/windows/check_gaussian_resize.ps1 `
    -BuildDir build-release-dyn-clang64 -Config path/to/viewer.toml -EarlyCapture

# Exercise rapid alternating sizes instead of three spaced resizes.
pwsh -File source/scripts/windows/check_gaussian_resize.ps1 `
    -BuildDir build-release-dyn-clang64 -Config path/to/viewer.toml -RapidResize
```

The script selects the process's drawable `GLFW30` window, checks the requested resize count, requires a swapchain rebuild and clean validation, and checks that the new PNG dimensions match the resized physical client area. It ignores old logs and captures on repeated runs. F12 requests the final screenshot after the resizes, so the process cannot consume a small fixed frame budget before the test finishes. Inspect the resulting PNG as well: clean validation is not an image correctness test.

The earlier generic window enumeration could select NVIDIA's `__wglDummyWindowFodder` instead of the renderer. A successful `MoveWindow` return alone therefore did not prove that the swapchain was resized. The new check identifies the GLFW window and verifies the actual output extent.

This is a manual Windows GPU check; it is not added to CPU-only CI. The PLY asset, configs and capture directories stay outside tracked source. The preexisting untracked `gaussian-fixture.ply` was preserved.
