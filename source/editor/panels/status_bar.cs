using System;
using System.Collections.Generic;

namespace deren.editor {
    /// <summary>Drives the single #status line of ui/editor.rml: path, dirty flag, node count, selection.</summary>
    internal sealed class status_bar {
        private readonly rml_document ui;

        internal status_bar(rml_document ui) {
            this.ui = ui;
        }

        /// <summary>
        /// Rewrites #status as "<path or untitled> *dirty* | nodes: N | selected: <name or ->".
        /// A non-empty notice (the active tool, a picked asset, the About entry of the Help menu) is
        /// appended after the selection so the menu and viewport toolbar have a visible result.
        /// </summary>
        internal void refresh(scene_document document, node_selection selection, string notice = "") {
            var path = string.IsNullOrEmpty(document.path) ? "untitled" : document.path!;
            var dirty = document.dirty ? " *dirty*" : string.Empty;
            var selected = selection.primary == Guid.Empty
                ? "-"
                : document.find(selection.primary)?.name ?? "-";
            var suffix = notice.Length == 0 ? string.Empty : $" | {notice}";
            ui.set_text("status", $"{path}{dirty} | nodes: {count(document.roots)} | selected: {selected}{suffix}");
        }

        private static int count(IReadOnlyList<document_node> nodes) {
            var total = nodes.Count;
            for (var index = 0; index < nodes.Count; index++) {
                if (nodes[index].children.Count != 0) {
                    total += count(nodes[index].children);
                }
            }
            return total;
        }
    }
}
