using System;

namespace deren.editor {
    /// <summary>How a push button is drawn: a filled call to action, an outlined button, or a label that
    /// only lights up under the pointer.</summary>
    internal enum ui_button_style {
        filled,
        outlined,
        subtle,
    }

    /// <summary>
    /// The reusable widgets the editor screens share. A screen describes its tree every frame and the
    /// context has already hit-tested the pointer by the time a control paints, so a control reads its
    /// hot state from the context instead of carrying it: nothing here holds state across a frame.
    /// </summary>
    internal static class ui_controls {
        /// <summary>Thickness of a drag handle between two areas, in logical pixels.</summary>
        internal const float splitter_thickness = 6.0F;

        /// <summary>A push button with its hover state. The caller dictates size and style.</summary>
        internal static ui_element button(string key, string text, ui_button_style style, ui_context? context,
                                          float width, float height) {
            uint text_color = style == ui_button_style.subtle ? ui_theme.text_key : ui_theme.text_bright;
            return ui_node.box(key).size(width, height)
                .label_text(text, ui_theme.body_size, text_color, centered: true)
                .painted((paint, area) => {
                    bool hot = context != null && context.is_hovered(key);
                    uint fill = style switch {
                        ui_button_style.filled => hot ? ui_theme.accent_border : ui_theme.accent,
                        _ => hot ? ui_theme.panel : 0U,
                    };
                    uint border = style switch {
                        ui_button_style.filled => ui_theme.accent_border,
                        ui_button_style.outlined => hot ? ui_theme.accent_border : ui_theme.outline,
                        _ => 0U,
                    };
                    paint.add_rect(area, 6.0F, fill, border, border == 0U ? 0.0F : 1.0F);
                });
        }

        /// <summary>
        /// A horizontal drag handle between two areas. It lights up under the pointer and while it is
        /// being dragged, so the user can tell that the divider between the preview and the sheet is a
        /// handle and not a piece of frame.
        /// </summary>
        internal static ui_element splitter(string key, ui_context? context, bool dragging) {
            return ui_node.box(key).size(float.NaN, splitter_thickness)
                .painted((paint, area) => {
                    bool hot = dragging || (context != null && context.is_hovered(key));
                    paint.add_rect(area, 0.0F, hot ? ui_theme.accent : ui_theme.panel_border, 0U, 0.0F);
                    float grip_x = area.x + area.width * 0.5F - 14.0F;
                    float grip_y = area.y + area.height * 0.5F - 1.0F;
                    paint.add_rect(new ui_rect(grip_x, grip_y, 28.0F, 2.0F), 1.0F,
                                   hot ? ui_theme.text_bright : ui_theme.outline, 0U, 0.0F);
                });
        }

        /// <summary>
        /// The vertical twin of <see cref="splitter"/>: it divides two columns, is a hit target of its own
        /// width, and lights up under the pointer and while dragged.
        /// </summary>
        internal static ui_element vertical_splitter(string key, ui_context? context, bool dragging) {
            return ui_node.box(key).size(splitter_thickness, 0.0F)
                .painted((paint, area) => {
                    bool hot = dragging || (context != null && context.is_hovered(key));
                    paint.add_rect(area, 0.0F, hot ? ui_theme.accent : ui_theme.panel_border, 0U, 0.0F);
                    float grip_x = area.x + area.width * 0.5F - 1.0F;
                    float grip_y = area.y + area.height * 0.5F - 14.0F;
                    paint.add_rect(new ui_rect(grip_x, grip_y, 2.0F, 28.0F), 1.0F,
                                   hot ? ui_theme.text_bright : ui_theme.outline, 0U, 0.0F);
                });
        }
    }
}
