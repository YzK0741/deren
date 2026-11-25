using System;
using System.Collections.Generic;
using System.Text;
using deren.scripting;

namespace deren.editor {
    /// <summary>
    /// Collects one frame of primitives and hands it to the native overlay. The library batches by
    /// clip: consecutive primitives under the same clip form one draw range, and the overlay scissors
    /// each range while recording. Text is appended to a single UTF-8 blob and referenced by
    /// offset and length, so a frame is one packet with borrowed spans and no per-string allocation
    /// after the blob has grown.
    /// </summary>
    internal sealed unsafe class ui_paint {
        private struct rect_command {
            internal ui_rect rect;
            internal float radius;
            internal uint fill;
            internal uint border;
            internal float border_width;
        }

        private struct text_command {
            internal float x;
            internal float y;
            internal float size;
            internal uint color;
            internal int offset;
            internal int length;
        }

        private struct draw_range {
            internal ui_rect clip;
            internal int rect_first;
            internal int rect_count;
            internal int text_first;
            internal int text_count;
        }

        private readonly List<rect_command> rects = [];
        private readonly List<text_command> texts = [];
        private readonly List<draw_range> draws = [];
        private readonly List<ui_rect> clip_stack = [];
        private readonly Dictionary<(string text, float size, float scale), (float width, float height)> measured = [];

        private byte[] blob = new byte[16384];
        private int blob_length;

        private editor_ui_rect_layout[] rect_buffer = new editor_ui_rect_layout[512];
        private editor_ui_text_layout[] text_buffer = new editor_ui_text_layout[256];
        private editor_ui_draw_layout[] draw_buffer = new editor_ui_draw_layout[32];

        private float width;
        private float height;
        private int open_rect_first;
        private int open_text_first;
        private bool draw_open;

        /// <summary>Vertex budget of the overlay module; the library stops emitting past it.</summary>
        internal const int max_rects = 16384;
        internal const int max_texts = 4096;

        internal void reset(float frame_width, float frame_height) {
            width = frame_width;
            height = frame_height;
            rects.Clear();
            texts.Clear();
            draws.Clear();
            clip_stack.Clear();
            blob_length = 0;
            open_rect_first = 0;
            open_text_first = 0;
            draw_open = false;
        }

        /// <summary>The area a primitive may touch; <see cref="ui_rect.empty"/> means nothing may be drawn.</summary>
        internal ui_rect clip => clip_stack.Count == 0 ? new ui_rect(0.0F, 0.0F, width, height) : clip_stack[^1];

        internal void push_clip(ui_rect area) {
            close_draw();
            var current = clip.intersect(area);
            clip_stack.Add(current);
        }

        internal void pop_clip() {
            if (clip_stack.Count == 0) { return; }
            close_draw();
            clip_stack.RemoveAt(clip_stack.Count - 1);
        }

        internal void add_rect(ui_rect area, float radius, uint fill, uint border, float border_width) {
            if (area.is_empty || (fill == 0 && border == 0)) { return; }
            if (!area.intersects(clip) || rects.Count >= max_rects) { return; }
            ensure_draw();
            rects.Add(new rect_command { rect = area, radius = radius, fill = fill, border = border, border_width = border_width });
        }

        internal void add_text(float x, float y, float size, uint color, string value) {
            if (value.Length == 0 || color == 0 || texts.Count >= max_texts) { return; }
            if (!clip.contains(x, y) && !clip.contains(x, y + size)) { return; }
            ensure_draw();
            var bytes = Encoding.UTF8.GetBytes(value);
            if (blob_length + bytes.Length > blob.Length) {
                Array.Resize(ref blob, Math.Max(blob.Length * 2, blob_length + bytes.Length));
            }
            bytes.CopyTo(blob, blob_length);
            texts.Add(new text_command {
                x = x, y = y, size = size, color = color, offset = blob_length, length = bytes.Length,
            });
            blob_length += bytes.Length;
        }

        /// <summary>
        /// Measures a run with the overlay's font. Results are cached because layout asks the same
        /// label every frame and the overlay call crosses the managed boundary. The cache is keyed by
        /// the scale as well: the overlay reports logical widths for the density it last rendered at,
        /// so a stale entry would misalign labels after a scale change.
        /// </summary>
        internal static (float width, float height) measure(string text, float size) {
            if (text.Length == 0) { return (0.0F, size); }
            float scale = current_scale > 0.0F ? current_scale : 1.0F;
            var key = (text, size, scale);
            if (instance != null && instance.measured.TryGetValue(key, out var cached)) { return cached; }
            (float width, float height) result = (text.Length * size * 0.55F, size * 1.25F);
            if (editor_ui_api.is_ready()) {
                var bytes = Encoding.UTF8.GetBytes(text);
                fixed (byte* data = bytes) {
                    var view = new utf8_view { data = data, length = (uint)bytes.Length };
                    float measured_width = 0.0F;
                    float measured_height = 0.0F;
                    if (editor_ui_api.table.measure_text(view, size, &measured_width, &measured_height) ==
                        script_status.success && measured_width > 0.0F) {
                        result = (measured_width, measured_height > 0.0F ? measured_height : size);
                    }
                }
            }
            instance?.measured[key] = result;
            return result;
        }

