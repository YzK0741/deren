using System.Runtime.InteropServices;

namespace deren.scripting {
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct managed_api {
        public delegate* unmanaged<frame_info*, script_status> update;
        public delegate* unmanaged<script_status> stop;

        // ABI 34: the two frame boundaries. Each one is a void callback; the host waits for
        // native_api notify_rendering before it continues, so managed code must always answer.
        public delegate* unmanaged<void> before_frame;
        public delegate* unmanaged<void> after_frame;
    }
}
