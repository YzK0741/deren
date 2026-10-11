# Gaussian rendering

Import `deren.engine.gaussian_splatting` and `deren.gaussian_loader`. The engine namespace is `deren::engine::gaussian_splatting`.

The foundation module supplies CPU packing, global sorting and static GPU upload. Import `deren.engine.gaussian_splatting.raster` for validated projection, SH evaluation and immutable draw snapshots, and `deren.engine.pass.gaussian_splatting` for the dedicated raster pass. Normal viewer/frame-chain integration is a subsequent stage.

## Data and validation

`pack_asset(asset, limits)` returns `std::expected<packed_asset, foundation_error>`. It validates manual assets as well as loader output: finite centers, positive scales, quaternion wxyz norm within 1e-4, opacity in [0,1], finite SH and degree 0..3. Quaternions within the accepted tolerance are normalized for covariance computation. Unused SH bands are zeroed; non-finite unused coefficients are still rejected.

GPU records use float32 with compile-time size/offset guards:

| Record | Bytes | Contents |
| --- | --- | --- |
| geometry_record | 48 | center/opacity; xx/xy/xz/0; yy/yz/zz/0 |
| sh_record | 192 | 48 coefficient-major RGB scalars |
| instance_record | 160 | two uint64 addresses; model at 16; inverse model at 80; degree at 144; padding at 148 |
| draw_reference | 8 | instance index; local particle index |

Geometry and instance records have 16-byte alignment. Matrix storage is the existing column-major GLM convention. Padding is zero initialized. `prepare_raster_frame` uploads the instance table without changing that layout.

Covariance is computed in double precision, then checked for finite float32 representation. Bounds are recomputed from the 3-sigma support; user-provided CPU bounds are ignored. Conditional outward rounding also handles a radius lost when added to a very large center. If finite conservative float bounds cannot represent the support, packing fails.

Default `foundation_limits` allow 4,000,000 references/particles and 1 GiB packed output. `packed_byte_size(count, limits)` checks count and multiplication before allocation; count additionally cannot exceed uint32. Packed bytes are 240 per particle. These limits describe allocation budgets, not a frame-rate promise. The process follows the existing no-exceptions allocation policy: an allocator exhausting host memory can terminate; structured budget errors do not guarantee recovery from OS allocation failure.

## Global sorting

`sort_draw_references(span<sort_instance const>, sort_camera, limits)` consumes borrowed CPU asset pointers only during the call. An instance is an asset, model matrix and unique uint64 stable ID; empty instances also need unique IDs. The camera needs finite position and a unit forward vector within 1e-5.

`validate_model(model)` returns the inverse. Accepted transforms are affine translation, rotation and positive uniform scale. It rejects non-finite values, non-affine bottom rows, zero scale, reflection, shear, non-uniform scale or a non-finite float32 inverse. Scale equality and axis orthogonality use relative tolerance 1e-5.

Sort keys use double world-center depth along camera forward. The complete scene is sorted far to near, then stable instance ID and local index. Output indices refer to the supplied instance table, even if input order changes. Static attributes are not moved. Invalid assets, transforms, cameras, non-finite/unrepresentable world centers and excessive counts/budgets produce errors. Count/budget checks and particle/transformed-center validation finish before allocating scene-sized sorting scratch.

This stage emits every valid particle, including particles behind the camera. It does not perform frustum or near-plane culling. The raster stage must apply conservative support-domain visibility and the planned near-plane omission policy.

Reference output is 8 bytes per particle and is checked against the byte budget. Sorting additionally uses a 16-byte key/reference per particle and 8-byte stable ID per instance; this temporary scratch is separate from that output budget. Caller-owned CPU assets and instance tables are also separate.

## Static GPU upload and ownership

`upload_asset(core, packed, limits)` returns `expected<shared_ptr<gpu_asset const>, foundation_error>`. It uses only the existing RHI APIs: the upload-capable `uniform_gpu_only` memory intent plus the `storage` flag, initial bytes and `device_address`. `storage_gpu_only` is allocate-only and cannot receive initial bytes. A nonempty asset on a backend without that extension returns unsupported. Empty assets create no buffers and have zero addresses.

