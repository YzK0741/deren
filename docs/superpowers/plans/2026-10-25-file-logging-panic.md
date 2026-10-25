# File-only Logging and Panic Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans inline; obtain independent final review.

**Goal:** Make logs and fatal evidence useful without a console and clarify log-prefix completion.
**Architecture:** Existing async deren-debug.log writer plus native emergency deren-crash.log writer;
submitted/completed counters implement prefix waiting and an atomic-only bounded panic drain.
**Tech Stack:** C++ modules, Win32/POSIX file writes, existing child harness, CTest/Python tests.
**Spec:** docs/superpowers/specs/2026-10-25-file-logging-panic-design.md; records approved proposal + file-only steering.

## Global constraints
Preserve previous utility outcome/thread-pool changes and unrelated files. No commit/push requested.
No RHI ABI change. Fast panic remains no-format/no-stack/no-log-wait. No console fallback in logging/fatal routes.

## Review focus
Emergency text survives blocked async logger and appears before raw frames. Signal code must not lazily
open or allocate. Reentry must terminate; failed file creation must not recurse. Prefix completion must
include flush and wake before newer work finishes. Late writes must not recreate a dead sink.

## Tasks
- [ ] RED: update fatal tests to require file evidence and no library diagnostics on console; add
      blocked logger/prefix completion tests with test-only pause hooks, preserving raw decoder coverage.
- [ ] Implement native file output/early panic body, identical Debug/Release file routing, prefix
      counters, shutdown wake correctness and bounded panic wait of 250ms without queue mutex.
- [ ] Migrate allocation, thread-pool panic, Windows/POSIX stack tests and docs to explicit output files.
- [ ] Focused tests then full -j14 build, CTest/device/runtime regression, exact tidy and independent review.
- [ ] Record limits and evidence; leave uncommitted.

## Execution ledger
Inline current workspace preserves previous approved uncommitted changes. Extra test-only hooks pause
the log writer and observe snapshot acquisition, avoiding timing guesses about queue state. No production
callbacks or public logging settings are introduced. The spec records existing approval, not a new gate.
