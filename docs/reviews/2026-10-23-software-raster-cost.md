# Software raster cost reduction

The CPU reuse optimization is retained. The affine GPU arithmetic experiment is rejected: it did not establish a meaningful frame-time improvement, and review found a pixel-edge coverage regression.

## Retained change

Parallel per-item planning computes projected records once for the current records/model/camera. Stable hardware/software splitting and the serial CSR builder reuse those projections. The CSR builder borrows the cache instead of allocating and copying another projected-record array. Uncached callers retain the original path; a nonempty cache with incorrect length is refused by CSR construction, preserving whole-frame hardware fallback. Caches are transient, never shared between frames.

Tests compare cached/uncached split order, counts, offsets and entries across mixed sizes, offscreen and near-plane records and several thresholds. The new test first failed to compile without the cache API and passes after implementation. The actual planar GPU capture preserves the pre-optimization depth/ID hashes and remains validation-clean.

## Measurements

Same textured compatibility asset, settings, threshold 1 px, far/near cameras and three rounds of 129 timed frames. Each stage records executable, asset and shader hashes and refuses to mark results complete if executable/shaders change during measurement. Stages were measured sequentially, not interleaved; small GPU differences cannot establish a gain. [Raw rows and stage identities](2026-10-23-software-cost-results.json).

| Stage | Far GPU | Near GPU | Far CPU-active | Near CPU-active |
| --- | ---: | ---: | ---: | ---: |
| Corrected all-enabled baseline | 12.12 ms | 6.32 ms | 10.39 ms | 10.00 ms |
| CPU projection reuse (retained) | 12.03 ms | 6.26 ms | 9.07 ms | 8.29 ms |
| CPU reuse + affine GPU experiment (rejected) | 12.08 ms | 6.17 ms | 8.75 ms | 8.40 ms |

The retained CPU change reduces measured CPU-active time by 1.32 ms far / 1.71 ms near (12.7% / 17.1%). Classification-related wall time falls from approximately 4.17/3.54 ms to 2.53/1.97 ms. GPU time is effectively unchanged. This does not make software raster a net win versus DAG+HZB, whose earlier same-asset reference was 10.44/5.64 ms GPU and 7.15/6.79 ms CPU-active.

## Rejected GPU experiment

The experiment precomputed origin-relative barycentric coefficients and a depth gradient per triangle, replacing pixel divisions/interpolation with dot products. Planar and sloped fixtures passed CPU/GPU ID comparisons and repeated across ring slots: 117,180 and 109,327 covered pixels, maximum 1 and 3 ULP depth differences, validation clean. The fixture generator now supports `--slope 0.25`.

These fixtures did not cover every pixel-edge case. Review identified the screen triangle a=(0,0), b=(3,0), c=(0,3), pixel center=(0.5,2.5): float32 affine weights can yield wa=-5.96e-8, rejecting a pixel that the original formula accepts at wa=0. The production shader was restored to its pre-experiment arithmetic. The modest pass-time change (far software pass 2.36 to 2.29 ms; near unchanged at 0.18 ms) does not justify retaining that coverage change.

The CPU work remains independent of this rejected shader experiment. Further GPU optimization should reduce actual candidate/triangle work or use an explicitly validated edge rule. Parallel CSR construction, GPU-side binning and new routing policy remain future work; they were not silently introduced by this change.

## Final verification

The 14-worker full build succeeds and all 66 CTest tests pass outside the sandbox (hardlink boundary tests require that environment). The restored production shader bundle SHA256 exactly matches the CPU-only measurement stage: `c45143e7abe46108c2f0bf242bd5236d77c06ce9b14c990160fce60834be0adb`. Independent review found no cache lifetime or fallback defect; the GPU edge finding was addressed by removing the experimental arithmetic. The planar CPU-only capture remains byte-identical to the corrected pre-optimization GPU depth/ID, validation clean. No commit or push was performed.
