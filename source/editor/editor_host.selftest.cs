using System;
using System.Collections.Generic;
using System.Globalization;
using System.Diagnostics;
using System.IO;
using System.Numerics;
using System.Runtime.InteropServices;
using deren;
using deren.scripting;

namespace deren.editor {
    /// <summary>
    /// The editor's self-test surface, split out of editor_host.cs so the production half keeps a
    /// readable size. It is a partial class because every check drives the same document, selection and
    /// runtime the production half owns - the tests are part of the editor, not a separate harness.
    /// </summary>
    internal sealed unsafe partial class editor_host {
        private script_status run_selftest() {
            if (document == null) {
                return selftest_failure("there is no document");
            }
            // The temp file lives beside the project assembly, not in the system temp directory: a
            // sandboxed run may deny writes there while the project directory is always writable
            // (the editor itself saves scenes under scenes/).
            var directory = Path.Combine(AppContext.BaseDirectory, "captures");
            Directory.CreateDirectory(directory);
            var path = Path.Combine(directory, $"editor-selftest-{Guid.NewGuid():N}.scene.toml");
            var selftest_document = document;
            var original_path = document.path;
            try {
                if (!document_io.try_save(document, path, out var save_error)) {
                    return selftest_failure($"save to a temp path failed: {save_error}");
                }
                host_logger.write($"Editor selftest: saved {count_nodes(document.roots)} node(s)");
                if (!document_io.try_load(path, out var reloaded, out var load_error) || reloaded == null) {
                    return selftest_failure($"reload failed: {load_error}");
                }
                host_logger.write("Editor selftest: reloaded the document");
                if (!same_document(document, reloaded, out var mismatch)) {
                    return selftest_failure($"reload mismatch: {mismatch}");
                }
                host_logger.write("Editor selftest: uid/name/transform of every node match");

                // THE WRITER'S HEADER IS THE VERSION CONTRACT the reader enforces: a file carrying keys
                // this writer emits (the per-node `script`) must not still claim the old version, or an
                // old build accepts it and drops the key on its next save.
                if (!document_io.try_serialize(document, out var header, out _) ||
                    !header.Contains("version = 2", StringComparison.Ordinal)) {
                    return selftest_failure("the writer still claims document version 1");
                }

                // THE VERSION RULE: version 1 documents predate the per-node `script` key and must keep
                // loading, so upgrading the editor does not refuse an old project; a version this writer
                // does not speak is refused instead of being parsed and truncated on the next save.
                var legacy_path = Path.Combine(directory, $"editor-selftest-legacy-{Guid.NewGuid():N}.scene.toml");
                File.WriteAllText(legacy_path,
                    "[document]\n" +
                    "    version = 1\n" +
                    "    name = \"legacy\"\n" +
                    "\n[[node]]\n" +
                    "    uid = \"" + Guid.NewGuid().ToString("D") + "\"\n" +
                    "    name = \"legacy-root\"\n" +
                    "    visible = true\n" +
                    "    flags = 0\n" +
                    "    position = [0, 0, 0]\n" +
                    "    rotation = [0, 0, 0]\n" +
                    "    scale = [1, 1, 1]\n");
                if (!document_io.try_load(legacy_path, out var legacy, out var legacy_error) || legacy == null) {
                    return selftest_failure($"a version 1 document no longer loads: {legacy_error}");
                }
                if (legacy.roots.Count != 1 || legacy.roots[0].script != null) {
                    return selftest_failure("a version 1 document did not load as written");
                }
                File.WriteAllText(legacy_path, "[document]\n    version = 3\n    name = \"future\"\n");
                if (document_io.try_load(legacy_path, out _, out _)) {
                    return selftest_failure("a document from a future version was accepted");
                }
                File.Delete(legacy_path);
                host_logger.write("Editor selftest: a version 1 document still loads and a future version is refused");

                var root = document.roots.Count != 0 ? document.roots[0] : null;
                if (root == null || selection_state == null) {
                    return selftest_failure("the document has no root to edit");
                }
                selection_state.select(root.uid);
                if (!apply_rename("selftest-root") || root.name != "selftest-root") {
                    return selftest_failure("rename through the button path failed");
                }
                host_logger.write("Editor selftest: renamed the root through the button path");

                // THE RUNTIME CAPS A NAME IN UTF-8 BYTES, not in characters, so the editor has to count
                // bytes too: a name kept only in the document is silently lost by the next push.
                if (apply_rename(new string('x', 300)) || root.name != "selftest-root") {
                    return selftest_failure("a rename past the 256-byte name cap was accepted");
                }
                // 200 characters of a two-byte code point are 400 bytes: a character count would pass.
                if (apply_rename(new string((char)0xE9, 200)) || root.name != "selftest-root") {
                    return selftest_failure("a name over the byte cap passed on a character count");
                }
                // Exactly at the cap is allowed; one byte less in a multi-byte tail would be too few.
                var cap_name = new string('y', 254) + "\u00e9";
                if (scene_sync.name_utf8_byte_count(cap_name) != scene_sync.max_name_utf8_bytes) {
                    return selftest_failure("the 256-byte name fixture is not 256 bytes");
                }
                if (!apply_rename(cap_name) || root.name != cap_name) {
                    return selftest_failure("a name at the 256-byte cap was refused");
                }
                if (apply_detail_change(detail_field.name, new string('z', 300)) || root.name != cap_name) {
                    return selftest_failure("the details name field bypassed the 256-byte cap");
                }
                if (!apply_rename("selftest-root") || root.name != "selftest-root") {
                    return selftest_failure("the root could not be renamed back after the cap checks");
                }
                notice = string.Empty;
                host_logger.write("Editor selftest: the 256-byte name cap is enforced before the runtime push");

                // `--name=value` SPLITS AT THE FIRST '=' so a value may itself contain '=' (a scene
                // path with a query, for example); splitting at the last one would break the switch.
                split_argument("--scene=levels=/a.toml", out var argument_name, out var argument_value);
                if (argument_name != "--scene" || argument_value != "levels=/a.toml") {
                    return selftest_failure($"--scene=value split into '{argument_name}' and '{argument_value}'");
                }
                split_argument("--editor-selftest", out argument_name, out argument_value);
                if (argument_name != "--editor-selftest" || argument_value != null) {
                    return selftest_failure("a bare flag was parsed as a value");
                }

                root.visible = false;
                root.flags = 5;
                if (!push_runtime(root)) {
                    return selftest_failure("could not prepare hidden node with custom flags");
                }
                selection_state.select(root.uid);
                if (!delete_selected() || document.find(root.uid) != null) {
                    return selftest_failure("delete did not remove the root subtree");
                }
                host_logger.write("Editor selftest: deleted the root subtree");

                if (undo_history == null || !undo_history.try_undo()) {
                    return selftest_failure("undo failed");
                }
                if (document.find(root.uid) == null || root.name != "selftest-root") {
                    return selftest_failure("undo did not restore the renamed root");
                }
                if (tree == null || !tree.try_get_flags(root.runtime, out var restored_flags) || restored_flags != 5) {
                    return selftest_failure("undo did not restore runtime flags");
                }
                root.visible = true;
                root.flags = 0;
                if (!push_runtime(root)) { return selftest_failure("could not restore test node properties"); }
                host_logger.write("Editor selftest: undo restored the root and its runtime flags");

                selection_state.select(root.uid);
                if (!apply_detail_change(detail_field.position_x, "2.5") || root.local.position.x != 2.5F) {
                    return selftest_failure("the details panel did not edit the transform");
                }
                if (tree == null || !tree.try_get_transform(root.runtime, out var runtime_transform) ||
                    runtime_transform.position.x != 2.5F) {
                    return selftest_failure("the details edit did not reach the runtime transform");
                }
                host_logger.write("Editor selftest: details edit reached the runtime node");

                if (undo_history == null || !undo_history.try_undo() || root.local.position.x != 0.0F) {
                    return selftest_failure("undo did not restore the transform");
                }
                host_logger.write("Editor selftest: undo restored the transform");

                // The panels below are driven here the same way a click would drive them (the editor
                // self-test runs in-process, before the window is interactive).
                refresh_panels();
                var visible_outliner = outliner;
                if (visible_outliner == null || visible_outliner.visible_rows != 1) {
                    return selftest_failure($"the outliner shows {visible_outliner?.visible_rows ?? -1} row(s) for 1 node");
                }
                outliner_filter = "missing-node";
                refresh_panels();
                if (visible_outliner.visible_rows != 0) {
                    return selftest_failure("the outliner filter did not hide every row");
                }
                outliner_filter = "selftest-root";
                refresh_panels();
                if (visible_outliner.visible_rows != 1) {
                    return selftest_failure("the outliner filter did not keep the matching row");
                }
                outliner_filter = string.Empty;
                refresh_panels();
                host_logger.write("Editor selftest: outliner filter");

                if (!toggle_row_visibility("row_0_vis") || root.visible) {
                    return selftest_failure("the outliner visibility glyph did not hide the node");
                }
                if (!toggle_row_visibility("row_0_vis") || !root.visible) {
                    return selftest_failure("the outliner visibility glyph did not restore the node");
                }
                host_logger.write("Editor selftest: outliner visibility");

                var expected_asset = assets.Length != 0 ? assets[0] : null;
                if (expected_asset != null) {
                    if (!select_asset("asset_0") || browser?.selected_asset != expected_asset) {
                        return selftest_failure("the content browser did not select the first asset");
                    }
                    if (!select_asset("asset_0_name") || browser?.selected_asset != expected_asset) {
                        return selftest_failure("the content browser did not select the asset from its label");
                    }
                    browser?.clear_selection();
                    refresh_browser();
                    host_logger.write($"Editor selftest: content browser selected '{expected_asset}'");
                } else {
                    host_logger.write("Editor selftest: content browser has no assets to select");
                }

                set_active_tool("vp_move");
                if (active_tool != "vp_move") {
                    return selftest_failure("the viewport tool did not change");
                }
                set_active_tool(tool_select_id);
                host_logger.write("Editor selftest: viewport tool mode");

                toggle_menu("menu_help_items");
                if (open_menu != "menu_help_items") {
                    return selftest_failure("the help menu did not open");
                }
                close_menus();
                if (open_menu.Length != 0) {
                    return selftest_failure("the menu did not close");
                }
                host_logger.write("Editor selftest: menu bar");

                set_dock_tab("tab_log");
                if (active_dock_tab != "tab_log") {
                    return selftest_failure("the output log tab did not activate");
                }
                set_dock_tab("tab_content");
                if (active_dock_tab != "tab_content") {
                    return selftest_failure("the content browser tab did not reactivate");
                }
                host_logger.write("Editor selftest: dock tabs");

                var unsaved_document = document;
                var undo_label = undo_history?.next_undo_label;
                if (new_scene() || !pending_new_scene || !ReferenceEquals(document, unsaved_document)) {
                    return selftest_failure("New Scene discarded unsaved work without confirmation");
                }
                if (!resolve_unsaved("unsaved_cancel") || pending_new_scene ||
                    !ReferenceEquals(document, unsaved_document) || undo_history?.next_undo_label != undo_label) {
                    return selftest_failure("Cancel did not preserve the document and undo history");
                }
                document.path = Path.Combine(path, "blocked.scene.toml");
                new_scene();
                if (resolve_unsaved("unsaved_save") || !pending_new_scene ||
                    !ReferenceEquals(document, unsaved_document)) {
                    return selftest_failure("a failed save discarded the document");
                }
                document.path = path;
                if (!resolve_unsaved("unsaved_save") || pending_new_scene ||
                    ReferenceEquals(document, unsaved_document) || undo_history?.can_undo == true ||
                    !document_io.try_load(path, out var saved_before_new, out _) || saved_before_new == null ||
                    !same_document(unsaved_document, saved_before_new, out _)) {
                    return selftest_failure("Save then New Scene did not preserve the old document on disk");
                }
                var discarded_document = document;
                new_scene();
                if (!pending_new_scene || !resolve_unsaved("unsaved_discard") || pending_new_scene ||
                    ReferenceEquals(document, discarded_document)) {
                    return selftest_failure("Discard did not create a new scene after confirmation");
                }
                host_logger.write("Editor selftest: unsaved confirmation save/failure/discard/cancel");

                // The caption close button must ask the same question: a dirty document gets the prompt,
                // and cancelling it leaves the document open. The window itself is only asked to close
                // after an explicit save/discard, which is why this test cancels.
                var close_document = document;
                var close_undo_label = undo_history?.next_undo_label;
                document.mark_dirty();
                close_window();
                if (!pending_close || !ReferenceEquals(document, close_document)) {
                    return selftest_failure("the close button skipped the unsaved prompt");
                }
                if (!resolve_unsaved("unsaved_cancel") || pending_close ||
                    !ReferenceEquals(document, close_document) || undo_history?.next_undo_label != close_undo_label) {
                    return selftest_failure("cancelling the close prompt did not keep the document");
                }
                host_logger.write("Editor selftest: close confirmation");
                // A crashed editor cannot run the cooperative stop that removes its play/thumbnail
                // workspaces, and the startup sweep is what takes them away on the next launch.
                foreach (var transient in new[] { "play-sessions", "thumbnail-jobs" }) {
                    var stale = Path.Combine(managed_bootstrap.project_directory, transient, "selftest-" + Guid.NewGuid().ToString("N"));
                    Directory.CreateDirectory(stale);
                    File.WriteAllText(Path.Combine(stale, "left-behind.txt"), "a crashed session left this behind");
                    play_session.sweep_stale_workspaces(managed_bootstrap.project_directory);
                    if (Directory.Exists(stale)) {
                        return selftest_failure($"the startup sweep left a stale '{transient}' workspace");
                    }
                }
                host_logger.write("Editor selftest: stale play workspaces swept");
                selection_state.select(document.roots[0].uid);
                refresh_panels(force_details: true);
                host_logger.write("Editor selftest: passed");
                return script_status.success;
            } finally {
                // Only the document the test started with keeps its original path. A phase that creates
                // a new scene replaces the document, and writing the old path onto the replacement would
                // claim the new scene was saved to the old file.
                if (ReferenceEquals(document, selftest_document)) {
                    selftest_document.path = original_path;
                }
                try {
                    File.Delete(path);
                } catch (Exception) {
                    // The temp file is best-effort: a leftover must not hide the selftest result.
                }
            }
        }

