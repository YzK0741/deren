# File-only logging and panic review

Date: 2026-10-25. Independent read-only review of the file-only logging/fatal-report work, against
`docs/superpowers/specs/2026-10-25-file-logging-panic-design.md` and
`docs/superpowers/plans/2026-10-25-file-logging-panic.md` (plan still marked 0/5). The work is
contained in the checkpoint commit `b3774b0d` (working tree clean apart from untracked artifacts), so
the pre-change behaviour was read from `b3774b0d~1` rather than assumed. No code, CMake or existing
doc was modified; this file is the only addition.

**Verdict: accepted with follow-ups.** No Critical defect. The design's four testable claims (file-only
ordinary routes, bounded panic/fast_panic evidence, prefix completion, preopened signal output) are
implemented and the targeted tests that cover them pass. The follow-ups are documentation drift inside
the changed files, an overstated "test-only hooks" claim, and test coverage that is thinner than the
plan's checkbox wording implies.

## Method and evidence

- Read every file named in the brief; diffed `source/utility/shared/{shared_utility.cpp,stacktrace_native.cpp,stacktrace_posix.cpp,stacktrace_native.hpp}`,
  `source/utility/utility.cppm` and the tests against `b3774b0d~1` to separate new behaviour from
  pre-existing behaviour.
- Ran the current Release binaries (built 20:34, sources last touched 20:20–20:27, so the artifacts
  match the tree): `test_utility` 153 checks / 0 failed, `test_stacktrace` 83 checks / 0 failed,
  `test_vessel` 98 checks / 0 failed. No GPU, device or render gate was run.
- Inspected the artifacts those runs produced (`panic.log.report`, `blocked-panic.log.report`,
  `fast.log.report`, `no-stderr.log.report`, `lazy-fallback.log.report`, `deren-debug.log`) and the
  export table of the Release `shared_utility.dll` (`llvm-readobj --coff-exports`).

## Verified against the review focus

1. **File-only ordinary routes, identical in Debug and Release.** `log_sink::log_sink` now
   unconditionally rotates and opens `deren-debug.log` (`source/utility/shared/shared_utility.cpp:193-197`),
   and every `NDEBUG` branch is gone from the sink: the worker writes the file with `std::flush`
   (`shared_utility.cpp:245-249`), `error_text` always queues `[ERROR] …` (`:313-315`); the previous
   Release-only file / Debug-stdout split and the Debug-red-stderr `error()` path existed in `b3774b0d~1`
   and are removed. The sink's only non-file route is the emergency file on write failure
   (`shared_utility.cpp:250-253` → `stacktrace_native.cpp:195-198`). `grep` finds no first-party
   `stdout`/`stderr`/`printf` in the logging or fatal routes; the only remaining stream users are the
   explicit `print`/`println` API (`source/utility/utility.cppm:231-264`), tests and vendored code.
2. **Panic evidence, ordering and bounds.** `panic` writes the bounded message and source location
   first (`shared_utility.cpp:326-327` → `stacktrace_native.cpp:199-215`), then raw frames (`:328-329`),
   then best-effort symbols (`:330-334`), then a 250 ms atomic-only prefix poll (`:337-339`), then
   `std::terminate()`. The produced report confirms the order:
   `deren panic: …` / `source: …test_stacktrace.cpp:205 in int main(…)` / `deren raw stack v1 …` /
   `frame pc=…` / `call stack:` / `symbols frames=0..0`. The message is capped at 4096 bytes
   (`stacktrace_native.cpp:202`); the location line is built in a 512-byte stack buffer
   (`stacktrace_native.cpp:45-71,204-215`).
   `fast_panic` writes `"deren fast_panic: "` + ≤4096 bytes + newline and aborts, with no formatting,
   stack capture, symbols or log wait (`stacktrace_native.cpp:326-336`); the `--long-fast` child's
   report contains exactly 4096 `x` (`source/tests/test_stacktrace.cpp:239-242`) and no `frame pc=`.
   Reentry terminates on two levels: `panic`'s thread-local guard takes `fast_panic("recursive panic")`
   (`shared_utility.cpp:322-325`), and `fast_panic`'s own guard `_Exit`s (`stacktrace_native.cpp:327-331`);
   the allocation path has its own third guard (`allocation_failure.cpp:20-25`).
   Signal paths never lazily open: the POSIX handler calls only `write_preopened_text`
   (`stacktrace_posix.cpp:58`, changed from a raw `stderr` write) and `install_handler` preopens
   (`:88-89`); the Windows filter's writes go through the same attempt-once `prepare_output`
   (`stacktrace_native.cpp:155-177`), which returns false rather than retrying when the creation before
   it failed — so a failed open can neither recurse nor allocate.
