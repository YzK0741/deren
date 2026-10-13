# Configuration and windowless rendering

`[render] async_present` now replaces `DEREN_ASYNC_PRESENT` in current builds. It defaults to
false and can be reapplied by the application's existing live settings loop. `offscreen` defaults
to false and is a startup setting. Both keys are parsed, exposed in `config.example.toml`, and
included in the interactive configuration generator; the default generator copies that template.
Historical asynchronous-presentation snapshots retain their original environment-variable switch.

A null `rhi::create_info::native_window` now selects windowless rendering. The backend creates
no application window, surface or swapchain, and does not request WSI instance/device extensions.
It allocates two ordinary device-local B8G8R8A8 SRGB images at the configured output resolution.
Each image corresponds to a frame slot: slot timeline completion protects reuse, submission
signals that timeline alone, and no acquire or presentation binary semaphore is awaited.
Views are destroyed before images, images before their memory, and all before the device.
The runtime submits normally, retains readback/GPU timings, skips presentation, and leaves the
output in a valid sampling layout. GLFW event polling and the debug GUI are skipped.

The existing `frame_swapchain()` facade continues to report output format/extent without requiring
an actual swapchain. Windowless `present()` and facade `recreate()` return `error::unsupported`;
the optional asynchronous-presentation extension is absent. A non-null caller-owned window
retains the normal surface/swapchain path. The backend no longer creates a fallback GLFW window.

`--capture-frames N` automatically uses windowless rendering. `--capture-windowed` explicitly
retains the hidden-window path for window-system regressions, with `offscreen = false`.
`check_gaussian_resize.ps1` now passes that override. This keeps resize tests testing actual
windows rather than silently changing them into offscreen tests.

## Verification

- Configuration regression first failed without the new fields, then passed with true values
  from a fixture and false compiled defaults.
- A CPU device-selection regression first failed for null-surface queue completeness, then passed;
  a device without `VK_KHR_swapchain` can be selected for offscreen rendering.
- Final Release dynamic build passed. Final CTest: 24/24, 21.47 seconds.
- Native Vulkan offscreen test: 1164 checks, zero failures. It checks no GLFW window is created,
  unsupported presentation/recreation, output extent, GUI refusal, and six submissions reusing
  both slots. The driver creates its own `temp_d3d_window_*` HWND; this is not a renderer window.
- Flower CPU and GPU paths each captured 12 frames at 1280x720 with synchronization validation
  enabled and no validation errors. Final RGB and pre-post HDR are independently byte-identical
  to the frozen hidden-window runtime for each path, including identical PNG file hashes.
- CPU/GPU comparison: channel RMSE [0.0029463, 0.0091998, 0.0081357] on a 0..255 scale,
  maximum channel difference 5, 44 differing pixels; HDR RGB RMSE 0.0000252383. This is the
  existing CPU/GPU numerical difference, not a windowless-rendering change.
- Windowed synchronous mode and config-enabled asynchronous mode each completed three
  maximize/restore cycles (six actions), clean synchronization validation, 1280x720 final output.
- Independent review found no critical or important defect. CPU probe roots intentionally can
  advertise async presentation without a native window; Vulkan offscreen roots do not advertise it.

Reproduction and results are in `docs/benchmarks/2026-10-12-offscreen/`: image comparisons,
source/runtime fingerprints, final capture logs/configs/images, short timing CSVs and windowed
restore logs. Full transient runs and HDR files remain under
`build-release-dyn-clang64/offscreen-check/`. Short capture timings are diagnostics, not a new
performance baseline: removing presentation changes which waits those measurements include.

```powershell
cmake --build build-release-dyn-clang64 -j 6
ctest --test-dir build-release-dyn-clang64 --output-on-failure -j 6
./build-release-dyn-clang64/test_runtime_dyn.exe --with-device
./source/scripts/windows/check_gaussian_gpu.ps1 -WorkDir build-release-dyn-clang64/offscreen-check/final -Frames 12 -Timings $true
```