Packed input is validated again: counts, degree, finite geometry/SH, opacity, padding, unused SH bands, valid bounds and positive-semidefinite covariance. Covariance validation requires the normalized matrix plus 1e-6 times identity to be positive semidefinite. This bounds any negative eigenvalue directly by 1e-6 times the largest absolute entry, allowing float rounding without a separate tolerance on its principal minors or determinant. GPU bounds are recomputed conservatively from the actual packed covariance, so they may differ slightly from CPU bounds. Supplied bounds must be finite, ordered and contain centers; narrowing them cannot narrow the published GPU bounds.

Both buffers must be created successfully with adequate sizes and valid aligned nonzero addresses. Allocation or address failure releases earlier handles and publishes no partial asset. Initial bytes are borrowed only by buffer creation. `gpu_asset` exposes count, degree, byte size, addresses and bounds and retains the buffers through the existing RHI owning handles. Dropping the last shared asset reference releases each owned reference once.

The engine/device/backend root must outlive these buffer owners. This asset does not keep a backend DLL loaded on its own. Raster frame snapshots hold shared assets; the host must retain each submitted snapshot until its completion slot has finished; factory ownership is not GPU submission lifetime tracking. This stage records no commands and never calls wait_idle.

## Validation

`test_gaussian_splatting` is headless and included in normal CTest and the ASan/UBSan build list. It checks fixed byte layouts, analytic rotated covariance, SH bands, conservative bounds, invalid inputs, budgets, transforms, multi-instance sorting/ties and RHI descriptor/bytes/failure cleanup using fake buffers. The isolated raster probe checks actual shader interpretation and HDR readback as described below.

## Raster snapshots and pass

`make_raster_asset(core, gaussian::asset by value, limits)` validates, packs and uploads one immutable CPU/GPU pair. Move the loaded asset into this factory to avoid copying its CPU particles. The returned shared asset retains CPU source, packed geometry and static GPU buffers. Asset and frame objects cannot be copied, moved or assigned; share their immutable owning pointers. Assets, frame uploads and pipelines must use the same device; the backend root outlives all owners.

`prepare_raster_frame(core, span<raster_instance const>, raster_camera, limits)` validates input, globally sorts references, rejects support outside the frustum and omits support intersecting the near plane. Side culling uses the projected regularized 3-sigma footprint, preserving tiny edge splats; depth planes use the 3D support. It uploads an immutable coherent instance table and visible references with initial bytes. The byte budget includes both uploads; the existing sorting scratch and retained CPU sources are separate. Invalid inputs fail before frame GPU allocation, including active view-dependent SH radiance overflow. A frame with no visible splats uploads nothing. Failures release partial uploads.

`raster_camera` contains the real VP matrix, world camera position and pixel viewport. The accepted camera is a finite, nonsingular forward-depth [0,1] perspective projection with finite far plane; orthographic, reversed depth and infinite far are rejected. Include the engine's projection Y flip in the matrix. Model transforms retain the positive uniform scale restriction. Projection uses the actual VP Jacobian in pixel units, 0.3 pixel^2 covariance regularization and a Cholesky square root. CPU helpers `project_splat`, `evaluate_sh` and `fragment_alpha` provide numerical references.

The Slang pair reads 48/192/160/8-byte BDA records, and a push block whose host struct is 112 bytes. The transmitted block is larger: the runtime's heap push appends its index lanes after whatever a pass hands over (three for `push_stage_block`), so the shader declares them at 112/116/120 and the block it declares spans 124 bytes - leaving the 112-byte struct, and every offset the host asserts, unchanged. Explicit VP/model columns match GLM storage. SH degree 0..3 uses the local camera-to-center direction, adds 0.5 and clamps negative radiance without an upper HDR clamp. Six vertices form each splat quad. The fragment kernel uses radius^2 <=9, alpha <=0.99 and discards alpha <1/255. Back-to-front drawing uses unpremultiplied color with ordinary alpha blending.

