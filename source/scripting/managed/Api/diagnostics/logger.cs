using System;
using System.Text;

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
    }
}
