# Buffer Transfer Service Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans inline; one independent review at the end.

**Goal:** Independent graphics buffer upload/readback batches with safe pending ownership and token completion.
**Architecture:** Engine service composes resource_cluster and command_submission; no ABI 54 interface change.
**Tech Stack:** C++23 modules, existing RHI, resource leases, Vulkan device probes and CPU probe tests.
**Spec:** docs/superpowers/specs/2026-10-25-buffer-transfer-design.md

## Constraints and review focus

Keep user workspace changes. No frame screenshot migration, implicit state guessing, extra queues or staging
pool. Operation inputs need owning leases from the same core. Caller spans expire after upload accepts them;
readback spans require completed GPU work and a live ticket. Failed recording must never be submitted.
Service/ticket destruction must not free pending buffers. Core lives through all retained work.

## Task 1: Batch service and ownership

- [x] Finalize the pending user choice on destruction/drain behavior and incorporate builder review.
- [x] Add transfer regression tests and compile module/test integration; observe the owning-lease test fail before its fix.
- [x] Implement transactional append/range validation, copied upload data, submit/barriers and owned pending state.
- [x] Implement poll/wait/read/collect/drain, preserving ownership on timeout/error.

## Task 2: Device integration and documentation

- [x] Verify upload/download batch readback, pending gate, dropped caller owners/tickets and scoped teardown.
- [x] Verify invalid setup/foreign core/ranges and refusal without a submission.
- [x] Document use and guarantees; run build -j14, headless tests and device probes.
- [x] Independent review; address findings and record evidence/limits.

## Completion ledger

2026-10-25: User selected independent buffer transfers and approved waiting on service-owned submissions
at destruction. Builder review led to requiring scoped_synchronization for real provenance/usage/range
validation and precise before/after scopes rather than legacy role guesses. resource_lease::from_shared
now refuses borrow(); its regression failed (53 checks, 1 failed) before the guard and passes afterwards.
Service/device tests were authored together with implementation, not an independently observed pre-feature
behavioral red cycle. They exercise queue-gated pending ownership and actual copied output.

Full build with 14 workers passed, CTest 69/69, device lane 6/6; new transfer probe 65 checks/0 failed,
zero Vulkan validation errors. Documentation 439/0 and format/diff checks pass. Independent read-only review
found no important issues. No frame completion extension, screenshot migration, staging pooling, benchmark,
commit or publication is included in this checkpoint.