Create `gaussian_splatting_pass` with the HDR target format (default RGBA16F), supply its two registered shaders through `pass_context`, then call `set_frame(snapshot)`. Resolve the existing scene-color and opaque-depth targets with the snapshot's viewport and a push-data callback. The pass LOADs both attachments, disables face culling and depth writes, depth-tests the center, draws six vertices per visible reference and returns depth to `shader_read`. It reports recording refusals through `last_error()` and performs no draw on failed prerequisites.

The pass owns its currently assigned snapshot only. The render host must separately keep every submitted snapshot until GPU completion before replacing/releasing it; use completion slots, not swapchain image indices. Immutable uploads avoid overwriting pending device addresses, with a per-frame allocation cost to measure at viewer integration. Snapshot preparation and the normal pass never call `wait_idle`.

Headless `test_gaussian_raster` checks mathematical references, a rotated/transformed off-center camera against numerical differentiation, the near/far plane agreement with the shader's own rejection (including a support that crosses either plane), snapshot sorting/culling/budgets/unload retention, failed uploads and pass recording behavior. Both shaders are built by CMake and the standalone scripts and loaded by the app. The push layout is pinned on both sides of the boundary: `test_gaussian_raster` asserts the host struct's size and offsets 64/80/88/96/104/108, and `test_shader_sources` asserts that a stage pushed by a lane-appending pass declares the index lanes (the emitted SPIR-V was checked by hand with `spirv-dis` at this revision: members at 0/16/32/48/64/80/88/96/100/104/108/112/116/120). The isolated probe below checks actual GPU output; headless checks alone do not prove rendering.

## Isolated GPU probe

`run_raster_probe(core, vertex_spirv, fragment_spirv)` renders five tiny RGBA16F scenes and compares all RGB pixels with the CPU reference. It uses only RHI creation, a primary command buffer, the pass and `image::get_content`. Cases cover known red/blue alpha 0.5 overlap (center RGB (0.5,0,0.25)), opaque depth occlusion, degree 1 SH direction, and degree 3 HDR anisotropy with two rotated/scaled instances, a noncentral camera and nonzero record indices, plus a tiny splat just outside the viewport whose regularized footprint reaches the last pixel. The absolute RGB tolerance is 0.005, covering RGBA16F storage and blend rounding.

This diagnostic explicitly waits for its own submissions; it is disabled during normal startup. Set `DEREN_GAUSSIAN_PROBE=1` to opt in, or run the hidden Windows helper with an existing valid app config:

```powershell
./source/scripts/windows/gaussian_raster_probe.ps1 -Config <config.toml> -BuildDir build-release-dyn-clang64
```

The config must locate this build's shaders and an available model, as for any app run. The helper runs two capture frames, requires a probe PASS and refuses validation errors. Its log lives under the build's `gaussian-raster-probe` directory (or `-WorkDir`). This does not wire Gaussian assets into the normal viewer or scene frame chain.

Verified 2026-10-11 on NVIDIA GeForce RTX 4060 Laptop GPU, driver 617.42: five cases / 5181 pixels, maximum RGB error 0.0008074641, no Vulkan validation errors. The clear-only negative run failed the overlap center with RGB (0,0,0), proving the readback comparison can detect a missing draw. The same run exposed and corrected the static factory's earlier allocate-only upload mismatch.

The projection/SH/raster approach follows NVIDIA's [vk_gaussian_splatting vertex reference](https://github.com/nvpro-samples/vk_gaussian_splatting/blob/main/shaders/threedgs_raster.vert.slang). Our record layout, Cholesky ellipse basis, 3-sigma domain and frame ownership follow this engine's design.

## The viewer: the whole path, and what it costs

