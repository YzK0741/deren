# RHI Resource and Synchronization Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans inline, followed by one independent review.

**Goal:** Expand ABI 53 resource/synchronization guarantees with implementation-backed verification.
**Architecture:** Existing RHI interfaces remain unchanged; mapped Vulkan allocation policy is tightened.
**Tech Stack:** C++23 modules, Vulkan/VMA, generated CPU fixtures, CTest, Markdown/Doxygen.
**Spec:** docs/superpowers/specs/2026-10-24-rhi-resource-sync-design.md

## Constraints and review focus

Preserve current uncommitted work and ABI 53. No automatic retention of borrowed operands. No claim that
coherence supplies GPU execution completion or barriers. Internal upload staging is exempt because it flushes.
Cached memory is preferred, never mandatory. Keep public declarations and standard consistent.

## Task 1: mapped-memory guarantee

- [x] Extend generate_vulkan_query_fixture.py to expose actual allocation policy bodies to test_vulkan_capabilities.
- [x] Add headless checks for required mapped/coherent memory on every mapped intent; observe staging failure.
- [x] Explicitly require coherence on mapped allocation policies and prefer cached on uniform_cached; rerun.
- [x] Extend test_submission_completion with post-creation host mutation, staging copy and token-completed readback.

## Task 2: standard and evidence

- [x] Add resource/update and completion sections plus capability/binding and evidence cross-references.
- [x] Align mapped and token source comments with those rules, without new virtuals or fields.
- [x] Build with 14 workers; run relevant headless and device tests, inspect validation logs.
- [x] Independent review and resolve findings; record verification and limits.

## Completion ledger

2026-10-24: Implemented inline in the existing workspace. All tasks completed; no commit or publication.
Allocation-policy red: 55 checks / 7 failures; green: 55 / 0. The red checks include the newly explicit
CPU_ONLY requirements and cached preference as well as the staging_source requirement; CPU_ONLY already
implicitly required coherence in VMA, so those two roles were not previously non-coherent.
Device submission: 75 / 0 with zero validation errors. Full build passed with 14 workers; CTest 69/69;
optional device lane 5/5. Independent read-only review found no actionable issues.
