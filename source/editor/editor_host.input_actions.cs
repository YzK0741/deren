using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;
using deren;

namespace deren.editor {
    internal sealed unsafe partial class editor_host {
        private SortedDictionary<string, List<string>> action_bindings = new(StringComparer.Ordinal);
        private string[] action_rows = [];
        private string? input_manifest;
        private string? selected_action;
        private string? input_key_action;
        private int? pending_input_key;
        private string new_action_name = string.Empty;

        private void show_input_actions() {
            try {
                var manifest = File.ReadAllText(project_manifest_path);
                if (!input_actions_file.try_read(manifest, out var actions, out var error)) {
                    ui?.set_text("project_settings_message", error);
                    return;
                }
                input_manifest = manifest;
                action_bindings = actions;
                selected_action = action_bindings.Keys.FirstOrDefault();
                new_action_name = string.Empty;
                ui?.set_value("input_action_name", "");
                ui?.set_text("input_actions_message", "");
                render_input_actions();
                open_project_panel = project_panel.input_actions;
                ui?.try_set_element_style("project_settings_prompt", "display", "none");
                ui?.try_set_element_style("input_actions_prompt", "display", "flex");
            } catch (Exception failure) when (failure is IOException or UnauthorizedAccessException) {
                ui?.set_text("project_settings_message", $"Cannot open input actions: {failure.Message}");
            }
        }

        private void render_input_actions() {
            action_rows = [.. action_bindings.Keys];
            var rows = new StringBuilder();
            for (int index = 0; index < action_rows.Length; ++index) {
                var name = action_rows[index];
                string active = name == selected_action ? " project_scene_selected" : "";
                string summary = string.Join(", ", action_bindings[name]);
                rows.Append($"""<button id="input_row_{index}" class="project_scene_row{active}"><span>{WebUtility.HtmlEncode(name)}</span><span class="input_key_summary">{WebUtility.HtmlEncode(summary)}</span></button>""");
            }
            ui?.try_set_markup("input_actions_list", rows.ToString());
            var keys = new StringBuilder();
            if (selected_action != null && action_bindings.TryGetValue(selected_action, out var bindings)) {
                for (int index = 0; index < bindings.Count; ++index) {
                    keys.Append($"""<button id="input_key_{index}" class="input_key_chip">{WebUtility.HtmlEncode(bindings[index])} ×</button>""");
                }
            }
            ui?.try_set_markup("input_action_keys", keys.ToString());
            ui?.try_set_element_style("input_action_edit", "display", selected_action == null ? "none" : "block");
            ui?.set_text("input_selected_action", selected_action ?? "");
        }

        private void handle_input_actions(rml_event input) {
            if (input.type == rml_event_type.change && input.id == "input_action_name") {
                new_action_name = input.value;
                return;
            }
            if (input.type != rml_event_type.click) { return; }
            input_key_action = null;
            pending_input_key = null;
            ui?.set_text("input_actions_message", "");
            if (try_index(input.id, "input_row_", action_rows.Length, out int row)) {
                selected_action = action_rows[row];
            } else if (selected_action != null && action_bindings.TryGetValue(selected_action, out var keys) &&
                       try_index(input.id, "input_key_", keys.Count, out int key)) {
                keys.RemoveAt(key);
            } else {
                switch (input.id) {
                    case "input_action_add":
                        string name = new_action_name.Trim();
                        if (name.Length == 0 || name[0] is not (>= 'a' and <= 'z') ||
                            name.Any(character => character is not (>= 'a' and <= 'z' or >= '0' and <= '9' or '_'))) {
                            ui?.set_text("input_actions_message", "Use a lower snake_case action name, such as open_door.");
                            return;
                        }
                        if (!action_bindings.TryAdd(name, [])) {
                            ui?.set_text("input_actions_message", "An action with that name already exists.");
                            return;
                        }
                        selected_action = name;
                        new_action_name = string.Empty;
                        ui?.set_value("input_action_name", "");
                        break;
                    case "input_action_record":
                        input_key_action = selected_action;
                        ui?.set_text("input_actions_message", "Press a key to add a binding. Esc cancels.");
                        return;
                    case "input_action_remove":
                        if (selected_action != null) { action_bindings.Remove(selected_action); }
                        selected_action = action_bindings.Keys.FirstOrDefault();
                        break;
                    case "input_actions_save":
                        if (input_manifest == null) { return; }
                        if (!input_actions_file.try_save(project_manifest_path, input_manifest, action_bindings, out var error)) {
                            ui?.set_text("input_actions_message", error);
                            return;
                        }
                        return_to_project_settings("Input actions saved. Changes apply on the next Play or game launch.");
                        return;
                    case "input_actions_cancel":
                        return_to_project_settings("");
                        return;
                    default:
                        return;
                }
            }
            render_input_actions();
        }

        private void return_to_project_settings(string message) {
            input_manifest = null;
            open_project_panel = project_panel.settings;
            ui?.try_set_element_style("input_actions_prompt", "display", "none");
            ui?.try_set_element_style("project_settings_prompt", "display", "flex");
            ui?.set_text("project_settings_message", message);
        }

        private bool capture_input_binding(input_event input) {
            if (input_key_action == null || input.type != input_type.key) { return false; }
            if (input.down) { pending_input_key ??= input.key; }
            return true;
        }

        private void finish_input_capture() {
            if (input_key_action is not { } name || pending_input_key is not { } key) { return; }
            pending_input_key = null;
            if (key == key_code.escape) {
                input_key_action = null;
                ui?.set_text("input_actions_message", "Recording cancelled.");
                return;
            }
            if (input_actions_file.key_name(key) is not { } binding) {
                ui?.set_text("input_actions_message", "Use a letter, number, arrow, Space, Enter, Tab, Shift or Ctrl.");
                return;
            }
            input_key_action = null;
            var keys = action_bindings[name];
            if (!keys.Contains(binding)) { keys.Add(binding); }
            ui?.set_text("input_actions_message", "");
            render_input_actions();
        }

        private static bool try_index(string id, string prefix, int count, out int index) {
            index = -1;
            return id.StartsWith(prefix, StringComparison.Ordinal) &&
                int.TryParse(id.AsSpan(prefix.Length), out index) && (uint)index < (uint)count;
        }
    }
}
