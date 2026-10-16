# Linux / POSIX stack diagnostics

Scope: complete the existing utility's Linux backend, keeping the user's
"good enough" boundary. macOS was explicitly excluded by the user. No renderer
platform port, crash watchdog or module snapshot service was added.

Normal capture still uses fixed storage and `_Unwind_Backtrace`. Healthy-context
symbolization now uses an explicit `posix_spawn` executable/argument array,
filtered LLVM environment, bounded nonblocking output and deadline. The child has
its own process group; cleanup kills the group and reaps the direct child.
`waitid(WNOWAIT)` reserves the PID until that cleanup. This contains ordinary
descendants, not a tool deliberately escaping its group.

The Linux ELF module resolver now uses the load bias from program headers.
Non-PIE ELF has a valid zero bias; the offline decoder accepts it when a module
path is present. A regression first demonstrated the old non-PIE result was
`??:0:0`, then verified both online and offline function/line decoding.

`install_crash_handler()` installs SIGSEGV/SIGBUS/SIGILL/SIGFPE actions and prepares
the current thread's alternate signal stack. Existing worker entry points reuse
`prepare_crash_thread()` for a 64 KiB per-thread stack, preserving one supplied by
another library. The handler records the signal, original context PC and memory
fault address using fixed storage and write, then restores normal signal/core
termination semantics. SIGABRT is untouched: fast_panic remains raw text + abort.

Deliberate limit: signal handlers do not unwind or resolve modules. Their PC alone
is not an ASLR-independent offline trace. Full call chains are captured from a
normal context (including ordinary panic); a full faulting chain requires the OS
core dump/debugger. This follows the distinction between normal diagnostics and
the restricted operations available in a signal handler. See the
[POSIX signal safety list](https://man7.org/linux/man-pages/man7/signal-safety.7.html)
and [LLVM symbolizer](https://llvm.org/docs/CommandGuide/llvm-symbolizer.html).

Validation in `2026-10-15-posix-stacktrace/`:

- Ubuntu/WSL x86-64, GCC 16 and llvm-symbolizer 21; native sources built with O2,
  debug info, frame pointers, disabled sibling calls, no exceptions and Werror.
- `red.log`: 6 initial tests, 4 failures in the unimplemented POSIX branches.
- `non-pie-red.log`: symbolization failed to find the function before bias fix.
- `pie-tests.log`, `final-non-pie-tests.log`: 7/7 tests each; capture, actual source
  symbolization with hostile LLVM options removed, missing executable, offline
  decode, timeout/descendant cleanup, output cap, actual memory fault, worker stack
  overflow, and no signal/stack output from fast_panic. Some cases share one test.
- `windows-build.log`, `windows-tests.log`: full Windows build and 28/28 CTest.
- `final-windows-build.log`, `final-windows-tests.log`: final incremental build and
  focused diagnostic/offline/docs checks after removing the macOS branches.

Native tests are independent of the renderer's C++ module toolchain. The CMake
Linux test target and a small Linux CI job run the same probe. This validates the
native diagnostic service, not the complete Linux renderer. Linux ARM64 context
extraction is implemented but untested; other architectures record a zero PC.

Read-only review confirmed Linux process cleanup and signal boundaries, and
identified stale zero-bias documentation. macOS-specific findings were closed by
the user's explicit exclusion and removal of those branches. No commit or push.
