using System;
using System.Collections.Generic;
using System.Globalization;
using System.Diagnostics;
using System.IO;
using System.Numerics;
using System.Runtime.InteropServices;
using deren;
using deren.scripting;

namespace deren.editor {
    /// <summary>
    /// The editor project's managed entry point. It owns one scene document, builds the activated
    /// scene tree once during startup (scene_sync.rebuild), and then mirrors every edit into that
    /// tree with scene_sync.create_one/destroy_one/push - the native host only allows activating a
    /// tree while the project scene is starting, so a rebuild is never repeated after startup.
    /// </summary>
    internal sealed unsafe partial class editor_host : game_entry {
        private const int event_budget = 128;
        private const string rename_field_id = "rename_field";
        private const string project_label_id = "project_label";
        private const string default_scene_file = "untitled.scene.toml";

        private static editor_host? instance;
        private static string? requested_scene;
        private static string? workspace_title;
        private static string? requested_start_scene;
        private static string? requested_thumbnail;
        private static string? requested_play;
        private static string? requested_game_scene;
        private static bool requested_editor;
        /// <summary>True when the own-UI frontend shows the scene editor instead of the Dope Sheet.</summary>
        private static bool requested_scene_screen;
        /// <summary>`--editor` asked for the editor without naming a frontend: resolve it after the loop.</summary>
        private static bool requested_editor_screen_default;
        /// <summary>`--editor-ui=classic` was given, so the RmlUi shell keeps the run whatever the order.</summary>
        private static bool requested_classic_ui;
        private static IReadOnlyDictionary<string, int> runtime_actions = new Dictionary<string, int>();
        private scene_runtime? runtime;
        private script_catalog? editable_scripts;
        private script_inspector? scripts_panel;
        private readonly output_log_panel output_log = new();
        private play_session? playing;
        private static bool requested_selftest;
        private static string runtime_selftest_asset = "damaged-helmet";

        private scene_document? document;
        private scene_tree? tree;
        private rml_document? ui;
        private outliner_panel? outliner;
        private details_panel? details;
        private environment_panel? environment_ui;
        // The scene-wide HDR path the project settings hold ("" = procedural sky) and the text of every
        // lane the renderer was last told about, so a frame only touches a lane whose map moved.
        private string global_environment_hdr = string.Empty;
        private readonly string?[] applied_environments = new string?[global_environment.slot_count];
        // The text whose hand-over last failed per lane, so a lane the renderer keeps refusing is retried
        // but not logged once per frame.
        private readonly string?[] failed_environments = new string?[global_environment.slot_count];
        // The .hdr files the environment dropdowns offer, cached until the asset library is rescanned.
        private List<environment_entry>? environment_entries;
        private status_bar? status_line;
        private undo_stack? undo_history;
        private node_selection? selection_state;
        private string rename_field = string.Empty;
        // The projection aspect last handed to the renderer, so the per-frame check only calls out when
        // the viewport column actually changes shape.
        private float applied_aspect = -1.0f;
        private readonly viewport_camera viewport_navigation = new();
        private readonly viewport_gizmo gizmo = new();
        // Child -> parent for the current document, rebuilt on demand from the root list. Building it
        // once keeps the per-ancestor walk in layout_gizmo O(1) instead of a full tree scan per level,
        // which made a deep selection cost O(depth * nodes) every frame.
        private readonly Dictionary<document_node, document_node?> parents = new();
        private scene_document? parents_document;
        private bool gizmo_local;
        private bool cancel_gizmo_requested;
        private float viewport_height;
        private Vector2? viewport_press;
        private bool focus_selection_requested;
        private bool camera_input_installed;
        // The dock tab whose body is visible.
        private string active_dock_tab = "tab_content";
        private workspace_layout layout = new();
        private string layout_path = string.Empty;
        // Splitter drag: the captured handle, the pointer position where the drag started and the size
        // (dp) of the panel it resizes at that moment. An empty handle means no drag is in progress.
        private string active_splitter = string.Empty;
        private float drag_origin = 0.0f;
        private float drag_start_size = 0.0f;
        private float drag_min_size = 0.0f;
        private float drag_max_size = 0.0f;
        // The menu dropdown currently open ("menu_file_items"...), or empty when every menu is closed.
        private string open_menu = string.Empty;
        // The asset ids listed by asset_manager, the two search fields and the picked asset tile.
        private string[] assets = [];
        private string browser_filter = string.Empty;
        private string outliner_filter = string.Empty;
        private content_browser_panel? browser;
        private asset_thumbnail_cache? thumbnails;
        // The viewport tool highlighted by the Select/Move/Rotate/Scale buttons (UE's transform widget mode).
        private const string tool_select_id = "vp_select";        private static readonly string[] tool_ids = ["vp_select", "vp_move", "vp_rotate", "vp_scale"];
        private string active_tool = tool_select_id;
        // A short status-bar suffix ("tool: select", "asset: ...", the About line) cleared by the next click.
        private string notice = string.Empty;
        private bool scene_path_open;
        private bool scene_path_saving;
        private string scene_path_text = string.Empty;
        private string? overwrite_path;
        private scene_document? pending_open;
        private (string asset, Vector2 press)? asset_drag;
        private bool pending_new_scene;
        // The close button waits on the same unsaved-changes prompt as New Scene; the answer is applied
        // to whichever action asked for the dialog.
        private bool pending_close;
        // Borderless caption press: the first press starts the system move loop, a second press inside
        // the double-click window maximises/restores instead.
        private bool caption_pressed;
        private long last_caption_press;
        private string last_clicked_asset = string.Empty;
        private long last_asset_click;
        private int asset_selftest_phase;
        private Guid selftest_asset_uid;
        // The Window-menu layout regression check that runs after the asset phases: it needs several
        // frames because a display change only reaches the element boxes on the next layout pass.
        private int panel_selftest_phase;
        private float panel_rows_height;
        private float panel_title_width;
        private float panel_details_y;
        // Panel visibility toggled from the Window menu; style keeps the layout space from being reserved.
        private bool outliner_shown = true;
        private bool details_shown = true;
        private bool dock_shown = true;
        // Nullable so the first refresh writes the style even when the history starts empty.
        private bool? undo_button_state;
        private bool? redo_button_state;

