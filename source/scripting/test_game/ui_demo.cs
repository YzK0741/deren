using System;
using System.Runtime.InteropServices;
using System.Text;
using deren.scripting;

namespace game {
    /// <summary>
    /// ABI 47 demo frame for the editor's own UI library. It paints one frame per tick with the
    /// shapes and text the library offers today: rounded panels with a border, alternating rows, a
    /// filled slider track and buttons, all clipped by draw ranges. The frame is plain data, so the
    /// editor can submit the same structures later.
    /// </summary>
    internal static unsafe class ui_demo {
        private const int rect_count = 14;
        private const int text_count = 20;
        private const int draw_count = 4;

        private static byte[] blob = [];
        private static int blob_length;

        private static uint rgba(byte r, byte g, byte b, byte a) =>
            (uint)(r | (g << 8) | (b << 16) | (a << 24));

        private static readonly uint background = rgba(12, 13, 16, 200);
        private static readonly uint header = rgba(32, 36, 43, 255);
        private static readonly uint panel = rgba(27, 30, 36, 255);
        private static readonly uint panel_border = rgba(62, 68, 80, 255);
        private static readonly uint row_even = rgba(36, 40, 48, 255);
        private static readonly uint row_odd = rgba(31, 35, 42, 255);
        private static readonly uint row_selected = rgba(34, 76, 120, 255);
        private static readonly uint accent = rgba(45, 111, 184, 255);
        private static readonly uint accent_border = rgba(88, 158, 226, 255);
        private static readonly uint outline = rgba(70, 76, 88, 255);
        private static readonly uint text_bright = rgba(236, 238, 242, 255);
        private static readonly uint text_dim = rgba(160, 170, 184, 255);
        private static readonly uint text_key = rgba(169, 190, 209, 255);

        private static readonly string[] channel_names = [
            "Bip001", "Bip001_Pelvis", "Bip001_L_Thigh", "Bip001_L_Calf", "Bip001_R_Thigh", "Bip001_Spine",
        ];

        private static void write_text(editor_ui_text_layout* text, float x, float y, float size,
                                       uint color, string value) {
            var bytes = Encoding.UTF8.GetBytes(value);
            if (blob_length + bytes.Length > blob.Length) {
                Array.Resize(ref blob, Math.Max(blob.Length * 2, blob_length + bytes.Length));
            }
            bytes.CopyTo(blob, blob_length);
            text->x = x;
            text->y = y;
            text->size = size;
            text->color = color;
            text->offset = (uint)blob_length;
            text->length = (uint)bytes.Length;
            blob_length += bytes.Length;
        }

        private static void write_rect(editor_ui_rect_layout* rect, float x, float y, float width, float height,
                                       float radius, uint fill, uint border, float border_width) {
            rect->x = x;
            rect->y = y;
            rect->width = width;
            rect->height = height;
            rect->radius[0] = radius;
            rect->radius[1] = radius;
            rect->radius[2] = radius;
            rect->radius[3] = radius;
            rect->fill = fill;
            rect->border = border;
            rect->border_width = border_width;
            rect->flags = (fill != 0 ? 1u : 0u) | (border != 0 ? 2u : 0u);
        }

