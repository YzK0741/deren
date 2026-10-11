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

`test_gaussian_splatting` is headless and included in normal CTest and the ASan/UBSan build list. It checks fixed byte layouts, analytic rotated covariance, SH bands, conservative bounds, invalid inputs, budgets, transforms, multi-instance sorting/ties and RHI descriptor/bytes/failure cleanup using fake buffers. GPU shader interpretation and actual draws still require the next-stage layout probe and HDR readback tests.

## Raster snapshots and pass

`make_raster_asset(core, gaussian::asset by value, limits)` validates, packs and uploads one immutable CPU/GPU pair. Move the loaded asset into this factory to avoid copying its CPU particles. The returned shared asset retains CPU source, packed geometry and static GPU buffers. Assets, frame uploads and pipelines must use the same device; the backend root outlives all owners.

`prepare_raster_frame(core, span<raster_instance const>, raster_camera, limits)` validates input, globally sorts references, rejects support outside the frustum and omits support intersecting the near plane. It uploads an immutable coherent instance table and visible references with initial bytes. The byte budget includes both uploads; the existing sorting scratch and retained CPU sources are separate. Invalid inputs fail before frame GPU allocation. A frame with no visible splats uploads nothing. Failures release partial uploads.

`raster_camera` contains the real VP matrix, world camera position and pixel viewport. The accepted camera is a finite, nonsingular forward-depth [0,1] perspective projection with finite far plane; orthographic, reversed depth and infinite far are rejected. Include the engine's projection Y flip in the matrix. Model transforms retain the positive uniform scale restriction. Projection uses the actual VP Jacobian in pixel units, 0.3 pixel² covariance regularization and a Cholesky square root. CPU helpers `project_splat`, `evaluate_sh` and `fragment_alpha` provide numerical references.

The Slang pair reads 48/192/160/8-byte BDA records and a 112-byte push block. Explicit VP/model columns match GLM storage. SH degree 0..3 uses the local camera-to-center direction, adds 0.5 and clamps negative radiance without an upper HDR clamp. Six vertices form each splat quad. The fragment kernel uses radius² <=9, alpha <=0.99 and discards alpha <1/255. Back-to-front drawing uses unpremultiplied color with ordinary alpha blending.

Create `gaussian_splatting_pass` with the HDR target format (default RGBA16F), supply its two registered shaders through `pass_context`, then call `set_frame(snapshot)`. Resolve the existing scene-color and opaque-depth targets with the snapshot's viewport and a push-data callback. The pass LOADs both attachments, disables face culling and depth writes, depth-tests the center, draws six vertices per visible reference and returns depth to `shader_read`. It reports recording refusals through `last_error()` and performs no draw on failed prerequisites.

The pass owns its currently assigned snapshot only. The render host must separately keep every submitted snapshot until GPU completion before replacing/releasing it; use completion slots, not swapchain image indices. Immutable uploads avoid overwriting pending device addresses, with a per-frame allocation cost to measure at viewer integration. Snapshot preparation and the normal pass never call `wait_idle`.

Headless `test_gaussian_raster` checks mathematical references, a rotated/transformed off-center camera against numerical differentiation, snapshot sorting/culling/budgets/unload retention, failed uploads and pass recording behavior. Both shaders are built by CMake and the standalone scripts and loaded by the app. SPIR-V validation confirms push offsets 0/16/32/48/64/80/88/96/100/104/108. Actual GPU output is checked by the subsequent isolated probe; headless checks alone do not prove rendering.
