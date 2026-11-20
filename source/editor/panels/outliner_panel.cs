using System;
using System.Collections.Generic;

namespace deren.editor {
    /// <summary>
    /// Renders the flattened document tree into the fixed row slots (#row_0.. #row_63) of ui/editor.rml.
    /// The managed RmlUi bridge only offers set_text/set_value/set_visible by element id, so the row
    /// slots are authored once in the markup and reused here instead of being created per node.
    /// </summary>
    internal sealed class outliner_panel {
        /// <summary>Number of row slots authored in ui/editor.rml.</summary>
        internal const int row_capacity = 64;

        private readonly rml_document ui;
        private readonly List<Guid> rows = new(row_capacity);
        private readonly List<int> depths = new(row_capacity);

        internal outliner_panel(rml_document ui) {
            this.ui = ui;
        }

        /// <summary>Number of row slots currently showing a node.</summary>
        internal int visible_rows => rows.Count;

        /// <summary>Maps a row slot back to the document node it shows; false for hidden slots.</summary>
        internal bool try_row_uid(int row, out Guid uid) {
            if (row < 0 || row >= rows.Count) {
                uid = Guid.Empty;
                return false;
            }
            uid = rows[row];
            return true;
        }

        /// <summary>
        /// Rewrites every slot from the document, the current selection and the outliner search filter.
        /// A node is shown when its own name or any descendant name contains the filter text, so typing
        /// a child name still reveals the parent chain that leads to it.
        /// </summary>
        internal void refresh(scene_document document, selection selection, string filter) {
            rows.Clear();
            depths.Clear();
            collect(document.roots, 0, filter);
            for (var row = 0; row < row_capacity; row++) {
                var slot = $"row_{row}";
                if (row < rows.Count) {
                    var node = document.find(rows[row]);
                    var indent = new string(' ', depths[row] * 2);
                    var selected = selection.primary == rows[row];
                    var visible = node?.visible ?? false;
                    ui.set_visible(slot, true);
                    ui.try_set_element_style(slot, "display", "flex");
                    ui.try_set_element_style(slot, "background-color", selected ? "#224c78" : "transparent");
                    ui.set_text($"{slot}_name", $"{indent}{node?.name ?? "<missing>"}");
                    ui.set_text($"{slot}_vis", visible ? "o" : "-");
                    ui.try_set_element_style($"{slot}_vis", "color", visible ? "#b0b0b0" : "#616161");
                } else {
                    ui.set_visible(slot, false);
                    ui.try_set_element_style(slot, "display", "none");
                }
            }
        }

        private void collect(IReadOnlyList<document_node> nodes, int depth, string filter) {
            for (var index = 0; index < nodes.Count && rows.Count < row_capacity; index++) {
                var node = nodes[index];
                if (!matches(node, filter)) {
                    continue;
                }
                rows.Add(node.uid);
                depths.Add(depth);
                if (node.children.Count != 0) {
                    collect(node.children, depth + 1, filter);
                }
            }
        }

        private static bool matches(document_node node, string filter) {
            if (filter.Length == 0) {
                return true;
            }
            return contains(node, filter);
        }

        private static bool contains(document_node node, string filter) {
            if (node.name.Contains(filter, StringComparison.OrdinalIgnoreCase)) {
                return true;
            }
            for (var index = 0; index < node.children.Count; index++) {
                if (contains(node.children[index], filter)) {
                    return true;
                }
            }
            return false;
        }
    }
}
