using System;
using System.Runtime.InteropServices;
using System.Text;
using deren.scripting;

namespace deren {
    /// <summary>
    /// One control's value, held in unmanaged memory so the widget that binds it can keep the pointer
    /// for the whole frame.
    ///
    /// THE ALLOCATION IS THE POINT: a panel is rebuilt every frame, but the overlay draws it AFTER the
    /// game's update returns - a widget bound to a stack local or a GC field would be read after that
    /// storage had gone. One sixteen-byte cell per control, owned by the game, freed on Dispose.
    /// </summary>
    public sealed unsafe class ui_value : IDisposable {
        private const int cell_size = 16;
        private byte* cell;

        public ui_value() {
            cell = (byte*)Marshal.AllocHGlobal(cell_size);
            new Span<byte>(cell, cell_size).Clear();
        }

        public ui_value(bool value) : this() {
            flag = value;
        }

        public ui_value(float value) : this() {
            number = value;
        }

        public ui_value(float x, float y, float z) : this() {
            vector = new vector_3 { x = x, y = y, z = z };
        }

        public ui_value(int selected) : this() {
            this.selected = selected;
        }

        ~ui_value() {
            Dispose();
        }

        /// <summary>A checkbox's state (one byte, 0 or 1 - the value the plugin's widget binds).</summary>
        public bool flag {
            get => cell != null && *cell != 0;
            set {
                if (cell != null) {
                    *cell = value ? (byte)1 : (byte)0;
                }
            }
        }

        /// <summary>A slider's value, or the single number a control edits.</summary>
        public float number {
            get => cell == null ? 0.0f : *(float*)cell;
            set {
                if (cell != null) {
                    *(float*)cell = value;
                }
            }
        }

        /// <summary>A vec3 control's three components, in the cell's three leading floats.</summary>
        public vector_3 vector {
            get => cell == null ? default : new vector_3 { x = *(float*)cell, y = *(float*)(cell + 4), z = *(float*)(cell + 8) };
            set {
                if (cell == null) {
                    return;
                }
                *(float*)cell = value.x;
                *(float*)(cell + 4) = value.y;
                *(float*)(cell + 8) = value.z;
            }
        }

        /// <summary>A combo's selected index.</summary>
        public int selected {
            get => cell == null ? 0 : *(int*)cell;
            set {
                if (cell != null) {
                    *(int*)cell = value;
                }
            }
        }

        internal byte* address => cell;

        /// <summary>Zeros the cell: what a one-shot control (a button's press) does after it is read.</summary>
        public void clear() {
            if (cell != null) {
                new Span<byte>(cell, cell_size).Clear();
            }
        }

        public void Dispose() {
            if (cell != null) {
                Marshal.FreeHGlobal((IntPtr)cell);
                cell = null;
            }
            GC.SuppressFinalize(this);
        }
    }

    /// <summary>
    /// The debug panel, as the HOST draws it: a game describes the widgets and the native overlay
    /// renders them into the window. There is no button-free limitation here - a button reports its
    /// press through the value cell the game passed in, and a press arrives on the NEXT update.
    ///
    /// THE WIDGET LIST IS PER FRAME: call begin_panel() once, describe every control, then end_panel().
    /// The panel object itself lives in the overlay and is reused (the host keeps its registration),
    /// so a game must not assume its controls survive a frame.
    ///
    /// A session without the GUI plugin keeps the API published and every call returns false, which is
    /// why `available` exists: a game that must know can ask once instead of testing each call.
    /// </summary>
    public static unsafe class ui {
        /// <summary>Whether the host resolved the panel table (the plugin may still be off).</summary>
        public static bool available => ui_api.is_ready();

        public static bool begin_panel(string title) {
            return begin_panel(title, 0.0f, 0.0f);
        }

        public static bool begin_panel(string title, float default_width, float default_height) {
            return begin_panel(title, default_width, default_height, out _);
        }

        public static bool begin_panel(string title, float default_width, float default_height,
                                       out script_status status) {
            if (string.IsNullOrEmpty(title)) {
                status = script_status.invalid_argument;
                return false;
            }
            using var text = native_string.create(title);
            status = ui_api.assign_begin(text.view, default_width, default_height);
            return status == script_status.success;
        }

