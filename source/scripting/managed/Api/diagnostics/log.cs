using System.Text;

namespace deren.scripting {
    /// <summary>
    /// Writes a line to the host log. This is the ordinary form: it lets a failure through, because
    /// a caller that cannot say something should hear about that instead of losing the message
    /// silently. <see cref="nothrow_log"/> shares this path and is the form to use on threads that
    /// must survive a failing log call.
    /// </summary>
    public static class log {
        /// <summary>Writes through the table the host published at startup.</summary>
        public static void write(string message) {
            write(managed_bootstrap.core, message);
        }

        /// <summary>
        /// Writes through a table the caller already holds. The bootstrap uses this before it has
        /// stored the table, and so does the module handshake that resolves it.
        /// </summary>
        internal static unsafe void write(native_api* api, string message) {
            if (api == null) {
                return;
            }
            write(*api, message);
        }

        internal static unsafe void write(native_api api, string message) {
            if (api.log == null) {
                // A host that publishes no logger is legal: there is nothing to fail against.
                return;
            }
            api.write_log(Encoding.UTF8.GetBytes(message));
        }
    }
}
