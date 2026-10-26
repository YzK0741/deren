# Bounded File Reads Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans inline, then request independent review.

**Goal:** Explicitly bounded, diagnosable binary reads without RHI dependency.
**Architecture:** Generic file_error + expected returns; one shared container-templated implementation
in a dedicated ordinary utility implementation unit; no old unbounded overload.
**Tech Stack:** C++23 modules, std::ifstream, vessel containers, CTest and existing allocation test hook.
**Spec:** docs/superpowers/specs/2026-10-25-bounded-file-reads-design.md (records user-approved scope).

## Global constraints
Preserve unrelated files and previous committed work. No commit/push requested. RHI ABI55 unchanged.
Regular files only; callers choose capacity. OOM remains fatal. No handle allocator or file-write changes.

## Review focus
Check before narrowing/allocation; preserve embedded NUL and high bytes; zero-size/zero-bound is valid.
Never return a partial read, do not conflate EOF with I/O failure, and check the actual opened stream's size.
This is not a file snapshot or hostile-path security API; do not imply tests cover concurrent mutation races.

## Task 1: Tests and implementation
- [x] Add tests for explicit expected/error contract, embedded NUL/high bytes, vector/string parity,
      exact/under/zero limit, empty file, missing path, directory rejection, >1 MiB read and rejection
      before payload allocation. Observe RED against existing one-argument optional interface.
- [x] Export file_error_code, file_error and file_error_name; require max_bytes on both reader APIs.
- [x] Implement shared bounded reader in source/utility/file_io.cpp and remove old iterator readers.
- [x] Migrate shader caller with explicit64MiB limit/diagnostics/move; wire CMake and tidy coverage.
- [x] Focused build/tests GREEN.

## Task 2: Integration
- [x] Full Release build -j14, CTest, relevant runtime/device smoke, exact CI tidy, boundary and docs checks.
- [x] Independent review; record evidence/limits and source-interface migration.

## Execution ledger
Inline current checkout: no concurrent source edits present. Spec records approved proposal and routine
choices announced in commentary; no new API architecture decision needing another confirmation.

Completed: full/final Release builds exit 0; final CTest 71/71; utility 196/0; device 7/7;
runtime device 1174/0; final 40-source CI tidy exit 0; format and diff checks clean. Review
found no implementation defect; strengthened string oversize preallocation rejection coverage
with >1 MiB fixture. Remaining untested fault/race paths and source migration are documented in
docs/reviews/2026-10-25-bounded-file-reads.md. No commit/push performed.