        internal static script_status draw(float width, float height) {
            if (!editor_ui_api.is_ready()) {
                return script_status.module_unavailable;
            }
            if (blob.Length == 0) {
                blob = new byte[1024];
            }
            blob_length = 0;

            // The demo has to fit the smallest window the demo runs at (640x360) and still fill a
            // large one, so every region is derived from the frame size instead of being fixed.
            const float margin = 20.0F;
            const float header_height = 32.0F;
            const float footer_height = 24.0F;
            const float row_height = 22.0F;
            const float row_step = 24.0F;
            var panel_x = margin;
            var panel_y = header_height + 12.0F;
            var panel_width = Math.Min(560.0F, width - 2 * margin);
            var strip_y = height - footer_height - 68.0F;
            var buttons_y = strip_y + 30.0F;
            var panel_height = strip_y - panel_y - 12.0F;
            var row_y = panel_y + 30.0F;

            var rects = stackalloc editor_ui_rect_layout[rect_count];
            var texts = stackalloc editor_ui_text_layout[text_count];
            var draws = stackalloc editor_ui_draw_layout[draw_count];

            // Backdrop and header row.
            write_rect(rects + 0, 0, 0, width, height, 0, background, 0, 0);
            write_rect(rects + 1, 0, 0, width, header_height, 0, header, 0, 0);
            write_text(texts + 0, 16, 9, 14, text_bright, "Deren editor UI (ABI 47)");
            write_text(texts + 1, width - 190, 10, 12, text_dim, "one frame, plain data");

            // Dope Sheet panel: a bordered card, a title and six channel rows.
            write_rect(rects + 2, panel_x, panel_y, panel_width, panel_height, 8, panel, panel_border, 1);
            write_text(texts + 2, panel_x + 16, panel_y + 8, 13, text_bright,
                       "Dope Sheet - laevatain / Bibideba");
            for (var row = 0; row < channel_names.Length; ++row) {
                var y = row_y + row * row_step;
                write_rect(rects + 3 + row, panel_x + 10, y, panel_width - 20, row_height, 4,
                           row == 0 ? row_selected : (row % 2 == 0 ? row_even : row_odd), 0, 0);
                write_text(texts + 3 + row, panel_x + 20, y + 3, 12, text_bright, channel_names[row]);
                write_text(texts + 9 + row, panel_x + panel_width - 110, y + 3, 12, text_key, "1289 keys");
            }

            // Keys strip: a track, a filled playback region, two time labels and two buttons.
            write_rect(rects + 9, margin, strip_y, panel_width, 24, 5, row_even, outline, 1);
            write_rect(rects + 10, margin + 2, strip_y + 2, panel_width * 0.42F, 20, 4, accent, 0, 0);
            write_text(texts + 15, margin + 8, strip_y + 5, 11, text_bright, "0.00 s");
            write_text(texts + 16, margin + panel_width - 62, strip_y + 5, 11, text_bright, "42.93 s");
            write_rect(rects + 11, margin, buttons_y, 118, 26, 6, accent, accent_border, 1);
            write_text(texts + 17, margin + 42, buttons_y + 6, 13, text_bright, "Play");
            write_rect(rects + 12, margin + 130, buttons_y, 138, 26, 6, 0, outline, 1);
            write_text(texts + 18, margin + 152, buttons_y + 6, 13, text_bright, "Save Motion");

            // Footer status line. The trailing CJK comes from the fallback face, so this line proves the
            // shaped pipeline, the cluster mapping and the second font at once.
            write_rect(rects + 13, 0, height - footer_height, width, footer_height, 0, header, 0, 0);
            write_text(texts + 19, 16, height - footer_height + 5, 12, text_dim,
                       "editor ui overlay: rounded rects, glyph atlas text and draw clips \u52a8\u753b\u7f16\u8f91\u5668 \u5173\u952e\u5e27");

            draws[0].rect_first = 0;
            draws[0].rect_count = 2;
            draws[0].text_first = 0;
            draws[0].text_count = 2;
            draws[0].clip = new editor_ui_clip_layout { x = 0, y = 0, width = width, height = height };

            draws[1].rect_first = 2;
            draws[1].rect_count = 7;
            draws[1].text_first = 2;
            draws[1].text_count = 13;
            draws[1].clip = new editor_ui_clip_layout { x = panel_x, y = panel_y, width = panel_width, height = panel_height };

            draws[2].rect_first = 9;
            draws[2].rect_count = 4;
            draws[2].text_first = 15;
            draws[2].text_count = 4;
            draws[2].clip = new editor_ui_clip_layout { x = margin, y = strip_y - 4, width = panel_width, height = 68 };

            draws[3].rect_first = 13;
            draws[3].rect_count = 1;
            draws[3].text_first = 19;
            draws[3].text_count = 1;
            draws[3].clip = new editor_ui_clip_layout { x = 0, y = height - footer_height, width = width, height = footer_height };

            fixed (byte* blob_pointer = blob) {
                var frame = new editor_ui_frame_layout {
                    version = 1,
                    size = (uint)sizeof(editor_ui_frame_layout),
                    width = width,
                    height = height,
                    scale = 1.0f,
                    rects = rects,
                    rect_count = rect_count,
                    texts = texts,
                    text_count = text_count,
                    blob = blob_pointer,
                    blob_size = (uint)blob_length,
                    draws = draws,
                    draw_count = draw_count,
                };
                return editor_ui_api.table.submit(&frame);
            }
        }
    }
}