        public script_status preinit(startup_options* options, int argc, byte** argv) {
            if (options == null || argc < 0 || (argc > 0 && argv == null)) {
                return script_status.invalid_argument;
            }
            var args = new string[argc];
            for (var i = 0; i < argc; ++i) {
                args[i] = argv[i] == null ? string.Empty : Marshal.PtrToStringUTF8((IntPtr)argv[i]) ?? string.Empty;
            }
            var first_option = args.Length > 2 && args[1] == "run" ? 3 : 1;
            for (var i = first_option; i < args.Length; ++i) {
                split_argument(args[i], out var name, out var inline_value);
                switch (name) {
                    case "--editor":
                        requested_editor = true;
                        // The scene editor on the own UI library is the default frontend now; the choice
                        // is resolved after the loop so --editor-ui=classic wins whatever its position.
                        requested_editor_screen_default = true;
                        break;
                    case "--scene":
                        requested_scene = inline_value ?? take_value(args, ref i);
                        if (string.IsNullOrEmpty(requested_scene)) {
                            requested_scene = null;
                        }
                        break;
                    case "--editor-play":
                        requested_play = inline_value ?? take_value(args, ref i);
                        break;
                    case "--editor-thumbnail":
                        requested_thumbnail = inline_value ?? take_value(args, ref i);
                        options->capture_frames = 8;
                        options->capture_windowed = 0;
                        break;
                    case "--editor-selftest-collision":
                        requested_editor = true;
                        requested_collision_selftest = true;
                        options->capture_frames = 8;
                        break;
                    case "--editor-selftest-audio-source":
                        requested_audio_source_selftest = true;
                        requested_editor = true;
                        options->capture_frames = 8;
                        break;
                    case "--editor-selftest-triggers":
                        requested_editor = true;
                        requested_trigger_selftest = true;
                        options->capture_frames = 8;
                        break;
                    case "--editor-selftest-input-actions":
                        requested_editor = true;
                        requested_input_action_selftest = true;
                        options->capture_frames = 8;
                        break;
                    case "--editor-selftest-project":
                        requested_editor = true;
                        requested_project_selftest = true;
                        options->capture_frames = 8;
                        break;
                    case "--editor-selftest-authoring":
                        requested_editor = true;
                        requested_authoring_selftest = true;
                        options->capture_frames = 12;
                        break;
                    case "--editor-selftest-lights":
                        requested_editor = true;
                        requested_lights_selftest = true;
                        options->capture_frames = 8;
                        break;
                    case "--editor-selftest-instances":
                        requested_instances_selftest = true;
                        requested_editor = true;
                        options->capture_frames = 8;
                        break;
                    case "--check-project":
                        requested_editor = true;
                        requested_project_check = true;
                        options->capture_frames = 8;
                        break;
                    case "--editor-selftest-animation":
                        requested_editor = true;
                        requested_animation_selftest = true;
                        options->capture_frames = 8;
                        break;
                    case "--editor-selftest-validation":
                        requested_editor = true;
                        requested_validation_selftest = true;
                        options->capture_frames = 8;
                        break;
                    case "--editor-selftest-animation-mode":
                        requested_editor = true;
                        requested_animation_mode_selftest = true;
                        options->capture_frames = 90;
                        break;
                    case "--editor-ui": {
                        // The documented opt-in form: --editor-ui=next runs the editor on its own UI
                        // library, --editor-ui=classic keeps RmlUi. The demo/selftest flags below are
                        // shortcuts that imply "next". "next" opens the scene screen; "animation" opens
                        // the Dope Sheet on the same library, and "scene" is its explicit spelling.
                        var choice = (inline_value ?? take_value(args, ref i) ?? string.Empty).Trim();
                        if (choice.Equals("next", StringComparison.OrdinalIgnoreCase) ||
                            choice.Equals("scene", StringComparison.OrdinalIgnoreCase)) {
                            requested_editor = true;
                            requested_ui_demo = true;
                            requested_scene_screen = true;
                            options->editor_ui = 1;
                        } else if (choice.Equals("animation", StringComparison.OrdinalIgnoreCase)) {
                            requested_editor = true;
                            requested_ui_demo = true;
                            options->editor_ui = 1;
                        } else if (choice.Equals("classic", StringComparison.OrdinalIgnoreCase)) {
                            // The RmlUi shell stays reachable until its code is removed, and this flag
                            // outranks the default so the order of the arguments cannot change the frontend.
                            requested_classic_ui = true;
                            requested_ui_demo = false;
                            requested_scene_screen = false;
                            options->editor_ui = 0;
                        } else {
                            host_logger.write(
                                $"Editor: unknown UI frontend '{choice}'; expected classic, next or animation");
                        }
                        break;
                    }
                    case "--editor-ui-demo":
                        // Runs the animation editor's Dope Sheet on the editor's own UI library: the
                        // renderer picks the new overlay instead of RmlUi and the classic shell never
                        // opens, so the two frontends stay comparable side by side. An interactive run
                        // stays open: only an explicit --capture-frames bounds it.
                        requested_editor = true;
                        requested_ui_demo = true;
                        options->editor_ui = 1;
                        break;
                    case "--editor-ui-scale": {
                        // Pins the editor UI scale instead of following the window's display density,
                        // so the 100%/150% acceptance pass is one flag away. Percent, e.g. 150.
                        var percent = inline_value ?? take_value(args, ref i) ?? string.Empty;
                        if (float.TryParse(percent, NumberStyles.Float, CultureInfo.InvariantCulture,
                                           out var scale_percent) && scale_percent > 0.0F) {
                            options->ui_scale = scale_percent / 100.0F;
                        }
                        break;
                    }
                    case "--editor-ui-selftest":
                        // Same screen, driven by the real pointer handlers on a copy of the motion files.
                        requested_editor = true;
                        requested_ui_demo = true;
                        requested_ui_selftest = true;
                        options->editor_ui = 1;
                        options->capture_frames = 120;
                        break;
                    case "--editor-ui-scene-selftest":
                        // The scene screen's own phases: rows, details, menus and the pointer claim.
                        requested_editor = true;
                        requested_ui_demo = true;
                        requested_scene_screen = true;
                        requested_ui_selftest = true;
                        options->editor_ui = 1;
                        options->capture_frames = 60;
                        break;
                    case "--editor-animation":
                        requested_editor = true;
                        requested_animation_mode = true;
                        options->capture_frames = Math.Max(options->capture_frames, 30);
                        break;
                    case "--editor-animation-toggle": {
                        // Diagnostic: rebuild the placements by switching modes N times and capture the
                        // result. Each switch destroys and recreates every placement, which is what
                        // exhausted the scene skin buffer before it recycled regions.
                        requested_editor = true;
                        var count = inline_value ?? take_value(args, ref i) ?? string.Empty;
                        if (!int.TryParse(count, out requested_animation_toggle) ||
                            requested_animation_toggle <= 0) {
                            requested_animation_toggle = 10;
                        }
                        options->capture_frames = requested_animation_toggle * 6 + 20;
                        break;
                    }
                    case "--editor-selftest-relocation":
                        requested_relocation_selftest = true;
                        requested_editor = true;
                        options->capture_frames = 8;
                        break;
                    case "--editor-selftest-browser":
                        requested_editor = true;
                        requested_browser_selftest = true;
                        options->capture_frames = 8;
                        break;
                    case "--editor-selftest-import":
                        requested_editor = true;
                        requested_import_selftest = inline_value ?? take_value(args, ref i);
                        options->capture_frames = 8;
                        break;
                    case "--editor-selftest-asset":
                        requested_selftest = true;
                        runtime_selftest_asset = inline_value ?? take_value(args, ref i);
                        break;
                    case "--editor-selftest":
                        requested_selftest = true;
                        break;
                    case "--capture-windowed":
                        options->capture_windowed = 1;
                        break;
                    case "--capture-frames": {
                        var value = inline_value ?? take_value(args, ref i);
                        if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var frames)) {
                            options->capture_frames = Math.Max(0, frames);
                        }
                        break;
                    }
                }
            }
            // `--editor` named no frontend: the scene screen is the default now. `--editor-ui=classic`
            // opts back into RmlUi, and every other `--editor-ui` form already chose a frontend, so this
            // only fills the gap. Resolving here keeps the choice independent of the argument order.
            if (requested_editor_screen_default && !requested_classic_ui && !requested_ui_demo) {
                requested_ui_demo = true;
                requested_scene_screen = true;
                options->editor_ui = 1;
            }
            // The selftest runs over several frames: one per asset phase, then the layout shots the
            // Window-menu check needs (a display change only reaches the boxes on the next pass).
            if (requested_selftest) { options->capture_frames = Math.Max(options->capture_frames, 20); }
            try {
                var root = managed_bootstrap.project_directory;
                var manifest_path = Path.Combine(root, "deren.toml");
                if (File.Exists(manifest_path)) {
                    var manifest = toml_table.parse(File.ReadAllText(manifest_path), out _);
                    workspace_title = manifest?.table("project")?.text("name");
                    if (manifest?.table("editor")?.text("scene") is { Length: > 0 } entry) {
                        requested_start_scene = Path.GetFullPath(entry, root);
                    }
                    if (requested_play != null) {
                        if (string.IsNullOrWhiteSpace(requested_play)) {
                            host_logger.write("Editor Play: a scene snapshot path is required");
                            return script_status.invalid_argument;
                        }
                        runtime_actions = scene_runtime.action_indices(manifest);
                    } else if (!requested_editor && !requested_selftest && requested_thumbnail == null &&
                               manifest?.table("game")?.text("scene") is { } startup_scene) {
                        var path = requested_scene ?? startup_scene;
                        if (!scene_runtime.try_resolve_scene(root, path, out requested_game_scene)) {
                            host_logger.write($"Scene runtime: '{path}' must be a project-relative .scene.toml file");
                            return script_status.invalid_argument;
                        }
                        runtime_actions = scene_runtime.action_indices(manifest);
                    } else if (requested_editor && requested_scene != null) {
                        requested_scene = Path.GetFullPath(requested_scene, root);
                    }
                }
            } catch (Exception error) {
                host_logger.write($"Editor: could not read the project ({error.Message})");
                return script_status.script_failure;
            }
            return script_status.success;
        }

        private static string take_value(string[] args, ref int index) =>
            index + 1 < args.Length ? args[++index] : string.Empty;

        /// <summary>
        /// Splits <c>--name=value</c> at the FIRST '='. The value may itself contain '=', and the option
        /// name must stay whole for the switch below: splitting at the last '=' would turn
        /// <c>--scene=levels=/a.toml</c> into the unknown option <c>--scene=levels</c>.
        /// </summary>
        internal static void split_argument(string argument, out string name, out string? value) {
            var separator = argument.IndexOf('=');
            name = separator < 0 ? argument : argument[..separator];
            value = separator < 0 ? null : argument[(separator + 1)..];
        }

        public script_status initialize(native_api* api, managed_api* scripts) {
            if (api == null || scripts == null || instance != null) {
                return script_status.invalid_argument;
            }
            var module_status = scene_api.initialize(api);
            if (module_status != script_status.success) {
                return module_status;
            }

            if ((requested_play ?? requested_game_scene) is { } runtime_path) {
                var snapshot_directory = requested_play == null ? null : Path.GetDirectoryName(Path.GetFullPath(runtime_path));
                var stop_marker = snapshot_directory == null ? null : Path.Combine(snapshot_directory, "stop.request");
                if (!scene_runtime.try_start(managed_bootstrap.project_directory, runtime_path, runtime_actions,
                                             stop_marker, out runtime, out var runtime_status)) {
                    return runtime_status;
                }
                if (!runtime!.apply_main_camera() && snapshot_directory != null) {
                    apply_play_camera(Path.Combine(snapshot_directory, "camera.json"));
                }
                instance = this;
                scripts->update = &update;
                scripts->stop = &stop;
                return script_status.success;
            }

            if (requested_scene == null && !requested_selftest) {
                requested_scene = requested_start_scene;
            }
            if (requested_thumbnail != null) {
                document = new scene_document();
                document.add_root(new document_node { name = requested_thumbnail, asset = requested_thumbnail });
            } else if (requested_scene != null) {
                if (!scene_instances.try_load(managed_bootstrap.project_directory, requested_scene, out var loaded, out var load_error) || loaded == null) {
                    host_logger.write($"Editor: cannot load scene '{requested_scene}': {load_error}");
                    if (!requested_project_check) {
                        return script_status.script_failure;
                    }
                    host_logger.write("Editor: project check opened an empty scene because the startup scene does not load");
                    document = create_default_document();
                } else {
                    document = loaded;
                }
            } else {
                document = create_default_document();
            }

            // The one and only rebuild: it creates and activates the tree, which the native host
            // requires before it will run the project (see app_scripting.cpp activate_scene).
            if (!scene_sync.rebuild(document, out var rebuilt_tree, out var rebuild_status) || rebuilt_tree == null) {
                host_logger.write($"Editor: cannot rebuild the scene tree ({rebuild_status})");
                if (!requested_project_check) {
                    cleanup();
                    return rebuild_status != script_status.success ? rebuild_status : script_status.script_failure;
                }
                // A project check must be able to report a project whose scene cannot be built at
                // all, so the check opens an empty document instead of refusing to start.
                document = create_default_document();
                if (!scene_sync.rebuild(document, out rebuilt_tree, out rebuild_status) || rebuilt_tree == null) {
                    host_logger.write($"Editor: the empty fallback scene did not rebuild either ({rebuild_status})");
                    cleanup();
                    return rebuild_status != script_status.success ? rebuild_status : script_status.script_failure;
                }
                host_logger.write("Editor: project check opened an empty scene because the startup scene is broken");
            }
            tree = rebuilt_tree;
            if (requested_thumbnail != null) {
                if (!orient_thumbnail(document.roots[0])) {
                    cleanup();
                    return script_status.script_failure;
                }
                instance = this;
                scripts->update = &update;
                scripts->stop = &stop;
                return script_status.success;
            }

            string shell_ui = Path.Combine(managed_bootstrap.assembly_directory, "ui");
            string project_root = managed_bootstrap.project_directory;
            string workspace = string.Equals(project_root, managed_bootstrap.assembly_directory, StringComparison.OrdinalIgnoreCase)
                ? project_root : Path.Combine(project_root, ".deren");
            if (requested_ui_demo) {
                // No classic shell at all: the Dope Sheet is painted by the editor's own UI library and
                // submitted through the overlay the renderer installed instead of RmlUi. Only the state
                // the screens read is created here; the panels and the classic document stay null.
                next_ui = new ui_context();
                selection_state = new node_selection();
                undo_history = new undo_stack();
                // Play is a separate process, so the session and its workspace belong to this frontend too:
                // without them the scene screen's Play button would have nothing to start.
                play_session.sweep_stale_workspaces(workspace);
                playing = new play_session(project_root, workspace);
                thumbnails = new asset_thumbnail_cache(project_root, workspace);
                instance = this;
                scripts->update = &update;
                scripts->stop = &stop;
                return script_status.success;
            }
            if (!rml_document.try_open(Path.Combine(shell_ui, "editor.rml"), Path.Combine(shell_ui, "LatoLatin-Regular.ttf"),
                                       out var opened_document, out var ui_status) || opened_document == null) {
                host_logger.write($"Editor: cannot open ui/editor.rml ({ui_status})");
                cleanup();
                return script_status.script_failure;
            }
            ui = opened_document;
            if (!requested_selftest) {
                Directory.CreateDirectory(workspace);
                layout_path = Path.Combine(workspace, "editor-layout.json");
                layout = workspace_layout.load(layout_path);
                apply_workspace_layout();
            }
            outliner = new outliner_panel(opened_document);
            details = new details_panel(opened_document);
            editable_scripts = new script_catalog(Path.Combine(managed_bootstrap.project_directory, "scripts"));
            scripts_panel = new script_inspector(opened_document, editable_scripts);
            reload_scripts();
            status_line = new status_bar(opened_document);
            undo_history = new undo_stack();
            selection_state = new node_selection();
            // A crashed editor cannot run the cooperative stop that removes its play/thumbnail
            // workspaces, so the next launch sweeps whatever an earlier process left behind.
            play_session.sweep_stale_workspaces(workspace);
            thumbnails = new asset_thumbnail_cache(project_root, workspace);
            playing = new play_session(project_root, workspace);
            browser = new content_browser_panel(opened_document, thumbnails, shell_ui);
            environment_ui = new environment_panel(opened_document);
            // An unset lane is the procedural sky; recording that as already applied keeps startup from
            // rendering three sky cubemaps the document never asked for.
            for (var lane = 1; lane < global_environment.slot_count; ++lane) {
                applied_environments[lane] = string.Empty;
            }
            global_environment_hdr = read_project_environment_hdr();
            // An empty library and a failed query look the same in the grid, so ask for the status and
            // report the failure instead of presenting it as "this project has no assets".
            assets = asset_manager.list(out var assets_status);
            if (assets_status != script_status.success) {
                host_logger.write($"Editor: the asset library could not be listed ({assets_status})");
                notice = "Asset library unavailable";
            }
            apply_tool_style();
            refresh_browser();
            opened_document.set_text(project_label_id, project_title());
            refresh_panels(force_details: true);
            sync_environment_lanes();
            // The managed bootstrap already applied the project's [lighting] environment_hdr while the
            // project started, so only the panel status has to reflect what is in effect.
            environment_ui?.set_status(global_environment_hdr.Length == 0
                ? "Scene map: procedural sky"
                : string.Create(CultureInfo.InvariantCulture, $"Scene map: {Path.GetFileName(global_environment_hdr)}"));
            host_logger.write($"Editor: document ready ({count_nodes(document.roots)} node(s))");

            if (requested_project_check) {
                var check_status = run_project_check_command();
                if (check_status != script_status.success) {
                    cleanup();
                    return check_status;
                }
            }
            if (requested_collision_selftest) {
                var collision_status = run_collision_selftest();
                if (collision_status != script_status.success) {
                    cleanup();
                    return collision_status;
                }
            }
            if (requested_audio_source_selftest) {
                var audio_status = run_audio_source_selftest();
                if (audio_status != script_status.success) {
                    cleanup();
                    return audio_status;
                }
            }
            if (requested_trigger_selftest) {
                var trigger_status = run_trigger_selftest();
                if (trigger_status != script_status.success) {
                    cleanup();
                    return trigger_status;
                }
            }
            if (requested_input_action_selftest) {
                var input_status = run_input_action_selftest();
                if (input_status != script_status.success) {
                    cleanup();
                    return input_status;
                }
            }
            if (requested_project_selftest) {
                var project_status = run_project_selftest();
                if (project_status != script_status.success) {
                    cleanup();
                    return project_status;
                }
            }
            if (requested_authoring_selftest) {
                var authoring_status = run_authoring_selftest();
                if (authoring_status != script_status.success) {
                    cleanup();
                    return authoring_status;
                }
            }
            if (requested_lights_selftest) {
                var lights_status = run_lights_selftest();
                if (lights_status != script_status.success) {
                    cleanup();
                    return lights_status;
                }
            }
            if (requested_instances_selftest) {
                var instances_status = run_instances_selftest();
                if (instances_status != script_status.success) {
                    cleanup();
                    return instances_status;
                }
            }
            if (requested_animation_selftest) {
                var animation_status = run_animation_selftest();
                if (animation_status != script_status.success) {
                    cleanup();
                    return animation_status;
                }
            }
            if (requested_validation_selftest) {
                var validation_status = run_validation_selftest();
                if (validation_status != script_status.success) {
                    cleanup();
                    return validation_status;
                }
            }
            if (requested_animation_mode) {
                // The mode opens empty, so the command line records the animated placement first and
                // then opens it in the preview the way the Timeline's Open button does.
                string? preview_asset = null;
                var preview_clip = 0;
                if (select_first_animatable() &&
                    document?.find(selection_state?.primary ?? Guid.Empty) is { asset: { Length: > 0 } animated } placed) {
                    preview_asset = animated;
                    preview_clip = Math.Max(0, placed.animation_clip);
                }
                if (enter_animation_mode() is { } animation_error) {
                    notice = animation_error;
                    host_logger.write($"Editor: cannot open the animation editor: {animation_error}");
                } else if (preview_asset != null && open_animation_asset(preview_asset, preview_clip) is { } open_error) {
                    notice = open_error;
                    host_logger.write($"Editor: cannot open the animation preview asset: {open_error}");
                }
            }
            if (requested_animation_mode_selftest) {
                var animation_mode_status = animation_mode_selftest_begin();
                if (animation_mode_status != script_status.success) {
                    cleanup();
                    return animation_mode_status;
                }
            }
            if (requested_relocation_selftest) {                var relocation_status = run_relocation_selftest();
                if (relocation_status != script_status.success) {
                    cleanup();
                    return relocation_status;
                }
            }
            if (requested_browser_selftest) {
                var browser_status = run_browser_selftest();
                if (browser_status != script_status.success) {
                    cleanup();
                    return browser_status;
                }
            }
            if (requested_import_selftest != null) {
                var import_status = run_import_selftest(requested_import_selftest);
                if (import_status != script_status.success) {
                    cleanup();
                    return import_status;
                }
            }
            if (requested_selftest) {
                var selftest_status = run_selftest();
                if (selftest_status != script_status.success) {
                    cleanup();
                    return selftest_status;
                }
                asset_selftest_phase = 1;
            }

            camera_input_installed = input_events.try_set_callback(input => {
                if (capture_input_binding(input)) { return; }
                if (input.type == input_type.key && input.key == key_code.f && input.down) {
                    focus_selection_requested = true;
                } else if (input.type == input_type.key && input.key == key_code.escape && input.down &&
                           gizmo.dragging != null) {
                    cancel_gizmo_requested = true;
                }
            });
            instance = this;
            scripts->update = &update;
            scripts->stop = &stop;
            host_logger.write("Editor: ready");
            return script_status.success;
        }

        private bool orient_thumbnail(document_node node) {
            if (tree == null || !tree.try_get_bounds(node.runtime, out var center, out var radius) || radius <= 0 ||
                !orbit_camera.try_get(out var pose)) {
                return false;
            }
            pose.target = center;
            pose.distance = MathF.Max(radius * 2.85f, 0.25f);
            pose.yaw_degrees = node.pipeline == "game_toon" ? 0 : 25;
            pose.pitch_degrees = node.pipeline == "game_toon" ? 0 : -12;
            return pose.try_apply();
        }

        private static scene_document create_default_document() {
            var created = new scene_document();
            // document_node already defaults to a fresh uid, identity transform, visible, no flags.
            created.add_root(new document_node { name = "root" });
            return created;
        }

        [UnmanagedCallersOnly]
        private static script_status update(frame_info* frame) {
            var current = instance;
            if (current == null || frame == null) {
                return script_status.invalid_argument;
            }
            try {
                if (current.runtime != null) {
                    return current.runtime.update(frame);
                }
                if (current.next_ui != null) {
                    // One own-UI frontend, two screens: the scene editor or the Dope Sheet.
                    return requested_scene_screen
                        ? current.run_scene_ui_frame()
                        : current.run_next_ui_frame();
                }
                if (current.playing?.poll(current.output_log.add, out var play_result, out var play_failed) == true) {
                    current.notice = play_result;
                    current.ui?.set_text("btn_play", "Play");
                    if (play_failed) { current.set_dock_tab("tab_log"); }
                }
                if (requested_thumbnail != null) {
                    return script_status.success;
                }
                if (current.thumbnails?.poll(out var thumbnail_result) == true) {
                    current.notice = thumbnail_result;
                    current.refresh_browser();
                }
                if (current.asset_selftest_phase != 0) {
                    var status = current.run_runtime_asset_selftest();
                    if (status != script_status.success) { return status; }
                }
                if (current.panel_selftest_phase != 0) {
                    var status = current.run_panel_selftest();
                    if (status != script_status.success) { return status; }
                }
                if (current.cancel_gizmo_requested) {
                    current.cancel_gizmo_requested = false;
                    current.finish_gizmo(cancel: true);
                }
                current.handle_ui_events();
                current.finish_input_capture();
                if (current.active_dock_tab == "tab_log" && current.ui != null) {
                    current.output_log.refresh(current.ui);
                }
                current.refresh_panels();
                current.update_animation_strip();
            current.animation_mode_toggle_tick();
                {
                    var animation_mode_status = current.animation_mode_selftest_tick();
                    if (animation_mode_status != script_status.success) { return animation_mode_status; }
                }
                current.sync_environment_lanes();
                current.sync_point_lights();
                current.update_viewport_aspect();
                current.layout_gizmo();
                if (current.focus_selection_requested) {
                    current.focus_selection_requested = false;
                    if (!current.pending_new_scene && !current.pending_close && current.pending_open == null && !current.scene_path_open && !current.asset_import_open && current.asset_edit_id == null && current.open_project_panel == project_panel.none) {
                        current.focus_selection();
                    }
                }
                return script_status.success;
            } catch (Exception error) {
                host_logger.write($"Editor: update failed: {error.GetBaseException().Message}");
                return script_status.script_failure;
            }
        }

        [UnmanagedCallersOnly]
        private static script_status stop() {
            if (instance == null) {
                return script_status.success;
            }
            var current = instance;
            instance = null;
            try {
                current.cleanup();
            } catch (Exception error) {
                host_logger.write($"Editor: shutdown failed: {error.GetBaseException().Message}");
                return script_status.script_failure;
            }
            return script_status.success;
        }

        private void handle_ui_events() {
            if (ui == null) {
                return;
            }
            // A bound keeps one busy document from monopolizing a frame.
            for (var count = 0; count < event_budget && ui.try_poll_event(out var input); ++count) {
                if (input.type == rml_event_type.pointer_cancel) {
                    viewport_navigation.cancel_drag();
                    finish_split_drag();
                }
                if (open_project_panel != project_panel.none) {
                    if (open_project_panel == project_panel.scenes) {
                        handle_project_scenes(input);
                    } else if (open_project_panel == project_panel.input_actions) {
                        handle_input_actions(input);
                    } else {
                        handle_project_settings(input);
                    }
                    continue;
                }
                if (asset_edit_id != null) {
                    handle_asset_edit(input);
                    continue;
                }
                if (asset_import_open) {
                    handle_asset_import(input);
                    continue;
                }
                if (scene_path_open) {
                    reset_outliner_drag();
                    if (input.type == rml_event_type.change && input.id == "scene_path") {
                        scene_path_text = input.value;
                        overwrite_path = null;
                        ui.set_text("scene_path_accept", scene_path_saving ? "Save As" : "Open");
                    } else if (input.type == rml_event_type.click) {
                        handle_scene_path(input.id);
                    }
                    continue;
                }
                if (pending_new_scene || pending_close || pending_open != null) {
                    reset_outliner_drag();
                    asset_drag = null;
                    viewport_press = null;
                    viewport_navigation.cancel_drag();
                    if (input.type == rml_event_type.click) { resolve_unsaved(input.id); }
                    continue;
                }
                if (input.type == rml_event_type.change) {
                    if (input.id == "camera_fov") {
                        camera_fov_draft = input.value;
                    } else if (input.id == "detail_animation_clip") {
                        apply_animation_clip(input.value);
                    } else if (light_fields.ContainsKey(input.id)) {
                        light_fields[input.id] = input.value;
                    } else if (audio_fields.ContainsKey(input.id)) {
                        audio_fields[input.id] = input.value;
                    } else if (material_fields.ContainsKey(input.id)) {
                        material_fields[input.id] = input.value;
                    } else if (input.id == rename_field_id) {
                        rename_field = input.value;
                    } else if (input.id == "browser_search") {
                        browser_filter = input.value;
                        reset_browser_interaction();
                        refresh_browser();
                    } else if (input.id == "outliner_search") {
                        outliner_filter = input.value;
                        refresh_panels();
                    } else if (handle_script_edit(input)) {
                        continue;
                    } else if (handle_node_environment_change(input.id, input.value)) {
                        // the per-object environment dropdown applied its own change
                    } else if (details_panel.try_field(input.id, out var field)) {
                        apply_detail_change(field, input.value);
                    } else if (environment_panel.try_change(input.id, input.value, out var lane, out var environment_selection)) {
                        apply_environment_selection(lane, environment_selection);
                    }
                    continue;
                }
                if (handle_outliner_pointer(input) || handle_asset_drag(input) || handle_gizmo(input) ||
                    handle_animation_strip(input) || handle_timeline_pointer(input)) {
                    continue;
                }
                handle_viewport_selection(input);
                viewport_navigation.handle_pointer(input, viewport_height);
                if (input.type == rml_event_type.pointer_down) {
                    if (input.button == 0) {
                        begin_split_drag(input);
                        handle_caption_press(input.id);
                    }
                    continue;
                }
                if (input.type == rml_event_type.pointer_move) {
                    update_split_drag(input);
                    continue;
                }
                if (input.type == rml_event_type.pointer_up) {
                    if (input.button == 0) {
                        finish_split_drag();
                    }
                    continue;
                }
                if (input.type != rml_event_type.click) {
                    continue;
                }
                if (!input.id.StartsWith("asset_", StringComparison.Ordinal)) {
                    last_clicked_asset = string.Empty;
                }
                if (handle_menu_click(input.id)) {
                    continue;
                }
                // Any other click dismisses a lingering notice from the previous menu action.
                notice = string.Empty;
                const string apply_script = "apply_script_param_";
                if (input.id.StartsWith(apply_script, StringComparison.Ordinal)) {
                    apply_script_draft(input.id["apply_".Length..]);
                    continue;
                }
                const string restore_script = "restore_script_param_";
                if (input.id.StartsWith(restore_script, StringComparison.Ordinal)) {
                    restore_script_parameter(input.id["restore_".Length..]);
                    continue;
                }
                switch (input.id) {
                    case "camera_apply_fov":
                    case "camera_main":
                    case "camera_view":
                    case "camera_align":
                        camera_command(input.id);
                        break;
                    case "material_apply":
                    case "material_reset":
                        apply_material_override(input.id == "material_reset");
                        break;
                    case "audio_apply":
                    case "audio_preview":
                    case "audio_stop":
                    case "audio_refresh":
                        handle_audio_source(input.id);
                        break;
                    case "light_apply":
                        apply_point_light();
                        break;
                    case "btn_delete":
                        delete_selected();
                        break;
                    case "output_clear":
                        output_log.clear();
                        break;
                    case "script_refresh":
                        reload_scripts();
                        refresh_panels();
                        break;
                    case "btn_play":
                        toggle_play();
                        break;
                    case "btn_animation":
                        toggle_animation_mode();
                        break;
                    case "btn_save":
                        save_document();
                        break;
                    case "btn_rename":
                        apply_rename(rename_field);
                        break;
                    case "btn_undo":
                        if (undo_history?.try_undo() == true) {
                            refresh_panels(force_details: true);
                        }
                        break;
                    case "btn_redo":
                        if (undo_history?.try_redo() == true) {
                            refresh_panels(force_details: true);
                        }
                        break;
                    case "detail_pipeline":
                        cycle_pipeline();
                        break;
                    case "detail_collision_box":
                    case "detail_trigger_volume":
                        toggle_volume(solid: input.id == "detail_collision_box");
                        break;
                    case "detail_moving_solid":
                        toggle_moving_solid();
                        break;
                    case "detail_animation_autoplay":
                        toggle_animation_autoplay();
                        break;
                    case "detail_trigger_target":
                        toggle_trigger_target();
                        break;
                    case "detail_visible":
                        toggle_selected_visibility();
                        break;
                    case "win_minimize":
                        window_control.try_minimize();
                        break;
                    case "win_maximize":
                        window_control.try_toggle_maximize();
                        break;
                    case "win_close":
                        close_window();
                        break;
                    case "browser_up":
                    case "browser_root":
                    case "browser_prev":
                    case "browser_next":
                        change_browser_location(input.id);
                        break;
                    case "browser_edit_asset":
                        show_asset_edit();
                        break;
                    case "browser_import":
                        show_asset_import();
                        break;
                    case "browser_refresh":
                        refresh_asset_library();
                        break;
                    case "browser_thumbnail":
                        if (browser == null || browser.selected_asset.Length == 0) {
                            notice = "Select an asset to render a thumbnail";
                        } else if (thumbnails != null) {
                            thumbnails.begin(browser.selected_asset, out notice);
                        }
                        break;
                    case "tab_content":
                    case "tab_log":
                    case "tab_timeline":
                        set_dock_tab(input.id);
                        break;
                    case "vp_space":
                        gizmo_local = !gizmo_local;
                        ui.set_text("vp_space", gizmo_local ? "Local" : "World");
                        break;
                    case "vp_focus":
                        focus_selection_requested = true;
                        break;
                    case "vp_select":
                    case "vp_move":
                    case "vp_rotate":
                    case "vp_scale":
                        set_active_tool(input.id);
                        break;
                    default:
                        // The outliner visibility glyph, an asset tile and a node row all share this path.
                        var logged_node = output_log.node_for(input.id);
                        if (logged_node != Guid.Empty && document?.find(logged_node) is { } logged) {
                            selection_state?.select(logged.uid);
                            reveal_in_outliner(logged);
                            refresh_panels(force_details: true);
                        } else if (!handle_outliner_control(input.id)) {
                            handle_asset_click(input.id, Stopwatch.GetTimestamp());
                        }
                        break;
                }
            }
        }

        /// <summary>
        /// Handles the menu bar and its dropdowns. A click on a menu title toggles its dropdown; a click
        /// on an entry runs the matching toolbar action, closes every dropdown and reports success so the
        /// caller does not fall through to the row/tile dispatch.
        /// </summary>
        private bool handle_menu_click(string id) {
            if (ui == null) {
                return false;
            }
            if (id == "btn_new") {
                toggle_menu("menu_add_items");
                return true;
            }
            if (id is "menu_file" or "menu_edit" or "menu_window" or "menu_help") {
                toggle_menu($"{id}_items");
                return true;
            }
            switch (id) {
                case "menu_add_node":
                    add_node();
                    break;
                case "menu_add_scene_instance":
                    show_project_scenes(for_instance: true);
                    break;
                case "menu_add_audio_source":
                    add_node(audio_source: true);
                    break;
                case "menu_add_collision_box":
                    if (orbit_camera.try_get(out var collision_view)) {
                        add_node(position: collision_view.target, collision_box: true);
                    }
                    break;
                case "menu_add_box_trigger":
                    if (orbit_camera.try_get(out var trigger_view)) {
                        add_node(position: trigger_view.target, box_trigger: true);
                    }
                    break;
                case "menu_add_camera":
                    add_camera();
                    break;
                case "menu_add_point_light":
                    add_point_light();
                    break;
                case "menu_file_new":
                    new_scene();
                    break;
                case "menu_file_open":
                    show_project_scenes();
                    break;
                case "menu_file_open_path":
                    show_scene_path(saving: false);
                    break;
                case "menu_file_project_settings":
                    show_project_settings();
                    break;
                case "menu_file_save_as":
                    show_scene_path(saving: true);
                    break;
                case "menu_file_save":
                    save_document();
                    break;
                case "menu_edit_undo":
                    if (undo_history?.try_undo() == true) {
                        refresh_panels(force_details: true);
                    }
                    break;
                case "menu_edit_redo":
                    if (undo_history?.try_redo() == true) {
                        refresh_panels(force_details: true);
                    }
                    break;
                case "menu_edit_duplicate":
                    duplicate_selection();
                    break;
                case "menu_edit_delete":
                    delete_selected();
                    break;
                case "menu_edit_rename":
                    apply_rename(rename_field);
                    break;
                case "menu_window_outliner":
                    toggle_panel("outliner", "flex", ref outliner_shown);
                    break;
                case "menu_window_details":
                    toggle_panel("details", "block", ref details_shown);
                    break;
                case "menu_window_browser":
                    toggle_panel("bottom_dock", "flex", ref dock_shown);
                    break;
                case "menu_window_sidebar_side":
                    set_workspace_layout(layout with { sidebar_left = !layout.sidebar_left });
                    break;
                case "menu_window_dock_side":
                    set_workspace_layout(layout with { dock_top = !layout.dock_top });
                    break;
                case "menu_window_panel_order":
                    set_workspace_layout(layout with { details_first = !layout.details_first });
                    break;
                case "menu_window_reset_layout":
                    set_workspace_layout(new workspace_layout());
                    break;
                case "menu_help_about":
                    notice = "deren editor - managed scene editor for deren";
                    refresh_panels();
                    break;
                default:
                    close_menus();
                    return false;
            }
            close_menus();
            return true;
        }

        private void toggle_menu(string items_id) {
            var open = open_menu != items_id;
            close_menus();
            if (open) {
                open_menu = items_id;
                ui?.set_visible(items_id, true);
            }
        }

        private void close_menus() {
            for (var index = 0; index < menu_dropdown_ids.Length; index++) {
                ui?.set_visible(menu_dropdown_ids[index], false);
            }
            open_menu = string.Empty;
        }

        private static readonly string[] menu_dropdown_ids =
            ["menu_file_items", "menu_edit_items", "menu_window_items", "menu_help_items", "menu_add_items"];

        private void set_workspace_layout(workspace_layout value) {
            active_splitter = string.Empty;
            layout = value;
            apply_workspace_layout();
            save_workspace_layout();
        }

        private void apply_workspace_layout() {
            if (ui == null) {
                return;
            }
            ui.try_set_element_style("main", "flex-direction", layout.sidebar_left ? "row-reverse" : "row");
            ui.try_set_element_style("left_area", "flex-direction", layout.dock_top ? "column-reverse" : "column");
            ui.try_set_element_style("right_column", "flex-direction", layout.details_first ? "column-reverse" : "column");
            set_panel_size("right_column", "width", layout.sidebar_width);
            set_panel_size("bottom_dock", "height", layout.dock_height);
            set_panel_size("outliner", "height", layout.outliner_height);
            ui.set_text("menu_window_sidebar_side", layout.sidebar_left ? "Move Sidebar Right" : "Move Sidebar Left");
            ui.set_text("menu_window_dock_side", layout.dock_top ? "Move Dock to Bottom" : "Move Dock to Top");
            ui.set_text("menu_window_panel_order", layout.details_first ? "Place Outliner First" : "Place Details First");
        }

        private void save_workspace_layout() {
            if (layout_path.Length != 0) {
                layout.save(layout_path);
            }
        }

        private void finish_split_drag() {
            if (active_splitter.Length != 0) {
                active_splitter = string.Empty;
                save_workspace_layout();
            }
        }

        private void set_panel_size(string id, string property, float size) =>
            ui?.try_set_element_style(id, property, string.Create(CultureInfo.InvariantCulture, $"{size:0.#}dp"));

        /// <summary>Shows or hides a panel from the Window menu without leaving its layout space behind.</summary>
        private void toggle_panel(string id, string display, ref bool shown) {
            if (ui == null) {
                return;
            }
            shown = !shown;
            // `display` is the element's own display type from editor.rml (outliner: flex, details:
            // block); restoring anything else silently changes its layout once it comes back.
            ui.try_set_element_style(id, "display", shown ? display : "none");
            sync_splitter_visibility();
        }

        /// <summary>
        /// A splitter only exists while both panels it separates are visible: a handle for a hidden
        /// panel still grabs the pointer and resizes something the user cannot see.
        /// </summary>
        private void sync_splitter_visibility() {
            if (ui == null) {
                return;
            }
            var outliner_splitter = outliner_shown && details_shown;
            ui.try_set_element_style("split_outliner", "display", outliner_splitter ? "block" : "none");
            ui.try_set_element_style("split_bottom", "display", dock_shown ? "block" : "none");
        }

        /// <summary>Starts over with one empty root node; the activated tree is reused, never re-activated.</summary>
        private bool new_scene(bool discard_unsaved = false) {
            if (document == null || tree == null) {
                return false;
            }
            if (document.dirty && !discard_unsaved) {
                pending_new_scene = true;
                active_splitter = string.Empty;
                close_menus();
                ui?.set_text("unsaved_message", "Save your changes before creating a new scene?");
                ui?.try_set_element_style("unsaved_prompt", "display", "flex");
                return false;
            }
            // BUILD THE NEW SCENE BEFORE DESTROYING THE OLD ONE. The runtime nodes of the two scenes are
            // independent, so the new subtree can be created while the old one still exists; the old
            // roots are only destroyed once the new scene is known to work. The other order loses the
            // old scene when the create fails and has nothing to fall back to.
            var previous = document;
            var next = create_default_document();
            document = next;
            if (!create_subtree(null, next.roots[0])) {
                // Drop whatever runtime nodes the failed attempt created and keep the old document,
                // whose runtime counterpart is still alive.
                for (var index = 0; index < next.roots.Count; ++index) {
                    if (next.roots[index].runtime.is_valid) {
                        destroy_runtime(next.roots[index]);
                    }
                }
                document = previous;
                notice = "Could not create the new scene";
                refresh_panels(force_details: true);
                return false;
            }
            var kept = new List<document_node>();
            for (var index = 0; index < previous.roots.Count; ++index) {
                var root = previous.roots[index];
                if (!destroy_runtime(root) && root.runtime.is_valid) {
                    kept.Add(root);
                }
            }
            // A node whose runtime counterpart refused to be destroyed stays in the document: dropping
            // it would leave a live scene node the editor can no longer reach.
            for (var index = 0; index < kept.Count; ++index) {
                next.insert_root(next.roots.Count, kept[index]);
            }
            if (kept.Count > 0) {
                host_logger.write($"Editor: new scene kept {kept.Count} node(s) whose runtime nodes could not be destroyed");
                notice = $"Could not discard {kept.Count} node(s)";
            }
            selection_state?.clear();
            undo_history?.clear();
            browser?.clear_selection();
            next.mark_dirty();
            refresh_panels(force_details: true);
            return true;
        }

        private bool resolve_unsaved(string choice) {
            if ((!pending_new_scene && !pending_close && pending_open == null) ||
                choice is not ("unsaved_save" or "unsaved_discard" or "unsaved_cancel")) {
                return false;
            }
            if (choice == "unsaved_save" && !save_document()) {
                ui?.set_text("unsaved_message", "Save failed. Your scene is unchanged. Retry, discard, or cancel.");
                return false;
            }
            var closing = pending_close;
            var opening = pending_open;
            pending_open = null;
            pending_new_scene = false;
            pending_close = false;
            last_clicked_asset = string.Empty;
            ui?.try_set_element_style("unsaved_prompt", "display", "none");
            if (choice == "unsaved_cancel") {
                // Cancel is a handled answer: nothing changes, and the caller reports the prompt resolved.
                return true;
            }
            if (opening != null) {
                return replace_scene(opening);
            }
            return closing ? window_control.try_close() : new_scene(discard_unsaved: true);
        }

        /// <summary>
        /// The caption close button: a dirty scene asks the unsaved-changes question first, exactly like
        /// New Scene; a clean one goes straight to the window's close request, and the app still decides
        /// whether that ends the run.
        /// </summary>
        private void close_window() {
            if (document?.dirty != true || tree == null) {
                window_control.try_close();
                return;
            }
            pending_close = true;
            active_splitter = string.Empty;
            close_menus();
            ui?.set_text("unsaved_message", "Save your changes before closing?");
            ui?.try_set_element_style("unsaved_prompt", "display", "flex");
        }

        /// <summary>
        /// A press inside the caption strip (the menu bar's empty area, the brand block, the project
        /// label) is the borderless replacement for the system title bar: the first press hands the move
        /// loop to the system, a second press inside the double-click window toggles maximise. Clicks on
        /// the menus and the window buttons resolve to their own ids, so they never reach here.
        /// </summary>
        private void handle_caption_press(string id) {
            if (id is not ("menubar" or "brand" or "project_label")) {
                caption_pressed = false;
                return;
            }
            if (open_menu.Length != 0) {
                close_menus();
                caption_pressed = false;
                return;
            }
            var now = Stopwatch.GetTimestamp();
            var double_click = caption_pressed && now >= last_caption_press &&
                Stopwatch.GetElapsedTime(last_caption_press, now).TotalSeconds <= 0.5;
            caption_pressed = !double_click;
            last_caption_press = now;
            if (double_click) {
                window_control.try_toggle_maximize();
            } else {
                // Blocks until the pointer button is released: the system owns the move loop.
                window_control.try_begin_drag();
            }
        }

        private bool handle_asset_click(string id, long timestamp) {
            if (browser == null || !content_browser_panel.try_tile_index(id, out var tile)) { return false; }
            if (browser.try_open_folder(tile)) {
                reset_browser_interaction();
                return true;
            }
            if (!browser.try_select_tile(tile)) { return false; }
            var asset = browser.selected_asset;
            notice = $"asset: {asset}";
            refresh_browser();
            refresh_panels();
            var activate = asset == last_clicked_asset && timestamp >= last_asset_click &&
                Stopwatch.GetElapsedTime(last_asset_click, timestamp).TotalMilliseconds <= 500;
            last_clicked_asset = activate ? string.Empty : asset;
            last_asset_click = timestamp;
            if (activate) { add_node(asset); }
            return true;
        }

        /// <summary>Highlights one of the four viewport tool buttons (the UE transform widget mode).</summary>
        private void set_active_tool(string id) {
            active_tool = id;
            apply_tool_style();
            notice = $"tool: {id["vp_".Length..]}";
            refresh_panels();
        }

        private void apply_tool_style() {
            if (ui == null) {
                return;
            }
            for (var index = 0; index < tool_ids.Length; index++) {
                var active = tool_ids[index] == active_tool;
                ui.try_set_element_style(tool_ids[index], "background-color", active ? "#0070e0" : "#2a2a2a");
                ui.try_set_element_style(tool_ids[index], "color", active ? "#ffffff" : "#c0c0c0");
            }
        }

        private bool add_node(string? asset_id = null, vector_3? position = null, point_light_component? light = null,
                              transform_value? camera_placement = null, float camera_fov = scene_cameras.default_fov, string? subscene = null,
                              bool box_trigger = false, bool audio_source = false, bool collision_box = false) {
            if (document == null || tree == null) {
                return false;
            }
            var parent = asset_id == null && light == null && camera_placement == null && subscene == null && !box_trigger && !audio_source && !collision_box && selection_state != null && selection_state.primary != Guid.Empty
                ? document.find(selection_state.primary)
                : null;
            if (parent?.subscene != null) { parent = null; }
            bool camera = camera_placement.HasValue;
            var node = new document_node {
                name = subscene != null ? Path.GetFileName(subscene)[..^".scene.toml".Length] :
                    audio_source ? "Audio Source" : collision_box ? "Collision Box" : box_trigger ? "Box Trigger" : camera ? "Camera" : light != null ? "Point Light" : asset_id ?? "node",
                subscene = subscene,
                box_trigger = box_trigger,
                collision_box = collision_box,
                audio_source = audio_source ? audio_source_component.unassigned : null,
                asset = asset_id, light = light, camera = camera,
                camera_fov = scene_cameras.valid_fov(camera_fov) ? camera_fov : scene_cameras.default_fov,
            };
            if (camera_placement is { } transform) { node.local = transform; }
            if (position is vector_3 placement) {
                node.local.position = placement;
            }
            if (subscene != null) {
                if (!scene_instances.try_populate(managed_bootstrap.project_directory, [node], document.path, out var error)) {
                    notice = error ?? "Could not load scene instance";
                    return false;
                }
                if (scene_lights.count(document.roots) + scene_lights.count([node]) > scene_lighting.max_point_lights) {
                    notice = $"A scene supports at most {scene_lighting.max_point_lights} point lights";
                    return false;
                }
            }
            var previous_selection = selection_state?.primary ?? Guid.Empty;
            var index = parent != null ? parent.children.Count : document.roots.Count;
            if (!insert_document_node(parent, index, node) || !create_subtree(parent, node)) {
                // Best effort: drop whatever runtime nodes the failed subtree already created.
                if (node.runtime.is_valid) { destroy_runtime(node); }
                remove_document_node(parent, node);
                if (asset_id != null) { notice = $"Cannot load asset: {asset_id}"; }
                if (subscene != null) { notice = $"Cannot add '{subscene}'. Check the Output Log or the engine log."; }
                refresh_panels();
                return false;
            }
            // Both branches select the new node: a created node that the outliner does not show as
            // selected looks like the click did nothing, and every later edit would target the old one.
            selection_state?.select(node.uid);
            reveal_in_outliner(node);
            if (asset_id != null) {
                notice = $"Added asset: {asset_id}";
            }
            document.mark_dirty();
            record_undo(subscene != null ? "Add Scene Instance" : audio_source ? "Add Audio Source" : collision_box ? "Add Collision Box" : box_trigger ? "Add Box Trigger" : camera ? "Add Camera" : light != null ? "Add Point Light" : asset_id == null ? "Create" : "Add Asset",
                () => {
                    // The document goes first so a refused destroy cannot orphan a runtime node.
                    if (!remove_document_node(parent, node)) { return false; }
                    if (!destroy_runtime(node)) {
                        if (!insert_document_node(parent, index, node)) {
                            host_logger.write($"Editor: lost node '{node.name}' while aborting an undo");
                        }
                        return false;
                    }
                    if (document.find(previous_selection) != null) { selection_state?.select(previous_selection); }
                    else { selection_state?.clear(); }
                    mark_dirty_and_refresh();
                    return true;
                },
                () => {
                    if (!insert_document_node(parent, index, node) || !create_subtree(parent, node)) {
                        if (node.runtime.is_valid) { destroy_runtime(node); }
                        remove_document_node(parent, node);
                        notice = $"Cannot recreate node: {node.name}";
                        refresh_panels(force_details: true);
                        return false;
                    }
                    selection_state?.select(node.uid);
                    mark_dirty_and_refresh();
                    return true;
                });
            refresh_panels(force_details: true);
            return true;
        }

        private bool delete_selected() {
            if (document == null || selection_state == null) {
                return false;
            }
            var uid = selection_state.primary;
            if (uid == Guid.Empty) {
                return false;
            }
            var node = document.find(uid);
            if (node == null || !locate_node(node, out var parent, out var index)) {
                return false;
            }
            // REMOVE FROM THE DOCUMENT BEFORE DESTROYING THE RUNTIME NODE. The document removal is the
            // only step that can still be taken back cheaply, so it happens first: a failed destroy can
            // then put the node back exactly where it was instead of leaving a document node whose
            // runtime counterpart is already gone.
            if (!remove_document_node(parent, node)) {
                return false;
            }
            if (!destroy_runtime(node)) {
                if (!insert_document_node(parent, index, node)) {
                    host_logger.write($"Editor: lost node '{node.name}' while aborting a delete");
                }
                refresh_panels();
                return false;
            }
            document.mark_dirty();
            record_undo("Delete",
                () => {
                    // THE UNDO OF A DELETE IS A CREATE, and a create can fail (the asset the node
                    // names may be gone). Reporting it keeps the entry on the undo side instead of
                    // leaving a node in the document that the scene has no runtime counterpart for.
                    if (!insert_document_node(parent, index, node) || !create_subtree(parent, node)) {
                        if (node.runtime.is_valid) { destroy_runtime(node); }
                        remove_document_node(parent, node);
                        notice = $"Cannot restore node: {node.name}";
                        refresh_panels(force_details: true);
                        return false;
                    }
                    mark_dirty_and_refresh();
                    return true;
                },
                () => {
                    // Same order as the original delete: the document goes first so a refused destroy
                    // cannot orphan a runtime node, and the node is put back at its captured index.
                    if (!remove_document_node(parent, node)) {
                        return false;
                    }
                    if (!destroy_runtime(node)) {
                        if (!insert_document_node(parent, index, node)) {
                            host_logger.write($"Editor: lost node '{node.name}' while aborting a delete");
                        }
                        return false;
                    }
                    mark_dirty_and_refresh();
                    return true;
                });
            selection_state.clear();
            refresh_panels();
            return true;
        }

        private bool save_document(string? destination = null) {
            if (document == null) {
                return false;
            }
            if (animation_mode) {
                notice = "The animation preview scene is not saved; leave the animation editor first.";
                return false;
            }
            var path = destination ?? document.path ??
                Path.Combine(managed_bootstrap.project_directory, "scenes", default_scene_file);
            try {
                if (scene_instances.references_path(managed_bootstrap.project_directory, document.roots, Path.GetFullPath(path))) {
                    notice = "Cannot save over a source scene used by this document.";
                    return false;
                }
                var directory = Path.GetDirectoryName(path);
                if (!string.IsNullOrEmpty(directory)) {
                    Directory.CreateDirectory(directory);
                }
            } catch (Exception error) {
                notice = $"Could not create the scene directory: {error.GetBaseException().Message}";
                host_logger.write($"Editor: {notice}");
                return false;
            }
            // document_io.try_save writes the file and marks the document saved itself.
            if (!document_io.try_save(document, path, out var save_error)) {
                notice = save_error ?? "Could not save the scene.";
                host_logger.write($"Editor: cannot save '{path}': {notice}");
                return false;
            }
            refresh_panels();
            host_logger.write($"Editor: saved {count_nodes(document.roots)} node(s) to '{path}'");
            return true;
        }

        /// <summary>The rename path shared by #btn_rename and --editor-selftest.</summary>
        private bool apply_rename(string name) {
            if (document == null || selection_state == null) {
                return false;
            }
            var node = selection_state.primary == Guid.Empty ? null : document.find(selection_state.primary);
            if (node == null) {
                return false;
            }
            var trimmed = name.Trim();
            if (trimmed.Length == 0 || trimmed == node.name) {
                return false;
            }
            // The runtime rename entry counts UTF-8 bytes, not characters, and refuses anything over
            // the shared limit. Validate here so the document never holds a name the scene cannot show.
            var bytes = scene_sync.name_utf8_byte_count(trimmed);
            if (bytes > scene_sync.max_name_utf8_bytes) {
                notice = $"Cannot rename: {bytes} UTF-8 bytes, the limit is {scene_sync.max_name_utf8_bytes}";
                refresh_panels(force_details: true);
                return false;
            }
            var previous = node.name;
            node.name = trimmed;
            if (!push_runtime(node)) {
                node.name = previous;
                return false;
            }
            document.mark_dirty();
            record_undo("Rename",
                () => apply_node_name(node, previous),
                () => apply_node_name(node, trimmed));
            rename_field = string.Empty;
            ui?.set_value(rename_field_id, string.Empty);
            refresh_panels(force_details: true);
            return true;
        }

        /// <summary>
        /// Applies one details row: parse, mutate the document node, push it onto the runtime node and
        /// record a snapshot undo command. A refused value redraws the row from the document so the
        /// input never keeps text the document rejected.
        /// </summary>
        private bool apply_detail_change(detail_field field, string text) {
            if (document == null || selection_state == null) {
                return false;
            }
            var node = selection_state.primary == Guid.Empty ? null : document.find(selection_state.primary);
            if (node == null) {
                return false;
            }
            var before = new node_snapshot(node);
            var changed = false;
            switch (field) {
                case detail_field.name: {
                    var trimmed = text.Trim();
                    if (trimmed.Length != 0 && trimmed != node.name) {
                        // Same UTF-8 byte limit as the rename field; the details row must not accept a
                        // name the runtime would refuse while the document keeps it.
                        var name_bytes = scene_sync.name_utf8_byte_count(trimmed);
                        if (name_bytes > scene_sync.max_name_utf8_bytes) {
                            notice = $"Cannot rename: {name_bytes} UTF-8 bytes, the limit is {scene_sync.max_name_utf8_bytes}";
                            break;
                        }
                        node.name = trimmed;
                        changed = true;
                    }
                    break;
                }
                case detail_field.flags: {
                    if (uint.TryParse(text.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var flags) &&
                        flags != node.flags) {
                        node.flags = flags;
                        changed = true;
                    }
                    break;
                }
                default: {
                    if (!float.TryParse(text.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out var number) ||
                        !float.IsFinite(number)) {
                        break;
                    }
                    var local = node.local;
                    changed = set_lane(ref local, field, number);
                    if (changed) {
                        node.local = local;
                    }
                    break;
                }
            }
            if (!changed) {
                details?.refresh(document, selection_state, true);
                return false;
            }
            if (!push_runtime(node)) {
                restore_node(node, before, mark_dirty: false);
                details?.refresh(document, selection_state, true);
                return false;
            }
            var after = new node_snapshot(node);
            document.mark_dirty();
            record_undo(details_panel.label(field),
                () => restore_node(node, before, mark_dirty: true),
                () => restore_node(node, after, mark_dirty: true));
            details?.refresh(document, selection_state, true);
            refresh_panels();
            return true;
        }

        /// <summary>Writes one transform lane, returning false when the value was already there.</summary>
        private static bool set_lane(ref transform_value local, detail_field field, float value) {
            switch (field) {
                case detail_field.position_x: return set_axis(ref local.position.x, value);
                case detail_field.position_y: return set_axis(ref local.position.y, value);
                case detail_field.position_z: return set_axis(ref local.position.z, value);
                case detail_field.rotation_x: return set_axis(ref local.rotation_degrees.x, value);
                case detail_field.rotation_y: return set_axis(ref local.rotation_degrees.y, value);
                case detail_field.rotation_z: return set_axis(ref local.rotation_degrees.z, value);
                case detail_field.scale_x: return set_axis(ref local.scale.x, value);
                case detail_field.scale_y: return set_axis(ref local.scale.y, value);
                case detail_field.scale_z: return set_axis(ref local.scale.z, value);
                default: return false;
            }
        }

        private static bool set_axis(ref float lane, float value) {
            if (lane == value) {
                return false;
            }
            lane = value;
            return true;
        }

        /// <summary>Flips the selected node's visibility through the same undo path as the details rows.</summary>
        private bool toggle_selected_visibility() {
            if (document == null || selection_state == null) {
                return false;
            }
            var node = selection_state.primary == Guid.Empty ? null : document.find(selection_state.primary);
            return node != null && set_node_visibility(node, !node.visible);
        }

        /// <summary>Writes one visibility flip to the document and the runtime node, recording undo.</summary>
        private bool set_node_visibility(document_node node, bool visible) {
            if (document == null) {
                return false;
            }
            var before = new node_snapshot(node);
            node.visible = visible;
            if (!push_runtime(node)) {
                restore_node(node, before, mark_dirty: false);
                return false;
            }
            var after = new node_snapshot(node);
            document.mark_dirty();
            record_undo(visible ? "Show Node" : "Hide Node",
                () => restore_node(node, before, mark_dirty: true),
                () => restore_node(node, after, mark_dirty: true));
            refresh_panels(force_details: true);
            return true;
        }

        /// <summary>
        /// Writes one name onto the document node and the runtime node. When the runtime refuses the
        /// name the document is put back, so the outliner never shows a label the scene does not have.
        /// </summary>
        private bool apply_node_name(document_node node, string name) {
            var previous = node.name;
            node.name = name;
            if (!push_runtime(node)) {
                node.name = previous;
                return false;
            }
            mark_dirty_and_refresh();
            return true;
        }

        /// <summary>Writes a snapshot back onto the document node and the runtime node.</summary>
        private bool restore_node(document_node node, node_snapshot snapshot, bool mark_dirty) {
            var previous = new node_snapshot(node);
            node.name = snapshot.name;
            node.local = snapshot.local;
            node.visible = snapshot.visible;
            node.flags = snapshot.flags;
            node.script = snapshot.script;
            node.environment = snapshot.environment;
            if (!push_runtime(node)) {
                // THE RUNTIME REFUSED THE SNAPSHOT: putting the document back keeps the two sides
                // equal, and returning false keeps the history entry where it was.
                node.name = previous.name;
                node.local = previous.local;
                node.visible = previous.visible;
                node.flags = previous.flags;
                node.script = previous.script;
                node.environment = previous.environment;
                return false;
            }
            if (mark_dirty) {
                mark_dirty_and_refresh();
            }
            return true;
        }

        /// <summary>The editable values of one node, captured before and after an edit.</summary>
        private sealed class node_snapshot {
            internal node_snapshot(document_node node) {
                name = node.name;
                local = node.local;
                visible = node.visible;
                flags = node.flags;
                script = node.script;
                environment = node.environment;
            }

            internal readonly string name;
            internal readonly transform_value local;
            internal readonly bool visible;
            internal readonly uint flags;
            internal readonly string? script;
            internal readonly int environment;
        }

        private void record_undo(string label, Func<bool> undo, Func<bool> redo) {
            undo_history?.push(new delegate_undo_command(label, undo, redo));
        }

        private void mark_dirty_and_refresh() {
            document?.mark_dirty();
            refresh_panels(force_details: true);
        }

        /// <summary>
        /// Parent of <paramref name="node"/> in one dictionary lookup, or null for a root. The map is
        /// built from the current document once and dropped whenever the structure changes, so the
        /// per-frame caller never pays for a tree scan.
        /// </summary>
        private bool try_parent(document_node node, out document_node? parent) {
            if (document == null) {
                parent = null;
                return false;
            }
            if (!ReferenceEquals(parents_document, document)) {
                parents.Clear();
                parents_document = document;
                collect_parents(document.roots, null);
            }
            return parents.TryGetValue(node, out parent);
        }

        private void collect_parents(IReadOnlyList<document_node> nodes, document_node? parent) {
            foreach (var node in nodes) {
                parents[node] = parent;
                if (node.children.Count != 0) {
                    collect_parents(node.children, node);
                }
            }
        }

        private void forget_parents() {
            parents.Clear();
            parents_document = null;
        }

        private bool insert_document_node(document_node? parent, int index, document_node node) {
            if (document == null) {
                return false;
            }
            if (parent != null) {
                parent.children.Insert(Math.Clamp(index, 0, parent.children.Count), node);
                forget_parents();
                return true;
            }
            document.insert_root(index, node);
            forget_parents();
            return true;
        }

        private bool remove_document_node(document_node? parent, document_node node) {
            if (document == null) {
                return false;
            }
            var removed = parent != null ? parent.children.Remove(node) : document.remove_root(node);
            if (removed) {
                forget_parents();
            }
            return removed;
        }

        /// <summary>Creates the runtime subtree top-down: a parent must exist before its children.</summary>
        private bool create_subtree(document_node? parent, document_node node) {
            if (!create_runtime(parent, node) || !push_runtime(node)) {
                return false;
            }
            for (var index = 0; index < node.children.Count; ++index) {
                if (!create_subtree(node, node.children[index])) {
                    return false;
                }
            }
            return true;
        }

        private bool create_runtime(document_node? parent, document_node node) {
            if (document == null || tree == null) {
                return false;
            }
            if (!scene_sync.create_one(document, tree, parent, node, out var status)) {
                host_logger.write($"Editor: cannot create node '{node.name}' in the scene ({status})");
                return false;
            }
            return true;
        }

        private bool destroy_runtime(document_node node) {
            if (document == null || tree == null) {
                return false;
            }
            if (!scene_sync.destroy_one(document, tree, node, out var status)) {
                host_logger.write($"Editor: cannot destroy node '{node.name}' in the scene ({status})");
                return false;
            }
            return true;
        }

        private bool push_runtime(document_node node) {
            if (document == null || tree == null) {
                return false;
            }
            if (!scene_sync.push(document, tree, node, out var status)) {
                host_logger.write($"Editor: cannot push node '{node.name}' to the scene ({status})");
                return false;
            }
            return true;
        }

        private bool locate_node(document_node target, out document_node? parent, out int index) {
            parent = null;
            index = -1;
            if (document == null) {
                return false;
            }
            for (var root_index = 0; root_index < document.roots.Count; ++root_index) {
                if (document.roots[root_index] == target) {
                    index = root_index;
                    return true;
                }
            }
            return locate_in_children(document.roots, target, ref parent, ref index);
        }

        private static bool locate_in_children(IReadOnlyList<document_node> nodes, document_node target,
                                               ref document_node? parent, ref int index) {
            for (var node_index = 0; node_index < nodes.Count; ++node_index) {
                var node = nodes[node_index];
                for (var child_index = 0; child_index < node.children.Count; ++child_index) {
                    if (node.children[child_index] == target) {
                        parent = node;
                        index = child_index;
                        return true;
                    }
                }
                if (locate_in_children(node.children, target, ref parent, ref index)) {
                    return true;
                }
            }
            return false;
        }

        /// <summary>
        /// The viewport image is the whole window render stretched into the viewport element, so the
        /// scene's projection must be built for that element's shape: a 16:9 projection squeezed into a
        /// taller box would stretch the world. Element rectangles are dp, and the aspect is unitless, so
        /// the render scale cancels out; the aspect is re-applied only when the element changes shape.
        /// </summary>
        private void update_viewport_aspect() {
            if (ui == null || !ui.try_get_element_rect("viewport", out var viewport) ||
                viewport.width < 64.0f || viewport.height < 64.0f) {
                return;
            }
            viewport_height = viewport.height;
            var aspect = viewport.width / viewport.height;
            if (MathF.Abs(aspect - applied_aspect) < 0.0005f) {
                return;
            }
            if (camera_projection.try_set_aspect(aspect)) {
                applied_aspect = aspect;
            }
        }

        /// <summary>
        /// Captures a splitter handle on pointer down: remembers where the pointer started, how large the
        /// panel it resizes currently is, and the range the drag may move it to. The neighbours keep their
        /// own minimum sizes, so the range depends on the container's current extent.
        /// </summary>
        private void begin_split_drag(rml_event input) {
            active_splitter = string.Empty;
            if (ui == null) {
                return;
            }
            switch (input.id) {
                case "split_right":
                    if (ui.try_get_element_rect("right_column", out var right) &&
                        ui.try_get_element_rect("left_area", out var left)) {
                        active_splitter = input.id;
                        drag_origin = input.x;
                        drag_start_size = right.width;
                        drag_min_size = 240.0f;
                        drag_max_size = MathF.Max(drag_min_size, left.width + right.width - 240.0f);
                    }
                    break;
                case "split_outliner":
                    if (ui.try_get_element_rect("outliner", out var outliner_rect) &&
                        ui.try_get_element_rect("right_column", out var column)) {
                        active_splitter = input.id;
                        drag_origin = input.y;
                        drag_start_size = outliner_rect.height;
                        drag_min_size = 120.0f;
                        drag_max_size = MathF.Max(drag_min_size, column.height - 200.0f);
                    }
                    break;
                case "split_bottom":
                    if (ui.try_get_element_rect("bottom_dock", out var dock) &&
                        ui.try_get_element_rect("main", out var main_area)) {
                        active_splitter = input.id;
                        drag_origin = input.y;
                        drag_start_size = dock.height;
                        drag_min_size = 120.0f;
                        drag_max_size = MathF.Max(drag_min_size, main_area.height - 200.0f);
                    }
                    break;
            }
        }

        /// <summary>
        /// Moves the captured splitter with the pointer. A right-hand splitter grows the panel when the
        /// pointer moves left, while the two horizontal handles grow their panel when it moves down.
        /// </summary>
        private void update_split_drag(rml_event input) {
            if (ui == null || active_splitter.Length == 0) {
                return;
            }
            float size;
            string panel;
            string property;
            switch (active_splitter) {
                case "split_right":
                    size = drag_start_size + (input.x - drag_origin) * (layout.sidebar_left ? 1 : -1);
                    panel = "right_column";
                    property = "width";
                    break;
                case "split_outliner":
                    size = drag_start_size + (input.y - drag_origin) * (layout.details_first ? -1 : 1);
                    panel = "outliner";
                    property = "height";
                    break;
                case "split_bottom":
                    size = drag_start_size + (input.y - drag_origin) * (layout.dock_top ? 1 : -1);
                    panel = "bottom_dock";
                    property = "height";
                    break;
                default:
                    return;
            }
            size = Math.Clamp(size, drag_min_size, drag_max_size);
            set_panel_size(panel, property, size);
            layout = active_splitter switch {
                "split_right" => layout with { sidebar_width = size },
                "split_outliner" => layout with { outliner_height = size },
                _ => layout with { dock_height = size },
            };
        }

        /// <summary>
        /// Switches the bottom dock between the Content Browser, the Output Log and the animation
        /// editor's Timeline. The Timeline tab only exists while the animation editor is open.
        /// </summary>
        private void set_dock_tab(string tab) {
            if (ui == null || tab == active_dock_tab) {
                return;
            }
            if (tab is not ("tab_content" or "tab_log" or "tab_timeline")) {
                return;
            }
            if (tab == "tab_timeline" && !animation_mode) {
                return;
            }
            active_dock_tab = tab;
            var browser = tab == "tab_content";
            var log = tab == "tab_log";
            var timeline = tab == "tab_timeline";
            ui.set_visible("browser_body", browser);
            ui.set_visible("log_body", log);
            ui.set_visible("timeline_body", timeline);
            ui.try_set_element_style("browser_body", "display", browser ? "flex" : "none");
            ui.try_set_element_style("log_body", "display", log ? "flex" : "none");
            ui.try_set_element_style("timeline_body", "display", timeline ? "flex" : "none");
            apply_tab_style("tab_content", browser);
            apply_tab_style("tab_log", log);
            apply_tab_style("tab_timeline", timeline);
        }

        private void apply_tab_style(string id, bool active) {
            if (ui == null) {
                return;
            }
            ui.try_set_element_style(id, "color", active ? "#efefef" : "#929292");
            ui.try_set_element_style(id, "border-bottom-color", active ? "#0070e0" : "transparent");
        }

        /// <summary>The project folder name shown at the right of the menu bar.</summary>
        private static string project_title() {
            if (!string.IsNullOrEmpty(workspace_title)) { return workspace_title; }
            var directory = managed_bootstrap.project_directory;
            if (string.IsNullOrEmpty(directory)) {
                return "deren editor";
            }
            var name = Path.GetFileName(directory.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
            return string.IsNullOrEmpty(name) ? "deren editor" : name;
        }

        private void refresh_panels(bool force_details = false) {
            if (document == null || selection_state == null || outliner == null || status_line == null) {
                return;
            }
            outliner.refresh(document, selection_state, outliner_filter);
            var selected = selection_state.primary == Guid.Empty ? null : document.find(selection_state.primary);
            details?.refresh(document, selection_state, force_details);
            refresh_point_light_details(selected);
            refresh_camera_material_details(selected);
            refresh_audio_source_details(selected);
            environment_ui?.refresh(document, global_environment_hdr, environment_catalog());
            environment_ui?.refresh_node(document, selected, environment_catalog());
            scripts_panel?.refresh(document, selection_state.primary, assets);
            status_line.refresh(document, selection_state, notice);
            update_history_buttons();
        }

        /// <summary>
        /// Dims Undo/Redo while their history side is empty. The document bridge has no enable flag, so
        /// the button is shown as unavailable and loses its pointer events; the styles are only written
        /// when the state flips, because <see cref="refresh_panels"/> runs every frame.
        /// </summary>
        private void update_history_buttons() {
            if (ui == null) {
                return;
            }
            bool can_undo = undo_history?.can_undo == true;
            bool can_redo = undo_history?.can_redo == true;
            if (undo_button_state != can_undo) {
                undo_button_state = can_undo;
                ui.try_set_element_style("btn_undo", "color", can_undo ? "#e8e8e8" : "#6a6a6a");
                ui.try_set_element_style("btn_undo", "pointer-events", can_undo ? "auto" : "none");
            }
            if (redo_button_state != can_redo) {
                redo_button_state = can_redo;
                ui.try_set_element_style("btn_redo", "color", can_redo ? "#e8e8e8" : "#6a6a6a");
                ui.try_set_element_style("btn_redo", "pointer-events", can_redo ? "auto" : "none");
            }
        }

        private void refresh_browser() {
            browser?.refresh(assets, browser_filter);
        }

        private void layout_gizmo() {
            if (ui == null || !ui.try_get_element_rect("viewport", out var viewport)) {
                return;
            }
            var node = gizmo.dragging ?? document?.find(selection_state?.primary ?? Guid.Empty);
            var parent_world = Matrix4x4.Identity;
            for (var ancestor = node; ancestor != null; ) {
                if (!ancestor.visible) {
                    node = null;
                    break;
                }
                if (!try_parent(ancestor, out var parent)) {
                    break;
                }
                if (parent != null) {
                    parent_world *= viewport_gizmo.local_matrix(parent.local);
                }
                ancestor = parent;
            }
            camera_projection.try_get_viewport_size(out var width, out var height);
            var tool = active_tool switch {
                "vp_move" => viewport_gizmo.gizmo_mode.move,
                "vp_rotate" => viewport_gizmo.gizmo_mode.rotate,
                "vp_scale" => viewport_gizmo.gizmo_mode.scale,
                _ => viewport_gizmo.gizmo_mode.select,
            };
            gizmo.layout(ui, viewport, node, parent_world, tool, gizmo_local, width, height);
            box_outline.layout(ui, viewport, node, parent_world, width, height);
        }

        private bool handle_gizmo(rml_event input) {
            if (gizmo.dragging is document_node node) {
                if (input.type == rml_event_type.pointer_cancel) {
                    finish_gizmo(cancel: true);
                } else if (input.type == rml_event_type.pointer_up && input.button == 0) {
                    finish_gizmo(cancel: false);
                } else if (input.type == rml_event_type.pointer_move && ui != null &&
                           ui.try_get_element_rect("viewport", out var area) && tree != null) {
                    var value = gizmo.update(new Vector2(input.x - area.x, input.y - area.y));
                    if (tree.try_set_transform(node.runtime, value)) {
                        node.local = value;
                        if (document != null && selection_state != null) {
                            details?.refresh(document, selection_state, true);
                        }
                    }
                }
                return true;
            }
            if (input.type != rml_event_type.pointer_down || input.button != 0 || open_menu.Length != 0 ||
                input.id is not ("viewport" or "viewport_image") || ui == null ||
                !ui.try_get_element_rect("viewport", out var viewport)) {
                return false;
            }
            if (!gizmo.begin(new Vector2(input.x - viewport.x, input.y - viewport.y))) {
                return false;
            }
            viewport_press = null;
            viewport_navigation.cancel_drag();
            return true;
        }

        private void finish_gizmo(bool cancel) {
            if (gizmo.dragging is not document_node node) {
                return;
            }
            var before = gizmo.original;
            var after = node.local;
            string label = gizmo.active_tool.ToString();
            gizmo.end();
            if (!cancel && !before.Equals(after)) {
                document?.mark_dirty();
                record_undo(label,
                    () => {
                        if (!apply_gizmo_transform(node, before)) { return false; }
                        mark_dirty_and_refresh();
                        return true;
                    },
                    () => {
                        if (!apply_gizmo_transform(node, after)) { return false; }
                        mark_dirty_and_refresh();
                        return true;
                    });
            } else if (cancel && !before.Equals(after)) {
                apply_gizmo_transform(node, before);
            }
            refresh_panels(force_details: true);
        }

        private bool apply_gizmo_transform(document_node node, transform_value value) {
            if (tree?.try_set_transform(node.runtime, value) != true) {
                return false;
            }
            node.local = value;
            return true;
        }

        private void show_scene_path(bool saving) {
            scene_path_open = true;
            scene_path_saving = saving;
            overwrite_path = null;
            scene_path_text = saving ? document?.path ?? default_scene_file : string.Empty;
            ui?.set_value("scene_path", scene_path_text);
            ui?.set_text("scene_path_title", saving ? "Save Scene As" : "Open Scene");
            ui?.set_text("scene_path_accept", saving ? "Save As" : "Open");
            ui?.set_text("scene_path_message", "Enter a path relative to the project's scenes folder, or an absolute path.");
            ui?.try_set_element_style("scene_path_prompt", "display", "flex");
        }

        private void hide_scene_path() {
            scene_path_open = false;
            ui?.try_set_element_style("scene_path_prompt", "display", "none");
        }

        private void handle_scene_path(string id) {
            if (id == "scene_path_cancel") {
                hide_scene_path();
                return;
            }
            if (id != "scene_path_accept") {
                return;
            }
            string path;
            try {
                var text = scene_path_text.Trim().Trim('"');
                if (text.Length == 0) {
                    ui?.set_text("scene_path_message", "Enter a scene path.");
                    return;
                }
                if (scene_path_saving && !text.EndsWith(".scene.toml", StringComparison.OrdinalIgnoreCase)) {
                    text += ".scene.toml";
                }
                path = Path.GetFullPath(text, Path.Combine(managed_bootstrap.project_directory, "scenes"));
            } catch (Exception error) when (error is ArgumentException or IOException or NotSupportedException) {
                ui?.set_text("scene_path_message", error.Message);
                return;
            }
            if (!scene_path_saving) {
                if (!scene_instances.try_load(managed_bootstrap.project_directory, path, out var loaded, out var error) || loaded == null) {
                    ui?.set_text("scene_path_message", error ?? "Could not open scene.");
                    return;
                }
                if (missing_asset_message(loaded) is { } missing) {
                    ui?.set_text("scene_path_message", missing);
                    return;
                }
                hide_scene_path();
                request_open_scene(loaded);
            } else if (File.Exists(path) && overwrite_path != path) {
                overwrite_path = path;
                ui?.set_text("scene_path_message", "A file already exists here. Choose Replace to overwrite it.");
                ui?.set_text("scene_path_accept", "Replace");
            } else if (save_document(path)) {
                hide_scene_path();
            } else {
                ui?.set_text("scene_path_message", string.IsNullOrEmpty(notice) ?
                    "Could not save the scene. Check the path and try again." : notice);
            }
        }

        private void request_open_scene(scene_document loaded) {
            if (document?.dirty == true) {
                pending_open = loaded;
                ui?.set_text("unsaved_message", "Save your changes before opening another scene?");
                ui?.try_set_element_style("unsaved_prompt", "display", "flex");
            } else {
                replace_scene(loaded);
            }
        }

        private bool replace_scene(scene_document next) {
            if (document == null || tree == null) {
                return false;
            }
            var old = document;
            document = next;
            foreach (var root in next.roots) {
                if (create_subtree(null, root)) {
                    continue;
                }
                foreach (var created in next.roots) {
                    if (created.runtime.is_valid) { destroy_runtime(created); }
                }
                document = old;
                notice = "Could not open scene: " + next.name;
                refresh_panels(force_details: true);
                return false;
            }
            foreach (var root in old.roots) {
                if (destroy_runtime(root)) {
                    continue;
                }
                foreach (var created in next.roots) { destroy_runtime(created); }
                document = old;
                foreach (var missing in old.roots) {
                    if (!missing.runtime.is_valid) { create_subtree(null, missing); }
                }
                notice = "Could not replace the current scene";
                refresh_panels(force_details: true);
                return false;
            }
            if (next.path != null && document_io.try_serialize(next, out var loaded_content, out _)) {
                next.mark_saved(next.path, loaded_content);
            }
            selection_state?.clear();
            undo_history?.clear();
            browser?.clear_selection();
            refresh_browser();
            notice = "Opened: " + next.name;
            refresh_panels(force_details: true);
            return true;
        }

        private void cycle_pipeline() {
            var node = document?.find(selection_state?.primary ?? Guid.Empty);
            if (node?.asset == null || tree == null || !asset_manager.try_load(node.asset, out var asset)) {
                return;
            }
            try {
                var pipelines = asset_manager.supported_pipelines(asset);
                if (pipelines.Length < 2) {
                    return;
                }
                // A node with no explicit pipeline runs its asset's declared default, so that is what
                // "before" has to mean: the cycle starts from the default and undo restores the null
                // that says "ask the asset" instead of pinning the default into the document.
                var before = node.pipeline;
                string pipeline = pipelines[(Array.IndexOf(pipelines, node.pipeline) + 1) % pipelines.Length];
                if (!tree.try_set_pipeline(node.runtime, pipeline)) {
                    return;
                }
                node.pipeline = pipeline;
                document?.mark_dirty();
                record_undo("Set Pipeline", () => restore_pipeline(node, before), () => restore_pipeline(node, pipeline));
                refresh_panels(force_details: true);
            } finally {
                asset_manager.release(asset);
            }
        }

        /// <summary>
        /// Writes a pipeline back onto the runtime node and the document. A null pipeline means "the
        /// asset's declared default": the runtime is still given the concrete name it needs, while the
        /// document records the absence so a save keeps the scene as it was authored.
        /// </summary>
        private bool restore_pipeline(document_node node, string? pipeline) {
            if (tree == null) {
                return false;
            }
            var target = pipeline;
            if (target == null) {
                if (node.asset == null || !asset_manager.try_load(node.asset, out var asset)) {
                    return false;
                }
                try {
                    if (!asset_manager.try_get_default_pipeline(asset, out target) || target == null) {
                        return false;
                    }
                } finally {
                    asset_manager.release(asset);
                }
            }
            if (!tree.try_set_pipeline(node.runtime, target)) {
                return false;
            }
            node.pipeline = pipeline;
            mark_dirty_and_refresh();
            return true;
        }

        private bool handle_asset_drag(rml_event input) {
            var pointer = new Vector2(input.x, input.y);
            if (input.type == rml_event_type.pointer_down && input.button == 0 &&
                content_browser_panel.try_tile_index(input.id, out var tile) &&
                browser?.try_tile_asset(tile, out var asset) == true) {
                asset_drag = (asset, pointer);
            } else if (input.type == rml_event_type.pointer_cancel) {
                asset_drag = null;
            } else if (input.type == rml_event_type.pointer_up && input.button == 0 && asset_drag is { } drag) {
                asset_drag = null;
                if (Vector2.DistanceSquared(drag.press, pointer) <= 16 ||
                    input.id is not ("viewport" or "viewport_image") ||
                    !try_viewport_ray(pointer, out var origin, out var direction)) {
                    return false;
                }
                // Drop onto the scene's Y=0 construction plane. A parallel or upward ray uses the orbit depth.
                float distance = MathF.Abs(direction.y) > 0.0001f ? -origin.y / direction.y : -1;
                float orbit_distance = orbit_camera.try_get(out var pose) ? pose.distance : 3;
                if (distance <= 0 || distance > orbit_distance * 4 || !float.IsFinite(distance)) {
                    distance = orbit_distance;
                }
                add_node(drag.asset, new vector_3 {
                    x = origin.x + direction.x * distance,
                    y = origin.y + direction.y * distance,
                    z = origin.z + direction.z * distance,
                });
                last_clicked_asset = string.Empty;
                return true;
            }
            return false;
        }

        private bool test_viewport_transform(document_node node) {
            if (ui == null || undo_history == null || !ui.try_get_element_rect("viewport", out var area)) {
                return false;
            }
            var before = node.local;
            foreach (var tool in new[] { "vp_move", "vp_rotate", "vp_scale" }) {
                set_active_tool(tool);
                layout_gizmo();
                if (!gizmo.try_handle(0, out var from, out var direction)) {
                    return false;
                }
                from += new Vector2(area.x, area.y);
                if (tool == "vp_rotate") {
                    direction = new Vector2(-direction.Y, direction.X);
                }
                var to = from + direction * 24;
                var down = new rml_event(rml_event_type.pointer_down, "viewport_image", "", from.X, from.Y);
                var move = new rml_event(rml_event_type.pointer_move, "viewport_image", "", to.X, to.Y);
                if (!handle_gizmo(down) || !handle_gizmo(move) || before.Equals(node.local)) {
                    return false;
                }
                var changed = node.local;
                handle_gizmo(new rml_event(rml_event_type.pointer_cancel, "", ""));
                if (!before.Equals(node.local) || !handle_gizmo(down) || !handle_gizmo(move)) {
                    return false;
                }
                handle_gizmo(new rml_event(rml_event_type.pointer_up, "viewport_image", "", to.X, to.Y));
                if (!undo_history.try_undo() || !before.Equals(node.local) ||
                    !undo_history.try_redo() || !changed.Equals(node.local) ||
                    !undo_history.try_undo() || !before.Equals(node.local)) {
                    return false;
                }
            }
            set_active_tool(tool_select_id);
            return true;
        }

        private void handle_viewport_selection(rml_event input) {
            const float click_slop = 4.0f;
            bool over_viewport = input.id is "viewport" or "viewport_image";
            var pointer = new Vector2(input.x, input.y);
            switch (input.type) {
                case rml_event_type.pointer_down:
                    viewport_press = input.button == 0 && over_viewport && open_menu.Length == 0 ? pointer : null;
                    break;
                case rml_event_type.pointer_move:
                    if (viewport_press is Vector2 start &&
                        Vector2.DistanceSquared(start, pointer) > click_slop * click_slop) {
                        viewport_press = null;
                    }
                    break;
                case rml_event_type.pointer_up when input.button == 0:
                    bool is_click = over_viewport && viewport_press is Vector2 press &&
                        Vector2.DistanceSquared(press, pointer) <= click_slop * click_slop;
                    viewport_press = null;
                    if (is_click) {
                        pick_viewport(pointer);
                    }
                    break;
                case rml_event_type.pointer_cancel:
                case rml_event_type.pointer_wheel:
                    viewport_press = null;
                    break;
            }
        }

        private bool try_viewport_ray(Vector2 pointer, out vector_3 origin, out vector_3 direction) {
            origin = default;
            direction = default;
            if (ui == null ||
                !ui.try_get_element_rect("viewport", out var viewport) || viewport.width <= 0 || viewport.height <= 0 ||
                !camera_projection.try_get_viewport_size(out var width, out var height)) {
                return false;
            }
            float u = (pointer.X - viewport.x) / viewport.width;
            float v = (pointer.Y - viewport.y) / viewport.height;
            return u >= 0 && u < 1 && v >= 0 && v < 1 &&
                camera_projection.try_screen_ray(u * width, v * height, out origin, out direction);
        }

        private void pick_viewport(Vector2 pointer) {
            if (tree == null || document == null || selection_state == null ||
                !try_viewport_ray(pointer, out var origin, out var direction) ||
                !tree.try_pick_node(origin, direction, out var hit)) {
                return;
            }
            var picked = document.find_runtime(hit);
            var node = picked?.instance_root ?? picked;
            if (node == null) {
                selection_state.clear();
            } else {
                selection_state.select(node.uid);
                reveal_in_outliner(node);
            }
            last_clicked_asset = string.Empty;
            notice = string.Empty;
            refresh_panels();
        }

        private bool focus_selection() {
            var node = selection_state == null ? null : document?.find(selection_state.primary);
            if (tree == null || node == null || !viewport_navigation.focus(tree, node.runtime, applied_aspect)) {
                notice = "Select a scene node to focus";
                return false;
            }
            notice = $"Focused: {node.name}";
            return true;
        }

        private void toggle_play() {
            if (playing == null || ui == null || document == null) {
                return;
            }
            if (animation_mode) {
                notice = "The animation editor previews in place; switch back to the scene to start Play.";
                refresh_panels();
                return;
            }
            if (playing.running) {
                playing.stop();
                ui.set_text("btn_play", "Stopping...");
                return;
            }
            stop_audio_preview();
            if (orbit_camera.try_get(out var camera) && playing.start(document, camera, out notice)) {
                ui.set_text("btn_play", "Stop");
                output_log.add(Guid.Empty, "Play started", false);
            }
        }

        private static void apply_play_camera(string camera_path) {
            if (File.Exists(camera_path)) {
                // A stale or hand-edited camera file must not keep the play session from starting:
                // an unreadable or empty pose falls back to the editor's default view instead.
                try {
                    var pose = System.Text.Json.JsonSerializer.Deserialize<orbit_camera?>(
                        File.ReadAllText(camera_path), play_session.camera_json);
                    if (pose is { } camera) {
                        // The ABI refuses a non-finite pose or a degenerate distance, so a bad
                        // file must not be applied silently after it parsed: say why the view
                        // stayed where it was.
                        if (!camera.try_apply(out var camera_status)) {
                            host_logger.write($"Editor: '{camera_path}' could not be applied ({camera_status}); keeping the default view");
                        }
                    } else {
                        host_logger.write($"Editor: '{camera_path}' holds no camera pose, using the default view");
                    }
                } catch (Exception error) {
                    host_logger.write($"Editor: cannot read '{camera_path}': {error.GetBaseException().Message}");
                }
            }
        }

        private void cleanup() {
            stop_audio_preview();
            editable_scripts?.Dispose();
            editable_scripts = null;
            scripts_panel = null;
            runtime?.Dispose();
            runtime = null;
            playing?.Dispose();
            playing = null;
            if (camera_input_installed) {
                input_events.try_set_callback(null);
                camera_input_installed = false;
            }
            ui?.Dispose();
            ui = null;
            outliner = null;
            details = null;
            status_line = null;
            browser = null;
            thumbnails?.Dispose();
            thumbnails = null;
            assets = [];
            undo_history?.clear();
            undo_history = null;
            selection_state = null;
            if (tree != null) {
                tree.detach();
                tree.destroy();
                tree = null;
            }
            document = null;
        }
    }
}
