using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;

namespace deren.scripting {
    internal static unsafe class managed_bootstrap {
        private const uint abi_version = 33;
        internal static native_api core;
        internal static Assembly? game_assembly;

        /// <summary>Directory the project assembly was loaded from: where `deren.toml` lives.</summary>
        internal static string project_directory = string.Empty;

        private static game_entry? game;
        private const int abi_mismatch = 1;
        private const int invalid_api = 2;

        /// <summary>
        /// Runs before the native window and the renderer exist. The project owns all startup
        /// decisions; the native host consumes the filled startup_options afterwards.
        /// </summary>
        [UnmanagedCallersOnly]
        public static int configure_from_engine(native_api* api, uint version, utf8_view game_path,
                                                startup_options* options, int argc, byte** argv) {
            if (api == null || game_path.data == null || game_path.length == 0 || options == null ||
                argc < 0 || (argc > 0 && argv == null)) {
                return invalid_api;
            }
            if (version != abi_version) {
                return abi_mismatch;
            }
            if (!api->is_valid()) {
                return invalid_api;
            }
            // ABI 33: refuse to run on a layout this side does not understand.
            if (!abi_manifest.verify(api)) {
                return invalid_api;
            }

            core = *api;
            try {
                var path = Path.GetFullPath(game_path.decode());
                project_directory = Path.GetDirectoryName(path) ?? string.Empty;
                var context = AssemblyLoadContext.GetLoadContext(typeof(managed_bootstrap).Assembly)!;
                game_assembly = context.LoadFromAssemblyPath(path);
            } catch (Exception error) {
                api->write_log(Encoding.UTF8.GetBytes($"Managed host: cannot load project assembly: {error.GetBaseException().Message}"));
                return (int)script_status.script_failure;
            }

            game_entry? created;
            try {
                var host_type = find_game_host(game_assembly);
                if (host_type == null) {
                    api->write_log("Managed host: project does not define exactly one game_entry."u8);
                    return (int)script_status.script_failure;
                }
                created = (game_entry?)Activator.CreateInstance(host_type, nonPublic: true);
            } catch (Exception error) {
                api->write_log(Encoding.UTF8.GetBytes($"Managed host: cannot create game_entry: {error.GetBaseException().Message}"));
                return (int)script_status.script_failure;
            }
            if (created == null) {
                api->write_log("Managed host: game_entry constructor returned null."u8);
                return (int)script_status.script_failure;
            }
            game = created;
            // The project's `deren.toml` sits next to the assembly it was loaded from. Parse the
            // lighting profile before preinit so the project can read it during startup; the
            // runtime side is applied later, once the scene API is bound.
            project_config.load(project_directory);
            var status = game.preinit(options, argc, argv);
            if (status != script_status.success) {
                api->write_log(Encoding.UTF8.GetBytes($"Managed host: project preinit failed ({status})."));
                game = null;
                return (int)status;
            }
            return 0;
        }

        [UnmanagedCallersOnly]
        public static int initialize_from_engine(native_api* api, managed_api* scripts) {
            if (api == null || scripts == null) {
                return invalid_api;
            }
            if (game == null) {
                api->write_log("Managed host: initialize called before preinit."u8);
                return invalid_api;
            }
            core = *api;
            // ABI 33: the layout gate also guards a session that reaches initialize without configure.
            if (!abi_manifest.verify(api)) {
                return invalid_api;
            }
            screenshot.bind(api);
            scene_api.initialize(api);
            // THE PANEL TABLE IS OPTIONAL: a host without the GUI plugin still publishes it, but an
            // older host does not publish the ui module at all - that is a session without a panel, not
            // an initialization failure, so the status is only logged when the module is missing.
            var ui_status = ui_api.initialize(api);
            if (ui_status != script_status.success) {
                api->write_log(Encoding.UTF8.GetBytes($"Managed host: no panel module ({ui_status}); ui calls will fail."));
            }
            // Framework baseline first: the project's initialize can still move anything after.
            project_config.apply();
            var status = game.initialize(api, scripts);
            if (status != script_status.success) {
                api->write_log(Encoding.UTF8.GetBytes($"Managed host: project initialization failed ({status})."));
                return (int)status;
            }
            scripts->signal = &signal_from_engine;
            if (scripts->update == null || scripts->stop == null || scripts->signal == null) {
                api->write_log("Managed host: project did not register update/stop/signal."u8);
                return (int)script_status.script_failure;
            }
            api->write_log("deren managed host ready"u8);
            return 0;
        }

        /// <summary>
        /// Called by the native render loop after a presented frame. Managed code owns the queue of
        /// callables; the native side only emits the signal.
        /// </summary>
        [UnmanagedCallersOnly]
        public static script_status signal_from_engine(uint signal) {
            try {
                frame_scheduler.dispatch(signal);
                return script_status.success;
            } catch (Exception error) {
                if (core.log != null) {
                    core.write_log(Encoding.UTF8.GetBytes(
                        $"Managed host: frame signal failed: {error.GetBaseException().Message}"));
                }
                return script_status.script_failure;
            }
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
