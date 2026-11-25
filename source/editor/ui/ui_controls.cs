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
        /// <summary>
        /// Thickness of a drag handle between two areas, in logical pixels. Slate's splitters are a thin
        /// handle that only announces itself under the pointer (SSplitter, in
        /// D:\UnrealEngine-5.8.2-release\Engine\Source\Runtime\Slate\Private\Widgets\Layout\SSplitter.cpp),
        /// so the handle is 4 px and carries no grip decoration of its own.
        /// </summary>
        internal const float splitter_thickness = 4.0F;

        /// <summary>The strip above a dock body: one cell per tab, the active one marked by a top rule.</summary>
        internal static ui_element dock_tab(string key, string text, bool active, ui_context? context,
                                            float height) {
            return ui_node.box(key).size(float.NaN, height)
                .painted((paint, area) => {
                    bool hot = context != null && context.is_hovered(key);
                    // FDockTabStyle paints Normal/Hovered/Foreground brushes plus a colour overlay; the
                    // foreground (active) cell is the lifted one and carries the indicator rule on top.
                    uint fill = active ? ui_theme.panel_border : hot ? ui_theme.row_hover : 0U;
                    paint.add_rect(area, 0.0F, fill, 0U, 0.0F);
                    if (active) {
                        paint.add_rect(new ui_rect(area.x, area.y, area.width, 2.0F), 0.0F,
                                       ui_theme.accent_border, 0U, 0.0F);
                    }
                })
                .label_text(text, ui_theme.body_size,
                            active ? ui_theme.text_bright : ui_theme.text_dim, centered: true)
                .pad(0.0F);
        }

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
                });
        }
    }
}
