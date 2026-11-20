using System;
using System.Collections.Generic;
using System.Globalization;
using System.Diagnostics;
using System.IO;
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
        // The dock tab whose body is visible; both bodies stay laid out, only visibility flips.
        private string active_dock_tab = "tab_content";
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
        // The viewport tool highlighted by the Select/Move/Rotate/Scale buttons (UE's transform widget mode).
        private const string tool_select_id = "vp_select";        private static readonly string[] tool_ids = ["vp_select", "vp_move", "vp_rotate", "vp_scale"];
        private string active_tool = tool_select_id;
        // A short status-bar suffix ("tool: select", "asset: ...", the About line) cleared by the next click.
        private string notice = string.Empty;
        private bool pending_new_scene;
        private string last_clicked_asset = string.Empty;
        private long last_asset_click;
        private int asset_selftest_phase;
        private Guid selftest_asset_uid;
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
            if (requested_selftest) { options->capture_frames = Math.Max(options->capture_frames, 4); }
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

            if (requested_scene != null) {
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

            if (!rml_document.try_open("ui/editor.rml", "ui/LatoLatin-Regular.ttf",
                                       out var opened_document, out var ui_status) || opened_document == null) {
                host_logger.write($"Editor: cannot open ui/editor.rml ({ui_status})");
                cleanup();
                return script_status.script_failure;
            }
            ui = opened_document;
            outliner = new outliner_panel(opened_document);
            details = new details_panel(opened_document);
            status_line = new status_bar(opened_document);
            undo_history = new undo_stack();
            selection_state = new selection();
            browser = new content_browser_panel(opened_document);
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

            instance = this;
            scripts->update = &update;
            scripts->stop = &stop;
            host_logger.write("Editor: ready");
            return script_status.success;
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
                if (current.asset_selftest_phase != 0) {
                    var status = current.run_runtime_asset_selftest();
                    if (status != script_status.success) { return status; }
                }
                current.handle_ui_events();
                current.refresh_panels();
                current.update_viewport_aspect();
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
                if (pending_new_scene) {
                    if (input.type == rml_event_type.click) { resolve_new_scene(input.id); }
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
                if (input.type == rml_event_type.pointer_down) {
                    begin_split_drag(input);
                    continue;
                }
                if (input.type == rml_event_type.pointer_move) {
                    update_split_drag(input);
                    continue;
                }
                if (input.type == rml_event_type.pointer_up) {
                    active_splitter = string.Empty;
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
                    case "detail_visible":
                        toggle_selected_visibility();
                        break;
                    case "tab_content":
                    case "tab_log":
                        set_dock_tab(input.id);
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
                    toggle_panel("outliner", "block", ref outliner_shown);
                    break;
                case "menu_window_details":
                    toggle_panel("details", "flex", ref details_shown);
                    break;
                case "menu_window_browser":
                    toggle_panel("bottom_dock", "flex", ref dock_shown);
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

        /// <summary>Shows or hides a panel from the Window menu without leaving its layout space behind.</summary>
        private void toggle_panel(string id, string display, ref bool shown) {
            if (ui == null) {
                return;
            }
            shown = !shown;
            ui.try_set_element_style(id, "display", shown ? display : "none");
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

        private bool resolve_new_scene(string choice) {
            if (!pending_new_scene || choice is not ("unsaved_save" or "unsaved_discard" or "unsaved_cancel")) {
                return false;
            }
            if (choice == "unsaved_save" && !save_document()) {
                ui?.set_text("unsaved_message", "Save failed. Your scene is unchanged. Retry, discard, or cancel.");
                return false;
            }
            pending_new_scene = false;
            last_clicked_asset = string.Empty;
            ui?.try_set_element_style("unsaved_prompt", "display", "none");
            return choice == "unsaved_cancel" || new_scene(discard_unsaved: true);
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

        private bool add_node(string? asset_id = null) {
            if (document == null || tree == null) {
                return false;
            }
            var parent = asset_id == null && selection_state != null && selection_state.primary != Guid.Empty
                ? document.find(selection_state.primary)
                : null;
            var node = new document_node { name = asset_id ?? "node", asset = asset_id };
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
                    if (!destroy_runtime(node)) { return; }
                    remove_document_node(parent, node);
                    if (asset_id != null) {
                        if (document.find(previous_selection) != null) { selection_state?.select(previous_selection); }
                        else { selection_state?.clear(); }
                    }
                    mark_dirty_and_refresh();
                },
                () => {
                    if (!insert_document_node(parent, index, node) || !create_subtree(parent, node)) {
                        if (node.runtime.is_valid) { destroy_runtime(node); }
                        remove_document_node(parent, node);
                        notice = $"Cannot recreate node: {node.name}";
                        refresh_panels(force_details: true);
                        return;
                    }
                    if (asset_id != null) { selection_state?.select(node.uid); }
                    mark_dirty_and_refresh();
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
                    insert_document_node(parent, index, node);
                    create_subtree(parent, node);
                    mark_dirty_and_refresh();
                },
                () => {
                    remove_document_node(parent, node);
                    destroy_runtime(node);
                    mark_dirty_and_refresh();
                });
            selection_state.clear();
            refresh_panels();
            return true;
        }

        private bool save_document() {
            if (document == null) {
                return false;
            }
            var path = string.IsNullOrEmpty(document.path)
                ? Path.Combine(managed_bootstrap.project_directory, "scenes", default_scene_file)
                : document.path!;
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
                () => {
                    node.name = previous;
                    push_runtime(node);
                    mark_dirty_and_refresh();
                },
                () => {
                    node.name = trimmed;
                    push_runtime(node);
                    mark_dirty_and_refresh();
                });
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

        /// <summary>Writes a snapshot back onto the document node and the runtime node.</summary>
        private void restore_node(document_node node, node_snapshot snapshot, bool mark_dirty) {
            node.name = snapshot.name;
            node.local = snapshot.local;
            node.visible = snapshot.visible;
            node.flags = snapshot.flags;
            push_runtime(node);
            if (mark_dirty) {
                mark_dirty_and_refresh();
            }
        }

        /// <summary>The editable values of one node, captured before and after an edit.</summary>
        private sealed class node_snapshot {
            internal node_snapshot(document_node node) {
                name = node.name;
                local = node.local;
                visible = node.visible;
                flags = node.flags;
            }

            internal readonly string name;
            internal readonly transform_value local;
            internal readonly bool visible;
            internal readonly uint flags;
        }

        private void record_undo(string label, Action undo, Action redo) {
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
            // Roots only expose add_root: an undone root deletion is appended at the end.
            document.add_root(node);
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
                    size = drag_start_size - (input.x - drag_origin);
                    panel = "right_column";
                    property = "width";
                    break;
                case "split_outliner":
                    size = drag_start_size + (input.y - drag_origin);
                    panel = "outliner";
                    property = "height";
                    break;
                case "split_bottom":
                    size = drag_start_size - (input.y - drag_origin);
                    panel = "bottom_dock";
                    property = "height";
                    break;
                default:
                    return;
            }
            size = Math.Clamp(size, drag_min_size, drag_max_size);
            ui.try_set_element_style(panel, property, string.Create(CultureInfo.InvariantCulture, $"{size:0.#}dp"));
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

        private void cleanup() {
            ui?.Dispose();
            ui = null;
            outliner = null;
            details = null;
            status_line = null;
            browser = null;
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
                if (!resolve_new_scene("unsaved_cancel") || pending_new_scene ||
                    !ReferenceEquals(document, unsaved_document) || undo_history?.next_undo_label != undo_label) {
                    return selftest_failure("Cancel did not preserve the document and undo history");
                }
                document.path = Path.Combine(path, "blocked.scene.toml");
                new_scene();
                if (resolve_new_scene("unsaved_save") || !pending_new_scene ||
                    !ReferenceEquals(document, unsaved_document)) {
                    return selftest_failure("a failed save discarded the document");
                }
                document.path = path;
                if (!resolve_new_scene("unsaved_save") || pending_new_scene ||
                    ReferenceEquals(document, unsaved_document) || undo_history?.can_undo == true ||
                    !document_io.try_load(path, out var saved_before_new, out _) || saved_before_new == null ||
                    !same_document(unsaved_document, saved_before_new, out _)) {
                    return selftest_failure("Save then New Scene did not preserve the old document on disk");
                }
                var discarded_document = document;
                new_scene();
                if (!pending_new_scene || !resolve_new_scene("unsaved_discard") || pending_new_scene ||
                    ReferenceEquals(document, discarded_document)) {
                    return selftest_failure("Discard did not create a new scene after confirmation");
                }
                host_logger.write("Editor selftest: unsaved confirmation save/failure/discard/cancel");
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
                if (runtime_selftest_asset == "laevatain" && added.pipeline != "game_toon") {
                    return selftest_failure("Laevatain did not retain its game_toon default");
                }
                selftest_asset_uid = added.uid;
                host_logger.write($"Editor selftest: runtime double click loaded and selected {runtime_selftest_asset}");
            } else if (asset_selftest_phase == 2) {
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
                browser_filter = string.Empty;
                notice = "Asset loading self-test passed";
                refresh_browser();
                refresh_panels(force_details: true);
                asset_selftest_phase = 0;
                host_logger.write("Editor selftest: runtime asset rollback/save passed");
                return script_status.success;
            }
            ++asset_selftest_phase;
            return script_status.success;
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
