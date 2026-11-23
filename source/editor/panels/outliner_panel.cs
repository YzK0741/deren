using System;
using System.Collections.Generic;
using System.Text;

namespace deren.editor {
    internal sealed class outliner_panel {
        private readonly rml_document ui;
        private readonly HashSet<Guid> collapsed = [];
        private List<row_state> rows = [];
        private List<row_state> next = [];
        private scene_document? shown_document;
        private Guid drop_target;
        private bool root_target;

        private readonly record struct row_state(Guid uid, int depth, string name, bool visible, bool selected,
                                                 bool primary, bool expandable, bool closed);

        internal outliner_panel(rml_document ui) {
            this.ui = ui;
        }

        internal int visible_rows => rows.Count;
        internal static string row_id(Guid uid) => $"row_{uid:N}";

        internal static bool try_row(string id, out Guid uid, out string part) {
            uid = Guid.Empty;
            part = string.Empty;
            if (!id.StartsWith("row_", StringComparison.Ordinal) || id.Length < 36 ||
                !Guid.TryParseExact(id.AsSpan(4, 32), "N", out uid)) {
                return false;
            }
            part = id[36..];
            return part is "" or "_name" or "_fold" or "_vis";
        }

        internal void toggle(Guid uid) {
            if (!collapsed.Remove(uid)) {
                collapsed.Add(uid);
            }
        }

        internal void expand(Guid uid) => collapsed.Remove(uid);

        internal IEnumerable<Guid> range(Guid first, Guid last) {
            int start = rows.FindIndex(row => row.uid == first);
            int end = rows.FindIndex(row => row.uid == last);
            if (start < 0 || end < 0) {
                yield return last;
                yield break;
            }
            for (int index = Math.Min(start, end); index <= Math.Max(start, end); ++index) {
                yield return rows[index].uid;
            }
        }

        internal void highlight_drop(Guid uid, bool root) {
            if (drop_target != uid) {
                if (drop_target != Guid.Empty) {
                    ui.try_set_element_style(row_id(drop_target), "border-bottom-color", "#252525");
                }
                drop_target = uid;
                if (uid != Guid.Empty) {
                    ui.try_set_element_style(row_id(uid), "border-bottom-color", "#56aeff");
                }
            }
            if (root_target != root) {
                root_target = root;
                ui.try_set_element_style("outliner_root_drop", "background-color", root ? "#284965" : "#282828");
            }
        }

        internal void refresh(scene_document document, node_selection selection, string filter) {
            if (!ReferenceEquals(shown_document, document)) {
                shown_document = document;
                collapsed.Clear();
            }
            next.Clear();
            collect(document.roots, selection, 0, filter);
            if (rows.Count != next.Count || different_rows()) {
                rebuild();
            } else {
                for (int index = 0; index < next.Count; ++index) {
                    if (rows[index] != next[index]) {
                        paint(next[index]);
                    }
                }
            }
            (rows, next) = (next, rows);
        }

        private bool different_rows() {
            for (int index = 0; index < rows.Count; ++index) {
                if (rows[index].uid != next[index].uid) {
                    return true;
                }
            }
            return false;
        }

        private void rebuild() {
            var markup = new StringBuilder();
            foreach (var row in next) {
                string id = row_id(row.uid);
                markup.Append($"""<div id="{id}" class="row"><div id="{id}_fold" class="row_fold"></div><div id="{id}_name" class="row_name"></div><div id="{id}_vis" class="row_vis"></div></div>""");
            }
            if (ui.try_set_markup("outliner_rows", markup.ToString())) {
                foreach (var row in next) {
                    paint(row);
                }
            }
        }

        private void paint(row_state row) {
            string id = row_id(row.uid);
            string indent = ((long)row.depth * 14 + 11).ToString(System.Globalization.CultureInfo.InvariantCulture);
            ui.try_set_element_style(id, "padding-left", indent + "dp");
            ui.try_set_element_style(id, "background-color", row.primary ? "#224c78" : row.selected ? "#263d55" : "transparent");
            ui.try_set_element_style(id, "border-bottom-color", row.uid == drop_target ? "#56aeff" : "#252525");
            ui.set_text(id + "_fold", row.expandable ? row.closed ? "+" : "-" : "");
            ui.set_text(id + "_name", row.name);
            ui.set_text(id + "_vis", row.visible ? "o" : "-");
            ui.try_set_element_style(id + "_vis", "color", row.visible ? "#b0b0b0" : "#616161");
        }

        private void collect(IReadOnlyList<document_node> nodes, node_selection selection, int depth, string filter) {
            foreach (var node in nodes) {
                if (filter.Length != 0 && !contains(node, filter)) {
                    continue;
                }
                bool closed = filter.Length == 0 && collapsed.Contains(node.uid);
                bool expandable = node.subscene == null && node.children.Count != 0;
                next.Add(new row_state(node.uid, depth, node.name, node.visible, selection.contains(node.uid),
                                       selection.primary == node.uid, expandable, closed));
                if (!closed && expandable) {
                    collect(node.children, selection, depth + 1, filter);
                }
            }
        }

        private static bool contains(document_node node, string filter) {
            if (node.name.Contains(filter, StringComparison.OrdinalIgnoreCase)) {
                return true;
            }
            if (node.subscene != null) { return false; }
            foreach (var child in node.children) {
                if (contains(child, filter)) {
                    return true;
                }
            }
            return false;
        }
    }
}
