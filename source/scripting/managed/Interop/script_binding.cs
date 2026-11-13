using System.Runtime.InteropServices;

namespace deren.scripting {
    [StructLayout(LayoutKind.Sequential)]
    internal struct script_binding {
        public node_handle node;
        public utf8_view type;
    }
}