THE `.ply` TO PIXELS PATH IS IN THE APPLICATION NOW, and it is one decision wide: the model path's extension picks the loader (`deren::app_config::model_format_of` - `.gltf`/`.glb` to the scene importer, `.ply` to `deren::gaussian::load_ply`, anything else refused by name). There is no `gaussian` configuration key and there is not meant to be one: the path IS the interface, so a second key could only drift away from it. The cloud is then packed onto the device once (`make_raster_asset`) and drawn every frame by a `gaussian` stage the runtime records between the lighting stage and the blended geometry - the order the design fixes, because the splats composite into the lit HDR image over the opaque depth and the blended meshes have to see them under themselves.

MEASURED ON THE ACCEPTANCE ASSET (NVIDIA's Bouquet of Flowers, `flowers_1.ply`, CC BY-SA 4.0, 562,974 splats, SH degree 3, 133.2 MB on disk; fetched rather than committed, by `source/scripts/fetch_gaussian_viewer_asset.ps1`):

| what | measured |
| --- | --- |
| `load_ply` | 0.4 s for the whole file |
| static tables | 562,974 geometry + 562,974 SH records = 135,113,760 bytes, uploaded once |
| the CPU frame, first version | 93 ms sort + 240 ms projection/SH = 334 ms, i.e. 2.9 fps on ONE thread |
| the CPU frame, now | 67 ms sort + 33 ms projection/SH = about 100 ms on 16 threads, i.e. 8-10 fps |
| validation | zero VUIDs with the layers on, at 1280x720 |

THE FRAME'S COST IS CPU-SIDE, WHICH IS WHY THE GPU LOOKS IDLE, and both halves are the design's own reference implementation: the depth sort and the per-splat projection/SH evaluation. They are now split across the machine's cores WITHOUT changing their answers - each worker fills its own storage and the results are concatenated in chunk order, and the sort's comparator is unchanged, so the draw order and the reported totals are the serial ones (the Gaussian tests pin that). What remains on the CPU is exactly what the design's out-of-scope item would remove: sorting and culling on the GPU.

THE FRAME'S ONLY OTHER LEAF IS A PLACEHOLDER, and it is scaffolding rather than scenery. Two measured reasons, and the second is why it is opaque:

* THE ENGINE CANNOT YET DRIVE A FRAME WITH NO IMPORTED GEOMETRY. With an empty scene the first frame's submission and the next frame's open both fail `11 = device_lost`, with zero VUIDs and an empty stderr, and unchanged by `shadow`, `clustered_lights`, `gui`, `unlit`, by removing the splat pass from the chain entirely, or by adding a geometry-carrying instancing grid.
* THE DEPTH BUFFER HAS TO BE WRITTEN BY SOMETHING OPAQUE. With a transparent placeholder the frame was legal and the splat pass recorded a full draw (562,974 visible splats, `record error 0`) and the image was still black, because every splat failed a depth test against a depth nothing had written.

So `deren::chores::add_placeholder_triangle` builds one triangle in the runtime's scene tree: opaque black, one unit across, sixty units behind the cloud (about seven pixels at the camera's 45 degree field, and inside the orbit camera's `max(100, ...)` far plane, because a clipped leaf is no opaque leaf). It is black and the frame's clear is black, so it is invisible wherever it lands - its first, full-frustum version was not, and it covered the sky until it was shrunk, which is how the background turned out to have been the environment all along rather than a clear colour.

TWO CORRECTNESS RULES EARNED THEIR COMMITS HERE, both from measurements rather than reviews:

* SNAPSHOT RETENTION IS BY FRAME SLOT. The design says snapshots are retained until their frame slot completes and are never keyed by the swapchain image index; holding ONE latest snapshot instead released the buffers of a frame the GPU could still be executing (`rhi::max_frames_in_flight` is 2), which showed up as blocky splats over an uninitialised background the moment a window resize put more frames in flight. `gaussian_frames` is one snapshot per slot now, indexed by the ring's position.
* A STAGE THAT SAMPLES OWES THE FRAME-ORDER DUTY. The splat stage reads the lit HDR image and the opaque depth, so it publishes their attachment writes the way the G-buffer samplers do. In a mixed scene the lighting stage has already done it; in a cloud-only one nothing else would.

WHAT IS STILL OPEN, in the order it matters: (1) a frame with no imported geometry must be drivable, and (2) the depth clear must not depend on a leaf being present - which together would let the placeholder go back to being transparent, or disappear. Both are engine work rather than viewer work.

### Known issue: the FIRST swapchain resize corrupts a cloud-only frame

REPRODUCED, and it is specific to this path rather than to resizing in general. Recipe: start the viewer with the cloud's path in `model`, let it settle, then resize the window once (any size) and look - the frame fills with blue and olive blocks (splats projected through garbage) with the bouquet pushed into a corner. Resizing a SECOND time clears it, which is the sharpest clue in the report: generation 0 to generation 1 is the transition that leaves something stale, and the next one is fine.

THE THREE RUNS THAT LOCALISE IT (same binary, same window, same scripted `MoveWindow` resize):

| run | config | after one resize |
| --- | --- | --- |
| cloud only (the viewer as shipped) | `model` = the `.ply` | CORRUPT |
| control 1 | `model` = `gltf_model/DamagedHelmet.gltf`, minimal config | clean |
| control 2 | THIS viewer config with `model` swapped to the helmet | clean |

So neither the resize path in general nor any key in the viewer's configuration is at fault: the difference is the cloud plus the placeholder - that is, a scene whose only leaf was created by `runtime::make_primitive` and a frame that draws splats, versus a scene built by `import_scene`. The suspected state is the per-generation publication of the scene's own data (the material/instance records `make_primitive` writes, and the push/heap binding the splat stage is resolved through) rather than any image the engine recreates, because the engine recreates those for every generation and a model frame proves that half works.

NOT YET FIXED. The two candidate fixes, both bounded: publish that state per generation in the runtime (the engine fix), or move the placeholder onto the import path, which is the route the controls above prove is generation-correct.

TWO MORE MEASUREMENTS NARROW IT FURTHER, both taken after the report above:

* IT IS NOT RARE. Four consecutive runs of the same scripted resize at four different sizes corrupted all four (a pixel difference of 40 to 111 against the pre-resize frame over the sky region, where a clean frame is 0 to 2). The single clean run that first suggested a race was the outlier, not the rule.
* IT IS UNINITIALISED MEMORY, NOT A STALE LEAF. The placeholder was temporarily given a bright red material as a probe: the corruption came out as dense random saturated noise (orange, green, blue, magenta) across the whole frame with only the GUI panel drawn correctly - not red. So what is on screen was never written by the frame's own passes: the leaf's data is not what is displayed, and the stale thing is the frame's TARGET/RESOLUTION path - a post/upscale or scene resolve that still names the previous generation's image after the swapchain is rebuilt.
* IT IS THE `make_primitive` LEAF, NOT THE SPLAT STAGE. Removing the splat pass from the chain entirely - so the frame draws no splat at all and the scene is that one placeholder leaf - still corrupts on the first resize (a sky-region difference of 64.5 against 0 to 2 for a clean frame). With the two controls above that leaves one variable standing: a leaf created by `runtime::make_primitive` at startup, whose scene data the generation rebuild does not republish, where `import_scene`'s does (which is why a model frame is immune).

THE FIXES, in the order they are worth doing: publish the scene's own data per generation in the runtime (`register_material`'s and `make_primitive`'s writes are the ones that survive only generation 0); or move the placeholder onto the import path, which the controls prove is generation-correct; or, as an interim only, re-add the placeholder after a detected swapchain recreation - correct because the new registration republishes the table, and ugly because it leaves one seven-pixel leaf behind per resize.

CORRECTION, AND THE PROBE THAT FOLLOWED IT: the import route was IMPLEMENTED (`add_placeholder_triangle` now builds a duck-typed node/drawable pair that satisfies `scene_drawable_iterator` and calls `import_scene`) and it does NOT fix the corruption - a scripted resize still measures 44.3 in the sky region, against 0 to 2 for a clean frame. So the `make_primitive`-versus-`import_scene` difference was NOT the cause, and that candidate fix is withdrawn.

WHAT THAT LEAVES: the placeholder's creation route is exonerated, the resize path is exonerated (the helmet controls are clean with this very configuration), and the configuration is exonerated (control 2 used all of it). The one variable left standing is THE CLOUD ASSET ITSELF: every corrupt run had the 135 MB of static tables loaded, and the run that removed the splat pass from the chain - so that the frame drew no splat at all - still corrupted, while both helmet controls (no cloud loaded) are clean. The next probe is therefore one line and already named: skip `set_gaussian_asset` (then skip `make_raster_asset`) and see which half of the cloud's presence matters.

### Retraction: the numbers above were measured with a metric that cannot tell corruption from a resize

EVERY RATE AND EVERY "STILL CORRUPTS" ABOVE IS WITHDRAWN, and the reason is a measurement error worth recording: the metric was the mean absolute pixel difference over a sky region between the frames before and after a resize. A resize moves what that region SHOWS - the sky is a gradient - so a perfectly clean resize measures 40 to 70, the same band the "corruption" was reported in. The decisive counter-example was produced by running the HELMET control three times and then LOOKING at the capture: the metric said 44.8 / 60.2 / 67.2 and the image was a clean helmet at 186 fps.

What survives is the VISUAL evidence only, and it is unambiguous: three captures of a cloud-only viewer after a resize show garbage (random saturated noise across the frame with only the GUI drawn, olive blocks over a blue field, and the user's own screenshot), while three captures of the helmet are clean. So the corruption is real and it appears in the cloud-only configuration - but "4 of 4 runs", "the splat stage is not involved", "the asset is exonerated" and "import_scene does not fix it" were all concluded from numbers in that unusable band, and each of them needs re-testing by looking at the image instead.

The lesson is the one this repository keeps re-learning: a metric that is not validated against a known-good case is not a measurement, it is a number. A validated metric here would compare the frame against a SAME-SIZE capture taken without any resize, not against itself before a resize.

### The validated metric was tried, and it fails its own validation

The same-size comparison was built and used: capture a frame from a run that STARTED at the target size (never resized) and compare it with a frame from a run resized to that size, same configuration, same camera. The numbers:

| comparison | difference |
| --- | --- |
| cloud, two clean frames, same size, neither resized (the BASELINE) | 29 |
| cloud, resized frame against the same-size reference | 72.9 |
| helmet, resized frame against the same-size reference | 51.1 |

The helmet's 51.1 is the disqualifying one: that frame was LOOKED at and it is a perfectly clean helmet at 239.9 fps. A resize changes the camera's aspect and what the frame shows, so "resized against not-resized" contains a legitimate difference of the same order as the corruption - the metric separates "was resized" from "was not", not "is corrupt" from "is clean".

SO THE EVIDENCE IS VISUAL ONLY, and on that evidence the corruption is real and it belongs to the cloud-only path: five captures of a cloud-only viewer after a resize show garbage (random saturated noise with only the GUI drawn, olive blocks over a blue field, blocky splats, and the user's own screenshot), while the helmet after a resize is clean in three captures - the last of them with EXACTLY this configuration and only the `model` line changed, which is the strongest control available here.

ONE MORE MISTAKE WORTH RECORDING, because it cost a wrong conclusion: an earlier "control 2" (this configuration with the model swapped to the helmet) never ran that configuration at all. The file had been produced by substituting the model path with `..\gltf_model\DamagedHelmet.gltf`, whose single backslashes are an INVALID TOML escape, so the parse failed and the run silently used the defaults. The valid control (`..\\gltf_model\\DamagedHelmet.gltf`, escaping the backslashes) is the one whose capture is clean above.

TO RUN IT: put the cloud's path in `model`, keep `gltf_model/DamagedHelmet.gltf` (or another model) findable from the working directory so the scene lookup succeeds, and start the viewer from the project root or the build directory. Left-drag orbits, the wheel zooms, F12 writes a screenshot through `[paths] screenshot_dir`, F1 toggles the panel and Escape quits; the panel's `gpu (60f)` and `cpu (60f)` rows are the costs measured above.
