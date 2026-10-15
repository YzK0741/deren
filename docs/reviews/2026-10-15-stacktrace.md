# Stack diagnostics integration

Implemented in the existing dirty workspace; previous resource and vessel changes
are preserved. No commit or push was made.

The final user correction is authoritative: `fast_panic(string_view)` is a
bounded raw-text-plus-abort entry with no format overload, source location or
stacktrace. It bypasses the logger, symbolizer and project allocator. OOM retains
its fixed-storage reason/bytes/alignment diagnostic and calls that native entry.
Raw stack capture remains available for ordinary panic and explicit diagnostics.

The service lives in shared_utility. Windows x64 crash unwinding starts from the
exception context, with the fault PC emitted first. Stack overflow skips unwind;
each project worker prepares its own stack reserve. Missing stderr is handled by
a handle preopened during startup. Healthy symbolization is bounded and retains
raw module/RVA records on failure; native and offline subprocesses contain their
descendants. The offline tool successfully recovered trace_fault and main source
lines from the real access-violation log. Missing system DLL symbols remain raw.

Validation evidence is in `2026-10-15-stacktrace/`:

- Initial missing-API RED: `red.log`.
- Corrected fast path RED: `fast-minimal-red.log` (2 unexpected origin/stack
  outputs); removing them passed the same assertions.
- Missing-stderr RED: `fallback-red.log`; startup-preopened fallback now passes.
- Full optimized build: `final-build-j8.log` and final `review-build.log`.
  An earlier j14 run exhausted Clang memory; j8 completed without changing flags.
- Full CTest: `final-ctest.log`, 28/28 passed. Stacktrace: 41 checks; vessel: 91.
  Includes DLL callback frames, source lines, depth/skip, injected allocation
  failure, no format overload, 4096-byte text cap, missing tool, timeout/output
  bounds, ordinary panic, actual AV, main/worker overflow and absent stderr.
- ASAN Debug: `final-asan-build.log`, `final-asan-tests.log`, 2/2 passed;
  stacktrace 33 checks and vessel 91. ASAN intercepts real faults before the OS
  filter (`asan-tests.log` and child logs); those 8 OS-specific assertions run in
  Release, while ASAN keeps its own handler. Sanitizers were not disabled.
- Offline containment: `offline-tests.log`, 3 tests passed, including descendant
  cleanup after timeout and successful parent exit. Full CTest includes them.
- Production diagnostic DLL, tests OFF: `production-build.log` and
  `production-symbols.log`; diagnostic entries present, test hooks absent.
- Lean diagnostics OFF, tests OFF: `lean-build.log` and `lean-flags.log`;
  shared_utility compiled/linked with prior lean Release settings.
- 15 scene captures: `captures.log`, `pixel-comparison.json`, `pixels.log`;
  clean validation and zero differing pixels against archived ownership baselines.
- Gaussian CPU/GPU, 40 frames each: `gaussian.log`,
  `gaussian-comparison.json`, `gaussian-pixels.log`; each path separately matches
  its archived RGBA pixels and HDR bytes exactly, with clean validation.
- Offline actual-crash decode: `final-offline-crash.log`; partial decode returns
  nonzero when system symbols are unavailable, preserving all raw frames.

Read-only review found offline process-tree containment and per-thread overflow
reserve gaps; both were fixed. Follow-up review verified Windows ctypes layouts,
handle permissions and cleanup, then identified two Gaussian worker entries;
those were included and the full suite rerun.

Usage and platform limits: [stacktrace manual](../stacktrace.md). Windows Clang
is validated. MSVC and POSIX runtime behavior were not tested; no POSIX signal
handler was introduced. OS writes/unwind/module queries remain best effort.
