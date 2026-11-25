using System;
using System.Globalization;

namespace deren.editor;

/// <summary>
/// The two tool strips the classic shell put between its title bar and the preview: the scene actions
/// (add, delete, save, undo, redo, play) with the rename field, and the viewport tools (select, move,
/// rotate, scale, world/local, focus) with the camera mode on the right. They are rebuilt every frame
/// like the rest of the screen and forward to the same host commands the classic buttons called.
/// </summary>
internal sealed unsafe partial class editor_host {
    /// <summary>Height of the action strip; the classic shell's first toolbar row.</summary>
    private const float scene_toolbar_height = 34.0F;

    /// <summary>Height of the viewport tool strip, one line of buttons narrower than the action strip.</summary>
    private const float scene_tools_height = 28.0F;

    /// <summary>The rename field beside the Rename button; the classic shell put it on the same row.</summary>
    private readonly ui_text_edit scene_name_edit = new();

    /// <summary>
    /// The action strip: what the classic toolbar offered above the preview. It is the preview area's own
    /// header, so it opens with the editor type and closes with the maximize cell.
    /// </summary>
    private ui_element build_scene_toolbar(area_node node, string path, float width) {
        var bar = ui_node.row("editor_toolbar").size(float.NaN, scene_toolbar_height).bg(ui_theme.header)
                          .spacing(4.0F)
                          .painted((paint, area) => paint.add_rect(
                              new ui_rect(area.x, area.y + area.height - 1.0F, area.width, 1.0F), 0.0F,
                              ui_theme.panel_border, 0U, 0.0F));
        float height = scene_toolbar_height - 10.0F;
        bar.add(ui_node.box("toolbar_inset").size(6.0F, 0.0F));
        bar.add(build_area_type_cell(node, path, scene_toolbar_height - 10.0F));
        bar.add(ui_node.box("toolbar_gap_type").size(6.0F, 0.0F));
        bar.add(ui_controls.button("toolbar_add", "Add", ui_button_style.outlined, next_ui, 58.0F, height));
        bar.add(ui_controls.button("toolbar_delete", "Delete", ui_button_style.outlined, next_ui, 64.0F, height));
        bar.add(ui_controls.button("toolbar_save", "Save", ui_button_style.filled, next_ui, 58.0F, height));
        bar.add(ui_node.box("toolbar_gap_a").size(8.0F, 0.0F));
        bar.add(ui_controls.button("toolbar_undo", "Undo", ui_button_style.subtle, next_ui, 56.0F, height));
        bar.add(ui_controls.button("toolbar_redo", "Redo", ui_button_style.subtle, next_ui, 56.0F, height));
        bar.add(ui_node.box("toolbar_gap_b").size(8.0F, 0.0F));
        bar.add(ui_controls.button("toolbar_play", playing?.running == true ? "Stop" : "Play",
                                   playing?.running == true ? ui_button_style.filled : ui_button_style.outlined,
                                   next_ui, 58.0F, height));
        bar.add(ui_node.spacer("toolbar_filler"));
        bar.add(ui_node.box("scene_name_field").size(200.0F, height)
                    .bg(ui_theme.window)
                    .stroke(scene_name_edit.focused ? ui_theme.accent : ui_theme.panel_border, 1.0F)
                    .pad(2.0F)
                    .painted((paint, area) => paint_scene_field(paint, area, scene_name_edit, "Node name")));
        bar.add(ui_controls.button("toolbar_rename", "Rename", ui_button_style.outlined, next_ui, 70.0F, height));
        bar.add(ui_node.spacer("toolbar_filler_right"));
        var mode = centered(ui_node.label("toolbar_mode", "SCENE EDITOR", ui_theme.small_size, ui_theme.text_dim),
                            float.NaN, scene_toolbar_height);
        bar.add(mode);
        bar.add(ui_node.box("toolbar_trailing").size(12.0F, 0.0F));
        bar.add(build_area_edit_cells(node, path, width));
        bar.add(build_area_max_cell(node, path, scene_toolbar_height - 10.0F));
        bar.add(ui_node.box("toolbar_trailing2").size(4.0F, 0.0F));
        return bar;
    }

