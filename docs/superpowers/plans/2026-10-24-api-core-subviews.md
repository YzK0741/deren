# api_core subviews implementation plan

> **For agentic workers:** Use superpowers:executing-plans inline.

**Goal:** Split root responsibilities into public non-rebindable borrowed members, using ordinary member names.

**Architecture:** `resource_factory`, `command_context`, `frame_context` are virtual interfaces. `api_core` exposes `resources`, `commands`, `frames` through a pointer-sized `context_view<T>` wrapper with arrow/get/bool access, deleted copying/assignment and private binding. Derived constructors call protected `bind_views` after their bases/members are constructed. The Vulkan core implements the interfaces on the same allocation; wrappers store adjusted interface pointers. No const_cast, mutable storage, extra ownership or per-view allocation is required.

**Spec:** Responsibility split and public-member wrapper approved in this conversation, including the user's requirement that member names have no adornments.

**Tech Stack:** C++ modules, virtual interfaces, existing backend/test implementations, CMake/Ninja.

## Boundaries

- Resources: memory/image/sampler/shader/pipeline/query/AS/micromap factories and texture groups.
- Commands: independent command allocation and untracked primary/batch submit. Existing command_submission token extension remains optional and unchanged.
- Frames: swapchain creation, current frame commands/images/readback, frame acquisition/walker/profiler/surface, no-argument frame submit, present.
- Root: release, ABI, capabilities/extensions, native context and device-wide idle.
- ABI 50; remove old root virtual entry points and migrate all production/test callers.
- Bind once during construction before publication. Views are borrowed and cannot outlive core. Interfaces are not owning RHI object handles.
- Ordinary member names; preserve existing workspace changes; no commit requested; 14 build jobs.

## Review focus

- Public slots cannot be assigned, reset or rebound by a caller.
- Derived-member contexts can be constructed before binding; constructors do not use unconstructed interface bases.
- Frame submit has no buffer argument; independent submit rejects borrowed frame commands.
- All native/probe backend interface-pointer adjustments preserve the same core state/lifetime.
- Cluster ownership, token identity/completion, GUI heap binding and startup behavior remain intact.

### Task 1: Contract

- [x] Add failing compile-time tests for member shape, assignment prohibition and submission boundaries.
- [x] Extract the three interfaces, add context_view/bind_views, bump ABI 50.

### Task 2: Implementations and migration

- [x] Adapt Vulkan and test roots, bind once in derived constructor bodies, split frame submit from independent submit.
- [x] Migrate engine, GUI and tests; preserve existing resource and frame protocols.
- [x] Build and resolve all consumers without compatibility root forwarding slots.

### Task 3: Verification and docs

- [x] Run full CTest, completion GPU probe, windowed Gaussian/PBR checks and relevant startup check.
- [x] Independent review and required corrections.
- [x] Update architecture/usage docs and validation record, retaining known baseline failures.

## Execution record

Implemented and verified in the current checkout, preserving pre-existing work. Vulkan uses same-core
interface bases; tests additionally exercise member-owned contexts. Frame submit is explicitly separated
from independent submission. ABI 50 requires matching rebuilt binaries. See
`docs/reviews/2026-10-24-api-core-subviews.md` for evidence and known baseline limits.
