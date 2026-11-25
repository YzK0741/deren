using System;
using System.Collections.Generic;

namespace deren.editor {
    /// <summary>
    /// Turns the area tree into elements. The tree owns the arrangement; the panels keep their own
    /// builders, so an area only decides where a panel sits and how much room it gets.
    /// </summary>
    internal sealed unsafe partial class editor_host {
        private readonly struct area_divider {
            internal area_divider(area_node split, float span) {
                this.split = split;
                this.span = span;
            }

            internal readonly area_node split;
            internal readonly float span;
        }

        private readonly Dictionary<string, area_divider> scene_area_dividers = [];
        private string scene_area_drag = string.Empty;
        // Where the drag started and the share the split had then: the drag is a delta from that press,
        // so pressing a divider never moves it and a drag keeps the offset it was grabbed at.
        private float scene_area_press;
        private float scene_area_press_share;
        private int scene_area_key;
        // The sizes the last build gave the two panels whose lists need them for the wheel and for
        // revealing a row: the outliner list and the dock list.
        private float scene_outliner_leaf_height;
        private float scene_dock_leaf_height;
        // The leaf the header maximized, and the leaf keyed by the path the last build gave it: the
        // header's type menu and its maximize cell both address a leaf by that path.
        private area_node? scene_area_maximized;
        private readonly Dictionary<string, area_node> scene_area_leaves = [];
        // The size each leaf was laid out at, so a split can run across the area's longer side instead
        // of guessing: dividing a wide preview should stack, dividing a tall one should sit side by side.
        private readonly Dictionary<string, (float width, float height)> scene_area_sizes = [];
        private const string scene_area_max_key = "max";

        /// <summary>
        /// Builds one node of the tree at the given rect and returns the element filling it. Splits
        /// distribute their span by share, clamped so no child drops below its minimum; leaves hand
        /// their rect to the panel that owns that editor type.
        /// </summary>
        private ui_element build_area_tree(area_node node, float x, float y, float width, float height,
                                           string path) {
            if (!node.is_split) {
                return build_area_leaf(node, width, height, path);
            }
            string key = "area_split_" + scene_area_key++;
            float thickness = ui_controls.splitter_thickness;
            int count = node.children.Count;
            float total = node.column ? width : height;
            float available = MathF.Max(1.0F, total - thickness * (count - 1));
            var sizes = new float[count];
            var minimums = new float[count];
            float share_total = 0.0F;
            for (int i = 0; i < count; ++i) {
                var (child_min_width, child_min_height) = node.children[i].minimum();
                minimums[i] = node.column ? child_min_width : child_min_height;
                share_total += node.shares[i];
            }
            float given = 0.0F;
            for (int i = 0; i < count; ++i) {
                if (i == count - 1) {
                    sizes[i] = MathF.Max(1.0F, available - given);
                } else {
                    sizes[i] = MathF.Max(minimums[i], available * (node.shares[i] / MathF.Max(share_total, 0.0001F)));
                    given += sizes[i];
                }
            }
            var split = node.column ? ui_node.row(key).size(width, height).spacing(0.0F)
                                    : ui_node.column(key).size(width, height).spacing(0.0F);
            float offset = 0.0F;
            for (int i = 0; i < count; ++i) {
                var (child_width, child_height) = node.column ? (sizes[i], height) : (width, sizes[i]);
                split.add(build_area_tree(node.children[i], x + (node.column ? offset : 0.0F),
                                          y + (node.column ? 0.0F : offset), child_width, child_height,
                                          path.Length == 0
                                              ? i.ToString(System.Globalization.CultureInfo.InvariantCulture)
                                              : path + "." + i.ToString(System.Globalization.CultureInfo.InvariantCulture)));
                offset += sizes[i];
                if (i == count - 1) { continue; }
                string divider = key + "_" + i;
                bool dragging = scene_area_drag == divider;
                scene_area_dividers[divider] = new area_divider(node, available);
                split.add(node.column ? ui_controls.vertical_splitter(divider, next_ui, dragging)
                                      : ui_controls.splitter(divider, next_ui, dragging));
                offset += thickness;
            }
            return split;
        }