        public static bool end_panel() {
            return end_panel(out _);
        }

        public static bool end_panel(out script_status status) {
            status = ui_api.assign_end();
            return status == script_status.success;
        }

        public static bool text(string value) {
            return text(value, out _);
        }

        public static bool text(string value, out script_status status) {
            using var encoded = native_string.create(value);
            status = ui_api.assign_text(encoded.view);
            return status == script_status.success;
        }

        public static bool checkbox(string label, ui_value value) {
            return checkbox(label, value, out _);
        }

        public static bool checkbox(string label, ui_value value, out script_status status) {
            if (string.IsNullOrEmpty(label) || value == null) {
                status = script_status.invalid_argument;
                return false;
            }
            using var text = native_string.create(label);
            status = ui_api.assign_checkbox(text.view, value.address);
            return status == script_status.success;
        }

        public static bool slider(string label, ui_value value, float minimum, float maximum) {
            return slider(label, value, minimum, maximum, out _);
        }

        public static bool slider(string label, ui_value value, float minimum, float maximum,
                                  out script_status status) {
            if (string.IsNullOrEmpty(label) || value == null) {
                status = script_status.invalid_argument;
                return false;
            }
            using var text = native_string.create(label);
            status = ui_api.assign_slider(text.view, (float*)value.address, minimum, maximum);
            return status == script_status.success;
        }

        public static bool vec3(string label, ui_value value, float speed = 0.05f) {
            return vec3(label, value, speed, out _);
        }

        public static bool vec3(string label, ui_value value, float speed, out script_status status) {
            if (string.IsNullOrEmpty(label) || value == null) {
                status = script_status.invalid_argument;
                return false;
            }
            using var text = native_string.create(label);
            status = ui_api.assign_vec3(text.view, (float*)value.address, speed);
            return status == script_status.success;
        }

        public static bool combo(string label, string[] items, ui_value value) {
            return combo(label, items, value, out _);
        }

        public static bool combo(string label, string[] items, ui_value value, out script_status status) {
            if (string.IsNullOrEmpty(label) || value == null || items == null || items.Length == 0) {
                status = script_status.invalid_argument;
                return false;
            }
            // THE ITEM NAMES LIVE IN ONE BUFFER the call pins: one utf8_view per item points into it, so
            // the host sees contiguous strings and nothing outlives the call.
            var total = 0;
            for (var index = 0; index < items.Length; ++index) {
                total += Encoding.UTF8.GetByteCount(items[index] ?? string.Empty);
            }
            var buffer = new byte[total];
            var views = new utf8_view[items.Length];
            var cursor = 0;
            fixed (byte* buffer_data = buffer)
            fixed (utf8_view* view_data = views) {
                for (var index = 0; index < items.Length; ++index) {
                    var item = items[index] ?? string.Empty;
                    var written = Encoding.UTF8.GetBytes(item, 0, item.Length, buffer, cursor);
                    views[index] = new utf8_view { data = buffer_data + cursor, length = (uint)written };
                    cursor += written;
                }
                using var text = native_string.create(label);
                status = ui_api.assign_combo(text.view, view_data, (uint)items.Length, (int*)value.address);
                return status == script_status.success;
            }
        }

        /// <summary>
        /// A button. The press is written into @p value as one byte during the overlay's draw and is
        /// therefore readable on the NEXT update - read it with <see cref="ui_value.flag"/>, then
        /// <see cref="ui_value.clear"/> it, exactly like a one-shot key press.
        /// </summary>
        public static bool button(string label, ui_value value) {
            return button(label, value, out _);
        }

        public static bool button(string label, ui_value value, out script_status status) {
            if (string.IsNullOrEmpty(label) || value == null) {
                status = script_status.invalid_argument;
                return false;
            }
            using var text = native_string.create(label);
            status = ui_api.assign_button(text.view, value.address);
            return status == script_status.success;
        }

        /// <summary>Whether the pointer is over the panel: a game with its own mouse-look should idle it.</summary>
        public static bool wants_mouse() {
            return wants_mouse(out _);
        }

        public static bool wants_mouse(out script_status status) {
            byte wants = 0;
            status = ui_api.read_wants_mouse(&wants);
            return status == script_status.success && wants != 0;
        }
    }
}
