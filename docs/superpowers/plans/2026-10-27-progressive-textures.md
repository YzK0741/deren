# Progressive textures implementation plan

> **For agentic workers:** Use superpowers:executing-plans. Track each task below.

**Goal:** Current-camera rendering with low-resolution fallback and bounded asynchronous upgrades.

**Architecture:** Immutable low/high images, completed-slot feedback, per-frame descriptor banks and
bounded upload futures. Preserve the default synchronous GPU residency baseline.

**Tech Stack:** C++ modules, Vulkan descriptor heaps, Slang, existing capture harness.

**Spec:** ../specs/2026-10-27-progressive-textures-design.md

## Global constraints
- Default off; no RHI ABI changes, CPU HZB, virtual texture pages or frozen camera.
- Keep pinned textures full resolution and maintain descriptor/resource lifetime across both slots.
- Work on codex/progressive-texture-residency; no commit or push without user request.

## Review focus
- NPOT, sRGB and malformed inputs: test exact dimensions, filtering, alpha and refusals.
- Budget smaller than a request: retain fallback without endless upload/evict cycles.
- In-flight resources: own each descriptor bank's images until its completed slot is reused.
- In-flight jobs on shutdown: join before GPU idle and backend/resource destruction.
- Disabled streaming or non-visibility fallback: retain valid textures and original baseline behavior.

## Task 1: Fallback and budget policy
- [x] Add standalone CPU tests in source/tests/test_progressive_texture.cpp and observe failure.
- [x] Implement source/engine/visibility/progressive_texture.h: RGBA8 fallback and bounded selection.
- [x] Run tests for NPOT, linear/sRGB, budget pressure, pinning and request limits.

## Task 2: Integrate asynchronous residency
- [x] Modify runtime declarations, constructor, texture_residency and post_graph.
- [x] Add completed-slot feedback buffers, low images, bounded futures, tracing and shutdown join.
- [x] Add per-frame texture banks in render_layout and shader surface/resolve/RT addressing.
- [x] Build and run residency, render-resource, shader and render-graph regressions.

## Task 3: Device and visual validation
- [x] Capture original/progressive fixed and moving views with Vulkan validation.
- [x] Verify low-only startup, completed upgrades, no per-frame checkpoint and bounded requests.
- [x] Record image differences, timings and known limitations in docs/benchmarks.
- [x] Review changes, fix material findings and run final checks.

## Execution ledger
- 2026-10-27: User explicitly authorized the ranked implementation. Proceed inline without another
  approval round; preserve unrelated untracked files. The approved first increment is two-tier
  streaming; later VT and target-mip feedback remain outside this increment.

- Completed: standalone policy tests 16/16; toggle RED then GREEN; full build and 72/72 CTest.
- Review fixes: continue restoration, cancel restoration on re-enable, migrate active unlit shader to frame bank.
- Validation and measurements: 11 final cases, all clean; two moving mode-order rounds. See docs/benchmarks/2026-10-27-progressive-textures.md.
- No commit, merge or push. Default GPU baseline retained; progressive remains opt-in.
