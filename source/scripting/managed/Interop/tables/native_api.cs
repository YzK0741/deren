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

        // ABI 34: answers the frame-boundary wait the host entered after before_frame/after_frame.
        // Exactly one call per signal; the host reports a call with nothing outstanding.
        public delegate* unmanaged<script_status> notify_rendering;

        // ABI 35: sets how long the host waits for one frame boundary. 0 waits indefinitely. Null
        // on an older host, which keeps its 5000 ms; frame_scheduler reports module_unavailable.
        public delegate* unmanaged<uint, script_status> set_frame_boundary_timeout;

        // ABI 37: reports a managed fault, which the host answers by writing one crash report and
        // terminating the process. Null on an older host; logger.panic then writes stderr and fails
        // fast on its own. `line` is 0 when the caller had no location.
        public delegate* unmanaged<utf8_view, utf8_view, uint, utf8_view, script_status> panic;

        public readonly bool is_valid() {
            return log != null && get_module != null && notify_rendering != null;
        }

        public readonly void write_log(ReadOnlySpan<byte> message) {
            fixed (byte* data = message) {
                log(data, (uint)message.Length);
            }
        }
    }
}
