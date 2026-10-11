# Gaussian raster pipeline implementation plan

> For agentic workers: use superpowers:executing-plans inline on master. The user requested continuing this pipeline on 2026-10-11 using the NVIDIA sample; this plan refines the existing approved raster design without changing its route.

Goal: provide a callable RHI Gaussian raster pass with CPU mathematical references, immutable frame snapshots and an offscreen correctness probe. Viewer/scene loading UI and frame-chain integration follow this batch.

Architecture: retain the existing geometry/SH/instance/reference layouts. Derive the pixel Jacobian directly from the real view-projection matrix. A dedicated Slang pair expands six vertices per sorted reference; a dedicated pass LOADs HDR color and opaque depth. Per-frame uploads are immutable snapshots retained until submission completes.

Tech Stack: C++23 modules, GLM, Slang, RHI, CMake/CTest.

Spec: ../specs/2026-10-10-gaussian-splatting-design.md, sections 4–7 and 9.
Reference: https://github.com/nvpro-samples/vk_gaussian_splatting/blob/main/shaders/threedgs_raster.vert.slang and threedgs_raster.frag.slang (read 2026-10-11). Use the math/reference approach with our established layouts and kernel parameters; do not copy the sample's descriptor model or matrix conventions.

## Global Constraints

- Direct master execution and push retain the user's authorization. Stop at 5% remaining quota and write progress.
- Preserve the unrelated Vulkan constructor change and preexisting probe DLL deletion.
- No backend types/imports, native escape, Gaussian-specific RHI types or ABI change.
- Push block exactly 112 bytes: VP 0, camera float4 64, reference address 80, instance address 88, viewport 96/100, count 104, flags 108.
- Geometry/SH/instance/reference strides remain 48/192/160/8 bytes.
- Forward depth [0,1] perspective; viewport Y comes from the supplied VP, which includes the engine's projection Y flip.
- Affine positive uniform model scales only. Global CPU order remains far to near with stable ties.
- Covariance regularization 0.3 pixel², 3σ domain r² <=9, alpha capped at 0.99, discard alpha <1/255; unpremultiplied HDR color.
- Omit support intersecting the near plane. Frustum rejection checks the entire support; preserve intersecting edge splats. Clamp a surviving center's representative far depth to 1.
- Disable culling and depth writes; depth test on. HDR and depth attachments LOAD/STORE, depth returned to shader_read.
- Immutable snapshots retain static assets and upload buffers. The host retains submitted snapshots until completion, using completion slots rather than swapchain image indices.
- No GPU sorting, TAA or viewer completion claim.

## Review Focus

- Public invalid camera/geometry/SH data must fail before frame-sized GPU allocations.
- Noncentral/anamorphic/rotated views must preserve Jacobian and matrix storage rather than silently transpose them.
- Near-plane/edge support and very small covariance must avoid invalid coverage and NaN.
- Cross-instance order and nonpremultiplied HDR blending must match a known overlapping sample.
- Failed uploads/recording must publish no half-frame; an unloaded static asset stays alive while a submitted snapshot is retained.

## Task 1: CPU raster reference and push layout

Files: create source/engine/gaussian_splatting/raster.cppm, raster.cpp, source/tests/test_gaussian_raster.cpp. Register module/test and ASan/strict analysis entries in CMakeLists.txt and .github/workflows/ci.yml.

Interfaces: raster_camera (VP, world position, viewport), raster_push, projection_visibility, projected_splat; validate_raster_camera -> expected<sort_camera, foundation_error>; project_splat(geometry_record, model, camera) -> expected<projected_splat, foundation_error>; evaluate_sh(sh_record, degree, normalized direction) -> expected<glm::vec3, foundation_error>; fragment_alpha(opacity, normalized_coordinate) -> float.

