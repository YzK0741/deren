using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

namespace deren.scripting {
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct audio_api_layout {
        internal delegate* unmanaged<utf8_view, ulong*, script_status> load_buffer;
        internal delegate* unmanaged<ulong, script_status> release_buffer;
        internal delegate* unmanaged<ulong, ulong*, script_status> create;
        internal delegate* unmanaged<utf8_view, ulong*, script_status> create_from_file;
        internal delegate* unmanaged<ulong, byte, ulong*, script_status> clone;
        internal delegate* unmanaged<ulong, script_status> release;
        internal delegate* unmanaged<ulong, uint, double, script_status> command;
        internal delegate* unmanaged<ulong, uint, double*, script_status> query;
        internal delegate* unmanaged<ulong, uint, double*, script_status> buffer_query;
        internal readonly bool is_valid() => load_buffer != null && release_buffer != null &&
            create != null && create_from_file != null && clone != null && release != null &&
            command != null && query != null && buffer_query != null;
    }

    internal static unsafe class audio_api {
        internal static audio_api_layout table;
        private static int generation;
        internal static int epoch => Volatile.Read(ref generation);
        internal static bool is_current(int value) => value == epoch && table.is_valid();
        internal static bool is_ready() {
            if (!host_call_guard.host_calls_allowed) {
                host_call_guard.note_refusal();
                return false;
            }
            return table.is_valid();
        }
        internal static void shutdown() {
            table = default;
            Interlocked.Increment(ref generation);
        }
        internal static script_status initialize(native_api* api) {
            shutdown();
            if (api == null || api->get_module == null) {
                return script_status.module_unavailable;
            }
            void* value = null;
            uint size = 0;
            var status = api->get_module((uint)native_module.audio, &value, &size);
            if (status != script_status.success) {
                return status;
            }
            if (value == null || size != (uint)sizeof(audio_api_layout)) {
                return script_status.module_invalid;
            }
            var resolved = *(audio_api_layout*)value;
            if (!resolved.is_valid()) {
                return script_status.module_invalid;
            }
            table = resolved;
            return script_status.success;
        }
        internal static bool try_path(string path, out native_string value, out script_status status) {
            value = default;
            if (string.IsNullOrWhiteSpace(path) || path.Contains('\0')) {
                status = script_status.invalid_argument;
                return false;
            }
            try {
                var root = string.IsNullOrEmpty(managed_bootstrap.project_directory)
                    ? Environment.CurrentDirectory : managed_bootstrap.project_directory;
                value = native_string.create(Path.GetFullPath(path, root));
                status = script_status.success;
                return true;
            } catch (Exception error) when (error is ArgumentException or NotSupportedException or IOException) {
                status = script_status.invalid_argument;
                return false;
            }
        }
    }
}
