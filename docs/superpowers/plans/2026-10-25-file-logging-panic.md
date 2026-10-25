# File-only Logging and Panic Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans inline; obtain independent final review.

**Goal:** Make logs and fatal evidence useful without a console and clarify log-prefix completion.
**Architecture:** Existing async deren-debug.log writer plus native emergency deren-crash.log writer;
submitted/completed counters implement prefix waiting and an atomic-only bounded panic drain.
**Tech Stack:** C++ modules, Win32/POSIX file writes, existing child harness, CTest/Python tests.
**Spec:** docs/superpowers/specs/2026-10-25-file-logging-panic-design.md; records approved proposal + file-only steering.

**Status (2026-10-25): accepted with follow-ups.** The behavior is implemented and verified by the CPU tests;
independent review is `docs/reviews/2026-10-25-file-logging-panic-review.md` (accepted with follow-ups) and the
acceptance/limits record is `docs/reviews/2026-10-25-file-logging-panic.md`. The follow-ups are POSIX sink
coverage, an explicit 250 ms budget assertion, panic/reentry/uncreatable-file probes, and the hook-scope
decision recorded below. The work checkpoint was committed in `b3774b0d`; only the final plan/review docs are
uncommitted at the time of writing.

## Global constraints
Preserve previous utility outcome/thread-pool changes and unrelated files. No commit/push requested.
No RHI ABI change. Fast panic remains no-format/no-stack/no-log-wait. No console fallback in logging/fatal routes.

## Review focus
Emergency text survives blocked async logger and appears before raw frames. Signal code must not lazily
open or allocate. Reentry must terminate; failed file creation must not recurse. Prefix completion must
include flush and wake before newer work finishes. Late writes must not recreate a dead sink.

## Tasks
- [x] RED: update fatal tests to require file evidence and no library diagnostics on console; add
      blocked logger/prefix completion tests with test-only pause hooks, preserving raw decoder coverage.
      Added: crash-log migration for OOM/thread-pool tests, console-clean assertions, flushed-prefix test, blocked
      logger panic, rotation test. Remaining gaps: worker-self-wait/panic-on-worker/reentry/uncreatable-file and an
      explicit 250 ms budget assertion.
- [x] Implement native file output/early panic body, identical Debug/Release file routing, prefix
      counters, shutdown wake correctness and bounded panic wait of 250ms without queue mutex.
- [x] Migrate allocation, thread-pool panic, Windows/POSIX stack tests and docs to explicit output files;
      corrected the stale Release/Debug and wait_all comments (I2). POSIX sink tests remain a follow-up (I4).
- [x] Focused tests then full -j14 build, CTest/device/runtime regression, exact tidy and independent review.
      Focused CPU tests, full CTest 71/71, test_docs, clang-format and boundary gates pass; independent review done.
      No device/render regression was needed (logging-only change).
- [x] Record limits and evidence in docs/reviews/2026-10-25-file-logging-panic.md (unwritable cwd = no fatal
      evidence, POSIX sink code-only, 250 ms not measured, hooks ship with VR_BUILD_TESTS=ON, concurrent panics
      interleave, raw frames duplicated). The work checkpoint is committed; this plan/review doc is the remaining
      uncommitted part.

## Execution ledger
Inline current workspace preserves previous approved uncommitted changes. Extra test-only hooks pause
the log writer and observe snapshot acquisition, avoiding timing guesses about queue state. No production
callbacks or public logging settings are introduced. The spec records existing approval, not a new gate.
