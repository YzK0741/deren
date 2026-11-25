using System;
using System.Text;
using deren.scripting;

namespace deren.editor;

/// <summary>
/// Shapen glyphs of one run, exactly as the overlay shapes them. The painter and the caret have to see
/// the same clusters and advances, so anything that needs per-glyph positions asks here instead of
/// re-implementing shaping.
/// </summary>
internal static unsafe class ui_text_shape {
    /// <summary>
    /// Shapes @p text and returns the glyphs in paint order. An empty result means the overlay could not
    /// shape (no frontend, or an empty run), which callers treat as "no glyphs", never as an error.
    /// </summary>
    internal static editor_ui_glyph_layout[] shape(string text, float size, out float width) {
        width = 0.0F;
        if (text.Length == 0 || !editor_ui_api.is_ready()) { return []; }
        var bytes = Encoding.UTF8.GetBytes(text);
        fixed (byte* data = bytes) {
            var view = new utf8_view { data = data, length = (uint)bytes.Length };
            uint count = 0;
            float measured = 0.0F;
            if (editor_ui_api.table.shape_text(view, size, null, 0, &count, &measured) != script_status.success) {
                return [];
            }
            width = measured;
            if (count == 0) { return []; }
            var glyphs = new editor_ui_glyph_layout[count];
            fixed (editor_ui_glyph_layout* buffer = glyphs) {
                uint written = 0;
                float shaped_width = 0.0F;
                if (editor_ui_api.table.shape_text(view, size, buffer, count, &written, &shaped_width) !=
                    script_status.success || written == 0) {
                    return [];
                }
                width = shaped_width;
                if (written < count) { Array.Resize(ref glyphs, (int)written); }
            }
            return glyphs;
        }
    }
}
