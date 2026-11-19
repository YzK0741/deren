using System;
using System.Runtime.InteropServices;
using System.Threading;

namespace deren.scripting {
    [StructLayout(LayoutKind.Sequential)]
    internal struct rml_ui_event_layout {
        internal uint type;
        internal uint id_length;
        internal uint value_length;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct rml_ui_api_layout {
        internal delegate* unmanaged<utf8_view, utf8_view, utf8_view, ulong*, script_status> open;
        internal delegate* unmanaged<ulong, script_status> close;
        internal delegate* unmanaged<ulong, utf8_view, utf8_view, script_status> set_text;
        internal delegate* unmanaged<ulong, utf8_view, utf8_view, script_status> set_value;
        internal delegate* unmanaged<ulong, utf8_view, byte, script_status> set_visible;
        internal delegate* unmanaged<ulong, rml_ui_event_layout*, byte*, uint, script_status> poll_event;
        internal readonly bool is_valid() => open != null && close != null && set_text != null &&
            set_value != null && set_visible != null && poll_event != null;
    }

    internal static unsafe class rml_ui_api {
        internal static rml_ui_api_layout table;
        private static int generation;
        private static int owner_thread;
        internal static int epoch => Volatile.Read(ref generation);
        internal static bool is_current(int value) => value == epoch && table.is_valid();
        internal static bool is_ready() {
            if (!host_call_guard.host_calls_allowed || Environment.CurrentManagedThreadId != owner_thread) {
                host_call_guard.note_refusal();
                return false;
            }
            return table.is_valid();
        }
        internal static void shutdown() {
            table = default;
            owner_thread = 0;
            Interlocked.Increment(ref generation);
        }
        internal static script_status initialize(native_api* api) {
            shutdown();
            if (api == null || api->get_module == null) {
                return script_status.module_unavailable;
            }
            void* value = null;
            uint size = 0;
            var status = api->get_module((uint)native_module.rml_ui, &value, &size);
            if (status != script_status.success) {
                return status;
            }
            if (value == null || size != (uint)sizeof(rml_ui_api_layout)) {
                return script_status.module_invalid;
            }
            var resolved = *(rml_ui_api_layout*)value;
            if (!resolved.is_valid()) {
                return script_status.module_invalid;
            }
            table = resolved;
            owner_thread = Environment.CurrentManagedThreadId;
            return script_status.success;
        }
    }
}
