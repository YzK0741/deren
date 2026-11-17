using System;
using deren.scripting;

namespace deren {
    /// <summary>
    /// One-shot screenshot requests from managed code.
    /// </summary>
    public static unsafe class screen_snapshot {
        private static native_api api;

        internal static void bind(native_api* value) {
            api = *value;
        }

        /// <summary>
        /// Requests a screenshot of the next rendered frame. The native side saves it under
        /// [paths] screenshot_dir with a timestamped name. Returns false when the host does not
        /// expose screenshots or no scene is active.
        /// </summary>
        public static bool try_request() {
            return try_request(out _);
        }

        public static bool try_request(out script_status status) {
            var request = api.request_screenshot;
            if (request == null) {
                status = script_status.module_unavailable;
                return false;
            }
            try {
                status = request();
                return status == script_status.success;
            } catch (Exception error) {
                status = script_status.script_failure;
                host_logger.write(
                    $"Game: screenshot request failed: {error.GetBaseException().Message}");
                return false;
            }
        }
    }
}
