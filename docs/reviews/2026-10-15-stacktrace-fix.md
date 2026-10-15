# Stack diagnostics: the review's confirmed defects, fixed

Follow-up to [2026-10-15-stacktrace.md](2026-10-15-stacktrace.md), in the same dirty
workspace and with the preceding work preserved. No commit or push was made. Every
item below was found by the read-only review of that batch and is fixed with a
regression test that fails against the old behaviour.

## 1. The fault PC is exact, not a return address

`symbolize()` subtracted one from EVERY frame, but the crash handler's first frame is
the exception address itself. The offline decoder did the right thing
(`address = offset if pc == fault_pc else max(0, offset - 1)`), so the two decoders
disagreed on exactly the frame that matters.

MEASURED on the access-violation probe of this build:

| address handed to the symbolizer | decoded |
| --- | --- |
| `0x12628` (the fault PC, as recorded) | `trace_fault` `stacktrace_fault.cpp:18:11` (`*page = 42;`) |
| `0x12627` (one byte lower, the blanket `pc - 1`) | `trace_fault` `stacktrace_fault.cpp:16:9` (`if (!page) std::abort();`) |

- `stack_trace::exact_address_count` (in `deren.utility.shared`) names the leading
  addresses that are exact PCs; `capture_stack` leaves it zero (its frames are return
  addresses), a trace rebuilt from a crash record sets it to one.
- `stacktrace.cpp`'s `symbolizer_address()` is the single place that applies the rule,
  used for both the group head and every frame in it.
