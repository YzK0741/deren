using System;
using System.Globalization;

namespace deren.editor {
    /// <summary>
    /// The workspace strip above the areas: one tab per workspace, then new, rename and close. Blender's
    /// workspace tabs sit at the top of the window for the same reason this strip does: the arrangement of
    /// areas belongs to the workspace and not to the document, so it gets its own row instead of sharing
    /// the caption with the menus.
    /// </summary>
    internal sealed unsafe partial class editor_host {
        private const float scene_workspace_height = 26.0F;
        private const float scene_workspace_tab_width = 92.0F;
        private readonly ui_text_edit scene_workspace_edit = new();
        private bool scene_workspace_renaming;

        /// <summary>
        /// The strip itself. Tabs are the same cells the dock uses, so an active tab reads the same way in
        /// both places; the rename field replaces the active tab in place, because that is the cell the
        /// name is about.
        /// </summary>
        private ui_element build_scene_workspace_strip() {
            var row = ui_node.row("workspace_strip").size(float.NaN, scene_workspace_height).bg(ui_theme.panel)
                              .spacing(0.0F);
            row.add(ui_node.box("workspace_inset").size(6.0F, 0.0F));
            for (int index = 0; index < scene_areas.workspaces.Count; ++index) {
                bool active = index == scene_areas.active_index;
                string key = string.Create(CultureInfo.InvariantCulture, $"workspace_tab_{index}");
                if (active && scene_workspace_renaming) {
                    row.add(ui_node.box("workspace_name_field")
                                .size(scene_workspace_tab_width, scene_workspace_height - 4.0F)
                                .bg(ui_theme.panel)
                                .stroke(scene_workspace_edit.focused ? ui_theme.accent : ui_theme.panel_border, 1.0F)
                                .pad(4.0F)
                                .painted((paint, area) =>
                                             paint_scene_field(paint, area, scene_workspace_edit, "workspace")));
                    continue;
                }
                var tab = ui_controls.dock_tab(key, scene_areas.workspaces[index].name, active, next_ui,
                                               scene_workspace_height);
                tab.size(scene_workspace_tab_width, scene_workspace_height);
                row.add(tab);
            }
            row.add(ui_node.box("workspace_gap").size(8.0F, 0.0F));
            row.add(ui_controls.button("workspace_new", "+", ui_button_style.subtle, next_ui, 26.0F,
                                       scene_workspace_height - 8.0F));
            row.add(ui_node.box("workspace_gap_new").size(4.0F, 0.0F));
            row.add(ui_controls.button("workspace_rename", "Rename", ui_button_style.subtle, next_ui, 62.0F,
                                       scene_workspace_height - 8.0F));
            row.add(ui_node.box("workspace_gap_rename").size(4.0F, 0.0F));
            row.add(ui_controls.button("workspace_close", "Close", ui_button_style.subtle, next_ui, 54.0F,
                                       scene_workspace_height - 8.0F));
            row.add(ui_node.spacer("workspace_filler"));
            // The strip is the one place that names the arrangement, so it also says which one is showing.
            if (!scene_workspace_renaming) {
                row.add(centered(ui_node.label("workspace_count",
                                               string.Create(CultureInfo.InvariantCulture,
                                                             $"{area_layout.leaves(scene_areas.root)} areas"),
                                               ui_theme.small_size, ui_theme.text_dim),
                                 float.NaN, scene_workspace_height));
                row.add(ui_node.box("workspace_trailing").size(10.0F, 0.0F));
            }
            return row;
        }

        /// <summary>Routes the strip: a tab switches, the buttons add, rename and close.</summary>
        private bool press_scene_workspace(string key) {
            if (scene_workspace_renaming && key != "workspace_name_field") {
                apply_scene_workspace_rename();
            }
            if (try_scene_index(key, "workspace_tab_", out var index)) {
                if (scene_areas.select_workspace(index)) {
                    scene_area_maximized = null;
                    save_scene_areas();
                    notice = "Workspace: " + scene_areas.active;
                }
                return true;
            }
            if (key == "workspace_name_field") {
                scene_workspace_edit.focused = true;
                return true;
            }
            switch (key) {
            case "workspace_new": {
                string name = scene_areas.add_workspace();
                scene_area_maximized = null;
                save_scene_areas();
                notice = "Workspace added: " + name;
                return true;
            }
            case "workspace_rename":
                begin_scene_workspace_rename();
                return true;
            case "workspace_close":
                if (!scene_areas.remove_workspace()) {
                    notice = "The last workspace stays";
                    return true;
                }
                scene_area_maximized = null;
                save_scene_areas();
                notice = "Workspace: " + scene_areas.active;
                return true;
            default:
                return false;
            }
        }

        private void begin_scene_workspace_rename() {
            scene_workspace_renaming = true;
            scene_workspace_edit.reset(scene_areas.active);
            scene_workspace_edit.focused = true;
            scene_workspace_edit.select_all();
        }

        private void apply_scene_workspace_rename() {
            if (!scene_workspace_renaming) { return; }
            scene_workspace_renaming = false;
            scene_workspace_edit.focused = false;
            string wanted = scene_workspace_edit.text;
            if (scene_areas.rename_workspace(wanted)) {
                save_scene_areas();
                notice = "Workspace: " + scene_areas.active;
            }
        }

        private void cancel_scene_workspace_rename() {
            scene_workspace_renaming = false;
            scene_workspace_edit.focused = false;
        }

        /// <summary>Typing goes to the rename field while it is open, like every other inline edit.</summary>
        private bool scene_workspace_types(uint codepoint) {
            if (!scene_workspace_renaming) { return false; }
            scene_workspace_edit.insert(codepoint);
            return true;
        }
    }
}
