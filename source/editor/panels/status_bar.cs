using System;
using System.Collections.Generic;

namespace deren.editor {
    /// <summary>Drives the single #status line of ui/editor.rml: path, dirty flag, node count, selection.</summary>
    internal sealed class status_bar {
        private readonly rml_document ui;

        internal status_bar(rml_document ui) {
            this.ui = ui;
        }

        /// <summary>Rewrites #status as "<path or untitled> *dirty* | nodes: N | selected: <name or ->".</summary>
        internal void refresh(scene_document document, selection selection) {
            var path = string.IsNullOrEmpty(document.path) ? "untitled" : document.path!;
            var dirty = document.dirty ? " *dirty*" : string.Empty;
            var selected = selection.primary == Guid.Empty
                ? "-"
                : document.find(selection.primary)?.name ?? "-";
            ui.set_text("status", $"{path}{dirty} | nodes: {count(document.roots)} | selected: {selected}");
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
