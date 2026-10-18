# Render Graph Composition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Implement ownable/constrained executable composition and migrate the existing post chain to pass-ref bound recording.

**Architecture:** Keep the CPU builder independent; add execution::builder in compose.cppm, reuse prepared_graph's recording loop, expose pure post::append and retain standalone compile wrappers.

**Tech Stack:** C++23 modules, expected/concepts, utility::vessel/mimalloc, existing RHI and Boost.Graph.

**Spec:** ../specs/2026-10-16-render-graph-composition-design.md

## Global Constraints

- Resources are created outside the graph; default legacy mode remains（2026-10-18 核对：该句的“default legacy mode”已不成立——图是唯一录制路径，legacy tail 与 `main_render_graph` 等启动开关已删除；资源图外创建这一条仍然成立。证据：source/application_configuration/application_configuration.cppm:255、docs/reviews/2026-10-17-render-graph-gate.md:997-1014）。
- Names: ownable, graph_appender, pass_recorder; exact noexcept return contracts.
- Preserve current workspace modifications; no commits/pushes in this task.
- Builds use -j12, serial build invocations because formatting is shared.

## Review Focus

- Foreign pass refs with equal indices must fail before recording.
- Every live action must be checked before the first barrier.
- Recorder/payload ownership must survive caller and builder destruction.
- Half-built append failures must latch and prevent preparation.
- Stable payload addresses must protect resolved_io spans.

## Tasks and ledger

### 1. Composition API and executor

- [x] Add test_render_graph_compose: concepts, import alias/conflict, foreign/duplicate/missing actions, type mismatch, culling, errors, retention and old/new recording modes; build to observe missing API.
- [x] Add CPU builder::contains(pass_ref), compose.cppm concepts/error/import/action builder, and shared executor bound-action support.
- [x] Build/run compose and existing execute/frame tests; inspect failures, then pass.

### 2. Pure append and production post binding

- [x] Extend post CPU tests to append after an upstream producer and root only the final output; verify missing append API fails.
- [x] Add post::inputs/options/outputs and append; keep compile as import/append/export wrapper.
- [x] Replace positional runtime recording with owned stable payloads and explicit pass_ref bindings.
- [x] Run post/bloom/compose tests and full Release build/CTest.

### 3. Acceptance and review

- [x] Run ASan graph/config subset, expanded boundary gate, real GPU image comparisons and windowed resize sync validation.
- [x] Obtain fresh independent review, resolve actionable findings, update docs/spec status and evidence.

Ruling: Continue in the existing checkout because the graph/cluster/post prerequisites are uncommitted here and the user authorized continuing this workspace; a fresh worktree would omit them. Preserve all unrelated changes.

Ruling: The user delegated execution after reviewing the written design; implement inline, with one fresh review at completion. No additional implementation-permission round.

Ruling: C++ named-module ownership prevents friending a forward-declared composition builder from execute. Use an internal detail::composition_access bridge; public prepared_graph keeps its existing preparation API and gains only record(commands).

Review: no correctness findings; internal bridge encapsulated, direct buffer/stale-snapshot/capability/shape probes added. An additional null callable probe failed as expected; bind_pass now rejects null function/member pointers.

Final: Release 43/43, ASan 15/15, expanded boundary zero, five GPU capture pairs byte-identical and clean, three window resizes clean. Null appender RED reproduced as SegFault before adding the value guard; final test passes. No commit/push.
