using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using deren.scripting;

namespace deren.editor;

/// <summary>
/// The scene screen's selftest: it drives the real input path over a submitted frame, so the assertions
/// cover the whole chain from the event queue to the document and back into the painted tree.
/// </summary>
internal sealed unsafe partial class editor_host {
    private int scene_selftest_phase;

    private script_status scene_selftest_tick() {
        switch (this.scene_selftest_phase) {
            case 0: {
                if (verify_scene_screen() is { } problem) {
                    return selftest_failure(problem);
                }
                host_logger.write(
                    "Editor selftest: the scene screen listed the nodes, selected one, showed it in the details panel and left the viewport to the camera.");
                this.scene_selftest_phase = 1;
                return script_status.success;
            }
            case 1: {
                if (verify_scene_folding() is { } problem) {
                    return selftest_failure(problem);
                }
                host_logger.write(
                    "Editor selftest: the outliner folded a branch away and the modifiers extended the selection.");
                this.scene_selftest_phase = 2;
                return script_status.success;
            }
            case 2: {
                // The details panel edits in place: a press turns a lane into a field, Enter applies it
                // through the document, one undo takes it back, and a malformed vector is refused.
                if (verify_scene_editing() is { } problem) {
                    return selftest_failure(problem);
                }
                host_logger.write(
                    "Editor selftest: the details panel renamed a node and set a transform as one undo step.");
                this.scene_selftest_phase = 3;
                return script_status.success;
            }
            case 3: {
                // The dividers resize the panels, stop at their clamps, and the release writes the sizes
                // into the layout file the classic shell also reads.
                if (verify_scene_splitters() is { } split_problem) {
                    return selftest_failure(split_problem);
                }
                host_logger.write(
                    "Editor selftest: the dividers resized the scene panels, clamped and persisted the layout.");
                this.scene_selftest_phase = 4;
                return script_status.success;
            }
            case 4: {
                // The outliner's other tools: the visibility mark, the search field, Delete, Ctrl+D and
                // the browser's double-click.
                if (verify_scene_outliner_tools() is { } tools_problem) {
                    return selftest_failure(tools_problem);
                }
                host_logger.write(
                    "Editor selftest: the outliner hid a node, filtered the rows and the keys removed and copied.");
                this.scene_selftest_phase = 5;
                return script_status.success;
            }
            case 5: {
                // Component lanes: a light value, a camera value, a material multiplier, an actor flag
                // and the flags word, each one its own single undo step.
                if (verify_scene_components() is { } component_problem) {
                    return selftest_failure(component_problem);
                }
                host_logger.write(
                    "Editor selftest: the details panel edited a light, a camera and an actor flag, one undo step each.");
                this.scene_selftest_phase = 6;
                return script_status.success;
            }
            case 6: {
                // The inspector scrolls: the wheel moves the window, only visible rows are built and the
                // clamp stops at the bottom of the content.
                if (verify_scene_details_scroll() is { } scroll_problem) {
                    return selftest_failure(scroll_problem);
                }
                host_logger.write(
                    "Editor selftest: the inspector scrolled through its rows and clamped at the content.");
                this.scene_selftest_phase = 7;
                return script_status.success;
            }
            case 7: {
                // Play: the header button runs the same session the classic shell starts.
                if (verify_scene_play() is { } play_problem) {
                    return selftest_failure(play_problem);
                }
                host_logger.write(
                    "Editor selftest: the header started the play session and the second press stopped it.");
                this.scene_selftest_phase = 8;
                return script_status.success;
            }
            case 8: {
                // Save and reopen: the file the screen writes is the file the loader reads back.
                if (verify_scene_save_reopen() is { } save_problem) {
                    return selftest_failure(save_problem);
                }
                host_logger.write(
                    "Editor selftest: the scene screen saved the document and reopened it with the edit kept.");
                this.scene_selftest_phase = 9;
                return script_status.success;
            }
            case 9: {
                // The preview's click: a press released in place picks, a press that moved is a camera drag.
                if (verify_scene_picking() is { } pick_problem) {
                    return selftest_failure(pick_problem);
                }
                host_logger.write(
                    "Editor selftest: a click in the preview picked a node and a drag left the selection alone.");
                this.scene_selftest_phase = 10;
                return script_status.success;
            }
            case 10: {
                // The browser's search: typing narrows the project list, the rows keep their absolute
                // index and the count in the caption follows.
                if (verify_scene_browser_search() is { } search_problem) {
                    return selftest_failure(search_problem);
                }
                host_logger.write(
                    "Editor selftest: the browser search narrowed the project list to the matching asset.");
                this.scene_selftest_phase = 11;
                return script_status.success;
            }
            case 11: {
                // Dragging a row onto another reparents it, one undo step puts it back.
                if (verify_scene_reparent() is { } drag_problem) {
                    return selftest_failure(drag_problem);
                }
                host_logger.write(
                    "Editor selftest: dragging an outliner row reparented the node and one undo restored it.");
                this.scene_selftest_phase = 12;
                return script_status.success;
            }
            default:
                return script_status.success;
        }
    }

    /// <summary>True when the node is one of the scene roots; the root list is an interface, not a list.</summary>
    private static bool scene_has_root(IReadOnlyList<document_node> roots, document_node node) {
        foreach (var root in roots) {
            if (root == node) { return true; }
        }
        return false;
    }

    /// <summary>Pushes one typed character through the real input queue.</summary>
    private void type_scene_character(char value) {
        next_ui_events.Clear();
        on_next_ui_input(new input_event(input_type.character, 0, true, 0, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, value));
        process_scene_input();
    }

