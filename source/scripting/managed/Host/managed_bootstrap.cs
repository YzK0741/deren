using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace deren.scripting {
    internal static unsafe class managed_bootstrap {
        private const uint abi_version = 43;
        internal static native_api core;
        internal static Assembly? game_assembly;

        /// <summary>Directory the project assembly was loaded from: where `deren.toml` lives.</summary>
        internal static string project_directory = string.Empty;

        private static game_entry? game;

        // Installed by initialize: the project's own stop entry, behind the shutdown wrapper below.
        private static delegate* unmanaged<script_status> project_stop;
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
                host_logger.write(api, $"Managed host: cannot load project assembly: {error.GetBaseException().Message}");
                return (int)script_status.script_failure;
            }

            game_entry? created;
            try {
                var host_type = find_game_host(game_assembly);
                if (host_type == null) {
                    host_logger.write(api, "Managed host: project does not define exactly one game_entry.");
                    return (int)script_status.script_failure;
                }
                created = (game_entry?)Activator.CreateInstance(host_type, nonPublic: true);
            } catch (Exception error) {
                host_logger.write(api, $"Managed host: cannot create game_entry: {error.GetBaseException().Message}");
                return (int)script_status.script_failure;
            }
            if (created == null) {
                host_logger.write(api, "Managed host: game_entry constructor returned null.");
                return (int)script_status.script_failure;
            }
            game = created;
            // The project's `deren.toml` sits next to the assembly it was loaded from. Parse the
            // lighting profile before preinit so the project can read it during startup; the
            // runtime side is applied later, once the scene API is bound.
            project_config.load(project_directory);
            var status = game.preinit(options, argc, argv);
            if (status == script_status.success) {
                return 0;
            }

            host_logger.write(api, $"Managed host: project preinit failed ({status}).");
            game = null;
            return (int)status;
        }

        [UnmanagedCallersOnly]
        public static int initialize_from_engine(native_api* api, managed_api* scripts) {
            if (api == null || scripts == null) {
                return invalid_api;
            }
            if (game == null) {
                host_logger.write(api, "Managed host: initialize called before preinit.");
                return invalid_api;
            }
            core = *api;
            // ABI 37: an exception nothing caught ends the process through the same report as an
            // explicit host_logger.panic. Installed here, where the table is first held - a fault before
            // this point has no host to report through, and the runtime's own message is the whole
            // story then.
            AppDomain.CurrentDomain.UnhandledException -= on_unhandled_exception;
            AppDomain.CurrentDomain.UnhandledException += on_unhandled_exception;
            // ABI 33: the layout check also guards a session that reaches initialize without configure.
            if (!abi_manifest.verify(api)) {
                return invalid_api;
            }
            screen_snapshot.bind(api);
            scene_api.initialize(api);
            // THE PANEL TABLE IS OPTIONAL: a host without the GUI plugin still publishes it, but an
            // older host does not publish the ui module at all - that is a session without a panel, not
            // an initialization failure, so the status is only logged when the module is missing.
            var ui_status = ui_api.initialize(api);
            if (ui_status != script_status.success) {
                host_logger.write(api, $"Managed host: no panel module ({ui_status}); ui calls will fail.");
            }
            var audio_status = audio_api.initialize(api);
            if (audio_status == script_status.module_invalid) {
                host_logger.write(api, "Managed host: invalid audio module; audio calls will fail.");
            }
            var rml_status = rml_ui_api.initialize(api);
            if (rml_status == script_status.module_invalid) {
                host_logger.write(api, "Managed host: invalid RmlUi module; document calls will fail.");
            }
            // Framework baseline first: the project's initialize can still move anything after.
            project_config.apply();
            var status = game.initialize(api, scripts);
            if (status != script_status.success) {
                host_logger.write(api, $"Managed host: project initialization failed ({status}).");
                return (int)status;
            }
            scripts->before_frame = &before_frame_from_engine;
            scripts->after_frame = &after_frame_from_engine;
            if (scripts->update == null || scripts->stop == null) {
                host_logger.write(api, "Managed host: project did not register update/stop.");
                return (int)script_status.script_failure;
            }
            // Wrap the project's stop so the frame worker ends before the native side tears down.
            project_stop = scripts->stop;
            scripts->stop = &stop_from_engine;
            host_logger.write(api, "deren managed host ready");
            return 0;
        }

        /// <summary>
        /// Called once by the render loop during shutdown. The frame worker is stopped first, so a
        /// batch that was queued but not started never runs against a torn-down host.
        /// </summary>
        [UnmanagedCallersOnly]
        public static script_status stop_from_engine() {
            frame_event_processor.shutdown();
            try {
                return project_stop == null ? script_status.script_failure : project_stop();
            } finally {
                rml_ui_api.shutdown();
                audio_api.shutdown();
            }
        }

        /// <summary>
        /// Called by the render loop once the frame is certain to be drawn. The batch runs on the
        /// worker thread owned by frame_event_processor, which also answers the host wait; this
        /// entry point only prepares it, so it returns before the tasks have run.
        /// </summary>
        [UnmanagedCallersOnly]
        public static void before_frame_from_engine() {
            run_boundary(frame_event_processor.dispatch_before);
        }

        /// <summary>Called by the render loop after the frame was presented.</summary>
        [UnmanagedCallersOnly]
        public static void after_frame_from_engine() {
            run_boundary(frame_event_processor.dispatch_after);
        }

        private static void run_boundary(Action dispatch) {
            try {
                dispatch();
            } catch (Exception error) {
                host_logger.write($"Managed host: frame boundary failed: {error.GetBaseException().Message}");
                // The host is blocked on the answer: a batch that never started must still release it.
                frame_event_processor.notify_host();
            }
        }

        /// <summary>
        /// ABI 37: an exception nothing caught ends the process through the host's crash report.
        ///
        /// The exception's own text is better evidence than the frames that merely noticed it, so it
        /// becomes the managed stack; the message names the type, which a reader greps for.
        /// </summary>
        private static void on_unhandled_exception(object? sender, UnhandledExceptionEventArgs args) {
            var description = args.ExceptionObject is Exception known
                                     ? $"unhandled managed exception: {known.GetType().FullName}: {known.Message}"
                                     : "unhandled managed exception";
            var detail = args.ExceptionObject.ToString() ?? string.Empty;
            // The report pairs a call site with every stack, so ask the exception where it was thrown
            // instead of leaving `source:` empty: the first frame carries the throw site's file and line.
            var file = string.Empty;
            var line = 0;
            if (args.ExceptionObject is Exception thrown) {
                var site = new System.Diagnostics.StackTrace(thrown, fNeedFileInfo: true).GetFrame(0);
                if (site != null) {
                    file = site.GetFileName() ?? string.Empty;
                    line = site.GetFileLineNumber();
                }
            }
            host_logger.panic(core, description, file, line, detail);
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
