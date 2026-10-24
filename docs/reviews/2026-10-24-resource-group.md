# Resource group implementation - 2026-10-24

## Delivered

RHI ABI 51 adds immutable shared resource groups, a pipeline logical binding layout and
`command_recorder::load_resource_group`. A texture_group is one member kind, alongside sampled/storage
images, uniform/storage buffer ranges, samplers and top-level acceleration structures. No public
physical descriptor allocation/container is introduced. Existing texture-group callers remain usable;
render-pass migration and lazy recording are subsequent work.

Descriptors/layouts are copied; binding-number/kind matching is order independent. Groups reject
borrowed/non-owning members, null/foreign/wrong-kind objects, duplicate bindings and invalid ranges.
A shared_object lifetime alias still requires its owner to actually retain the target. Recorded and
pending primary/secondary/batch/frame work retain whole groups, so descriptors and member owners
survive caller release. Reusable private slots return on final group destruction, including failed
creation rollback. Loading adds no barriers and does not freeze nested texture-group contents.

Vulkan publishes uint32 shader handles through declared parameter fields; ordinary heap pushes cannot
overwrite them. Current Slang ConstantBuffer access lowers to StorageBuffer pointers, so logical
uniform bindings use native storage descriptors and uniform allocations gain native storage usage.
Both logical uniform and native storage range/alignment constraints are enforced.

The old texture-group fixed range overlapped scene reservations and could not be reserved at its
requested base. Table/pool moved to slots 32768/32769 onward, reserved during core construction before
dynamic allocations. Resource working heap budget is now 4 MiB instead of 1280 KiB. Shader constants,
CMake rules and both standalone shader compiler lists were updated together.

## Verification

- Contract red test failed on missing group descriptor/factory/load/layout symbols before implementation.
- Full Release build with 14 jobs passed; subsequent focused test rebuild passed.
- Full CTest: 69/69 passed (`tmp/resource-group-ctest-final2.log`). Later edits affect only the device
  branch of the new test; its default/headless layout branch remains unchanged.
- Resource-group Vulkan device test: 107 checks, zero failures, zero VUID/validation errors. Covers
  mixed GPU buffer/uniform/image/sampler/nested texture-group readback; storage-image content readback;
  7000 rounds of successful creation and partial-failure recycling; invalid layout/foreign/type/range/
  borrowed inputs; primary pending retention behind a queue gate; secondary batch/primary composition;
  acquired-frame retention and rerecord release; reserved parameter protection; TLAS member creation.
- Fragment-only resources with a plain vertex shader and vertex-only depth pipelines both create
  successfully. The depth-only fixture exposed the existing builder's unconditional empty fragment
  module creation; native stage count/module creation now follows the supplied shader stages.
- Existing texture-group device suite: 95 checks, zero failures, zero validation errors. Its old copy
  fixtures lacked transfer usage flags; those declarations were corrected without weakening checks.
- Submission completion device suite: 63 checks, zero failures, zero validation errors.
- Frame device suite: 12 checks, zero failures, zero validation errors.
- Gaussian/PBR windowed captures: exit 0, fresh captures, zero validation errors. Gaussian GPU sort
  completion consumer: PASS 48 cases. Normal startup rendered repeatedly and closed gracefully.
- Independent review found two issues (non-consuming graphics stage capability requirement and native
  uniform/storage alignment). Both were fixed and the follow-up review reported no actionable findings.
- git diff --check passed; no commit or publication performed.

Initial test failures were corrected, not counted as passes: missing standalone shader-list/manual
registration; manual ASCII requirement; uniform descriptor representation mismatch; missing explicit
storage-image transition in the test fixture. Final results above use corrected sources and logs.

## Limits

Only one loaded flat general group per pipeline is supported; no recursive resource_group, ray-tracing
pipeline group layout, arbitrary descriptor arrays, lazy recording or unified push_constants yet.
TLAS descriptor creation/retention is exercised; this test does not build/traverse a scene through a
general-group TLAS. Graphics fixtures verify pipeline creation; the mixed GPU execution/readback
fixture is compute. Stage-kind associations in the layout remain a shader interface agreement;
reflection validates parameter shape/range, not arbitrary shader intent. Missing required bindings
before draw/dispatch remain a caller precondition until fuller recording validation is implemented.

Core/backend lifetime and external AS scene dependencies remain explicit caller obligations. No
performance improvement is claimed by these correctness checks. Prior unrelated device-suite failures
are not blanket-declared fixed, although the texture-group reservation prerequisite is now corrected.

Local evidence: `tmp/resource-group-full-build-final2.log`, `tmp/resource-group-build-final.log`,
`tmp/resource-group-test_resource_group.log`, per-suite validation logs, and
`tmp/resource-group-smoke-final.log` / `tmp/resource-group-smoke/results.json`.
