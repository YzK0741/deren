# Gaussian splatting

The viewer loads trained Gaussian PLY assets and renders them through the normal frame chain. GPU preparation, VRDX stable sorting, a projection/color cache and indirect drawing are enabled by default when supported. A CPU reference path remains available. This chapter describes the implementation as of 2026-10-12; earlier development notes are historical evidence, not the current operating instructions.

## Run the viewer

Set the top-level `model` to a trained `.ply` file. The extension selects the loader: `.gltf` and `.glb` use the mesh importer; `.ply` uses the Gaussian loader. There is no separate Gaussian enable key. The loader contract is described in \ref md_docs_2gaussian__loader "Gaussian PLY loader".

```toml
model = "gaussian-viewer/flowers/flowers_1/flowers_1.ply"
grid_side = 0
[render]
window_width = 1280
window_height = 720
taa = false
[lighting]
sun_direction = [0.0, 0.25, 1.0]
```

NVIDIA's Bouquet of Flowers is the acceptance asset: 562,974 particles, SH degree 3, CC BY-SA 4.0, credited to Jean-Eudes Marvie. Fetch it with `scripts/fetch_gaussian_viewer_asset.ps1`; the large asset is not committed. A missing or malformed file is reported at startup rather than presented as a successful blank scene.

The camera is fitted to the cloud's conservative bounds. `--capture-camera` overrides the fit for reproducible images. The bouquet viewer aligns its local -Y flower face toward the configured sky sun while rotating about the bounds center. This is a placement convention for the bouquet, not automatic front-face detection for arbitrary assets. SH appearance includes captured lighting; changing the sky sun does not physically relight the cloud.

Scripted `--capture-frames` runs render directly into ordinary images without creating a window,
surface or swapchain. `[render] offscreen = true` selects that startup mode explicitly. Windowed
presentation overlap is controlled by `[render] async_present` in `config.toml` (default false).
The resize regression helper explicitly requests `--capture-windowed` because it exercises real
window restoration. See [the dynamic API](dynamic_runtime.md) for the null-window contract.

## Select and diagnose a path

| Environment variable | Purpose |
| --- | --- |
| `DEREN_GAUSSIAN_PATH` | `auto` (default), `cpu`, or `gpu` |
| `DEREN_GAUSSIAN_PROBE=1` | Original CPU-prepared GPU raster oracle |
| `DEREN_GPU_SORT_PROBE=1` | Native stable key/value sorting oracle |
| `DEREN_GAUSSIAN_GPU_PROBE=1` | GPU preparation, references, indirect count and pixels |
| `DEREN_GAUSSIAN_BENCHMARK` | Path for buffered timing CSV |
| `DEREN_GAUSSIAN_HDR_CAPTURE` | Path for requested pre-post RGBA16F capture |
| `DEREN_GAUSSIAN_CPU_TRACE` | Opt-in per-frame CPU preparation log |

`auto` uses GPU preparation when its requirements are met and falls back to CPU on initialization failure. Forced `gpu` reports failure without silently using the CPU path. Startup logs identify the actual path and candidate count. Recording refusals are consumed through the stage collection seam and logged; failed frames do not reuse an old Gaussian snapshot.

The normal path does not wait for readback. Probes and screenshots explicitly wait for their own submissions and should be disabled during performance measurements.

## Frame pipeline and ownership

Static geometry and SH tables are validated and uploaded once. The CPU updates only camera and instance metadata on the GPU path. Each completed frame-ring slot owns its dynamic buffers and retains every asset used by that submission; a swapchain image index is not a completion slot.

The GPU frame records these operations before raster rendering:

1. Reset four status counters and the indirect draw command.
2. Prepare fixed candidate slots in stable instance-ID/local-index order. Validate transformed data, test visibility, generate float32 forward-depth keys, and cache visible projections and colors.
3. Sort uint32 keys with original candidate indices as values. Float flip of negative forward depth produces far-to-near order; invisible keys are UINT32_MAX. Stable sorting preserves ties without moving static attributes.
4. Expand the visible prefix into instance/local references and write a 16-byte draw command. Any numeric error suppresses the entire Gaussian draw.
5. Publish references and cached projections for shader reads and arguments for indirect reads. Rebind pipelines and push data after sorting.

The pass runs after lighting and before transparent meshes. It LOADs lit HDR color and opaque depth, tests center depth, disables depth writes and culling, then draws six vertices per visible reference. The host `visible_count` is a candidate bound on the GPU path; the indirect command supplies the actual visible draw count. Completed statistics are read only after the slot's completion point.

The engine/device/backend root must outlive its resource owners. Slot growth allocates replacements before publishing them, includes retained capacity and queried sorting scratch in its byte budget, and releases partial allocations on failure. No pending slot may be reused before completion.

## Modules and entry points