        // Run after initialize so startup-only binders cannot accidentally make this test pass.
        private script_status run_runtime_asset_selftest() {
            if (document == null || selection_state == null || tree == null || undo_history == null) {
                return selftest_failure("runtime asset test has no scene");
            }
            if (asset_selftest_phase == 1) {
                if (Array.IndexOf(assets, runtime_selftest_asset) < 0) {
                    host_logger.write($"Editor selftest: runtime asset test skipped (no {runtime_selftest_asset} fixture)");
                    asset_selftest_phase = 0;
                    panel_selftest_phase = 1;
                    return script_status.success;
                }
                browser_filter = runtime_selftest_asset;
                refresh_browser();
                var count = count_nodes(document.roots);
                var time = Stopwatch.GetTimestamp();
                if (!handle_asset_click("asset_0", time) || count_nodes(document.roots) != count) {
                    return selftest_failure("single asset click created a node");
                }
                handle_asset_click("asset_0_name", time + Stopwatch.Frequency / 10);
                var added = document.find(selection_state.primary);
                if (count_nodes(document.roots) != count + 1 || added?.asset != runtime_selftest_asset || !added.runtime.is_valid) {
                    return selftest_failure("double click did not load and select the asset after startup");
                }
                if (runtime_selftest_asset == "laevatain") {
                    // THE DOCUMENT KEEPS THE ABSENCE: binding applies the asset's declared default to
                    // the runtime (see scene_sync.try_bind_asset) without writing it back into the
                    // node, so the check belongs on the asset side and on the document staying null.
                    if (added.pipeline != null) {
                        return selftest_failure("Laevatain's asset default was written into the document");
                    }
                    if (!asset_manager.try_load(runtime_selftest_asset, out var laevatain)) {
                        return selftest_failure("Laevatain could not be loaded for its default check");
                    }
                    try {
                        if (!asset_manager.try_get_default_pipeline(laevatain, out var declared) || declared != "game_toon") {
                            return selftest_failure("Laevatain did not declare its game_toon default");
                        }
                    } finally {
                        asset_manager.release(laevatain);
                    }
                }
                selftest_asset_uid = added.uid;
                host_logger.write($"Editor selftest: runtime double click loaded and selected {runtime_selftest_asset}");
            } else if (asset_selftest_phase == 2) {
                update_viewport_aspect();
                if (!focus_selection() || !orbit_camera.try_get(out var framed) ||
                    !tree.try_get_bounds(document.find(selftest_asset_uid)!.runtime, out var center, out var radius) ||
                    radius <= 0 || framed.target.y != center.y || framed.distance <= radius) {
                    return selftest_failure("viewport focus did not frame the asset bounds");
                }
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_down, "viewport_image", "", 10, 10, 1), viewport_height);
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_move, "details", "", 30, 20), viewport_height);
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_cancel, "", ""), viewport_height);
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_move, "viewport", "", 60, 40), viewport_height);
                if (!orbit_camera.try_get(out var orbited) || MathF.Abs(orbited.yaw_degrees - framed.yaw_degrees - 5.6f) > 0.01f) {
                    return selftest_failure("viewport orbit or drag cancellation failed");
                }
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_down, "viewport", "", 10, 10, 2), viewport_height);
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_move, "viewport", "", 20, 20), viewport_height);
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_up, "viewport", "", 20, 20, 2), viewport_height);
                viewport_navigation.handle_pointer(new(rml_event_type.pointer_wheel, "viewport", "", 1), viewport_height);
                if (!orbit_camera.try_get(out var moved) || moved.target.y <= orbited.target.y || moved.distance >= orbited.distance) {
                    return selftest_failure("viewport pan or zoom failed");
                }
                focus_selection();
                host_logger.write("Editor selftest: viewport focus, orbit, pan, zoom and cancel passed");
                if (ui == null || !ui.try_get_element_rect("viewport", out var viewport)) {
                    return selftest_failure("viewport layout was unavailable for picking");
                }
                float x = viewport.x + viewport.width * 0.5f;
                float y = viewport.y + viewport.height * 0.5f;
                handle_viewport_selection(new(rml_event_type.pointer_down, "viewport_image", "", x, y));
                handle_viewport_selection(new(rml_event_type.pointer_move, "viewport_image", "", x + 12, y));
                handle_viewport_selection(new(rml_event_type.pointer_up, "viewport_image", "", x, y));
                if (selection_state?.primary != selftest_asset_uid) {
                    return selftest_failure("a viewport drag changed the selection");
                }
                float empty_x = viewport.x + 2, empty_y = viewport.y + 2;
                handle_viewport_selection(new(rml_event_type.pointer_down, "viewport_image", "", empty_x, empty_y));
                handle_viewport_selection(new(rml_event_type.pointer_up, "viewport_image", "", empty_x, empty_y));
                if (selection_state?.primary != Guid.Empty) {
                    return selftest_failure("clicking empty viewport did not clear selection");
                }
                handle_viewport_selection(new(rml_event_type.pointer_down, "viewport_image", "", x, y));
                handle_viewport_selection(new(rml_event_type.pointer_up, "viewport_image", "", x, y));
                if (selection_state?.primary != selftest_asset_uid) {
                    return selftest_failure("clicking the framed asset did not select it");
                }
                var picked_runtime = document.find(selftest_asset_uid)!.runtime;
                if (!tree.try_set_visible(picked_runtime, false)) {
                    return selftest_failure("could not hide the pickable asset");
                }
                handle_viewport_selection(new(rml_event_type.pointer_down, "viewport_image", "", x, y));
                handle_viewport_selection(new(rml_event_type.pointer_up, "viewport_image", "", x, y));
                if (selection_state?.primary != Guid.Empty || !tree.try_set_visible(picked_runtime, true)) {
                    return selftest_failure("a hidden asset was picked or could not be revealed");
                }
                selection_state.select(selftest_asset_uid);
                host_logger.write("Editor selftest: viewport picking, visibility and deselection passed");
                if (!test_viewport_transform(document.find(selftest_asset_uid)!)) {
                    return selftest_failure("gizmo drag, cancel, undo or redo failed");
                }
                host_logger.write("Editor selftest: transform gizmo passed");

                if (animation_player.try_get_info(out var animation) && (animation.playing || animation.time != 0)) {
                    return selftest_failure("an imported animation started playing in the editor");
                }
                if (!undo_history.try_undo() || document.find(selftest_asset_uid) != null) {
                    return selftest_failure("undo did not remove the imported asset");
                }
                host_logger.write("Editor selftest: runtime asset undo");
            } else if (asset_selftest_phase == 3) {
                if (!undo_history.try_redo() || document.find(selftest_asset_uid)?.runtime.is_valid != true ||
                    selection_state.primary != selftest_asset_uid) {
                    return selftest_failure("redo did not reload and select the imported asset");
                }
                host_logger.write("Editor selftest: runtime asset redo");
            } else {
                if (animation_player.try_get_info(out var animation) && (animation.playing || animation.time != 0)) {
                    return selftest_failure("redo started animation playback in the editor");
                }
                var count = count_nodes(document.roots);
                var selected = selection_state.primary;
                var undo_label = undo_history.next_undo_label;
                if (add_node("__editor_missing_asset__") || count_nodes(document.roots) != count ||
                    selection_state.primary != selected || undo_history.next_undo_label != undo_label) {
                    return selftest_failure("failed asset load changed the scene, selection or history");
                }
                var path = Path.Combine(managed_bootstrap.project_directory, "captures", $"asset-selftest-{Guid.NewGuid():N}.scene.toml");
                var original_path = document.path;
                try {
                    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                    if (!document_io.try_save(document, path, out _) ||
                        !document_io.try_load(path, out var saved, out _) ||
                        saved?.find(selftest_asset_uid)?.asset != runtime_selftest_asset) {
                        return selftest_failure("imported asset did not round trip through the scene file");
                    }
                } finally {
                    document.path = original_path;
                    document.mark_dirty();
                    File.Delete(path);
                }
                if (runtime_selftest_asset == "laevatain" &&
                    Array.IndexOf(assets, "damaged-helmet") >= 0 && Array.IndexOf(assets, "flowers") >= 0 &&
                    !test_mixed_scene()) {
                    return selftest_failure("mixed scene drag/drop, save point or reopening failed");
                }
                browser_filter = string.Empty;
                notice = "Asset loading self-test passed";
                refresh_browser();
                refresh_panels(force_details: true);
                asset_selftest_phase = 0;
                panel_selftest_phase = 1;
                host_logger.write("Editor selftest: runtime asset rollback/save passed");
                return script_status.success;
            }
            ++asset_selftest_phase;
            return script_status.success;
        }

        private bool test_mixed_scene() {
            if (document == null || selection_state == null || undo_history == null || browser == null ||
                ui == null || !ui.try_get_element_rect("viewport", out var area)) {
                host_logger.write("Editor mixed selftest: layout unavailable");
                return false;
            }
            browser_filter = string.Empty;
            refresh_browser();
            var ids = new[] { "damaged-helmet", "flowers" };
            for (int i = 0; i < ids.Length; ++i) {
                int tile = 0;
                while (browser.try_tile_asset(tile, out var id) && id != ids[i]) { ++tile; }
                if (!browser.try_tile_asset(tile, out _)) {
                    host_logger.write("Editor mixed selftest: missing asset tile");
                    return false;
                }
                handle_asset_drag(new(rml_event_type.pointer_down, $"asset_{tile}", "", area.x, area.y + area.height + 24));
                if (!handle_asset_drag(new(rml_event_type.pointer_up, "viewport_image", "",
                        area.x + area.width * (i == 0 ? 0.25f : 0.75f), area.y + area.height * 0.55f)) ||
                    document.find(selection_state.primary) is not { } placed || placed.asset != ids[i] ||
                    !placed.runtime.is_valid || placed.local.position.Equals(default(vector_3))) {
                    host_logger.write("Editor mixed selftest: drop did not place the asset");
                    return false;
                }
            }

            var character = document.find(selftest_asset_uid)!;
            // BINDING MUST NOT WRITE THE ASSET DEFAULT INTO THE DOCUMENT: a null pipeline already
            // means "use the default", and pinning it here would move the save point and make the
            // undo of the next pipeline change land on a state the file never had.
            if (character.pipeline != null) {
                host_logger.write("Editor mixed selftest: binding wrote the asset default into the document");
                return false;
            }
            selection_state.select(character.uid);
            string? original_pipeline = character.pipeline;
            cycle_pipeline();
            if (character.pipeline == original_pipeline || !undo_history.try_undo() ||
                character.pipeline != original_pipeline) {
                host_logger.write("Editor mixed selftest: pipeline edit or undo failed");
                return false;
            }

            var path = Path.Combine(managed_bootstrap.project_directory, "captures", $"mixed-selftest-{Guid.NewGuid():N}.scene.toml");
            var original = document;
            try {
                show_scene_path(saving: true);
                scene_path_text = path;
                handle_scene_path("scene_path_accept");
                if (scene_path_open || original.dirty || original.path != path) {
                    host_logger.write("Editor mixed selftest: Save As did not advance the save point");
                    return false;
                }
                if (!toggle_selected_visibility() || !original.dirty ||
                    !undo_history.try_undo() || original.dirty ||
                    !undo_history.try_redo() || !original.dirty || !undo_history.try_undo() || original.dirty) {
                    host_logger.write("Editor mixed selftest: visibility undo/redo did not track the save point");
                    return false;
                }
                if (!delete_selected() || !original.dirty || !undo_history.try_undo() || original.dirty) {
                    host_logger.write("Editor mixed selftest: delete undo did not return to the save point");
                    return false;
                }
                selection_state.select(character.uid);
                if (!toggle_selected_visibility() || !original.dirty) {
                    host_logger.write("Editor mixed selftest: visibility edit did not mark the scene dirty");
                    return false;
                }
                show_scene_path(saving: false);
                scene_path_text = path;
                handle_scene_path("scene_path_accept");
                if (pending_open == null || !resolve_unsaved("unsaved_cancel") || !ReferenceEquals(original, document) ||
                    !original.dirty || !undo_history.try_undo() || original.dirty) {
                    host_logger.write("Editor mixed selftest: Open cancellation did not preserve the draft");
                    return false;
                }
                show_scene_path(saving: false);
                scene_path_text = path;
                handle_scene_path("scene_path_accept");
                if (scene_path_open || ReferenceEquals(original, document) || document == null || document.dirty ||
                    !same_document(original, document, out _) || undo_history.can_undo) {
                    host_logger.write("Editor mixed selftest: reopened scene did not match saved scene");
                    return false;
                }
                foreach (var node in document.roots) {
                    if (!node.runtime.is_valid) {
                        host_logger.write("Editor mixed selftest: reopened runtime node is missing");
                        return false;
                    }
                }
            } finally {
                File.Delete(path);
                if (document?.path == path) { document.path = null; }
            }
            var loaded_character = document?.find(selftest_asset_uid);
            if (loaded_character == null) {
                host_logger.write("Editor mixed selftest: reopened character missing");
                return false;
            }
            selection_state.select(loaded_character.uid);
            set_active_tool("vp_move");
            focus_selection();
            host_logger.write("Editor selftest: mixed scene drag/drop, pipeline, save point and reopening passed");
            return true;
        }

        // Window-menu visibility must restore the element's own display type (the outliner is a flex
        // column, the details a block) and hide the splitter of a hidden panel: restoring the wrong
        // one collapses the outliner rows and lays the details out sideways, and a splitter left over
        // a hidden panel still moves it. Runs over several frames because a display change only
        // reaches the element boxes on the next layout pass.
        private script_status run_panel_selftest() {
            if (ui == null) {
                return selftest_failure("panel visibility test has no document");
            }
            switch (panel_selftest_phase) {
                case 1:
                    if (!ui.try_get_element_rect("outliner_rows", out var rows) ||
                        !ui.try_get_element_rect("details_title", out var title) ||
                        !ui.try_get_element_rect("details", out var details)) {
                        return selftest_failure("panel visibility test cannot measure the layout");
                    }
                    panel_rows_height = rows.height;
                    panel_title_width = title.width;
                    panel_details_y = details.y;
                    // Go through the menu handler, not toggle_panel directly: the regression was the
                    // display string the Window menu restores, which a direct call would bypass.
                    handle_menu_click("menu_window_outliner");
                    panel_selftest_phase = 2;
                    return script_status.success;
                case 3:
                    if (!ui.try_get_element_rect("details", out var hidden) ||
                        hidden.y > panel_details_y - 100.0f) {
                        return selftest_failure("a hidden panel left its layout space reserved");
                    }
                    handle_menu_click("menu_window_outliner");
                    panel_selftest_phase = 4;
                    return script_status.success;
                case 5:
                    if (!ui.try_get_element_rect("outliner_rows", out var restored_rows) ||
                        !ui.try_get_element_rect("details", out var restored_details) ||
                        MathF.Abs(restored_rows.height - panel_rows_height) > 1.0f ||
                        MathF.Abs(restored_details.y - panel_details_y) > 1.0f) {
                        return selftest_failure("re-showing the outliner did not restore its layout");
                    }
                    handle_menu_click("menu_window_details");
                    panel_selftest_phase = 6;
                    return script_status.success;
                case 7:
                    handle_menu_click("menu_window_details");
                    panel_selftest_phase = 8;
                    return script_status.success;
                case 9:
                    if (!ui.try_get_element_rect("details_title", out var revealed) ||
                        MathF.Abs(revealed.width - panel_title_width) > 1.0f) {
                        return selftest_failure("re-showing the details did not restore its layout");
                    }
                    handle_menu_click("menu_window_sidebar_side");
                    handle_menu_click("menu_window_dock_side");
                    handle_menu_click("menu_window_panel_order");
                    panel_selftest_phase = 10;
                    return script_status.success;
                case 11:
                    if (!ui.try_get_element_rect("right_column", out var sidebar) ||
                        !ui.try_get_element_rect("viewport", out var viewport) ||
                        !ui.try_get_element_rect("bottom_dock", out var dock) ||
                        !ui.try_get_element_rect("outliner", out var outliner_area) ||
                        !ui.try_get_element_rect("details", out var details_area) ||
                        sidebar.x >= viewport.x || dock.y >= viewport.y || details_area.y >= outliner_area.y) {
                        return selftest_failure("the rearranged workspace did not move all three panels");
                    }
                    begin_split_drag(new(rml_event_type.pointer_down, "split_right", "", sidebar.x + sidebar.width, sidebar.y));
                    update_split_drag(new(rml_event_type.pointer_move, "split_right", "", sidebar.x + sidebar.width + 20, sidebar.y));
                    finish_split_drag();
                    panel_selftest_phase = 12;
                    return script_status.success;
                case 13:
                    if (!ui.try_get_element_rect("right_column", out var widened) || MathF.Abs(widened.width - 370) > 1) {
                        return selftest_failure("the left sidebar splitter did not widen to the right");
                    }
                    handle_menu_click("menu_window_reset_layout");
                    panel_selftest_phase = 14;
                    return script_status.success;
                case 15:
                    if (!ui.try_get_element_rect("right_column", out var reset) ||
                        !ui.try_get_element_rect("viewport", out var reset_viewport) ||
                        MathF.Abs(reset.width - 350) > 1 || reset.x <= reset_viewport.x) {
                        return selftest_failure("Reset Layout did not restore the right sidebar");
                    }
                    panel_selftest_phase = 0;
                    notice = "Panel self-test passed";
                    host_logger.write("Editor selftest: panel visibility, rearrangement and reset passed");
                    return script_status.success;
                default:
                    // Even phases are the frames that let a display change reach the element boxes.
                    ++panel_selftest_phase;
                    return script_status.success;
            }
        }

        private static script_status selftest_failure(string message) {
            host_logger.write($"Editor selftest: FAILED - {message}");
            return script_status.script_failure;
        }

        private static bool same_document(scene_document expected, scene_document actual, out string? mismatch) {
            var left = new List<document_node>();
            flatten(expected.roots, left);
            var right = new List<document_node>();
            flatten(actual.roots, right);
            if (left.Count != right.Count) {
                mismatch = $"{left.Count} node(s) reloaded as {right.Count}";
                return false;
            }
            for (var index = 0; index < left.Count; ++index) {
                var before = left[index];
                var after = right[index];
                if (before.uid != after.uid) {
                    mismatch = $"node {index} uid {before.uid} reloaded as {after.uid}";
                    return false;
                }
                if (before.name != after.name) {
                    mismatch = $"node '{before.name}' reloaded as '{after.name}'";
                    return false;
                }
                if (before.asset != after.asset || before.pipeline != after.pipeline || before.script != after.script ||
                    before.visible != after.visible || before.flags != after.flags) {
                    mismatch = $"node '{before.name}' properties changed on reload";
                    return false;
                }
                if (!same_transform(before.local, after.local)) {
                    mismatch = $"node '{before.name}' transform changed on reload";
                    return false;
                }
            }
            mismatch = null;
            return true;
        }

        private static void flatten(IReadOnlyList<document_node> nodes, List<document_node> into) {
            for (var index = 0; index < nodes.Count; ++index) {
                into.Add(nodes[index]);
                flatten(nodes[index].children, into);
            }
        }

        private static bool same_transform(transform_value left, transform_value right) =>
            left.position.x == right.position.x && left.position.y == right.position.y &&
            left.position.z == right.position.z && left.rotation_degrees.x == right.rotation_degrees.x &&
            left.rotation_degrees.y == right.rotation_degrees.y && left.rotation_degrees.z == right.rotation_degrees.z &&
            left.scale.x == right.scale.x && left.scale.y == right.scale.y && left.scale.z == right.scale.z;

        private static int count_nodes(IReadOnlyList<document_node> nodes) {
            var total = nodes.Count;
            for (var index = 0; index < nodes.Count; ++index) {
                if (nodes[index].children.Count != 0) {
                    total += count_nodes(nodes[index].children);
                }
            }
            return total;
        }
    }
}
