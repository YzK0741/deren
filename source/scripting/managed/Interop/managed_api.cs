using System.Runtime.InteropServices;

namespace deren.scripting {
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct managed_api {
        public delegate* unmanaged[Cdecl]<frame_info*, script_status> update;
        public delegate* unmanaged[Cdecl]<script_status> stop;
    }
}
