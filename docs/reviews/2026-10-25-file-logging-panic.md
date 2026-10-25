# File-only logging and panic - acceptance (2026-10-25)

Status: **accepted with follow-ups.** Independent review:
`docs/reviews/2026-10-25-file-logging-panic-review.md` (0 Critical / 5 Important / 11 Minor). The
implementation checkpoint is `b3774b0d`; this acceptance pass adds the rotation/console/ordering tests,
corrects the stale comments, records the hook-scope decision, and fixes the outcome Doxygen group.

## Delivered behavior

- **Ordinary logging:** every build writes asynchronous `[ERROR]`/normal lines to `deren-debug.log`;
  startup rotates the previous content to `deren-debug.log.old` with a session timestamp and an 8 MiB cap.
  No logging route falls back to stdout/stderr. A file failure falls back only to the emergency file.
- **Fatal evidence:** `panic` writes its bounded message and source location directly to `deren-crash.log`
  before raw frames and best-effort readable symbols, then gives an existing async sink at most 250 ms of
  atomic-prefix polling before `std::terminate`. `fast_panic` is bounded plain text with no formatting, stack
  capture, symbolization or log wait. POSIX signal handlers use only output preopened at install time; a failed
  emergency open is not retried recursively.
- **Prefix completion:** `wait_log_all()` snapshots the submitted sequence under the queue lock and waits for
  that prefix's write+flush; later messages do not extend the wait, a closed sink does not falsely complete,
  late writes are dropped before incrementing submitted, and the worker cannot wait on itself. `panic` does not
  call `wait_log_all()`; it uses the lock-free bounded prefix poll.
- **Test hooks:** compiled only for `VR_BUILD_TESTS=ON` (the default Release package config), inert without a
  caller, absent from the module interface. This follows the existing `DEREN_MEMORY_TESTING` precedent; a
  dedicated test-support option remains a possible future split.

## Verification (current tree)

- Release build: PASS.
- Release CTest: **71/71 PASS** (includes `test_utility` 162 checks, `test_stacktrace` 85,
  `test_runtime_injection` 96, `test_vessel` 98).
- Focused additions: `test_log_rotation` precreates a previous-session log, runs a child, and verifies the
  current file, `deren-debug.log.old` timestamp/content and console cleanliness; thread-pool invalid-count
  and panic tests now assert no library diagnostics on the redirected console; panic report asserts
  message/source-location before the raw frame block.
- `test_docs` **477/0 PASS**; clang-format-check PASS; `check-backend-boundary --require-zero` PASS;
  Doxygen **0 warnings** (engine-outcome group defined); `git diff --check` clean.
- Frozen render gates were run on `b3774b0d` (15/15, visibility tracked reference matched); this acceptance
  pass changes tests/comments/docs only.

## Review disposition

| Review finding | Disposition |
| --- | --- |
| I1 rotation untested/newly active in Debug | **Fixed**: `test_log_rotation` covers `.old` content, session timestamp and current-file isolation through a child process. The 8 MiB cap and once-per-process claim remain code-verified only. |
| I2 stale Release/Debug and `panic` calls `wait_all` comments | **Fixed**: shared sink/utility/CMake/init_utils comments now state every-build file routing and the lock-free bounded panic path. |
| I3 "test-only hooks" scope overstated | **Documented/accepted**: hooks are compiled when `VR_BUILD_TESTS=ON`, exported but inert and not in the interface; same precedent as memory hooks. |
| I4 POSIX sink coverage narrower than "Windows and Linux" | **Limitation**: Linux CI builds only the standalone stacktrace probe, not the async sink/rotation/panic; recorded below. |
| I5 asserted properties weaker than plan | **Partially fixed**: added console-clean checks for the thread-pool invalid-count child and panic source-location ordering, plus rotation; remaining gaps below. |

## Limits and follow-ups

- The 250 ms panic budget is asserted by completion under the blocked-logger test, not measured directly; a
  finite multi-second wait would still pass that child.
- POSIX async sink/rotation/panic/late-write paths are code-reviewed only; Linux CI compiles the standalone
  probe, not `shared_utility`.
- An unwritable working directory now yields **no fatal evidence at all** (console fallback removed by design).
- Concurrent panics from different threads each write a full report and may interleave; same-thread reentry is
  correct. Raw frames are duplicated (explicit raw block plus symbolizer evidence). Rotation reads the previous
  log into memory without an input size cap. First-ever sink use during static destruction loses its line.
- Debug-route identity is structural (no NDEBUG branches remain) plus the Release run; no Debug test binary or
  MSVC build was exercised here. POSIX `LOG_TEST_API` export/import is reasoned, not compiled on this host.
- Remaining test gaps: worker-self-wait, panic on the worker, recursive panic reentry, uncreatable
  `deren-crash.log`, an explicit 250 ms bound, and console-clean assertion in `test_runtime_injection`.

## Verdict

**Accepted with follow-ups.** The file-only routing, bounded panic evidence, prefix completion and shutdown
semantics are implemented and covered by passing CPU tests; the remaining items are test/coverage/documentation
follow-ups, not correctness blockers for the logging change.
