using deren.scripting;

namespace deren {
    /// <summary>
    /// The application window, for a project that draws its own caption (`[render]
    /// borderless_window = true`). A borderless window has no system title bar, so the managed UI owns
    /// the minimise / maximise / close buttons and the drag area; these calls forward those actions to
    /// the app's window instead of letting the UI fake them.
    ///
    /// Every call is a capability query as much as an action: an offscreen capture or a host that
    /// predates ABI 43 answers false, so the UI can hide or ignore its caption buttons instead of
    /// assuming the window exists. The functions never touch scene or frame data.
    /// </summary>
    public static unsafe class window_control {
        /// <summary>Hides the window the way the system minimise button does.</summary>
        public static bool try_minimize() {
            return try_minimize(out _);
        }

        public static bool try_minimize(out script_status status) {
            status = scene_api.request_window_minimize();
            return status == script_status.success;
        }

        /// <summary>Maximises a restored window and restores a maximised one.</summary>
        public static bool try_toggle_maximize() {
            return try_toggle_maximize(out _);
        }

        public static bool try_toggle_maximize(out script_status status) {
            status = scene_api.request_window_toggle_maximize();
            return status == script_status.success;
        }

        /// <summary>
        /// Whether the window is maximised. Returns false when the host cannot answer (no window), not
        /// when the window is merely restored - check <paramref name="maximized"/> only on true.
        /// </summary>
        public static bool try_is_maximized(out bool maximized) {
            return try_is_maximized(out maximized, out _);
        }

        public static bool try_is_maximized(out bool maximized, out script_status status) {
            maximized = false;
            byte value = 0;
            status = scene_api.query_window_is_maximized(&value);
            if (status != script_status.success) {
                return false;
            }
            maximized = value != 0;
            return true;
        }

        /// <summary>Requests the same close the system close button asks for; the app still decides.</summary>
        public static bool try_close() {
            return try_close(out _);
        }

        public static bool try_close(out script_status status) {
            status = scene_api.request_window_close();
            return status == script_status.success;
        }

        /// <summary>
        /// Starts an interactive move from a caption-area press. The call BLOCKS until the pointer
        /// button is released, because the system owns the move loop; the frame rendered before the
        /// press stays on screen for the duration. Returns false when the platform has no such loop.
        /// </summary>
        public static bool try_begin_drag() {
            return try_begin_drag(out _);
        }

        public static bool try_begin_drag(out script_status status) {
            status = scene_api.request_window_begin_drag();
            return status == script_status.success;
        }
    }
}
