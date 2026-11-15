using System.Runtime.InteropServices;

namespace deren.scripting {
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct managed_api {
        public delegate* unmanaged<frame_info*, script_status> update;
        public delegate* unmanaged<script_status> stop;
        public delegate* unmanaged<uint, script_status> signal;
    }
}