- `deren.gaussian_loader`: `load_ply` and `parse_ply`, standalone CPU loading with structured diagnostics.
- `deren.engine.gaussian_splatting`: `pack_asset`, `upload_asset`, `validate_model` and `sort_draw_references`.
- `deren.engine.gaussian_splatting.raster`: `make_raster_asset`, `prepare_raster_frame`, `project_splat`, `evaluate_sh` and `fragment_alpha`.
- `deren.engine.gaussian_splatting.gpu_frame`: `prepare_gpu_frame`, preparation command recording and completed-slot statistics.
- `deren.engine.pass.gaussian_splatting`: mutually exclusive CPU/GPU snapshot setters and `last_error`.

The asset factory retains both validated CPU data and immutable GPU tables. The CPU frame factory globally sorts in double precision, evaluates conservative visibility and uploads references. The GPU factory validates camera/instance metadata, prepares reusable slot resources and records the compute/sort/finalize sequence. It returns an owning shared frame; pipeline pointers used during recording are borrowed from the pass.

## Data layout and rendering rules

| Record | Bytes | Main contents |
| --- | ---: | --- |
| geometry_record | 48 | Center, opacity, six covariance entries |
| sh_record | 192 | 48 coefficient-major RGB scalars |
| instance_record | 160 | Geometry/SH addresses, model/inverse, SH degree |
| draw_reference | 8 | Original instance index and local particle index |
| GPU projection cache | 48 | Three float4 values: center, ellipse basis, color/opacity |
| raster_push | 112 | VP, camera, addresses, viewport, count and flags |
| GPU preparation parameters | 176 | Raster parameters, forward, ranges and buffer addresses |
| indirect draw command | 16 | Vertex count, instance count and first indices |

Geometry/instance/cache records use 16-byte alignment and column-major GLM transforms. Raster push data is followed by three heap lanes at byte offsets 112, 116 and 120, making the transmitted block 124 bytes. Preparation pushes only the address of its 176-byte parameter buffer plus the heap lanes.

Raster flag bit 0 enables the GPU projection cache. Reserved instance words 37/38 (byte offsets 148/152) carry that instance's cache base address; the CPU instance path leaves padding zero. References and their original instance/local index semantics stay unchanged. Preparation parameters use uncached projection, then the six raster vertices read its float32 results. Unwritten invisible cache entries are never referenced.

Packing accepts finite centers, positive scales, opacity in [0,1], finite SH degree 0..3 and wxyz quaternions within 1e-4 of unit norm. Accepted quaternions are normalized; unused SH bands are zeroed. Covariance and conservative 3-sigma bounds are computed and checked before upload. Packed GPU input is validated again, including padding and positive-semidefinite covariance tolerance.

Instances require unique uint64 stable IDs. Model transforms allow affine translation, rotation and positive uniform scale; reflection, shear, nonuniform/zero scale and nonfinite inverses are rejected. Cameras require a finite, nonsingular perspective projection with forward depth [0,1], finite far plane and the engine's projection Y flip. Orthographic, reversed-depth and infinite-far projections are not supported.

Projection uses the actual VP Jacobian in pixel units, covariance regularization of 0.3 pixel squared and a Cholesky ellipse basis. Side rejection uses the regularized footprint. Near-plane-intersecting support is omitted. SH degree 0..3 uses the local camera-to-center direction, adds 0.5 and clamps negative color without clamping HDR highlights. The fragment support is radius squared <=9, alpha is capped at 0.99, and alpha below 1/255 is discarded. Composition uses ordinary unpremultiplied back-to-front alpha blending.

## VRDX and memory budgets

VRDX 0.6.0 is vendored unchanged in `source/third_party/vrdx`, with its MIT license and provenance. Its pinned revision is:

```text
b0db22b0c65e17a9b803bc4daf20354898b820eb
```

The backend wraps VRDX through the optional RHI sorting extension; engine code does not include Vulkan or VRDX headers. RHI ABI is 33 (`deren::promise::rhi::abi_version`, `source/promise/rhi/rhi.contract.cppm`).

The Vulkan 1.3 implementation checks synchronization2, VK_KHR_push_descriptor, subgroup operations, workgroup size and shared-memory limits. Sorting scratch uses the requirements returned by the backend; zero scratch size for a nonzero count means unsupported. Traditional sorting descriptors invalidate heap bindings, so the wrapper restores the core's heaps and subsequent stages rebind their state.

Default foundation budgets are 4,000,000 particles/references and 1 GiB packed output. Static packed data costs 240 bytes per particle. Dynamic GPU budgets include instances, ranges, keys, values, references, projection cache, scratch, parameters, counters and draw arguments. One-X-dispatch preparation also caps candidates at 65,535 * 128. Structured budget errors do not promise recovery from system host-memory exhaustion.

