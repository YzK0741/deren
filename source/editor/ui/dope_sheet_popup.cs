using System;

namespace deren.editor {
    /// <summary>
    /// The Dope Sheet's menus and dialogs. The entries are built from the current state every frame - a
    /// disabled Undo is a fact of this frame, not of a widget - and the open panel is handed to
    /// <see cref="ui_context.add_overlay"/>, so it floats over the screen instead of joining its layout.
    /// </summary>
    internal sealed unsafe partial class editor_host {
        private const float sheet_menu_height = 22.0F;

        /// <summary>Rebuilds the menu entries; the enabled flags follow the clip and the history.</summary>
        private void build_menus(motion_document? clip) {
            next_ui_menus.Clear();
            var file = new ui_menu { key = "menu_file", title = "File" };
            file.items.Add(new ui_command {
                key = "menu_file_save",
                label = "Save Motion",
                enabled = clip != null,
                run = save_next_ui_motion,
            });
            file.items.Add(new ui_command {
                key = "menu_file_reload",
                label = "Reload Clip from Disk",
                enabled = clip != null,
                run = request_reload_clip,
            });
            file.items.Add(new ui_command {
                key = "menu_file_close",
                label = "Back to the Scene Editor",
                enabled = true,
                run = leave_animation_mode_for_menus,
            });
            var edit = new ui_menu { key = "menu_edit", title = "Edit" };
            edit.items.Add(new ui_command {
                key = "menu_edit_undo",
                label = "Undo",
                enabled = undo_history?.can_undo ?? false,
                run = () => next_ui_history_step(undo: true),
            });
            edit.items.Add(new ui_command {
                key = "menu_edit_redo",
                label = "Redo",
                enabled = undo_history?.can_redo ?? false,
                run = () => next_ui_history_step(undo: false),
            });
            edit.items.Add(new ui_command {
                key = "menu_edit_cancel",
                label = "Cancel the Dragged Key",
                enabled = next_ui_gesture == sheet_gesture.key,
                run = cancel_next_ui_gesture,
            });
            var help = new ui_menu { key = "menu_help", title = "Help" };
            help.items.Add(new ui_command {
                key = "menu_help_shortcuts",
                label = "Keyboard Shortcuts",
                enabled = true,
                run = show_shortcuts_dialog,
            });
            next_ui_menus.Add(file);
            next_ui_menus.Add(edit);
            next_ui_menus.Add(help);
        }

        /// <summary>Starring: file, edit and help are held by key so the header can draw them in order.</summary>
        private ui_menu? find_menu(string key) {
            foreach (var menu in next_ui_menus) {
                if (menu.key == key) { return menu; }
            }
            return null;
        }

        /// <summary>Queues the open menu or dialog on top of the frame the caller just built.</summary>
        /// <param name="header_height">The height the menu titles sit in, so the panel hangs below them.</param>
        private void add_popup_overlays(float header_height) {
            if (next_ui == null) { return; }
            next_ui.clear_overlays();
            if (next_ui_dialog != null) {
                next_ui.add_overlay(ui_popup.build_scrim("dialog_scrim", next_ui.width, next_ui.height), 0.0F, 0.0F);
                float height = ui_popup.dialog_height(next_ui_dialog);
                float x = MathF.Max(0.0F, (next_ui.width - ui_popup.dialog_width) * 0.5F);
                float y = MathF.Max(0.0F, (next_ui.height - height) * 0.5F);
                next_ui.add_overlay(ui_popup.build_dialog(next_ui_dialog, next_ui), x, y);
                return;
            }
            if (next_ui_open_menu.Length == 0) { return; }
            if (find_menu(next_ui_open_menu) is not { } menu) {
                next_ui_open_menu = string.Empty;
                return;
            }
            // The title's rectangle comes from the previous frame, which is the one the press landed on.
            var anchor = next_ui.last_tree?.find(menu.key);
            float left = anchor?.rect.x ?? 0.0F;
            next_ui.add_overlay(ui_popup.build_menu(menu, next_ui), left, header_height);
        }