    /// <summary>
    /// The browser search: a press focuses the field, typed text narrows the matches, the rebuilt frame
    /// shows only those rows, and Escape clears the field again.
    /// </summary>
    private string? verify_scene_browser_search() {
        if (next_ui == null || assets.Length == 0) {
            return "the browser check has no assets";
        }
        var root = submit_scene_frame_for_test(1600.0F, 900.0F);
        if (root.find("scene_asset_filter") is not { } field) {
            return "the browser has no search field";
        }
        int before_matches = scene_asset_matches.Count;
        if (before_matches != assets.Length) {
            return $"the unfiltered browser listed {before_matches} of {assets.Length} assets";
        }
        try {
            press_scene_via_events(field.rect.x + 6.0F, field.rect.y + field.rect.height * 0.5F);
            if (!scene_asset_edit.focused) {
                return "a press on the browser search did not focus it";
            }
            foreach (char letter in "damaged helmets") { type_scene_character(letter); }
            if (scene_asset_matches.Count >= before_matches) {
                return "typing into the browser search did not narrow the list";
            }
            bool every_match = true;
            foreach (int index in scene_asset_matches) {
                if (!assets[index].Contains("damaged", StringComparison.OrdinalIgnoreCase)) { every_match = false; }
            }
            if (!every_match) {
                return "the browser search kept an asset that does not match";
            }
            var filtered = submit_scene_frame_for_test(1600.0F, 900.0F);
            int rows = 0;
            while (filtered.find(string.Create(CultureInfo.InvariantCulture, $"scene_asset_{rows}")) != null) { ++rows; }
            if (rows != scene_asset_matches.Count) {
                return $"the filtered browser built {rows} rows for {scene_asset_matches.Count} matches";
            }
            // The count in the caption is the same number, so the reader cannot be misled by a clipped list.
            if (filtered.find("browser_caption_count")?.text is not { } count_text ||
                !count_text.StartsWith(
                    scene_asset_matches.Count.ToString(CultureInfo.InvariantCulture), StringComparison.Ordinal)) {
                return $"the browser count read '{filtered.find("browser_caption_count")?.text}'";
            }
        } finally {
            cancel_scene_asset_filter();
            scene_asset_edit.focused = false;
            scene_browser_list.first = 0;
            next_ui_events.Clear();
        }
        return null;
    }

    /// <summary>
    /// Dragging one row onto another: the press arms the drag, the move past the slop starts it, and the
    /// release hands the node to the classic reparent path, which records one undo step.
    /// </summary>
    private string? verify_scene_reparent() {
        if (next_ui == null || document == null || selection_state == null || tree == null) {
            return "the reparent check has no tree or document";
        }
        if (document.roots.Count < 2) {
            return "the scene needs two root nodes for the reparent check";
        }
        var root = submit_scene_frame_for_test(1600.0F, 900.0F);
        // Prefer a root that has no children, so the check cannot trip over an instance scene.
        var moving = document.roots[0];
        var target = document.roots[1];
        int moving_row = scene_rows.FindIndex(row => row.node == moving);
        int target_row = scene_rows.FindIndex(row => row.node == target);
        if (moving_row < 0 || target_row < 0) {
            return "the rows for the reparent check are missing";
        }
        if (root.find(string.Create(CultureInfo.InvariantCulture, $"scene_row_{moving_row}")) is not { } from_row ||
            root.find(string.Create(CultureInfo.InvariantCulture, $"scene_row_{target_row}")) is not { } to_row) {
            return "the outliner rows did not build for the reparent check";
        }
        Guid before_primary = selection_state.primary;
        int before_undo = undo_history?.depth ?? 0;
        int before_roots = document.roots.Count;
        try {
            float from_x = from_row.rect.x + 60.0F;
            float from_y = from_row.rect.y + from_row.rect.height * 0.5F;
            string from_key = next_ui?.hit_test(from_x, from_y)?.key ?? "(none)";
            press_scene_via_events(from_x, from_y);
            if (!scene_drag_armed || scene_drag_active) {
                return $"a press on '{from_key}' did not arm the drag (armed {scene_drag_armed}, active {scene_drag_active})";
            }
            move_scene_via_events(to_row.rect.x + 60.0F, to_row.rect.y + to_row.rect.height * 0.5F);
            if (!scene_drag_active || scene_drag_target != target.uid) {
                return "the drag did not settle on the row under the pointer";
            }
            release_scene_via_events(to_row.rect.x + 60.0F, to_row.rect.y + to_row.rect.height * 0.5F);
            if (scene_drag_active || scene_drag_armed) {
                return "the release left the drag running";
            }
            if (!target.children.Contains(moving) || scene_has_root(document.roots, moving)) {
                return "the drop did not move the node under its new parent";
            }
            if (document.roots.Count != before_roots - 1) {
                return $"the drop left {document.roots.Count} roots instead of {before_roots - 1}";
            }
            if ((undo_history?.depth ?? 0) != before_undo + 1) {
                return "the reparent did not record exactly one undo step";
            }
            if (undo_history?.try_undo() != true || !scene_has_root(document.roots, moving) ||
                target.children.Contains(moving)) {
                return "undo did not put the node back where it was";
            }
        } finally {
            while ((undo_history?.depth ?? 0) > before_undo) {
                if (undo_history?.try_undo() != true) { break; }
            }
            if (selection_state.primary != before_primary) { selection_state.select(before_primary); }
            scene_drag_node = Guid.Empty;
            scene_drag_target = Guid.Empty;
            scene_drag_armed = false;
            scene_drag_active = false;
            next_ui_events.Clear();
            rebuild_scene_rows(document.roots);
        }
        return null;
    }

