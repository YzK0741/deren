using System;

namespace deren.scripting {
    /// <summary>
    /// <see cref="log"/> for threads that must not die. A log call crosses the native boundary, so a
    /// null table, a message that cannot be encoded, or a host that already tore its logger down
    /// must not turn a caught exception into a dead thread or an aborted shutdown. The second attempt
    /// exists on purpose: a failure to report a failure still deserves one more try.
    /// </summary>
    public static class nothrow_log {
        public static void write(string message) {
            try {
                log.write(message);
            } catch (Exception error) {
                try {
                    log.write($"Managed host: a log message could not be written: {error.GetBaseException().Message}");
                } catch (Exception) {
                    // Nowhere left to report it, and propagating is exactly what this exists to
                    // prevent: the caller is usually already handling someone else's failure.
                }
            }
        }
    }
}
