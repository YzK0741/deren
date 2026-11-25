using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using deren.scripting;

namespace deren.editor {
    /// <summary>
    /// What one area shows. This is the area's editor type, the way a Blender area keeps an
    /// <c>eSpace_Type</c>: the area owns no data, it only decides which panel fills its rect.
    /// </summary>
    internal enum area_kind {
        viewport,
        outliner,
        details,
        dope,
        dock,
    }

    /// <summary>The editor types in the order the header menu lists them, and the name each one shows.</summary>
    internal static class area_kinds {
        internal static readonly area_kind[] all = [area_kind.viewport, area_kind.outliner,
                                                    area_kind.details, area_kind.dope, area_kind.dock];

        internal static string name_of(area_kind kind) => kind switch {
            area_kind.viewport => "Viewport",
            area_kind.outliner => "Outliner",
            area_kind.details => "Details",
            area_kind.dope => "Dope Sheet",
            _ => "Content",
        };
    }

    /// <summary>
    /// One node of the area tree. A split holds two or more children and the share each one gets; a leaf
    /// holds one editor type. Blender keeps a screen as a graph of vertices and edges, which allows L shaped
    /// junctions; a tree cannot, so the trade is deliberate: side by side and stacked splits only, and an
    /// area is "swapped" by changing its editor type instead of moving its edges.
    /// </summary>
    internal sealed class area_node {
        internal bool column;
        internal List<area_node> children = [];
        internal List<float> shares = [];
        internal area_kind kind;

        internal bool is_split => children.Count != 0;

        internal static area_node leaf(area_kind kind) => new() { kind = kind };

        internal static area_node split(bool column, params (area_node node, float share)[] parts) {
            var node = new area_node { column = column };
            foreach (var (child, share) in parts) {
                node.children.Add(child);
                node.shares.Add(share);
            }
            return node;
        }

        internal void normalize() {
            float total = 0.0F;
            foreach (var share in shares) { total += MathF.Max(0.0F, share); }
            if (total <= 0.0F) {
                for (int i = 0; i < shares.Count; ++i) { shares[i] = 1.0F / MathF.Max(1, shares.Count); }
                return;
            }
            for (int i = 0; i < shares.Count; ++i) {
                shares[i] = MathF.Max(0.0F, shares[i]) / total;
            }
        }

        /// <summary>The smallest size this subtree may take, so a divider can never hide a panel.</summary>
        internal (float width, float height) minimum() {
            if (!is_split) {
                return kind switch {
                    area_kind.viewport => (area_layout.area_min_viewport_width, area_layout.area_min_viewport_height),
                    area_kind.details => (area_layout.area_min_details_width, area_layout.area_min_details_height),
                    _ => (area_layout.area_min_panel_width, area_layout.area_min_panel_height),
                };
            }
            float width = 0.0F;
            float height = 0.0F;
            float thickness = ui_controls.splitter_thickness;
            foreach (var child in children) {
                var (child_width, child_height) = child.minimum();
                if (column) {
                    width += child_width + thickness;
                    height = MathF.Max(height, child_height);
                } else {
                    width = MathF.Max(width, child_width);
                    height += child_height + thickness;
                }
            }
            if (column) { width -= thickness; } else { height -= thickness; }
            return (width, height);
        }

        internal area_node? find_leaf(area_kind wanted) {
            if (!is_split) { return kind == wanted ? this : null; }
            foreach (var child in children) {
                if (child.find_leaf(wanted) is { } found) { return found; }
            }
            return null;
        }

        internal area_node clone() {
            var copy = new area_node { column = column, kind = kind, shares = [.. shares] };
            foreach (var child in children) { copy.children.Add(child.clone()); }
            return copy;
        }
    }

    /// <summary>
    /// One workspace: a name and the area tree it shows. Blender keeps the same pairing per workspace,
    /// and the strip above the areas switches between them; the layouts are saved together so a reader
    /// can come back to either arrangement.
    /// </summary>
    internal sealed class area_workspace {
        internal area_workspace(string name, area_node root) {
            this.name = name;
            this.root = root;
        }

        internal string name;
        internal area_node root;
    }

    /// <summary>
    /// The editor's area layout: one tree per workspace, saved next to the project so the arrangement
    /// survives a restart. The classic shell's <c>editor-layout.json</c> is untouched until that shell is
    /// removed; this file is the new frontend's own record.
    /// </summary>
    internal sealed class area_layout {
        internal const int version = 2;
        internal const int area_max_leaves = 8;
        internal const float area_min_viewport_width = 160.0F;
        internal const float area_min_viewport_height = 120.0F;
        internal const float area_min_panel_width = 140.0F;
        internal const float area_min_panel_height = 90.0F;
        internal const float area_min_details_width = 180.0F;
        internal const float area_min_details_height = 120.0F;
        internal const float area_min_share = 0.12F;

        internal readonly List<area_workspace> workspaces = [new area_workspace("Layout", default_tree())];
        internal int active_index;