        private ui_element build_area_leaf(area_node node, float width, float height, string path) {
            scene_area_leaves[path] = node;
            scene_area_sizes[path] = (width, height);
            switch (node.kind) {
                case area_kind.viewport: {
                    // The preview's header is the two tool strips: the scene actions above, which carry the
                    // editor type and maximize, then the viewport tools with the camera mode on the right.
                    float strips = scene_toolbar_height + scene_tools_height;
                    float preview = MathF.Max(1.0F, height - strips);
                    return ui_node.column("viewport_area").size(width, height).spacing(0.0F)
                                   .add(build_scene_toolbar(node, path, width),
                                        build_scene_toolbar_tools(),
                                        ui_node.box("viewport").size(width, preview));
                }
                case area_kind.outliner: {
                    scene_outliner_leaf_height = height;
                    float list_height = MathF.Max(scene_outliner.row_height,
                                                  height - scene_caption_height - scene_column_header_height);
                    return ui_node.column("outliner").size(width, height).bg(ui_theme.panel).clipped()
                                   .add(build_scene_outliner_caption(node, path, width),
                                        build_scene_outliner_columns(),
                                        build_scene_outliner(list_height));
                }
                case area_kind.details: {
                    float body = MathF.Max(ui_theme.row_height, height - scene_caption_height);
                    return ui_node.column("details_panel").size(width, height).bg(ui_theme.panel).clipped()
                                   .add(build_scene_caption("details_caption", string.Empty, node, path, width),
                                        ui_node.row("details_row").size(float.NaN, body).spacing(0.0F)
                                               .add(build_scene_details(body),
                                                    build_scene_details_scrollbar(body)));
                }
                case area_kind.dope:
                    return build_scene_dope_leaf(node, width, height, path);
                default: {
                    scene_dock_leaf_height = height;
                    float list_height = MathF.Max(scene_browser_list.row_height, height - scene_caption_height);
                    return ui_node.column("browser").size(width, height).bg(ui_theme.panel).clipped()
                                   .add(build_scene_bottom_caption(node, path, width),
                                        build_scene_bottom(list_height, width));
                }
            }
        }

        /// <summary>
        /// The two cells an area header adds to its panel's caption: the editor type on the left, which
        /// opens the type menu, and maximize/restore on the right, which is the leaf the preview fills.
        /// </summary>
        private ui_element build_area_type_cell(area_node node, string path, float height = scene_caption_height - 6.0F) {
            return ui_controls.button("area_type_" + path, area_kinds.name_of(node.kind), ui_button_style.subtle,
                                      next_ui, 84.0F, height);
        }

        private ui_element build_area_max_cell(area_node node, string path, float height = scene_caption_height - 6.0F) {
            bool maximized = ReferenceEquals(scene_area_maximized, node);
            return ui_controls.button("area_max_" + path, maximized ? "Restore" : "Max", ui_button_style.subtle,
                                      next_ui, 62.0F, height);
        }

        /// <summary>
        /// Split and Join, the two arrangement commands an area owns. They are painted only when the header
        /// is wide enough to carry them: a narrow caption keeps its own cells instead of squeezing them.
        /// </summary>
        private ui_element build_area_edit_cells(area_node node, string path, float width) {
            var cells = ui_node.row("area_edit_" + path).size(float.NaN, scene_caption_height).spacing(0.0F);
            if (width < 360.0F || ReferenceEquals(scene_area_maximized, node)) { return cells; }
            float height = scene_caption_height - 6.0F;
            cells.add(ui_controls.button("area_divide_" + path, "Split", ui_button_style.subtle, next_ui, 46.0F,
                                         height));
            cells.add(ui_node.box("area_edit_gap_" + path).size(4.0F, 0.0F));
            if (path.Contains('.', StringComparison.Ordinal)) {
                cells.add(ui_controls.button("area_join_" + path, "Join", ui_button_style.subtle, next_ui, 42.0F,
                                             height));
                cells.add(ui_node.box("area_edit_tail_" + path).size(6.0F, 0.0F));
            }
            return cells;
        }

        /// <summary>The leaf a header path names, or null when the path is stale.</summary>
        private area_node? scene_area_leaf(string path) {
            return scene_area_leaves.TryGetValue(path, out var node) ? node : null;
        }