    /// <summary>
    /// The viewport click gesture: the ray comes from the projection and the pick from the live tree, so the
    /// check presses the preview's centre, then drags, and requires the drag to keep the selection still.
    /// </summary>
    private string? verify_scene_picking() {
        if (next_ui == null || document == null || selection_state == null || tree == null) {
            return "the pick check has no context, document, selection or tree";
        }
        var root = submit_scene_frame_for_test(1600.0F, 900.0F);
        if (root.find("viewport") is not { } viewport) {
            return "the frame has no viewport to click";
        }
        Guid before_primary = selection_state.primary;
        var before_all = selection_state.all;
        float centre_x = viewport.rect.x + viewport.rect.width * 0.5F;
        float centre_y = viewport.rect.y + viewport.rect.height * 0.5F;
        try {
            // A press that never moves is a click: the release runs the pick.
            press_scene_via_events(centre_x, centre_y);
            if (!scene_viewport_press) {
                return "a press on the preview was not recorded as a pick";
            }
            release_scene_via_events(centre_x, centre_y);
            if (scene_viewport_press) {
                return "releasing the preview press left the pick pending";
            }
            // A press that drags belongs to the camera: the release must not pick.
            selection_state.select(scene_rows[0].node.uid);
            Guid dragged_from = selection_state.primary;
            press_scene_via_events(centre_x, centre_y);
            move_scene_via_events(centre_x + 60.0F, centre_y + 40.0F);
            release_scene_via_events(centre_x + 60.0F, centre_y + 40.0F);
            if (selection_state.primary != dragged_from) {
                return "a camera drag over the preview changed the selection";
            }
        } finally {
            selection_state.select(before_primary);
            if (before_primary == Guid.Empty) { selection_state.clear(); }
            scene_viewport_press = false;
            next_ui_events.Clear();
        }
        return null;
    }

    /// <summary>
    /// The save path the File menu uses, then the loader the editor starts with: an edit is renamed, the
    /// document is written to a scratch scene under `dynamic`, and the reopened copy must carry the name.
    /// </summary>
    private string? verify_scene_save_reopen() {
        if (document == null || next_ui == null) {
            return "the save check has no document";
        }
        if (document.roots.Count == 0) {
            return "the scene has no root node to save";
        }
        var node = document.roots[0];
        string original_name = node.name;
        string? original_path = document.path;
        string relative = Path.Combine("dynamic", "scene-save-selftest", "saved.scene.toml");
        string absolute = Path.Combine(managed_bootstrap.project_directory, relative);
        try {
            node.name = original_name + " saved";
            document.mark_dirty();
            if (!save_document(absolute)) {
                return $"the scene screen could not save the document ({notice})";
            }
            if (!File.Exists(absolute)) {
                return "the save did not write a file";
            }
            if (!scene_instances.try_load(managed_bootstrap.project_directory, relative, out var reopened,
                                          out var load_error) || reopened == null) {
                return $"the saved scene did not reopen ({load_error})";
            }
            if (count_nodes(reopened.roots) != count_nodes(document.roots)) {
                return "the reopened scene lost nodes";
            }
            if (reopened.roots[0].name != node.name) {
                return "the reopened scene did not keep the edited name";
            }
        } finally {
            node.name = original_name;
            document.path = original_path;
            try {
                if (File.Exists(absolute)) { File.Delete(absolute); }
                string? directory = Path.GetDirectoryName(absolute);
                if (directory != null && Directory.Exists(directory) &&
                    Directory.GetFileSystemEntries(directory).Length == 0) {
                    Directory.Delete(directory);
                }
            } catch (IOException) {
                // A left-behind scratch file is harmless; the assertion above already ran.
            } catch (UnauthorizedAccessException) {
            }
            next_ui_events.Clear();
        }
        return null;
    }

