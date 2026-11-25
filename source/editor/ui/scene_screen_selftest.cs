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
            case 12: {
                // The browser's pages: a short page size makes the list span pages, the arrow buttons
                // walk them and a row keeps the absolute asset index the press needs.
                if (verify_scene_browser_pages() is { } page_problem) {
                    return selftest_failure(page_problem);
                }
                host_logger.write(
                    "Editor selftest: the browser paged through the project and every row kept its asset.");
                this.scene_selftest_phase = 13;
                return script_status.success;
            }
            case 13: {
                // The status line: path, dirty flag, node count, the selected node, then the notice.
                if (verify_scene_status_line() is { } status_problem) {
                    return selftest_failure(status_problem);
                }
                host_logger.write(
                    "Editor selftest: the footer showed the path, the dirty flag, the nodes, the selection and the notice.");
                this.scene_selftest_phase = 14;
                return script_status.success;
            }
            case 14: {
                // The output tab: the log the host fills is readable, a line naming a node selects it,
                // and Clear empties the tab without touching the document.
                if (verify_scene_output_tab() is { } output_problem) {
                    return selftest_failure(output_problem);
                }
                host_logger.write(
                    "Editor selftest: the output tab listed the log, followed a line to its node and cleared.");
                this.scene_selftest_phase = 15;
                return script_status.success;
            }
            case 15: {
                // The environment rows: a lane row lists the maps, a pick goes through the host's own
                // commands, and the lane shows up in the inspector only while the document carries it.
                if (verify_scene_environment() is { } environment_problem) {
                    return selftest_failure(environment_problem);
                }
                host_logger.write(
                    "Editor selftest: the environment rows listed the maps and a lane lived as one undo step.");
                this.scene_selftest_phase = 16;
                return script_status.success;
            }
            case 16: {
                // The script rows: the binding lists the catalogue, a listed parameter applies as one undo
                // step, and a text parameter edits in place.
                if (verify_scene_script() is { } script_problem) {
                    return selftest_failure(script_problem);
                }
                host_logger.write(
                    "Editor selftest: the script rows listed the catalogue and each parameter was one undo step.");
                this.scene_selftest_phase = 17;
                return script_status.success;
            }
            case 17: {
                // The area headers: each leaf names its editor type, the type cell opens the catalogue,
                // and the last cell maximizes one area and restores the tree.
                if (verify_scene_area_header() is { } area_problem) {
                    return selftest_failure(area_problem);
                }
                host_logger.write(
                    "Editor selftest: the area headers listed the editor types and maximized one area.");
                this.scene_selftest_phase = 18;
                return script_status.success;
            }
            case 18: {
                // The workspace strip and the tree tools: a workspace is added, renamed, switched and
                // saved, the Dope Sheet runs as an area type, and an area splits and joins again.
                if (verify_scene_workspaces() is { } workspace_problem) {
                    return selftest_failure(workspace_problem);
                }
                host_logger.write(
                    "Editor selftest: the workspace strip added, renamed and saved an arrangement, the Dope Sheet ran as an area and an area split and joined.");
                this.scene_selftest_phase = 19;
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
            // "helmet" is the part both helmet assets share, so the filter must keep exactly those.
            foreach (char letter in "helmet") { type_scene_character(letter); }
            if (scene_asset_matches.Count == 0 || scene_asset_matches.Count >= before_matches) {
                return "typing into the browser search did not narrow the list";
            }
            bool every_match = true;
            foreach (var entry in scene_asset_matches) {
                if (entry.directory || !assets[entry.asset_index].Contains("helmet", StringComparison.OrdinalIgnoreCase)) {
                    every_match = false;
                }
            }
            if (!every_match) {
                return "the browser search kept an asset that does not match";
            }
            var filtered = submit_scene_frame_for_test(1600.0F, 900.0F);
            // Every row the page claims to show has to exist in the frame, keyed by its absolute asset
            // index so the press that follows names the asset and not the row number.
            for (var index = 0; index < scene_browser_shown; ++index) {
                var entry = scene_asset_matches[scene_browser_first + index];
                string key = entry.directory
                    ? string.Create(CultureInfo.InvariantCulture, $"scene_folder_{scene_browser_first + index}")
                    : string.Create(CultureInfo.InvariantCulture, $"scene_asset_{entry.asset_index}");
                if (filtered.find(key) == null) {
                    return $"the filtered browser did not build the row '{key}'";
                }
            }
            // The count in the caption names the slice on screen, so a clipped list cannot mislead.
            if (filtered.find("browser_caption_count")?.text is not { } count_text ||
                !count_text.EndsWith(
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
    /// The bottom dock's second tab: the output log renders newest first, a line that names a node
    /// walks the selection there, and Clear empties the tab.
    /// </summary>
    private string? verify_scene_output_tab() {
        if (next_ui == null || document == null || selection_state == null) {
            return "the output tab check has no document";
        }
        if (document.roots.Count == 0) {
            return "the output tab check needs a node";
        }
        var node = document.roots[0];
        int before_tab = scene_bottom_tab;
        int before_first = scene_output_list.first;
        var before_primary = selection_state.primary;
        try {
            output_log.add(Guid.Empty, "Editor selftest: a plain line", false);
            output_log.add(node.uid, "Editor selftest: a line about " + node.name, false);
            scene_bottom_tab = 1;
            scene_output_list.first = 0;
            var root = submit_scene_frame_for_test(1600.0F, 900.0F);
            var count_text = root.find("output_caption_count")?.text;
            if (count_text == null ||
                !count_text.StartsWith(output_log.count.ToString(CultureInfo.InvariantCulture),
                                       StringComparison.Ordinal)) {
                return $"the output caption read '{count_text}' for {output_log.count} lines";
            }
            // Newest first: the node-tagged line is the last one queued, so its id is count - 1.
            string newest = string.Create(CultureInfo.InvariantCulture, $"output_line_{output_log.count - 1}");
            if (root.find(newest + "_text") == null) {
                return "the output tab did not build its newest line";
            }
            if (selection_state.primary != node.uid) {
                selection_state.select(Guid.Empty);
            }
            if (root.find(newest) is not { } line_row) {
                return "the output tab has no row to press";
            }
            press_scene_via_events(line_row.rect.x + 20.0F, line_row.rect.y + line_row.rect.height * 0.5F);
            if (selection_state.primary != node.uid) {
                return "pressing a node line did not select that node";
            }
            var cleared_root = submit_scene_frame_for_test(1600.0F, 900.0F);
            if (cleared_root.find("output_clear") is not { } clear_button) {
                return "the output tab has no Clear button";
            }
            press_scene_via_events(clear_button.rect.x + clear_button.rect.width * 0.5F,
                                   clear_button.rect.y + clear_button.rect.height * 0.5F);
            if (output_log.count != 0) {
                return $"Clear left {output_log.count} line(s) in the log";
            }
            var empty_root = submit_scene_frame_for_test(1600.0F, 900.0F);
            if (empty_root.find("output_empty") == null) {
                return "the empty log tab did not say it was empty";
            }
        } finally {
            scene_bottom_tab = before_tab;
            scene_output_list.first = before_first;
            selection_state.select(before_primary);
            next_ui_events.Clear();
        }
        return null;
    }

    /// <summary>
    /// The script rows: the binding row lists the catalogue, a listed parameter applies as one undo step,
    /// a text parameter edits in place, and the node's own values come back after the undos.
    /// </summary>
    private string? verify_scene_script() {
        if (next_ui == null || document == null || selection_state == null) {
            return "the script check has no document";
        }
        var node = find_scene_node(document.roots, candidate => candidate.script != null);
        if (node == null) {
            return "no node in the scene carries a script";
        }
        if (editable_scripts == null || editable_scripts.entries.Count == 0) {
            return "the script catalogue is empty, so the rows cannot be checked";
        }
        var definition = scene_script_definition(node);
        if (definition == null || definition.parameters.Length == 0) {
            return "the script of the selected node declares no parameters";
        }
        int boolean_index = -1;
        int text_index = -1;
        for (var index = 0; index < definition.parameters.Length; ++index) {
            var kind = definition.parameters[index].kind;
            if (kind == parameter_kind.boolean && boolean_index < 0) { boolean_index = index; }
            if (kind is parameter_kind.integer or parameter_kind.number or parameter_kind.vector && text_index < 0) {
                text_index = index;
            }
        }
        if (boolean_index < 0 || text_index < 0) {
            return "the script needs one boolean and one text parameter to check both row kinds";
        }

        var before_primary = selection_state.primary;
        string? before_script = node.script;
        var before_values = new SortedDictionary<string, string>(node.script_parameters, StringComparer.Ordinal);
        int before_undo = undo_history?.depth ?? 0;
        string? problem = null;
        try {
            selection_state.select(node.uid);
            scene_details_node = Guid.Empty;
            var root = submit_scene_frame_for_test(1600.0F, 900.0F);

            // The binding row opens the catalogue: "No script" first, then one entry per definition.
            var picker = root.find("scene_script_picker_value");
            if (picker == null) {
                problem = "the inspector has no script row";
            } else {
                press_scene_via_events(picker.rect.x + 20.0F, picker.rect.y + picker.rect.height * 0.5F);
                if (scene_combo_target != "script" || scene_combo_items.Count < 2 ||
                    scene_combo_items[0].value.Length != 0) {
                    problem = "the script row did not open the catalogue";
                } else if (scene_combo_items.Count != editable_scripts.entries.Count + 1) {
                    problem = "the script list did not carry every definition";
                } else {
                    pick_scene_combo(string.Empty);
                    if (node.script != null) { problem = "picking No script left the binding in place"; }
                }
            }
            if (problem == null) {
                if (undo_history?.try_undo() != true || node.script != before_script) {
                    problem = "one undo did not bring the script binding back";
                }
            }

            if (problem == null) {
                // A listed parameter is a row that opens a list of the values its kind allows.
                scene_details_node = Guid.Empty;
                root = submit_scene_frame_for_test(1600.0F, 900.0F);
                string key = string.Create(CultureInfo.InvariantCulture, $"scene_script_param_{boolean_index}_value");
                if (root.find(key) is not { } choice) {
                    problem = "the boolean parameter has no list row";
                } else {
                    press_scene_via_events(choice.rect.x + 20.0F, choice.rect.y + choice.rect.height * 0.5F);
                    if (!scene_combo_open || scene_combo_target !=
                        string.Create(CultureInfo.InvariantCulture, $"script_param_{boolean_index}")) {
                        problem = "the parameter row did not open its list";
                    } else {
                        string current = node.script_parameters.GetValueOrDefault(definition.parameters[boolean_index].name) ?? string.Empty;
                        pick_scene_combo(string.Equals(current, "True", StringComparison.OrdinalIgnoreCase) ? "false" : "true");
                        if (node.script_parameters.GetValueOrDefault(definition.parameters[boolean_index].name) == null) {
                            problem = "picking a value did not write the parameter";
                        }
                    }
                }
            }
            if (problem == null) {
                if (undo_history?.try_undo() != true ||
                    !string.Equals(node.script_parameters.GetValueOrDefault(definition.parameters[boolean_index].name),
                                   before_values.GetValueOrDefault(definition.parameters[boolean_index].name),
                                   StringComparison.Ordinal)) {
                    problem = "one undo did not restore the parameter";
                }
            }

            if (problem == null) {
                // A text parameter edits in place, exactly like a transform lane.
                scene_details_node = Guid.Empty;
                root = submit_scene_frame_for_test(1600.0F, 900.0F);
                string key = string.Create(CultureInfo.InvariantCulture, $"scene_property_value_script_{text_index}");
                if (root.find(key) is not { } field) {
                    problem = "the text parameter has no value row";
                } else {
                    press_scene_via_events(field.rect.x + 12.0F, field.rect.y + field.rect.height * 0.5F);
                    if (scene_value_edit is not { focused: true }) {
                        problem = "a press on the text parameter did not open its field";
                    } else {
                        scene_value_edit.select_all();
                        scene_value_edit.insert_run("7");
                        handle_scene_key(new input_event(input_type.key, 0, true, (int)key_code.enter,
                                                         0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0U));
                        string name = definition.parameters[text_index].name;
                        if (node.script_parameters.GetValueOrDefault(name) != "7") {
                            problem = "the typed value did not reach the parameter";
                        } else if (undo_history?.try_undo() != true ||
                                   !string.Equals(node.script_parameters.GetValueOrDefault(name),
                                                  before_values.GetValueOrDefault(name), StringComparison.Ordinal)) {
                            problem = "one undo did not restore the typed parameter";
                        }
                    }
                }
            }
        } finally {
            scene_collapsed.Clear();
            cancel_scene_edit();
            while ((undo_history?.depth ?? 0) > before_undo) {
                if (undo_history?.try_undo() != true) { break; }
            }
            node.script = before_script;
            node.script_parameters = before_values;
            push_runtime(node);
            selection_state.select(before_primary == Guid.Empty ? node.uid : before_primary);
            scene_details_node = Guid.Empty;
            next_ui_events.Clear();
        }
        return problem;
    }

    /// <summary>
    /// The environment rows: a press opens the map list under the row, the list starts with the entry that
    /// means "no map", a second press closes it, a lane written through the host's own command appears as a
    /// row until one undo removes it, and the per-object row offers the scene map first.
    /// </summary>
    private string? verify_scene_environment() {
        if (next_ui == null || document == null || selection_state == null) {
            return "the environment check has no document";
        }
        var node = find_scene_node(document.roots, candidate => candidate.asset != null && candidate.subscene == null);
        if (node == null) {
            return "the environment check needs a node with an asset";
        }
        var before_primary = selection_state.primary;
        var before_lanes = (string?[])document.environment_slots.Clone();
        int before_undo = undo_history?.depth ?? 0;
        try {
            selection_state.select(node.uid);
            scene_details_node = Guid.Empty;
            scene_details_scroll = 0.0F;
            var root = submit_scene_frame_for_test(1600.0F, 900.0F);
            // The environment section is the last one, so the panel has to reach its bottom to build it.
            scene_details_scroll = MathF.Max(0.0F, scene_details_content - scene_details_window);
            root = submit_scene_frame_for_test(1600.0F, 900.0F);
            if (root.find("scene_env_scene") is not { } scene_row) {
                return "the inspector listed no scene map row";
            }
            press_scene_via_events(scene_row.rect.x + 40.0F, scene_row.rect.y + scene_row.rect.height * 0.5F);
            if (!scene_combo_open) {
                return "pressing the scene map row opened no list";
            }
            if (scene_combo_items.Count < 2 || scene_combo_items[0].value != environment_panel.sky_value) {
                return "the map list did not start with the procedural sky";
            }
            submit_scene_frame_for_test(1600.0F, 900.0F);
            if (next_ui.overlay_root?.find("scene_combo_item_0") == null) {
                return "the map list was not in the frame it was opened for";
            }
            press_scene_via_events(scene_row.rect.x + 40.0F, scene_row.rect.y + scene_row.rect.height * 0.5F);
            if (scene_combo_open) {
                return "a second press on the row did not close the list";
            }
            apply_environment_text(1, "environments/selftest.hdr");
            scene_details_node = Guid.Empty;
            var lane_root = submit_scene_frame_scrolled_to_bottom(1600.0F, 900.0F);
            if (lane_root.find("scene_env_lane_1_name") == null) {
                return "a lane the document carries had no row";
            }
            if (undo_history?.try_undo() != true) {
                return "the lane edit recorded no undo step";
            }
            scene_details_node = Guid.Empty;
            var undone_root = submit_scene_frame_scrolled_to_bottom(1600.0F, 900.0F);
            if (undone_root.find("scene_env_lane_1_name") != null) {
                return "undo left the lane row in the inspector";
            }
            var object_root = submit_scene_frame_scrolled_to_bottom(1600.0F, 900.0F);
            if (object_root.find("scene_env_object") is not { } object_row) {
                return "the inspector listed no object map row";
            }
            // The row is the last one in the panel, so at the maximum scroll its bottom half sits at the
            // panel edge: the press goes into the part that is really on screen, as a reader would click.
            var panel_rect = object_root.find("details_panel")?.rect ?? new ui_rect(0.0F, 0.0F, 1600.0F, 900.0F);
            float click_y = Math.Clamp(object_row.rect.y + object_row.rect.height * 0.5F,
                                       panel_rect.y + 2.0F, panel_rect.y + panel_rect.height - 2.0F);
            press_scene_via_events(object_row.rect.x + 40.0F, click_y);
            if (!scene_combo_open || scene_combo_items[0].value != environment_panel.scene_value) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the object list did not start with the scene map (open {scene_combo_open}, row y {object_row.rect.y:0.0}, click {click_y:0.0}, panel {panel_rect.y:0.0}+{panel_rect.height:0.0})");
            }
            handle_scene_key(new input_event(input_type.key, 0, true, (int)key_code.escape,
                                             0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0U));
            if (scene_combo_open) {
                return "Escape left the object list open";
            }
        } finally {
            close_scene_combo();
            selection_state.select(before_primary);
            scene_details_node = Guid.Empty;
            scene_details_scroll = 0.0F;
            while ((undo_history?.depth ?? 0) > before_undo) {
                if (undo_history?.try_undo() != true) { break; }
            }
            for (var lane = 0; lane < document.environment_slots.Length; ++lane) {
                if (!string.Equals(document.environment_slots[lane], before_lanes[lane], StringComparison.Ordinal)) {
                    document.environment_slots[lane] = before_lanes[lane];
                }
            }
            sync_environment_lanes();
            next_ui_events.Clear();
        }
        return null;
    }

    /// <summary>
    /// The status footer: the classic single line, so the path, the dirty flag, the node count, the
    /// selected node and the notice are all readable at once instead of the notice replacing the line.
    /// </summary>
    private string? verify_scene_status_line() {
        if (next_ui == null || document == null || selection_state == null) {
            return "the status line check has no document";
        }
        if (document.roots.Count == 0) {
            return "the status line check needs a node";
        }
        var node = document.roots[0];
        var before_primary = selection_state.primary;
        bool before_dirty = document.dirty;
        string before_name = node.name;
        string before_notice = notice;
        string? problem = null;
        try {
            selection_state.select(node.uid);
            var text = submit_scene_frame_for_test(1600.0F, 900.0F).find("footer_text")?.text;
            if (text == null) {
                problem = "the footer has no text";
            } else if (!text.Contains(node.name, StringComparison.Ordinal)) {
                problem = $"the footer did not name the selection: '{text}'";
            } else if (!text.Contains($"nodes: {count_nodes(document.roots)}", StringComparison.Ordinal)) {
                problem = $"the footer did not count the nodes: '{text}'";
            } else if (document.dirty != text.Contains("*dirty*", StringComparison.Ordinal)) {
                problem = $"the footer's dirty flag disagreed with the document: '{text}'";
            } else {
                // An unsaved edit is what the flag has to show, so make one through the real mark.
                node.name = before_name + " unsaved";
                document.mark_dirty();
                var dirty_text = submit_scene_frame_for_test(1600.0F, 900.0F).find("footer_text")?.text;
                if (dirty_text == null || !dirty_text.Contains("*dirty*", StringComparison.Ordinal)) {
                    problem = $"the footer did not show the dirty flag: '{dirty_text}'";
                } else {
                    notice = "renamed";
                    var notice_text = submit_scene_frame_for_test(1600.0F, 900.0F).find("footer_text")?.text;
                    if (notice_text == null ||
                        !notice_text.EndsWith(" | renamed", StringComparison.Ordinal) ||
                        !notice_text.Contains("unsaved", StringComparison.Ordinal)) {
                        problem = $"the footer replaced the line with the notice: '{notice_text}'";
                    }
                }
            }
        } finally {
            notice = before_notice;
            node.name = before_name;
            // Undoing the edit through the document's own comparison is what restores the flag.
            document.mark_dirty();
            selection_state.select(before_primary);
            next_ui_events.Clear();
        }
        if (problem == null && !before_dirty && document.dirty) {
            return "the status line check could not restore the document's dirty flag";
        }
        return problem;
    }

    /// <summary>
    /// The browser pages: a short page size spreads the project over several pages, the arrow buttons
    /// walk them, the caption names the slice, and a row still carries its absolute asset index.
    /// </summary>
    private string? verify_scene_browser_pages() {
        if (next_ui == null || assets.Length < 3) {
            return "the browser page check needs three assets";
        }
        if (scene_folder_parent("a/b/c") != "a/b" || scene_folder_parent("a").Length != 0) {
            return "the folder helpers did not split a path";
        }
        int before_size = scene_browser_page_size;
        int before_page = scene_browser_page;
        int before_first = scene_browser_list.first;
        try {
            scene_browser_page_size = 2;
            scene_browser_page = 0;
            scene_browser_list.first = 0;
            rebuild_scene_browser_matches();
            if (scene_browser_page_count < 2) {
                return $"the browser fit {scene_asset_matches.Count} assets on one page of two";
            }
            var root = submit_scene_frame_for_test(1600.0F, 900.0F);
            if (root.find("browser_next") is not { } next_button) {
                return "the browser has no next-page button";
            }
            press_scene_via_events(next_button.rect.x + next_button.rect.width * 0.5F,
                                   next_button.rect.y + next_button.rect.height * 0.5F);
            if (scene_browser_page != 1) {
                return $"pressing next left the browser on page {scene_browser_page}";
            }
            var paged = submit_scene_frame_for_test(1600.0F, 900.0F);
            var first_entry = scene_asset_matches[scene_browser_first];
            string first_key = first_entry.directory
                ? string.Create(CultureInfo.InvariantCulture, $"scene_folder_{scene_browser_first}")
                : string.Create(CultureInfo.InvariantCulture, $"scene_asset_{first_entry.asset_index}");
            if (paged.find(first_key) == null) {
                return $"page two did not build its first row '{first_key}'";
            }
            if (paged.find("browser_caption_count")?.text is not { } count_text ||
                !count_text.StartsWith((scene_browser_first + 1).ToString(CultureInfo.InvariantCulture),
                                       StringComparison.Ordinal)) {
                return $"the page count read '{paged.find("browser_caption_count")?.text}'";
            }
            if (paged.find("browser_prev") is not { } prev_button) {
                return "the browser has no previous-page button";
            }
            press_scene_via_events(prev_button.rect.x + prev_button.rect.width * 0.5F,
                                   prev_button.rect.y + prev_button.rect.height * 0.5F);
            if (scene_browser_page != 0) {
                return $"pressing previous left the browser on page {scene_browser_page}";
            }
        } finally {
            scene_browser_page_size = before_size;
            scene_browser_page = before_page;
            scene_browser_list.first = before_first;
            rebuild_scene_browser_matches();
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

    /// <summary>
    /// Submits a frame with the inspector scrolled to its last row, for checks that need the sections at
    /// the bottom of a panel that is shorter than its content.
    /// </summary>
    private ui_element submit_scene_frame_scrolled_to_bottom(float width, float height) {
        submit_scene_frame_for_test(width, height);
        scene_details_scroll = MathF.Max(0.0F, scene_details_content - scene_details_window);
        return submit_scene_frame_for_test(width, height);
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
    /// The area tree's dividers: a press starts a drag, a move edits that split's share, the release writes
    /// the tree, a drag past the minimum clamps instead of hiding a panel, and the file round trips.
    /// </summary>
    private string? verify_scene_splitters() {
        if (next_ui == null) { return "the splitter check has no UI context"; }
        // The tree is edited in place, so the copy has to be a deep one: a reference would keep the
        // shares this phase moves.
        var before_root = scene_areas.root.clone();
        string before_path = scene_area_path;
        string directory = Path.Combine(Environment.CurrentDirectory, "dynamic", "scene-layout-selftest");
        string path = Path.Combine(directory, "editor-areas.json");
        Directory.CreateDirectory(directory);
        if (File.Exists(path)) { File.Delete(path); }
        scene_area_path = path;
        try {
            var root = submit_scene_frame_for_test(1280.0F, 720.0F);
            // The default tree is three splits: preview beside sidebar, the dock under the preview and
            // the outliner over the details.
            var outer = scene_areas.root;
            if (outer.children.Count != 2 || outer.children[0].children.Count != 2 ||
                outer.children[1].children.Count != 2) {
                return "the default area tree is not the three splits the screen expects";
            }
            var preview_column = outer.children[0];
            var sidebar_column = outer.children[1];
            var divider = root.find("area_split_0_0");
            if (divider == null) { return "the area tree built no divider between the preview and the sidebar"; }
            float before_share = outer.shares[0];
            press_scene_via_events(divider.rect.x + 2.0F, divider.rect.y + divider.rect.height * 0.5F);
            if (scene_area_drag != "area_split_0_0") { return "a press on the divider started no drag"; }
            next_ui_events.Clear();
            on_next_ui_input(new input_event(input_type.mouse_move, 0, false, 0,
                                             divider.rect.x - 60.0F, divider.rect.y + 4.0F,
                                             0.0F, 0.0F, 0.0F, 0U));
            process_scene_input();
            if (!(outer.shares[0] < before_share - 0.01F)) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"dragging the divider left gave a share of {outer.shares[0]:0.000}");
            }
            next_ui_events.Clear();
            on_next_ui_input(new input_event(input_type.mouse_button, 0, false, 0,
                                             divider.rect.x - 60.0F, divider.rect.y + 4.0F,
                                             0.0F, 0.0F, 0.0F, 0U));
            process_scene_input();
            if (scene_area_drag.Length != 0) { return "releasing the divider left the drag running"; }
            if (!File.Exists(path)) { return "the release did not write the area layout"; }
            if (MathF.Abs(area_layout.load(path).root.shares[0] - outer.shares[0]) > 0.005F) {
                return "the area layout file kept a different share than the tree";
            }
            // Past the limit the share clamps, so neither side can swallow the other.
            if (!begin_area_drag("area_split_0_0", 0.0F, 0.0F)) { return "the divider handle was not known"; }
            update_area_drag(-2000.0F, 0.0F);
            float narrow = 1280.0F * outer.shares[0];
            if (narrow < scene_viewport_min - 0.5F) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the clamp left the preview {narrow:0.0} wide");
            }
            update_area_drag(4000.0F, 0.0F);
            if (1280.0F * (1.0F - outer.shares[0]) < area_layout.area_min_panel_width - 0.5F) {
                return "the clamp let the divider swallow the sidebar";
            }
            // The sidebar's own divider moves the boundary between the outliner and the details.
            scene_area_drag = string.Empty;
            var inner = root.find("area_split_2_0");
            if (inner == null) { return "the sidebar built no divider between the outliner and the details"; }
            press_scene_via_events(inner.rect.x + inner.rect.width * 0.5F, inner.rect.y + 2.0F);
            if (scene_area_drag != "area_split_2_0") { return "a press on the sidebar divider started no drag"; }
            float before_top = sidebar_column.shares[0];
            update_area_drag(0.0F, inner.rect.y + 60.0F);
            if (MathF.Abs(sidebar_column.shares[0] - before_top) < 0.01F) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"dragging the sidebar divider changed neither half ({before_top:0.000} -> {sidebar_column.shares[0]:0.000})");
            }
            // The dock's divider sets the preview's own split, so dragging it up grows the project panel.
            scene_area_drag = string.Empty;
            var dock = root.find("area_split_1_0");
            if (dock == null) { return "the preview built no divider above the dock"; }
            if (!begin_area_drag("area_split_1_0", dock.rect.x + 2.0F, dock.rect.y + 2.0F)) {
                return "the dock divider handle was not known";
            }
            float before_dock = preview_column.shares[1];
            update_area_drag(0.0F, dock.rect.y - 50.0F);
            if (preview_column.shares[1] <= before_dock) {
                return "dragging the dock divider up did not grow the project panel";
            }
            scene_area_drag = string.Empty;
            if (Path.GetDirectoryName(path) is { Length: > 0 } parent && Directory.Exists(parent)) {
                File.Delete(path);
            }
        } finally {
            scene_areas.root = before_root;
            scene_area_path = before_path;
            scene_area_drag = string.Empty;
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
            // 5. A second press on the same project tile inside the click window adds a node. The tile of
            // the asset the first row already uses is chosen on purpose: adding a whole imported scene
            // would test the glTF importer here, not the double press.
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            string? used = scene_rows.Count > 0 ? scene_rows[0].node.asset : null;
            int tile_index = used == null ? 0 : Array.IndexOf(assets, used);
            if (tile_index < 0) { tile_index = 0; }
            var tile = root.find(string.Create(CultureInfo.InvariantCulture, $"scene_asset_{tile_index}_name"))
                       ?? root.find(string.Create(CultureInfo.InvariantCulture, $"scene_asset_{tile_index}"));
            if (tile == null) { return "the project panel listed no tile to add"; }
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
            if (root.find("scene_env_scene") != null || root.find("scene_section_environment") != null) {
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
            if (root.find("scene_env_scene") == null) {
                return "the scrolled inspector still hid the section at its bottom";
            }
            scroll_scene_ui(400.0F, details.rect.x + 20.0F, details.rect.y + 20.0F);
            // A tall inspector needs more than one notch to get back, and the clamp stops it at zero.
            for (var attempt = 0; attempt < 8 && scene_details_scroll > 0.0F; ++attempt) {
                scroll_scene_ui(400.0F, details.rect.x + 20.0F, details.rect.y + 20.0F);
            }
            if (scene_details_scroll != 0.0F) {
                return $"the wheel did not return the inspector to its top (it stopped at {scene_details_scroll} of {scene_details_content})";
            }
            if (moved < ui_theme.row_step) { return "one wheel notch moved less than a row"; }
        } finally {
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
            var button = root.find("toolbar_play");
            if (button == null) { return "the toolbar has no play button"; }
            press_scene_via_events(button.rect.x + button.rect.width * 0.5F,
                                   button.rect.y + button.rect.height * 0.5F);
            if (!playing.running) { return $"the play button started no session ({notice})"; }
            root = submit_scene_frame_for_test(1100.0F, 900.0F);
            if (root.find("toolbar_play")?.text != "Stop") { return "the play button did not turn into Stop"; }
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
    /// <summary>
    /// The area headers: every leaf names its editor type and carries a type cell plus a maximize cell.
    /// The type cell opens the catalogue in the shared overlay, a pick switches that leaf's kind, and the
    /// maximize cell swaps the tree for the one leaf and back.
    /// </summary>
    private string? verify_scene_area_header() {
        if (next_ui == null) {
            return "the area header check has no context";
        }
        var root = submit_scene_frame_for_test(1280.0F, 720.0F);
        string viewport_path = string.Empty;
        foreach (var entry in scene_area_leaves) {
            if (entry.Value.kind == area_kind.viewport) {
                viewport_path = entry.Key;
                break;
            }
        }
        if (viewport_path.Length == 0) {
            return "the layout held no viewport area";
        }
        var type_cell = root.find("area_type_" + viewport_path);
        if (type_cell == null) {
            return $"the viewport area {viewport_path} carried no type cell";
        }
        var maximized_before = scene_area_maximized;
        var tree_before = scene_areas.root.clone();
        try {
            press_scene_via_events(type_cell.rect.x + type_cell.rect.width * 0.5F,
                                   type_cell.rect.y + type_cell.rect.height * 0.5F);
            if (!scene_combo_open || scene_combo_target != "area_type|" + viewport_path) {
                return $"the type cell opened no catalogue (target '{scene_combo_target}', open {scene_combo_open})";
            }
            if (scene_combo_items.Count != area_kinds.all.Length) {
                return $"the catalogue listed {scene_combo_items.Count} of {area_kinds.all.Length} editor types";
            }
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            if (next_ui.overlay_root?.find("scene_combo_item_0") == null) {
                return "the open catalogue painted no item";
            }
            close_scene_combo();
            // Switch the viewport to another editor and back: the header must name what the leaf became.
            apply_scene_area_kind(viewport_path, area_kind.details);
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            if (root.find("area_type_" + viewport_path)?.text != "Details") {
                return "the header did not name the editor type the menu picked";
            }
            apply_scene_area_kind(viewport_path, area_kind.viewport);
            // Maximize: the tree gives way to the one leaf, whose own header then offers Restore.
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            var max_cell = root.find("area_max_" + viewport_path);
            if (max_cell == null) {
                return "the viewport area carried no maximize cell";
            }
            press_scene_via_events(max_cell.rect.x + max_cell.rect.width * 0.5F,
                                   max_cell.rect.y + max_cell.rect.height * 0.5F);
            if (scene_area_maximized == null) {
                return "the maximize cell maximized no area";
            }
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            if (root.find("area_split_0_0") != null) {
                return "the maximized frame still showed the dividers";
            }
            if (root.find("viewport") == null) {
                return "the maximized frame did not fill the window with the viewport";
            }
            var restore_cell = root.find("area_max_max");
            if (restore_cell == null) {
                return "the maximized area carried no restore cell";
            }
            press_scene_via_events(restore_cell.rect.x + restore_cell.rect.width * 0.5F,
                                   restore_cell.rect.y + restore_cell.rect.height * 0.5F);
            if (scene_area_maximized != null) {
                return "a press on Restore left the area maximized";
            }
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            if (root.find("area_split_0_0") == null) {
                return "the restored frame had no dividers";
            }
        } finally {
            scene_area_maximized = maximized_before;
            scene_areas.root = tree_before;
            save_scene_areas();
            close_scene_combo();
            notice = string.Empty;
            next_ui_events.Clear();
        }
        return null;
    }

    /// <summary>
    /// The workspace strip and the two tree tools: adding, switching, renaming and closing a workspace
    /// round-trips through the layout file, the Dope Sheet leaf paints its sheet and keeps a press out of
    /// the scene selection, and a split divides one area while the join merges it back into its sibling.
    /// </summary>
    private string? verify_scene_workspaces() {
        if (next_ui == null) {
            return "the workspace check has no context";
        }
        var workspaces_before = new List<(string name, area_node tree)>();
        foreach (var workspace in scene_areas.workspaces) {
            workspaces_before.Add((workspace.name, workspace.root.clone()));
        }
        int active_before = scene_areas.active_index;
        var maximized_before = scene_area_maximized;
        string temp = Path.Combine(Path.GetTempPath(), "deren-area-selftest.json");
        try {
            var root = submit_scene_frame_for_test(1280.0F, 720.0F);
            if (root.find("workspace_strip") == null) { return "the frame had no workspace strip"; }
            if (root.find("workspace_tab_0") == null) { return "the strip listed no workspace tab"; }
            int count_before = scene_areas.workspaces.Count;
            var new_button = root.find("workspace_new");
            if (new_button == null) { return "the strip carried no new button"; }
            press_scene_via_events(new_button.rect.x + new_button.rect.width * 0.5F,
                                   new_button.rect.y + new_button.rect.height * 0.5F);
            if (scene_areas.workspaces.Count != count_before + 1) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the new button left {scene_areas.workspaces.Count} workspace(s), not {count_before + 1}");
            }
            if (scene_areas.active_index != count_before) { return "adding a workspace did not switch to it"; }
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            string tab_key = string.Create(CultureInfo.InvariantCulture, $"workspace_tab_{count_before}");
            if (root.find(tab_key) == null) { return "the strip painted no tab for the added workspace"; }
            // The rename runs through the real field: the press opens it with the name selected, one
            // character replaces that selection, and Enter commits it.
            var rename_button = root.find("workspace_rename");
            if (rename_button == null) { return "the strip carried no rename button"; }
            press_scene_via_events(rename_button.rect.x + rename_button.rect.width * 0.5F,
                                   rename_button.rect.y + rename_button.rect.height * 0.5F);
            if (!scene_workspace_renaming) { return "the rename button opened no field"; }
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            if (root.find("workspace_name_field") == null) { return "the open rename painted no field"; }
            next_ui_events.Clear();
            on_next_ui_input(new input_event(input_type.character, 0, true, 0, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
                                             (uint)'D'));
            process_scene_input();
            if (scene_workspace_edit.text != "D") {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the rename field took '{scene_workspace_edit.text}', not 'D'");
            }
            handle_scene_key(new input_event(input_type.key, 0, true, (int)key_code.enter, 0.0F, 0.0F, 0.0F,
                                             0.0F, 0.0F, 0U));
            if (scene_workspace_renaming) { return "Enter left the rename field open"; }
            if (scene_areas.active != "D") {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the workspace kept the name '{scene_areas.active}', not 'D'");
            }
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            if (root.find(tab_key)?.text != "D") { return "the tab did not paint the new name"; }
            // The first tab switches back, and the layout file keeps both arrangements.
            var first_tab = root.find("workspace_tab_0");
            if (first_tab == null) { return "the strip lost its first tab"; }
            press_scene_via_events(first_tab.rect.x + first_tab.rect.width * 0.5F,
                                   first_tab.rect.y + first_tab.rect.height * 0.5F);
            if (scene_areas.active_index != 0) { return "the first tab did not switch the workspace"; }
            scene_areas.save(temp);
            var loaded = area_layout.load(temp);
            if (loaded.workspaces.Count != scene_areas.workspaces.Count) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the file kept {loaded.workspaces.Count} of {scene_areas.workspaces.Count} workspace(s)");
            }
            if (loaded.active != workspaces_before[0].name) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the file remembered '{loaded.active}' as active, not '{workspaces_before[0].name}'");
            }
            if (area_layout.leaves(loaded.root) != area_layout.leaves(scene_areas.root)) {
                return "the reloaded workspace held a different area count";
            }
            // The Dope Sheet is an area type like any other: it paints its sheet and its press stays
            // inside its own input bridge instead of reaching for a scene node.
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            string viewport_path = string.Empty;
            foreach (var entry in scene_area_leaves) {
                if (entry.Value.kind == area_kind.viewport) {
                    viewport_path = entry.Key;
                    break;
                }
            }
            if (viewport_path.Length == 0) { return "the layout held no viewport area"; }
            apply_scene_area_kind(viewport_path, area_kind.dope);
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            var dope = root.find("dope_panel");
            if (dope == null) { return "the Dope Sheet area painted no sheet"; }
            if (root.find("ruler_track") == null) { return "the Dope Sheet area painted no ruler"; }
            var primary_before = selection_state?.primary ?? Guid.Empty;
            press_scene_dope(dope.rect.x + 30.0F, dope.rect.y + 60.0F);
            if (selection_state != null && selection_state.primary != primary_before) {
                return "a press in the Dope Sheet area changed the scene selection";
            }
            // Split the preview and join it back: one more leaf, then the same count as before.
            apply_scene_area_kind(viewport_path, area_kind.viewport);
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            int leaves_before = area_layout.leaves(scene_areas.root);
            var divide_cell = root.find("area_divide_" + viewport_path);
            if (divide_cell == null) { return "the viewport header carried no split cell"; }
            press_scene_via_events(divide_cell.rect.x + divide_cell.rect.width * 0.5F,
                                   divide_cell.rect.y + divide_cell.rect.height * 0.5F);
            if (area_layout.leaves(scene_areas.root) != leaves_before + 1) {
                return "the split cell did not add an area";
            }
            root = submit_scene_frame_for_test(1280.0F, 720.0F);
            string child = viewport_path + ".0";
            var join_cell = root.find("area_join_" + child);
            if (join_cell == null) { return "the split area carried no join cell"; }
            press_scene_via_events(join_cell.rect.x + join_cell.rect.width * 0.5F,
                                   join_cell.rect.y + join_cell.rect.height * 0.5F);
            if (area_layout.leaves(scene_areas.root) != leaves_before) {
                return "the join cell did not merge the area back";
            }
        } finally {
            scene_area_maximized = maximized_before;
            scene_areas.workspaces.Clear();
            foreach (var (name, tree) in workspaces_before) {
                scene_areas.workspaces.Add(new area_workspace(name, tree));
            }
            scene_areas.active_index = active_before;
            save_scene_areas();
            cancel_scene_workspace_rename();
            notice = string.Empty;
            next_ui_events.Clear();
            try { File.Delete(temp); } catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
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

