using System;
using System.Text;
using deren.scripting;

namespace deren {
    /// <summary>
    /// One-shot screenshot requests from managed code.
    /// </summary>
    public static unsafe class screenshot {
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
            var request = api.request_screenshot;
            if (request == null) {
                return false;
            }
            try {
                return request() == script_status.success;
            } catch (Exception error) {
                managed_bootstrap.core.write_log(Encoding.UTF8.GetBytes(
                    $"Game: screenshot request failed: {error.GetBaseException().Message}"));
                return false;
            }
        }
    }
}
