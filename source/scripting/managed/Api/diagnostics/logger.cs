using System;
using System.Runtime.CompilerServices;
using System.Text;
using System.Threading;

namespace deren.scripting {
    /// <summary>
    /// The managed side's one way to say something to the host.
    ///
    /// THERE IS NO THROWING FORM AND NO SWALLOWING FORM ANY MORE: <see cref="write"/> answers
    /// whether the host took the line. A caller that cannot continue without its log can react to
    /// `false`; a caller on a thread that must survive - the frame worker, a shutdown path - can
    /// ignore the answer without an exception escaping. The old `nothrow_log` wrapper existed only
    /// to swallow the failure the ordinary form raised, so the distinction is a bool now.
    /// </summary>
    public static class logger {
        /// <summary>
        /// Hands one line to the host. Returns false when it could not be handed over: no table
        /// yet, a host that publishes no logger, or a message that cannot be encoded. NEVER throws,
        /// because one caller is the shutdown path, which still has to report something after the
        /// host tore its logger down.
        /// </summary>
        public static bool write(string message) {
            return write(managed_bootstrap.core, message);
        }

        /// <summary>
        /// Writes through a table the caller already holds. The bootstrap uses this before it has
        /// stored the table, and so does the module handshake that resolves it.
        /// </summary>
        internal static unsafe bool write(native_api* api, string message) {
            if (api == null) {
                return false;
            }
            return write(*api, message);
        }

        internal static unsafe bool write(native_api api, string message) {
            if (api.log == null) {
                // A host that publishes no logger is legal: there is nothing to report against.
                return false;
            }
            try {
                api.write_log(Encoding.UTF8.GetBytes(message));
                return true;
            } catch (Exception) {
                // A message that cannot be encoded, or a host that fails mid-call: the answer is
                // the same either way, and throwing here would punish the caller for logging.
                return false;
            }
        }

        // Set by the first panic, so a second fault raised while the first report is being written
        // (the unhandled-exception hook firing during an explicit call) does not overwrite the story.
        private static int reported;

        /// <summary>
        /// Ends the process with one report, the managed counterpart of deren::utility::panic.
        ///
        /// The host writes the header this call supplies, then the native raw stack and best-effort
        /// symbols, then terminates - so a reader sees the managed file/line and call stack above
        /// the frames that reached the host, and does not have to know which language faulted. When
        /// no host answers (an older host, a torn-down table, a failed hand-over) this writes the
        /// same report to stderr and fails fast, because a panic that returns is not a panic.
        /// </summary>
        public static void panic(string message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0) {
            panic(managed_bootstrap.core, message, file, line, capture_stack());
        }

        /// <summary>
        /// The form the unhandled-exception hook uses: the exception's own text is a better managed
        /// stack than the frames that only noticed it.
        /// </summary>
        internal static unsafe void panic(native_api api, string message, string file, int line, string managed_stack) {
            if (Interlocked.Exchange(ref reported, 1) != 0) {
                return;
            }
            if (api.panic != null) {
                try {
                    byte[] message_bytes = Encoding.UTF8.GetBytes(message ?? string.Empty);
                    byte[] file_bytes = Encoding.UTF8.GetBytes(file ?? string.Empty);
                    byte[] stack_bytes = Encoding.UTF8.GetBytes(managed_stack ?? string.Empty);
                    fixed (byte* message_data = message_bytes)
                    fixed (byte* file_data = file_bytes)
                    fixed (byte* stack_data = stack_bytes) {
                        var message_view = new utf8_view { data = message_data, length = (uint)message_bytes.Length };
                        var file_view = new utf8_view { data = file_data, length = (uint)file_bytes.Length };
                        var stack_view = new utf8_view { data = stack_data, length = (uint)stack_bytes.Length };
                        // The host terminates inside this call. `success` therefore means a host that
                        // recorded the report and returned, which only a test host does.
                        if (api.panic(message_view, file_view, line < 0 ? 0u : (uint)line, stack_view) ==
                            script_status.success) {
                            return;
                        }
                    }
                } catch (Exception) {
                    // Fall through: failing to hand the report over must not fail the report.
                }
            }
            string fallback = $"deren panic (managed): {message}\nsource: {file}:{line} in managed code\n" +
                              $"managed call stack:\n{managed_stack}\n";
            try {
                Console.Error.Write(fallback);
                Console.Error.Flush();
            } catch (Exception) {
            }
            Environment.FailFast(fallback);
        }

        private static string capture_stack() {
            try {
                // File information is what makes the managed frames point at sources instead of just
                // leaving method names in the report.
                return new System.Diagnostics.StackTrace(fNeedFileInfo: true).ToString();
            } catch (Exception) {
                return string.Empty;
            }
        }
    }
}
