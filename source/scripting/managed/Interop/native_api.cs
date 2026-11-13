using System;
using System.Runtime.InteropServices;

namespace deren.scripting {
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct native_api {
        public uint size;
        public uint version;
        public void* context;
        public delegate* unmanaged[Cdecl]<void*, byte*, uint, void> log;
        public delegate* unmanaged[Cdecl]<void*, node_handle, transform_value*, script_status> get_transform;
        public delegate* unmanaged[Cdecl]<void*, node_handle, transform_value*, script_status> set_transform;
        public delegate* unmanaged[Cdecl]<void*, utf8_view, transform_value*, node_handle*, script_status> try_spawn;
        public delegate* unmanaged[Cdecl]<void*, node_handle, script_status> try_despawn;

        public readonly bool is_valid() {
            return size >= sizeof(native_api) && version == managed_bootstrap.abi_version &&
                log != null && get_transform != null && set_transform != null && try_spawn != null && try_despawn != null;
        }

        public readonly void write_log(ReadOnlySpan<byte> message) {
            fixed (byte* data = message) {
                log(context, data, (uint)message.Length);
            }
        }
    }
}
