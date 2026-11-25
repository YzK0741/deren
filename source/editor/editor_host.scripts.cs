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
                if (node.instance_parent != null) {
                    notice = "The script of an instance node comes from its source scene";
                    return true;
                }
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

            if (node.instance_root is { } instance && node.source_uid is { } source) {
                string? value = text.Length == 0 ? null : text;
                if (value != override_value(instance, source, field.name)) {
                    set_instance_override(instance, source, field.name, value, "Set " + field.label);
                }
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

        /// <summary>
        /// Restores the value the source scene authors, which removes the placement override.
        /// </summary>
        private void restore_script_parameter(string id) {
            if (scripts_panel?.try_field(id, out var field) != true || field == null ||
                document?.find(selection_state?.primary ?? Guid.Empty) is not { } node ||
                node.instance_root is not { } instance || node.source_uid is not { } source) {
                return;
            }
            if (override_value(instance, source, field.name) != null) {
                set_instance_override(instance, source, field.name, null, "Restore " + field.label);
            }
        }

        private static string? override_value(document_node instance, Guid source, string name) =>
            instance.instance_overrides != null && instance.instance_overrides.TryGetValue(source, out var values)
                ? values.GetValueOrDefault(name) : null;

        private static SortedDictionary<Guid, SortedDictionary<string, string>> clone_overrides(
            document_node instance) {
            var clone = new SortedDictionary<Guid, SortedDictionary<string, string>>();
            if (instance.instance_overrides == null) { return clone; }
            foreach (var (source, values) in instance.instance_overrides) {
                clone.Add(source, new SortedDictionary<string, string>(values, StringComparer.Ordinal));
            }
            return clone;
        }

        /// <summary>
        /// Replaces the placement overrides and re-derives every expanded node from the source
        /// snapshot, so the tree, the panel and the play runtime all see the same values.
        /// </summary>
        private static void apply_override_state(document_node instance,
                                                 SortedDictionary<Guid, SortedDictionary<string, string>> state) {
            instance.instance_overrides = state.Count == 0 ? null : state;
            if (instance.instance_node_ids == null) { return; }
            foreach (var (source, expanded) in instance.instance_node_ids) {
                if (find_uid(instance, expanded) is not { } node) { continue; }
                node.script_parameters = node.source_script_parameters is { } snapshot
                    ? new SortedDictionary<string, string>(snapshot, StringComparer.Ordinal)
                    : new SortedDictionary<string, string>(StringComparer.Ordinal);
                if (state.TryGetValue(source, out var values)) {
                    foreach (var (name, value) in values) { node.script_parameters[name] = value; }
                }
            }
        }

        private static document_node? find_uid(document_node root, Guid uid) {
            foreach (var child in root.children) {
                if (child.uid == uid) { return child; }
                if (find_uid(child, uid) is { } found) { return found; }
            }
            return null;
        }

        private void set_instance_override(document_node instance, Guid source, string name, string? value,
                                           string label) {
            var before = clone_overrides(instance);
            var after = clone_overrides(instance);
            if (value == null) {
                if (after.TryGetValue(source, out var existing)) {
                    existing.Remove(name);
                    if (existing.Count == 0) { after.Remove(source); }
                }
            } else {
                if (!after.TryGetValue(source, out var values)) {
                    values = new SortedDictionary<string, string>(StringComparer.Ordinal);
                    after.Add(source, values);
                }
                values[name] = value;
            }
            apply_override_state(instance, after);
            mark_dirty_and_refresh();
            record_undo(label,
                () => { apply_override_state(instance, before); mark_dirty_and_refresh(); return true; },
                () => { apply_override_state(instance, after); mark_dirty_and_refresh(); return true; });
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
