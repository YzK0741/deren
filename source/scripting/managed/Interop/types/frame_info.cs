using System.Runtime.InteropServices;

namespace deren.scripting {
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct frame_info {
        public ulong scene;
        public float delta_time;
        public uint input_count;
        public byte* input;
    }
}