3. **Prefix completion.** `wait_all` takes the submitted sequence under the queue lock
   (`shared_utility.cpp:289-290`), waits for `completed >= target` (`:294-296`), and the worker bumps
   `completed` only after the write+`std::flush` (`:245-258`). The old `|| !accepting` escape that let a
   closed sink report pending work complete is removed (`b3774b0d~1` had
   `wait(... pending == 0 || !accepting)`). A late write is dropped before `submitted` is incremented
   (`:266-277`), so a dropped message is neither "pending" nor "complete"; `shutdown` now clears
   `accepting` and `running` under the same lock (`:210-213`), closing the store-outside-the-lock window
   the old code had. `test_log_wait_is_a_flushed_prefix` genuinely asserts the property: the worker is
   held inside the write hook, the waiter's snapshot is observed through a counter, a *later* message is
   queued, and only then is the first write permitted — the waiter must return while the second message
   is still held, and the file must contain the first line and not the second
   (`source/tests/test_utility.cpp:513-538`). Because the check reads the file from a second handle, an
   implementation that wrote without flushing would fail it.
4. **Test hooks.** `DEREN_LOG_TESTING` and `log_test_hooks.cpp` are added to `shared_utility` only under
   `VR_BUILD_TESTS` (`CMakeLists.txt:1253-1256`); the hook header is included and called under the same
   macro (`shared_utility.cpp:6-8,242-244,291-293`), and nothing in the module interface changed. No
   production setting or callback is introduced. See finding I3 for the caveat about `VR_BUILD_TESTS`
   defaulting to ON.
5. **Docs.** `README.md:451` and `docs/dynamic_runtime.md:12-28` describe the new routing correctly
   (all builds → `deren-debug.log`/`.old`, panic → `deren-crash.log` first, no console fallback, 250 ms
   existing-sink poll, preopened POSIX signal output); `docs/slang_migration.md:642` already speaks of
   `deren-debug.log`. See I2 for the comments that still describe the old behaviour.

## Findings

### Critical

None found. The two properties most likely to be broken — a busy logger hanging `panic`, and a waiter
hanging on a closed sink — are both implemented and both covered by a passing test
(`blocked-panic.log.report` is written while the worker is parked forever; `--late-log` exits 0 after a
post-shutdown `log_text` + `wait_log_all`).

### Important

**I1 — Log rotation has no test, and it is newly active in Debug builds.** Rotation now runs for every
build (`shared_utility.cpp:47-100,194`), whereas before it was behind `#ifdef NDEBUG`; Debug sessions
therefore begin truncating/rotating a real log for the first time. No test anywhere asserts the
`.old` content, the session timestamp, the 8 MiB cap, or the once-per-process claim (no test mentions
`deren-debug.log.old`; `source/tests/test_stacktrace.cpp:418-421` only checks that the *current* file
receives the exit-drain line). Check #1's "rotation `.old`" half is therefore code-verified only.

**I2 — Stale comments inside the changed files still describe the old Debug/Release split,** which
contradicts the design's "Debug and Release route identically" and will mislead the next reader:

- `shared_utility.cpp:37-38` — "Startup rotation for the Release log file … Only called in Release
  builds (NDEBUG)" (the guard was deleted by this change).
- `shared_utility.cpp:139` — `std::ofstream file = {}; // Release builds write to deren-debug.log`.
- `shared_utility.cpp:118-122` and `:136-137` — the class comment says `drained_cv` wakes
  `wait_all()` "which `panic()` calls before `std::terminate()`". `panic` no longer calls `wait_all`
  (it calls the lock-free `wait_prefix_for`); that comment describes exactly the design the spec
  forbids, so it is the most dangerous stale sentence in the change.
- `source/utility/utility.cppm:579` — `log` documented as "output goes to the terminal in Debug
  builds"; `:605-606` and `:619-620` — `error` documented as "in Debug builds prints directly to stderr
  in red (not queued); in Release builds …". The bodies (`:612-615,623-625`) have no such branch.
- `CMakeLists.txt:246-248` — "the shared module writes its own stdout/stderr fallback instead of
  calling this module's `println`"; `CMakeLists.txt:998` — "Debug keeps the console for terminal log
  output".
