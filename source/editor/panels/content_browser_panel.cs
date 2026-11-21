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
        // Last write time of the file behind displayed_thumbnails. Comparing the file identity alone is
        // not enough: a rebuilt thumbnail can land on the same path, and then the slot would keep the
        // old texture forever.
        private readonly long[] displayed_stamps = new long[tile_capacity];
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
            // An empty id is never a real asset: reporting one as a tile would make the click handler
            // act on a key that cannot be loaded.
            return id.Length != 0;
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
            var slot = $"asset_{tile}";
            var image_id = $"{slot}_image";
            if (asset.Length == 0) {
                clear_thumbnail(tile, image_id, slot);
                return;
            }
            var path = thumbnails.path_for(asset);
            // A missing file clears the slot; an existing one also carries its stamp so a rebuild is
            // noticed even when the cache hands back the same path.
            var stamp = File.Exists(path) ? File.GetLastWriteTimeUtc(path).Ticks : 0;
            string? image = stamp == 0 ? null : path;
            if (displayed_thumbnails[tile] == image && displayed_stamps[tile] == stamp) {
                return;
            }
            bool shown = image != null && ui.set_value(image_id, $"thumbnails/{Path.GetFileName(image)}");
            ui.try_set_element_style(image_id, "display", shown ? "block" : "none");
            if (!shown) {
                ui.set_value(image_id, string.Empty);
            }
            ui.set_visible($"{slot}_placeholder", !shown);
            displayed_thumbnails[tile] = shown ? image : null;
            displayed_stamps[tile] = shown ? stamp : 0;
        }

        /// <summary>Drops a slot's image and its remembered state; a later refresh can refill it.</summary>
        private void clear_thumbnail(int tile, string image_id, string slot) {
            if (displayed_thumbnails[tile] == null && displayed_stamps[tile] == 0) {
                return;
            }
            ui.set_value(image_id, string.Empty);
            ui.try_set_element_style(image_id, "display", "none");
            ui.set_visible($"{slot}_placeholder", true);
            displayed_thumbnails[tile] = null;
            displayed_stamps[tile] = 0;
        }

        /// <summary>Rewrites every slot from the asset library and the search filter.</summary>
        internal void refresh(IReadOnlyList<string> assets, string filter) {
            visible.Clear();
            for (var index = 0; index < assets.Count && visible.Count < tile_capacity; index++) {
                // An empty id cannot be rendered or selected, and keeping it in the list would put an
                // empty key behind a tile that looks like a real asset.
                if (assets[index].Length == 0) {
                    continue;
                }
                if (filter.Length == 0 || assets[index].Contains(filter, StringComparison.OrdinalIgnoreCase)) {
                    visible.Add(assets[index]);
                }
            }
            for (var tile = 0; tile < tile_capacity; tile++) {
                var slot = $"asset_{tile}";
                if (tile >= visible.Count) {
                    ui.set_visible(slot, false);
                    ui.try_set_element_style(slot, "display", "none");
                    // Forget the hidden slot: its key is filtered out, so its stamp must not be trusted
                    // when a different key with a stale file lands on the same tile later.
                    clear_thumbnail(tile, $"{slot}_image", slot);
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
            // The grid is empty whenever nothing is visible, including when the filter matches nothing.
            ui.set_visible("browser_empty", visible.Count == 0);
        }
    }
}