        /// <summary>The active workspace's tree; the callers still talk to one tree at a time.</summary>
        internal area_node root {
            get => workspaces[active_index].root;
            set => workspaces[active_index].root = value;
        }

        internal string active => workspaces[active_index].name;

        /// <summary>Switches the visible workspace; a bad index is ignored rather than clamped silently.</summary>
        internal bool select_workspace(int index) {
            if ((uint)index >= (uint)workspaces.Count || index == active_index) { return false; }
            active_index = index;
            return true;
        }

        /// <summary>
        /// Adds a workspace with a fresh copy of the default tree and names it after the first unused
        /// "Layout N", so the tabs never need to disambiguate two identical names.
        /// </summary>
        internal string add_workspace() {
            string name = next_workspace_name();
            workspaces.Add(new area_workspace(name, default_tree()));
            active_index = workspaces.Count - 1;
            return name;
        }

        /// <summary>Drops the active workspace; the last one stays, because a screen needs a tree.</summary>
        internal bool remove_workspace() {
            if (workspaces.Count <= 1) { return false; }
            workspaces.RemoveAt(active_index);
            active_index = Math.Clamp(active_index, 0, workspaces.Count - 1);
            return true;
        }

        /// <summary>
        /// Renames the active workspace. Blank names are refused, and a name already taken by another
        /// workspace gets a numeric suffix, because the tab strip shows the name and nothing else.
        /// </summary>
        internal bool rename_workspace(string wanted) {
            string name = wanted.Trim();
            if (name.Length == 0) { return false; }
            for (int i = 0; i < workspaces.Count; ++i) {
                if (i != active_index && string.Equals(workspaces[i].name, name, StringComparison.Ordinal)) {
                    string basis = name;
                    for (int suffix = 2;; ++suffix) {
                        name = string.Create(System.Globalization.CultureInfo.InvariantCulture, $"{basis} {suffix}");
                        if (!workspace_named(name)) { break; }
                    }
                    break;
                }
            }
            if (string.Equals(workspaces[active_index].name, name, StringComparison.Ordinal)) { return false; }
            workspaces[active_index].name = name;
            return true;
        }

        internal bool workspace_named(string name) {
            foreach (var workspace in workspaces) {
                if (string.Equals(workspace.name, name, StringComparison.Ordinal)) { return true; }
            }
            return false;
        }

        private string next_workspace_name() {
            for (int index = 2;; ++index) {
                string name = string.Create(System.Globalization.CultureInfo.InvariantCulture, $"Layout {index}");
                if (!workspace_named(name)) { return name; }
            }
        }

        /// <summary>
        /// The arrangement the scene screen has always shown: the preview beside the sidebar, the dock
        /// under the preview, and the outliner over the details in the sidebar. The shares reproduce the
        /// classic panel sizes at a 1600 pixel window (a 250 pixel sidebar, a 170 pixel dock and a 254
        /// pixel outliner), so the first frame looks like the shell it replaces; a drag stores whatever
        /// the reader prefers instead.
        /// </summary>
        internal static area_node default_tree() {
            // The dock gets a third of the preview column: a 150 px tile plus the tool row and the
            // breadcrumb need that much before the grid starts clipping its own content.
            return area_node.split(true,
                (area_node.split(false, (area_node.leaf(area_kind.viewport), 0.66F),
                                (area_node.leaf(area_kind.dock), 0.34F)), 0.84F),
                (area_node.split(false, (area_node.leaf(area_kind.outliner), 0.33F),
                                (area_node.leaf(area_kind.details), 0.67F)), 0.16F));
        }

        internal void save(string path) {
            try {
                var file = new area_file_dto { version = version, active = active };
                file.workspaces = [];
                foreach (var workspace in workspaces) {
                    file.workspaces.Add(new area_workspace_dto { name = workspace.name,
                                                                 tree = to_dto(workspace.root) });
                }
                File.WriteAllText(path, JsonSerializer.Serialize(file));
            } catch (Exception error) when (error is IOException or UnauthorizedAccessException) {
                host_logger.write($"Editor: could not save the area layout ({error.Message})");
            }
        }

        internal static area_layout load(string path) {
            var layout = new area_layout();
            try {
                if (!File.Exists(path)) { return layout; }
                var file = JsonSerializer.Deserialize<area_file_dto>(File.ReadAllText(path));
                if (file == null || file.workspaces == null) { return layout; }
                // Version 1 held one tree and its name; reading it as a one-workspace list is the whole
                // migration, so a project that predates the tab strip keeps the arrangement it had.
                var loaded = new List<area_workspace>();
                foreach (var workspace in file.workspaces) {
                    if (workspace.tree == null || from_dto(workspace.tree) is not { } tree) { continue; }
                    string name = string.IsNullOrWhiteSpace(workspace.name) ? "Layout" : workspace.name.Trim();
                    loaded.Add(new area_workspace(name, tree));
                }
                if (loaded.Count == 0) { return layout; }
                layout.workspaces.Clear();
                foreach (var workspace in loaded) { layout.workspaces.Add(workspace); }
                layout.active_index = 0;
                if (file.active != null) {
                    for (int i = 0; i < layout.workspaces.Count; ++i) {
                        if (string.Equals(layout.workspaces[i].name, file.active, StringComparison.Ordinal)) {
                            layout.active_index = i;
                            break;
                        }
                    }
                }
            } catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) {
                host_logger.write($"Editor: could not read the area layout ({error.Message})");
                return new area_layout();
            }
            return layout;
        }