- Adjacent, same drift: `source/backends/vulkan/core/init_utils/init_utils.cppm:489` — "Debug prints
  red to stderr, Release writes to the log file".

Plan Task 3 ("migrate … docs to explicit output files") is therefore only half done: the two
user-facing docs were updated, the code comments were not (and `utility.cppm`'s `log` note was edited
in this commit without correcting its Debug claim).

**I3 — The "test-only hooks" wording is stronger than the build.** `VR_BUILD_TESTS` defaults to `ON`
(`CMakeLists.txt:1179`), and that is the configuration of the Release package: the shipped
`build-release-dyn-clang64/shared_utility.dll` exports all five hooks
(`utility_log_test_pause_after`, `_pause_count`, `_snapshot_count`, `_before_write`, `_did_snapshot`).
So the hooks are compiled into — and callable from — the production DLL and add a call per log line on
the worker path, even though they are inert without a caller and absent from the module interface. This
is the same pattern `DEREN_MEMORY_TESTING` already uses (its hooks are exported from the same DLL), so
it is defensible; but "compiled only for test builds (not production)" should be stated as "only when
`VR_BUILD_TESTS=ON`", and a decision recorded: accept the precedent, or move both hook TUs behind a
dedicated test-support option. The clang64 CI build is where they are exercised; the MSVC
`dllexport`/`dllimport` pairing (`log_test_hooks.hpp:3-9`) is reasoned, not compiled.