- The test rebuilds the fault frame against THIS image from the crash log's ASLR-free
  RVA (the parent's own base is read out of a raw record) and decodes it with the count
  set to one and to zero: the exact PC must land on `:18` and the adjusted one must not.
  The fault RVA is read from the log, so the check survives relinking; only the two
  source lines are pinned, like the suite's other line assertions.

## 2. The symbolizer child no longer inherits `LLVM_SYMBOLIZER_OPTS`

`CreateProcessW` was passed `nullptr` for the environment, i.e. "inherit everything",
while the offline tool explicitly drops this variable. MEASURED: an unknown option in
it makes llvm-symbolizer exit nonzero (`error: unknown argument`), which this service
reports as `failed`, and a style option (`--output-style=JSON`, `--print-address`)
would mix foreign text into a block the caller reads as the fixed protocol. The
assessment of this feature had already required "no environment variable may change the
output protocol" (`docs/reviews/2026-10-14-stacktrace/assessment.md`).

`stacktrace_native.cpp` now builds a filtered block (case-insensitive names, bounded at
1 MiB, `CREATE_UNICODE_ENVIRONMENT`) and passes it; a pathological environment degrades
to the old inherit behaviour rather than failing symbolization. The test proves both
directions: a probe tool reports what IT received (`llvm_symbolizer_opts=(unset)` while
the ordinary variable that selected the probe survives), and the real tool still decodes
the executable's frame with the stray variable set.

## 3. A clean exit with no output is not `ok`

`symbolize_group` reported `ok` for a child that exited 0 without writing anything, and
the offline decoder printed nothing while returning exit 0 - a partial decode looked
complete, and a module without symbols looked like a frame that was symbolized.

- In-process: `symbolize()` turns `ok` with zero bytes written into `unavailable`
  (raw records are kept either way, and a group that wrote nothing is indistinguishable
  from one that was never decoded).
- Offline: `symbolize_stacktrace.py` counts empty output as a failure, so a run that
  decodes nothing exits nonzero, as `docs/stacktrace.md` promises.

## 4. Crash records carry the access kind and the data address

The handler wrote the exception code and the fault PC only. An access violation's
`ExceptionInformation` says WHAT was touched (`[0]` read/write/execute, `[1]` the data
address) - the first question a corruption post-mortem asks, and the PC cannot answer
it. The crash line now reads `... fault_pc=0x... access=write data=0x...`; the offline
decoder's `fault_pc=` parse is unaffected.

## 5. Corrections to claims that were not true

- `write_trace` printed the absolute PC in the `rva=` field when resolution failed
  (`base=0x0`), while `docs/stacktrace.md` claimed raw records always retain an RVA.
  The code is right (the field is `pc - base` with `base == 0`); the document now says a
  zero base means "no RVA".
- The poll loop's comment claimed it kept polling after the child exited for
  descendants' sake; the loop breaks in exactly that case and the job's kill-on-close
  cleanup ends descendants. The comment now describes the code.

## Evidence (`2026-10-15-stacktrace-fix/`)

- `build.log`: full optimized build, 280/280 edges, no errors.
- `ctest.log`: `100% tests passed out of 28` in 33.6 s. `test_stacktrace` reports
  **50 checks, 0 failed** (it was 41 before this batch).
- `ctest-before-docfix.log`: the RED run this batch started from - the two failures were
  the env assertion explained above and three em dashes this batch had put into
  `docs/stacktrace.md` (the manual runs every `docs/*.md` through pdflatex, so the file
  must stay ASCII). Kept as the record.
- `asan-build.log`, `asan-tests.log`: ASAN, `100% tests passed out of 3`
  (`test_stacktrace_offline`, `test_vessel`, `test_stacktrace`); `test_stacktrace`
  **38 checks, 0 failed** and `test_vessel` 91. The native-fault checks are skipped
  there by design, as in the previous batch.
- `test_stacktrace_offline.py` has 5 tests now (2 new, both offline-decoder rules): a
  fake tool that echoes the addresses it was handed shows the fault PC arriving exact
  (`0x2340`) and the return PC one byte lower (`0x10ff`); a tool that exits 0 without
  writing anything makes the run exit 1 while every raw line is still printed.
  `symbolize_stacktrace.py` previously had no automated coverage at all.

## Not changed here, and why

The two items below were fixed in the second round, which follows this list; the
third is still open.

- `panic()` into a destroyed log sink, and the fatal path's fallback handle depending
  on `install_crash_handler()` having run first: both fixed in the second round.
- The loader lock turned out NOT to be a hazard for this code: the two ways a fault can meet
  a held lock were measured with a fixture DLL, and neither reaches a deadlock (see the third
  round below). What remains unattended is a hang from causes we have NOT measured (a stuck
  console write, a pathological unwind), which is a watchdog policy decision rather than a
  known defect.
- POSIX symbolization and signal handling (this is the Windows-first batch, as
  `docs/stacktrace.md` and the plan state), the always-true `install_crash_handler()`,
  and the discarded `prepare_crash_thread()` results are unchanged. The offline tool's
  basename matching and whole-log read were fixed in the second round.

## Second round: the fatal path, the sink, and the two decoders tied together

### 1. The fallback log is no longer lost before startup installs the handler

`install_crash_handler()` preopens `deren-crash.log` only when stderr is invalid at
that moment, and `main` calls it. A fatal that fires BEFORE that - an allocation
failure during static initialization, in a console-less launch - found no handle at
all and dropped its message silently: the window where the evidence matters most.
`write_text` now opens the same file itself, once per process (a monotonic flag, so a
failure is not retried), when both stderr and the cached handle are absent. That is
the fatal path's one deliberate file open: a single `CreateFileW` on a relative path,
and it costs nothing once startup has preopened the handle.

RED, measured: with the lazy open disabled the child mode `--no-stderr-no-install`
fails on `read_log("deren-crash.log")` being empty (`red-lazy-fallback.log`: 53
checks, 1 failed). `test_stacktrace` went from 50 to 53 checks with it.

### 2. The sink can no longer be reached after it is destroyed, and cannot wait on itself

The sink in `deren.utility.shared` was a function-local static, so it was destroyed at
exit - while `panic()` is callable from any static destructor ordered after it. A late
`log()`/`error()`/`panic()` then locked a destroyed mutex and signalled a destroyed
condition variable: undefined behaviour, not the "message dropped during teardown" the
accepting gate promised. The object is now never destroyed and the drain is registered
at first use with `std::atexit`, which runs in the same reverse order a static
destructor would - so the queue is still drained before the process ends, and every
later write is dropped by the accepting gate. `wait_all()` also recognises the
worker's own thread id and returns immediately there: a panic ON the log worker used
to block forever on the pending count that thread itself decrements, in the path that
must not hang.

Evidence note, stated plainly: the UB is not deterministically observable (the
destroyed object's memory stays mapped and the sticky `accepting == false` makes the
late call a silent drop on the old code too), so no RED was achievable for it. Three
checks pin the observable behaviour instead: the late writer's child exits 0 (a hang
would be a harness kill, not exit 0), the drain still writes the last queued line at
exit (`--exit-log` with no explicit wait), and `test_utility`'s concurrent-drain test
still passes (114 checks, 0 failed).