        private static ui_paint? instance;
        private static float current_scale = 1.0F;

        internal static void bind(ui_paint paint) => instance = paint;

        /// <summary>The density the next frame renders at; the measure cache is keyed by it.</summary>
        internal static void set_scale(float scale) => current_scale = scale > 0.0F ? scale : 1.0F;

        private void ensure_draw() {
            if (draw_open) { return; }
            draws.Add(new draw_range {
                clip = clip,
                rect_first = rects.Count,
                text_first = texts.Count,
            });
            open_rect_first = rects.Count;
            open_text_first = texts.Count;
            draw_open = true;
        }

        private void close_draw() {
            if (!draw_open) { return; }
            var last = draws[^1];
            last.rect_count = rects.Count - open_rect_first;
            last.text_count = texts.Count - open_text_first;
            draws[^1] = last;
            draw_open = false;
        }

        /// <summary>Packs the frame and submits it. Returns false when the overlay module is gone.</summary>
        /// <param name="display_scale">
        /// Framebuffer pixels per logical pixel. The packet stays in logical units and the overlay
        /// multiplies, which keeps text and rounded corners sharp at 150% and 200%.
        /// </param>
        internal bool submit(float display_scale) {
            close_draw();
            if (!editor_ui_api.is_ready()) { return false; }
            if (rect_buffer.Length < rects.Count) { rect_buffer = new editor_ui_rect_layout[rects.Count]; }
            if (text_buffer.Length < texts.Count) { text_buffer = new editor_ui_text_layout[texts.Count]; }
            if (draw_buffer.Length < draws.Count) { draw_buffer = new editor_ui_draw_layout[draws.Count]; }
            for (var index = 0; index < rects.Count; ++index) {
                var command = rects[index];
                fixed (editor_ui_rect_layout* buffer = rect_buffer) {
                    var entry = buffer + index;
                    entry->x = command.rect.x;
                    entry->y = command.rect.y;
                    entry->width = command.rect.width;
                    entry->height = command.rect.height;
                    entry->radius[0] = command.radius;
                    entry->radius[1] = command.radius;
                    entry->radius[2] = command.radius;
                    entry->radius[3] = command.radius;
                    entry->fill = command.fill;
                    entry->border = command.border;
                    entry->border_width = command.border_width;
                    entry->flags = (command.fill != 0 ? 1U : 0U) | (command.border != 0 ? 2U : 0U);
                }
            }
            for (var index = 0; index < texts.Count; ++index) {
                var command = texts[index];
                fixed (editor_ui_text_layout* buffer = text_buffer) {
                    var entry = buffer + index;
                    entry->x = command.x;
                    entry->y = command.y;
                    entry->size = command.size;
                    entry->color = command.color;
                    entry->offset = (uint)command.offset;
                    entry->length = (uint)command.length;
                }
            }
            for (var index = 0; index < draws.Count; ++index) {
                var command = draws[index];
                fixed (editor_ui_draw_layout* buffer = draw_buffer) {
                    var entry = buffer + index;
                    entry->rect_first = (uint)command.rect_first;
                    entry->rect_count = (uint)command.rect_count;
                    entry->text_first = (uint)command.text_first;
                    entry->text_count = (uint)command.text_count;
                    entry->clip.x = command.clip.x;
                    entry->clip.y = command.clip.y;
                    entry->clip.width = command.clip.width;
                    entry->clip.height = command.clip.height;
                }
            }
            var frame = new editor_ui_frame_layout {
                version = 1,
                size = (uint)sizeof(editor_ui_frame_layout),
                width = width,
                height = height,
                scale = display_scale > 0.0F ? display_scale : 1.0F,
                rect_count = (uint)rects.Count,
                text_count = (uint)texts.Count,
                blob_size = (uint)blob_length,
                draw_count = (uint)draws.Count,
            };
            fixed (editor_ui_rect_layout* rect_pointer = rect_buffer)
            fixed (editor_ui_text_layout* text_pointer = text_buffer)
            fixed (editor_ui_draw_layout* draw_pointer = draw_buffer)
            fixed (byte* blob_pointer = blob) {
                frame.rects = rect_pointer;
                frame.texts = text_pointer;
                frame.draws = draw_pointer;
                frame.blob = blob_pointer;
                return editor_ui_api.table.submit(&frame) == script_status.success;
            }
        }
    }
}
