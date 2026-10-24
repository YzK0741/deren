# Deferred Render Operations Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans in the current checkout. Execute through verification without further handoff; the user explicitly requested implementation.

**Goal:** Buffer and simplify recording operations while preserving observable GPU work.

**Architecture:** A shared CPU operation stream owns polymorphic operation values in pooled command storage. Vulkan validates on enqueue and replays optimized batches at explicit application and immediate boundaries; pending resource groups remain retained.

**Tech Stack:** C++ modules, vessel/mimalloc, Vulkan, existing CPU/device probes.

**Spec:** `docs/superpowers/specs/2026-10-24-render-operations-design.md`

**Status (2026-10-24, accepted with follow-ups):** the shared operation stream, ABI 53
`command_recorder::apply_operations()` and the Vulkan deferral/flush integration are implemented and accepted
with follow-ups. Independent review: `docs/reviews/2026-10-24-render-operations-review.md` (0 Critical / 5
Important / 11 Minor); acceptance: `docs/reviews/2026-10-24-render-operations.md`. Open follow-ups (do not
claim them as done): a pre-deferral recording-cost baseline (I1), CPU recording/allocation measurements (I2),
native-interop/execute/post-ABI53 GUI restore coverage (I3), and the remaining device coverage gaps (I5).


## Global constraints

- No exceptions or RTTI; ordinary unadorned member names; preserve the dirty checkout.
- Both coarse equality and fine unused-state elimination; never remove effectful operations.
- Preserve GUI explicit heap binding, secondary recording, and submission lifetimes.
- Copy push payloads. Errors must not publish executable failed recordings.
- Build with 14 jobs. Do not commit or push without a request.

## Review focus

- A/B/A and repeated draws must preserve consumer-visible state and effect count.
- Partial push overlaps and pipeline token defaults must preserve valid bytes.
- Mid-stream application/native interop must preserve subsequent state.
- Reused object addresses must not revive stale pending bindings.
- Secondary execution and implicit flush failures must not submit partial recordings.

## Task 1: Capture baseline and implement the shared operation stream

Files: new `source/promise/rhi/render_operations.cppm`, `source/tests/test_render_operations.cpp`; modify `CMakeLists.txt`, Vulkan recorder instrumentation.

- [ ] Capture a pre-deferral baseline: not reconstructed (the feature builds on already-authorized uncommitted RHI changes). The recorded control is optimized-vs-`DEREN_RENDER_OPERATIONS_UNOPTIMIZED` on the same deferred stream; request counts live in the untracked `tmp/render-operations-baseline` and have no native setter counts/build identity.
- [x] Add tests for adjacent duplicates, overwritten state, A/B/A with consumers, protected duplicate execution, push copying/full/partial overlap, terminal/intermediate windows and independent state keys. Watch missing stream implementation fail. Expanded with independent cull/depth-write state kinds (275 checks PASS).
- [x] Implement `operation_kind`, `render_operation`, typed derived values, pooled storage, and `operation_stream::simplify(bool final)`. Per-kind accepted/retained counters are not implemented; backend stats (`DEREN_RENDER_OPERATION_STATS`) provide request/retained/setter/work totals.
- [x] Build/run `test_render_operations`; compare interpreter outputs for deterministic generated streams with simplification on/off (275 checks PASS).

## Task 2: Integrate validated Vulkan deferral

Files: `core.declarations.cppm`, new `core.render_operations.cpp`, `core.api_core.cpp`, `core.resource_groups.cpp`, `core.texture_groups.cpp`, `core.cpp`, `radix_sort.cpp`, scoped synchronization and shared Vulkan command buffer bridge.

- [x] Add ABI 53 `command_recorder::apply_operations() -> error`, native interop preparation/finish, and mergeable forwarding; update ABI assertions/documentation. ABI 53 docs corrected for queued group/push semantics.
- [ ] Device-probe coverage is incomplete: `test_resource_group` covers copied pending push, explicit application, stale buffer/pipeline identity, scope failure/reset and group retention; `test_dynamic_link` covers the apply lifecycle. Missing: mid-window state beyond the apply-then-bind case and secondary/mergeable ordering under a failing implicit flush (I5).
- [x] Enqueue validated state/execution; keep unsupported families as ordered application boundaries. Add generation identities for borrowed buffer/pipeline bindings.
- [x] Replay only after complete batch validation, latch deferred errors, flush before immediate/native work and end recording, reset storage on legal begin. Preserve logical group/push state across compatible pipeline changes and republish at consumers (code-inspected; native restore path not directly device-probed, I3).
- [ ] CPU probe backend is an ABI-surface double, not equivalent save/restore/identity semantics (M1). `DEREN_RENDER_OPERATIONS_UNOPTIMIZED` remains a developer-only switch (M2).
- [x] Run operation/contract tests and resource-group, texture-group, completion and frame device probes (all PASS with `--with-device`).

## Task 3: Verify rendering and record comparison

Files: new `docs/reviews/2026-10-24-render-operations.md`; update relevant contract/manual docs and this progress ledger.

- [x] Build all and run the complete CTest suite: Release 69/69 PASS.
- [x] Repeat PBR/Gaussian configs; compare requests, retained operations, native lowering calls and captures: deferred setters 33 vs 41, gaussian 40 vs 52, work counts equal, screenshots byte-identical; warmup excluded; GUI enabled captures excluded (render gate forces GUI off) (I1 narrows the claim).
- [x] Independently review the resulting change: docs/reviews/2026-10-24-render-operations-review.md (0 Critical / 5 Important / 11 Minor, accepted with follow-ups); I4 fixed, I1/I2/I3/I5 recorded as follow-ups, gates rerun.
- [x] Report measured counts and limitations: docs/reviews/2026-10-24-render-operations.md + results.json (build identity included). Changes committed as the Lead checkpoint, not left uncommitted.

## Execution ledger

- User addition: capture current per-frame operation counts before optimization; compare identical workloads afterward.
- Ruling: work in the current checkout because the feature builds on already-authorized uncommitted RHI changes; a new worktree would omit those dependencies.
