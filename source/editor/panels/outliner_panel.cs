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

        /// <summary>Rewrites every slot from the document and the current selection.</summary>
        internal void refresh(scene_document document, selection selection) {
            rows.Clear();
            depths.Clear();
            collect(document.roots, 0);
            for (var row = 0; row < row_capacity; row++) {
                var slot = $"row_{row}";
                if (row < rows.Count) {
                    var node = document.find(rows[row]);
                    var indent = new string(' ', depths[row] * 2);
                    var marker = selection.primary == rows[row] ? "> " : string.Empty;
                    ui.set_visible(slot, true);
                    ui.set_text($"{slot}_name", $"{indent}{marker}{node?.name ?? "<missing>"}");
                } else {
                    ui.set_visible(slot, false);
                }
            }
        }

        private void collect(IReadOnlyList<document_node> nodes, int depth) {
            for (var index = 0; index < nodes.Count && rows.Count < row_capacity; index++) {
                var node = nodes[index];
                rows.Add(node.uid);
                depths.Add(depth);
                if (node.children.Count != 0) {
                    collect(node.children, depth + 1);
                }
            }
        }
    }
}