        /// <summary>The node a dotted path names: "1.1" is the second child of the second child.</summary>
        internal static area_node? node_at(area_node root, string path) {
            if (path.Length == 0) { return root; }
            var node = root;
            foreach (var span in path.Split('.')) {
                if (!int.TryParse(span, System.Globalization.NumberStyles.Integer,
                                  System.Globalization.CultureInfo.InvariantCulture, out var index) ||
                    index < 0 || index >= node.children.Count) {
                    return null;
                }
                node = node.children[index];
            }
            return node;
        }

        /// <summary>
        /// Splits one leaf into two of the same editor type, the way an area's split handle does. The
        /// new split runs across the area's longer side, so a wide preview gains a stacked companion
        /// instead of two narrow strips.
        /// </summary>
        internal static bool divide(area_node root, string path, bool stacked) {
            var leaf = node_at(root, path);
            if (leaf == null || leaf.is_split) { return false; }
            if (leaves(root) >= area_max_leaves) { return false; }
            var first = area_node.leaf(leaf.kind);
            var second = area_node.leaf(leaf.kind);
            leaf.children.Add(first);
            leaf.children.Add(second);
            leaf.shares.Add(0.5F);
            leaf.shares.Add(0.5F);
            leaf.column = !stacked;
            return true;
        }

        /// <summary>
        /// Merges one leaf into its sibling: the leaf leaves the split and its share goes to the nearest
        /// sibling, so the remaining area takes the space instead of leaving a hole. The root leaf has no
        /// sibling to merge into and is refused.
        /// </summary>
        internal static bool join(area_node root, string path) {
            int slash = path.LastIndexOf('.');
            if (slash < 0) { return false; }
            var parent = node_at(root, path[..slash]);
            if (parent == null || parent.is_split == false || parent.children.Count < 2) { return false; }
            if (!int.TryParse(path[(slash + 1)..], System.Globalization.NumberStyles.Integer,
                              System.Globalization.CultureInfo.InvariantCulture, out var index) ||
                index < 0 || index >= parent.children.Count) {
                return false;
            }
            int keeper = index == 0 ? 1 : index - 1;
            parent.shares[keeper] += parent.shares[index];
            parent.children.RemoveAt(index);
            parent.shares.RemoveAt(index);
            parent.normalize();
            if (parent.children.Count == 1) {
                // A split of one child is not a split; the survivor takes its parent's place in place.
                var only = parent.children[0];
                parent.kind = only.kind;
                parent.column = false;
                parent.children = [];
                parent.shares = [];
            }
            return true;
        }

        internal static int leaves(area_node node) {
            if (!node.is_split) { return 1; }
            int total = 0;
            foreach (var child in node.children) { total += leaves(child); }
            return total;
        }

        private static area_dto to_dto(area_node node) {
            if (!node.is_split) {
                return new area_dto { kind = Enum.GetName(node.kind), };
            }
            var dto = new area_dto { kind = "split", axis = node.column ? "column" : "row", };
            foreach (var child in node.children) { dto.children ??= []; dto.children.Add(to_dto(child)); }
            dto.shares = [.. node.shares];
            return dto;
        }

        private static area_node? from_dto(area_dto dto) {
            if (dto.kind == "split") {
                if (dto.children == null || dto.children.Count < 2 || dto.children.Count > 8) { return null; }
                var node = new area_node { column = dto.axis != "row", };
                foreach (var child in dto.children) {
                    if (from_dto(child) is not { } parsed) { return null; }
                    node.children.Add(parsed);
                }
                if (dto.shares != null && dto.shares.Count == node.children.Count) {
                    node.shares = [.. dto.shares];
                } else {
                    for (int i = 0; i < node.children.Count; ++i) { node.shares.Add(1.0F); }
                }
                foreach (var share in node.shares) {
                    if (!float.IsFinite(share) || share <= 0.0F) { return null; }
                }
                node.normalize();
                return node;
            }
            if (dto.kind is { } name && Enum.TryParse<area_kind>(name, false, out var kind)
                && Enum.IsDefined(kind)) {
                return area_node.leaf(kind);
            }
            return null;
        }
    }

    internal sealed class area_dto {
        public string? kind { get; set; }
        public string? axis { get; set; }
        public List<float>? shares { get; set; }
        public List<area_dto>? children { get; set; }
    }

    internal sealed class area_workspace_dto {
        public string? name { get; set; }
        public area_dto? tree { get; set; }
    }

    internal sealed class area_file_dto {
        public int version { get; set; }
        public string? active { get; set; }
        public List<area_workspace_dto>? workspaces { get; set; }
    }
}
