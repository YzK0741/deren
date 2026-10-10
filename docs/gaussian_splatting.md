# Gaussian rendering foundation

Import `deren.engine.gaussian_splatting` and `deren.gaussian_loader`. The engine namespace is `deren::engine::gaussian_splatting`.

This module supplies CPU packing, global sorting and static GPU upload. It does not yet draw Gaussian assets; projection, SH evaluation, the Slang raster shader, pass integration and a viewer are the next stage.

## Data and validation

`pack_asset(asset, limits)` returns `std::expected<packed_asset, foundation_error>`. It validates manual assets as well as loader output: finite centers, positive scales, quaternion wxyz norm within 1e-4, opacity in [0,1], finite SH and degree 0..3. Quaternions within the accepted tolerance are normalized for covariance computation. Unused SH bands are zeroed; non-finite unused coefficients are still rejected.

GPU records use float32 with compile-time size/offset guards:

| Record | Bytes | Contents |
| --- | --- | --- |
| geometry_record | 48 | center/opacity; xx/xy/xz/0; yy/yz/zz/0 |
| sh_record | 192 | 48 coefficient-major RGB scalars |
| instance_record | 160 | two uint64 addresses; model at 16; inverse model at 80; degree at 144; padding at 148 |
| draw_reference | 8 | instance index; local particle index |

Geometry and instance records have 16-byte alignment. Matrix storage is the existing column-major GLM convention. Padding is zero initialized. `instance_record` is a layout contract in this stage, not an automatically uploaded table.

Covariance is computed in double precision, then checked for finite float32 representation. Bounds are recomputed from the 3-sigma support; user-provided CPU bounds are ignored. Conditional outward rounding also handles a radius lost when added to a very large center. If finite conservative float bounds cannot represent the support, packing fails.

Default `foundation_limits` allow 4,000,000 references/particles and 1 GiB packed output. `packed_byte_size(count, limits)` checks count and multiplication before allocation; count additionally cannot exceed uint32. Packed bytes are 240 per particle. These limits describe allocation budgets, not a frame-rate promise. The process follows the existing no-exceptions allocation policy: an allocator exhausting host memory can terminate; structured budget errors do not guarantee recovery from OS allocation failure.

## Global sorting

`sort_draw_references(span<sort_instance const>, sort_camera, limits)` consumes borrowed CPU asset pointers only during the call. An instance is an asset, model matrix and unique uint64 stable ID; empty instances also need unique IDs. The camera needs finite position and a unit forward vector within 1e-5.

`validate_model(model)` returns the inverse. Accepted transforms are affine translation, rotation and positive uniform scale. It rejects non-finite values, non-affine bottom rows, zero scale, reflection, shear, non-uniform scale or a non-finite float32 inverse. Scale equality and axis orthogonality use relative tolerance 1e-5.

Sort keys use double world-center depth along camera forward. The complete scene is sorted far to near, then stable instance ID and local index. Output indices refer to the supplied instance table, even if input order changes. Static attributes are not moved. Invalid assets, transforms, cameras, non-finite/unrepresentable world centers and excessive counts/budgets produce errors.

This stage emits every valid particle, including particles behind the camera. It does not perform frustum or near-plane culling. The raster stage must apply conservative support-domain visibility and the planned near-plane omission policy.

Reference output is 8 bytes per particle and is checked against the byte budget. Sorting additionally uses a 16-byte key/reference per particle and 8-byte stable ID per instance; this temporary scratch is separate from that output budget. Caller-owned CPU assets and instance tables are also separate.

## Static GPU upload and ownership

`upload_asset(core, packed, limits)` returns `expected<shared_ptr<gpu_asset const>, foundation_error>`. It uses only the existing RHI APIs: GPU-only storage buffers, initial bytes and `device_address`. A nonempty asset on a backend without that extension returns unsupported. Empty assets create no buffers and have zero addresses.

Packed input is validated again: counts, degree, finite geometry/SH, opacity, padding, unused SH bands, valid bounds and positive-semidefinite covariance. Covariance principal minors and determinant use normalized tolerance 1e-6 to allow float rounding. GPU bounds are recomputed conservatively from the actual packed covariance, so they may differ slightly from CPU bounds. Supplied bounds must be finite, ordered and contain centers; narrowing them cannot narrow the published GPU bounds.

Both buffers must be created successfully with adequate sizes and valid aligned nonzero addresses. Allocation or address failure releases earlier handles and publishes no partial asset. Initial bytes are borrowed only by buffer creation. `gpu_asset` exposes count, degree, byte size, addresses and bounds and retains the buffers through the existing RHI owning handles. Dropping the last shared asset reference releases each owned reference once.

The engine/device/backend root must outlive these buffer owners. This asset does not keep a backend DLL loaded on its own. Future frame snapshots must hold shared GPU assets until their frame slot has completed; factory ownership is not GPU submission lifetime tracking. This stage records no commands and never calls wait_idle.

## Validation

`test_gaussian_splatting` is headless and included in normal CTest and the ASan/UBSan build list. It checks fixed byte layouts, analytic rotated covariance, SH bands, conservative bounds, invalid inputs, budgets, transforms, multi-instance sorting/ties and RHI descriptor/bytes/failure cleanup using fake buffers. GPU shader interpretation and actual draws still require the next-stage layout probe and HDR readback tests.
