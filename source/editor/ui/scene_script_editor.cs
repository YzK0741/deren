using System;
using System.Collections.Generic;
using System.Globalization;
using deren.scripting;

namespace deren.editor;

/// <summary>
/// The behaviour (script) part of the scene inspector: the script a node carries, the parameters that
/// script declares, and the per-placement overrides of an expanded instance node.
/// </summary>
/// <remarks>
/// The classic shell built these rows in <c>script_inspector</c> and looked the field up again on every
/// message, because that panel owned the current node. This screen rebuilds its rows every frame, so a
/// row carries the parameter index and the host's own writers are called with the field it resolves to.
/// Nothing here duplicates the transaction: <see cref="set_script_settings"/> and
/// <see cref="set_instance_override"/> are the same ones the classic panel used, so a script change and an
/// override stay one undo step each.
/// </remarks>
internal sealed unsafe partial class editor_host {
    /// <summary>
    /// The script rows: the binding the node carries, one row per parameter it declares, and - for an
    /// expanded instance node - the source the values come from plus a restore button per overridden value.
    /// </summary>
    private void add_scene_script(scene_details_body column, document_node node) {
        add_scene_section(column, "script", "Script");
        var definition = scene_script_definition(node);
        string binding = node.script == null
            ? "No script"
            : definition == null ? "Missing: " + scene_script_label(node.script) : scene_script_label(node.script);
        if (node.instance_root != null) {
            add_scene_property(column, "script_source", "Source Scene", node.subscene ?? "(instance)");
            add_scene_property(column, "script_binding", "Script", binding);
        } else {
            add_scene_choice_row(column, "scene_script_picker", "Script", binding);
        }
        if (node.script != null && definition == null) {
            add_scene_property(column, "script_missing", "Missing", scene_script_label(node.script));
            return;
        }
        if (definition == null) { return; }

        for (var index = 0; index < definition.parameters.Length; ++index) {
            var field = definition.parameters[index];
            bool overridden = node.instance_root is { } instance && node.source_uid is { } source &&
                              override_value(instance, source, field.name) != null;
            string suffix = node.instance_root == null ? string.Empty : overridden ? "  (override)" : "  (from source)";
            string display = scene_script_display(node, field) + suffix;
            string key = string.Create(CultureInfo.InvariantCulture, $"scene_script_param_{index}");
            switch (field.kind) {
                case parameter_kind.boolean:
                case parameter_kind.node:
                case parameter_kind.asset:
                    add_scene_choice_row(column, key, field.label, display);
                    break;
                default:
                    add_scene_value_row(column, string.Create(CultureInfo.InvariantCulture, $"script_{index}"),
                                        field.label, node, scene_script_seed(node, field), display);
                    break;
            }
            if (overridden) {
                var row = ui_node.row(string.Create(CultureInfo.InvariantCulture, $"scene_script_restore_row_{index}"))
                                 .size(float.NaN, ui_theme.row_height).spacing(0.0F);
                row.add(ui_node.box("scene_script_restore_inset_" + index.ToString(CultureInfo.InvariantCulture))
                              .size(8.0F, 0.0F));
                row.add(ui_controls.button(string.Create(CultureInfo.InvariantCulture, $"scene_script_restore_{index}"),
                                           "From source", ui_button_style.subtle, next_ui, 90.0F,
                                           ui_theme.row_height - 2.0F));
                column.add(row, ui_theme.row_height);
            }
        }
    }

    /// <summary>
    /// Matches any child of a script row - the inset, the name and the value are all hit targets - and
    /// returns the combo target the row opens: "script" for the binding, "script_param_N" for a value.
    /// </summary>
    private static bool try_scene_script_key(string key, out string target) {
        target = string.Empty;
        if (!key.StartsWith("scene_script_", StringComparison.Ordinal) ||
            key.StartsWith("scene_script_restore_", StringComparison.Ordinal)) { return false; }
        foreach (var suffix in new[] { "_inset", "_name", "_value" }) {
            if (key.EndsWith(suffix, StringComparison.Ordinal)) {
                key = key[..^suffix.Length];
                break;
            }
        }
        if (key == "scene_script_picker") {
            target = "script";
            return true;
        }
        if (try_scene_index(key, "scene_script_param_", out var index)) {
            target = string.Create(CultureInfo.InvariantCulture, $"script_param_{index}");
            return true;
        }
        return false;
    }


    /// <summary>The definition of the script a node carries, or null when it is missing or unset.</summary>
    private script_definition? scene_script_definition(document_node node) {
        if (node.script == null || editable_scripts == null) { return null; }
        return editable_scripts.try_get(node.script, out var definition) ? definition : null;
    }