### 3. The two decoders are tied together by a test at last

This document's residual-risk note said the two implementations of one rule had no
direct comparison. The offline side (delegated, disjoint files) now says exactly what
the in-process side says:

- the exact-frame rule is POSITIONAL - the first frame after each `deren crash` line is
  exact, every later frame is a return address - instead of the value test
  `pc == fault_pc`, so a later frame that repeats the fault PC is still adjusted;
- `ntpath.basename` resolves `C:\Windows\SYSTEM32\ntdll.dll` under `--module-dir` on any
  host (measured: `PurePosixPath(...).name` is the whole string);
- a module the archive does not hold is `warning: missing module file <path>` on stderr,
  once per distinct file, and the frame FAILS without spawning a child - measured: the
  old code let the run exit 0 while handing the child a nonexistent `--obj`;
- `--budget` (default 30 s) bounds the whole decode, `--timeout` still bounds one call;
- the log is streamed line by line (measured on an 18.7 MiB / 400k-frame log: 55.8 MiB
  peak allocation before, 2.0 MiB after).

Measured RED against the pre-change script: 12 tests, `FAILED (failures=3, errors=1)` -
the repeated fault PC was handed the unadjusted rva twice, the missing module produced
exit 0 with no warning, `--budget` was an unrecognised argument, and
`module_file_name` did not exist.