The flower asset has 135,113,760 bytes of static GPU tables. With projection caching, requested dynamic capacity is 40,680,448 bytes per slot, 81,360,896 for two slots. The cache alone adds 27,022,752 bytes per slot. Immutable CPU assets and allocator block overhead are separate.

## Measurements and their limits

Measurements below use an RTX 4060 Laptop GPU, driver 617.42, Release, 562,974 SH3 particles and 1280x720. GUI, TAA, VSync, validation and frame cap were disabled; each path discarded 60 warmup frames and sampled 300 frames, with three static and three rotating runs. Rotation advanced 0.5 degrees per frame.

| Change | Measured result |
| --- | --- |
| CPU to GPU preparation/sort | Frame median 116.8-157.8 ms to 5.89-7.12 ms; 19.5-26.6x |
| CPU preparation work | About 92 ms to about 0.002 ms of instance/camera metadata |
| Projection/color cache | Gaussian GPU interval total reduced 14-28% in six pairs |
| Cached raster stage | About 1.62-1.66 ms to 0.87-1.12 ms; 31-47% reduction |

These are historical measurements taken before the later Bloom pipeline wiring repair. That repair restores real Bloom work, so the earlier whole-frame times are not current FPS guarantees. Cache tests show a repeatable reduction in Gaussian GPU work; scheduling/presentation variation prevented an equivalent whole-frame FPS claim. The two optimizations' percentages must not be multiplied into an unmeasured combined speedup.

Six static/rotating before/after cache PNGs are pixel-exact; an additional pre-post RGBA16F capture is byte-exact. CPU/GPU float32 versus double ordering is not claimed identical for the full flower scene. Acceptance uses maximum RGB error <=0.005 for small scenes, flower HDR RGB RMSE <=0.005 and final per-channel RGB RMSE <=1/255; local differences are recorded alongside averages.

## Regression checks and repaired failures

Headless tests cover loading, layouts, covariance/SH math, camera/model validation, budgets, sorting, allocation rollback, resource retention, indirect recording and runtime injection. Native probes cover 48 stable-sort cases and 11 GPU raster scenes; the latter compares 10,626 pixels with maximum HDR RGB error 0.0008074641, maximum depth difference 1 float32 ULP and zero small-sample double-order differences. Clean validation alone is not a pixel-correctness test.

Reproduce image and performance checks with the Windows helpers:

```powershell
$build = 'build-release-dyn-clang64'
$scripts = 'scripts/windows'
& "$scripts/check_gaussian_gpu.ps1" -BuildDir $build
& "$scripts/benchmark_gaussian_gpu.ps1" -BuildDir $build
& "$scripts/check_gaussian_resize.ps1" -BuildDir $build `
    -Config '<viewer.toml>' -MaximizeRestore -EarlyCapture
```

The resize helper also accepts `-RapidResize` for twenty actions and `-Camera` for a fixed capture pose. Inspect early restored frames as well as final output. The following fixes are part of the current viewer:

| Failure | Current behavior |
| --- | --- |
| Empty mesh scene depended on a placeholder triangle | Shared resources are prepared before frame acquire; geometry-free scenes clear color/depth |
| Camera movement exposed stale background/depth | The scene pass records a clear-only rendering scope when no opaque meshes are visible |
| Swapchain transition preceded the acquire wait | Initial transitions chain the COLOR_ATTACHMENT_OUTPUT semaphore wait |
| VRDX traditional descriptors invalidated heap state | Heap bindings are restored and later pipelines/push data rebound |
| Recording errors were not collected | Invoked Gaussian passes report refused recording through the stage collection seam |
| Left gray band after maximize/restore | Named shared-pipeline lookup restores all four Bloom passes before the composite reads them |

The gray-band diagnosis compared healthy pre-post HDR with corrupted final PNGs, ruled out GUI draw submission and observed four unresolved Bloom passes at nonzero intensity. The runtime now searches both its legacy registry and the attached passes' named pipelines. Five early restored captures, a final early capture and twenty maximize/restore actions were clean, with full CTest 24/24 and independent review. Bloom now works as configured: 13 of 14 old frozen images change; omitting only Bloom addition in an isolated diagnostic matches all 14. References were not updated to conceal this intended visual change.

Detailed measurements and historical investigations remain in the repository's `docs/reviews` records for GPU frame acceptance, projection cache acceptance, empty-scene resources, camera clears, resize synchronization and maximize/restore. Rejected hypotheses and withdrawn image-difference metrics are not current guidance and are not repeated in this chapter.

## Current boundaries

Center-depth global sorting is an approximation, not per-pixel volumetric order. Near-plane support is omitted rather than clipped. The current renderer does not provide Gaussian PBR relighting, Gaussian-generated mesh shadows, mesh-shader splats, SH compression, asynchronous old-camera sorting, stochastic transparency or a Gaussian-specific TAA treatment. Their absence is not covered by the measured performance claims.
