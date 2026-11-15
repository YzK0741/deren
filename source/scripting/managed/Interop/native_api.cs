using System;
using System.Runtime.InteropServices;

namespace deren.scripting {
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct native_api {
        public delegate* unmanaged<byte*, uint, void> log;

        // ABI 33: `byte_size` receives sizeof() of the module table `table` points at, so this side
        // can compare the host's layout against its own mirror instead of trusting the version.
        public delegate* unmanaged<uint, void**, uint*, script_status> get_module;
        public delegate* unmanaged<script_status> request_screenshot;

        // ABI 31: raw window input. The host calls the callback on the render thread for every event
        // and hands `context` back unchanged; a null callback stops the forwarding.
        public delegate* unmanaged<delegate* unmanaged<void*, native_input_event*, void>, void*, void> set_input_event_callback;

        // ABI 33: one {type, sizeof} entry per mirrored type; abi_manifest.verify compares all of
        // them and refuses to bind on a mismatch.
        public delegate* unmanaged<abi_layout_entry*, uint, script_status> abi_layout;

        public readonly bool is_valid() {
            return log != null && get_module != null;
        }

        public readonly void write_log(ReadOnlySpan<byte> message) {
            fixed (byte* data = message) {
                log(data, (uint)message.Length);
            }
        }
    }
}