- [x] Add tests for the 112-byte offsets; identity perspective covariance (25.3,0,100.3), off-axis Jacobian (31.55,-3.125,101.8625), projection Y, uniform scale and rotated anisotropy.
- [x] Test near-plane omission, behind/outside support and a surviving edge splat; zero viewport, nonfinite/orthographic camera, malformed geometry/SH, SH direction/signs through degree 3 and HDR values above 1.
- [x] Test alpha at center, radius 3, beyond radius 3, cap, threshold and invalid input.
- [x] Run the new executable with scaffolding returning unsupported: expect failing behavioral checks.
- [x] Implement validated references in double; use a Cholesky covariance square root (the same ellipse as the sample's eigenbasis, with potentially more quad overdraw).
- [x] Build test_gaussian_raster; CTest -R 'test_gaussian_(loader|splatting|raster)' must pass. Commit this deliverable.

## Task 2: immutable frame upload, Slang pair and RHI pass

Files: extend raster.cppm, create raster.gpu.cpp, source/engine/pass/gaussian_splatting.cppm/.cpp, source/shaders/gaussian_raster.slang and gaussian_common.slang; extend test_gaussian_raster.cpp, shader-source registration and docs/gaussian_splatting.md.

Interfaces: make_raster_asset(core, gaussian::asset by value, limits) binds immutable CPU source/geometry and GPU asset; raster_instance references that shared asset/model/stable ID. prepare_raster_frame(core, instances, camera, limits) returns immutable shared raster_frame holding instance/reference buffers and all asset owners. gaussian_splatting_pass owns its pipeline and receives one shared frame snapshot.

- [x] Add RED tests for paired asset ownership, globally sorted/cull-filtered references, count/byte limits, second upload failure, invalid/unaligned addresses, no GPU allocations on invalid input, snapshot unload retention.
- [x] Implement coherent initial-byte uploads, structured errors and zero-count no-allocation behavior. Include instance+reference upload bytes in the frame budget; scratch is separate.
- [x] Add RED pass tests for alpha/depth pipeline recipe, six-vertex draw count, LOAD attachments, depth transitions, disabled cull/depth write, 112-byte push, and refused bind/push/barrier/render begin.
- [x] Implement the dedicated pass and Slang entry points using BDA word loads, explicit GLM column storage, direct VP Jacobian, shared SH constants and kernel parameters.
- [x] Register both shader entries in CMake and both standalone scripts; register shared include dependency and app shader loader.
- [x] Build shaders, validate SPIR-V offsets/strides and run shader-source plus raster tests. Commit only after green.

## Task 3: offscreen GPU verification and final gate

Files: add an isolated RHI probe under the Gaussian raster module and connect an explicit existing probe entry or standalone test driver after reading the backend bootstrap interface. No normal-frame wait_idle for retention.

- [x] Run small float HDR readback cases (plus regularized viewport edge coverage): red/blue alpha0.5 overlap center RGB (0.5,0,0.25), anisotropic coverage, SH sign, transformed second instance and opaque depth occlusion.
- [x] Use a nontrivial camera/model and nonzero local/instance indices to exercise matrix and 48/192/160/8-byte layouts; inspect against CPU reference.
- [x] Record actual GPU/device and numerical tolerance. If GPU capabilities prevent execution, report that limitation and preserve the probe; do not claim rendered validation.
- [x] Run full build/CTest, format, zero backend boundary and strict analysis; dispatch the required independent final reviewer, address important findings with RED/GREEN.
- [x] Push authorized commits and confirm remote LLVM23/Debug/ASan CI; record scope, limitations and next viewer-integration work.

Done 2026-10-11: two independent reviews of this batch ran (the second over a5ff51b0..e3bce691 plus the
working-tree changes; one blocker and two important findings, of which one blocker and one minor were real
and two important were misreadings). The blocker was the push block's declared lanes, fixed in cdab7ee6 with
a derived lane contract in test_shader_sources that fails without it; the plane-divergence reading was
settled by transcribing the shader's rejection into test_gaussian_raster and pinning both planes. The first
CI run then failed Release on five readability-trailing-comma findings that this machine's LLVM 22 clang-tidy
does not report (CI is LLVM 23) - fixed in 6f334e32, after which both jobs are green, as is the record commit.
Scope, limitations and the next viewer-integration work are in ../reviews/2026-10-11-gaussian-raster-pipeline.md.
