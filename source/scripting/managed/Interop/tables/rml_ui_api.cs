using System;
using System.Runtime.InteropServices;
using System.Threading;

namespace deren.scripting {
    [StructLayout(LayoutKind.Sequential)]
    internal struct rml_ui_event_layout {
        internal uint type;
        internal uint id_length;
        internal uint value_length;
        // ABI 42: pointer events only, in density-independent pixels; button and modifiers are the
        // values native forwarded. DOM click/change events leave all four at zero.
        internal float x;
        internal float y;
        internal int button;
        internal uint modifiers;
    }

    /// <summary>ABI 42: one element's laid-out border box, in density-independent pixels.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct element_rect_layout {
        internal float x;
        internal float y;
        internal float width;
        internal float height;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct rml_ui_api_layout {
        internal delegate* unmanaged<utf8_view, utf8_view, utf8_view, ulong*, script_status> open;
        internal delegate* unmanaged<ulong, script_status> close;
        internal delegate* unmanaged<ulong, utf8_view, utf8_view, script_status> set_text;
        internal delegate* unmanaged<ulong, utf8_view, utf8_view, script_status> set_value;
        internal delegate* unmanaged<ulong, utf8_view, byte, script_status> set_visible;
        internal delegate* unmanaged<ulong, rml_ui_event_layout*, byte*, uint, script_status> poll_event;
        // ABI 42 optional entries: a host built before them leaves them null, and only the calls that
        // need them report module_unavailable. is_valid() must not require them.
        internal delegate* unmanaged<ulong, utf8_view, element_rect_layout*, script_status> get_element_rect;
        internal delegate* unmanaged<ulong, utf8_view, utf8_view, utf8_view, script_status> set_element_style;
        internal delegate* unmanaged<ulong, utf8_view, utf8_view, script_status> set_markup;
        // ABI 46: swaps the markup of an open document, so one shell can host several layouts. Like the
        // ABI 42 entries above it is optional: a host without it answers module_unavailable.
        internal delegate* unmanaged<ulong, utf8_view, utf8_view, utf8_view, script_status> reopen;
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
            var status = api->get_module((uint)native_module.rml_ui_extended, &value, &size);
            if (status == script_status.module_unavailable) {
                status = api->get_module((uint)native_module.rml_ui, &value, &size);
            }
            if (status != script_status.success) {
                return status;
            }
            uint current_size = (uint)sizeof(rml_ui_api_layout);
            // Optional tails grow one entry at a time, so a host may hand over any prefix of the table:
            // the six mandatory entries at least, then whole entries up to what this build knows. A size
            // in between (a host from before an entry was appended, or after one was) is legal; anything
            // shorter than the mandatory prefix or not entry-aligned is not.
            uint entry = (uint)sizeof(nint);
            uint required_size = 6 * entry;
            if (value == null || size < required_size || size > current_size || (size % entry) != 0) {
                return script_status.module_invalid;
            }
            rml_ui_api_layout resolved = default;
            Buffer.MemoryCopy(value, &resolved, current_size, size);
            if (!resolved.is_valid()) {
                return script_status.module_invalid;
            }
            table = resolved;
            owner_thread = Environment.CurrentManagedThreadId;
            return script_status.success;
        }
    }
}