        /// <summary>
        /// Gives an open dialog or menu the first chance at a press, and says whether it took it. The
        /// dialog's scrim covers the screen, so nothing behind it can be reached while it is open.
        /// </summary>
        private bool handle_popup_press(string key) {
            if (next_ui_dialog != null) {
                if (ui_popup.find(next_ui_dialog, key) is { } button) {
                    run_dialog_command(button);
                } else {
                    next_ui_dialog = null;
                }
                return true;
            }
            if (next_ui_open_menu.Length == 0) { return false; }
            if (find_menu(next_ui_open_menu) is { } menu && ui_popup.find(next_ui_menus, key) is { } entry) {
                run_menu_command(entry);
                return true;
            }
            // Pressing another title closes this menu and then opens that one, so a menu bar needs one
            // press per menu rather than two.
            if (find_menu(key) != null) {
                next_ui_open_menu = string.Empty;
                return false;
            }
            next_ui_open_menu = string.Empty;
            return true;
        }

        /// <summary>Runs a menu entry: the menu closes first so its command can open a dialog.</summary>
        private void run_menu_command(ui_command command) {
            next_ui_open_menu = string.Empty;
            if (!command.enabled) { return; }
            command.run?.Invoke();
        }

        /// <summary>Runs a dialog button; the dialog closes before the button does its work.</summary>
        private void run_dialog_command(ui_command command) {
            next_ui_dialog = null;
            if (!command.enabled) { return; }
            command.run?.Invoke();
        }

        private void show_shortcuts_dialog() {
            var dialog = new ui_dialog {
                key = "dialog_shortcuts",
                title = "Keyboard Shortcuts",
                message = "Space: play or pause the preview.\n"
                          + "Esc: cancel a dragged key, close a menu or a dialog.\n"
                          + "In the name field: Enter applies, Esc restores, Ctrl+Z undoes.\n"
                          + "Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V: select, copy, cut and paste.\n"
                          + "Ctrl+Left/Right and Home/End: move the caret by word or to an edge.\n"
                          + "Drag the divider above the sheet to resize it; the wheel scrolls the rows.",
            };
            dialog.buttons.Add(new ui_command {
                key = "dialog_shortcuts_close",
                label = "Close",
                style = ui_button_style.filled,
            });
            next_ui_dialog = dialog;
        }

        /// <summary>
        /// Reloading throws away every edit made in memory, so it asks first when the clip is dirty. The
        /// question is a dialog of its own; the answer runs the reload or drops it.
        /// </summary>
        private void request_reload_clip() {
            if (timeline_clip == null) { return; }
            if (!timeline_clip.dirty) {
                reload_clip_from_disk();
                return;
            }
            var dialog = new ui_dialog {
                key = "dialog_reload",
                title = "Discard Unsaved Edits?",
                message = "'" + timeline_clip.clip_name +
                          "' has edits that are not on disk.\nReloading restores the file and drops the undo history.",
            };
            dialog.buttons.Add(new ui_command {
                key = "dialog_reload_cancel",
                label = "Cancel",
                style = ui_button_style.outlined,
            });
            dialog.buttons.Add(new ui_command {
                key = "dialog_reload_discard",
                label = "Discard",
                style = ui_button_style.filled,
                run = reload_clip_from_disk,
            });
            next_ui_dialog = dialog;
        }

        /// <summary>Reads the clip again, then forgets the history that pointed at the old arrays.</summary>
        private void reload_clip_from_disk() {
            undo_history?.clear();
            timeline_clip = null;
            load_timeline_clip();
            timeline_selected_channel = 0;
            next_ui_selected_key = -1;
            next_ui_gesture = sheet_gesture.none;
            next_ui_open_menu = string.Empty;
            reset_name_field(timeline_clip?.clip_name ?? string.Empty);
            notice = timeline_clip == null
                ? notice
                : string.Create(System.Globalization.CultureInfo.InvariantCulture,
                                $"Reloaded '{timeline_clip.clip_name}' from disk");
        }

        /// <summary>The File menu's way out of the isolated animation editor.</summary>
        private void leave_animation_mode_for_menus() {
            next_ui_open_menu = string.Empty;
            next_ui_dialog = null;
            exit_animation_mode();
            notice = "Left the animation editor";
        }
    }
}
