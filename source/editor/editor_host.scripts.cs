using System;
using System.Collections.Generic;
using System.Linq;
using deren.scripting;

namespace deren.editor {
    internal sealed unsafe partial class editor_host {
        private void reload_scripts() {
            if (editable_scripts == null) {
                return;
            }
            editable_scripts.reload();
            scripts_panel?.invalidate();
            foreach (var error in editable_scripts.errors) {
                host_logger.write($"Editor scripts: {error}");
                output_log.add(Guid.Empty, error, true);
            }
        }

        private bool handle_script_edit(rml_event input) {
            if (document == null || scripts_panel == null ||
                document.find(selection_state?.primary ?? Guid.Empty) is not { } node) {
                return false;
            }
            if (input.id == "script_picker") {
                string? binding = input.value.Length == 0 ? null : input.value;
                if (binding != node.script && (binding == null || editable_scripts?.try_get(binding, out _) == true)) {
                    set_script_settings(node, "Set Script", binding, new(StringComparer.Ordinal));
                }
                return true;
            }
            if (scripts_panel.draft(input.id, input.value)) {
                return true;
            }
            return commit_script_parameter(node, input.id, input.value);
        }

        private void apply_script_draft(string id) {
            if (scripts_panel?.try_draft(id, out var value) == true &&
                document?.find(selection_state?.primary ?? Guid.Empty) is { } node) {
                commit_script_parameter(node, id, value);
            }
        }

        private bool commit_script_parameter(document_node node, string id, string raw) {
            if (document == null || scripts_panel?.try_field(id, out var field) != true || field == null) {
                return false;
            }
            string text = raw.Trim();
            if (text.Length != 0 && (!field.try_value(text, document, null, out _) ||
                (field.kind == parameter_kind.asset && !assets.Contains(text, StringComparer.Ordinal)))) {
                notice = $"Invalid value for {field.label}";
                return true;
            }

            var next = new SortedDictionary<string, string>(node.script_parameters, StringComparer.Ordinal);
            if (text.Length == 0 ? next.Remove(field.name) : next.GetValueOrDefault(field.name) != text) {
                if (text.Length != 0) {
                    next[field.name] = text;
                }
                set_script_settings(node, "Set " + field.label, node.script, next);
            }
            return true;
        }

        private void set_script_settings(document_node node, string label, string? binding,
                                         SortedDictionary<string, string> values) {
            string? previous_binding = node.script;
            var previous_values = node.script_parameters;
            bool apply(string? name, SortedDictionary<string, string> parameters) {
                node.script = name;
                node.script_parameters = parameters;
                mark_dirty_and_refresh();
                return true;
            }
            apply(binding, values);
            record_undo(label, () => apply(previous_binding, previous_values), () => apply(binding, values));
        }
    }
}
