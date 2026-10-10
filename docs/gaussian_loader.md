# Gaussian PLY loader

Module `deren.gaussian_loader`, CMake target `gaussian_loader`: standalone CPU asset loading, without engine, Vulkan or CUDA dependencies.

```cpp
import deren.gaussian_loader;
auto result = deren::gaussian::load_ply("scene.ply");
if (!result) {
    // result.error(): code, byte_offset, property, message
} else {
    // particles: center, positive scale, normalized wxyz rotation, opacity, SH
    // sh_degree and bounds_min/bounds_max (3-sigma support)
}
```

`parse_ply(span<const byte>, options)` also accepts memory. Both APIs return `std::expected<asset, diagnostic>`.
Supported PLY 1.0 encodings: ASCII and binary little endian. Only one vertex element and scalar properties are supported. Required fields: x/y/z, scale_0..2, rot_0..3, opacity, f_dc_0..2; optional complete f_rest_0..N sequence determines SH degree0..3. Unknown scalar attributes are consumed; lists and other elements are rejected.

Values use the INRIA convention: exp scale, stable sigmoid opacity, normalized wxyz quaternion, channel-major file SH converted to coefficient-major RGB. Unused entries in the 48-float SH array are zero. Bounds reflect rotated 3-sigma ellipsoids. Empty assets have zero bounds.

Default limits: 4,000,000 particles, 1 GiB file bytes, 1 GiB output bytes. Header is limited to64 KiB and256 attributes. Count arithmetic and binary payload length are checked before particle allocation. Budgets concern file and resulting asset, not total process peak memory. STL allocation failure retains the project's existing no-exceptions behavior; these limits do not promise recoverable system OOM.

Malformed fields, nonfinite values, zero quaternions, unusable float32 scales/covariance, trailing payload, truncation and exceeded limits return errors. No partially converted asset is returned. Diagnostics identify byte offset and property when available.

This module does not upload resources or display a scene. GPU packing and rendering follow the Gaussian design specification separately.