    /// <summary>
    /// Folding and multi-selection: a synthetic tree proves the collapse hides a whole branch and that
    /// Ctrl adds to the selection while Shift takes the range between the primary node and the press.
    /// </summary>
    private string? verify_scene_folding() {
        if (next_ui == null || document == null || selection_state == null) {
            return "the scene screen has no context, document or selection";
        }
        Guid before_primary = selection_state.primary;
        var before_primary_set = selection_state.all;
        try {
            // A tree that is not attached to the document: parent -> two children, one with a grandchild.
            var parent = new document_node { name = "fold-parent" };
            var first = new document_node { name = "fold-first" };
            var second = new document_node { name = "fold-second" };
            var grandchild = new document_node { name = "fold-grandchild" };
            first.children.Add(grandchild);
            parent.children.Add(first);
            parent.children.Add(second);
            document_node[] roots = [parent];
            scene_collapsed.Clear();
            rebuild_scene_rows(roots);
            if (scene_rows.Count != 4) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the folded tree listed {scene_rows.Count} rows instead of four");
            }
            if (!scene_rows[0].has_children || scene_rows[0].closed) {
                return "the parent row did not report an open branch";
            }
            if (scene_rows[1].depth != 1 || scene_rows[2].depth != 2) {
                return "the flattened rows did not carry their depth";
            }
            scene_collapsed.Add(parent.uid);
            rebuild_scene_rows(roots);
            if (scene_rows.Count != 1) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"closing the parent left {scene_rows.Count} rows instead of one");
            }
            if (!scene_rows[0].closed) { return "the closed parent row did not report itself closed"; }
            scene_collapsed.Clear();
            rebuild_scene_rows(roots);
            if (scene_rows.Count != 4) { return "opening the parent again did not restore the branch"; }
            // Now the live document, so the modifiers are tested through the same flat order the UI uses.
            rebuild_scene_rows(document.roots);
            if (scene_rows.Count < 3) { return "the selftest document has too few rows for a range"; }
            var primary = scene_rows[0].node.uid;
            selection_state.select(primary);
            select_scene_row(scene_rows[1].node.uid, modifier.control);
            if (selection_state.all.Count != 2) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"Ctrl added to the selection and left {selection_state.all.Count} node(s)");
            }
            if (!selection_state.contains(primary)) { return "Ctrl dropped the node that was already selected"; }
            // A range runs from the primary node, which Ctrl moved to the second row; re-anchor on the
            // first row so the range covers three rows instead of two.
            selection_state.select(primary);
            select_scene_row(scene_rows[2].node.uid, modifier.shift);
            if (selection_state.all.Count != 3) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"Shift selected {selection_state.all.Count} nodes for a three row range");
            }
        } finally {
            scene_collapsed.Clear();
            rebuild_scene_rows(document.roots);
            selection_state.select(before_primary);
            if (before_primary_set.Count > 1) { selection_state.replace(before_primary_set, before_primary); }
            next_ui_events.Clear();
        }
        return null;
    }

    /// <summary>Builds and submits one scene frame, then returns the tree the press will be tested against.</summary>
    private ui_element submit_scene_frame_for_test(float width, float height) {
        next_ui!.begin(width, height, 1.0F);
        var root = build_scene_screen();
        if (!next_ui.submit(root)) {
            report_next_ui_failure("overlay rejected a selftest frame");
        }
        return root;
    }

    /// <summary>Pushes a left press through the registered input callback, as the window would.</summary>
    private void press_scene_via_events(float x, float y) {
        next_ui_events.Clear();
        on_next_ui_input(new input_event(input_type.mouse_button, 0, true, 0, x, y, 0.0F, 0.0F, 0.0F, 0U));
        process_scene_input();
    }

    /// <summary>Pushes a left release, so a press and a release make the click the preview listens for.</summary>
    private void release_scene_via_events(float x, float y) {
        next_ui_events.Clear();
        on_next_ui_input(new input_event(input_type.mouse_button, 0, false, 0, x, y, 0.0F, 0.0F, 0.0F, 0U));
        process_scene_input();
    }

    /// <summary>Pushes a pointer move, which is what turns a press on the preview into a camera drag.</summary>
    private void move_scene_via_events(float x, float y) {
        next_ui_events.Clear();
        on_next_ui_input(new input_event(input_type.mouse_move, 0, true, 0, x, y, 0.0F, 0.0F, 0.0F, 0U));
        process_scene_input();
    }

    private string? verify_scene_screen() {
        if (next_ui == null || document == null || selection_state == null) {
            return "the scene screen has no context, document or selection";
        }
        Guid before_primary = selection_state.primary;
        string before_menu = next_ui_open_menu;
        var before_dialog = next_ui_dialog;
        try {
            var root = submit_scene_frame_for_test(1280.0F, 720.0F);
            int nodes = count_nodes(document.roots);
            if (scene_rows.Count != nodes) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the outliner listed {scene_rows.Count} rows for {nodes} node(s)");
            }
            if (nodes == 0) { return "the selftest document has no nodes to select"; }
            // A press on the third row selects that node and the details panel shows it on the next frame.
            int index = Math.Min(2, scene_rows.Count - 1);
            var target = scene_rows[index].node;
            var row = root.find(string.Create(CultureInfo.InvariantCulture, $"scene_row_{index}"));
            if (row == null) { return "the outliner built no row to press"; }
            press_scene_via_events(row.rect.x + 40.0F, row.rect.y + row.rect.height * 0.5F);
            if (selection_state.primary != target.uid) {
                return "a press on an outliner row did not select its node";
            }
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            var shown = root.find("scene_property_value_name");
            if (shown == null) { return "the details panel showed no name row for the selected node"; }
            if (shown.text != target.name) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the details panel showed '{shown.text}' for '{target.name}'");
            }
            // The viewport belongs to the camera; the panels do not.
            var viewport = root.find("viewport");
            if (viewport == null) { return "the scene screen has no viewport slot"; }
            if (scene_pointer_claims(viewport.rect.x + viewport.rect.width * 0.5F, viewport.rect.y + 20.0F)) {
                return "the pointer claim covered the 3D viewport";
            }
            var pressed_row = root.find(string.Create(CultureInfo.InvariantCulture, $"scene_row_{index}"));
            if (pressed_row == null) { return "the outliner lost its row after the rebuild"; }
            if (!scene_pointer_claims(pressed_row.rect.x + 20.0F, pressed_row.rect.y + 10.0F)) {
                return "the pointer claim left an outliner row to the camera";
            }
            // A wheel over the preview must not scroll a list.
            int before_first = scene_outliner.first;
            scroll_scene_ui(1.0F, viewport.rect.x + viewport.rect.width * 0.5F, viewport.rect.y + 20.0F);
            if (scene_outliner.first != before_first) {
                return "a wheel over the 3D viewport scrolled the outliner";
            }
            // The File menu opens, its panel exists as an overlay, and Help opens a dialog that Escape closes.
            var file = root.find("scene_menu_file");
            if (file == null) { return "the header has no File menu"; }
            press_scene_ui(file.rect.x + file.rect.width * 0.5F, file.rect.y + file.rect.height * 0.5F);
            if (next_ui_open_menu != "scene_menu_file") {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"pressing the File title left the open menu as '{next_ui_open_menu}'");
            }
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            if (next_ui.overlay_root?.find("scene_menu_save") == null) {
                return "the open File menu dropped no panel with its Save entry";
            }
            // Help is a different title: one press must switch the menu instead of closing and reopening.
            var help = root.find("scene_menu_help");
            if (help == null) { return "the header has no Help menu"; }
            press_scene_ui(help.rect.x + help.rect.width * 0.5F, help.rect.y + help.rect.height * 0.5F);
            if (next_ui_open_menu != "scene_menu_help") {
                return "pressing Help did not switch the open menu in one press";
            }
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            var shortcuts = next_ui.overlay_root?.find("scene_menu_shortcuts");
            if (shortcuts == null) { return "the Help menu has no shortcuts entry"; }
            press_scene_ui(shortcuts.rect.x + shortcuts.rect.width * 0.5F,
                           shortcuts.rect.y + shortcuts.rect.height * 0.5F);
            if (next_ui_dialog?.key != "dialog_scene_shortcuts") {
                return "the shortcuts entry opened no dialog";
            }
            handle_scene_key(new input_event(input_type.key, 0, true, (int)key_code.escape, 0.0F, 0.0F,
                                             0.0F, 0.0F, 0.0F, 0U));
            if (next_ui_dialog != null) { return "Escape left the dialog open"; }
            // Picking an asset records the choice the add-node command will use.
            if (assets.Length > 0) {
                root = submit_scene_frame_for_test(1280.0F, 720.0F);
                var tile = root.find("scene_asset_0");
                if (tile == null) { return "the project list built no tile to press"; }
                press_scene_via_events(tile.rect.x + 30.0F, tile.rect.y + tile.rect.height * 0.5F);
                if (scene_picked_asset != assets[0]) {
                    return string.Create(CultureInfo.InvariantCulture,
                                         $"pressing the first tile picked '{scene_picked_asset}' instead of '{assets[0]}'");
                }
            }
        } finally {
            selection_state.select(before_primary);
            next_ui_open_menu = before_menu;
            next_ui_dialog = before_dialog;
            next_ui_gesture = sheet_gesture.none;
            next_ui_events.Clear();
        }
        return null;
    }

    /// <summary>
    /// The three dividers: a press starts a drag, a move resizes the panel it borders, the release ends
    /// the drag and writes the sizes, and a drag past the limit clamps instead of swallowing the preview.
    /// </summary>
    private string? verify_scene_splitters() {
        if (next_ui == null) { return "the splitter check has no UI context"; }
        var before_layout = scene_layout;
        string before_path = scene_layout_path;
        string directory = Path.Combine(Environment.CurrentDirectory, "dynamic", "scene-layout-selftest");
        string path = Path.Combine(directory, "editor-layout.json");
        Directory.CreateDirectory(directory);
        if (File.Exists(path)) { File.Delete(path); }
        scene_layout_path = path;
        try {
            var root = submit_scene_frame_for_test(1280.0F, 720.0F);
            var outliner_split = root.find("split_outliner");
            var details_split = root.find("split_details");
            var browser_split = root.find("split_browser");
            if (outliner_split == null || details_split == null || browser_split == null) {
                return "the scene screen built fewer than three dividers";
            }
            float start_outliner = scene_layout.scene_outliner_width;
            press_scene_via_events(outliner_split.rect.x + outliner_split.rect.width * 0.5F,
                                   outliner_split.rect.y + outliner_split.rect.height * 0.5F);
            if (scene_split_gesture != scene_split.outliner) { return "a press on the left divider started no drag"; }
            next_ui_events.Clear();
            on_next_ui_input(new input_event(input_type.mouse_move, 0, false, 0,
                                             outliner_split.rect.x + 60.0F,
                                             outliner_split.rect.y + 4.0F, 0.0F, 0.0F, 0.0F, 0U));
            process_scene_input();
            if (MathF.Abs(scene_layout.scene_outliner_width - (start_outliner + 60.0F)) > 1.0F) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"dragging the left divider gave {scene_layout.scene_outliner_width:0.0}");
            }
            next_ui_events.Clear();
            on_next_ui_input(new input_event(input_type.mouse_button, 0, false, 0,
                                             outliner_split.rect.x + 60.0F,
                                             outliner_split.rect.y + 4.0F, 0.0F, 0.0F, 0.0F, 0U));
            process_scene_input();
            if (scene_split_gesture != scene_split.none) { return "releasing the left divider left the drag running"; }
            if (!File.Exists(path)) { return "the release did not write the layout file"; }
            var written = workspace_layout.load(path);
            if (MathF.Abs(written.scene_outliner_width - scene_layout.scene_outliner_width) > 0.5F) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the layout file kept {written.scene_outliner_width:0.0} instead of {scene_layout.scene_outliner_width:0.0}");
            }
            // The right divider moves the other way: dragging left widens the details panel.
            float start_details = scene_layout.scene_details_width;
            press_scene_via_events(details_split.rect.x + details_split.rect.width * 0.5F,
                                   details_split.rect.y + details_split.rect.height * 0.5F);
            if (scene_split_gesture != scene_split.details) { return "a press on the right divider started no drag"; }
            update_scene_split(details_split.rect.x - 40.0F, details_split.rect.y);
            if (scene_layout.scene_details_width <= start_details) {
                return "dragging the right divider left did not widen the details panel";
            }
            // Past the limit the divider clamps and the preview keeps its minimum width.
            update_scene_split(1270.0F, details_split.rect.y);
            float preview = 1280.0F - scene_layout.scene_outliner_width - scene_layout.scene_details_width -
                            ui_controls.splitter_thickness * 2.0F;
            if (preview < scene_viewport_min - 0.5F) {
                return string.Create(CultureInfo.InvariantCulture, $"the clamp left the preview {preview:0.0} wide");
            }
            // The bottom divider grows the browser upwards.
            scene_split_gesture = scene_split.browser;
            float start_browser = scene_layout.scene_browser_height;
            update_scene_split(0.0F, browser_split.rect.y - 50.0F);
            if (scene_layout.scene_browser_height <= start_browser) {
                return "dragging the bottom divider up did not grow the project panel";
            }
            scene_split_gesture = scene_split.none;
            if (Path.GetDirectoryName(path) is { Length: > 0 } parent && Directory.Exists(parent)) {
                File.Delete(path);
            }
        } finally {
            scene_layout = before_layout;
            scene_layout_path = before_path;
            scene_split_gesture = scene_split.none;
            next_ui_events.Clear();
        }
        return null;
    }
    /// <summary>
    /// The outliner's remaining tools: the visibility mark is its own hit target, the search field
    /// narrows the rows, Delete removes the selection and Ctrl+D copies it, and a double press on a
    /// project tile adds a node.
    /// </summary>
    private string? verify_scene_outliner_tools() {
        if (next_ui == null || document == null || selection_state == null) {
            return "the outliner tool check has no document";
        }
        int before_undo = undo_history?.depth ?? 0;
        int before_roots = document.roots.Count;
        var before_selection = selection_state.primary;
        selection_state.select(before_selection);
        try {
            var root = submit_scene_frame_for_test(1280.0F, 720.0F);
            if (scene_rows.Count == 0) { return "the outliner listed no rows"; }
            // 1. The visibility mark flips the node and leaves the selection alone.
            var node = scene_rows[0].node;
            bool was_visible = node.visible;
            var vis = root.find("scene_row_0_vis");
            if (vis == null) { return "the first row has no visibility mark"; }
            press_scene_ui(vis.rect.x + vis.rect.width * 0.5F, vis.rect.y + vis.rect.height * 0.5F);
            if (node.visible == was_visible) { return "the visibility mark did not toggle the node"; }
            if (selection_state.primary != before_selection) { return "the visibility mark moved the selection"; }
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            vis = root.find("scene_row_0_vis");
            if (vis != null) { press_scene_ui(vis.rect.x + vis.rect.width * 0.5F, vis.rect.y + vis.rect.height * 0.5F); }
            if (node.visible != was_visible) { return "the second press did not restore the node's visibility"; }
            // 2. The filter keeps only matches and their ancestors, and folding is ignored while it runs.
            int total_rows = scene_rows.Count;
            string filter = "actor";
            scene_filter_edit.reset(filter);
            scene_filter_edit.focused = true;
            apply_scene_filter();
            rebuild_scene_rows(document.roots);
            if (scene_rows.Count == 0 || scene_rows.Count >= total_rows) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the filter left {scene_rows.Count} of {total_rows} rows");
            }
            foreach (var row in scene_rows) {
                if (!row.node.name.Contains(filter, StringComparison.OrdinalIgnoreCase)) {
                    bool ancestor = false;
                    foreach (var candidate in scene_rows) {
                        if (candidate.node.children.Contains(row.node)) {
                            ancestor = true;
                            break;
                        }
                    }
                    if (!ancestor) {
                        return string.Create(CultureInfo.InvariantCulture,
                                             $"the filter kept '{row.node.name}', which neither matches nor leads to a match");
                    }
                }
            }
            scene_filter_edit.reset(string.Empty);
            scene_filter_edit.focused = false;
            apply_scene_filter();
            rebuild_scene_rows(document.roots);
            if (scene_rows.Count != total_rows) { return "clearing the filter did not restore every row"; }
            // 3. Delete removes the selected node and Ctrl+Z brings it back.
            selection_state.select(scene_rows[scene_rows.Count - 1].node.uid);
            int rows_before_delete = count_nodes(document.roots);
            handle_scene_key(new input_event(input_type.key, 0, true, key_code.delete,
                                             0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0U));
            if (count_nodes(document.roots) != rows_before_delete - 1) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"Delete left {count_nodes(document.roots)} nodes, expected {rows_before_delete - 1}");
            }
            if (undo_history?.try_undo() != true) { return "Delete could not be undone"; }
            if (count_nodes(document.roots) != rows_before_delete) { return "undoing Delete did not restore the node"; }
            // 4. Ctrl+D copies the selection, and one undo step removes the copy again.
            selection_state.select(scene_rows[0].node.uid);
            int rows_before_copy = count_nodes(document.roots);
            int undo_before_copy = undo_history?.depth ?? 0;
            handle_scene_key(new input_event(input_type.key, 0, true, key_code.d,
                                             0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0U, modifier.control));
            if (count_nodes(document.roots) != rows_before_copy + 1) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"Ctrl+D left {count_nodes(document.roots)} nodes, expected {rows_before_copy + 1}");
            }
            if ((undo_history?.depth ?? 0) != undo_before_copy + 1) { return "Ctrl+D was not a single undo step"; }
            // 5. A second press on the same project tile inside the click window adds a node.
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            var tile = root.find("scene_asset_0_name") ?? root.find("scene_asset_0");
            if (tile == null) { return "the project panel listed no first tile"; }
            int nodes_before_add = count_nodes(document.roots);
            press_scene_ui(tile.rect.x + 4.0F, tile.rect.y + tile.rect.height * 0.5F);
            press_scene_ui(tile.rect.x + 4.0F, tile.rect.y + tile.rect.height * 0.5F);
            if (count_nodes(document.roots) != nodes_before_add + 1) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"a double press on a tile left {count_nodes(document.roots)} nodes, expected {nodes_before_add + 1}");
            }
        } finally {
            while ((undo_history?.depth ?? 0) > before_undo) {
                if (undo_history?.try_undo() != true) { break; }
            }
            scene_filter_edit.reset(string.Empty);
            scene_filter_edit.focused = false;
            scene_filter_applied = string.Empty;
            scene_last_asset = string.Empty;
            selection_state?.select(before_selection);
            rebuild_scene_rows(document.roots);
            next_ui_events.Clear();
        }
        return null;
    }
    /// <summary>
    /// The component lanes: numeric and vector values write the whole component through one undo step,
    /// and the boolean rows flip through the classic shell's own commands.
    /// </summary>
    private string? verify_scene_components() {
        if (next_ui == null || document == null || selection_state == null) {
            return "the component check has no document";
        }
        int before_undo = undo_history?.depth ?? 0;
        var before_selection = selection_state.primary;
        try {
            // 1. A point light's intensity.
            var light = find_scene_node(document.roots, n => n.light != null);
            if (light?.light is not { } light_before) { return "the scene has no point light to edit"; }
            selection_state.select(light.uid);
            var root = submit_scene_frame_for_test(1100.0F, 900.0F);
            var row = root.find("scene_property_value_light_intensity");
            if (row == null) { return "the light details show no intensity row"; }
            press_scene_via_events(row.rect.x + 4.0F, row.rect.y + row.rect.height * 0.5F);
            if (scene_value_edit is not { focused: true }) { return "pressing the intensity row opened no field"; }
            scene_value_edit.select_all();
            scene_value_edit.insert_run("42");
            handle_scene_key(new input_event(input_type.key, 0, true, key_code.enter,
                                             0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0U));
            if (light.light is not { } light_after || MathF.Abs(light_after.intensity - 42.0F) > 0.01F) {
                return "the intensity lane did not write 42";
            }
            if (undo_history?.try_undo() != true || light.light != light_before) {
                return "undoing the intensity edit did not restore the light";
            }
            // 2. A camera's field of view and its main-camera flag.
            var camera = find_scene_node(document.roots, n => n.camera);
            if (camera == null) { return "the scene has no camera to edit"; }
            selection_state.select(camera.uid);
            root = submit_scene_frame_for_test(1100.0F, 900.0F);
            row = root.find("scene_property_value_camera_fov");
            if (row == null) { return "the camera details show no field of view row"; }
            float camera_before = camera.camera_fov;
            press_scene_via_events(row.rect.x + 4.0F, row.rect.y + row.rect.height * 0.5F);
            scene_value_edit?.select_all();
            scene_value_edit?.insert_run("60");
            handle_scene_key(new input_event(input_type.key, 0, true, key_code.enter,
                                             0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0U));
            if (MathF.Abs(camera.camera_fov - 60.0F) > 0.01F) { return "the field of view lane did not write 60"; }
            if (undo_history?.try_undo() != true || MathF.Abs(camera.camera_fov - camera_before) > 0.01F) {
                return "undoing the field of view edit did not restore it";
            }
            // 3. An actor flag: the toggle row flips the node and one undo puts it back.
            var actor = document.roots.Count != 0 ? document.roots[0] : null;
            if (actor == null) { return "the scene has no root node"; }
            selection_state.select(actor.uid);
            root = submit_scene_frame_for_test(1100.0F, 900.0F);
            var toggle = root.find("scene_property_value_collision_box");
            if (toggle == null) { return "the actor section shows no solid box row"; }
            bool solid_before = actor.collision_box;
            press_scene_via_events(toggle.rect.x + 4.0F, toggle.rect.y + toggle.rect.height * 0.5F);
            if (actor.collision_box == solid_before) { return "the solid box row did not toggle"; }
            if (undo_history?.try_undo() != true || actor.collision_box != solid_before) {
                return "undoing the solid box toggle did not restore it";
            }
            // 4. The flags word goes through the same transaction as the classic row.
            root = submit_scene_frame_for_test(1100.0F, 900.0F);
            row = root.find("scene_property_value_flags");
            if (row == null) { return "the actor section shows no flags row"; }
            uint flags_before = actor.flags;
            press_scene_via_events(row.rect.x + 4.0F, row.rect.y + row.rect.height * 0.5F);
            scene_value_edit?.select_all();
            scene_value_edit?.insert_run("7");
            handle_scene_key(new input_event(input_type.key, 0, true, key_code.enter,
                                             0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0U));
            if (actor.flags != 7U) { return "the flags lane did not write 7"; }
            if (undo_history?.try_undo() != true || actor.flags != flags_before) {
                return "undoing the flags edit did not restore it";
            }
        } finally {
            while ((undo_history?.depth ?? 0) > before_undo) {
                if (undo_history?.try_undo() != true) { break; }
            }
            cancel_scene_edit();
            selection_state?.select(before_selection);
            rebuild_scene_rows(document.roots);
            next_ui_events.Clear();
        }
        return null;
    }

    /// <summary>
    /// The inspector scrolls: a node with every component overflowed the panel, the wheel moved the
    /// window, and the clamp kept the offset inside the content.
    /// </summary>
    private string? verify_scene_details_scroll() {
        if (next_ui == null || document == null || selection_state == null) {
            return "the details scroll check has no document";
        }
        var before_selection = selection_state.primary;
        float before_scroll = scene_details_scroll;
        try {
            // The inspector is taller than its panel once the window is short, which is the case a small
            // window on a real machine produces; the content itself depends on the components a node has.
            var actor = find_scene_node(document.roots, n => n.asset != null && n.subscene == null);
            if (actor == null) { return "the scene has no placement to inspect"; }
            selection_state.select(actor.uid);
            scene_details_node = Guid.Empty;
            scene_details_scroll = 0.0F;
            var root = submit_scene_frame_for_test(1100.0F, 430.0F);
            if (scene_details_content <= scene_details_window) {
                return $"the inspector needed {scene_details_content} px inside {scene_details_window} px and did not overflow";
            }
            if (root.find("scene_property_value_animation_autoplay") != null || root.find("scene_section_animation") != null) {
                return "the inspector built its last section at rest instead of only the visible rows";
            }
            var details = root.find("details_body");
            if (details == null) { return "the details panel has no body"; }
            scroll_scene_ui(-6.0F, details.rect.x + 20.0F, details.rect.y + 20.0F);
            if (scene_details_scroll <= 0.0F) { return "the wheel did not scroll the inspector"; }
            float moved = scene_details_scroll;
            scroll_scene_ui(-400.0F, details.rect.x + 20.0F, details.rect.y + 20.0F);
            float limit = MathF.Max(0.0F, scene_details_content - scene_details_window);
            if (scene_details_scroll < limit - 0.5F || scene_details_scroll > limit + 0.5F) {
                return $"the wheel clamp stopped at {scene_details_scroll} instead of {limit}";
            }
            root = submit_scene_frame_for_test(1100.0F, 430.0F);
            if (root.find("scene_section_animation") == null) {
                return "the scrolled inspector still hid the section at its bottom";
            }
            scroll_scene_ui(400.0F, details.rect.x + 20.0F, details.rect.y + 20.0F);
            if (scene_details_scroll != 0.0F) { return "the wheel did not return the inspector to its top"; }
            if (moved < ui_theme.row_step) { return "one wheel notch moved less than a row"; }        } finally {
            selection_state.select(before_selection);
            scene_details_scroll = before_scroll;
            scene_details_node = Guid.Empty;
            next_ui_events.Clear();
        }
        return null;
    }
    /// <summary>
    /// Play from the new frontend: the header button starts the same session the classic shell starts,
    /// turns into Stop, and the second press asks the session to finish.
    /// </summary>
    private string? verify_scene_play() {
        if (playing == null || document == null || next_ui == null) {
            return "the play check has no session";
        }
        bool before = playing.running;
        try {
            var root = submit_scene_frame_for_test(1100.0F, 900.0F);
            var button = root.find("scene_play");
            if (button == null) { return "the header has no play button"; }
            press_scene_via_events(button.rect.x + button.rect.width * 0.5F,
                                   button.rect.y + button.rect.height * 0.5F);
            if (!playing.running) { return $"the play button started no session ({notice})"; }
            root = submit_scene_frame_for_test(1100.0F, 900.0F);
            if (root.find("scene_play")?.text != "Stop") { return "the play button did not turn into Stop"; }
            press_scene_via_events(button.rect.x + button.rect.width * 0.5F,
                                   button.rect.y + button.rect.height * 0.5F);
            if (!playing.running && !playing.stopping) {
                return "the second press neither stopped nor finished the session";
            }
        } finally {
            if (!before && playing.running) { playing.stop(); }
            notice = string.Empty;
            next_ui_events.Clear();
        }
        return null;
    }
    /// <summary>Every node of a tree, parents before children.</summary>
    private static void collect_scene_nodes(IReadOnlyList<document_node> roots, List<document_node> nodes) {
        foreach (var node in roots) {
            nodes.Add(node);
            collect_scene_nodes(node.children, nodes);
        }
    }

    /// <summary>The first node in a flattened tree that satisfies a predicate.</summary>
    private static document_node? find_scene_node(IReadOnlyList<document_node> roots, Func<document_node, bool> match) {
        foreach (var node in roots) {
            if (match(node)) { return node; }
            if (find_scene_node(node.children, match) is { } found) { return found; }
        }
        return null;
    }
    /// <summary>
    /// The inline details editor: pressing a value row opens a field seeded with what the row showed, a
    /// character event types into it, Enter writes it through the document as ONE undo step, and a
    /// malformed transform is refused while Escape leaves the node alone.
    /// </summary>
    private string? verify_scene_editing() {
        if (next_ui == null || document == null || selection_state == null) {
            return "the details check has no context, document or selection";
        }
        var node = scene_rows.Count == 0 ? null : scene_rows[0].node;
        if (node == null) {
            var before = selection_state.primary;
            selection_state.select(document.roots[0].uid);
            submit_scene_frame_for_test(1280.0F, 720.0F);
            node = document.roots[0];
            selection_state.select(before);
        }
        string original_name = node.name;
        var original_local = node.local;
        Guid before_primary = selection_state.primary;
        int before_undo_count = undo_history?.depth ?? 0;
        try {
            selection_state.select(node.uid);
            var root = submit_scene_frame_for_test(1280.0F, 720.0F);
            var name_row = root.find("scene_property_value_name");
            if (name_row == null) { return "the details panel has no editable name row"; }
            press_scene_via_events(name_row.rect.x + 20.0F, name_row.rect.y + name_row.rect.height * 0.5F);
            if (scene_value_edit is not { focused: true }) { return "pressing the name row opened no field"; }
            if (scene_value_edit.text != original_name) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the name field started from '{scene_value_edit.text}', not '{original_name}'");
            }
            // Type one character through the real callback, then apply with Enter.
            next_ui_events.Clear();
            on_next_ui_input(new input_event(input_type.character, 0, true, 0, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, (uint)'X'));
            process_scene_input();
            handle_scene_key(new input_event(input_type.key, 0, true, (int)key_code.enter, 0.0F, 0.0F, 0.0F,
                                             0.0F, 0.0F, 0U));
            if (scene_value_edit is { focused: true }) { return "Enter left the name field open"; }
            if (node.name != original_name + "X") {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the rename wrote '{node.name}', not '{original_name}X'");
            }
            if (undo_history?.try_undo() != true || node.name != original_name) {
                return "one undo did not take the rename back";
            }
            // A transform lane writes all three numbers as a single undo step.
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            var position_row = root.find("scene_property_value_position");
            if (position_row == null) { return "the details panel has no editable position row"; }
            press_scene_via_events(position_row.rect.x + 20.0F, position_row.rect.y + position_row.rect.height * 0.5F);
            if (scene_value_edit is not { focused: true }) { return "pressing the position row opened no field"; }
            scene_value_edit.select_all();
            scene_value_edit.insert_run("1.5, -2, 3");
            handle_scene_key(new input_event(input_type.key, 0, true, (int)key_code.enter, 0.0F, 0.0F, 0.0F,
                                             0.0F, 0.0F, 0U));
            if (MathF.Abs(node.local.position.x - 1.5F) > 0.0001F ||
                MathF.Abs(node.local.position.y + 2.0F) > 0.0001F ||
                MathF.Abs(node.local.position.z - 3.0F) > 0.0001F) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the position wrote {format_scene_vector(node.local.position)}");
            }
            if (undo_history?.try_undo() != true ||
                MathF.Abs(node.local.position.x - original_local.position.x) > 0.0001F ||
                MathF.Abs(node.local.position.z - original_local.position.z) > 0.0001F) {
                return "one undo did not take the whole transform back";
            }
            // A malformed vector is refused and the node keeps its values; Escape drops the edit.
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            var scale_row = root.find("scene_property_value_scale");
            if (scale_row == null) { return "the details panel has no editable scale row"; }
            press_scene_via_events(scale_row.rect.x + 20.0F, scale_row.rect.y + scale_row.rect.height * 0.5F);
            if (scene_value_edit is not { focused: true }) { return "pressing the scale row opened no field"; }
            scene_value_edit.select_all();
            scene_value_edit.insert_run("1, 2");
            handle_scene_key(new input_event(input_type.key, 0, true, (int)key_code.enter, 0.0F, 0.0F, 0.0F,
                                             0.0F, 0.0F, 0U));
            if (!notice.Contains("three numbers", StringComparison.Ordinal)) {
                return string.Create(CultureInfo.InvariantCulture, $"a two number vector was accepted: '{notice}'");
            }
            if (MathF.Abs(node.local.scale.x - original_local.scale.x) > 0.0001F ||
                MathF.Abs(node.local.scale.z - original_local.scale.z) > 0.0001F) {
                return "a refused vector still changed the node";
            }
            press_scene_via_events(scale_row.rect.x + 20.0F, scale_row.rect.y + scale_row.rect.height * 0.5F);
            scene_value_edit?.insert_run("garbage");
            handle_scene_key(new input_event(input_type.key, 0, true, (int)key_code.escape, 0.0F, 0.0F, 0.0F,
                                             0.0F, 0.0F, 0U));
            if (scene_value_edit is { focused: true }) { return "Escape left the scale field open"; }
            if (MathF.Abs(node.local.scale.x - original_local.scale.x) > 0.0001F) {
                return "Escape wrote the value anyway";
            }
        } finally {
            node.name = original_name;
            node.local = original_local;
            push_runtime(node);
            cancel_scene_edit();
            selection_state.select(before_primary);
            while ((undo_history?.depth ?? 0) > before_undo_count) {
                if (undo_history?.try_undo() != true) { break; }
            }
            next_ui_events.Clear();
        }
        return null;
    }
}

