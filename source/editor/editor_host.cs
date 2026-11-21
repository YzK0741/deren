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
    internal sealed unsafe class editor_host : game_entry {
        private const int event_budget = 128;
        private const string rename_field_id = "rename_field";
        private const string project_label_id = "project_label";
        private const string default_scene_file = "untitled.scene.toml";

        private static editor_host? instance;
        private static string? requested_scene;
        private static string? requested_thumbnail;
        private static string? requested_play;
        private play_behaviours? behaviours;
        private play_session? playing;
        private string? play_stop_marker;
        private long last_play_stop_check;
        private static bool requested_selftest;
        private static string runtime_selftest_asset = "damaged-helmet";

        private scene_document? document;
        private scene_tree? tree;
        private rml_document? ui;
        private outliner_panel? outliner;
        private details_panel? details;
        private status_bar? status_line;
        private undo_stack? undo_history;
        private selection? selection_state;
        private string rename_field = string.Empty;
        // The projection aspect last handed to the renderer, so the per-frame check only calls out when
        // the viewport column actually changes shape.
        private float applied_aspect = -1.0f;
        private readonly viewport_camera viewport_navigation = new();
        private readonly viewport_gizmo gizmo = new();
        private bool gizmo_local;
        private bool cancel_gizmo_requested;
        private float viewport_height;
        private Vector2? viewport_press;
        private bool focus_selection_requested;
        private bool camera_input_installed;
        // The dock tab whose body is visible; both bodies stay laid out, only visibility flips.
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

        public script_status preinit(startup_options* options, int argc, byte** argv) {
            if (options == null || argc < 0 || (argc > 0 && argv == null)) {
                return script_status.invalid_argument;
            }
            var args = new string[argc];
            for (var i = 0; i < argc; ++i) {
                args[i] = argv[i] == null ? string.Empty : Marshal.PtrToStringUTF8((IntPtr)argv[i]) ?? string.Empty;
            }
            for (var i = 1; i < args.Length; ++i) {
                var argument = args[i];
                var separator = argument.IndexOf('=');
                var name = separator < 0 ? argument : argument[..separator];
                var inline_value = separator < 0 ? null : argument[(separator + 1)..];
                switch (name) {
                    case "--scene":
                        requested_scene = inline_value ?? take_value(args, ref i);
                        if (string.IsNullOrEmpty(requested_scene)) {
                            requested_scene = null;
                        }
                        break;
                    case "--editor-play":
                        requested_play = inline_value ?? take_value(args, ref i);
                        requested_scene = requested_play;
                        break;
                    case "--editor-thumbnail":
                        requested_thumbnail = inline_value ?? take_value(args, ref i);
                        options->capture_frames = 8;
                        options->capture_windowed = 0;
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
            // The selftest runs over several frames: one per asset phase, then the layout shots the
            // Window-menu check needs (a display change only reaches the boxes on the next pass).
            if (requested_selftest) { options->capture_frames = Math.Max(options->capture_frames, 20); }
            return script_status.success;
        }

        private static string take_value(string[] args, ref int index) =>
            index + 1 < args.Length ? args[++index] : string.Empty;

        public script_status initialize(native_api* api, managed_api* scripts) {
            if (api == null || scripts == null || instance != null) {
                return script_status.invalid_argument;
            }
            var module_status = scene_api.initialize(api);
            if (module_status != script_status.success) {
                return module_status;
            }

            if (requested_thumbnail != null) {
                document = new scene_document();
                document.add_root(new document_node { name = requested_thumbnail, asset = requested_thumbnail });
            } else if (requested_scene != null) {
                if (!document_io.try_load(requested_scene, out var loaded, out var load_error) || loaded == null) {
                    host_logger.write($"Editor: cannot load scene '{requested_scene}': {load_error}");
                    return script_status.script_failure;
                }
                document = loaded;
            } else {
                document = create_default_document();
            }

            // The one and only rebuild: it creates and activates the tree, which the native host
            // requires before it will run the project (see app_scripting.cpp activate_scene).
            if (!scene_sync.rebuild(document, out var rebuilt_tree, out var rebuild_status) || rebuilt_tree == null) {
                host_logger.write($"Editor: cannot rebuild the scene tree ({rebuild_status})");
                cleanup();
                return rebuild_status != script_status.success ? rebuild_status : script_status.script_failure;
            }
            tree = rebuilt_tree;
            if (requested_play != null) {
                play_stop_marker = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(requested_play))!, "stop.request");
                var camera_path = Path.Combine(Path.GetDirectoryName(play_stop_marker)!, "camera.json");
                if (File.Exists(camera_path)) {
                    var pose = System.Text.Json.JsonSerializer.Deserialize<orbit_camera>(File.ReadAllText(camera_path), play_session.camera_json);
                    pose.try_apply();
                }
                instance = this;
                scripts->update = &update;
                scripts->stop = &stop;
                return script_status.success;
            }
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

            if (!rml_document.try_open("ui/editor.rml", "ui/LatoLatin-Regular.ttf",
                                       out var opened_document, out var ui_status) || opened_document == null) {
                host_logger.write($"Editor: cannot open ui/editor.rml ({ui_status})");
                cleanup();
                return script_status.script_failure;
            }
            ui = opened_document;
            if (!requested_selftest) {
                layout_path = Path.Combine(managed_bootstrap.project_directory, "editor-layout.json");
                layout = workspace_layout.load(layout_path);
                apply_workspace_layout();
            }
            outliner = new outliner_panel(opened_document);
            details = new details_panel(opened_document);
            status_line = new status_bar(opened_document);
            undo_history = new undo_stack();
            selection_state = new selection();
            // A crashed editor cannot run the cooperative stop that removes its play/thumbnail
            // workspaces, so the next launch sweeps whatever an earlier process left behind.
            play_session.sweep_stale_workspaces(managed_bootstrap.project_directory);
            thumbnails = new asset_thumbnail_cache(managed_bootstrap.project_directory);
            playing = new play_session(managed_bootstrap.project_directory);
            browser = new content_browser_panel(opened_document, thumbnails);
            assets = asset_manager.list();
            apply_tool_style();
            refresh_browser();
            opened_document.set_text(project_label_id, project_title());
            refresh_panels(force_details: true);
            host_logger.write($"Editor: document ready ({count_nodes(document.roots)} node(s))");

            if (requested_selftest) {
                var selftest_status = run_selftest();
                if (selftest_status != script_status.success) {
                    cleanup();
                    return selftest_status;
                }
                asset_selftest_phase = 1;
            }

            camera_input_installed = input_events.try_set_callback(input => {
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
                if (requested_play != null) {
                    return current.update_play(frame);
                }
                if (current.playing?.poll(out var play_result) == true) {
                    current.notice = play_result;
                    current.ui?.set_text("btn_play", "Play");
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
                current.refresh_panels();
                current.update_viewport_aspect();
                current.layout_gizmo();
                if (current.focus_selection_requested) {
                    current.focus_selection_requested = false;
                    if (!current.pending_new_scene && !current.pending_close && current.pending_open == null && !current.scene_path_open) {
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
                if (scene_path_open) {
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
                    asset_drag = null;
                    viewport_press = null;
                    viewport_navigation.cancel_drag();
                    if (input.type == rml_event_type.click) { resolve_unsaved(input.id); }
                    continue;
                }
                if (input.type == rml_event_type.change) {
                    if (input.id == rename_field_id) {
                        rename_field = input.value;
                    } else if (input.id == "browser_search") {
                        browser_filter = input.value;
                        refresh_browser();
                    } else if (input.id == "outliner_search") {
                        outliner_filter = input.value;
                        refresh_panels();
                    } else if (details_panel.try_field(input.id, out var field)) {
                        apply_detail_change(field, input.value);
                    }
                    continue;
                }
                if (handle_asset_drag(input) || handle_gizmo(input)) {
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
                switch (input.id) {
                    case "btn_new":
                        add_node();
                        break;
                    case "btn_delete":
                        delete_selected();
                        break;
                    case "btn_play":
                        toggle_play();
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
                    case "browser_thumbnail":
                        if (browser == null || browser.selected_asset.Length == 0) {
                            notice = "Select an asset to render a thumbnail";
                        } else if (thumbnails != null) {
                            thumbnails.begin(browser.selected_asset, out notice);
                        }
                        break;
                    case "tab_content":
                    case "tab_log":
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
                        if (!toggle_row_visibility(input.id) && !handle_asset_click(input.id, Stopwatch.GetTimestamp())) {
                            select_row(input.id);
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
            if (id is "menu_file" or "menu_edit" or "menu_window" or "menu_help") {
                toggle_menu($"{id}_items");
                return true;
            }
            switch (id) {
                case "menu_file_new":
                    new_scene();
                    break;
                case "menu_file_open":
                    show_scene_path(saving: false);
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
            ["menu_file_items", "menu_edit_items", "menu_window_items", "menu_help_items"];

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
            var roots = new List<document_node>(document.roots);
            for (var index = 0; index < roots.Count; index++) {
                destroy_runtime(roots[index]);
            }
            document = create_default_document();
            selection_state?.clear();
            undo_history?.clear();
            browser?.clear_selection();
            if (!create_subtree(null, document.roots[0])) {
                refresh_panels(force_details: true);
                return false;
            }
            document.mark_dirty();
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

        /// <summary>Toggles a node from the visibility glyph at the right of its outliner row.</summary>
        private bool toggle_row_visibility(string id) {
            if (!id.StartsWith("row_", StringComparison.Ordinal) ||
                !id.EndsWith("_vis", StringComparison.Ordinal) || document == null || outliner == null) {
                return false;
            }
            var digits = id["row_".Length..^"_vis".Length];
            if (!int.TryParse(digits, NumberStyles.Integer, CultureInfo.InvariantCulture, out var row) ||
                !outliner.try_row_uid(row, out var uid)) {
                return false;
            }
            var node = document.find(uid);
            return node != null && set_node_visibility(node, !node.visible);
        }

        /// <summary>Picks an asset tile (#asset_N or #asset_N_name) and reports it in the status bar.</summary>
        private bool select_asset(string id) {
            if (!id.StartsWith("asset_", StringComparison.Ordinal)) {
                return false;
            }
            var digits = id["asset_".Length..];
            if (digits.EndsWith("_name", StringComparison.Ordinal)) {
                digits = digits[..^"_name".Length];
            }
            if (!int.TryParse(digits, NumberStyles.Integer, CultureInfo.InvariantCulture, out var tile) ||
                browser == null || !browser.try_select_tile(tile)) {
                return false;
            }
            notice = $"asset: {browser.selected_asset}";
            refresh_browser();
            refresh_panels();
            return true;
        }

        private bool handle_asset_click(string id, long timestamp) {
            if (!select_asset(id) || browser == null) { return false; }
            var asset = browser.selected_asset;
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

        /// <summary>A row click arrives as either the slot (#row_N) or its text (#row_N_name).</summary>
        private void select_row(string id) {
            if (!id.StartsWith("row_", StringComparison.Ordinal) || selection_state == null) {
                return;
            }
            var digits = id["row_".Length..];
            if (digits.EndsWith("_name", StringComparison.Ordinal)) {
                digits = digits[..^"_name".Length];
            }
            if (!int.TryParse(digits, NumberStyles.Integer, CultureInfo.InvariantCulture, out var row) ||
                outliner == null || !outliner.try_row_uid(row, out var uid)) {
                return;
            }
            selection_state.select(uid);
            refresh_panels();
        }

        private bool add_node(string? asset_id = null, vector_3? position = null) {
            if (document == null || tree == null) {
                return false;
            }
            var parent = asset_id == null && selection_state != null && selection_state.primary != Guid.Empty
                ? document.find(selection_state.primary)
                : null;
            var node = new document_node { name = asset_id ?? "node", asset = asset_id };
            if (position is vector_3 placement) {
                node.local.position = placement;
            }
            var previous_selection = selection_state?.primary ?? Guid.Empty;
            var index = parent != null ? parent.children.Count : document.roots.Count;
            if (!insert_document_node(parent, index, node) || !create_subtree(parent, node)) {
                // Best effort: drop whatever runtime nodes the failed subtree already created.
                if (node.runtime.is_valid) { destroy_runtime(node); }
                remove_document_node(parent, node);
                if (asset_id != null) { notice = $"Cannot load asset: {asset_id}"; }
                refresh_panels();
                return false;
            }
            if (asset_id != null) {
                selection_state?.select(node.uid);
                notice = $"Added asset: {asset_id}";
            }
            document.mark_dirty();
            record_undo(asset_id == null ? "Create" : "Add Asset",
                () => {
                    if (!destroy_runtime(node)) { return false; }
                    remove_document_node(parent, node);
                    if (asset_id != null) {
                        if (document.find(previous_selection) != null) { selection_state?.select(previous_selection); }
                        else { selection_state?.clear(); }
                    }
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
                    if (asset_id != null) { selection_state?.select(node.uid); }
                    mark_dirty_and_refresh();
                    return true;
                });
            refresh_panels(force_details: asset_id != null);
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
            if (!destroy_runtime(node)) {
                return false;
            }
            if (!remove_document_node(parent, node)) {
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
                    document.mark_dirty();
                    mark_dirty_and_refresh();
                    return true;
                },
                () => {
                    if (!destroy_runtime(node) || !remove_document_node(parent, node)) {
                        return false;
                    }
                    document.mark_dirty();
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
            var path = destination ?? document.path ??
                Path.Combine(managed_bootstrap.project_directory, "scenes", default_scene_file);
            try {
                var directory = Path.GetDirectoryName(path);
                if (!string.IsNullOrEmpty(directory)) {
                    Directory.CreateDirectory(directory);
                }
            } catch (Exception error) {
                host_logger.write($"Editor: cannot create '{Path.GetDirectoryName(path)}': {error.GetBaseException().Message}");
                return false;
            }
            // document_io.try_save writes the file and marks the document saved itself.
            if (!document_io.try_save(document, path, out var save_error)) {
                host_logger.write($"Editor: cannot save '{path}': {save_error}");
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
                        node.name = trimmed;
                        changed = true;
                    }
                    break;
                }
                case detail_field.script: {
                    string? binding = string.IsNullOrWhiteSpace(text) ? null : text.Trim();
                    changed = binding != node.script;
                    node.script = binding;
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
            if (!push_runtime(node)) {
                // THE RUNTIME REFUSED THE SNAPSHOT: putting the document back keeps the two sides
                // equal, and returning false keeps the history entry where it was.
                node.name = previous.name;
                node.local = previous.local;
                node.visible = previous.visible;
                node.flags = previous.flags;
                node.script = previous.script;
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
            }

            internal readonly string name;
            internal readonly transform_value local;
            internal readonly bool visible;
            internal readonly uint flags;
            internal readonly string? script;
        }

        private void record_undo(string label, Func<bool> undo, Func<bool> redo) {
            undo_history?.push(new delegate_undo_command(label, undo, redo));
        }

        private void mark_dirty_and_refresh() {
            document?.mark_dirty();
            refresh_panels(force_details: true);
        }

        private bool insert_document_node(document_node? parent, int index, document_node node) {
            if (document == null) {
                return false;
            }
            if (parent != null) {
                parent.children.Insert(Math.Clamp(index, 0, parent.children.Count), node);
                return true;
            }
            document.insert_root(index, node);
            return true;
        }

        private bool remove_document_node(document_node? parent, document_node node) {
            if (document == null) {
                return false;
            }
            if (parent != null) {
                return parent.children.Remove(node);
            }
            return document.remove_root(node);
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

        /// <summary>Switches the bottom dock between the Content Browser and the Output Log.</summary>
        private void set_dock_tab(string tab) {
            if (ui == null || (tab != "tab_content" && tab != "tab_log") || tab == active_dock_tab) {
                return;
            }
            active_dock_tab = tab;
            var browser = tab == "tab_content";
            ui.set_visible("browser_body", browser);
            ui.set_visible("log_body", !browser);
            apply_tab_style("tab_content", browser);
            apply_tab_style("tab_log", !browser);
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
            details?.refresh(document, selection_state, force_details);
            status_line.refresh(document, selection_state, notice);
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
                locate_node(ancestor, out var parent, out _);
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
                if (!document_io.try_load(path, out var loaded, out var error) || loaded == null) {
                    ui?.set_text("scene_path_message", error ?? "Could not open scene.");
                    return;
                }
                hide_scene_path();
                if (document?.dirty == true) {
                    pending_open = loaded;
                    ui?.set_text("unsaved_message", "Save your changes before opening another scene?");
                    ui?.try_set_element_style("unsaved_prompt", "display", "flex");
                } else {
                    replace_scene(loaded);
                }
            } else if (File.Exists(path) && overwrite_path != path) {
                overwrite_path = path;
                ui?.set_text("scene_path_message", "A file already exists here. Choose Replace to overwrite it.");
                ui?.set_text("scene_path_accept", "Replace");
            } else if (save_document(path)) {
                hide_scene_path();
            } else {
                ui?.set_text("scene_path_message", "Could not save the scene. Check the path and try again.");
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
                input.id.StartsWith("asset_", StringComparison.Ordinal)) {
                var digits = input.id.AsSpan("asset_".Length);
                int suffix = digits.IndexOf('_');
                if (suffix >= 0) { digits = digits[..suffix]; }
                if (int.TryParse(digits, out var tile) && browser?.try_tile_asset(tile, out var asset) == true) {
                    asset_drag = (asset, pointer);
                }
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
            var node = document.find_runtime(hit);
            if (node == null) {
                selection_state.clear();
            } else {
                selection_state.select(node.uid);
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
            if (playing.running) {
                playing.stop();
                ui.set_text("btn_play", "Stopping...");
                return;
            }
            if (orbit_camera.try_get(out var camera) && playing.start(document, camera, out notice)) {
                ui.set_text("btn_play", "Stop");
            }
        }

        private script_status update_play(frame_info* frame) {
            try {
                long now = Stopwatch.GetTimestamp();
                if (Stopwatch.GetElapsedTime(last_play_stop_check, now).TotalMilliseconds >= 100) {
                    last_play_stop_check = now;
                    if (File.Exists(play_stop_marker)) {
                        window_control.try_close();
                        return script_status.success;
                    }
                }
                if (behaviours == null && document != null) {
                    behaviours = new play_behaviours(Path.Combine(managed_bootstrap.project_directory, "scripts"));
                    behaviours.attach_nodes(document.roots, frame->scene);
                    if (animation_player.try_get_info(out _)) {
                        animation_player.try_play();
                    }
                }
                behaviours?.update(frame->delta_time);
                return script_status.success;
            } catch (Exception error) {
                host_logger.write($"Editor play: {error.GetBaseException().Message}");
                return script_status.script_failure;
            }
        }

        private void cleanup() {
            behaviours?.Dispose();
            behaviours = null;
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

        private script_status run_selftest() {
            if (document == null) {
                return selftest_failure("there is no document");
            }
            // The temp file lives beside the project assembly, not in the system temp directory: a
            // sandboxed run may deny writes there while the project directory is always writable
            // (the editor itself saves scenes under scenes/).
            var directory = Path.Combine(AppContext.BaseDirectory, "captures");
            Directory.CreateDirectory(directory);
            var path = Path.Combine(directory, $"editor-selftest-{Guid.NewGuid():N}.scene.toml");
            var original_path = document.path;
            try {
                if (!document_io.try_save(document, path, out var save_error)) {
                    return selftest_failure($"save to a temp path failed: {save_error}");
                }
                host_logger.write($"Editor selftest: saved {count_nodes(document.roots)} node(s)");
                if (!document_io.try_load(path, out var reloaded, out var load_error) || reloaded == null) {
                    return selftest_failure($"reload failed: {load_error}");
                }
                host_logger.write("Editor selftest: reloaded the document");
                if (!same_document(document, reloaded, out var mismatch)) {
                    return selftest_failure($"reload mismatch: {mismatch}");
                }
                host_logger.write("Editor selftest: uid/name/transform of every node match");

                var root = document.roots.Count != 0 ? document.roots[0] : null;
                if (root == null || selection_state == null) {
                    return selftest_failure("the document has no root to edit");
                }
                selection_state.select(root.uid);
                if (!apply_rename("selftest-root") || root.name != "selftest-root") {
                    return selftest_failure("rename through the button path failed");
                }
                host_logger.write("Editor selftest: renamed the root through the button path");

                root.visible = false;
                root.flags = 5;
                if (!push_runtime(root)) {
                    return selftest_failure("could not prepare hidden node with custom flags");
                }
                selection_state.select(root.uid);
                if (!delete_selected() || document.find(root.uid) != null) {
                    return selftest_failure("delete did not remove the root subtree");
                }
                host_logger.write("Editor selftest: deleted the root subtree");

                if (undo_history == null || !undo_history.try_undo()) {
                    return selftest_failure("undo failed");
                }
                if (document.find(root.uid) == null || root.name != "selftest-root") {
                    return selftest_failure("undo did not restore the renamed root");
                }
                if (tree == null || !tree.try_get_flags(root.runtime, out var restored_flags) || restored_flags != 5) {
                    return selftest_failure("undo did not restore runtime flags");
                }
                root.visible = true;
                root.flags = 0;
                if (!push_runtime(root)) { return selftest_failure("could not restore test node properties"); }
                host_logger.write("Editor selftest: undo restored the root and its runtime flags");

                selection_state.select(root.uid);
                if (!apply_detail_change(detail_field.position_x, "2.5") || root.local.position.x != 2.5F) {
                    return selftest_failure("the details panel did not edit the transform");
                }
                if (tree == null || !tree.try_get_transform(root.runtime, out var runtime_transform) ||
                    runtime_transform.position.x != 2.5F) {
                    return selftest_failure("the details edit did not reach the runtime transform");
                }
                host_logger.write("Editor selftest: details edit reached the runtime node");

                if (undo_history == null || !undo_history.try_undo() || root.local.position.x != 0.0F) {
                    return selftest_failure("undo did not restore the transform");
                }
                host_logger.write("Editor selftest: undo restored the transform");

                // The panels below are driven here the same way a click would drive them (the editor
                // self-test runs in-process, before the window is interactive).
                refresh_panels();
                var visible_outliner = outliner;
                if (visible_outliner == null || visible_outliner.visible_rows != 1) {
                    return selftest_failure($"the outliner shows {visible_outliner?.visible_rows ?? -1} row(s) for 1 node");
                }
                outliner_filter = "missing-node";
                refresh_panels();
                if (visible_outliner.visible_rows != 0) {
                    return selftest_failure("the outliner filter did not hide every row");
                }
                outliner_filter = "selftest-root";
                refresh_panels();
                if (visible_outliner.visible_rows != 1) {
                    return selftest_failure("the outliner filter did not keep the matching row");
                }
                outliner_filter = string.Empty;
                refresh_panels();
                host_logger.write("Editor selftest: outliner filter");

                if (!toggle_row_visibility("row_0_vis") || root.visible) {
                    return selftest_failure("the outliner visibility glyph did not hide the node");
                }
                if (!toggle_row_visibility("row_0_vis") || !root.visible) {
                    return selftest_failure("the outliner visibility glyph did not restore the node");
                }
                host_logger.write("Editor selftest: outliner visibility");

                var expected_asset = assets.Length != 0 ? assets[0] : null;
                if (expected_asset != null) {
                    if (!select_asset("asset_0") || browser?.selected_asset != expected_asset) {
                        return selftest_failure("the content browser did not select the first asset");
                    }
                    if (!select_asset("asset_0_name") || browser?.selected_asset != expected_asset) {
                        return selftest_failure("the content browser did not select the asset from its label");
                    }
                    browser?.clear_selection();
                    refresh_browser();
                    host_logger.write($"Editor selftest: content browser selected '{expected_asset}'");
                } else {
                    host_logger.write("Editor selftest: content browser has no assets to select");
                }

                set_active_tool("vp_move");
                if (active_tool != "vp_move") {
                    return selftest_failure("the viewport tool did not change");
                }
                set_active_tool(tool_select_id);
                host_logger.write("Editor selftest: viewport tool mode");

                toggle_menu("menu_help_items");
                if (open_menu != "menu_help_items") {
                    return selftest_failure("the help menu did not open");
                }
                close_menus();
                if (open_menu.Length != 0) {
                    return selftest_failure("the menu did not close");
                }
                host_logger.write("Editor selftest: menu bar");

                set_dock_tab("tab_log");
                if (active_dock_tab != "tab_log") {
                    return selftest_failure("the output log tab did not activate");
                }
                set_dock_tab("tab_content");
                if (active_dock_tab != "tab_content") {
                    return selftest_failure("the content browser tab did not reactivate");
                }
                host_logger.write("Editor selftest: dock tabs");

                var unsaved_document = document;
                var undo_label = undo_history?.next_undo_label;
                if (new_scene() || !pending_new_scene || !ReferenceEquals(document, unsaved_document)) {
                    return selftest_failure("New Scene discarded unsaved work without confirmation");
                }
                if (!resolve_unsaved("unsaved_cancel") || pending_new_scene ||
                    !ReferenceEquals(document, unsaved_document) || undo_history?.next_undo_label != undo_label) {
                    return selftest_failure("Cancel did not preserve the document and undo history");
                }
                document.path = Path.Combine(path, "blocked.scene.toml");
                new_scene();
                if (resolve_unsaved("unsaved_save") || !pending_new_scene ||
                    !ReferenceEquals(document, unsaved_document)) {
                    return selftest_failure("a failed save discarded the document");
                }
                document.path = path;
                if (!resolve_unsaved("unsaved_save") || pending_new_scene ||
                    ReferenceEquals(document, unsaved_document) || undo_history?.can_undo == true ||
                    !document_io.try_load(path, out var saved_before_new, out _) || saved_before_new == null ||
                    !same_document(unsaved_document, saved_before_new, out _)) {
                    return selftest_failure("Save then New Scene did not preserve the old document on disk");
                }
                var discarded_document = document;
                new_scene();
                if (!pending_new_scene || !resolve_unsaved("unsaved_discard") || pending_new_scene ||
                    ReferenceEquals(document, discarded_document)) {
                    return selftest_failure("Discard did not create a new scene after confirmation");
                }
                host_logger.write("Editor selftest: unsaved confirmation save/failure/discard/cancel");

                // The caption close button must ask the same question: a dirty document gets the prompt,
                // and cancelling it leaves the document open. The window itself is only asked to close
                // after an explicit save/discard, which is why this test cancels.
                var close_document = document;
                var close_undo_label = undo_history?.next_undo_label;
                document.mark_dirty();
                close_window();
                if (!pending_close || !ReferenceEquals(document, close_document)) {
                    return selftest_failure("the close button skipped the unsaved prompt");
                }
                if (!resolve_unsaved("unsaved_cancel") || pending_close ||
                    !ReferenceEquals(document, close_document) || undo_history?.next_undo_label != close_undo_label) {
                    return selftest_failure("cancelling the close prompt did not keep the document");
                }
                host_logger.write("Editor selftest: close confirmation");
                // A crashed editor cannot run the cooperative stop that removes its play/thumbnail
                // workspaces, and the startup sweep is what takes them away on the next launch.
                foreach (var transient in new[] { "play-sessions", "thumbnail-jobs" }) {
                    var stale = Path.Combine(managed_bootstrap.project_directory, transient, "selftest-" + Guid.NewGuid().ToString("N"));
                    Directory.CreateDirectory(stale);
                    File.WriteAllText(Path.Combine(stale, "left-behind.txt"), "a crashed session left this behind");
                    play_session.sweep_stale_workspaces(managed_bootstrap.project_directory);
                    if (Directory.Exists(stale)) {
                        return selftest_failure($"the startup sweep left a stale '{transient}' workspace");
                    }
                }
                host_logger.write("Editor selftest: stale play workspaces swept");
                selection_state.select(document.roots[0].uid);
                refresh_panels(force_details: true);
                host_logger.write("Editor selftest: passed");
                return script_status.success;
            } finally {
                document.path = original_path;
                try {
                    File.Delete(path);
                } catch (Exception) {
                    // The temp file is best-effort: a leftover must not hide the selftest result.
                }
            }
        }

        // Run after initialize so startup-only binders cannot accidentally make this test pass.
        private script_status run_runtime_asset_selftest() {
            if (document == null || selection_state == null || tree == null || undo_history == null) {
                return selftest_failure("runtime asset test has no scene");
            }
            if (asset_selftest_phase == 1) {
                if (Array.IndexOf(assets, runtime_selftest_asset) < 0) {
                    host_logger.write($"Editor selftest: runtime asset test skipped (no {runtime_selftest_asset} fixture)");
                    asset_selftest_phase = 0;
                    panel_selftest_phase = 1;
                    return script_status.success;
                }
                browser_filter = runtime_selftest_asset;
                refresh_browser();
                var count = count_nodes(document.roots);
                var time = Stopwatch.GetTimestamp();
                if (!handle_asset_click("asset_0", time) || count_nodes(document.roots) != count) {
                    return selftest_failure("single asset click created a node");
                }
                handle_asset_click("asset_0_name", time + Stopwatch.Frequency / 10);
                var added = document.find(selection_state.primary);
                if (count_nodes(document.roots) != count + 1 || added?.asset != runtime_selftest_asset || !added.runtime.is_valid) {
                    return selftest_failure("double click did not load and select the asset after startup");
                }
                if (runtime_selftest_asset == "laevatain") {
                    // THE DOCUMENT KEEPS THE ABSENCE: binding applies the asset's declared default to
                    // the runtime (see scene_sync.try_bind_asset) without writing it back into the
                    // node, so the check belongs on the asset side and on the document staying null.
                    if (added.pipeline != null) {
                        return selftest_failure("Laevatain's asset default was written into the document");
                    }
                    if (!asset_manager.try_load(runtime_selftest_asset, out var laevatain)) {
                        return selftest_failure("Laevatain could not be loaded for its default check");
                    }
                    try {
                        if (!asset_manager.try_get_default_pipeline(laevatain, out var declared) || declared != "game_toon") {
                            return selftest_failure("Laevatain did not declare its game_toon default");
                        }
                    } finally {
                        asset_manager.release(laevatain);
                    }
                }
                selftest_asset_uid = added.uid;
                host_logger.write($"Editor selftest: runtime double click loaded and selected {runtime_selftest_asset}");
            } else if (asset_selftest_phase == 2) {
                update_viewport_aspect();
                if (!focus_selection() || !orbit_camera.try_get(out var framed) ||
                    !tree.try_get_bounds(document.find(selftest_asset_uid)!.runtime, out var center, out var radius) ||
                    radius <= 0 || framed.target.y != center.y || framed.distance <= radius) {
                    return selftest_failure("viewport focus did not frame the asset bounds");
                }
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_down, "viewport_image", "", 10, 10, 1), viewport_height);
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_move, "details", "", 30, 20), viewport_height);
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_cancel, "", ""), viewport_height);
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_move, "viewport", "", 60, 40), viewport_height);
                if (!orbit_camera.try_get(out var orbited) || MathF.Abs(orbited.yaw_degrees - framed.yaw_degrees - 5.6f) > 0.01f) {
                    return selftest_failure("viewport orbit or drag cancellation failed");
                }
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_down, "viewport", "", 10, 10, 2), viewport_height);
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_move, "viewport", "", 20, 20), viewport_height);
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_up, "viewport", "", 20, 20, 2), viewport_height);
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_wheel, "viewport", "", 1), viewport_height);
                if (!orbit_camera.try_get(out var moved) || moved.target.y <= orbited.target.y || moved.distance >= orbited.distance) {
                    return selftest_failure("viewport pan or zoom failed");
                }
                focus_selection();
                host_logger.write("Editor selftest: viewport focus, orbit, pan, zoom and cancel passed");
                if (ui == null || !ui.try_get_element_rect("viewport", out var viewport)) {
                    return selftest_failure("viewport layout was unavailable for picking");
                }
                float x = viewport.x + viewport.width * 0.5f;
                float y = viewport.y + viewport.height * 0.5f;
                handle_viewport_selection(new(rml_event_type.pointer_down, "viewport_image", "", x, y));
                handle_viewport_selection(new(rml_event_type.pointer_move, "viewport_image", "", x + 12, y));
                handle_viewport_selection(new(rml_event_type.pointer_up, "viewport_image", "", x, y));
                if (selection_state?.primary != selftest_asset_uid) {
                    return selftest_failure("a viewport drag changed the selection");
                }
                float empty_x = viewport.x + 2, empty_y = viewport.y + 2;
                handle_viewport_selection(new(rml_event_type.pointer_down, "viewport_image", "", empty_x, empty_y));
                handle_viewport_selection(new(rml_event_type.pointer_up, "viewport_image", "", empty_x, empty_y));
                if (selection_state?.primary != Guid.Empty) {
                    return selftest_failure("clicking empty viewport did not clear selection");
                }
                handle_viewport_selection(new(rml_event_type.pointer_down, "viewport_image", "", x, y));
                handle_viewport_selection(new(rml_event_type.pointer_up, "viewport_image", "", x, y));
                if (selection_state?.primary != selftest_asset_uid) {
                    return selftest_failure("clicking the framed asset did not select it");
                }
                var picked_runtime = document.find(selftest_asset_uid)!.runtime;
                if (!tree.try_set_visible(picked_runtime, false)) {
                    return selftest_failure("could not hide the pickable asset");
                }
                handle_viewport_selection(new(rml_event_type.pointer_down, "viewport_image", "", x, y));
                handle_viewport_selection(new(rml_event_type.pointer_up, "viewport_image", "", x, y));
                if (selection_state?.primary != Guid.Empty || !tree.try_set_visible(picked_runtime, true)) {
                    return selftest_failure("a hidden asset was picked or could not be revealed");
                }
                selection_state.select(selftest_asset_uid);
                host_logger.write("Editor selftest: viewport picking, visibility and deselection passed");
                if (!test_viewport_transform(document.find(selftest_asset_uid)!)) {
                    return selftest_failure("gizmo drag, cancel, undo or redo failed");
                }
                host_logger.write("Editor selftest: transform gizmo passed");

                if (animation_player.try_get_info(out var animation) && (animation.playing || animation.time != 0)) {
                    return selftest_failure("an imported animation started playing in the editor");
                }
                if (!undo_history.try_undo() || document.find(selftest_asset_uid) != null) {
                    return selftest_failure("undo did not remove the imported asset");
                }
                host_logger.write("Editor selftest: runtime asset undo");
            } else if (asset_selftest_phase == 3) {
                if (!undo_history.try_redo() || document.find(selftest_asset_uid)?.runtime.is_valid != true ||
                    selection_state.primary != selftest_asset_uid) {
                    return selftest_failure("redo did not reload and select the imported asset");
                }
                host_logger.write("Editor selftest: runtime asset redo");
            } else {
                if (animation_player.try_get_info(out var animation) && (animation.playing || animation.time != 0)) {
                    return selftest_failure("redo started animation playback in the editor");
                }
                var count = count_nodes(document.roots);
                var selected = selection_state.primary;
                var undo_label = undo_history.next_undo_label;
                if (add_node("__editor_missing_asset__") || count_nodes(document.roots) != count ||
                    selection_state.primary != selected || undo_history.next_undo_label != undo_label) {
                    return selftest_failure("failed asset load changed the scene, selection or history");
                }
                var path = Path.Combine(managed_bootstrap.project_directory, "captures", $"asset-selftest-{Guid.NewGuid():N}.scene.toml");
                var original_path = document.path;
                try {
                    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                    if (!document_io.try_save(document, path, out _) ||
                        !document_io.try_load(path, out var saved, out _) ||
                        saved?.find(selftest_asset_uid)?.asset != runtime_selftest_asset) {
                        return selftest_failure("imported asset did not round trip through the scene file");
                    }
                } finally {
                    document.path = original_path;
                    document.mark_dirty();
                    File.Delete(path);
                }
                if (runtime_selftest_asset == "laevatain" &&
                    Array.IndexOf(assets, "damaged-helmet") >= 0 && Array.IndexOf(assets, "flowers") >= 0 &&
                    !test_mixed_scene()) {
                    return selftest_failure("mixed scene drag/drop, save point or reopening failed");
                }
                browser_filter = string.Empty;
                notice = "Asset loading self-test passed";
                refresh_browser();
                refresh_panels(force_details: true);
                asset_selftest_phase = 0;
                panel_selftest_phase = 1;
                host_logger.write("Editor selftest: runtime asset rollback/save passed");
                return script_status.success;
            }
            ++asset_selftest_phase;
            return script_status.success;
        }

        private bool test_mixed_scene() {
            if (document == null || selection_state == null || undo_history == null || browser == null ||
                ui == null || !ui.try_get_element_rect("viewport", out var area)) {
                host_logger.write("Editor mixed selftest: layout unavailable");
                return false;
            }
            browser_filter = string.Empty;
            refresh_browser();
            var ids = new[] { "damaged-helmet", "flowers" };
            for (int i = 0; i < ids.Length; ++i) {
                int tile = 0;
                while (browser.try_tile_asset(tile, out var id) && id != ids[i]) { ++tile; }
                if (!browser.try_tile_asset(tile, out _)) {
                    host_logger.write("Editor mixed selftest: missing asset tile");
                    return false;
                }
                handle_asset_drag(new(rml_event_type.pointer_down, $"asset_{tile}", "", area.x, area.y + area.height + 24));
                if (!handle_asset_drag(new(rml_event_type.pointer_up, "viewport_image", "",
                        area.x + area.width * (i == 0 ? 0.25f : 0.75f), area.y + area.height * 0.55f)) ||
                    document.find(selection_state.primary) is not { } placed || placed.asset != ids[i] ||
                    !placed.runtime.is_valid || placed.local.position.Equals(default(vector_3))) {
                    host_logger.write("Editor mixed selftest: drop did not place the asset");
                    return false;
                }
            }

            var character = document.find(selftest_asset_uid)!;
            // BINDING MUST NOT WRITE THE ASSET DEFAULT INTO THE DOCUMENT: a null pipeline already
            // means "use the default", and pinning it here would move the save point and make the
            // undo of the next pipeline change land on a state the file never had.
            if (character.pipeline != null) {
                host_logger.write("Editor mixed selftest: binding wrote the asset default into the document");
                return false;
            }
            selection_state.select(character.uid);
            string? original_pipeline = character.pipeline;
            cycle_pipeline();
            if (character.pipeline == original_pipeline || !undo_history.try_undo() ||
                character.pipeline != original_pipeline) {
                host_logger.write("Editor mixed selftest: pipeline edit or undo failed");
                return false;
            }

            var path = Path.Combine(managed_bootstrap.project_directory, "captures", $"mixed-selftest-{Guid.NewGuid():N}.scene.toml");
            var original = document;
            try {
                show_scene_path(saving: true);
                scene_path_text = path;
                handle_scene_path("scene_path_accept");
                if (scene_path_open || original.dirty || original.path != path) {
                    host_logger.write("Editor mixed selftest: Save As did not advance the save point");
                    return false;
                }
                if (!toggle_selected_visibility() || !original.dirty ||
                    !undo_history.try_undo() || original.dirty ||
                    !undo_history.try_redo() || !original.dirty || !undo_history.try_undo() || original.dirty) {
                    host_logger.write("Editor mixed selftest: visibility undo/redo did not track the save point");
                    return false;
                }
                if (!delete_selected() || !original.dirty || !undo_history.try_undo() || original.dirty) {
                    host_logger.write("Editor mixed selftest: delete undo did not return to the save point");
                    return false;
                }
                selection_state.select(character.uid);
                if (!toggle_selected_visibility() || !original.dirty) {
                    host_logger.write("Editor mixed selftest: visibility edit did not mark the scene dirty");
                    return false;
                }
                show_scene_path(saving: false);
                scene_path_text = path;
                handle_scene_path("scene_path_accept");
                if (pending_open == null || !resolve_unsaved("unsaved_cancel") || !ReferenceEquals(original, document) ||
                    !original.dirty || !undo_history.try_undo() || original.dirty) {
                    host_logger.write("Editor mixed selftest: Open cancellation did not preserve the draft");
                    return false;
                }
                show_scene_path(saving: false);
                scene_path_text = path;
                handle_scene_path("scene_path_accept");
                if (scene_path_open || ReferenceEquals(original, document) || document == null || document.dirty ||
                    !same_document(original, document, out _) || undo_history.can_undo) {
                    host_logger.write("Editor mixed selftest: reopened scene did not match saved scene");
                    return false;
                }
                foreach (var node in document.roots) {
                    if (!node.runtime.is_valid) {
                        host_logger.write("Editor mixed selftest: reopened runtime node is missing");
                        return false;
                    }
                }
            } finally {
                File.Delete(path);
                if (document?.path == path) { document.path = null; }
            }
            var loaded_character = document?.find(selftest_asset_uid);
            if (loaded_character == null) {
                host_logger.write("Editor mixed selftest: reopened character missing");
                return false;
            }
            selection_state.select(loaded_character.uid);
            set_active_tool("vp_move");
            focus_selection();
            host_logger.write("Editor selftest: mixed scene drag/drop, pipeline, save point and reopening passed");
            return true;
        }

        // Window-menu visibility must restore the element's own display type (the outliner is a flex
        // column, the details a block) and hide the splitter of a hidden panel: restoring the wrong
        // one collapses the outliner rows and lays the details out sideways, and a splitter left over
        // a hidden panel still moves it. Runs over several frames because a display change only
        // reaches the element boxes on the next layout pass.
        private script_status run_panel_selftest() {
            if (ui == null) {
                return selftest_failure("panel visibility test has no document");
            }
            switch (panel_selftest_phase) {
                case 1:
                    if (!ui.try_get_element_rect("outliner_rows", out var rows) ||
                        !ui.try_get_element_rect("details_title", out var title) ||
                        !ui.try_get_element_rect("details", out var details)) {
                        return selftest_failure("panel visibility test cannot measure the layout");
                    }
                    panel_rows_height = rows.height;
                    panel_title_width = title.width;
                    panel_details_y = details.y;
                    // Go through the menu handler, not toggle_panel directly: the regression was the
                    // display string the Window menu restores, which a direct call would bypass.
                    handle_menu_click("menu_window_outliner");
                    panel_selftest_phase = 2;
                    return script_status.success;
                case 3:
                    if (!ui.try_get_element_rect("details", out var hidden) ||
                        hidden.y > panel_details_y - 100.0f) {
                        return selftest_failure("a hidden panel left its layout space reserved");
                    }
                    handle_menu_click("menu_window_outliner");
                    panel_selftest_phase = 4;
                    return script_status.success;
                case 5:
                    if (!ui.try_get_element_rect("outliner_rows", out var restored_rows) ||
                        !ui.try_get_element_rect("details", out var restored_details) ||
                        MathF.Abs(restored_rows.height - panel_rows_height) > 1.0f ||
                        MathF.Abs(restored_details.y - panel_details_y) > 1.0f) {
                        return selftest_failure("re-showing the outliner did not restore its layout");
                    }
                    handle_menu_click("menu_window_details");
                    panel_selftest_phase = 6;
                    return script_status.success;
                case 7:
                    handle_menu_click("menu_window_details");
                    panel_selftest_phase = 8;
                    return script_status.success;
                case 9:
                    if (!ui.try_get_element_rect("details_title", out var revealed) ||
                        MathF.Abs(revealed.width - panel_title_width) > 1.0f) {
                        return selftest_failure("re-showing the details did not restore its layout");
                    }
                    handle_menu_click("menu_window_sidebar_side");
                    handle_menu_click("menu_window_dock_side");
                    handle_menu_click("menu_window_panel_order");
                    panel_selftest_phase = 10;
                    return script_status.success;
                case 11:
                    if (!ui.try_get_element_rect("right_column", out var sidebar) ||
                        !ui.try_get_element_rect("viewport", out var viewport) ||
                        !ui.try_get_element_rect("bottom_dock", out var dock) ||
                        !ui.try_get_element_rect("outliner", out var outliner_area) ||
                        !ui.try_get_element_rect("details", out var details_area) ||
                        sidebar.x >= viewport.x || dock.y >= viewport.y || details_area.y >= outliner_area.y) {
                        return selftest_failure("the rearranged workspace did not move all three panels");
                    }
                    begin_split_drag(new(rml_event_type.pointer_down, "split_right", "", sidebar.x + sidebar.width, sidebar.y));
                    update_split_drag(new(rml_event_type.pointer_move, "split_right", "", sidebar.x + sidebar.width + 20, sidebar.y));
                    finish_split_drag();
                    panel_selftest_phase = 12;
                    return script_status.success;
                case 13:
                    if (!ui.try_get_element_rect("right_column", out var widened) || MathF.Abs(widened.width - 370) > 1) {
                        return selftest_failure("the left sidebar splitter did not widen to the right");
                    }
                    handle_menu_click("menu_window_reset_layout");
                    panel_selftest_phase = 14;
                    return script_status.success;
                case 15:
                    if (!ui.try_get_element_rect("right_column", out var reset) ||
                        !ui.try_get_element_rect("viewport", out var reset_viewport) ||
                        MathF.Abs(reset.width - 350) > 1 || reset.x <= reset_viewport.x) {
                        return selftest_failure("Reset Layout did not restore the right sidebar");
                    }
                    panel_selftest_phase = 0;
                    notice = "Panel self-test passed";
                    host_logger.write("Editor selftest: panel visibility, rearrangement and reset passed");
                    return script_status.success;
                default:
                    // Even phases are the frames that let a display change reach the element boxes.
                    ++panel_selftest_phase;
                    return script_status.success;
            }
        }

        private static script_status selftest_failure(string message) {
            host_logger.write($"Editor selftest: FAILED - {message}");
            return script_status.script_failure;
        }

        private static bool same_document(scene_document expected, scene_document actual, out string? mismatch) {
            var left = new List<document_node>();
            flatten(expected.roots, left);
            var right = new List<document_node>();
            flatten(actual.roots, right);
            if (left.Count != right.Count) {
                mismatch = $"{left.Count} node(s) reloaded as {right.Count}";
                return false;
            }
            for (var index = 0; index < left.Count; ++index) {
                var before = left[index];
                var after = right[index];
                if (before.uid != after.uid) {
                    mismatch = $"node {index} uid {before.uid} reloaded as {after.uid}";
                    return false;
                }
                if (before.name != after.name) {
                    mismatch = $"node '{before.name}' reloaded as '{after.name}'";
                    return false;
                }
                if (before.asset != after.asset || before.pipeline != after.pipeline || before.script != after.script ||
                    before.visible != after.visible || before.flags != after.flags) {
                    mismatch = $"node '{before.name}' properties changed on reload";
                    return false;
                }
                if (!same_transform(before.local, after.local)) {
                    mismatch = $"node '{before.name}' transform changed on reload";
                    return false;
                }
            }
            mismatch = null;
            return true;
        }

        private static void flatten(IReadOnlyList<document_node> nodes, List<document_node> into) {
            for (var index = 0; index < nodes.Count; ++index) {
                into.Add(nodes[index]);
                flatten(nodes[index].children, into);
            }
        }

        private static bool same_transform(transform_value left, transform_value right) =>
            left.position.x == right.position.x && left.position.y == right.position.y &&
            left.position.z == right.position.z && left.rotation_degrees.x == right.rotation_degrees.x &&
            left.rotation_degrees.y == right.rotation_degrees.y && left.rotation_degrees.z == right.rotation_degrees.z &&
            left.scale.x == right.scale.x && left.scale.y == right.scale.y && left.scale.z == right.scale.z;

        private static int count_nodes(IReadOnlyList<document_node> nodes) {
            var total = nodes.Count;
            for (var index = 0; index < nodes.Count; ++index) {
                if (nodes[index].children.Count != 0) {
                    total += count_nodes(nodes[index].children);
                }
            }
            return total;
        }
    }
}
