# Passchain Managed Resources Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task inline. Steps use checkbox syntax.

**Goal:** Make passchain collect dynamic resource descriptions, create/reuse owned generations by lifetime, distribute shared snapshots and initialize completed frame-slot resources; migrate raster shadow resources as the first real consumer.

**Architecture:** A nested RHI-only resource module owns images/buffers and complete view bundles. Passes have optional description/binding hooks; the chain performs a transactional collection and allocation step, with lifetime groups and immutable snapshots. Runtime keeps submitted snapshots and bridges existing shadow bindings, keeping current waits and barriers during this first migration.

**Tech Stack:** C++23 modules, existing RHI object_manager, std::map/vector/shared_ptr/expected, CPU probe tests, Vulkan validation.

**Spec:** ../specs/2026-10-13-unified-resource-access-design.md, refined by the user's accepted passchain lifetime-group design.

## Global Constraints

- No RHI ABI, shader slot or image-quality change.
- Preserve existing waits for replacement and borrowed swapchain resources; automatic barriers are later work.
- Device roots outlive owned allocations; views release before images.
- Preserve dynamic growth/reuse and explicit failure; history is not blindly reset each frame.
- Keep existing uncommitted work; do not create a checkout that loses it or commit/push unrelated changes.

## Review Focus

Duplicate producer descriptions, failed multi-resource creation, stale/in-flight snapshots, frame/output index distinction, and destruction after caller drops the device root need tests.

## Tasks

- [x] 1. Add failing portable probe tests for registry allocation, reuse/growth, conflict rollback, lifetime grouping, per-slot reset and snapshot retention.
- [x] 2. Implement source/engine/render_resource/instances.cppm: image/buffer requests, lifetime grouping, shared generation ownership, transactional update and explicit completed-frame initialization.
- [x] 3. Add frame_pass dynamic describe/bind hooks and pass_chain collect/create/bind/begin_frame operations; test shared producer/consumer distribution.
- [x] 4. Let shadow_pass describe layered per-frame images; replace runtime shadow ownership with borrowed views of a chain-managed snapshot. Keep existing rebuild drain and heap binding order; retain frame snapshots until their slot wait succeeds.
- [x] 5. Build, run CTest and native/capture regressions; review independently and document scope/results.

Initial implementation supports owned images and buffers. AS/imported resources retain explicit existing management until their specialized descriptors/completion paths are migrated. Existing passes with empty hooks retain their behavior.

## Progress checkpoint — 2026-10-13

Paused at the user's explicit request. Tasks 1–4 complete. Task 5 has successful Release build,
24/24 CTest, 92 managed-resource/frame checks, 1164 native checks, clean PBR/ Gaussian captures,
and an independent review whose four findings were fixed and re-reviewed. Remaining: compare final
candidate captures against the frozen baseline and finalize evidence/report bookkeeping.
See ../../reviews/2026-10-13-managed-resources.md for exact results and resume instructions.

Ruling: reconcile before acquisition, reset only after successful completed-slot wait — a creation
failure must not strand an acquired image/semaphore. Preserve separate submitted snapshots for the
application and fallback shadow chains; resolve shadow ownership afresh on chain handover.

## Completion — 2026-10-13

Resumed at the user's request. Direct pixel comparisons of baseline vs candidate-final passed for
all four scenes (identical RGBA pixels, 1280x720), with clean synchronization-validation logs.
Captured images/configs/logs and the final manifest are archived under
`docs/reviews/2026-10-13-managed-resources/`. Binary hashes were recorded at archival time.
All five tasks are complete. No render source changes were made during this final bookkeeping step.
