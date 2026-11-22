using System;
using deren;
using deren.scripting;

namespace deren.editor {
    internal sealed unsafe partial class editor_host {
        private static string? requested_import_selftest;
        private bool asset_import_open;
        private string import_source = string.Empty;
        private string import_id = string.Empty;

        private void show_asset_import() {
            if (!asset_authoring.try_open(out var authoring) || authoring.library_root() is not { } root) {
                notice = "Configure an asset library for this project before importing.";
                return;
            }
            reset_outliner_drag();
            asset_drag = null;
            viewport_press = null;
            viewport_navigation.cancel_drag();
            import_source = string.Empty;
            import_id = string.Empty;
            ui?.set_value("asset_import_source", import_source);
            ui?.set_value("asset_import_id", import_id);
            ui?.set_text("asset_import_message", $"Destination: {root}");
            ui?.try_set_element_style("asset_import_prompt", "display", "flex");
            asset_import_open = true;
        }

        private void hide_asset_import() {
            asset_import_open = false;
            ui?.try_set_element_style("asset_import_prompt", "display", "none");
        }

        private void handle_asset_import(rml_event input) {
            if (input.type == rml_event_type.change) {
                if (input.id == "asset_import_source") { import_source = input.value; }
                if (input.id == "asset_import_id") { import_id = input.value; }
                return;
            }
            if (input.type != rml_event_type.click) { return; }
            switch (input.id) {
                case "asset_import_cancel":
                    hide_asset_import();
                    break;
                case "asset_import_browse":
                    if (asset_authoring.try_open(out var authoring) && authoring.pick_model() is { } selected) {
                        import_source = selected;
                        ui?.set_value("asset_import_source", selected);
                        if (import_id.Length == 0) {
                            import_id = asset_importer.suggested_id(selected);
                            ui?.set_value("asset_import_id", import_id);
                        }
                    }
                    break;
                case "asset_import_accept":
                    var source = import_source.Trim().Trim('"');
                    if (source.Length == 0) {
                        ui?.set_text("asset_import_message", "Choose a GLB or glTF model.");
                        return;
                    }
                    var id = import_id.Trim();
                    if (id.Length == 0) { id = asset_importer.suggested_id(source); }
                    if (try_import_model(source, id, out var error)) {
                        hide_asset_import();
                    } else {
                        ui?.set_text("asset_import_message", error);
                    }
                    break;
            }
        }

        private bool try_import_model(string source, string id, out string error) {
            error = "The project has no configured asset library.";
            if (!asset_authoring.try_open(out var authoring) || authoring.library_root() is not { } library) {
                return false;
            }
            if (!asset_importer.try_import(authoring, library, source, id, out error)) {
                host_logger.write($"Editor import: {error}");
                return false;
            }
            assets = asset_manager.list();
            scripts_panel?.invalidate();
            browser_filter = id;
            ui?.set_value("browser_search", browser_filter);
            refresh_browser();
            if (browser?.try_tile_asset(0, out var first) == true && first == id) {
                browser.try_select_tile(0);
                refresh_browser();
            }
            notice = $"Imported asset: {id}";
            return true;
        }

        private void refresh_asset_library() {
            if (!asset_authoring.try_open(out var authoring) || authoring.refresh() != script_status.success) {
                notice = "Could not refresh the asset library.";
                return;
            }
            var updated = asset_manager.list(out var status);
            if (status != script_status.success) {
                notice = $"Could not list the asset library ({status}).";
                return;
            }
            assets = updated;
            scripts_panel?.invalidate();
            invalidate_environment_catalog();
            if (browser != null && Array.IndexOf(updated, browser.selected_asset) < 0) {
                browser.clear_selection();
            }
            refresh_browser();
            notice = "Asset library refreshed";
        }
    }
}
