# Deferred Render Operations Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans in the current checkout. Execute through verification without further handoff; the user explicitly requested implementation.

**Goal:** Buffer and simplify recording operations while preserving observable GPU work.

**Architecture:** A shared CPU operation stream owns polymorphic operation values in pooled command storage. Vulkan validates on enqueue and replays optimized batches at explicit application and immediate boundaries; pending resource groups remain retained.

**Tech Stack:** C++ modules, vessel/mimalloc, Vulkan, existing CPU/device probes.

**Spec:** `docs/superpowers/specs/2026-10-24-render-operations-design.md`

**Status (2026-10-24):** the implementation is present in the working tree - the shared operation stream,
ABI 53 `command_recorder::apply_operations()` and the Vulkan deferral/flush integration, with
`test_render_operations` registered in CTest. The Task 1-3 checkboxes remain unticked because the final
device comparison, independent review and `docs/reviews/2026-10-24-render-operations.md` are not completed
yet; this plan is a partial checkpoint, not an accepted implementation.


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

- [ ] Capture current PBR/Gaussian per-frame RHI entry counts with an opt-in statistics log before enabling deferral. Save matching configs and raw logs under `tmp/render-operations-baseline`.
- [ ] Add tests for adjacent duplicates, overwritten state, A/B/A with consumers, protected duplicate execution, push copying/full/partial overlap, terminal/intermediate windows and independent state keys. Watch missing stream implementation fail.
- [ ] Implement `operation_kind`, `render_operation`, typed derived values, pooled storage, and `operation_stream::simplify(bool final)` plus per-kind accepted/retained counters.
- [ ] Build/run `test_render_operations`; compare interpreter outputs for deterministic generated streams with simplification on/off.

## Task 2: Integrate validated Vulkan deferral

Files: `core.declarations.cppm`, new `core.render_operations.cpp`, `core.api_core.cpp`, `core.resource_groups.cpp`, `core.texture_groups.cpp`, `core.cpp`, `radix_sort.cpp`, scoped synchronization and shared Vulkan command buffer bridge.

- [ ] Add ABI 53 `command_recorder::apply_operations() -> error`, native interop preparation/finish, and mergeable forwarding; update ABI assertions/documentation.
- [ ] Add failing device probes for pending copied push, explicit application, mid-window state, stale buffer/pipeline identity, scope failure/reset and secondary ordering.
- [ ] Enqueue validated state/execution; keep unsupported families as ordered application boundaries. Add generation identities for borrowed buffer/pipeline bindings.
- [ ] Replay only after complete batch validation, latch deferred errors, flush before immediate/native work and end recording, reset storage on legal begin. Preserve logical group/push state across compatible pipeline changes and republish at consumers.
- [ ] Update CPU probe recording semantics without test-only production switches.
- [ ] Run operation/contract tests and resource-group, texture-group, completion and frame device probes.

## Task 3: Verify rendering and record comparison

Files: new `docs/reviews/2026-10-24-render-operations.md`; update relevant contract/manual docs and this progress ledger.

- [ ] Build all and run the complete CTest suite.
- [ ] Repeat baseline PBR/Gaussian configs; compare requests, retained operations, native lowering calls and captures. Explain warmup and native GUI exclusions.
- [ ] Independently review the resulting change; resolve actionable findings and rerun affected checks.
- [ ] Report measured counts and limitations; leave changes uncommitted.

## Execution ledger

- User addition: capture current per-frame operation counts before optimization; compare identical workloads afterward.
- Ruling: work in the current checkout because the feature builds on already-authorized uncommitted RHI changes; a new worktree would omit those dependencies.
