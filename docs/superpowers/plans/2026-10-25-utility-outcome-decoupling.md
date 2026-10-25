# Utility / Engine Outcome Separation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make utility independent of RHI while preserving existing error-classification behavior.
**Architecture:** Move result/verdict/propagate/enforce to deren.engine.outcome, namespace deren::engine::outcome.
Keep ensure/panic/logging in utility; no compatibility re-export that would retain the dependency.
**Tech Stack:** C++ modules, CMake, CTest and Python boundary tests.
**Spec:** User-approved first step from the utility audit: move RHI-specific outcome handling into engine.

## Global Constraints

Use the existing workspace; preserve unrelated untracked files. No RHI ABI change (55).
Scope is the outcome dependency split; thread pool and file I/O remain follow-up work.

## Review Focus

Utility must neither import nor link promise transitively. Engine consumers must explicitly import the new
module. Success values, rewritten error_info and fatal guards preserve behavior. CI must build the moved
tests in sanitizer jobs. Shared sink/panic ownership remains process-wide.

### Task 1: Regression boundary

- [x] Add a Python boundary test checking utility imports and direct static_utility links.
      Audit the generated CMake dependency graph separately for transitive dependencies.
- [x] Observe it fail on current RHI import/link; retain a headless utility-only consumer test.

### Task 2: Outcome module and consumers

- [x] Create source/engine/outcome/outcome.cppm exporting result<T>, verdict, pass_success, pass_failure,
      fatal, propagate and enforce in deren::engine::outcome; preserve algorithms and fatal behavior.
- [x] Remove those types/RHI import from utility and promise from static_utility links.
- [x] Migrate runtime.frames.cppm, move outcome tests to test_engine_outcome, and retain ensure checks in test_utility.
- [x] Add engine_outcome_core target, expose it to deren_engine, register tests and update CI target list.
- [x] Run focused tests and generated dependency closure audit.

### Task 3: Verification and documentation

- [x] Update current dependency comments/docs, explicitly document the source migration and retained ABI55.
- [x] Full build -j14, CTest, relevant device probes, exact CI tidy gate and independent review.
- [x] Record evidence/limits; leave work reviewable without assuming a commit/push request.

## Execution ledger

Ruling: implement inline in current workspace, as explicitly authorized by the user. A standalone
engine_outcome_core module target lets outcome tests run without the renderer.
Pre-flight: runtime and moved tests both consume the same exported module; utility tests lose their only RHI import.

Boundary ruling: the durable source check guards imports/direct links; a separate traversal of the
actual generated CMake Graphviz graph verifies the current transitive closure. No generic CMake parser
is introduced. The closure contains utility, vstd, xxhash, mimalloc and platform libraries only.

Completed: full Release build exit 0; CTest 71/71; device probes 7/7; runtime device 1174/0;
39-source CI tidy exit 0; independent review found no actionable issues. Evidence and limits are in
`docs/reviews/2026-10-25-utility-outcome-decoupling.md`. No commit/push performed.
