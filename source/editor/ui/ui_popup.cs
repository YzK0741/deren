using System;
using System.Collections.Generic;

namespace deren.editor {
    /// <summary>One entry of a menu or one button of a dialog: a label and the work it does.</summary>
    internal sealed class ui_command {
        internal string key = string.Empty;
        internal string label = string.Empty;
        /// <summary>A disabled entry stays visible but neither lights up nor runs.</summary>
        internal bool enabled = true;
        internal ui_button_style style = ui_button_style.outlined;
        internal Action? run;
    }

    /// <summary>A drop-down menu: a title in the header bar and the entries it shows when open.</summary>
    internal sealed class ui_menu {
        internal string key = string.Empty;
        internal string title = string.Empty;
        internal readonly List<ui_command> items = [];
    }

    /// <summary>
    /// A modal panel: a scrim that eats the pointer, a message and buttons. The screen owns which dialog
    /// is open; the popup layer only knows how to draw and hit-test one.
    /// </summary>
    internal sealed class ui_dialog {
        internal string key = string.Empty;
        internal string title = string.Empty;
        /// <summary>Body text; a newline starts a new line, because the layout has no word wrap yet.</summary>
        internal string message = string.Empty;
        internal readonly List<ui_command> buttons = [];
    }

    /// <summary>
    /// Menus and dialogs, both of which float over the screen instead of joining its layout. They are
    /// built here and handed to <see cref="ui_context.add_overlay"/>, so they paint after the screen and
    /// are hit-tested before it.
    /// </summary>
    internal static class ui_popup {
        internal const float menu_width = 220.0F;
        internal const float menu_item_height = 24.0F;
        internal const float dialog_width = 380.0F;
        internal const float dialog_button_width = 96.0F;
        internal const float dialog_button_height = 28.0F;
        private const float menu_padding = 4.0F;
        private const float dialog_padding = 14.0F;
        private const float dialog_line_height = 18.0F;

        /// <summary>A menu title in the header bar; it stays lit while its menu is the open one.</summary>
        internal static ui_element menu_title(ui_menu menu, bool open, ui_context? context, float height) {
            var (text_width, _) = ui_paint.measure(menu.title, ui_theme.body_size);
            float width = text_width + 20.0F;
            string key = menu.key;
            return ui_node.box(key).size(width, height)
                .painted((paint, area) => {
                    bool hot = open || (context != null && context.is_hovered(key));
                    uint fill = open ? ui_theme.accent : hot ? ui_theme.panel : 0U;
                    uint border = hot && !open ? ui_theme.outline : 0U;
                    paint.add_rect(area, 6.0F, fill, border, border == 0U ? 0.0F : 1.0F);
                    var (_, text_height) = ui_paint.measure(menu.title, ui_theme.body_size);
                    paint.add_text(area.x + (area.width - text_width) * 0.5F,
                                   area.y + (area.height - text_height) * 0.5F, ui_theme.body_size,
                                   open ? ui_theme.text_bright : ui_theme.text_key, menu.title);
                });
        }

        /// <summary>The dropped panel of an open menu, positioned by the caller under its title.</summary>
        internal static ui_element build_menu(ui_menu menu, ui_context? context) {
            var panel = ui_node.column(menu.key + "_panel").size(menu_width, float.NaN)
                            .bg(ui_theme.panel)
                            .stroke(ui_theme.outline, 1.0F)
                            .pad(menu_padding)
                            .spacing(2.0F);
            foreach (var item in menu.items) {
                panel.add(build_item(item, menu_width - menu_padding * 2.0F, menu_item_height, context));
            }
            return panel;
        }

        /// <summary>How tall a dialog will be, so the caller can center it before it is built.</summary>
        internal static float dialog_height(ui_dialog dialog) {
            int lines = dialog.message.Length == 0 ? 0 : count_lines(dialog.message);
            return dialog_padding * 2.0F + ui_theme.title_size + 3.0F + 8.0F +
                   lines * dialog_line_height + 8.0F + dialog_button_height;
        }

        /// <summary>The scrim that swallows the pointer behind a dialog; it covers the whole frame.</summary>
        internal static ui_element build_scrim(string key, float width, float height) =>
            ui_node.box(key).size(width, height).bg(ui_theme.rgba(0, 0, 0, 200));

        /// <summary>The dialog panel: a title, the message lines and the buttons, right-aligned.</summary>
        internal static ui_element build_dialog(ui_dialog dialog, ui_context? context) {
            var panel = ui_node.column(dialog.key).size(dialog_width, dialog_height(dialog))
                            .bg(ui_theme.panel)
                            .stroke(ui_theme.outline, 1.0F)
                            .pad(dialog_padding)
                            .spacing(8.0F);
            panel.add(ui_node.label(dialog.key + "_title", dialog.title, ui_theme.title_size, ui_theme.text_bright));
            if (dialog.message.Length > 0) {
                foreach (var line in dialog.message.Split('\n')) {
                    panel.add(ui_node.label(dialog.key + "_line", line, ui_theme.body_size, ui_theme.text_dim));
                }
            }
            var buttons = ui_node.row(dialog.key + "_buttons").size(float.NaN, dialog_button_height).spacing(8.0F);
            buttons.add(ui_node.spacer());
            foreach (var button in dialog.buttons) {
                buttons.add(ui_controls.button(button.key, button.label, button.style, context, dialog_button_width,
                                               dialog_button_height));
            }
            panel.add(buttons);
            return panel;
        }

        /// <summary>The entry of an open menu that a key belongs to, or null when the key is not one.</summary>
        internal static ui_command? find(IReadOnlyList<ui_menu> menus, string key) {
            foreach (var menu in menus) {
                foreach (var item in menu.items) {
                    if (item.key == key) { return item; }
                }
            }
            return null;
        }

        /// <summary>The button of an open dialog that a key belongs to, or null when the key is not one.</summary>
        internal static ui_command? find(ui_dialog? dialog, string key) {
            if (dialog == null) { return null; }
            foreach (var button in dialog.buttons) {
                if (button.key == key) { return button; }
            }
            return null;
        }

        internal static int count_lines(string text) {
            int lines = 1;
            foreach (char value in text) {
                if (value == '\n') { ++lines; }
            }
            return lines;
        }

        private static ui_element build_item(ui_command item, float width, float height, ui_context? context) {
            string key = item.key;
            bool enabled = item.enabled;
            string label = item.label;
            return ui_node.box(key).size(width, height)
                .painted((paint, area) => {
                    if (enabled && context != null && context.is_hovered(key)) {
                        paint.add_rect(area, 4.0F, ui_theme.row_hover, 0U, 0.0F);
                    }
                    var (_, text_height) = ui_paint.measure(label, ui_theme.body_size);
                    paint.add_text(area.x + 8.0F, area.y + (area.height - text_height) * 0.5F, ui_theme.body_size,
                                   enabled ? ui_theme.text_bright : ui_theme.text_dim, label);
                });
        }
    }
}
