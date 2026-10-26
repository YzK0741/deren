# Logical Descriptor Writes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove physical descriptor arithmetic from runtime through logical RHI writes.
**Architecture:** Extend descriptor_space with fixed-profile queries and object-based writes/bind. Vulkan resolves and validates the existing shader-slot grid internally.
**Tech Stack:** C++ modules, Vulkan descriptor heaps, CMake/CTest, PowerShell.
**Spec:** ../specs/2026-10-25-logical-descriptor-writes-design.md (approved).

## Global Constraints

ABI 55; preserve shader indices, resource_group/texture_group and caller-managed GPU lifetimes.
Use current dirty workspace; no commit/push/reset. Inline execution is already authorized by the user's implementation request.

## Review Focus

Foreign objects must be checked before casts; arithmetic must reject overflow; dynamic group slots must
remain inaccessible; rejected writes must not mutate descriptors; unavailable fixed profile must not appear ready.

### Task 1: Contract and backend

Files: rhi.extension.cppm, rhi.contract.cppm, core.declarations.cppm, core.descriptor_space.cpp,
core.api_core.cpp, source/tests/test_texture_group.cpp.
Interfaces: descriptor_slot_range fixed_slots() const; write_image(descriptor_image_write const&),
write_buffer(descriptor_buffer_write const&), write_acceleration_structure(descriptor_acceleration_structure_write const&),
bind(descriptor_bind_info const&). All return error except the query; base defaults refuse unsupported.

- [x] Add failing device checks for fixed interval, null/foreign resources and illegal slots/ranges.
- [x] Append tagged request types, add default-refusing virtuals, bump ABI and update version fixtures.
- [x] Implement validated Vulkan writes, AS provenance registry and delegated bind.
- [x] Build/run texture-group probe; existing group behavior and new writes pass with no validation errors.

### Task 2: Consumers and boundary

Files: runtime.declarations.cppm, runtime.constructor.cppm, runtime.frames.cppm, runtime.cpp,
pass/software_visibility.cpp, source/tests/test_docs.cpp.
Consumes Task 1's logical slot/object API. Existing runtime helper signatures switch from physical offset/address
to shader slot/resource. AS gets its dedicated helper. No descriptor_heap reference remains in migrated consumers.

- [x] Add a failing source boundary check forbidding heap_slot_offset/heap_slot_stride/physical heap writes in consumers.
- [x] Migrate runtime helpers, all writers/bind/readiness and diagnostics; migrate software pass writes.
- [x] Run CPU docs/boundary probes and device Gaussian/PBR/frame consumers.

### Task 3: Verification and standard

Files: docs/rhi_api_standard.md, docs/dynamic_runtime.md, docs/mainpage.md, scripts/gates/check_device_probes.ps1,
docs/reviews/2026-10-25-logical-descriptor-writes.md.

- [x] Full Release build -j14; CTest; device lane; changed-source formatting and diff checks.
- [x] Update current standard/ABI documentation and record evidence/remaining limits.
- [x] Fresh independent implementation review; address important findings; preserve all uncommitted work.

## Execution ledger

Ruling: retain existing workspace and do not introduce another approval gate; user explicitly approved implementing this design here.
Pre-flight: Task 2 consumes Task 1 signatures exactly; Task 3 documents both. Default unsupported virtuals preserve source compatibility for CPU probes, ABI bump enforces binary incompatibility.

Task 1/2: complete. Review fix: include runtime.probes.cppm in boundary and logical bind migration (observed RED then GREEN).
Ruling: migrate the CPU test backend's tracking layer too; the fixed profile remains optional but this renderer fixture must serve it.
Ruling: correct stale runtime device-test frame submission assertion to the existing frame_context path; do not weaken production validation.
Task 3: complete. Full build passes, CTest 69/69, device lane 7/7, actual runtime device probe 1174/0,
docs/boundary 475/0, zero archived validation errors. Review and follow-up: no remaining important issues.
Detailed evidence: ../../reviews/2026-10-25-logical-descriptor-writes.md. No commit/push requested or performed.
