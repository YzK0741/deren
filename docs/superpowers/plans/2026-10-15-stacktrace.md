# Stacktrace and fast_panic

User approved continuing the assessed stacktrace integration and relaxed diagnostic compilation, and explicitly requested fast_panic for OOM/fatal errors. Execute inline in the existing dirty workspace, preserving all preceding work; no commit/push. Windows is the validated platform.

shared_utility owns the service. Public POD trace holds at most64 addresses; noexcept capture uses fixed storage. fast_panic writes bounded raw text directly to OS stderr then aborts, without stacktrace, source_location or format-string overloads, without project heap allocation, logger/GPU waits or symbolizer processes. OOM uses the same native terminal entry. Ordinary panic retains logging/flush and gains raw evidence before these operations. Windows crash handler writes exception code/address/context first and unwinds from ContextRecord, with minimal stack-overflow fallback. OS unwinding/module queries remain best effort.

Healthy-context symbolization uses vessel storage, explicit configured llvm-symbolizer, total timeout/output bounds and restricted child handles. Failure preserves module/RVA evidence. An offline script decodes the same raw records. Keep matching EXE/DLL/debug data. Keep rich diagnostics outside the minimal fatal path.

VR_DIAGNOSTICS defaults ON: optimized Release uses O2/debug info, unwind tables/frame pointers, no sibling-call optimization or LTO. OFF retains prior lean Release knobs. No exceptions enabled.

- [x] RED tests for capture/fast_panic/symbolization API.
- [x] Native capture/output and public API; fatal/OOM/panic integration.
- [x] Context-aware Windows crash handler and app registration.
- [x] Bounded symbolizer/offline tool, DLL/process/crash tests.
- [x] Diagnostic compiler flags; optimized symbols/lines, depth/skip, timeout/output cap and OOM tests.
- [x] Full validation, review and usage/evidence documentation.

Critical checks: no project allocation in capture/fast_panic, EXE -> DLL -> callback frames, missing tool/symbols, hung/oversized child, fault address equal to first context frame. POSIX signal handlers are outside this Windows-first batch. Compile-only portable fallbacks are not claimed tested.

User correction supersedes initial fast_panic design: bounded raw text + abort only; no stacktrace, source_location or format-string overload. OOM uses this minimal native entry. Ordinary panic/explicit diagnostics retain stacks. Regression observed RED: two unexpected origin/stack outputs before removing them.
