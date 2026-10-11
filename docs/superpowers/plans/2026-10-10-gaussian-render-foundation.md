# Gaussian rendering foundation implementation plan

> **For agentic workers:** Use superpowers:executing-plans inline on master after plan review. Each task includes tests, implementation, verification and a separate commit.

**Goal:** Convert the completed PLY loader's output into validated GPU records, deterministic global draw references and shared RHI GPU assets, ready for a dedicated Gaussian raster pass.

**Architecture:** Add `deren.engine.gaussian_splatting` under `source/engine/gaussian_splatting/`. Pure CPU packing and sorting are separated from GPU allocation. Asset upload is transactional; immutable GPU assets retain their buffers through RHI ownership. The raster pass, shaders and runtime integration are the following batch, not claimed complete by this foundation.

**Tech Stack:** C++23 modules, GLM, existing RHI buffer/device_address APIs, CMake/CTest and existing test harness.

**Spec:** [Gaussian splatting design](../specs/2026-10-10-gaussian-splatting-design.md), sections 2, 4, 5, 7 and 9. Loader implementation and CI are complete on master `ef42e608`.

## Global Constraints

- Work directly on master as requested; preserve the preexisting deletion of `deren_probe_backend_detach_probe.dll`.
- Latest user instruction reinstates stopping at 5% remaining quota and recording progress; subsequent resume retains this threshold.
- Engine code imports no Vulkan types, backend modules or native escape. No Gaussian-specific RHI objects or ABI changes.
- Geometry stride 48 bytes, SH stride 192 bytes, instance stride 160 bytes and draw-reference stride 8 bytes. Match the existing column-major GLM/Slang convention.
- Accept affine translation/rotation/positive uniform scale; reject non-uniform scale, shear, reflection and non-finite transforms.
- Global order is far to near; ties use stable instance ID followed by local particle index.
- Device/backend root lifetime follows the existing engine contract. A shared asset does not independently extend the loaded backend DLL's lifetime.
- No GPU sorting, indirect commands, TAA changes or rendering claims in this batch.

## Review Focus

- Loader assets are publicly constructible: invalid scales, quaternion, opacity, degree or SH must be rejected before allocation/upload.
- Matrix storage and quaternion wxyz order must not silently transpose covariance or instance transforms.
- Distinct instances of the same asset participate in one global sort; duplicate stable IDs are rejected rather than allowing nondeterministic ties.
- Arithmetic and allocation budgets cover geometry, SH and draw references; large counts must fail before allocation.
- A failed second buffer allocation or zero address releases earlier resources and publishes no partial GPU asset.

## Files and interfaces

Create:
- `source/engine/gaussian_splatting/gaussian_splatting.cppm`: exported input/result types, GPU records, CPU functions and shared GPU asset factory.
- `source/engine/gaussian_splatting/gaussian_splatting.cpp`: CPU validation, covariance packing and sorting.
- `source/engine/gaussian_splatting/gaussian_splatting.gpu.cpp`: RHI allocation/address lookup and owned GPU assets.
- `source/tests/test_gaussian_splatting.cpp`: headless math, layout, limits, ordering and failure-path tests using the existing `vk_test.h` harness.
- `docs/gaussian_splatting.md`: data convention, factory contract, memory accounting and current implementation boundary.

Modify `CMakeLists.txt` to register the module/implementation sources, link `gaussian_loader` to `deren_engine`, and register the headless test in `VR_TEST_TARGETS`. Add the test to the ASan build list in `.github/workflows/ci.yml`; register documentation in `Doxyfile` using existing conventions.

Namespace: `deren::engine::gaussian_splatting`.

- `geometry_record`: three `std::array<float, 4>` lanes: center/opacity, xx/xy/xz/0, yy/yz/zz/0.
- `sh_record`: `std::array<float, 48> coefficients` in coefficient-major RGB order.
- `instance_record`: uint64 geometry/sh addresses, model/inverse model matrices, uint32 degree and three padding words.
- `draw_reference`: uint32 instance index and local particle index.
- `foundation_error`: structured enum code and message; all fallible functions return `std::expected`.
- `foundation_limits`: default maximum 4,000,000 draw references and 1 GiB packed bytes; callers can tighten the budgets. Enforce uint32 index/count ceilings independently.
- `packed_asset`: geometry and SH vectors plus SH degree and local bounds.
- `pack_asset(deren::gaussian::asset const&, foundation_limits const& = {}) -> expected<packed_asset, foundation_error>`.
- `sort_instance`: borrowed CPU asset pointer, model transform, stable uint64 ID; borrowed only during preparation.
- `sort_camera`: finite world position and unit forward direction. No projection or culling in this foundational API.
- `sort_draw_references(span<sort_instance const>, sort_camera const&, foundation_limits const& = {}) -> expected<vector<draw_reference>, foundation_error>`.
- `gpu_asset`: immutable published metadata and owned geometry/SH buffers; exposes counts, bounds and addresses. Factory returns `shared_ptr<gpu_asset const>`.
- `upload_asset(rhi::api_core&, packed_asset const&, foundation_limits const& = {}) -> expected<shared_ptr<gpu_asset const>, foundation_error>`.

