using System;
using System.Runtime.InteropServices;

namespace deren.scripting {
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct native_api {
        public delegate* unmanaged[Cdecl]<byte*, uint, void> log;
        public delegate* unmanaged[Cdecl]<uint, void**, script_status> get_module;

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
