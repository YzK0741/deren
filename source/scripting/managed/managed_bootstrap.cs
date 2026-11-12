using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace deren.scripting {
    [StructLayout(LayoutKind.Sequential)]
    internal struct entity_handle {
        public ulong scene;
        public uint index;
        public uint generation;
    }

    internal enum script_status {
        success = 0,
        invalid_argument = 1,
        invalid_entity = 2,
        scene_exists = 3,
        scene_not_found = 4,
        script_failure = 5,
        spawn_unavailable = 6
    }

    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct utf8_view {
        public byte* data;
        public uint length;

        public readonly string decode() {
            return Encoding.UTF8.GetString(new ReadOnlySpan<byte>(data, checked((int)length)));
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct script_binding {
        public entity_handle entity;
        public utf8_view type;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct native_api {
        public uint size;
        public uint version;
        public void* context;
        public delegate* unmanaged[Cdecl]<void*, byte*, uint, void> log;
        public delegate* unmanaged[Cdecl]<void*, entity_handle, transform_value*, script_status> get_transform;
        public delegate* unmanaged[Cdecl]<void*, entity_handle, transform_value*, script_status> set_transform;
        public delegate* unmanaged[Cdecl]<void*, utf8_view, transform_value*, entity_handle*, script_status> try_spawn;
        public delegate* unmanaged[Cdecl]<void*, entity_handle, script_status> try_despawn;

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

    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct managed_api {
        public uint size;
        public uint version;
        public delegate* unmanaged[Cdecl]<native_api*, ulong, utf8_view, script_binding*, uint, utf8_view*, uint, script_status> start_scene;
        public delegate* unmanaged[Cdecl]<ulong, float, byte*, uint, script_status> update_scene;
        public delegate* unmanaged[Cdecl]<ulong, script_status> stop_scene;
    }

    internal static unsafe class managed_bootstrap {
        internal const uint abi_version = 4;
        private const int abi_mismatch = 1;
        private const int invalid_api = 2;

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static int initialize_from_engine(native_api* api, managed_api* scripts) {
            if (api == null || scripts == null) {
                return invalid_api;
            }
            if (api->size < sizeof(native_api) || api->version != abi_version ||
                scripts->size < sizeof(managed_api) || scripts->version != abi_version) {
                return abi_mismatch;
            }
            if (!api->is_valid()) {
                return invalid_api;
            }

            scripts->start_scene = &scene_scripts.start_scene;
            scripts->update_scene = &scene_scripts.update_scene;
            scripts->stop_scene = &scene_scripts.stop_scene;
            api->write_log("deren managed host ready"u8);
            return 0;
        }
    }
}