    /// <summary>
    /// The short name of one script row: the type after the last dot of the binding, which is what the
    /// classic picker listed beside the assembly name.
    /// </summary>
    private static string scene_script_label(string binding) {
        int colon = binding.LastIndexOf(':');
        string type = colon >= 0 && colon + 1 < binding.Length ? binding[(colon + 1)..] : binding;
        int dot = type.LastIndexOf('.');
        return dot >= 0 && dot + 1 < type.Length ? type[(dot + 1)..] : type;
    }

    /// <summary>The text a parameter row shows for the selected node.</summary>
    private string scene_script_display(document_node node, script_parameter field) {
        string value = node.script_parameters.GetValueOrDefault(field.name) ?? string.Empty;
        if (value.Length == 0) { return "(default)"; }
        if (field.kind != parameter_kind.node) { return value; }
        return Guid.TryParse(value, out var uid) && document?.find(uid) is { } target ? target.name : value;
    }

    /// <summary>The value an inline field opens with: what the row shows, or empty for the default.</summary>
    private static string scene_script_seed(document_node node, script_parameter field) =>
        node.script_parameters.GetValueOrDefault(field.name) ?? string.Empty;

    /// <summary>
    /// One list of choices for a parameter: the default first, then the values the kind allows. The
    /// classic panel used a select box for these three kinds and a text field for the rest.
    /// </summary>
    private List<(string label, string value)> scene_script_items(script_parameter field) {
        var items = new List<(string label, string value)> { ("(default)", string.Empty) };
        switch (field.kind) {
            case parameter_kind.boolean:
                items.Add(("true", "true"));
                items.Add(("false", "false"));
                break;
            case parameter_kind.asset:
                foreach (var id in assets) { items.Add((id, id)); }
                break;
            case parameter_kind.node:
                if (document != null) { append_scene_node_items(document.roots, string.Empty, items); }
                break;
        }
        return items;
    }

    private static void append_scene_node_items(IReadOnlyList<document_node> nodes, string prefix,
                                                List<(string label, string value)> items) {
        foreach (var node in nodes) {
            string label = prefix + (node.name.Length == 0 ? "(unnamed)" : node.name);
            items.Add((label, node.uid.ToString("D", CultureInfo.InvariantCulture)));
            append_scene_node_items(node.children, label + " / ", items);
        }
    }

    /// <summary>Writes one parameter value, through the override of an instance node or the node itself.</summary>
    private void apply_scene_script_value(document_node node, int index, string text) {
        var definition = scene_script_definition(node);
        if (document == null || definition == null || index < 0 || index >= definition.parameters.Length) {
            return;
        }
        var field = definition.parameters[index];
        string raw = text.Trim();
        if (raw.Length != 0 && (!field.try_value(raw, document, null, out _) ||
                                (field.kind == parameter_kind.asset && Array.IndexOf(assets, raw) < 0))) {
            notice = "Invalid value for " + field.label;
            return;
        }

        if (node.instance_root is { } instance && node.source_uid is { } source) {
            string? value = raw.Length == 0 ? null : raw;
            if (value != override_value(instance, source, field.name)) {
                set_instance_override(instance, source, field.name, value, "Set " + field.label);
            }
            return;
        }

        var next = new SortedDictionary<string, string>(node.script_parameters, StringComparer.Ordinal);
        bool changed = raw.Length == 0
            ? next.Remove(field.name)
            : next.GetValueOrDefault(field.name) != raw;
        if (changed) {
            if (raw.Length != 0) { next[field.name] = raw; }
            set_script_settings(node, "Set " + field.label, node.script, next);
        }
    }

    /// <summary>Removes a placement's override, so the value the source scene authors comes back.</summary>
    private void apply_scene_script_restore(document_node node, int index) {
        var definition = scene_script_definition(node);
        if (definition == null || index < 0 || index >= definition.parameters.Length) { return; }
        var field = definition.parameters[index];
        if (node.instance_root is not { } instance || node.source_uid is not { } source) { return; }
        if (override_value(instance, source, field.name) != null) {
            set_instance_override(instance, source, field.name, null, "Restore " + field.label);
        }
    }

    /// <summary>Binds another script to the node, or clears it; an instance node keeps its source's one.</summary>
    private void apply_scene_script_binding(document_node node, string binding) {
        if (node.instance_root != null) {
            notice = "The script of an instance node comes from its source scene";
            return;
        }
        string? next = binding.Length == 0 ? null : binding;
        if (next == node.script) { return; }
        if (next != null && editable_scripts?.try_get(next, out _) != true) {
            notice = "That script is no longer in the catalogue";
            return;
        }
        set_script_settings(node, "Set Script", next,
                            new SortedDictionary<string, string>(StringComparer.Ordinal));
        notice = next == null ? "Removed the script" : "Script: " + scene_script_label(next);
    }
}
