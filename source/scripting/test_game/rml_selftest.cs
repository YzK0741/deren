using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using deren.scripting;

namespace deren {
    internal static unsafe class rml_selftest {
        internal static string? path;
        private static rml_document? document;
        private static rml_event copied;
        private static bool missing;
        internal static script_status initialize(managed_api* scripts) {
            scripts->update = &update;
            scripts->stop = &stop;
            missing = path == "missing";
            if (missing) {
                return !rml_document.try_open("missing.rml", "missing.ttf", out _, out var status) &&
                       status == script_status.module_unavailable ? script_status.success : script_status.script_failure;
            }
            if (!rml_document.try_open(Path.Combine(path!, "hud.rml"), Path.Combine(path!, "LatoLatin-Regular.ttf"),
                                       out document, out _) || document == null) {
                return script_status.script_failure;
            }
            if (!document.set_text("status", "Copied <text> & \u4e2d") ||
                !document.set_value("name", "value") || !document.set_visible("panel", false) ||
                !document.set_visible("panel", true) || document.set_text("missing", "text") ||
                document.set_text("status", "bad\0text")) {
                return script_status.script_failure;
            }
            return Task.Run(() => !document.set_text("status", "worker", out var status) &&
                status == script_status.module_unavailable).GetAwaiter().GetResult()
                ? script_status.success : script_status.script_failure;
        }
        [UnmanagedCallersOnly]
        private static script_status update(frame_info* frame) {
            if (missing) { return script_status.success; }
            if (document == null || !document.try_poll_event(out copied) ||
                copied.type != rml_event_type.click || copied.id != "apply" ||
                document.try_poll_event(out _, out var empty_status) || empty_status != script_status.success) {
                return script_status.script_failure;
            }
            document.Dispose();
            document.Dispose();
            if (document.set_text("status", "stale", out var disposed) || disposed != script_status.invalid_node) {
                return script_status.script_failure;
            }
            if (!rml_document.try_open(Path.Combine(path!, "hud.rml"), Path.Combine(path!, "LatoLatin-Regular.ttf"),
                                       out document) || document == null || copied.id != "apply") {
                return script_status.script_failure;
            }
            return script_status.success;
        }
        [UnmanagedCallersOnly]
        private static script_status stop() {
            document?.Dispose();
            document?.Dispose();
            return document == null || !document.is_valid ? script_status.success : script_status.script_failure;
        }
    }
}
