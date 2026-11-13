using System.Runtime.InteropServices;

namespace deren.scripting {
    // The native node identity is a process-wide scene_tree node id packed as a 64-bit value.
    // The managed side treats it as opaque and only checks that it is non-zero.
    [StructLayout(LayoutKind.Sequential)]
    internal struct node_handle {
        public ulong value;
    }
}
