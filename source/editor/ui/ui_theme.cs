namespace deren.editor {
    /// <summary>
    /// Typed theme values for the editor's own UI. M0 keeps them in code because they change with the
    /// library itself; the data-driven theme (colors from a file, hot reload) belongs to M1.
    /// Colors are packed the way the overlay shader reads them: byte 0 red, byte 1 green, byte 2 blue,
    /// byte 3 alpha.
    /// </summary>
    internal static class ui_theme {
        internal static uint rgba(byte red, byte green, byte blue, byte alpha) =>
            (uint)(red | (green << 8) | (blue << 16) | (alpha << 24));

        internal static readonly uint window = rgba(12, 13, 16, 226);
        internal static readonly uint header = rgba(32, 36, 43, 255);
        internal static readonly uint panel = rgba(27, 30, 36, 255);
        internal static readonly uint panel_border = rgba(62, 68, 80, 255);
        internal static readonly uint row_even = rgba(36, 40, 48, 255);
        internal static readonly uint row_odd = rgba(31, 35, 42, 255);
        internal static readonly uint row_selected = rgba(34, 76, 120, 255);
        /// <summary>A lift under the pointer, subtle enough to read over <see cref="panel"/> and a row.</summary>
        internal static readonly uint row_hover = rgba(48, 56, 68, 255);
        internal static readonly uint accent = rgba(45, 111, 184, 255);
        internal static readonly uint accent_border = rgba(88, 158, 226, 255);
        internal static readonly uint outline = rgba(70, 76, 88, 255);
        internal static readonly uint grid = rgba(48, 53, 63, 255);
        internal static readonly uint playhead = rgba(236, 168, 78, 255);
        internal static readonly uint text_bright = rgba(236, 238, 242, 255);
        internal static readonly uint text_dim = rgba(160, 170, 184, 255);
        internal static readonly uint text_key = rgba(169, 190, 209, 255);
        internal static readonly uint key = rgba(150, 196, 236, 255);
        internal static readonly uint key_selected = rgba(255, 214, 148, 255);
        /// <summary>The text selection highlight; translucent so the glyphs stay readable inside it.</summary>
        internal static readonly uint selection = rgba(45, 111, 184, 150);

        internal const float title_size = 13.0F;
        internal const float body_size = 12.0F;
        internal const float small_size = 11.0F;
        internal const float radius = 8.0F;
        internal const float row_height = 22.0F;
        internal const float row_step = 24.0F;
        internal const float gap = 8.0F;
        internal const float padding = 12.0F;
    }
}
