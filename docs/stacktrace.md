# Stack diagnostics

`deren.utility.shared` owns the diagnostic service, including across plugins.

```cpp
import deren.utility.shared;
namespace diag = deren::utility::diagnostics;
auto trace = diag::capture_stack(0, 32);
diag::write_stack(trace);
auto readable = diag::symbolize(trace); // Healthy context: allocates and starts a tool.
```

Capture uses fixed storage for at most 64 addresses. Depth zero returns an empty
trace; skip excludes additional caller frames. Raw records retain PC, module base,
object address and module path. Windows records use a module base and RVA;
POSIX records use the load bias and object virtual address. A resolved non-PIE
ELF can have `base=0x0`; an empty module path identifies an unresolved frame. Symbolization keeps
these records when symbols or the tool are unavailable, and a tool that writes
nothing is `unavailable` rather than `ok`. Its timeout applies across module groups;
output is bounded. `symbolizer_options::executable = "off"` selects raw output only.

A captured frame holds a RETURN address, so it is decoded at `pc - 1`. A fault PC is
EXACT - the crash handler's first frame is the exception address itself - and moving
it one byte back decodes the instruction before the fault. `stack_trace::exact_address_count`
names the leading addresses that are exact: those are symbolized unchanged, every
later frame at `pc - 1`. `capture_stack` returns return addresses, so it leaves the
count at zero; a trace rebuilt from a crash record sets it to one.

## Fatal errors

```cpp
deren::utility::fast_panic("allocation failed");
```

`fast_panic(std::string_view)` is noexcept and accepts only raw text. It writes a
fixed prefix, at most 4096 message bytes and a newline directly to OS stderr,
then aborts. There is no format overload, source location, stack capture,
symbolization, project heap allocation or logger flush in this path. Provide a
valid string view. Output is best effort; OS writes and abort handlers can still
block. The allocator uses this entry and supplies its bounded byte/alignment
diagnostic assembled in fixed storage.

Ordinary `panic` captures raw frames first - those are what the offline decoder reads - and
then symbolizes the SAME frames and prints them through its logging path, so a panic report
carries a readable stack next to its message and location. The symbolization is best effort
and bounded like any healthy-context call (deadline, output cap): a missing tool, missing
symbols or a timeout simply leaves the raw records, which are already written. `fast_panic` -
OOM, a broken heap - never symbolizes at all.

The sink behind that flush is never destroyed: the object outlives the process and
its final drain is registered at first use, so a `panic()` - or a plain `log()` -
from a static destructor ordered after it returns and drops its lines instead of
touching a dead mutex, and a `panic()` ON the log worker does not wait for the
queue that thread is itself draining. The queue is still drained before the process
ends.

## Windows crash handling

The application calls `install_crash_handler()` during startup. If stderr is
absent, installation preopens `deren-crash.log` in the working directory; fatal
calls then use the cached handle. A fatal that arrives BEFORE installation - an
allocation failure during static initialization, in a console-less launch - opens
that same file itself, once per process: that is the fatal path's one deliberate
file open, bounded to a single `CreateFileW` on a relative path, and it costs
nothing once startup has preopened the handle. The handler records the
exception code, the fault PC and - for an access violation - the access kind and
the data address, before best effort unwinding from the actual x64 exception
context. Stack overflow writes only the fault PC and an unwind-skipped marker.
Corrupted stacks or loader state can prevent further output - but the module lookup is not
hostage to the loader lock: MEASURED with a DLL whose `DllMain` holds that lock while
another thread faults, the record still comes out complete, and a fault raised INSIDE
`DllMain` never reaches the handler at all (the loader turns it into a failed load).

Overflow reserve is per thread. The project thread pool, logging worker and Gaussian chunk workers call
`prepare_crash_thread()` at entry. External workers should do the same. This does
not install or replace the process exception filter. Other libraries may replace
that filter; ASAN owns native fault reporting in sanitizer builds, where the
native-fault assertions are skipped. They run in the optimized Release suite.
Windows Clang is validated; POSIX behavior is described below.

## POSIX diagnostics

Normal capture uses `_Unwind_Backtrace` with fixed address storage. In a healthy
context, `dladdr` locates the module, ELF program headers supply the load bias,
and `llvm-symbolizer` decodes object addresses.
The native launcher uses `posix_spawn` with an explicit executable and argument
array, filtered environment, nonblocking pipe, timeout and output cap. A separate
process group is killed and the direct child reaped on completion or failure.
This contains ordinary descendants; it is not a sandbox for a tool that deliberately
escapes its process group.

`install_crash_handler()` installs `sigaction` handlers for SIGSEGV, SIGBUS, SIGILL
and SIGFPE. `prepare_crash_thread()` supplies a 64 KiB alternate signal stack for
the calling thread and preserves an existing alternate stack. Existing project
worker entry points already call it. SIGABRT remains untouched, including when
`fast_panic` aborts.

The signal handler writes only the signal number, context PC and (for memory
faults) data address to stderr, then re-raises the signal with its default action.
It performs no unwinding, module lookup, formatting-library call, allocation,
logging or symbolization. The PC extraction covers Linux x86-64 and ARM64;
other architectures report zero rather than a fabricated PC. A complete crash
chain depends on the OS core dump/debugger; the signal record alone is not an
ASLR-independent offline trace. Full call chains are available from normal
capture and ordinary panic. Missing stderr may lose the signal record.

Linux x86-64 was tested in Ubuntu/WSL using optimized PIE and non-PIE probes,
including actual memory faults and a worker stack overflow. macOS is outside this
integration. These native probes do not claim a complete Linux renderer build.
A small independent Linux CI job runs the same tests.

## Builds and offline decoding

`VR_DIAGNOSTICS=ON` is the default. Release keeps optimization at O2, debug data,
unwind tables and frame pointers, disables sibling-call optimization and omits
LTO and executable stripping. Exceptions remain disabled. OFF retains the lean
Release options; useful stacks and line information are not guaranteed there.

CMake locates `llvm-symbolizer` beside the compiler or on its configure-time
search path. Healthy symbolization uses that explicit executable, no shell,
restricted inherited handles, a filtered environment and a kill-on-close Windows
job. The environment filter is the same rule the offline tool applies: a variable
that rewrites the tool's output protocol (`LLVM_SYMBOLIZER_OPTS`) never reaches the
child, so the text is always read as the one protocol this service decodes. There is
no tool invocation inside fast_panic or the OS crash/signal handlers.

Archive the exact EXE, DLL and debug data with each crash log. An offline tool
can decode the raw records:

```text
python scripts/symbolize_stacktrace.py crash.log --module-dir archived-build
```

The script applies the same rule this service does - the first frame after each
`deren crash` line is exact, every later frame is a return address - and prints every
raw line unchanged. Missing modules/symbols can produce partial results and a nonzero
exit code; a module the archive does not hold is reported as
`warning: missing module file <path>` on stderr, once per distinct file, rather than
being handed to a child that can only fail on it. `--budget` bounds the whole decode
across frames (default 30 seconds) and `--timeout` bounds one tool call (10 seconds);
each call's output is capped and its process tree is cleaned up. `LLVM_SYMBOLIZER_OPTS` is removed from the offline
child's environment, and the log is read line by line, so decoding never loads the
whole file into memory.