        /// <summary>
        /// Routes the header cells: a press on the type cell opens the editor-type list under itself, a
        /// press on the maximize cell maximizes that leaf or restores the tree, and the split and join
        /// cells reshape the tree in place.
        /// </summary>
        private bool press_scene_area_header(string key, float x, float y) {
            if (key.StartsWith("area_type_", StringComparison.Ordinal)) {
                string path = key["area_type_".Length..];
                if (scene_area_leaf(path) == null) { return true; }
                var rect = next_ui?.last_tree?.find(key)?.rect ?? new ui_rect(x, y, 0.0F, 0.0F);
                open_scene_combo("area_type|" + path, rect);
                return true;
            }
            if (key.StartsWith("area_divide_", StringComparison.Ordinal)) {
                divide_scene_area(key["area_divide_".Length..]);
                return true;
            }
            if (key.StartsWith("area_join_", StringComparison.Ordinal)) {
                join_scene_area(key["area_join_".Length..]);
                return true;
            }
            if (!key.StartsWith("area_max_", StringComparison.Ordinal)) { return false; }
            var leaf = scene_area_leaf(key["area_max_".Length..]);
            if (leaf == null) { return true; }
            scene_area_maximized = ReferenceEquals(scene_area_maximized, leaf) ? null : leaf;
            save_scene_areas();
            notice = scene_area_maximized == null ? "Areas restored" : "Area maximized";
            return true;
        }

        /// <summary>
        /// Splits the area across its longer side: a wide preview gains a stacked companion, a tall one a
        /// neighbour. Both halves start with the same editor type, which is what an area split does.
        /// </summary>
        private void divide_scene_area(string path) {
            var size = scene_area_sizes.TryGetValue(path, out var measured) ? measured : (width: 1.0F, height: 1.0F);
            bool stacked = size.width >= size.height;
            if (!area_layout.divide(scene_areas.root, path, stacked)) {
                notice = "The areas are full";
                return;
            }
            scene_area_maximized = null;
            save_scene_areas();
            notice = "Area split";
        }

        /// <summary>Merges the area into its nearest sibling and gives the sibling the space.</summary>
        private void join_scene_area(string path) {
            scene_area_maximized = null;
            if (!area_layout.join(scene_areas.root, path)) {
                notice = "This area has no sibling to join";
                return;
            }
            save_scene_areas();
            notice = "Areas joined";
        }

        /// <summary>Switches one leaf's editor type, the way a Blender area's type menu does.</summary>
        private void apply_scene_area_kind(string path, area_kind kind) {
            var leaf = scene_area_leaf(path);
            if (leaf == null) { return; }
            leaf.kind = kind;
            scene_area_maximized = null;
            save_scene_areas();
            notice = "Area: " + area_kinds.name_of(kind);
        }

        /// <summary>
        /// One drag step: the pointer's travel since the press is added to the share the split had then,
        /// clamped by both children's minimum and by the smallest share. Binary splits are what the
        /// default tree uses; a wider split keeps its other shares.
        /// </summary>
        private void update_area_drag(float x, float y) {
            if (next_ui == null) { return; }
            if (!scene_area_dividers.TryGetValue(scene_area_drag, out var divider)) { return; }
            if (divider.split.children.Count != 2) { return; }
            float pointer = divider.split.column ? x : y;
            float span = MathF.Max(1.0F, divider.span);
            float low = area_layout.area_min_share;
            float high = 1.0F - area_layout.area_min_share;
            var (first_min_width, first_min_height) = divider.split.children[0].minimum();
            var (second_min_width, second_min_height) = divider.split.children[1].minimum();
            float first_minimum = divider.split.column ? first_min_width : first_min_height;
            float second_minimum = divider.split.column ? second_min_width : second_min_height;
            low = MathF.Max(low, first_minimum / span);
            high = MathF.Min(high, 1.0F - second_minimum / span);
            if (low > high) { low = high = 0.5F; }
            float share = Math.Clamp(scene_area_press_share + (pointer - scene_area_press) / span, low, high);
            divider.split.shares[0] = share;
            divider.split.shares[1] = 1.0F - share;
        }

        /// <summary>Starts a divider drag: remembers the press axis and the share the split has now.</summary>
        private bool begin_area_drag(string key, float x, float y) {
            if (!scene_area_dividers.TryGetValue(key, out var divider)) { return false; }
            if (divider.split.children.Count != 2) { return false; }
            scene_area_drag = key;
            scene_area_press = divider.split.column ? x : y;
            scene_area_press_share = divider.split.shares[0];
            return true;
        }

        private void save_scene_areas() {
            if (scene_area_path.Length == 0) { return; }
            scene_areas.save(scene_area_path);
        }

        /// <summary>The outliner list height of the last build.</summary>
        private float scene_outliner_list_height() {
            return MathF.Max(scene_outliner.row_height,
                             scene_outliner_leaf_height - scene_caption_height - scene_column_header_height);
        }

        /// <summary>The dock list height of the last build.</summary>
        private float scene_dock_list_height() {
            return MathF.Max(scene_browser_list.row_height, scene_dock_leaf_height - scene_caption_height);
        }
    }
}