**I4 — POSIX coverage is narrower than the design's "Windows and Linux" claim.** CI's Linux job builds
and runs only the standalone probe from `stacktrace_native.cpp` + `stacktrace_posix.cpp`
(`.github/workflows/ci.yml:34-52`); `shared_utility.cpp` (the sink, rotation, exit drain, panic) and
`log_test_hooks` are never compiled on Linux, and the hooks' POSIX `LOG_TEST_API` is untested. On the
Windows host this review could not build the module for POSIX. Concretely unverified on POSIX: the file
routing of ordinary logs, rotation, `--exit-log`-style drain, late-write drop, the lazy-open window, and
`deren::utility::panic` itself. The POSIX half of `log_rotation_claim.cpp:50-60` is also per-image, not
per-process, and untested — its comment (`:52-53`, "written, NOT measured - CI is windows-latest for
both jobs") is stale now that a Linux job exists, even though that job does not build the sink.

**I5 — Several asserted properties are weaker than the plan implies.**
- The "no library diagnostics on console" check exists on the panic/crash children
  (`test_stacktrace.cpp:106`), the allocation children (`test_vessel.cpp:148`) and the POSIX probe
  (`test_stacktrace_posix.py:29,73,81`), but not for the thread-pool invalid-count child
  (`test_utility.cpp:494-505` checks only that the message is in `deren-crash.log`) nor in
  `test_runtime_injection` (its `child_output.txt` is printed on failure, never asserted clean).
- `panic`'s "source location before raw frames" is checked for presence, not order
  (`test_stacktrace.cpp:426-428` asserts the message precedes `frame pc=0x`, and that
  `test_stacktrace.cpp` appears somewhere).
- No test drives the worker-self-wait guard (`shared_utility.cpp:286-288,299-300`), a panic *on* the
  worker thread, reentry ("recursive panic"), or an uncreatable `deren-crash.log` (the failed-open
  no-retry path at `stacktrace_native.cpp:155-157`); all four are code-verified only.
- The `--panic-blocked-log` child proves only that the panic completes eventually — the harness gives
  it 10 s (`source/tests/vessel_child.cpp:57`) — so a regression to a multi-second (but finite) log
  wait would still pass; there is no assertion on the 250 ms budget.
- `test_runtime_injection`'s `last_panic()` reads the last `deren panic: ` marker in an append-only
  `deren-crash.log` (`test_runtime_injection.cpp:228-233`), so a child that terminated without writing
  would silently inherit the previous child's text instead of failing.

### Minor

- **M1 — Multi-thread reentry is not detected.** `panic`'s guard is `thread_local`
  (`shared_utility.cpp:322-325`), so two threads panicking concurrently each write a full report and
  race the symbolizer and the 250 ms poll; each report is built from several separate `write` calls
  (`stacktrace_native.cpp:199-215`), so their lines can interleave. Same-thread reentry (the case the
  spec names) is correct.
- **M2 — The self-wait guard is published late.** `worker_id` is stored after
  `prepare_crash_thread()` (`shared_utility.cpp:224-225`), so a fatal call in that tiny window would
  poll its own prefix for the full 250 ms before terminating (bounded, not a hang).
- **M3 — Raw frames are written twice in a panic report:** `write_stack` (`shared_utility.cpp:329`)
  and then `symbolize`'s own raw evidence (`stacktrace.cpp:86-88`) are both emitted, so the raw block
  appears before and inside `call stack:`. Harmless duplication of evidence, already visible in the
  captured `panic.log.report`; noted for size, not correctness.
- **M4 — Rotation reads the whole previous log into memory with no size cap**
  (`shared_utility.cpp:63`; the 8 MiB cap applies to `.old`, not to the input). Pre-existing, but it
  now runs in Debug too.
- **M5 — Test-hook robustness.** A paused worker spins in `utility_log_test_before_write`
  (`log_test_hooks.cpp:32-33`); if a test failed without re-arming `pause_after(-1)`, the atexit drain
  would join a parked worker and hang the test binary rather than report a failure. The current prefix
  test resets on every path (`test_utility.cpp:521-523,535`), so this is a harness fragility only.
- **M6 — First-ever use during static destruction** constructs the sink after `exit` has begun, so the
  queued line is never drained (the atexit registration made there will not run). Not the "dead sink
  recreated" case the spec forbids — the never-destroyed sink is reused, and a late write on an existing
  sink is correctly dropped (`shared_utility.cpp:266-277`, exercised by `--late-log`) — but a
  first-touch at teardown loses its line. Pre-existing shape; worth one sentence in the limits.

## Plan checkbox assessment

- **Task 1 (RED tests: file evidence, no console diagnostics, blocked logger, prefix completion, raw
  decoder preserved)** — *satisfiable, with I5's gaps.* Every listed property has a test except rotation,
  reentry, uncreatable-file, worker-self-wait and (for two tests) console cleanliness. RED-capability is
  real: against `b3774b0d~1` the new tests must fail, because the old `panic` routed through
  `error_text` and an unbounded `wait_log_all` (the blocked-logger child would be killed at 10 s), the
  old sink wrote to `stdout` in Debug and fell back to `stdout` on file failure, and the old
  `write_text` tried `stderr` first. No recorded RED run exists in the tree, so that part is inferred
  from the diff, not observed.
- **Task 2 (native file output, early panic body, identical routing, prefix counters, shutdown wake,
  250 ms mutex-free panic wait)** — *genuinely satisfied and verified* by code reading plus the three
  passing CPU tests and the produced reports.
- **Task 3 (migrate allocation, thread-pool panic, Windows/POSIX stack tests and docs)** — *partially
  done.* Tests are migrated; `README`/`docs/dynamic_runtime.md` are updated; the comments in I2 are not.
  This checkbox should not be marked until those lines are corrected (they are comments, so no
  behaviour change).
- **Task 4 (focused tests, full `-j14` build, CTest/device/runtime, tidy, independent review)** — *Lead's
  gate.* This review covers only the independent-review half; locally I ran the three CPU tests above
  (no build, no CTest, no device/render).
- **Task 5 (record limits and evidence; leave uncommitted)** — *not satisfiable as written:* the work is
  already committed in `b3774b0d`, contrary to "No commit/push requested … leave uncommitted". The plan
  wording needs to record the checkpoint (and the remaining plan/review edits are the uncommitted part).
  A limits paragraph should also state: the 250 ms budget bounds our log wait only, not symbolizer
  budget or kernel I/O; an unwritable working directory now yields *no* fatal evidence at all (the
  console fallback is gone by design); concurrent panics interleave; POSIX sink behaviour is
  code-only; and the test hooks live in any `VR_BUILD_TESTS=ON` artifact.

## Unverified / limits of this review

- No Debug-build test run: `build/` is configured Debug but contains no test executables, and no build
  was performed. "Debug and Release route identically" is verified structurally (no `NDEBUG` remains in
  the routes) plus the Release run, not by executing a Debug binary.
- No MSVC build; the `LOG_TEST_API` export/import branch and the `#if defined(_MSC_VER)` half of the
  Windows detection are unverified (CI drives clang64).
- POSIX sink/panic/rotation were not compiled or run on this Windows host (see I4).
- GPU, device, runtime, full CTest and clang-tidy gates were out of scope.
- The 250 ms panic bound is asserted by completion, not by measurement; the passing blocked-logger child
  is consistent with a ≤10 s wait.
