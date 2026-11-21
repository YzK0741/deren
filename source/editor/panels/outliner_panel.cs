using System;
using System.Collections.Generic;

namespace deren.editor {
    /// <summary>
    /// Renders the flattened document tree into the fixed row slots (#row_0.. #row_63) of ui/editor.rml.
    /// The managed RmlUi bridge only offers set_text/set_value/try_set_element_style by element id, so the
    /// row slots are authored once in the markup and reused here instead of being created per node; a slot
    /// is shown or hidden through `display`, which takes its box out of the column, not `visibility`.
    /// </summary>
    internal sealed class outliner_panel {
        /// <summary>Number of row slots authored in ui/editor.rml.</summary>
        internal const int row_capacity = 64;

        private readonly rml_document ui;
        private readonly List<Guid> rows = new(row_capacity);
        private readonly List<int> depths = new(row_capacity);
        // WHAT IS CURRENTLY ON SCREEN, so a frame that changes nothing writes nothing. Every ui call above
        // the bridge is a cross-ABI string marshal, and this panel used to rewrite all 64 slots (roughly six
        // calls each) every frame just to say the same thing: a busy scene paid ~380 host calls per frame
        // for a tree that had not moved. The cache also covers the row set, because an add/delete/reorder
        // moves slot contents and only a structural change may touch `display`.
        private readonly List<string> labels = new(row_capacity);
        private readonly List<bool> visibilities = new(row_capacity);
        private string cached_filter = string.Empty;
        private Guid cached_selection = Guid.Empty;

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
        /// Rewrites the slots that changed from the document, the current selection and the outliner search
        /// filter. A node is shown when its own name or any descendant name contains the filter text, so
        /// typing a child name still reveals the parent chain that leads to it.
        /// </summary>
        internal void refresh(scene_document document, selection selection, string filter) {
            // The flattened row list is rebuilt every frame (it is an in-memory walk of at most 64 nodes);
            // comparing it with the cached one is the ONLY reliable structural signal. The document's
            // revision counter cannot serve here: it only moves for edits that mark the file dirty, so a
            // pure reselection or a filter keystroke would never be noticed.
            var next_rows = new List<Guid>(row_capacity);
            var next_depths = new List<int>(row_capacity);
            collect(document.roots, 0, filter, next_rows, next_depths);

            if (!same_structure(next_rows, next_depths, filter)) {
                rebuild(document, selection, next_rows, next_depths, filter);
                return;
            }

            for (var row = 0; row < rows.Count; row++) {
                var slot = $"row_{row}";
                var node = document.find(rows[row]);
                var label = $"{new string(' ', depths[row] * 2)}{node?.name ?? "<missing>"}";
                if (label != labels[row]) {
                    ui.set_text($"{slot}_name", label);
                    labels[row] = label;
                }
                var visible = node?.visible ?? false;
                if (visible != visibilities[row]) {
                    ui.set_text($"{slot}_vis", visible ? "o" : "-");
                    ui.try_set_element_style($"{slot}_vis", "color", visible ? "#b0b0b0" : "#616161");
                    visibilities[row] = visible;
                }
            }
            repaint_selection(selection.primary);
        }

        /// <summary>
        /// Writes every slot: the row set, the depths or the filter moved, so the whole column is new.
        /// Slots past the last node are hidden here too, which is why a structural change is the only
        /// path that touches `display`.
        /// </summary>
        private void rebuild(scene_document document, selection selection, List<Guid> next_rows, List<int> next_depths, string filter) {
            rows.Clear();
            rows.AddRange(next_rows);
            depths.Clear();
            depths.AddRange(next_depths);
            labels.Clear();
            visibilities.Clear();
            cached_filter = filter;
            for (var row = 0; row < row_capacity; row++) {
                var slot = $"row_{row}";
                if (row < rows.Count) {
                    var node = document.find(rows[row]);
                    var label = $"{new string(' ', depths[row] * 2)}{node?.name ?? "<missing>"}";
                    var visible = node?.visible ?? false;
                    ui.try_set_element_style(slot, "display", "flex");
                    // The background is written here as well, not only by repaint_selection: a slot that
                    // showed the selection before the rebuild still carries its #224c78 from an earlier
                    // frame, and the element style is sticky, so a fresh row set must state its own colour.
                    ui.try_set_element_style(slot, "background-color", rows[row] == selection.primary ? "#224c78" : "transparent");
                    ui.set_text($"{slot}_name", label);
                    ui.set_text($"{slot}_vis", visible ? "o" : "-");
                    ui.try_set_element_style($"{slot}_vis", "color", visible ? "#b0b0b0" : "#616161");
                    labels.Add(label);
                    visibilities.Add(visible);
                } else {
                    ui.try_set_element_style(slot, "display", "none");
                }
            }
            cached_selection = selection.primary;
        }

        /// <summary>
        /// Repaints the background of the rows the selection moved between. Only two slots can change, so
        /// this costs two style writes instead of sixty-four.
        /// </summary>
        private void repaint_selection(Guid primary) {
            if (primary == cached_selection) {
                return;
            }
            var previous = rows.IndexOf(cached_selection);
            if (previous >= 0) {
                ui.try_set_element_style($"row_{previous}", "background-color", "transparent");
            }
            var current = rows.IndexOf(primary);
            if (current >= 0) {
                ui.try_set_element_style($"row_{current}", "background-color", "#224c78");
            }
            cached_selection = primary;
        }

        private bool same_structure(List<Guid> next_rows, List<int> next_depths, string filter) {
            if (filter != cached_filter || next_rows.Count != rows.Count) {
                return false;
            }
            for (var row = 0; row < rows.Count; row++) {
                if (next_rows[row] != rows[row] || next_depths[row] != depths[row]) {
                    return false;
                }
            }
            return true;
        }

        private void collect(IReadOnlyList<document_node> nodes, int depth, string filter, List<Guid> into, List<int> into_depths) {
            for (var index = 0; index < nodes.Count && into.Count < row_capacity; index++) {
                var node = nodes[index];
                if (!matches(node, filter)) {
                    continue;
                }
                into.Add(node.uid);
                into_depths.Add(depth);
                if (node.children.Count != 0) {
                    collect(node.children, depth + 1, filter, into, into_depths);
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