## Task 1: validated geometry/SH packing and layout

- [x] Add failing headless tests for record sizes/offsets and known covariance values: identity rotation, scales (1,2,3) -> diagonal (1,4,9); quarter-turn about Z swaps X/Y; a 45-degree Z rotation of scales (1,2,3) -> xx=yy=2.5, xy=-1.5, zz=9 (tolerance 1e-5).
- [x] Add tests for degree 0..3, coefficient-major RGB preservation, zero padding/unused bands, empty assets, and invalid publicly constructed inputs (NaN, zero scale, invalid quaternion, opacity outside [0,1], invalid degree).
- [x] Add exact-boundary/one-over tests for packed-byte budgets and a multiplication-overflow guard; observe the tests fail before implementation.
- [x] Implement records with static_assert size/offset guards and `pack_asset`. Accumulate covariance in double, reject non-finite float results, and copy SH without color-space conversion. Require normalized quaternion within a documented tolerance of 1e-4; do not silently repair manual assets.
- [x] Recompute conservative local 3-sigma bounds from validated particles; do not trust externally supplied asset bounds. Preserve the loader's outward-rounding rule.
- [x] Build/run `test_gaussian_splatting`, verify existing loader tests, format touched files and commit this deliverable.

## Task 2: instance validation and deterministic global references

- [x] Add failing tests for translation, rotation and positive uniform scale; reject non-affine bottom rows, non-uniform scale, shear, reflection, singular/overflowing inverse and non-finite matrix values. Use relative tolerance 1e-5 for equal scales/orthogonality, with finite inverse required.
- [x] Add ordering tests: centers at z=-1/-3 with camera forward (0,0,-1) place -3 first; reversed forward reverses the order. Two instances of a shared asset interleave by world depth. Equal depths sort by stable ID then local index regardless of input order.
- [x] Test zero particles, duplicate stable IDs, null asset input, non-unit/zero camera forward, non-finite camera/depth, and exact/one-over total reference budgets; observe RED.
- [x] Implement `sort_draw_references` with validated inputs and double-precision world center/depth arithmetic. Store only references in output, leave static properties unchanged. Validate IDs for all instances, including empty ones.
- [x] No culling at this stage: emit all valid references. Conservative support-domain frustum checks and near-plane omission belong to the raster batch, so this function cannot be mistaken for a visible-list builder.
- [x] Run focused tests and commit the CPU baseline independently.

## Task 3: transactional static GPU upload

- [x] Add a minimal fake RHI core/device_address/buffer implementation in the test file, following existing headless dependency-injection tests. Record buffer descriptors and release counts.
- [x] Add failing tests for exact initial bytes, storage/device_address flags, GPU-only usage, byte accounting, unsupported address capability, first/second allocation failure, zero first/second address, mismatched packed counts and invalid packed metadata.
- [x] Empty assets succeed with zero addresses and no allocation. Test successful shared ownership releases each buffer exactly once after the last owner drops. No GPU submission is exercised or implied here.
- [x] Implement `upload_asset` with `rhi::query_extension<rhi::device_address>`, `api_core::create_buffer`, `buffer_desc::initial_bytes`, `buffer_usage::storage_gpu_only`, and storage/device_address flags. Obtain addresses with `buffer_address(buffer, 0)`.
- [x] Own buffers with existing RHI object ownership conventions and publish the shared asset only after both buffers and addresses succeed. Validate packed input again because it is public; do not retain upload spans beyond creation.
- [x] Document API, budgets, ownership and completion-slot responsibilities. Future frame snapshots must retain these assets until GPU work completes; this factory alone does not supply that submission lifetime.
- [x] Run full Release build, all CTest tests, clang-format-check, check-backend-boundary and applicable clang-tidy sequentially with builds. Review the final diff and commit.
- [ ] Push authorized commits and confirm Release/Debug, strict LLVM 23 analysis and ASan/UBSan CI pass before reporting completion.

## Following batch

The foundation is complete when the CPU records/order and RHI upload are tested and usable. Next: CPU projection/SH references, Slang vertex/fragment shader and layout probe, Gaussian pass, frame-slot snapshots, runtime/viewer integration and actual HDR readback tests. Preserve the 112-byte push block and 0.3 pixel-squared regularization from the spec. That batch must validate near-plane handling and real viewing before claiming Gaussian rendering works.
