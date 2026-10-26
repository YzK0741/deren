# Lazy Unload Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans inline, followed by final independent review.

**Goal:** Add safe explicit unload/reload to shared-source lazy resources.
**Architecture:** Track physical dependency leases in the existing loading gate; refuse unloading while
bindings/recordings cache native data. Preserve source and metadata and return to unloaded on success.
**Tech Stack:** C++23 modules, existing Vulkan/VMA owners, shared command/group retention.
**Spec:** docs/superpowers/specs/2026-10-26-lazy-unload-design.md.

## Constraints and review focus
Keep all earlier uncommitted work. Implement user's requested first unload method in place; no commit,
push, new worktree, automatic eviction or descriptor-generation manager. Build -j14.
Review lease acquisition/release ordering, lock inversion, untracked descriptor writes and GPU lifetime.

## Tasks
- [x] Tests RED: unload API, source lifetime/reload, rejection with live command/view/group/slot;
      common loading gate unloaded/loading/failed/busy behavior.
- [x] Implement common gate and RHI unload methods; ABI57 and factory/loader fixture compatibility.
- [x] Track dependency leases in commands, views, groups and fixed descriptors; internal native writer.
- [x] Release/reload device tests and validation; CPU regression and eight-device lane.
- [x] Update standard and compatibility docs; independent review and fix important findings.

## Ledger
First unload intentionally requires dropping dependent logical binding containers; their native caches
are not invalidated transparently. User requested a first explicit method, not automatic residency.

Final independent review found radix-sort native operands bypassed demand/retention. A regression
reproduced two failed unload assertions, then passed after acquiring leases before native handle reads.
Added explicit pending-only submission, completed reusable recording, and raw heap refusal checks.

Acceptance: Release build -j14 passed; CTest71/71; device probes8/8; dedicated validation-enabled lazy
probe369/0 with unchanged shader readback and no VUID/Validation Error/SYNC-HAZARD diagnostics.
See docs/reviews/2026-10-26-lazy-unload.md. No commit/push.
