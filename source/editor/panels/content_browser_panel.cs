using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;

namespace deren.editor {
    internal sealed class content_browser_panel {
        internal const int page_size = 48;

        private readonly record struct browser_tile(string key, string name, string folder, bool directory);

        private readonly rml_document ui;
        private readonly asset_thumbnail_cache thumbnails;
        private readonly string ui_directory;
        private readonly string?[] displayed_thumbnails = new string?[page_size];
        private readonly long[] displayed_stamps = new long[page_size];
        private readonly List<browser_tile> catalogue = [];
        private readonly HashSet<string> folders = new(StringComparer.Ordinal);
        private readonly List<browser_tile> matches = [];
        private browser_tile[] visible = [];
        private IReadOnlyList<string>? source;
        private string filter = string.Empty;
        private string folder = string.Empty;
        private string selected = string.Empty;
        private int page;

        internal content_browser_panel(rml_document ui, asset_thumbnail_cache thumbnails, string ui_directory) {
            this.ui = ui;
            this.thumbnails = thumbnails;
            this.ui_directory = ui_directory;
        }

        internal string selected_asset => selected;
        internal string current_folder => folder;

        internal bool try_tile_asset(int index, out string id) {
            if ((uint)index < visible.Length && !visible[index].directory) {
                id = visible[index].key;
                return true;
            }
            id = string.Empty;
            return false;
        }

        internal bool try_select_tile(int index) {
            if (!try_tile_asset(index, out var id)) { return false; }
            selected = id;
            return true;
        }

        internal bool try_open_folder(int index) {
            return (uint)index < visible.Length && visible[index].directory && navigate(visible[index].key);
        }

        internal bool go_up() => folder.Length != 0 && navigate(parent_folder(folder));
        internal bool go_to_root() => navigate(string.Empty);

        private bool navigate(string next) {
            if (next == folder) { return false; }
            folder = next;
            selected = string.Empty;
            page = 0;
            rebuild_matches();
            render();
            return true;
        }

        internal bool change_page(int step) {
            int next = Math.Clamp(page + step, 0, page_count - 1);
            if (next == page) { return false; }
            page = next;
            selected = string.Empty;
            render();
            return true;
        }

        private int page_count => Math.Max(1, (matches.Count + page_size - 1) / page_size);

        internal void clear_selection() => selected = string.Empty;

        internal void refresh(IReadOnlyList<string> assets, string search) {
            search = search.Trim();
            bool changed = !ReferenceEquals(source, assets);
            if (changed) { rebuild_catalogue(assets); }
            if (filter != search) {
                filter = search;
                page = 0;
                changed = true;
            }
            if (changed) { rebuild_matches(); }
            render();
        }

        private void rebuild_catalogue(IReadOnlyList<string> assets) {
            source = assets;
            catalogue.Clear();
            folders.Clear();
            bool connected = asset_authoring.try_open(out var authoring);
            string? root = connected ? authoring.library_root() : null;
            foreach (string id in assets) {
                if (id.Length == 0) { continue; }
                var directory = root != null ? authoring.asset_directory(id) : null;
                var grouping = string.Empty;
                if (root != null && directory != null && Path.GetDirectoryName(directory) is { } parent) {
                    grouping = Path.GetRelativePath(root, parent).Replace(Path.DirectorySeparatorChar, '/');
                    if (grouping is "." or ".." || grouping.StartsWith("../", StringComparison.Ordinal) ||
                        Path.IsPathRooted(grouping)) { grouping = string.Empty; }
                }
                string? name = connected ? authoring.display_name(id) : null;
                catalogue.Add(new browser_tile(id, string.IsNullOrWhiteSpace(name) ? id : name, grouping, false));
                for (var branch = grouping; branch.Length != 0 && folders.Add(branch); branch = parent_folder(branch)) { }
            }
            while (folder.Length != 0 && !folders.Contains(folder)) { folder = parent_folder(folder); }
        }

        private void rebuild_matches() {
            matches.Clear();
            if (filter.Length == 0) {
                foreach (var child in folders.Where(child => parent_folder(child) == folder)
                                             .Order(StringComparer.OrdinalIgnoreCase).ThenBy(child => child, StringComparer.Ordinal)) {
                    matches.Add(new browser_tile(child, Path.GetFileName(child), folder, true));
                }
                matches.AddRange(catalogue.Where(item => item.folder == folder));
            } else {
                string prefix = folder.Length == 0 ? string.Empty : folder + "/";
                matches.AddRange(catalogue.Where(item =>
                    (item.folder == folder || item.folder.StartsWith(prefix, StringComparison.Ordinal)) &&
                    (item.key.Contains(filter, StringComparison.OrdinalIgnoreCase) ||
                     item.name.Contains(filter, StringComparison.OrdinalIgnoreCase) ||
                     item.folder.Contains(filter, StringComparison.OrdinalIgnoreCase)))
                    .OrderByDescending(item => item.key.Equals(filter, StringComparison.OrdinalIgnoreCase)));
            }
            page = Math.Min(page, page_count - 1);
        }

