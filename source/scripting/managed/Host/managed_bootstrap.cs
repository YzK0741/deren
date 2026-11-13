using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace deren.scripting {
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
