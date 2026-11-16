using System;

namespace deren.scripting {
    /// <summary>
    /// Tells the API wrappers whether the calling thread may reach the host.
    ///
    /// A blocking frame-event batch runs while the render loop is parked on the boundary wait, so a
    /// scene call from it happens in the same window as one from update. A non-blocking batch runs
    /// on its own thread while the host records the next frame, and no lock covers the runtime state
    /// those calls touch. Rather than let a call race the record pass, the wrappers return
    /// `module_unavailable` on that thread and this guard counts the refusals for one log line, so a
    /// task that reaches too far is visible instead of producing a corrupted frame.
    /// </summary>
    internal static class host_call_guard {
        [ThreadStatic] private static bool nonblocking_thread;
        [ThreadStatic] private static int refused_calls;

        /// <summary>False only on the non-blocking executor's thread.</summary>
        internal static bool host_calls_allowed => !nonblocking_thread;

        /// <summary>Marks the calling thread as the non-blocking executor's.</summary>
        internal static void enter_nonblocking_thread() {
            nonblocking_thread = true;
        }

        /// <summary>Starts the refusal count for one batch.</summary>
        internal static void begin_batch() {
            refused_calls = 0;
        }

        /// <summary>Counts a call the wrappers refused, and reports the refusal to the caller.</summary>
        internal static void note_refusal() {
            ++refused_calls;
        }

        /// <summary>Ends the batch and reports how many calls it lost.</summary>
        internal static int end_batch() {
            var count = refused_calls;
            refused_calls = 0;
            return count;
        }
    }
}