        private void render() {
            int first = page * page_size;
            int count = Math.Min(page_size, matches.Count - first);
            var current = matches.GetRange(first, count);
            if (!visible.SequenceEqual(current)) {
                var markup = new StringBuilder("""<div id="asset_grid">""");
                for (var index = 0; index < current.Count; ++index) {
                    var item = current[index];
                    string id = slot_id(index);
                    string title = WebUtility.HtmlEncode(item.name);
                    string label = item.directory ? "Folder" : item.name != item.key ? item.key : item.folder;
                    if (label.Length == 0) { label = "Library asset"; }
                    string icon = item.directory ? "asset_folder" : "asset_cube";
                    markup.Append($"""<div id="{id}" class="browser_tile"><div class="asset_preview"><img id="{id}_image" class="asset_thumbnail"/><div id="{id}_placeholder" class="{icon}"><div class="asset_cube_inner"></div></div></div><div id="{id}_name" class="browser_tile_name">{title}</div><div class="asset_kind">{WebUtility.HtmlEncode(label)}</div></div>""");
                }
                markup.Append("</div>");
                if (!ui.try_set_markup("asset_grid_host", markup.ToString())) { return; }
                visible = [.. current];
                if (!visible.Any(item => !item.directory && item.key == selected)) { selected = string.Empty; }
                Array.Clear(displayed_thumbnails);
                Array.Clear(displayed_stamps);
            }
            for (var index = 0; index < visible.Length; ++index) {
                var item = visible[index];
                var id = slot_id(index);
                bool active = !item.directory && item.key == selected;
                ui.try_set_element_style(id, "background-color", active ? "#224c78" : "#2d2d2d");
                ui.try_set_element_style(id, "border-color", active ? "#0070e0" : "#3c3c3c");
                if (!item.directory) { update_thumbnail(index, item.key); }
            }
            ui.set_text("browser_location", folder.Length == 0 ? "Assets" : "Assets / " + folder.Replace("/", " / "));
            ui.set_text("browser_count", count == 0 ? "0 items" : $"{first + 1}–{first + count} of {matches.Count}");
            ui.set_text("browser_page", $"{page + 1} / {page_count}");
            ui.set_text("browser_empty", filter.Length == 0 ? "No assets in this folder." : "No assets match this search.");
            ui.set_visible("browser_empty", count == 0);
            ui.try_set_element_style("browser_empty", "display", count == 0 ? "block" : "none");
            ui.try_set_element_style("browser_paging", "display", page_count > 1 ? "flex" : "none");
            set_enabled("browser_prev", page > 0);
            set_enabled("browser_next", page + 1 < page_count);
            set_enabled("browser_up", folder.Length != 0);
            set_enabled("browser_root", folder.Length != 0);
        }

        private void set_enabled(string id, bool enabled) {
            ui.try_set_element_style(id, "pointer-events", enabled ? "auto" : "none");
            ui.try_set_element_style(id, "opacity", enabled ? "1" : "0.4");
        }

        private void update_thumbnail(int index, string asset) {
            var path = thumbnails.path_for(asset);
            var stamp = File.Exists(path) ? File.GetLastWriteTimeUtc(path).Ticks : 0;
            string? image = stamp == 0 ? null : path;
            if (displayed_thumbnails[index] == image && displayed_stamps[index] == stamp) { return; }
            string slot = slot_id(index);
            string image_id = $"{slot}_image";
            bool shown = image != null && ui.set_value(image_id, Path.GetRelativePath(ui_directory, image).Replace(Path.DirectorySeparatorChar, '/'));
            ui.try_set_element_style(image_id, "display", shown ? "block" : "none");
            if (!shown) { ui.set_value(image_id, string.Empty); }
            ui.set_visible($"{slot}_placeholder", !shown);
            displayed_thumbnails[index] = shown ? image : null;
            displayed_stamps[index] = shown ? stamp : 0;
        }

        internal static bool try_tile_index(string id, out int index) {
            index = -1;
            if (!id.StartsWith("asset_", StringComparison.Ordinal)) { return false; }
            var digits = id.AsSpan("asset_".Length);
            int suffix = digits.IndexOf('_');
            if (suffix >= 0) { digits = digits[..suffix]; }
            return int.TryParse(digits, NumberStyles.None, CultureInfo.InvariantCulture, out index);
        }

        private static string slot_id(int index) => $"asset_{index}";
        private static string parent_folder(string path) {
            int last_slash = path.LastIndexOf('/');
            return last_slash < 0 ? string.Empty : path[..last_slash];
        }
    }
}
