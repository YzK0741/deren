# Gaussian projection cache acceptance

Date: 2026-10-12. This follows GPU sorting and the bouquet orientation correction. No commit or push was made.

## Finding and change

Isolated diagnostic shaders kept the original vertex shader while discarding fragments, or kept projection/opacity while replacing vertex SH color with a constant. Normal raster medians were 1.62-1.75 ms; discarded fragments still took about 1.55 ms, while removing SH-related vertex work took 0.87-0.93 ms. These are cost estimates, not equivalent-quality alternatives. Diagnostic sources and SPIR-V stayed under the ignored build tree and were never installed as normal shaders.

GPU preparation already computed projection and SH radiance once per candidate. It now saves three float4 values (48 bytes): NDC center/depth, the regularized ellipse basis, and color/opacity. The six raster vertices fetch those values and form their corners instead of repeating projection and SH. Fragment support, alpha cutoff, blending, depth and sorting are unchanged. The CPU path still uses the original projection helper; the shared helper now exposes its computed cache as an optional output.

Per-instance reserved lanes 37/38 carry the low/high words of that instance's cache address; the offset follows stable-ID candidate ranges, preserving original instance/local indices. Raster push flag bit 0 enables cache consumption. Preparation parameters retain flags=0, and the original 112-byte push, 160-byte instance and 8-byte reference layouts are unchanged. Invisible candidates do not enter references, and any numeric error makes the entire indirect instance count zero, so unwritten cache entries are never read.

The GPU-only cache is owned by its completed frame slot, participates in staged allocation/growth and the retained-capacity budget, and has a shader-write to shader-read publication barrier. The cache stores float32 values without compression. No readback wait is introduced.

## Measurements

RTX 4060 Laptop, driver 617.42, Release, 562,974 SH3 flower particles, 1280x720. The corrected bouquet orientation aligns local -Y toward sun [0, 0.25, 1]. Both variants used the same current executable and configuration; baseline loaded a preserved copy of the original five Gaussian SPIR-V stages, cache loaded the new shaders. The baseline executable also allocates the new cache but does not use it, keeping host preparation/allocation conditions equal. GUI/TAA/VSync/validation/frame cap off, buffered timing CSV on; 60 warmup then 300 samples. Three static and three rotating repetitions, rotation 0.5 degrees per frame. No other builds, tests or GPU workloads ran during sampling.

Gaussian total below is the per-sample sum of prepare, sort, finalize and raster, then its median, rather than a sum of separately computed medians.

| Scene/run | Gaussian total baseline -> cache, ms | Reduction | Frame median baseline -> cache, ms |
| --- | ---: | ---: | ---: |
| Static 1 | 2.485 -> 1.849 | 25.57% | 6.131 -> 5.404 |
| Static 2 | 2.456 -> 1.961 | 20.14% | 5.930 -> 6.657 |
| Static 3 | 2.456 -> 1.844 | 24.90% | 7.013 -> 5.333 |
| Rotation 1 | 2.452 -> 2.097 | 14.47% | 7.178 -> 7.167 |
| Rotation 2 | 2.452 -> 1.777 | 27.56% | 7.140 -> 5.358 |
| Rotation 3 | 2.448 -> 1.872 | 23.55% | 6.067 -> 6.658 |

Raster median fell from 1.62-1.66 ms to 0.87-1.12 ms (31-47%); preparation rose from about 0.61-0.67 ms to 0.71-0.72 ms for cache writes. Measured total GPU interval medians also improved in all six pairs. End-to-end frame times vary with desktop scheduling/presentation and GPU clock conditions; two pairs became slower, so this report does not claim a repeatable equivalent FPS increase. The repeatable reduction in Gaussian GPU work is the basis for keeping the change.

Memory tradeoff: 27,022,752 additional bytes per flower slot; dynamic capacity including existing scratch is now 40,680,448 bytes/slot and 81,360,896 bytes for two slots. Immutable assets and allocator block overhead are separate. Existing byte limits can refuse the larger allocation.

Raw CSV, configurations, diagnostic shader sources/binaries, captures, and median/p95 JSON are in `build-release-dyn-clang64/gaussian-raster-profile`; `cache-performance.json` includes all intervals. The build-tree `profile_gaussian_raster.ps1` and summarizer reproduce these runs with the preserved baseline. These diagnostic artifacts depend on this local build tree.

## Correctness and review

- Six matched baseline/cache final PNGs are pixel-exact, covering static and rotated final yaw 182.
- A separate validated baseline/cache capture has byte-exact RGBA16F HDR and pixel-exact PNG output (`gaussian-cache-hdr-baseline` versus `gaussian-cache-images/gpu`).
- CPU/cache flower comparison meets existing thresholds: HDR RGB RMSE 0.0000252383, maximum local HDR difference 0.021728515625; final RGB RMSE [0.00360844, 0.00982706, 0.00908104] in 0..255 units, 55 pixels differ, max channel difference 5. These CPU/GPU differences already exist with the uncached GPU shaders, as shown by the byte-exact cache/baseline HDR comparison. No dense-scene double/float sorting equivalence is claimed.
- Native probes pass: 48 stable sort cases; original 5 CPU raster cases / 5,181 pixels; 11 GPU raster cases / 10,626 pixels, maximum HDR RGB error 0.0008074641, depth difference 1 ULP and zero small-sample double-order differences. Synchronization validation is clean (`gaussian-cache-probes`).
- 20 consecutive hidden-window resizes pass synchronization validation (`gaussian-cache-resize`).
- Cache resource/budget/address/publication tests were observed RED before implementation and GREEN afterwards. Full CTest passes 24/24, release build, format, two changed Gaussian C++ sources under clang-tidy, and five Gaussian SPIR-V stages pass. The existing Windows hard-link boundary cases require the sandbox-external test invocation.
- Independent read-only review found no blockers in cache layout, stable-ID offsets, visibility/error gating, cached math, CPU separation, allocation rollback, lifetime or publication.

Mesh shaders, tighter fragment coverage and compression remain outside this change.
