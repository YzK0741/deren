using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace deren.editor {
    /// <summary>One entry of an environment dropdown: the text a document stores and the label it shows.</summary>
    internal readonly record struct environment_entry(string text, string label);

    /// <summary>
    /// Drives the Environment rows of ui/editor.rml: the scene-wide map and the custom lanes the scene
    /// document stores, plus the per-object row the details panel owns.
    /// </summary>
    /// <remarks>
    /// The rows are dropdowns whose options are the .hdr files of the asset library, plus a procedural-sky
    /// entry and a "choose a file" entry that asks the platform dialog. The managed bridge can only address
    /// elements that already exist, so the panel writes the row markup into <c>environment_rows</c> whenever
    /// the set of USED lanes changes and each dropdown's options through
    /// <see cref="rml_document.try_set_markup"/> whenever the catalogue or the stored value moved; a frame
    /// that changes nothing writes nothing, which keeps a popup the user has open from being rebuilt
    /// underneath the pointer.
    /// Only lanes that carry a map get a row: the engine has sixteen slots and drawing fifteen empty rows
    /// would bury the panel. A lane appears the moment an object picks a distinct map (the host allocates
    /// it), and "procedural sky" in a lane row gives it back.
    /// Applying a selection is the host's job: this class only maps a change event back to a lane and to
    /// the value the user picked.
    /// </remarks>
    internal sealed class environment_panel {
        internal const int lane_count = global_environment.slot_count;
        /// <summary>Option value that means "no map": the renderer's procedural sky.</summary>
        internal const string sky_value = "@sky";
        /// <summary>Option value that means "ask the platform for a file".</summary>
        internal const string choose_value = "@choose";
        /// <summary>Option value of the per-object row that means "sample the scene-wide map".</summary>
        internal const string scene_value = "@scene";
        /// <summary>
        /// Option value of the per-object row for a lane the document stores no map for. It only ever shows
        /// up when a hand-edited document points an object at an empty lane; picking it is a no-op because
        /// the row is repainted from the document.
        /// </summary>
        internal const string empty_lane_value = "@lane";

        private const string rows_id = "environment_rows";
        private const string global_id = "environment_global";
        private const string lane_prefix = "environment_lane_";
        private const string status_id = "environment_status";
        /// <summary>The id of the per-object dropdown the details panel paints.</summary>
        internal const string node_id = "detail_environment";

        private readonly rml_document ui;
        // The markup key last written per lane: null means "not written yet", which forces the first write.
        private readonly string?[] shown = new string?[lane_count];
        // The set of lanes with a row, and whether the container has been written at all.
        private string rows_key = string.Empty;
        private bool rows_written;

        internal environment_panel(rml_document ui) {
            this.ui = ui;
        }

        /// <summary>
        /// Rewrites the lane rows from the document and the scene row from the project map. A lane the
        /// document leaves null keeps the project value, so it is shown as the procedural sky rather than
        /// copied.
        /// </summary>
        internal void refresh(scene_document document, string global, IReadOnlyList<environment_entry> catalog) {
            var lanes = used_lanes(document);
            var key = layout_key(lanes);
            if (!rows_written || !string.Equals(rows_key, key, StringComparison.Ordinal)) {
                if (ui.try_set_markup(rows_id, rows_markup(lanes))) {
                    rows_key = key;
                    rows_written = true;
                    Array.Fill(shown, null);
                }
            }
            write(0, global, catalog);
            foreach (var lane in lanes) {
                write(lane, document.environment_slots[lane - 1] ?? string.Empty, catalog);
            }
        }

        /// <summary>Shows the outcome of the last apply on the row below the dropdowns.</summary>
        internal void set_status(string text) {
            ui.set_text(status_id, text);
        }

        /// <summary>
        /// Forgets what one lane shows, so the next <see cref="refresh"/> rewrites it. Used when a pick was
        /// cancelled: the dropdown already moved to the "choose a file" entry and has to go back.
        /// </summary>
        internal void invalidate(int lane) {
            if (lane >= 0 && lane < lane_count) {
                shown[lane] = null;
            }
        }

        /// <summary>Forgets the per-object row, so the next refresh rewrites its options and value.</summary>
        internal void invalidate_node() {
            node_key = null;
        }

        private string? node_key;

        /// <summary>
        /// Paints the per-object dropdown of the details panel: the scene-wide map, every map the catalogue
        /// lists, the row's current map when the catalogue does not carry it, and the picker entry.
        /// </summary>
        internal void refresh_node(scene_document document, document_node? node, IReadOnlyList<environment_entry> catalog) {
            if (node == null) {
                node_key = null;
                return;
            }
            var current = node.environment == 0 ? string.Empty : document.environment_slots[node.environment - 1] ?? string.Empty;
            // The lane index belongs in the key: two nodes can share a map text while one of them samples
            // the scene map and the other a lane, and the row has to move when the selection does.
            var key = markup_key(catalog, current) + '\u0004' + node.environment.ToString(CultureInfo.InvariantCulture);
            if (string.Equals(node_key, key, StringComparison.Ordinal)) {
                return;
            }
            node_key = key;
            var empty_lane = node.environment != 0 && current.Length == 0;
            if (!ui.try_set_markup(node_id, node_markup(current, catalog, empty_lane))) {
                node_key = null;
                return;
            }
            ui.set_value(node_id, node.environment == 0 ? scene_value : empty_lane ? empty_lane_value : current);
        }

        /// <summary>
        /// Maps a change-event id to the lane it edits; false for every other element. Lane 0 is the
        /// scene-wide map, which the host keeps in the project settings rather than the document.
        /// </summary>
        internal static bool try_change(string? id, string text, out int lane, out string value) {
            lane = 0;
            value = text;
            if (id == null) {
                return false;
            }
            if (id == global_id) {
                return true;
            }
            if (!id.StartsWith(lane_prefix, StringComparison.Ordinal)) {
                return false;
            }
            if (!int.TryParse(id.AsSpan(lane_prefix.Length), NumberStyles.None, CultureInfo.InvariantCulture, out lane)) {
                lane = 0;
                return false;
            }
            return lane >= 1 && lane < lane_count;
        }

        private void write(int lane, string current, IReadOnlyList<environment_entry> catalog) {
            var key = markup_key(catalog, current);
            if (string.Equals(shown[lane], key, StringComparison.Ordinal)) {
                return;
            }
            shown[lane] = key;
            var id = element_id(lane);
            if (!ui.try_set_markup(id, markup(current, catalog))) {
                shown[lane] = null; // the row is not in the document yet; try again on the next refresh
                return;
            }
            ui.set_value(id, current.Length == 0 ? sky_value : current);
        }

        private static List<int> used_lanes(scene_document document) {
            var lanes = new List<int>();
            for (var lane = 1; lane < lane_count; ++lane) {
                if (!string.IsNullOrEmpty(document.environment_slots[lane - 1])) {
                    lanes.Add(lane);
                }
            }
            return lanes;
        }

        private static string layout_key(IReadOnlyList<int> lanes) {
            var key = new StringBuilder();
            foreach (var lane in lanes) {
                key.Append(lane.ToString(CultureInfo.InvariantCulture)).Append(',');
            }
            return key.ToString();
        }

        private static string rows_markup(IReadOnlyList<int> lanes) {
            var markup = new StringBuilder();
            markup.Append("<div class=\"detail_row\"><div class=\"detail_label\">Scene map</div>")
                  .Append("<select id=\"").Append(global_id).Append("\" class=\"env_select\"></select></div>");
            if (lanes.Count == 0) {
                markup.Append("<div class=\"detail_row\"><div class=\"detail_label\">Lanes</div>")
                      .Append("<div class=\"detail_text\">Per-object maps appear here</div></div>");
            }
            foreach (var lane in lanes) {
                markup.Append("<div class=\"detail_row\"><div class=\"detail_label\">Lane ")
                      .Append(lane.ToString(CultureInfo.InvariantCulture)).Append("</div>")
                      .Append("<select id=\"").Append(element_id(lane)).Append("\" class=\"env_select\"></select></div>");
            }
            return markup.ToString();
        }

        private static string element_id(int lane) =>
            lane == 0 ? global_id : lane_prefix + lane.ToString(CultureInfo.InvariantCulture);

        // The catalogue text plus the stored value: either moving means the options have to be rebuilt.
        private static string markup_key(IReadOnlyList<environment_entry> catalog, string current) {
            var key = new StringBuilder(current);
            key.Append('\u0001');
            foreach (var entry in catalog) {
                key.Append(entry.text).Append('\u0002').Append(entry.label).Append('\u0003');
            }
            return key.ToString();
        }

        private static string markup(string current, IReadOnlyList<environment_entry> catalog) {
            var markup = new StringBuilder();
            markup.Append("<option value=\"").Append(sky_value).Append("\">(procedural sky)</option>");
            var listed = current.Length == 0;
            foreach (var entry in catalog) {
                listed |= string.Equals(entry.text, current, StringComparison.Ordinal);
                markup.Append("<option value=\"").Append(escape(entry.text)).Append("\">")
                      .Append(escape(entry.label)).Append("</option>");
            }
            if (!listed) {
                // A map the catalogue does not carry (an older scene, or a file outside the library) still
                // has to be visible, otherwise selecting the row would silently drop it.
                markup.Append("<option value=\"").Append(escape(current)).Append("\">")
                      .Append(escape(Path.GetFileName(current))).Append("</option>");
            }
            markup.Append("<option value=\"").Append(choose_value).Append("\">Choose a file...</option>");
            return markup.ToString();
        }

        private static string node_markup(string current, IReadOnlyList<environment_entry> catalog, bool empty_lane) {
            var markup = new StringBuilder();
            markup.Append("<option value=\"").Append(scene_value).Append("\">(scene map)</option>");
            if (empty_lane) {
                // The document points this object at a lane that carries no map, so the row shows that rather
                // than claiming the object samples the scene map.
                markup.Append("<option value=\"").Append(empty_lane_value).Append("\">Lane without a map</option>");
            }
            var listed = false;
            foreach (var entry in catalog) {
                listed |= string.Equals(entry.text, current, StringComparison.Ordinal);
                markup.Append("<option value=\"").Append(escape(entry.text)).Append("\">")
                      .Append(escape(entry.label)).Append("</option>");
            }
            if (!listed && current.Length > 0) {
                markup.Append("<option value=\"").Append(escape(current)).Append("\">")
                      .Append(escape(Path.GetFileName(current))).Append("</option>");
            }
            markup.Append("<option value=\"").Append(choose_value).Append("\">Choose a file...</option>");
            return markup.ToString();
        }

        internal static string escape(string text) {
            if (text.IndexOfAny(new[] { '&', '<', '>', '"' }) < 0) {
                return text;
            }
            var escaped = new StringBuilder(text.Length + 8);
            foreach (var character in text) {
                switch (character) {
                    case '&': escaped.Append("&amp;"); break;
                    case '<': escaped.Append("&lt;"); break;
                    case '>': escaped.Append("&gt;"); break;
                    case '"': escaped.Append("&quot;"); break;
                    default: escaped.Append(character); break;
                }
            }
            return escaped.ToString();
        }
    }
}
