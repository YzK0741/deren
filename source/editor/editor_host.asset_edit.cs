using System;
using System.IO;
using deren;
using deren.scripting;

namespace deren.editor {
    internal sealed unsafe partial class editor_host {
        private string? asset_edit_id;
        private string asset_edit_name = string.Empty;
        private string asset_edit_path = string.Empty;

        private void show_asset_edit() {
            if (browser?.selected_asset is not { Length: > 0 } id) {
                notice = "Select an asset to edit its name or folder";
                return;
            }
            if (!asset_authoring.try_open(out var authoring) ||
                authoring.library_root() is not { } root || authoring.asset_directory(id) is not { } path ||
                authoring.display_name(id) is not { } title) {
                notice = "Could not read this asset's catalogue entry";
                return;
            }
            reset_outliner_drag();
            reset_browser_interaction();
            viewport_press = null;
            viewport_navigation.cancel_drag();
            asset_edit_id = id;
            asset_edit_name = title;
            asset_edit_path = Path.GetRelativePath(root, path).Replace(Path.DirectorySeparatorChar, '/');
            ui?.set_text("asset_edit_message", $"Asset ID: {id}");
            ui?.set_value("asset_edit_name", title);
            ui?.set_value("asset_edit_path", asset_edit_path);
            ui?.try_set_element_style("asset_edit_prompt", "display", "flex");
        }

        private void handle_asset_edit(rml_event input) {
            if (input.type == rml_event_type.change) {
                if (input.id == "asset_edit_name") { asset_edit_name = input.value; }
                if (input.id == "asset_edit_path") { asset_edit_path = input.value; }
                return;
            }
            if (input.type != rml_event_type.click || asset_edit_id is not { } id) { return; }
            if (input.id == "asset_edit_save") {
                if (!asset_authoring.try_open(out var authoring)) {
                    ui?.set_text("asset_edit_message", "Could not access the asset catalogue.");
                    return;
                }
                if (!asset_relocation.try_apply(authoring, id, asset_edit_path.Trim(), asset_edit_name.Trim(), out var error)) {
                    ui?.set_text("asset_edit_message", error);
                    host_logger.write($"Editor asset edit: {error}");
                    return;
                }
                browser?.go_to_root();
                browser_filter = id;
                ui?.set_value("browser_search", id);
                refresh_asset_library();
                if (browser?.try_tile_asset(0, out var first) == true && first == id) {
                    browser.try_select_tile(0);
                    refresh_browser();
                }
                notice = $"Updated asset: {id}";
            } else if (input.id != "asset_edit_cancel") {
                return;
            }
            asset_edit_id = null;
            ui?.try_set_element_style("asset_edit_prompt", "display", "none");
        }
    }
}