And the gap itself is now a test: `test_stacktrace` runs `symbolize_stacktrace.py` on
the SAME raw record its crash child just wrote, and requires the offline output to
contain the fault line the in-process decoder was asserted on
(`stacktrace_fault.cpp:18`). A hand cross-check on a fresh real record agrees: frame 0
decodes to `trace_fault stacktrace_fault.cpp:18:11` exactly, frame 1 to
`main test_stacktrace.cpp:105`, one warning per missing system DLL, exit 1.
One measured detail the cross-check had to handle: cmd.exe strips the first and last
quote of a `cmd /c <command>` whose leading character is a quote, and this command both
begins and ends with one, so it needs a second pair around the whole thing (without it
the child dies with "the filename, directory name, or volume label syntax is
incorrect").

### Final numbers after integration

- Release, `build-release-dyn-clang64`: `100% tests passed out of 28` in 35.4 s
  (`ctest.log`); `test_stacktrace` **64 checks, 0 failed**, `test_utility` 114.
- ASAN, `build-vessel-asan-clang64`: `100% tests passed out of 3` in 6.9 s
  (`asan-tests.log`); `test_stacktrace` 45 checks, `test_vessel` 91, plus the 12 Python
  decoder tests. The native-fault and cross-check blocks are skipped there by design.
- The offline decoder suite grew from 5 to 12 tests; its earlier RED/GREEN record is in
  the sections above.
- `docs/stacktrace.md` now states the fallback's lazy open, the sink's lifetime, the
  positional rule the offline tool shares, `--budget`, the missing-module warning, the
  streaming read, and the measured loader-lock behaviour.

## Third round: the loader lock was measured, and the risk is not there

The second round's open item was: `crash_filter` resolves each frame's module with
`GetModuleHandleExW`/`GetModuleFileNameW`, those need the loader lock, and a fault could
arrive while the lock is held - so the filter might block forever, leaving neither
evidence nor an exit. Two candidate fixes were queued (write the raw PCs before any
lock-taking call; snapshot the module table at startup and map PCs from it).

**It was measured first, with an instrument built for it**: `source/tests/loader_lock_fixture.cpp`,
a test-only DLL whose `DllMain` can either fault or block, plus two child modes in
`test_stacktrace`. Both ways a fault can meet a held lock were run against the shipped
filter:

| case | lock state | MEASURED |
| --- | --- | --- |
| `--loader-lock-crash`: one thread blocks inside `LoadLibrary` (DllMain waits on an event the test never signals), another thread faults | another thread holds it, indefinitely | the record comes out COMPLETE - every frame has `base=`/`module=` - and the process terminates (`loader-lock.log`) |
| `--loader-lock-fault`: the fault is raised inside the fixture's `DllMain` | the faulting thread itself holds it | the filter is NEVER CALLED: the loader turns an exception in `DllMain` into a FAILED LOAD (`LoadLibrary` returns null), so nothing is recorded and nothing hangs |

So the premise of the queued work was wrong: the module lookup is not hostage to the
loader lock on this platform, and the other half never reaches the filter at all. The
planned fixes were therefore **not** implemented, and the reason is worth recording:

- `exact` snapshot of the module table (`A4`) would only speed up a path that runs once
  per crash, and it would trade the loader's always-current module list for a stale
  snapshot whose refresh would have to chase third-party loads (the Vulkan loader and the
  GPU driver's DLLs are loaded by code we do not own).
- Writing the raw PCs before the module lookup (`A1`) guarded against a risk that is not
  there.
- A watchdog (`A2`) is a policy decision about hangs from causes we have NOT measured. It
  is still available if a hang is ever observed; it costs the WER dump path, so it should
  be taken when there is a measured reason.

What the round leaves behind is the fixture and five checks that pin BOTH halves as
behaviour: the fatal path must keep working while another thread holds the loader lock,
and a `DllMain` fault must keep being invisible (a failed load, not a crash record). The
latter is also the reason `crash_filter` does not need to defend against its own thread
holding the lock. `test_stacktrace` is 64 checks, 0 failed.

## Fourth round: panic prints the stack, which is why `symbolize()` earns its keep

The in-process symbolizer had no production caller: only tests used it, and the offline
decoder did the real work. Instead of retiring it, `panic` now uses it - a panic report is
read by a human, and the raw records it already wrote are for the offline decoder, not for
whoever is staring at `debug.log`.

What `panic` does now, in this order:

1. `capture_stack()` ONCE, and the raw record to stderr first (unchanged: that is the offline
   decoder's input, and it survives even if everything below fails).
2. The existing message, location and timestamp lines through the log sink.
3. `symbolize(trace)` on the SAME frames, printed through the sink as `call stack:` -
   one block, best effort.

MEASURED, from the panic child's log:

```
call stack:
symbols frames=0..0
deren::utility::panic@deren.utility.shared(std::__1::basic_string_view<...>, std::__1::source_location)
C:/Users/23530/Desktop/yzk/deren/source/utility/shared/shared_utility.cpp:314:28
symbols frames=1..2
main
C:/Users/23530/Desktop/yzk/deren/source/tests/test_stacktrace.cpp:185:13
__tmainCRTStartup
??:0:0
```

`test_stacktrace` grew to **64 checks**: the raw record must still be there, `call stack:` must appear in whichever file the panic's channel wrote to (Release queues it to
`debug.log`, Debug writes it to stderr), and - under the symbolizer guard - real `symbols
frames=` output must appear too.

Bounds, so this cannot become the hang that path must not be: the symbolization is the same
bounded child process as any healthy-context call (1 s deadline by default, output cap, a
missing tool becomes `unavailable`, the child is killed with its job on cleanup), it runs
AFTER the raw record is out, and `fast_panic` - the OOM path - never touches it. The block is
itself a raw record plus `symbols frames=` sections, so a saved `debug.log` stays decodable
offline.

## Environment notes for reproducing this run

- Under the workspace-write sandbox, ninja and ctest cannot run: every compiler is
  spawned with piped stdio, which the sandbox refuses (reproduced independently with
  `node -e "require('child_process').execSync(...)"` -> `EPERM`, while PowerShell's own
  pipeline works). The build and the suites were run with full access.
- ninja's CMake regeneration edges live in a `pool = console` edge, which does not
  complete without a console here, so `CMAKE_SUPPRESS_REGENERATION=ON` was set for the
  two build directories while building and restored to `OFF` afterwards. Both
  directories are back to the normal configuration (`RERUN_CMAKE` edges present).
- A full-tree ASAN build is not possible in this checkout: `deren_vulkan.dll` fails to
  link with undefined `__asan_*`/`__ubsan_*` symbols. That gap is pre-existing, which is
  why the previous batch built and ran only the two ASAN test targets; this batch
  mirrors that scope.
