using System.Runtime.InteropServices;

namespace deren.scripting {
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct managed_api {
        public uint size;
        public uint version;
        public delegate* unmanaged[Cdecl]<native_api*, ulong, utf8_view, script_binding*, uint, utf8_view*, uint, script_status> start_scene;
        public delegate* unmanaged[Cdecl]<ulong, float, byte*, uint, script_status> update_scene;
        public delegate* unmanaged[Cdecl]<ulong, script_status> stop_scene;
    }
}
