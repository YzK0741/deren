using System;
using System.Collections.Generic;

namespace deren.editor {
    /// <summary>
    /// Owns one frame of the editor's own interface: it measures and arranges the declarative tree,
    /// paints it into the overlay packet and submits that packet. State that has to survive a frame
    /// lives here, keyed by the widget key, because the tree itself is rebuilt every frame.
    /// </summary>
    internal sealed class ui_context {
        private readonly ui_paint paint = new();
        private ui_element? tree;
        private readonly List<(ui_element element, float x, float y)> overlays = [];

        internal float width = 1.0F;
        internal float height = 1.0F;
        internal float display_scale = 1.0F;
        internal int frame_index;

        // Per-key state that outlives one frame.
        internal string hovered_key = string.Empty;
        internal string focused_key = string.Empty;
        internal string active_key = string.Empty;
        internal readonly Dictionary<string, float> scroll = [];

        // The pointer in logical pixels; the hover key is derived from it once the tree is arranged,
        // so a control can paint its hot state without the screen threading the pointer through.
        internal float pointer_x;
        internal float pointer_y;
        internal bool pointer_known;

        internal ui_element? last_tree => tree;

        /// <summary>Records where the pointer is; the next submit turns it into <see cref="hovered_key"/>.</summary>
        internal void set_pointer(float x, float y) {
            pointer_x = x;
            pointer_y = y;
            pointer_known = true;
        }

        internal bool is_hovered(string key) => hovered_key == key;

        internal void begin(float frame_width, float frame_height, float scale) {
            width = MathF.Max(1.0F, frame_width);
            height = MathF.Max(1.0F, frame_height);
            display_scale = scale > 0.0F ? scale : 1.0F;
            ui_paint.set_scale(display_scale);
            ++frame_index;
        }

        /// <summary>Lays out and submits one tree. Returns false when the overlay refused the frame.</summary>
        internal bool submit(ui_element root) {
            paint_begin();
            tree = root;
            ui_layout_engine.measure(root, width);
            ui_layout_engine.arrange(root, new ui_rect(0.0F, 0.0F, width, height));
            foreach (var overlay in overlays) {
                ui_layout_engine.measure(overlay.element, width);
                ui_layout_engine.arrange(overlay.element, new ui_rect(overlay.x, overlay.y, overlay.element.width,
                                                                      overlay.element.height));
            }
            hovered_key = pointer_known ? hit_test(pointer_x, pointer_y)?.key ?? string.Empty : string.Empty;
            paint_element(root);
            foreach (var overlay in overlays) {
                paint_element(overlay.element);
            }
            return paint.submit(display_scale);
        }

        /// <summary>The topmost overlay of the last submit, or null; the selftest finds popup entries in it.</summary>
        internal ui_element? overlay_root => overlays.Count == 0 ? null : overlays[^1].element;

        /// <summary>
        /// Empties the overlay queue. The builder calls this before it queues the current frame's popups;
        /// the previous ones stay hittable until then, which is the same one-frame-late contract the
        /// pointer handlers already live by.
        /// </summary>
        internal void clear_overlays() => overlays.Clear();

        /// <summary>
        /// Queues one more element to lay out and paint after the tree, at a position of its own. A popup,
        /// a menu or a dialog belongs here: it must not join the flow of the screen it floats over, it must
        /// paint last and it must be hit-tested first. The queue is emptied by every submit.
        /// </summary>
        internal void add_overlay(ui_element element, float x, float y) => overlays.Add((element, x, y));

        /// <summary>Deepest element under a logical-pixel point, or null when the point misses the tree.</summary>
        internal ui_element? hit_test(float x, float y) {
            for (var index = overlays.Count - 1; index >= 0; --index) {
                if (hit_test(overlays[index].element, x, y) is { } found) { return found; }
            }
            return tree == null ? null : hit_test(tree, x, y);
        }

        private static ui_element? hit_test(ui_element element, float x, float y) {
            if (!element.rect.contains(x, y)) { return null; }
            for (var index = element.children.Count - 1; index >= 0; --index) {
                if (hit_test(element.children[index], x, y) is { } found) { return found; }
            }
            return element;
        }

        private void paint_begin() {
            paint.reset(width, height);
            ui_paint.bind(paint);
        }

        private void paint_element(ui_element element) {
            bool clipped = element.clip_children;
            if (clipped) { paint.push_clip(element.rect); }
            if (element.background != 0 || element.border != 0) {
                paint.add_rect(element.rect, element.radius, element.background, element.border, element.border_width);
            }
            element.painter?.Invoke(paint, element.content);
            foreach (var child in element.children) {
                paint_element(child);
            }
            if (element.text.Length > 0 && !element.content.is_empty) {
                var (text_width, text_height) = ui_paint.measure(element.text, element.font_size);
                float text_x = element.text_right
                    ? element.content.right - text_width
                    : element.text_center_x
                        ? element.content.x + (element.content.width - text_width) * 0.5F
                        : element.content.x;
                float text_y = element.text_center
                    ? element.content.y + (element.content.height - text_height) * 0.5F
                    : element.content.y;
                paint.add_text(text_x, text_y, element.font_size, element.text_color, element.text);
            }
            if (clipped) { paint.pop_clip(); }
        }
    }
}
