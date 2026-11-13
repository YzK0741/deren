using System;
using System.Runtime.InteropServices;
using System.Text;

namespace deren.scripting {
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct utf8_view {
        public byte* data;
        public uint length;

        public readonly string decode() {
            return Encoding.UTF8.GetString(new ReadOnlySpan<byte>(data, checked((int)length)));
        }
    }
}
