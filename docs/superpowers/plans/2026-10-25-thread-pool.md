# Thread Pool Completion Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement inline, then independent review.

**Goal:** Make existing completion, admission and shutdown behavior explicit and release task captures promptly.
**Architecture:** Keep priority queue and pending counts. Clear worker callables before completion; serialize shutdown admission.
**Tech Stack:** C++ modules, CTest, existing child-process harness, semaphore-controlled concurrency tests.
**Spec:** docs/superpowers/specs/2026-10-25-thread-pool-design.md (records user-approved proposal).

## Global Constraints
Preserve previous uncommitted outcome extraction and unrelated workspace files. No commit/push requested.
No batch tokens or snapshot waits; RHI ABI 55 unchanged. Empty batch remains a no-op.

## Review Focus
Discard and execution both release captures before publishing completion, without holding access_mutex.
Admission cannot race shutdown. Batch rejection must be atomic with respect to queue publication.
Multiple waiters must all wake; destructor joins before synchronization members die.
User-controlled destructors may query/post to the pool but must not wait for themselves.

## Task 1: Tests and fix
- [x] Extend test_utility with capture lifetime/reentrant query, empty batch rejection, live priority drain,
      unrelated priority isolation, shutdown wait/discard and invalid worker-count subprocess regressions.
- [x] Observe RED for retained captures, accepted empty callables and missing positive-count diagnostic.
- [x] Update thread_pool.cppm: validate input, serialize admission, release execution/discard callables
      outside lock, then decrement pending and notify_all; correct API documentation.
- [x] Focused test_utility GREEN, repeat controlled concurrency tests.

## Task 2: Integration and verification
- [x] Include existing vessel_child.cpp in test_utility; confirm thread_pool module in CI tidy gate.
- [x] Full Release build -j14, CTest, device/runtime smoke as appropriate, CI tidy and independent review.
- [x] Record results and limits. Leave changes uncommitted.

## Execution ledger
Inline in current workspace, preserving the already-tested utility/outcome split. A separate worktree
would omit those dependencies; no concurrent edit conflict exists. The spec is a record of the just-approved
proposal, not a new design decision requiring another confirmation.

Completed: original RED 119 checks/8 failures; final test_utility 147/0 repeated 20 times;
Release build exit 0, CTest 71/71, device 7/7, runtime device1174/0, tidy exit0 covering39 unique
gate inputs (one duplicate thread_pool entry in 40-entry run was removed). Final independent review
found no actionable issues. Direct std::function movement was rejected after a small-callable
destructor regression timed out: payloads now use memory::allocator-backed unique_ptr storage,
constructed/copied outside the lock. See docs/reviews/2026-10-25-thread-pool.md. No commit/push.
