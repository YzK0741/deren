using System;
using System.Collections.Generic;

namespace deren.editor {
    internal enum ui_layout {
        /// <summary>A leaf box: it paints its own background, text and painter, and lays out nothing.</summary>
        none,
        row,
        column,
    }

    /// <summary>
    /// One node of the editor's declarative interface. A screen describes its widgets as a fresh tree
    /// every frame; the tree is measured, arranged, painted and dropped again. Identity that has to
    /// survive a frame (scroll, focus, selection) is stored by key in <see cref="ui_context"/>, never
    /// in the node itself.
    /// Sizes are logical pixels. A width or height of NaN means "as small as the content needs".
    /// </summary>
    internal sealed class ui_element {
        internal string key = string.Empty;
        internal ui_layout layout = ui_layout.none;
        internal readonly List<ui_element> children = [];

        // Box style.
        internal float width = float.NaN;
        internal float height = float.NaN;
        internal float grow;
        internal float padding;
        internal float gap;
        internal uint background;
        internal uint border;
        internal float border_width;
        internal float radius;
        internal bool clip_children;

        // Text style; an empty string means no text.
        internal string text = string.Empty;
        internal float font_size = ui_theme.body_size;
        internal uint text_color = ui_theme.text_bright;
        internal bool text_right;
        internal bool text_center;
        /// <summary>Centers the text horizontally inside the content box, as a button label needs.</summary>
        internal bool text_center_x;

        /// <summary>Extra content painted after the background and inside the element's clip.</summary>
        internal Action<ui_paint, ui_rect>? painter;

        // Layout results, valid only for the frame that just arranged this element.
        internal ui_rect rect;
        internal ui_rect content;

        internal ui_element add(params ui_element[] items) {
            children.AddRange(items);
            return this;
        }

        internal ui_element bg(uint color, float corner = 0.0F) {
            background = color;
            radius = corner;
            return this;
        }

        internal ui_element stroke(uint color, float stroke_width = 1.0F) {
            border = color;
            border_width = stroke_width;
            return this;
        }

        internal ui_element size(float fixed_width, float fixed_height) {
            width = fixed_width;
            height = fixed_height;
            return this;
        }

        internal ui_element min_height(float fixed_height) {
            height = fixed_height;
            return this;
        }

        internal ui_element pad(float amount) {
            padding = amount;
            return this;
        }

        internal ui_element weight(float amount) {
            grow = amount;
            return this;
        }

        internal ui_element spacing(float amount) {
            gap = amount;
            return this;
        }

        internal ui_element clipped() {
            clip_children = true;
            return this;
        }

        /// <summary>Extra content painted inside the clip, such as keys on a timeline lane.</summary>
        internal ui_element painted(Action<ui_paint, ui_rect> body) {
            painter = body;
            return this;
        }

        internal ui_element label_text(string value, float size, uint color, bool right = false, bool centered = false) {
            text = value;
            font_size = size;
            text_color = color;
            text_right = right;
            text_center = centered;
            text_center_x = centered;
            return this;
        }

        /// <summary>Finds a child by key; the screen uses it to update one widget between rebuilds.</summary>
        internal ui_element? find(string wanted) {
            if (key == wanted) { return this; }
            foreach (var child in children) {
                if (child.find(wanted) is { } found) { return found; }
            }
            return null;
        }
    }

    /// <summary>Small constructors that keep screen code readable: layout containers and leaf labels.</summary>
    internal static class ui_node {
        internal static ui_element row(string key, float gap = 0.0F) =>
            new() { key = key, layout = ui_layout.row, gap = gap };

        internal static ui_element column(string key, float gap = 0.0F) =>
            new() { key = key, layout = ui_layout.column, gap = gap };

        internal static ui_element box(string key) => new() { key = key };

        internal static ui_element label(string key, string text, float size, uint color, bool right = false) {
            var created = new ui_element { key = key, text = text, font_size = size, text_color = color, text_right = right };
            created.height = size + 3.0F;
            return created;
        }

        /// <summary>Eats the leftover space of its row or column, so the widgets after it stay right-aligned.</summary>
        /// <summary>
    /// Empty space that absorbs the leftover width of a row. The key defaults to "spacer" for a layout-only
    /// filler, but a row that reacts to presses passes its own key, because the hit test reports the
    /// deepest element and a shared key would swallow every click on the blank part of that row.
    /// </summary>
    internal static ui_element spacer(string key = "spacer", float weight = 1.0F) =>
        new() { key = key, grow = weight };
    }

    /// <summary>
    /// Two-pass layout: measure the wanted size bottom-up, then arrange the final rectangles top-down.
    /// Row and column containers give each child its measured size plus a share of the leftover space
    /// proportional to its weight; a child that asked for a fixed size keeps it.
    /// </summary>
    internal static class ui_layout_engine {
        internal static void measure(ui_element element, float available_width) {
            float inner = MathF.Max(0.0F, available_width - element.padding * 2.0F -
                                             element.gap * Math.Max(0, element.children.Count - 1));
            foreach (var child in element.children) {
                measure(child, inner);
            }
            if (element.layout == ui_layout.none) {
                if (float.IsNaN(element.width)) {
                    element.width = element.text.Length == 0
                        ? element.padding * 2.0F
                        : ui_paint.measure(element.text, element.font_size).width + element.padding * 2.0F;
                }
                if (float.IsNaN(element.height)) {
                    element.height = element.text.Length == 0
                        ? element.padding * 2.0F
                        : ui_paint.measure(element.text, element.font_size).height + element.padding * 2.0F;
                }
                return;
            }
            float along = 0.0F;
            float across = 0.0F;
            bool horizontal = element.layout == ui_layout.row;
            for (var index = 0; index < element.children.Count; ++index) {
                var child = element.children[index];
                if (index > 0) { along += element.gap; }
                along += horizontal ? child.width : child.height;
                across = MathF.Max(across, horizontal ? child.height : child.width);
            }
            along += element.padding * 2.0F;
            across += element.padding * 2.0F;
            if (float.IsNaN(element.width)) {
                element.width = horizontal ? along : across;
            }
            if (float.IsNaN(element.height)) {
                element.height = horizontal ? across : along;
            }
        }

        internal static void arrange(ui_element element, ui_rect area) {
            element.rect = area;
            element.content = area.inset(element.padding);
            if (element.content.is_empty) { element.content = ui_rect.empty; }
            if (element.layout == ui_layout.none || element.children.Count == 0) { return; }

            bool horizontal = element.layout == ui_layout.row;
            float available = (horizontal ? element.content.width : element.content.height) -
                              element.gap * (element.children.Count - 1);
            float fixed_total = 0.0F;
            float weight_total = 0.0F;
            foreach (var child in element.children) {
                fixed_total += horizontal ? child.width : child.height;
                weight_total += MathF.Max(0.0F, child.grow);
            }
            float leftover = MathF.Max(0.0F, available - fixed_total);
            float offset = 0.0F;
            foreach (var child in element.children) {
                float base_size = horizontal ? child.width : child.height;
                float grow = MathF.Max(0.0F, child.grow);
                float length = base_size + (weight_total > 0.0F ? leftover * grow / weight_total : 0.0F);
                float cross = horizontal ? element.content.height : element.content.width;
                var child_area = horizontal
                    ? new ui_rect(element.content.x + offset, element.content.y, length, cross)
                    : new ui_rect(element.content.x, element.content.y + offset, cross, length);
                arrange(child, child_area);
                offset += length + element.gap;
            }
        }
    }
}
