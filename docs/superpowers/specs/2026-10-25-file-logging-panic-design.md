# File-only logging and panic design

Records the user's 2026-10-25 approval, amended explicitly to avoid stdout/stderr because ordinary
renderer launches have no console. Existing explicit utility print/stream APIs and command-line tools
are not logging routes and are outside this change. Status (2026-10-25): implemented and accepted with
follow-ups; see docs/reviews/2026-10-25-file-logging-panic.md.

All builds write ordinary asynchronous logs/errors to deren-debug.log, using the existing rotation policy.
Use existing deren-crash.log for direct panic/fast_panic/raw crash diagnostics on Windows and Linux.
No console fallback. Open emergency output at handler installation; before installation, normal-context
fatal calls may attempt one lazy OS-level open. POSIX signal handlers use preopened output only.
Report writes remain best effort; unwritable storage or stalled OS I/O cannot be made infallible.

Panic writes bounded plain message and source location directly before stack capture. It then writes
raw frames and best-effort readable symbols to the same emergency file. Reentry takes fast_panic.
It never constructs the asynchronous logger, and only gives an already-existing logger 250ms to
finish the previously published sequence (atomic polling, no logger mutex acquisition). This bounds
our log waiting, not symbolizer's existing budget or kernel I/O. Fast_panic remains plain bounded
text, no formatting/stack capture/symbolization/log wait, using the existing OS write path.

Normal wait_log_all captures the submitted sequence and waits for that prefix to finish write/flush;
newer messages do not extend the wait. Single FIFO worker makes a completed counter sufficient.
Closing admission does not falsely declare pending messages complete. Worker and late-exit rules stay.
No category system, configurable sink framework, ring buffer, batching/rotation redesign or GUI dialogs.