    /// <summary>
    /// The viewport tools, matching the classic Select / Move / Rotate / Scale / World / Focus row with the
    /// camera mode on the right. The area's identity and its maximize cell sit on the action strip above.
    /// </summary>
    private ui_element build_scene_toolbar_tools() {
        var bar = ui_node.row("editor_tools").size(float.NaN, scene_tools_height).bg(ui_theme.panel)
                          .spacing(4.0F)
                          .painted((paint, area) => paint.add_rect(
                              new ui_rect(area.x, area.y + area.height - 1.0F, area.width, 1.0F), 0.0F,
                              ui_theme.panel_border, 0U, 0.0F));
        float height = scene_tools_height - 8.0F;
        bar.add(ui_node.box("tools_inset").size(4.0F, 0.0F));
        bar.add(tool_button("vp_select", "Select"));
        bar.add(tool_button("vp_move", "Move"));
        bar.add(tool_button("vp_rotate", "Rotate"));
        bar.add(tool_button("vp_scale", "Scale"));
        bar.add(ui_node.box("tools_gap").size(8.0F, 0.0F));
        bar.add(ui_controls.button("vp_space", gizmo_local ? "Local" : "World", ui_button_style.subtle, next_ui,
                                  64.0F, height));
        bar.add(ui_controls.button("vp_focus", "Focus (F)", ui_button_style.subtle, next_ui, 76.0F, height));
        bar.add(ui_node.spacer("tools_filler"));
        bar.add(centered(ui_node.label("viewport_mode", "Perspective  /  Lit", ui_theme.small_size,
                                       ui_theme.text_dim), float.NaN, scene_tools_height));
        bar.add(ui_node.box("tools_trailing").size(8.0F, 0.0F));
        return bar;
    }

    private ui_element tool_button(string key, string text) {
        return ui_controls.button(key, text, active_tool == key ? ui_button_style.filled : ui_button_style.subtle,
                                  next_ui, 60.0F, scene_tools_height - 8.0F);
    }

    /// <summary>
    /// Routes a press on either strip. Returns true when the key belonged to a strip, so the caller stops
    /// before the outliner and the inspector try the same key.
    /// </summary>
    private bool press_scene_toolbar(string key, float x) {
        switch (key) {
            case "toolbar_add": {
                // The classic Add button added the asset picked in the browser, so the same rule applies here.
                string? asset = scene_picked_asset is { Length: > 0 } picked ? picked
                    : assets.Length != 0 ? assets[0] : null;
                if (asset == null) {
                    notice = "There is no asset to add";
                } else if (add_node(asset)) {
                    notice = $"Added '{asset}'";
                }
                return true;
            }
            case "toolbar_delete":
                delete_selected();
                notice = "Deleted";
                return true;
            case "toolbar_save":
                notice = save_document() ? "Scene saved" : notice;
                return true;
            case "toolbar_undo":
                if (undo_history?.try_undo() == true) { notice = "Undo"; }
                return true;
            case "toolbar_redo":
                if (undo_history?.try_redo() == true) { notice = "Redo"; }
                return true;
            case "toolbar_play":
                toggle_scene_play();
                return true;
            case "toolbar_rename":
                apply_rename(scene_name_edit.text);
                return true;
            case "scene_name_field": {
                // The name field behaves like the inspector's: focus it and place the caret where pressed.
                apply_scene_edit();
                scene_name_edit.focused = true;
                float field_x = next_ui?.last_tree?.find("scene_name_field")?.rect.x ?? x;
                scene_name_edit.move_to(scene_name_edit.caret_at(x - field_x - 2.0F, ui_theme.small_size),
                                        false);
                return true;
            }
            case "vp_select":
            case "vp_move":
            case "vp_rotate":
            case "vp_scale":
                set_active_tool(key);
                return true;
            case "vp_space":
                gizmo_local = !gizmo_local;
                notice = gizmo_local ? "Local space" : "World space";
                return true;
            case "vp_focus":
                focus_selection_requested = true;
                return true;
            default:
                return false;
        }
    }

    /// <summary>Seeds the rename field with the primary node's name whenever the selection changes.</summary>
    private void sync_scene_name_field() {
        if (scene_name_edit.focused) { return; }
        var node = document == null ? null : document.find(selection_state?.primary ?? Guid.Empty);
        string name = node?.name ?? string.Empty;
        if (scene_name_edit.text != name) { scene_name_edit.reset(name); }
    }
}
