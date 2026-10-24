# RHI command recorder implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement this approved plan inline.

**Goal:** Share command recording vocabulary without widening submission permissions.

**Architecture:** `command_recorder` owns the virtual recording interface. `command_buffer` and `mergeable_command_buffer` remain sibling owners; only the former exposes `execute` and individual submission. Extensions accept one recorder pointer/reference, and Vulkan resolves it through its live-object registries before accessing implementation state.

**Tech Stack:** C++ modules, Vulkan, CMake/Ninja, existing contract and device tests.

**Spec:** Approved design in this conversation; background: `docs/reviews/2026-10-24-rhi-ue-comparison.md`.

## Global constraints

- RHI ABI 49; rebuild backend and GUI together.
- Keep direct owner calls; no command_list or recording view.
- Preserve resource ownership, pending retention, heap layout and submission semantics.
- Preserve existing workspace changes and use 14 build jobs.

## Review focus

- A recorder or mergeable buffer cannot be individually submitted or receive execute.
- Foreign recorder pointers must fail before a backend cast.
- Heap bind/push and scoped synchronization work on both kinds of recorder.
- Reserved pipeline push bytes remain protected on both recording paths.
- GUI heap rebinding and existing frame/batch retention remain intact.

### Task 1: Contract and callers

- [x] Add compile-time recording/submission boundary tests and observe failure.
- [x] Extract common recording interface and bump ABI; keep typed factories/submission.
- [x] Replace dual heap targets and migrate runtime/GUI/mock callers.
- [x] Build ownership test and all affected targets.

### Task 2: Vulkan resolution and regression coverage

- [x] Share validated recorder resolution between heap operations and scoped synchronization.
- [x] Cover mergeable recording, foreign handles and reserved push range with existing device tests.
- [x] Remove redundant engine recording branches while retaining typed ownership.
- [x] Run full build, CTest and relevant device/GUI smoke checks.
- [x] Obtain independent review, fix findings and record results.

## Execution ledger

- Contract test observed red (missing command_recorder), then green. Full build and final CTest: 68/68.
- Review completed twice with a fresh reviewer; no actionable findings.
- Ruling: retain the internal mergeable-to-secondary forwarding adapter; sharing contract vocabulary and validated resolution avoids redesigning submission retention.
- Ruling: do not repair the pre-existing texture-group heap layout in this round; baseline ABI48 reproduces device-suite failures. Reserved-push device test was added but cannot reach its assertions until that separate issue is repaired.
- Optional radix device checks are skipped because GPU sorting is unavailable; no real mergeable GPU-sort coverage claimed.
- Details and raw evidence references: docs/reviews/2026-10-24-rhi-command-recorder.md.
