using System.Runtime.InteropServices;

namespace deren.scripting {
    /// <summary>
    /// The panel widget table: the flat mirror of deren::scripting::ui_api (ABI 31).
    ///
    /// The managed side describes the debug panel one verb at a time; the host turns each call into
    /// the overlay plugin's widgets. Every entry is optional - a host without a GUI plugin still
    /// publishes the table (its entries then answer module_unavailable), so a game can tell
    /// "no panel this session" from "no panel API at all".
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct ui_api_layout {
        public delegate* unmanaged<utf8_view, float, float, script_status> begin_panel;
        public delegate* unmanaged<script_status> end_panel;
        public delegate* unmanaged<utf8_view, script_status> text;
        public delegate* unmanaged<utf8_view, byte*, script_status> checkbox;
        public delegate* unmanaged<utf8_view, float*, float, float, script_status> slider;
        public delegate* unmanaged<utf8_view, float*, float, script_status> vec3;
        public delegate* unmanaged<utf8_view, utf8_view*, uint, int*, script_status> combo;
        public delegate* unmanaged<utf8_view, byte*, script_status> button;
        public delegate* unmanaged<byte*, script_status> wants_mouse;

        public readonly bool is_valid() {
            return begin_panel != null && end_panel != null && text != null && checkbox != null &&
                   slider != null && vec3 != null && combo != null && button != null && wants_mouse != null;
        }
    }

    /// <summary>
    /// The resolved panel callbacks. Same shape as scene_api: one layout, validated once at
    /// initialization, then every wrapper checks <see cref="is_ready"/> before calling.
    /// </summary>
    internal static unsafe class ui_api {
        internal static delegate* unmanaged<utf8_view, float, float, script_status> begin_panel;
        internal static delegate* unmanaged<script_status> end_panel;
        internal static delegate* unmanaged<utf8_view, script_status> text;
        internal static delegate* unmanaged<utf8_view, byte*, script_status> checkbox;
        internal static delegate* unmanaged<utf8_view, float*, float, float, script_status> slider;
        internal static delegate* unmanaged<utf8_view, float*, float, script_status> vec3;
        internal static delegate* unmanaged<utf8_view, utf8_view*, uint, int*, script_status> combo;
        internal static delegate* unmanaged<utf8_view, byte*, script_status> button;
        internal static delegate* unmanaged<byte*, script_status> wants_mouse;

        /// <summary>
        /// Whether the host resolved the panel table. Says nothing about whether a call may run now:
        /// a stopped host keeps the table resolved, and a non-blocking batch is refused below.
        /// </summary>
        internal static bool is_resolved() {
            return begin_panel != null && end_panel != null && text != null && checkbox != null &&
                   slider != null && vec3 != null && combo != null && button != null && wants_mouse != null;
        }

        /// <summary>
        /// Whether a call may reach the host on this thread: the table is resolved AND this thread is
        /// allowed to call. Same guard as the scene and audio tables - a non-blocking batch runs while
        /// the host records the next frame, and describing widgets into it would race that pass.
        /// </summary>
        internal static bool is_ready() {
            if (!host_call_guard.host_calls_allowed) {
                host_call_guard.note_refusal();
                return false;
            }
            return is_resolved();
        }

        internal static script_status initialize(native_api* api) {
            begin_panel = null;
            end_panel = null;
            text = null;
            checkbox = null;
            slider = null;
            vec3 = null;
            combo = null;
            button = null;
            wants_mouse = null;

            void* table = null;
            uint byte_size = 0;
            var status = api->get_module((uint)native_module.ui, &table, &byte_size);
            if (status != script_status.success) {
                return status;
            }
            if (table == null) {
                return script_status.module_invalid;
            }
            // ABI 33: same table-size check as the scene module (see scene_api.initialize).
            if (byte_size != sizeof(ui_api_layout)) {
                host_logger.write(api, 
                    $"Managed host: the ui module table is {byte_size} bytes, this build expects " +
                    $"{sizeof(ui_api_layout)}.");
                return script_status.module_invalid;
            }
            var resolved = *(ui_api_layout*)table;
            if (!resolved.is_valid()) {
                return script_status.module_invalid;
            }
            begin_panel = resolved.begin_panel;
            end_panel = resolved.end_panel;
            text = resolved.text;
            checkbox = resolved.checkbox;
            slider = resolved.slider;
            vec3 = resolved.vec3;
            combo = resolved.combo;
            button = resolved.button;
            wants_mouse = resolved.wants_mouse;
            return script_status.success;
        }

        internal static script_status assign_begin(utf8_view title, float default_width, float default_height) {
            return is_ready() ? begin_panel(title, default_width, default_height) : script_status.module_unavailable;
        }

        internal static script_status assign_end() {
            return is_ready() ? end_panel() : script_status.module_unavailable;
        }

        internal static script_status assign_text(utf8_view value) {
            return is_ready() ? text(value) : script_status.module_unavailable;
        }

        internal static script_status assign_checkbox(utf8_view label, byte* value) {
            return is_ready() ? checkbox(label, value) : script_status.module_unavailable;
        }

        internal static script_status assign_slider(utf8_view label, float* value, float minimum, float maximum) {
            return is_ready() ? slider(label, value, minimum, maximum) : script_status.module_unavailable;
        }

        internal static script_status assign_vec3(utf8_view label, float* value, float speed) {
            return is_ready() ? vec3(label, value, speed) : script_status.module_unavailable;
        }

        internal static script_status assign_combo(utf8_view label, utf8_view* items, uint count, int* selected) {
            return is_ready() ? combo(label, items, count, selected) : script_status.module_unavailable;
        }

        internal static script_status assign_button(utf8_view label, byte* pressed) {
            return is_ready() ? button(label, pressed) : script_status.module_unavailable;
        }

        internal static script_status read_wants_mouse(byte* value) {
            return is_ready() ? wants_mouse(value) : script_status.module_unavailable;
        }
    }
}
