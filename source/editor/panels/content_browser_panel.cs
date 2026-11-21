using System;
using System.Collections.Generic;
using System.IO;

namespace deren.editor {
    /// <summary>
    /// Renders the asset library of the running project into the fixed tile slots (#asset_0..#asset_23)
    /// of ui/editor.rml. The managed RmlUi bridge cannot create elements, so the grid is authored once
    /// in the markup and every refresh only rewrites the visible slots.
    /// </summary>
    internal sealed class content_browser_panel {
        /// <summary>Number of tile slots authored in ui/editor.rml.</summary>
        internal const int tile_capacity = 24;

        private readonly rml_document ui;
        private readonly asset_thumbnail_cache thumbnails;
        private readonly string?[] displayed_thumbnails = new string?[tile_capacity];
        private readonly List<string> visible = new(tile_capacity);
        private string selected = string.Empty;

        internal content_browser_panel(rml_document ui, asset_thumbnail_cache thumbnails) {
            this.ui = ui;
            this.thumbnails = thumbnails;
        }

        /// <summary>Asset id of the last picked tile, or an empty string when nothing is picked.</summary>
        internal string selected_asset => selected;

        /// <summary>Maps a tile slot back to the asset id it shows; false for hidden slots.</summary>
        internal bool try_tile_asset(int tile, out string id) {
            if (tile < 0 || tile >= visible.Count) {
                id = string.Empty;
                return false;
            }
            id = visible[tile];
            return true;
        }

        /// <summary>Picks the asset shown by a tile slot; false for hidden slots.</summary>
        internal bool try_select_tile(int tile) {
            if (!try_tile_asset(tile, out var id)) {
                return false;
            }
            selected = id;
            return true;
        }

        /// <summary>Drops the picked tile, for example when the document switches to another scene.</summary>
        internal void clear_selection() {
            selected = string.Empty;
        }

        private void update_thumbnail(int tile, string asset) {
            var path = thumbnails.path_for(asset);
            string? image = File.Exists(path) ? path : null;
            if (displayed_thumbnails[tile] == image) {
                return;
            }
            var slot = $"asset_{tile}";
            var image_id = $"{slot}_image";
            bool shown = image != null && ui.set_value(image_id, $"thumbnails/{Path.GetFileName(image)}");
            ui.try_set_element_style(image_id, "display", shown ? "block" : "none");
            if (!shown) {
                ui.set_value(image_id, string.Empty);
            }
            ui.set_visible($"{slot}_placeholder", !shown);
            displayed_thumbnails[tile] = shown ? image : null;
        }

        /// <summary>Rewrites every slot from the asset library and the search filter.</summary>
        internal void refresh(IReadOnlyList<string> assets, string filter) {
            visible.Clear();
            for (var index = 0; index < assets.Count && visible.Count < tile_capacity; index++) {
                if (filter.Length == 0 || assets[index].Contains(filter, StringComparison.OrdinalIgnoreCase)) {
                    visible.Add(assets[index]);
                }
            }
            for (var tile = 0; tile < tile_capacity; tile++) {
                var slot = $"asset_{tile}";
                if (tile >= visible.Count) {
                    ui.set_visible(slot, false);
                    ui.try_set_element_style(slot, "display", "none");
                    continue;
                }
                var id = visible[tile];
                ui.set_visible(slot, true);
                ui.try_set_element_style(slot, "display", "block");
                ui.try_set_element_style(slot, "background-color", id == selected ? "#224c78" : "#2d2d2d");
                ui.set_text($"{slot}_name", id);
                update_thumbnail(tile, id);
                ui.try_set_element_style(slot, "border-color", id == selected ? "#0070e0" : "#3c3c3c");
            }
            ui.set_text("browser_count", $"{visible.Count} / {assets.Count} asset(s)");
            ui.set_visible("browser_empty", assets.Count == 0);
        }
    }
}
