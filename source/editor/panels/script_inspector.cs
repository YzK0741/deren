using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Net;
using System.Text;

namespace deren.editor {
    internal sealed class script_inspector(rml_document ui, script_catalog catalog) {
        private readonly Dictionary<string, script_parameter> fields = new(StringComparer.Ordinal);
        private readonly Dictionary<string, string> drafts = new(StringComparer.Ordinal);
        private scene_document? shown_document;
        private int revision = -1;
        private Guid shown_node;
        private bool invalidated = true;

        internal void invalidate() => invalidated = true;

        internal bool try_field(string id, out script_parameter? field) => fields.TryGetValue(id, out field);

        internal bool draft(string id, string value) {
            if (!drafts.ContainsKey(id)) { return false; }
            drafts[id] = value;
            return true;
        }

        internal bool try_draft(string id, out string value) => drafts.TryGetValue(id, out value!);

        internal void refresh(scene_document document, Guid uid, string[] assets) {
            if (!invalidated && shown_document == document && revision == document.revision && shown_node == uid) {
                return;
            }
            invalidated = false;
            shown_document = document;
            revision = document.revision;
            shown_node = uid;
            fields.Clear();
            drafts.Clear();
            if (document.find(uid) is not { } node) {
                ui.try_set_markup("script_controls", "");
                return;
            }

            var markup = new StringBuilder();
            markup.Append("""<div class="detail_header">Script</div><div class="detail_row"><select id="script_picker" class="script_select">""");
            option(markup, "", "No script");
            if (node.script != null && !catalog.try_get(node.script, out _)) {
                option(markup, node.script, $"Missing: {node.script}");
            }
            foreach (var definition in catalog.entries) {
                option(markup, definition.binding, definition.type.FullName ?? definition.binding);
            }
            markup.Append("""</select><button id="script_refresh">Refresh</button></div>""");

            if (node.script != null && catalog.try_get(node.script, out var script) && script != null) {
                int index = 0;
                foreach (var field in script.parameters) {
                    string id = $"script_param_{index++}";
                    fields.Add(id, field);
                    markup.Append($"""<div class="detail_row"><div class="detail_label">{WebUtility.HtmlEncode(field.label)}</div>""");
                    if (field.kind is parameter_kind.boolean or parameter_kind.node or parameter_kind.asset) {
                        markup.Append($"""<select id="{id}" class="script_select">""");
                        option(markup, "", "Default");
                        string? current = node.script_parameters.GetValueOrDefault(field.name);
                        if (field.kind == parameter_kind.boolean) {
                            option(markup, "true", "True");
                            option(markup, "false", "False");
                        } else if (field.kind == parameter_kind.asset) {
                            if (current != null && !assets.Contains(current, StringComparer.Ordinal)) {
                                option(markup, current, "Missing: " + current);
                            }
                            foreach (string asset in assets) { option(markup, asset, asset); }
                        } else {
                            if (current != null && (!Guid.TryParse(current, out var target) || document.find(target) == null)) {
                                option(markup, current, "Missing node");
                            }
                            node_options(markup, document.roots, "");
                        }
                        markup.Append("</select>");
                    } else {
                        string placeholder = field.kind == parameter_kind.vector ? "Default or X, Y, Z" : "Default";
                        drafts[id] = node.script_parameters.GetValueOrDefault(field.name, "");
                        markup.Append($"""<input id="{id}" type="text" placeholder="{placeholder}"/><button id="apply_{id}" class="script_apply">Apply</button>""");
                    }
                    markup.Append("</div>");
                }
            }
            if (!ui.try_set_markup("script_controls", markup.ToString())) {
                fields.Clear();
                return;
            }
            ui.set_value("script_picker", node.script ?? "");
            foreach (var (id, field) in fields) {
                ui.set_value(id, node.script_parameters.GetValueOrDefault(field.name, ""));
            }
        }

        private static void option(StringBuilder markup, string value, string label) =>
            markup.Append($"""<option value="{WebUtility.HtmlEncode(value)}">{WebUtility.HtmlEncode(label)}</option>""");

        private static void node_options(StringBuilder markup, IReadOnlyList<document_node> nodes, string prefix) {
            foreach (var node in nodes) {
                option(markup, node.uid.ToString("D", CultureInfo.InvariantCulture), prefix + node.name);
                node_options(markup, node.children, prefix + node.name + " / ");
            }
        }
    }
}
