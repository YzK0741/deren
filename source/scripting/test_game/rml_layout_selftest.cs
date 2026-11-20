using System;
using System.IO;
using System.Runtime.InteropServices;
using deren.scripting;

namespace deren {
    /// <summary>
    /// ABI 42: proves the managed layout surface against the real RmlUi fixture - pointer feedback in
    /// density-independent pixels, an element rect that follows a style write on the next paint, and
    /// the module_unavailable answer a host without the optional table tail produces.
    /// </summary>
    internal static unsafe class rml_layout_selftest {
        internal static string? path;
        internal static bool optional_absent;
        private static rml_document? document;
        private static int phase;

        internal static script_status initialize(managed_api* scripts) {
            scripts->update = &update;
            scripts->stop = &stop;
            if (!rml_document.try_open(Path.Combine(path!, "hud.rml"), Path.Combine(path!, "LatoLatin-Regular.ttf"),
                                       out document) || document == null) {
                return script_status.script_failure;
            }
            return script_status.success;
        }

        [UnmanagedCallersOnly]
        private static script_status update(frame_info* frame) {
            if (document == null) { return script_status.script_failure; }
            switch (phase) {
            case 0:
                // A host built before ABI 42 publishes the original six entries only; the managed side
                // must refuse these two calls instead of binding a null pointer.
                if (optional_absent) {
                    phase = 2;
                    return document.try_get_element_rect("panel", out _, out var rect_status) ||
                                   rect_status != script_status.module_unavailable ||
                                   document.try_set_element_style("panel", "width", "360dp", out var style_status) ||
                                   style_status != script_status.module_unavailable
                               ? script_status.script_failure
                               : script_status.success;
                }
                // First tick: the viewport and panel rects are in density-independent pixels, and a
                // missing element is refused rather than reported as a zero rect.
                if (!document.try_get_element_rect(null, out var viewport) || !near(viewport.x, 0.0F) ||
                    !near(viewport.y, 0.0F) || !near(viewport.width, 320.0F) || !near(viewport.height, 240.0F)) {
                    return script_status.script_failure;
                }
                if (!document.try_get_element_rect("panel", out var panel) || !near(panel.x, 20.0F) ||
                    !near(panel.y, 20.0F) || !near(panel.width, 320.0F) || !near(panel.height, 220.0F)) {
                    return script_status.script_failure;
                }
                if (document.try_get_element_rect("missing", out _) ||
                    document.try_get_element_rect("missing", out _, out var missing_status) ||
                    missing_status != script_status.invalid_node) {
                    return script_status.script_failure;
                }
                // Style writes go through the same parser an inline style uses; a malformed declaration
                // is rejected, and a missing element never becomes a silent no-op.
                if (!document.try_set_element_style("panel", "width", "360dp") ||
                    !document.try_set_element_style("panel", "border-bottom-color", "#4a9eff") ||
                    document.try_set_element_style("missing", "width", "360dp") ||
                    document.try_set_element_style("panel", "width", "not-a-width", out var rejected) ||
                    rejected != script_status.invalid_argument) {
                    return script_status.script_failure;
                }
                phase = 1;
                return script_status.success;
            case 1:
                // The host painted again, so the width written during the first tick is laid out. The
                // rect is the border box, because RmlUi lays out with box-sizing: border-box.
                if (!document.try_get_element_rect("panel", out var resized) || !near(resized.width, 360.0F)) {
                    return script_status.script_failure;
                }
                return check_pointer_events() ? script_status.success : script_status.script_failure;
            default:
                return script_status.success;
            }
        }

        /// <summary>
        /// The host forwarded one move onto the button, a press, a drag off the panel and the release.
        /// Nothing here is a DOM click: the button is released elsewhere, so the queue is entirely
        /// pointer feedback and every position is in density-independent pixels.
        /// </summary>
        private static bool check_pointer_events() {
            if (!poll(rml_event_type.pointer_move, "apply", 112.0F, 192.0F) ||
                !poll(rml_event_type.pointer_down, "apply", 112.0F, 192.0F) ||
                !poll(rml_event_type.pointer_move, string.Empty, 5.0F, 5.0F) ||
                !poll(rml_event_type.pointer_up, string.Empty, 5.0F, 5.0F)) {
                return false;
            }
            return !document!.try_poll_event(out _, out var empty_status) && empty_status == script_status.success;
        }

        private static bool poll(rml_event_type type, string id, float x, float y) =>
            document!.try_poll_event(out var value) && value.type == type && value.id == id && near(value.x, x) &&
            near(value.y, y) && value.button == 0 && value.modifiers == 0;

        private static bool near(float value, float expected) => MathF.Abs(value - expected) <= 0.5F;

        [UnmanagedCallersOnly]
        private static script_status stop() {
            document?.Dispose();
            document = null;
            return script_status.success;
        }
    }
}
