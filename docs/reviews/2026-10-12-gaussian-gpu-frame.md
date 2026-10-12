# Gaussian GPU preparation and stable sorting acceptance

Date: 2026-10-12. Implementation is in the existing working tree; no commit or push was made.

Gaussian frame preparation now traverses and sorts particles on the GPU. The CPU validates the camera and instance metadata, orders instance ranges by stable ID, and updates only small mapped tables. VRDX sorts fixed candidate slots; invisible keys go to the end. Finalization writes references and a 16-byte indirect draw command. The existing six-vertex raster, SH degree 0..3, regularized 3-sigma footprint, near-plane omission, alpha kernel and depth/blend behavior remain in use.

The default `auto` path selects GPU preparation when available and falls back to CPU on initialization failure. `DEREN_GAUSSIAN_PATH=cpu|gpu|auto` selects a diagnostic path. Forced GPU failure is reported as `FAIL` and never silently renders a CPU snapshot. Recording refusals are collected from the pass and logged; numeric errors suppress the entire Gaussian draw. Frames retain assets and buffers per completed frame-ring slot, rather than swapchain image index. Normal preparation performs no `wait_idle` or readback wait.

VRDX 0.6.0 is vendored unchanged under `source/third_party/vrdx`, pinned to `b0db22b0c65e17a9b803bc4daf20354898b820eb`, with its MIT license and provenance. Vulkan uses the existing 1.3 device with `VK_KHR_push_descriptor`, synchronization2 and checked subgroup/workgroup/shared-memory support. The RHI ABI is 32. Sorting restores descriptor heaps after traditional push descriptors and requires subsequent pipeline/push rebinds.

## Performance

Device: NVIDIA GeForce RTX 4060 Laptop GPU, NVIDIA driver 617.42. Release build, Bouquet of Flowers (562,974 particles, SH3), 1280x720, GUI/TAA/VSync/validation off, frame cap off. Each path ran three stationary and three rotating repetitions, discarding 60 warmup frames and taking the next 300 samples. Rotation advanced 0.5 degrees per presented frame. Both paths used identical camera/configuration and buffered CSV instrumentation; per-frame Gaussian console tracing was disabled.

| Scene/run | CPU median / p95 ms | GPU median / p95 ms | Median frame speedup |
| --- | ---: | ---: | ---: |
| Static 1 | 123.633 / 165.796 | 5.901 / 6.401 | 20.95x |
| Static 2 | 156.835 / 167.030 | 5.890 / 6.296 | 26.63x |
| Static 3 | 149.910 / 161.189 | 5.908 / 6.495 | 25.37x |
| Rotation 1 | 152.065 / 164.022 | 7.115 / 7.583 | 21.37x |
| Rotation 2 | 157.792 / 167.153 | 7.101 / 7.552 | 22.22x |
| Rotation 3 | 116.814 / 163.439 | 5.981 / 6.449 | 19.53x |

CPU preparation median fell from 91.78–92.27 ms to 0.0020–0.0025 ms. GPU stage medians were prepare 0.609–0.622 ms, sort 0.175 ms, finalize 0.0127–0.0128 ms, and raster 1.610–1.625 ms. Lighting, Gaussian stages and transparent work have separate timestamps; both engine/backend mark capacities are 32. End-to-end frame samples include presentation and scheduling, which explains variation between runs; these measurements are not a promised FPS for other devices or scenes. Initial sorter/pipeline creation is excluded by warmup.

Requested dynamic buffer capacity is 13,657,696 bytes per slot, 27,315,392 bytes for both slots. This includes keys, values, references, queried scratch, instance/range tables, parameters, status and indirect arguments; immutable assets and allocator block overhead are separate.

Raw CSV, exact configurations, screenshots, HDR files and `performance.json` are under `build-release-dyn-clang64/gpu-performance-acceptance`. Reproduce with `source/scripts/windows/benchmark_gaussian_gpu.ps1`.

## Images and correctness

The original five-scene CPU raster probe and the GPU probe use the same shared projection/SH helper. The final GPU probe passes 11 scenes and 10,626 compared pixels, max HDR RGB error 0.0008074641 (limit 0.005), maximum forward-depth difference 1 float32 ULP (limit 4), and zero small-sample double-order differences. It checks SH0/1/2/3, transformed anisotropic instances, regularized edge footprints, opaque depth, tied/reordered instances, all outside, far/near support, numeric overflow, stable key/value order, reference expansion, indirect count, deterministic repeats, and completed-slot reuse across visibility changes. Numeric overflow produces a blank Gaussian draw.

48 independent real GPU sort cases cover 0/1, 255/256/257, 511/512/513, 4095/4096/4097 and 65,537 items, with duplicate/all-equal/ascending/descending keys. Keys and values agree exactly with CPU stable sorting. The probe also rejects alignment/overlap/missing-scratch mistakes, unopened command buffers and unrepresentable maximum counts. All diagnostic submissions ran with synchronization validation and no errors.

| Flower final pose | HDR RGB RMSE | Maximum local HDR RGB difference | Final RGB channel RMSE, 0..255 units | Differing pixels / max channel difference |
| --- | ---: | ---: | --- | --- |
| Stationary yaw 0 | 0.000007815 | 0.004882813 | 0.002552 / 0.002756 / 0.002946 | 18 / 1 |
| Rotated yaw 182 | 0.000015431 | 0.010986328 | 0.002946 / 0.004996 / 0.003125 | 28 / 3 |

Both meet HDR RMSE <=0.005 and final per-channel RMSE <=1/255. Dense-scene CPU double ordering and GPU float32 ordering are not claimed identical. The local rotated difference is recorded explicitly rather than hidden by a mean; the final image differs by at most three channel levels on 28 pixels, without missing footprints. GPU screenshots were inspected. Moving through yaw 182 versus a fresh stationary run at exactly that pose produced identical PNG pixels and identical HDR bytes. Evidence: `gpu-camera-history-held` and `gpu-performance-acceptance/rotation-3`.

Three hidden-window resizes passed with synchronization validation, ending at 1162x724; evidence `gpu-resize-final`. Empty mesh/particle startup retains the previous clear-only scene fix. Fake tests cover instance validation, budgets including scratch, slot reuse, rollback of partial allocations/growth, two independent slots retaining old assets, recording before rendering, rebinding after sort, indirect drawing, empty frames and timestamp order. Asset unload/reload ownership was verified by the two-slot fake test; an interactive viewer unload/reload was not exercised.

The final default-auto smoke run selected GPU for all 562,974 flower candidates, produced a fresh screenshot after 12 frames and passed synchronization validation; evidence `gpu-default-auto-final`. No path override was supplied.

## Final checks and review

Release build, five Gaussian SPIR-V stages, clang-format and Gaussian clang-tidy checks passed. Full CTest passed 24/24. Windows hard-link cases in the boundary test require execution outside the filesystem sandbox; rerunning the unchanged suite with that access passed. Ordinary model captures matched all 14 frozen hashes without updating references. CI now builds the new headless GPU-frame test and checks the new sources.

An independent read-only review found one issue: runtime did not consume Gaussian pass recording errors. The fix carries `last_error()` through the existing stage collection seam and logs the skipped frame; the capture/resize checkers reject that failure. The reviewer verified the fix and found no remaining blockers. Collection only occurs for invoked passes, preventing stale error reports.

Mesh shaders, SH compression, size culling, asynchronous old-camera sorting and stochastic transparency were not implemented. Raster is now the largest Gaussian GPU interval; future work can evaluate mesh shaders against the same acceptance protocol.
