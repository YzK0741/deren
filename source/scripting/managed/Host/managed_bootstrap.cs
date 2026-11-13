using System;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;

namespace deren.scripting {
    internal static unsafe class managed_bootstrap {
        private const uint abi_version = 16;
        internal static native_api core;
        internal static Assembly? game_assembly;
        private const int abi_mismatch = 1;
        private const int invalid_api = 2;

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static int initialize_from_engine(native_api* api, managed_api* scripts, uint version, utf8_view game_path) {
            if (api == null || scripts == null || game_path.data == null || game_path.length == 0) {
                return invalid_api;
            }
            if (version != abi_version) {
                return abi_mismatch;
            }
            if (!api->is_valid()) {
                return invalid_api;
            }

            core = *api;
            try {
                var path = Path.GetFullPath(game_path.decode());
                var context = AssemblyLoadContext.GetLoadContext(typeof(managed_bootstrap).Assembly)!;
                game_assembly = context.LoadFromAssemblyPath(path);
            } catch (Exception error) {
                api->write_log(Encoding.UTF8.GetBytes($"Managed host: cannot load project assembly: {error.GetBaseException().Message}"));
                return (int)script_status.script_failure;
            }

            var host_type = find_game_host(game_assembly);
            if (host_type == null) {
                api->write_log("Managed host: project does not define exactly one game_entry."u8);
                return (int)script_status.script_failure;
            }
            var host = (game_entry)Activator.CreateInstance(host_type, nonPublic: true)!;
            var status = host.initialize(api, scripts);
            if (status != script_status.success) {
                api->write_log(Encoding.UTF8.GetBytes($"Managed host: project initialization failed ({status})."));
                return (int)status;
            }
            if (scripts->update == null || scripts->stop == null) {
                api->write_log("Managed host: project did not register update/stop."u8);
                return (int)script_status.script_failure;
            }
            api->write_log("deren managed host ready"u8);
            return 0;
        }

        private static Type? find_game_host(Assembly assembly) {
            Type? found = null;
            foreach (var type in assembly.GetTypes()) {
                if (type.IsAbstract || type.ContainsGenericParameters ||
                    !typeof(game_entry).IsAssignableFrom(type) || type.GetConstructor(Type.EmptyTypes) == null) {
                    continue;
                }
                if (found != null) {
                    throw new InvalidOperationException(
                        $"Project defines more than one game_entry ('{found.FullName}', '{type.FullName}').");
                }
                found = type;
            }
            return found;
        }
    }
}
