using System;
using System.Text;
using System.Threading;
using deren.scripting;

namespace deren {
    public enum rml_event_type : uint { click = 1, change = 2, pointer_down = 3, pointer_move = 4, pointer_up = 5 }
    /// <summary>
    /// One UI event. <c>x</c>/<c>y</c>, <c>button</c> and <c>modifiers</c> are ABI 42 pointer-event
    /// payload, in density-independent pixels; click and change leave them at zero.
    /// </summary>
    public readonly record struct rml_event(rml_event_type type, string id, string value,
                                            float x = 0.0f, float y = 0.0f, int button = 0, uint modifiers = 0);
    /// <summary>ABI 42: one element's border box in density-independent pixels.</summary>
    public readonly record struct rml_rect(float x, float y, float width, float height);

    /// <summary>A render-thread document owned by this script session. Close it with Dispose.</summary>
    public sealed class rml_document : IDisposable {
        private ulong value;
        private readonly int epoch;
        private rml_document(ulong value) {
            this.value = value;
            epoch = rml_ui_api.epoch;
        }
        private ulong handle => Volatile.Read(ref value);
        public bool is_valid => handle != 0 && rml_ui_api.is_current(epoch);
        public static bool is_available => rml_ui_api.is_ready();

        public static bool try_open(string markup, string font, out rml_document? document, string? fallback_font = null) =>
            try_open(markup, font, out document, out _, fallback_font);
        public static unsafe bool try_open(string markup, string font, out rml_document? document,
                                           out script_status status, string? fallback_font = null) {
            document = null;
            if (!rml_ui_api.is_ready()) {
                status = script_status.module_unavailable;
                return false;
            }
            // Project-relative paths follow the same rule as the audio API.
            if (!audio_api.try_path(markup, out var markup_path, out status)) {
                return false;
            }
            using (markup_path) {
                if (!audio_api.try_path(font, out var font_path, out status)) {
                    return false;
                }
                using (font_path) {
                    native_string fallback_path = default;
                    if (fallback_font != null && !audio_api.try_path(fallback_font, out fallback_path, out status)) {
                        return false;
                    }
                    using (fallback_path) {
                        ulong value = 0;
                        status = rml_ui_api.table.open(markup_path.view, font_path.view, fallback_path.view, &value);
                        if (status != script_status.success || value == 0) {
                            return false;
                        }
                        document = new rml_document(value);
                        return true;
                    }
                }
            }
        }
        private bool available(out script_status status) {
            status = !rml_ui_api.is_ready() ? script_status.module_unavailable :
                !is_valid ? script_status.invalid_node : script_status.success;
            return status == script_status.success;
        }
        private static bool valid_text(string? text, bool allow_empty = true) => text != null &&
            (allow_empty || text.Length != 0) && text.Length <= 1024 * 1024 && !text.Contains('\0');
        public bool set_text(string id, string text) => set_text(id, text, out _);
        public bool set_value(string id, string text) => set_value(id, text, out _);
        public bool set_text(string id, string text, out script_status status) => set_content(id, text, false, out status);
        public bool set_value(string id, string text, out script_status status) => set_content(id, text, true, out status);
        private unsafe bool set_content(string id, string text, bool input, out script_status status) {
            if (!available(out status)) { return false; }
            if (!valid_text(id, false) || !valid_text(text)) {
                status = script_status.invalid_argument;
                return false;
            }
            using var element = native_string.create(id);
            using var content = native_string.create(text);
            status = input ? rml_ui_api.table.set_value(handle, element.view, content.view) :
                             rml_ui_api.table.set_text(handle, element.view, content.view);
            return status == script_status.success;
        }
        public bool set_visible(string id, bool visible) => set_visible(id, visible, out _);
        public unsafe bool set_visible(string id, bool visible, out script_status status) {
            if (!available(out status)) { return false; }
            if (!valid_text(id, false)) {
                status = script_status.invalid_argument;
                return false;
            }
            using var element = native_string.create(id);
            status = rml_ui_api.table.set_visible(handle, element.view, visible ? (byte)1 : (byte)0);
            return status == script_status.success;
        }
        /// <summary>
        /// ABI 42: the element's border box in density-independent pixels. A null or empty id is the
        /// document root, i.e. the viewport, which is how proportions are derived without naming an
        /// element. The rect describes the last layout pass, so a style written here - or by
        /// <see cref="try_set_element_style(string?, string?, string?)"/> - becomes visible after the
        /// host paints again.
        /// </summary>
        public bool try_get_element_rect(string? id, out rml_rect rect) => try_get_element_rect(id, out rect, out _);
        public unsafe bool try_get_element_rect(string? id, out rml_rect rect, out script_status status) {
            rect = default;
            if (!available(out status)) { return false; }
            if (rml_ui_api.table.get_element_rect == null) {
                status = script_status.module_unavailable;
                return false;
            }
            if (id != null && !valid_text(id)) {
                status = script_status.invalid_argument;
                return false;
            }
            using var element = native_string.create(id);
            element_rect_layout result = default;
            status = rml_ui_api.table.get_element_rect(handle, element.view, &result);
            if (status != script_status.success) { return false; }
            rect = new rml_rect(result.x, result.y, result.width, result.height);
            return true;
        }
        /// <summary>
        /// ABI 42: writes one CSS declaration - <c>"width"</c> with <c>"360dp"</c>, or
        /// <c>"background-color"</c> with <c>"#4a9eff"</c> - through the same parser an inline style
        /// uses. A host built before ABI 42 reports module_unavailable. No change event is raised.
        /// </summary>
        public bool try_set_element_style(string? id, string? property, string? value) =>
            try_set_element_style(id, property, value, out _);
        public unsafe bool try_set_element_style(string? id, string? property, string? value, out script_status status) {
            if (!available(out status)) { return false; }
            if (rml_ui_api.table.set_element_style == null) {
                status = script_status.module_unavailable;
                return false;
            }
            if (id == null || !valid_text(id, false) || property == null || !valid_text(property, false) ||
                !valid_text(value)) {
                status = script_status.invalid_argument;
                return false;
            }
            using var element = native_string.create(id);
            using var name = native_string.create(property);
            using var content = native_string.create(value);
            status = rml_ui_api.table.set_element_style(handle, element.view, name.view, content.view);
            return status == script_status.success;
        }
        /// <summary>False with success means the queue is empty. Returned strings are owned copies.</summary>
        public bool try_poll_event(out rml_event result) => try_poll_event(out result, out _);
        public unsafe bool try_poll_event(out rml_event result, out script_status status) {
            result = default;
            if (!available(out status)) { return false; }
            rml_ui_event_layout info = default;
            status = rml_ui_api.table.poll_event(handle, &info, null, 0);
            if (status != script_status.success || info.type == 0) { return false; }
            var size = (ulong)info.id_length + info.value_length;
            if (size > 1024 * 1024 || info.type is < 1 or > 5) {
                status = script_status.module_invalid;
                return false;
            }
            var bytes = new byte[Math.Max(1, (int)size)];
            fixed (byte* buffer = bytes) {
                status = rml_ui_api.table.poll_event(handle, &info, buffer, (uint)bytes.Length);
            }
            if (status != script_status.success) { return false; }
            if (info.type is < 1 or > 5 || (ulong)info.id_length + info.value_length > (ulong)bytes.Length) {
                status = script_status.module_invalid;
                return false;
            }
            result = new rml_event((rml_event_type)info.type,
                Encoding.UTF8.GetString(bytes, 0, (int)info.id_length),
                Encoding.UTF8.GetString(bytes, (int)info.id_length, (int)info.value_length),
                info.x, info.y, info.button, info.modifiers);
            return true;
        }
        public bool close() => close(out _);
        public unsafe bool close(out script_status status) {
            if (handle == 0 || !rml_ui_api.is_current(epoch)) {
                Interlocked.Exchange(ref value, 0);
                status = script_status.success;
                return true;
            }
            if (!available(out status)) { return false; }
            status = rml_ui_api.table.close(handle);
            if (status == script_status.success || status == script_status.invalid_node) {
                Interlocked.Exchange(ref value, 0);
            }
            return status == script_status.success;
        }
        public void Dispose() => close(out _);
    }
}
