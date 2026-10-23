# Triangle-driven software raster prototype

The UE-inspired producer is an opt-in A/B path, selected with `DEREN_SOFTWARE_RASTER_TRIANGLES=1` in addition to the existing software-raster configuration. The default remains the tiled producer. The complete Subtilis system still comprises visibility buffer, Cluster DAG, HZB/recovery and optional software raster.

## Implementation

One 64-thread workgroup handles one unique software cluster; each lane prepares a triangle and visits pixel centers inside its clipped bounding rectangle. This removes the old tile loop that tested each candidate triangle against up to 256 pixels. Floating-point coverage and depth interpolation use the same helper as the old producer; the previously rejected affine arithmetic has not been reintroduced.

A private device-local uint64 buffer per frame slot stores `(depth_bits << 32) | packed_id`, cleared to all ones. Atomic min preserves nearest normal-Z depth and minimum software ID on equal depth as one indivisible candidate. Separate clear, raster and unpack dispatches have explicit buffer barriers. Unpack writes the existing depth/ID storage images, so graph outputs, resolve, merge and hardware tie preference stay unchanged. Scratch costs 8 bytes/pixel per allocated frame slot (7.03 MiB each at 1280x720).

RHI ABI 47 exposes **enabled** buffer int64 atomics (conservative false by default). Unsupported capabilities, missing pipeline, scratch allocation/address/descriptor failure refuse preparation before hardware runs are shortened. A failed list publication restores the full hardware runs. Host allocation OOM retains the project's existing fatal policy. The prototype retains CPU classification/CSR construction and deduplicates its entries; removing this CPU work is a separate optimization, not a benefit claimed here.

UE references: local `Engine/Shaders/Private/Nanite/NaniteRasterizer.usf`, `NaniteRasterizer.ush`, `NaniteWritePixel.ush` and `ClusterCulling.usf` in the supplied UE 5.8.2 checkout. Deren retains normal Z and its own floating coverage contract; UE's reversed-Z maximum and fixed-subpixel coverage were not copied.

## Correctness and validation

All captures below enabled Vulkan validation and used 1280x720, fixed camera `(0,0,4,0,0,0)`, diagnostic threshold 4096 px, HZB disabled for the CPU oracle. Frames 8 and 9 cover both frame-ring slots. This large threshold is a correctness control, not the performance setting.

| Fixture | Covered pixels | CPU/GPU IDs | Maximum depth difference | Repeat across slots |
| --- | ---: | --- | ---: | --- |
| Planar grid | 117,180 | Exact | 2 ULP | Exact GPU depth/ID |
| Sloped grid (0.25) | 109,327 | Exact | 3 ULP | Exact GPU depth/ID |
| Coplanar duplicate items plus farther overlapping item | 119,734 | Exact | 2 ULP | Exact GPU depth/ID |

Planar GPU depth/ID also exactly match the previous tiled producer. The overlap fixture exercises inter-workgroup contention and equal-depth ID ordering. Validation reported no errors. Raw prefixes: `tmp/sw-triangle-{8,9}`, `tmp/sw-triangle-slope-{8,9}`, `tmp/sw-triangle-tie-{8,9}`. These are software-producer comparisons; they do not constitute full-stage hardware/resolve/merge acceptance.

Helper tests cover unique dispatch sorting/deduplication, malformed counts/overflow rejection and uint64 candidate ordering. Explicit prototype preparation without device/pipeline refuses both slots. The missing helpers first failed to compile, and missing-device assertions first failed before the fix. Full build succeeded with 14 workers; full CTest passed **66/66**. Fresh independent review found no Important/Critical defect; two preexisting-style comments describing tiled-only recording/two scratch slots remain deferred documentation polish.

## Performance

Same textured compatibility asset used by the corrected acceptance matrix, 1280x720, threshold 1 px, DAG+HZB/recovery active, other rendering settings held fixed. Three interleaved rounds per mode/camera, 130 capture frames / 129 GPU samples each, 18 runs total. Modes used the same executable and shader bundle; hashes were checked unchanged at completion. Values are medians of three run means. [Raw rows, settings, image hashes and build identities](2026-10-23-software-triangle-results.json).

| Mode | Far GPU frame | Near GPU frame | Far software pass | Near software pass | Far CPU-active | Near CPU-active |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| DAG+HZB, software off | 10.31 ms | 5.46 ms | — | — | 7.27 ms | 6.90 ms |
| DAG+HZB + tiled software | 11.84 ms | 5.86 ms | 2.33 ms | 0.18 ms | 8.65 ms | 8.02 ms |
| DAG+HZB + triangle software | 10.52 ms | 5.53 ms | 0.10 ms | 0.09 ms | 8.36 ms | 7.88 ms |

The triangle producer reduces the measured software pass by **95.7% far (23.3x)** and **50% near (2x)**. Whole-frame GPU time improves **11.1% far / 5.6% near versus the old software path**. This is not a 23x frame-speed claim.

Against software off, triangle-mode GPU medians remain +0.21 ms far / +0.07 ms near, within visible run-to-run variation; CPU-active remains +1.09 / +0.98 ms. Far triangle GPU run means ranged 9.28–10.54 ms versus hardware 10.17–10.52 ms; near ranged 5.41–5.74 versus hardware 5.15–5.64 ms. These data establish a much cheaper producer but **do not establish a stable net win for hybrid rendering**. Software stays default-off, triangle mode stays opt-in, and this result does not supersede the separate historical legacy/geometry comparison.

Bistro far screenshots match exactly between tiled/triangle modes in all three rounds. Near final-color screenshots have two versions differing at 14,823 pixels, with mean absolute channel differences 0.00862/0.00858/0.00844 on a 0–255 scale; the same two hashes also appear in repeated **software-off** captures. An additional validation-enabled 130-frame near capture per software mode read back the actual software depth/ID: both match exactly (depth SHA256 `c1a278749cec50862b38ddd306291242df759ab458eaf3ba194604ac2756128a`, ID `0c660f2bd3eff3150dd0040789abe2291613b9af319df870203d4f77a4913a5f`). No validation errors. Thus the final-color variation is not evidence of different software-producer images; its broader hardware/HZB/shading cause is not resolved by this experiment. Raw outputs/logs live under `tmp/software-triangle-near-raw`.

CPU classification and CSR building still cost approximately 2.28/1.91 ms wall time with triangle mode versus 1.19/1.03 ms without software. The experiment deliberately isolated GPU work distribution; replacing CPU tile binning with direct cluster routing and then considering GPU-side routing is the next cost target. No broader geometry or full-stage correctness claim follows from these performance captures.
