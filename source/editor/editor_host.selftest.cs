using System;
using System.Linq;
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
                // this writer emits (the per-node `script`, the scene `[environment]` lanes) must not
                // still claim the old version, or an old build accepts it and drops the keys on its
                // next save.
                if (!document_io.try_serialize(document, out var header, out _) ||
                    !header.Contains("version = 13", StringComparison.Ordinal)) {
                    return selftest_failure("the writer did not publish the animation-placement document version");
                }

                // THE SCENE ENVIRONMENT travels with the document: a lane and a node's lane index must
                // come back through a save and a reload, and an absent lane has to stay absent rather
                // than turning into an empty one the next save would write.
                var environment_path = Path.Combine(directory, $"editor-selftest-environment-{Guid.NewGuid():N}.scene.toml");
                var saved_slots = (string?[])document.environment_slots.Clone();
                var saved_environment = document.roots.Count != 0 ? document.roots[0].environment : 0;
                try {
                    document.environment_slots[0] = "env/lane-1.hdr";
                    // A lane the document carries has to hold a real map: a node pointing at an empty lane
                    // samples a descriptor nobody sets, so the reader refuses that combination (checked
                    // below), which is why the round trip uses a second map rather than an empty string.
                    document.environment_slots[1] = "env/lane-2.hdr";
                    if (document.roots.Count != 0) {
                        document.roots[0].environment = 2;
                    }
                    if (!document_io.try_save(document, environment_path, out var environment_error)) {
                        return selftest_failure($"the environment document did not save: {environment_error}");
                    }
                    if (!File.ReadAllText(environment_path).Contains("[environment]", StringComparison.Ordinal)) {
                        return selftest_failure("the writer dropped the scene environment section");
                    }
                    if (!document_io.try_load(environment_path, out var environment_document, out var environment_load_error) ||
                        environment_document == null) {
                        return selftest_failure($"the environment document did not reload: {environment_load_error}");
                    }
                    if (!string.Equals(environment_document.environment_slots[0], "env/lane-1.hdr", StringComparison.Ordinal) ||
                        !string.Equals(environment_document.environment_slots[1], "env/lane-2.hdr", StringComparison.Ordinal) ||
                        environment_document.environment_slots[2] != null) {
                        return selftest_failure("the scene environment lanes did not survive a save and reload");
                    }
                    var environment_root = environment_document.roots.Count != 0 ? environment_document.roots[0] : null;
                    if (environment_root == null || environment_root.environment != 2) {
                        return selftest_failure("the per-node environment lane did not survive a save and reload");
                    }
                    host_logger.write("Editor selftest: the scene environment and a node lane survive a save");
                } finally {
                    for (var lane = 0; lane < document.environment_slots.Length; ++lane) {
                        document.environment_slots[lane] = saved_slots[lane];
                    }
                    if (document.roots.Count != 0) {
                        document.roots[0].environment = saved_environment;
                    }
                    if (File.Exists(environment_path)) {
                        File.Delete(environment_path);
                    }
                }

                // THE PANEL PATH: a lane edit has to reach the document, mark it dirty and be undoable,
                // through the same entry point the change handler uses. The map is an absolute path to a
                // real HDR so the host accepts it.
                var environment_probe = Path.GetFullPath(Path.Combine(
                    managed_bootstrap.project_directory, "..", "..", "..", "asset", "environments", "neutral.hdr"));
                var lane_before = document.environment_slots[0];
                var revision_before = document.revision;
                apply_environment_text(1, environment_probe);
                if (!string.Equals(document.environment_slots[0], environment_probe, StringComparison.Ordinal)) {
                    return selftest_failure("an environment lane edit did not reach the document");
                }
                if (document.revision <= revision_before) {
                    return selftest_failure("an environment lane edit did not mark the document dirty");
                }
                if (undo_history == null || !undo_history.try_undo() ||
                    !string.Equals(document.environment_slots[0], lane_before, StringComparison.Ordinal)) {
                    return selftest_failure("undoing an environment lane edit did not restore the document");
                }
                // THE PER-OBJECT DROPDOWN: choosing a map for a node allocates the lane that carries it,
                // or the first free one, and a second object asking for the same map shares the lane
                // instead of burning another slot. Clearing the lane sends both objects back to the
                // scene-wide map, because a node left on an empty lane samples an unset descriptor.
                var probe_node = document.roots.Count != 0 ? document.roots[0] : null;
                if (probe_node == null) {
                    return selftest_failure("the selftest scene has no node to configure");
                }
                var node_lane_before = probe_node.environment;
                var lane_text_before = document.environment_slots[0];
                try {
                    assign_node_environment(probe_node, environment_probe);
                    if (probe_node.environment != 1 ||
                        !string.Equals(document.environment_slots[0], environment_probe, StringComparison.Ordinal)) {
                        return selftest_failure("the per-object dropdown did not allocate the first free lane");
                    }
                    // ONE PICK IS ONE UNDO STEP. The lane text and the node's lane move together, so a single
                    // undo has to take both back; two steps would leave the document in the half-applied state
                    // where the lane is filled but no object samples it.
                    if (undo_history == null || !undo_history.try_undo() ||
                        !string.Equals(document.environment_slots[0], lane_text_before, StringComparison.Ordinal) ||
                        probe_node.environment != node_lane_before) {
                        return selftest_failure("undoing a per-object environment pick did not restore the lane and the node");
                    }
                    assign_node_environment(probe_node, environment_probe);
                    if (probe_node.environment != 1 || document.environment_slots[0] == null) {
                        return selftest_failure("the per-object dropdown did not re-apply after an undo");
                    }
                    assign_node_environment(probe_node, environment_probe);
                    if (probe_node.environment != 1) {
                        return selftest_failure("the per-object dropdown moved a node that already had the map");
                    }
                    clear_environment_lane(1);
                    if (document.environment_slots[0] != null || probe_node.environment != 0) {
                        return selftest_failure("clearing an environment lane left an object sampling it");
                    }
                    if (undo_history == null || !undo_history.try_undo() ||
                        !string.Equals(document.environment_slots[0], environment_probe, StringComparison.Ordinal) ||
                        probe_node.environment != 1) {
                        return selftest_failure("undoing a lane clear did not restore the map and the node");
                    }
                } finally {
                    document.environment_slots[0] = lane_text_before;
                    probe_node.environment = node_lane_before;
                    push_runtime(probe_node);
                }
                host_logger.write("Editor selftest: an environment lane edit is dirty and undoable");
                host_logger.write("Editor selftest: a per-object lane is allocated, shared and cleared");

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
                // Version 3 knows the per-node `script` but no environment: it must keep loading, with
                // the lanes left absent so the project map stays in charge.
                File.WriteAllText(legacy_path,
                    "[document]\n" +
                    "    version = 3\n" +
                    "    name = \"v3\"\n" +
                    "\n[[node]]\n" +
                    "    uid = \"" + Guid.NewGuid().ToString("D") + "\"\n" +
                    "    name = \"v3-root\"\n" +
                    "    environment = 0\n" +
                    "    visible = true\n" +
                    "    flags = 0\n" +
                    "    position = [0, 0, 0]\n" +
                    "    rotation = [0, 0, 0]\n" +
                    "    scale = [1, 1, 1]\n");
                if (!document_io.try_load(legacy_path, out var older, out var older_error) || older == null) {
                    return selftest_failure($"a version 3 document no longer loads: {older_error}");
                }
                if (older.roots.Count != 1 || older.roots[0].environment != 0 ||
                    Array.Exists(older.environment_slots, slot => slot != null)) {
                    return selftest_failure("a version 3 document did not load as written");
                }
                File.WriteAllText(legacy_path, "[document]\n    version = 14\n    name = \"future\"\n");
                if (document_io.try_load(legacy_path, out _, out _)) {
                    return selftest_failure("a document from a future version was accepted");
                }
                File.Delete(legacy_path);
                // A NODE MAY NOT SAMPLE A LANE THE DOCUMENT DOES NOT FILL. A hand-edited or older-tool file
                // could carry `environment = 2` with no `slot_2`; loading it would leave the object sampling a
                // descriptor nobody sets, which renders unlit with no error to explain it, so the reader
                // refuses the combination instead of silently downgrading it to the scene map.
                var broken_path = Path.Combine(directory, $"editor-selftest-broken-{Guid.NewGuid():N}.scene.toml");
                File.WriteAllText(broken_path,
                    "[document]\n" +
                    "    version = 4\n" +
                    "    name = \"broken\"\n" +
                    "\n[[node]]\n" +
                    "    uid = \"" + Guid.NewGuid().ToString("D") + "\"\n" +
                    "    name = \"empty-lane\"\n" +
                    "    environment = 2\n" +
                    "    visible = true\n" +
                    "    flags = 0\n" +
                    "    position = [0, 0, 0]\n" +
                    "    rotation = [0, 0, 0]\n" +
                    "    scale = [1, 1, 1]\n");
                if (document_io.try_load(broken_path, out _, out _)) {
                    return selftest_failure("a document with a node on an unfilled environment lane was accepted");
                }
                // The lane itself is fine as long as some lane the node names is filled: the same node on a
                // lane the section does fill has to load, so the refusal above is the lane check and not
                // the environment section as a whole.
                File.WriteAllText(broken_path,
                    "[document]\n" +
                    "    version = 4\n" +
                    "    name = \"filled\"\n" +
                    "\n[environment]\n" +
                    "    slot_2 = \"env/lane-2.hdr\"\n" +
                    "\n[[node]]\n" +
                    "    uid = \"" + Guid.NewGuid().ToString("D") + "\"\n" +
                    "    name = \"lane-two\"\n" +
                    "    environment = 2\n" +
                    "    visible = true\n" +
                    "    flags = 0\n" +
                    "    position = [0, 0, 0]\n" +
                    "    rotation = [0, 0, 0]\n" +
                    "    scale = [1, 1, 1]\n");
                if (!document_io.try_load(broken_path, out var filled, out var filled_error) || filled == null ||
                    filled.roots.Count != 1 || filled.roots[0].environment != 2) {
                    return selftest_failure($"a document with a filled environment lane did not load: {filled_error}");
                }
                // A VERSION BEFORE 4 HAS NO ENVIRONMENT SECTION, so carrying one is a contradiction (a v3
                // writer could not have produced it) and the reader says so rather than upgrading the file.
                File.WriteAllText(broken_path,
                    "[document]\n" +
                    "    version = 3\n" +
                    "    name = \"v3-with-lanes\"\n" +
                    "\n[environment]\n" +
                    "    slot_1 = \"env/lane-1.hdr\"\n");
                if (document_io.try_load(broken_path, out _, out _)) {
                    return selftest_failure("a version 3 document carrying an environment section was accepted");
                }
                File.Delete(broken_path);
                host_logger.write("Editor selftest: versions 1 and 3 still load and a future version is refused");
                host_logger.write("Editor selftest: a node sampling an unfilled lane is refused");

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

                if (!handle_outliner_control(outliner_panel.row_id(root.uid) + "_vis") || root.visible) {
                    return selftest_failure("the outliner visibility glyph did not hide the node");
                }
                if (!handle_outliner_control(outliner_panel.row_id(root.uid) + "_vis") || !root.visible) {
                    return selftest_failure("the outliner visibility glyph did not restore the node");
                }
                host_logger.write("Editor selftest: outliner visibility");

                var expected_asset = assets.Length != 0 ? assets[0] : null;
                if (expected_asset != null) {
                    browser_filter = expected_asset;
                    refresh_browser();
                    if (!handle_asset_click("asset_0", 0) || browser?.selected_asset != expected_asset) {
                        return selftest_failure("the content browser did not select the first asset");
                    }
                    if (!handle_asset_click("asset_0_name", Stopwatch.Frequency) || browser?.selected_asset != expected_asset) {
                        return selftest_failure("the content browser did not select the asset from its label");
                    }
                    browser?.clear_selection();
                    browser_filter = string.Empty;
                    reset_browser_interaction();
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
                // Behaviours write through the core callback. A component readback alone
                // would pass even when the renderer's local matrix was left untouched.
                if (!tree.try_get_bounds(added.runtime, out var bounds_before, out _)) {
                    return selftest_failure("could not read the asset bounds before a script transform");
                }
                var original_pose = added.local;
                var shifted_pose = original_pose;
                shifted_pose.position.x += 0.5f;
                if (scene_api.assign_transform(added.runtime.handle, &shifted_pose) != script_status.success ||
                    !tree.try_get_bounds(added.runtime, out var bounds_after, out _) ||
                    MathF.Abs(bounds_after.x - bounds_before.x - 0.5f) > 0.001f ||
                    scene_api.assign_transform(added.runtime.handle, &original_pose) != script_status.success) {
                    return selftest_failure("a script transform did not move the rendered scene bounds");
                }
                host_logger.write("Editor selftest: script transform updates scene bounds");
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

        private bool test_outliner_hierarchy() {
            if (document == null || selection_state == null || undo_history == null || outliner == null ||
                tree == null || ui == null) {
                return false;
            }
            var large = new scene_document();
            var last = new document_node { name = "above the old limit" };
            for (int row = 0; row < 80; ++row) {
                large.add_root(new document_node { name = $"Row {row}" });
            }
            large.add_root(last);
            outliner.refresh(large, selection_state, "");
            bool large_tree = outliner.visible_rows == 81 &&
                ui.try_set_element_style(outliner_panel.row_id(last.uid), "color", "#fff");
            refresh_panels();
            if (!large_tree) {
                return false;
            }

            var helmet = document.roots.First(node => node.asset == "damaged-helmet");
            var flowers = document.roots.First(node => node.asset == "flowers");
            select_outliner_row(helmet.uid, 0);
            select_outliner_row(flowers.uid, 2);
            if (selection_state.all.Count != 2) {
                return false;
            }
            select_outliner_row(flowers.uid, 1);
            if (selection_state.contains(flowers.uid) || !selection_state.contains(helmet.uid)) {
                return false;
            }
            select_outliner_row(flowers.uid, 1);

            var original_world = world_matrix(helmet);
            var group = new document_node {
                name = "Cast and Props",
                local = new transform_value {
                    position = new vector_3 { x = 2, y = 1, z = -3 },
                    rotation_degrees = new vector_3 { x = 17, y = 31, z = 8 },
                    scale = new vector_3 { x = 2, y = 2, z = 2 },
                },
            };
            if (!create_subtree(null, group) || !insert_document_node(null, document.roots.Count, group)) {
                return false;
            }
            var source = outliner_panel.row_id(helmet.uid);
            var target = outliner_panel.row_id(group.uid);
            handle_outliner_pointer(new(rml_event_type.pointer_down, source, "", 1, 1));
            handle_outliner_pointer(new(rml_event_type.pointer_move, target, "", 1, 24));
            handle_outliner_pointer(new(rml_event_type.pointer_up, target, "", 1, 24));
            if (group.children.Count != 2 || !near_matrix(world_matrix(helmet), original_world) ||
                !tree.try_get_transform(helmet.runtime, out var runtime_pose) || !same_transform(runtime_pose, helmet.local) ||
                !undo_history.try_undo() || group.children.Count != 0 || !document.roots.Contains(helmet) ||
                !undo_history.try_redo() || group.children.Count != 2 || !near_matrix(world_matrix(helmet), original_world)) {
                return false;
            }

            int expanded = outliner.visible_rows;
            handle_outliner_control(target + "_fold");
            if (outliner.visible_rows != expanded - 2) {
                return false;
            }
            outliner.refresh(document, selection_state, "damaged-helmet");
            if (outliner.visible_rows != 2) {
                return false;
            }
            refresh_panels();
            handle_outliner_control(target + "_fold");
            selection_state.select(group.uid);
            if (reparent_selection(helmet) || !duplicate_selection() ||
                document.find(selection_state.primary) is not { } copy || copy.children.Count != 2 ||
                copy.children[0].uid == group.children[0].uid ||
                !undo_history.try_undo() || document.find(copy.uid) != null ||
                !undo_history.try_redo() || document.find(copy.uid) == null) {
                return false;
            }
            selection_state.select(helmet.uid);
            if (!reparent_selection(copy) || !near_matrix(world_matrix(helmet), original_world) ||
                !copy.children.Contains(helmet) || group.children.Contains(helmet)) {
                return false;
            }
            host_logger.write("Editor selftest: uncapped Outliner, multi-select, subtree duplication and world-space reparent passed");
            return true;
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

            if (!test_outliner_hierarchy()) {
                host_logger.write("Editor mixed selftest: hierarchy editing failed");
                return false;
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

        private static bool requested_instances_selftest;

        private script_status run_instances_selftest() {
            if (document == null || selection_state == null || undo_history == null) {
                return selftest_failure("scene instance fixture is unavailable");
            }
            string project = managed_bootstrap.project_directory;
            var source_path = Path.Combine(project, "scenes", "prop-kit.scene.toml");
            var target_path = Path.Combine(project, "scenes", "instances.scene.toml");
            const string reference = "scenes/prop-kit.scene.toml";
            var source = new scene_document { name = "Prop Kit" };
            var prop = new document_node { name = "Linked Helmet", asset = "damaged-helmet" };
            prop.local.position.y = 1.55f;
            var light = new document_node { name = "Linked Light", light = point_light_component.default_light };
            light.local.position = new vector_3 { y = 0.4f, z = 1.2f };
            source.add_root(prop);
            source.add_root(light);
            if (!document_io.try_save(source, source_path, out var error)) {
                return selftest_failure($"could not save the source scene: {error}");
            }
            show_project_scenes(for_instance: true);
            int row = Array.IndexOf(project_scenes, reference);
            if (row < 0) { return selftest_failure("the source scene is missing from the picker"); }
            handle_project_scenes(new(rml_event_type.click, $"project_scene_{row}", string.Empty));
            handle_project_scenes(new(rml_event_type.click, "project_scene_open", string.Empty));
            if (document.find(selection_state.primary) is not { children.Count: 2 } first ||
                !apply_detail_change(detail_field.position_x, "-0.66") ||
                !duplicate_selection() ||
                document.find(selection_state.primary) is not { children.Count: 2 } second ||
                !apply_detail_change(detail_field.position_x, "0.66") ||
                first.children[0].uid == second.children[0].uid ||
                first.children[0].instance_root != first || second.children[0].instance_root != second ||
                !undo_history.try_undo() || second.local.position.x != first.local.position.x ||
                !undo_history.try_redo() || second.local.position.x != 0.66f ||
                scene_lights.collect(document).Length != 2) {
                return selftest_failure("scene instances did not place, duplicate, transform or undo independently");
            }
            second.children[0].environment = 2;
            second.environment = 1;
            bool override_applied = scene_sync.environment_lane(first.children[0]) == 0 &&
                                    scene_sync.environment_lane(second.children[0]) == 1;
            second.environment = 0;
            bool source_restored = scene_sync.environment_lane(second.children[0]) == 2;
            second.children[0].environment = 0;
            if (!override_applied || !source_restored) {
                return selftest_failure("scene instance environment override did not preserve its sibling or restore its source");
            }
            var saved_source = File.ReadAllText(source_path);
            if (save_document(source_path) || File.ReadAllText(source_path) != saved_source) {
                return selftest_failure("saving an instance host overwrote its source scene");
            }
            if (!document_io.try_save(document, target_path, out error) ||
                File.ReadAllText(target_path).Contains(prop.name, StringComparison.Ordinal)) {
                return selftest_failure($"the host scene did not save only the references: {error}");
            }
            var stable_id = first.children[0].uid;
            prop.name = "Updated Helmet";
            if (!document_io.try_save(source, source_path, out error) ||
                !scene_instances.try_load(project, target_path, out var loaded, out error) || loaded == null ||
                loaded.find(first.uid)?.children[0].name != prop.name ||
                loaded.find(second.uid)?.children[0].name != prop.name ||
                loaded.find(first.uid)?.children[0].uid != stable_id ||
                !replace_scene(loaded)) {
                return selftest_failure($"reopening did not pick up the shared source update: {error}");
            }
            if (document.find(first.uid) is not { } placed ||
                document.find(second.uid) is not { } other ||
                placed.children[0] is not { } door_a || other.children[0] is not { } door_b) {
                return selftest_failure("the reopened instances lost their expanded nodes");
            }
            prop.script = "demo.dll:Door";
            prop.script_parameters["target"] = "north";
            if (!document_io.try_save(source, source_path, out error) ||
                !scene_instances.try_load(project, target_path, out loaded, out error) || loaded == null ||
                !replace_scene(loaded) ||
                document.find(first.uid)?.children[0].script_parameters.GetValueOrDefault("target") != "north" ||
                document.find(second.uid)?.children[0].script_parameters.GetValueOrDefault("target") != "north" ||
                document.find(first.uid)?.children[0].source_script_parameters?.GetValueOrDefault("target") != "north" ||
                document.find(first.uid)?.children[0].source_uid == null) {
                return selftest_failure($"a source script parameter did not reach both instances: {error}");
            }
            var instance_a = document.find(first.uid)!;
            var instance_b = document.find(second.uid)!;
            set_instance_override(instance_a, instance_a.children[0].source_uid!.Value, "target", "east", "Set target");
            set_instance_override(instance_b, instance_b.children[0].source_uid!.Value, "target", "west", "Set target");
            if (instance_a.children[0].script_parameters.GetValueOrDefault("target") != "east" ||
                instance_b.children[0].script_parameters.GetValueOrDefault("target") != "west" ||
                instance_a.children[0].source_script_parameters?.GetValueOrDefault("target") != "north") {
                return selftest_failure("a placement override did not stay local to its instance");
            }
            if (!document_io.try_save(document, target_path, out error) ||
                !File.ReadAllText(target_path).Contains("instance_parameter.", StringComparison.Ordinal) ||
                !scene_instances.try_load(project, target_path, out loaded, out error) || loaded == null ||
                loaded.find(first.uid)?.children[0].script_parameters.GetValueOrDefault("target") != "east" ||
                loaded.find(second.uid)?.children[0].script_parameters.GetValueOrDefault("target") != "west" ||
                loaded.find(first.uid)?.children[0].source_script_parameters?.GetValueOrDefault("target") != "north" ||
                !replace_scene(loaded)) {
                return selftest_failure($"a placement override did not survive saving and reopening: {error}");
            }
            prop.script_parameters["target"] = "south";
            prop.script_parameters["label"] = "front";
            if (!document_io.try_save(source, source_path, out error) ||
                !scene_instances.try_load(project, target_path, out loaded, out error) || loaded == null ||
                loaded.find(first.uid)?.children[0] is not { } updated_a ||
                loaded.find(second.uid)?.children[0] is not { } updated_b ||
                updated_a.script_parameters.GetValueOrDefault("label") != "front" ||
                updated_b.script_parameters.GetValueOrDefault("label") != "front" ||
                updated_a.script_parameters.GetValueOrDefault("target") != "east" ||
                updated_b.script_parameters.GetValueOrDefault("target") != "west" ||
                updated_a.source_script_parameters?.GetValueOrDefault("target") != "south" ||
                updated_b.source_script_parameters?.GetValueOrDefault("target") != "south" ||
                !replace_scene(loaded)) {
                return selftest_failure($"a source edit did not reach both instances or lost an override: {error}");
            }
            var restored = document.find(first.uid)!;
            set_instance_override(restored, restored.children[0].source_uid!.Value, "target", null, "Restore target");
            if (document.find(first.uid)?.children[0].script_parameters.GetValueOrDefault("target") != "south" ||
                document.find(second.uid)?.children[0].script_parameters.GetValueOrDefault("target") != "west" ||
                document.find(first.uid)?.instance_overrides != null) {
                return selftest_failure("restoring an override did not return the source value");
            }
            var cyclic = new scene_document { name = "Cycle" };
            string cycle_name = $"cyclic-{Guid.NewGuid():N}.scene.toml";
            cyclic.add_root(new document_node { name = "Self", subscene = $"scenes/{cycle_name}" });
            var cycle_path = Path.Combine(project, "scenes", cycle_name);
            try {
                if (!document_io.try_save(cyclic, cycle_path, out error) ||
                    scene_instances.try_load(project, cycle_path, out _, out error) ||
                    error?.Contains("Circular", StringComparison.Ordinal) != true) {
                    return selftest_failure("a cyclic subscene was not diagnosed");
                }
            } finally {
                File.Delete(cycle_path);
            }
            host_logger.write("Editor selftest: scene instance picker, independent transforms, source updates, per-instance parameter overrides and cycle checks passed");
            return script_status.success;
        }

        private static bool requested_animation_selftest;

        /// <summary>
        /// Proves the animation authoring path of milestone 4: the two per-placement fields survive a
        /// save and a reload, the document gates refuse bad values, the Details rows drive the
        /// selected placement through Undo and Redo, and every placement of one animated asset answers
        /// its OWN player instead of a single scene-wide one.
        /// </summary>
        private script_status run_animation_selftest() {
            if (document == null || selection_state == null || undo_history == null) {
                return selftest_failure("the animation fixture is unavailable");
            }
            string project = managed_bootstrap.project_directory;
            var path = Path.Combine(project, "scenes", $"animation-selftest-{Guid.NewGuid():N}.scene.toml");
            const string animated = "laevatain";
            var original = document;
            var scene = new scene_document { name = "Animation Selftest" };
            var authored = new document_node {
                name = "Authored",
                asset = animated,
                animation_clip = 0,
                animation_autoplay = true,
            };
            var plain = new document_node { name = "Plain", asset = animated };
            scene.add_root(authored);
            scene.add_root(plain);
            try {
                if (!document_io.try_save(scene, path, out var error) || !document_io.try_load(path, out var loaded, out error) || loaded == null) {
                    return selftest_failure($"the animation placement fields did not save: {error}");
                }
                if (loaded.find(authored.uid) is not { animation_clip: 0, animation_autoplay: true } reloaded_authored ||
                    loaded.find(plain.uid) is not { animation_clip: -1, animation_autoplay: false }) {
                    return selftest_failure("the animation placement fields did not survive saving and reloading");
                }
                var text = File.ReadAllText(path);
                if (!text.Contains("animation_clip = 0", StringComparison.Ordinal) ||
                    !text.Contains("animation_autoplay = true", StringComparison.Ordinal)) {
                    return selftest_failure("the writer did not publish the animation placement fields");
                }
                // The version gate: the rows are document version 13, so an older document must refuse
                // them instead of silently dropping a placement's playback.
                File.WriteAllText(path, text.Replace("version = 13", "version = 12", StringComparison.Ordinal));
                if (document_io.try_load(path, out _, out var older_error) ||
                    older_error?.Contains("animation placement settings need document version 13", StringComparison.Ordinal) != true) {
                    return selftest_failure($"a version 12 document with animation placement settings was not refused: {older_error}");
                }
                // A negative index and animation rows without a placement are authoring mistakes.
                File.WriteAllText(path, text.Replace("animation_clip = 0", "animation_clip = -1", StringComparison.Ordinal));
                if (document_io.try_load(path, out _, out var negative_error) ||
                    negative_error?.Contains("animation_clip must be zero or greater", StringComparison.Ordinal) != true) {
                    return selftest_failure($"a negative animation clip was not diagnosed: {negative_error}");
                }
                File.WriteAllText(path, text);
                var loose = new document_node { name = "Loose", animation_autoplay = true };
                scene.add_root(loose);
                bool refused_loose = !document_io.try_save(scene, path, out var loose_error) &&
                                     loose_error?.Contains("has animation settings but no asset placement", StringComparison.Ordinal) == true;
                scene.remove_root(loose);
                if (!refused_loose) {
                    return selftest_failure("animation settings on a node without a placement were not refused");
                }
                authored.animation_clip = -2;
                bool refused_negative = !document_io.try_save(scene, path, out var clip_error) &&
                                        clip_error?.Contains("invalid initial animation clip", StringComparison.Ordinal) == true;
                authored.animation_clip = 0;
                if (!refused_negative) {
                    return selftest_failure("an invalid initial animation clip was not refused");
                }
                // The Details rows write the selected placement and record Undo.
                if (!replace_scene(loaded) || document.find(authored.uid) is not { animation_autoplay: true } placement) {
                    return selftest_failure("the animation scene did not open");
                }
                selection_state.select(placement.uid);
                toggle_animation_autoplay();
                if (placement.animation_autoplay) {
                    return selftest_failure("the Auto Play row did not flip the placement");
                }
                if (!undo_history.try_undo() || !placement.animation_autoplay ||
                    !undo_history.try_redo() || placement.animation_autoplay) {
                    return selftest_failure("the Auto Play row did not participate in Undo and Redo");
                }
                apply_animation_clip("-1");
                if (placement.animation_clip != -1 || !undo_history.try_undo() || placement.animation_clip != 0) {
                    return selftest_failure("the Initial Clip row did not participate in Undo");
                }
                // Per-placement players: the editor tree binds each placement's asset, so both must
                // report their own clip list and accept their own select/seek.
                string[] clips = placement.runtime.is_valid ? animation_player.get_clip_names(placement.runtime) : [];
                if (clips.Length > 0) {
                    if (document.find(plain.uid) is not { } other ||
                        !animation_player.try_select(placement.runtime, 0) ||
                        !animation_player.try_select(other.runtime, 0) ||
                        !animation_player.try_seek(placement.runtime, 5.0f) ||
                        !animation_player.try_seek(other.runtime, 20.0f)) {
                        return selftest_failure("two placements of one animated asset did not answer their own players");
                    }
                    // Deleting one character must not stop the other one's animation: its own player
                    // survives, and only the removed placement answers scene_not_found.
                    selection_state.select(placement.uid);
                    delete_selected();
                    bool removed_dropped = !animation_player.try_get_info(placement.runtime, out _);
                    bool survivor_kept = animation_player.try_get_info(other.runtime, out var survivor) &&
                                         survivor.available && survivor.clip_count > 0;
                    if (!removed_dropped || !survivor_kept) {
                        return selftest_failure("deleting one animated placement disturbed its neighbour or left a stale player");
                    }
                    if (!undo_history.try_undo() ||
                        document.find(authored.uid) is not { } back_node ||
                        !animation_player.try_get_info(back_node.runtime, out var back) ||
                        !back.available) {
                        return selftest_failure("Undoing the deletion did not restore the character's player");
                    }
                } else {
                    host_logger.write($"Editor selftest: the project has no animated placement for '{animated}', player addressing skipped");
                }
                host_logger.write("Editor selftest: animation placement clip, auto play, version gate, Undo and per-placement players passed");
                return script_status.success;
            } finally {
                File.Delete(path);
            }
        }

        private static bool requested_validation_selftest;

        /// <summary>Checks the project validator itself against a scratch project: one clean build, then one
        /// broken copy per problem class, so the gate the exporter relies on has its own coverage.</summary>
        private script_status run_validation_selftest() {
            string root = Path.Combine(managed_bootstrap.project_directory, $"validation-selftest-{Guid.NewGuid():N}");
            string manifest = Path.Combine(root, "deren.toml");
            string scene = Path.Combine(root, "scenes", "main.scene.toml");
            string voice = Path.Combine(root, "audio", "voice.ogg");
            try {
                Directory.CreateDirectory(Path.Combine(root, "scenes"));
                Directory.CreateDirectory(Path.Combine(root, "scripts"));
                Directory.CreateDirectory(Path.Combine(root, "asset"));
                Directory.CreateDirectory(Path.Combine(root, "audio"));
                File.WriteAllBytes(Path.Combine(root, "scripts", "Game.dll"), [0]);
                File.WriteAllBytes(voice, [0]);
                File.WriteAllText(Path.Combine(root, "render.toml"), "[render]\n    window_title = \"Validation Selftest\"\n");

                asset_authoring.try_open(out var authoring);
                asset_authoring? library = authoring.asset_directory != null ? authoring : (asset_authoring?)null;
                File.WriteAllText(scene, camera_scene("Validation Main"));
                File.WriteAllText(manifest, validation_manifest("scenes/main.scene.toml", "[\"audio/voice.ogg\"]"));
                var report = project_validator.validate(root, library, null);
                if (!report.ok || report.scene_count != 1 || report.checked_references != 1) {
                    return selftest_failure($"the clean scratch project did not check clean ({report.summary()} {first_problem(report)})");
                }

                File.Delete(voice);
                report = project_validator.validate(root, library, null);
                if (report.ok || !has_problem(report, "'audio/voice.ogg' does not exist")) {
                    return selftest_failure("a missing [export] include entry was not reported");
                }
                File.WriteAllBytes(voice, [0]);

                File.WriteAllText(manifest, validation_manifest("scenes/main.scene.toml", "[\"audio/voice.ogg\"]")
                                                .Replace("    scene = \"scenes/main.scene.toml\"\n", string.Empty, StringComparison.Ordinal));
                report = project_validator.validate(root, library, null);
                if (report.ok || !has_problem(report, "no startup scene is configured")) {
                    return selftest_failure("a project without a [game] scene was not reported");
                }
                File.WriteAllText(manifest, validation_manifest("scenes/main.scene.toml", "[\"audio/voice.ogg\"]"));

                File.WriteAllText(scene, camera_scene("Validation Main", "    asset = \"definitely-missing\"\n"));
                report = project_validator.validate(root, library, null);
                if (report.ok || !has_problem(report, "asset 'definitely-missing' is not in the asset library")) {
                    return selftest_failure("an unresolvable asset id was not reported");
                }

                File.WriteAllText(scene, camera_scene("Validation Main", "    script = \"Missing.dll:demo.Nothing\"\n"));
                report = project_validator.validate(root, library, null);
                if (report.ok || !has_problem(report, "behaviour assembly 'Missing.dll' is not in scripts/")) {
                    return selftest_failure("a behaviour whose assembly is missing was not reported");
                }

                File.WriteAllText(scene, camera_scene("Validation Main", "    subscene = \"scenes/absent.scene.toml\"\n"));
                report = project_validator.validate(root, library, null);
                if (report.ok || !has_problem(report, "scene instance 'scenes/absent.scene.toml' does not exist")) {
                    return selftest_failure("a missing scene instance was not reported");
                }

                File.WriteAllText(scene, camera_scene("Validation Main", "    audio_file = \"audio/absent.ogg\"\n"));
                report = project_validator.validate(root, library, null);
                if (report.ok || !has_problem(report, "audio file 'audio/absent.ogg' does not exist")) {
                    return selftest_failure("a missing audio file was not reported");
                }

                File.WriteAllText(scene, camera_scene("Validation Main").Replace("[[node]]", "[environment]\n    slot_1 = \"asset:sky.hdr\"\n\n[[node]]", StringComparison.Ordinal));
                report = project_validator.validate(root, library, null);
                if (report.ok || !has_problem(report, "environment maps are project files, not asset library ids")) {
                    return selftest_failure("an asset-id environment map was not reported");
                }

                File.WriteAllText(scene, camera_scene("Validation Main").Replace("[[node]]", "[environment]\n    slot_1 = \"environment/absent.hdr\"\n\n[[node]]", StringComparison.Ordinal));
                report = project_validator.validate(root, library, null);
                if (report.ok || !has_problem(report, "environment map 'environment/absent.hdr' does not exist")) {
                    return selftest_failure("a missing environment map was not reported");
                }

                File.WriteAllText(scene, camera_scene("Validation Loop", "    subscene = \"scenes/loop.scene.toml\"\n"));
                File.WriteAllText(Path.Combine(root, "scenes", "loop.scene.toml"),
                                  camera_scene("Validation Loop", "    subscene = \"scenes/main.scene.toml\"\n"));
                report = project_validator.validate(root, library, null);
                if (report.ok || !has_problem(report, "the scene instance chain loops back to this file")) {
                    return selftest_failure("a looping scene instance chain was not reported");
                }

                show_project_scenes();
                handle_project_scenes(new(rml_event_type.click, "project_scene_check", string.Empty));
                if (notice == null || !notice.Contains("Project check", StringComparison.Ordinal)) {
                    return selftest_failure("the project scene panel did not run the project check");
                }
                close_project_panel();

                host_logger.write("Editor selftest: project validation passes a clean project and reports every broken reference class passed");
                return script_status.success;
            } catch (Exception failure) when (failure is IOException or UnauthorizedAccessException) {
                return selftest_failure($"the validation selftest could not write its scratch project: {failure.Message}");
            } finally {
                try {
                    if (Directory.Exists(root)) { Directory.Delete(root, true); }
                } catch (Exception failure) when (failure is IOException or UnauthorizedAccessException) {
                    host_logger.write($"Editor selftest: could not remove '{root}': {failure.Message}");
                }
            }
        }

        private static string camera_scene(string name, string extra = "") =>
            $"[document]\n    version = 13\n    name = \"{name}\"\n\n" +
            (extra.Length == 0 ? string.Empty : string.Empty) +
            $"[[node]]\n    uid = \"4b1c2d3e-4f50-4a61-8b72-9c8d0e1f2a3b\"\n    name = \"Camera\"\n" +
            $"    camera = \"main\"\n{extra}";

        private static string validation_manifest(string scene, string include) =>
            "[project]\n    name = \"Validation Selftest\"\n    main_scene = \"render.toml\"\n\n" +
            "[paths]\n    asset_library = \"asset\"\n\n" +
            "[scripts]\n    assembly = \"scripts/Game.dll\"\n\n" +
            $"[editor]\n    scene = \"{scene}\"\n\n" +
            $"[game]\n    scene = \"{scene}\"\n\n" +
            $"[export]\n    include = {include}\n";

        private static bool has_problem(project_report report, string fragment) =>
            report.problems.Exists(problem => problem.message.Contains(fragment, StringComparison.Ordinal));

        private static string first_problem(project_report report) =>
            report.problems.Count == 0 ? string.Empty : report.problems[0].describe();

        private static bool requested_relocation_selftest;

        private script_status run_relocation_selftest() {
            const string asset_id = "move-sample";
            const string renamed = "Studio Helmet";
            const string destination = "Props/Street/Helmet";
            if (document?.path is not { } scene_path || browser == null ||
                !asset_authoring.try_open(out var authoring) || authoring.library_root() is not { } root ||
                authoring.asset_directory(asset_id) is not { } source ||
                authoring.motion_path(asset_id) is not { } motion) {
                return selftest_failure("asset relocation fixture was not loaded");
            }

            var saved = File.ReadAllText(scene_path);
            var source_manifest = Path.Combine(source, "asset.toml");
            var original = File.ReadAllText(source_manifest);
            foreach (string blocked in new[] { "../outside", "Taken", "Too/Deep/To/Index" }) {
                if (asset_relocation.try_apply(authoring, asset_id, blocked, renamed, out _) ||
                    authoring.asset_directory(asset_id) != source || File.ReadAllText(source_manifest) != original) {
                    return selftest_failure($"refused path '{blocked}' changed the package");
                }
            }

            browser_filter = asset_id;
            refresh_browser();
            if (!handle_asset_click("asset_0", 0)) {
                return selftest_failure("asset editing did not select the package");
            }
            show_asset_edit();
            if (asset_edit_id != asset_id) { return selftest_failure("the asset edit dialog did not open"); }
            handle_asset_edit(new(rml_event_type.change, "asset_edit_name", renamed));
            handle_asset_edit(new(rml_event_type.change, "asset_edit_path", destination));
            handle_asset_edit(new(rml_event_type.click, "asset_edit_save", string.Empty));

            string target = Path.Combine(root, destination);
            if (asset_edit_id != null || Directory.Exists(source) ||
                authoring.asset_directory(asset_id) is not { } indexed || Path.GetRelativePath(target, indexed) != "." ||
                authoring.display_name(asset_id) != renamed || authoring.motion_path(asset_id) != motion ||
                !File.ReadAllText(Path.Combine(target, "asset.toml")).Contains("# Keep model comment", StringComparison.Ordinal) ||
                !File.ReadAllText(Path.Combine(target, "asset.toml")).Contains("id = \"move-sample\"", StringComparison.Ordinal) ||
                File.ReadAllText(scene_path) != saved) {
                return selftest_failure("moving the package changed its identity, references, or unrelated text");
            }
            if (!document_io.try_load(scene_path, out var loaded, out var error) || loaded == null ||
                missing_asset_message(loaded) != null || !replace_scene(loaded)) {
                return selftest_failure($"the saved scene could not reopen after relocating its asset: {error}");
            }
            browser_filter = renamed;
            refresh_browser();
            if (!browser.try_tile_asset(0, out var match) || match != asset_id) {
                return selftest_failure("search did not find the new display name");
            }
            host_logger.write("Editor selftest: asset rename, relocation, rollback, motion and stable scene references passed");
            return script_status.success;
        }

        private static bool requested_browser_selftest;

        private script_status run_browser_selftest() {
            if (browser == null || document == null || undo_history == null ||
                !handle_asset_click("asset_0_name", 0) || browser.current_folder != "Fixtures" ||
                !browser.try_tile_asset(content_browser_panel.page_size - 1, out var last) || last != "fixture-047") {
                return selftest_failure("the content browser did not open the fixture folder or rendered the wrong first page");
            }
            change_browser_location("browser_next");
            if (!browser.try_tile_asset(0, out var next) || next != "fixture-048") {
                return selftest_failure("the content browser did not reach assets after the first page");
            }
            var before = count_nodes(document.roots);
            if (!handle_asset_click("asset_5", 0) ||
                !handle_asset_click("asset_5_name", Stopwatch.Frequency / 10) ||
                document.find(selection_state?.primary ?? Guid.Empty)?.asset != "fixture-053" ||
                count_nodes(document.roots) != before + 1 || !undo_history.try_undo()) {
                return selftest_failure("placement did not use the selected page's asset");
            }
            change_browser_location("browser_up");
            browser_filter = "fixture-053";
            refresh_browser();
            if (browser.current_folder.Length != 0 || !browser.try_tile_asset(0, out var search) ||
                search != "fixture-053" || browser.try_tile_asset(1, out _)) {
                return selftest_failure("search did not find a nested asset from the root");
            }
            var broken = new scene_document { name = "Missing reference fixture" };
            var parent = new document_node { name = "Props" };
            parent.children.Add(new document_node { name = "Lamp", asset = "removed-lamp" });
            broken.add_root(parent);
            if (missing_asset_message(broken) is not { } message ||
                !message.Contains("Props / Lamp", StringComparison.Ordinal) ||
                !message.Contains("removed-lamp", StringComparison.Ordinal)) {
                return selftest_failure("asset reference diagnostics omitted the id or node path");
            }
            browser_filter = string.Empty;
            refresh_browser();
            if (!handle_asset_click("asset_0_name", 0) || browser.current_folder != "Fixtures") {
                return selftest_failure("could not re-open the import target folder");
            }
            host_logger.write("Editor selftest: folder navigation, paging, nested search, placement and missing references passed");
            return script_status.success;
        }

        private script_status run_import_selftest(string source) {
            if (document == null || tree == null || undo_history == null ||
                !asset_authoring.try_open(out var authoring) || authoring.library_root() is not { } library) {
                return selftest_failure("asset import test has no configured catalogue");
            }
            const string id = "import-test";
            const string missing_id = "import-test-missing";
            if (!check_import_rejections(library, missing_id, out var rejection)) {
                return selftest_failure(rejection);
            }

            var before = count_nodes(document.roots);
            var previous = document.roots.Count > 0 ? document.roots[0].runtime : default;
            show_asset_import();
            if (!asset_import_open) { return selftest_failure("the asset import dialog did not open"); }
            handle_asset_import(new(rml_event_type.change, "asset_import_source", source));
            handle_asset_import(new(rml_event_type.change, "asset_import_id", id));
            handle_asset_import(new(rml_event_type.click, "asset_import_accept", string.Empty));
            if (asset_import_open || Array.IndexOf(assets, id) < 0 || count_nodes(document.roots) != before ||
                (previous.is_valid && !tree.try_get_transform(previous, out _))) {
                return selftest_failure("import did not refresh the browser and preserve the existing scene");
            }
            if (try_import_model(source, id, out var duplicate) || !duplicate.Contains("already in use", StringComparison.Ordinal)) {
                return selftest_failure("duplicate asset ID was accepted");
            }
            var time = Stopwatch.GetTimestamp();
            if (!handle_asset_click("asset_0", time) ||
                !handle_asset_click("asset_0_name", time + Stopwatch.Frequency / 10) ||
                count_nodes(document.roots) != before + 1 || document.find(selection_state?.primary ?? Guid.Empty)?.asset != id ||
                !undo_history.try_undo() || count_nodes(document.roots) != before ||
                !undo_history.try_redo() || count_nodes(document.roots) != before + 1) {
                return selftest_failure("the imported tile could not place, undo or redo the model");
            }

            var path = Path.Combine(managed_bootstrap.project_directory, "scenes", "imported.scene.toml");
            if (!document_io.try_save(document, path, out var error) ||
                !document_io.try_load(path, out var loaded, out error) || loaded == null ||
                !same_document(document, loaded, out error) || !replace_scene(loaded)) {
                return selftest_failure($"the imported scene did not reload: {error}");
            }
            if (Directory.GetDirectories(library, ".import-*").Length != 0) {
                return selftest_failure("asset import left a staging directory behind");
            }
            host_logger.write("Editor selftest: import dialog, rollback, placement and reload passed");
            return script_status.success;
        }

        private bool check_import_rejections(string library, string missing_id, out string error) {
            var scratch = Path.Combine(managed_bootstrap.project_directory, ".deren", $"import-fixture-{Guid.NewGuid():N}");
            Directory.CreateDirectory(scratch);
            try {
                var broken = Path.Combine(scratch, "missing-texture.gltf");
                File.WriteAllText(broken, """{"asset":{"version":"2.0"},"images":[{"uri":"textures/missing.png"}]}""");
                if (try_import_model(broken, missing_id, out var missing) ||
                    !missing.Contains("Missing model dependency", StringComparison.Ordinal) ||
                    Directory.Exists(Path.Combine(library, missing_id))) {
                    error = $"missing texture was not rejected cleanly: {missing}";
                    return false;
                }
                var bad_model = Path.Combine(scratch, "unsupported.gltf");
                File.WriteAllText(bad_model,
                    """{"asset":{"version":"2.0"},"extensionsUsed":["DEREN_TestUnsupported"],"extensionsRequired":["DEREN_TestUnsupported"]}""");
                const string invalid_id = "import-test-invalid";
                if (try_import_model(bad_model, invalid_id, out var invalid) ||
                    !invalid.Contains("could not load the imported model", StringComparison.Ordinal) ||
                    Directory.Exists(Path.Combine(library, invalid_id))) {
                    error = $"a model rejected by the engine was not rolled back: {invalid}";
                    return false;
                }
            } finally {
                Directory.Delete(scratch, recursive: true);
            }
            error = string.Empty;
            return true;
        }

        private static bool requested_collision_selftest;

        private script_status run_collision_selftest() {
            if (document == null || selection_state == null || undo_history == null ||
                !handle_menu_click("menu_add_collision_box") ||
                document.find(selection_state.primary) is not { collision_box: true } node) {
                return selftest_failure("could not add an authored collision box");
            }
            toggle_volume(solid: true);
            if (node.collision_box || !undo_history.try_undo() || !node.collision_box ||
                !undo_history.try_redo() || node.collision_box || !undo_history.try_undo()) {
                return selftest_failure("solid box edits did not participate in Undo and Redo");
            }

            // The kernel is exercised directly for the geometry cases: a thin wall, a corner, a gap the
            // capsule must fit through, and a floor. The numbers are exact contact distances (the kernel
            // has no skin): the wall face is at x = 1.98 and the sphere radius is 0.12, so a head-on move
            // stops at 1.86, and the same capsule walking diagonally into both a wall and a cross box
            // stops on x = 1.86 and z = 0.86.
            static vector_3 at(float x, float y, float z) => new() { x = x, y = y, z = z };
            static collision_box block(vector_3 center, vector_3 half, uint id) =>
                new(center, half, at(1, 0, 0), at(0, 1, 0), at(0, 0, 1), id);
            static collision_capsule sphere(float x, float y, float z, float radius = 0.12f) =>
                new(at(x, y, z), at(x, y, z), radius);
            collision_world.reset();
            if (!collision_world.add_box(block(at(2, 0, 0), at(0.02f, 1.5f, 3), 1)) ||
                !collision_world.add_box(block(at(0, -0.6f, 0), at(2, 0.1f, 2), 3))) {
                return selftest_failure("the collision kernel did not accept the selftest boxes");
            }
            var straight = default(capsule_move);
            var diagonal = default(capsule_move);
            if (!collision_world.move(sphere(0, 0, 0), at(12, 0, 0), out straight) ||
                !collision_world.move(sphere(0, 0, 0), at(6, 0, 2), out diagonal) ||
                !straight.blocked || !diagonal.blocked ||
                MathF.Abs(straight.displacement.x - 1.86f) > 0.005f ||
                MathF.Abs(diagonal.displacement.x - 1.86f) > 0.005f ||
                MathF.Abs(diagonal.displacement.z - 2.0f) > 0.005f ||
                straight.grounded || diagonal.grounded || straight.normal.x > -0.95f) {
                return selftest_failure($"unexpected capsule sweep result: straight {straight}, slide {diagonal}");
            }
            if (!collision_world.add_box(block(at(0, 0, 1), at(3, 1.5f, 0.02f), 2))) {
                return selftest_failure("the collision kernel did not accept the corner box");
            }
            var corner = default(capsule_move);
            if (!collision_world.move(sphere(0, 0, 0), at(6, 0, 2), out corner) || !corner.blocked ||
                MathF.Abs(corner.displacement.x - 1.86f) > 0.005f ||
                MathF.Abs(corner.displacement.z - 0.86f) > 0.005f) {
                return selftest_failure($"a corner did not slide along both walls: {corner}");
            }
            // A downward probe onto the floor must report a landing with an up-facing normal, and the
            // same probe over the floor's edge must travel unimpeded: that is how a behaviour tells them
            // apart. The floor's top is y = -0.5 and the sphere keeps its radius (0.12) above it, so
            // landing from y = 0.2 must travel -0.58.
            var land = default(capsule_move);
            var edge = default(capsule_move);
            if (!collision_world.move(sphere(0, 0.2f, 0), at(0, -3, 0), out land) ||
                !collision_world.move(sphere(3, 0.2f, 0), at(0, -3, 0), out edge) ||
                !land.blocked || !land.grounded || MathF.Abs(land.displacement.y + 0.58f) > 0.005f ||
                land.normal.y < 0.99f || edge.blocked || edge.grounded ||
                MathF.Abs(edge.displacement.y + 3.0f) > 0.001f) {
                return selftest_failure($"a downward sweep did not land on the floor and fall past an edge: land {land}, edge {edge}");
            }
            // The capsule the demo walks with has a 0.3 radius, so a 0.7 wide opening between two walls
            // is passable on the centre line and blocked once the capsule is offset into a wall.
            if (!collision_world.remove_box(1) || !collision_world.remove_box(2) ||
                !collision_world.add_box(block(at(0, 0, 0.85f), at(0.05f, 1.5f, 0.5f), 4)) ||
                !collision_world.add_box(block(at(0, 0, -0.85f), at(0.05f, 1.5f, 0.5f), 5))) {
                return selftest_failure("the collision kernel did not accept the doorway boxes");
            }
            var gap = default(capsule_move);
            var offset = default(capsule_move);
            if (!collision_world.move(sphere(-2, 0, 0, 0.3f), at(5, 0, 0), out gap) ||
                !collision_world.move(sphere(-2, 0, 0.2f, 0.3f), at(5, 0, 0), out offset) ||
                gap.blocked || MathF.Abs(gap.displacement.x - 5.0f) > 0.005f ||
                !offset.blocked) {
                return selftest_failure($"a 0.7 wide opening did not admit a 0.3 radius capsule on the centre line: gap {gap}, offset {offset}");
            }
            collision_world.reset();
            var path = Path.Combine(managed_bootstrap.project_directory, "scenes", $"collision-check-{Guid.NewGuid():N}.scene.toml");
            try {
                if (!document_io.try_save(document, path, out var error) ||
                    !document_io.try_load(path, out var loaded, out error) ||
                    loaded?.find(node.uid)?.collision_box != true) {
                    return selftest_failure($"collision box data did not round trip: {error}");
                }
            } finally {
                File.Delete(path);
            }
            // A moving solid follows its node's world transform instead of the one captured at load. The
            // flag only makes sense on a solid box, participates in Undo and Redo, needs version 11 and
            // must survive a save and a reload. The solid box left over from the Undo/Redo round is on.
            toggle_moving_solid();
            if (!node.collision_box || !node.moving_solid) {
                return selftest_failure("could not mark a solid box as a moving solid");
            }
            if (!undo_history.try_undo() || node.moving_solid || !undo_history.try_redo() || !node.moving_solid) {
                return selftest_failure("the moving solid flag did not participate in Undo and Redo");
            }
            try {
                if (!document_io.try_save(document, path, out var error) ||
                    !document_io.try_load(path, out var loaded, out error) ||
                    loaded?.find(node.uid)?.moving_solid != true) {
                    return selftest_failure($"moving solid data did not round trip: {error}");
                }
                var older = File.ReadAllText(path).Replace("version = 13", "version = 10", StringComparison.Ordinal);
                File.WriteAllText(path, older);
                if (document_io.try_load(path, out _, out var refused) || refused?.Contains("moving solids need document version 11", StringComparison.Ordinal) != true) {
                    return selftest_failure($"a version 10 document with a moving solid was not refused: {refused}");
                }
            } finally {
                File.Delete(path);
            }
            // A moving solid samples its node's world transform at the start of every sweep, so lifting a
            // door opens the passage it used to fill. A static box keeps the transform it was collected
            // with, which is what the same node must do once it is no longer marked as moving.
            if (tree == null || document.roots.Count == 0) {
                return selftest_failure("the scene has no root to sweep through the moving solid");
            }
            var walker = document.roots[0];
            var walker_restore = walker.local;
            var door_restore = node.local;
            try {
                node.local.position = new vector_3 { x = 0, y = 0, z = 0 };
                node.local.scale = new vector_3 { x = 2, y = 2, z = 2 };
                walker.local.position = new vector_3 { x = 0, y = 0, z = -3 };
                if (!push_runtime(node) || !push_runtime(walker)) {
                    return selftest_failure("could not place the moving solid and the sweeping root");
                }
                var collision = new scene_collision(document, tree);
                collision_move closed = default;
                if (!collision.try_move(walker.runtime, new vector_3 { z = 5 }, 0.1f, 0.0f, 0.0f, 0.0f, out closed) || !closed.blocked) {
                    return selftest_failure($"the closed moving solid did not block the sweep: {closed}");
                }
                // Driving the door's transform, as a behaviour does, must move its collision with it.
                node.local.position.y = 5;
                collision_move opened = default;
                if (!push_runtime(node) ||
                    !collision.try_move(walker.runtime, new vector_3 { z = 5 }, 0.1f, 0.0f, 0.0f, 0.0f, out opened) ||
                    opened.blocked || MathF.Abs(opened.displacement.z - 5) > 0.001f) {
                    return selftest_failure($"the opened moving solid still blocked the sweep: {opened}");
                }
                // The same node authored as a static box stays at the transform it was collected with.
                node.moving_solid = false;
                node.local.position.y = 0;
                if (!push_runtime(node)) { return selftest_failure("could not re-author the solid box"); }
                walker.local.position = new vector_3 { x = 0, y = 0, z = -3 };
                if (!push_runtime(walker)) { return selftest_failure("could not reset the root for the static sweep"); }
                collision = new scene_collision(document, tree);
                node.local.position.y = 5;
                collision_move stale = default;
                if (!push_runtime(node) ||
                    !collision.try_move(walker.runtime, new vector_3 { z = 5 }, 0.1f, 0.0f, 0.0f, 0.0f, out stale) ||
                    !stale.blocked) {
                    return selftest_failure($"a static box followed its node instead of its authored transform: {stale}");
                }
            } finally {
                node.moving_solid = true;
                node.local = door_restore;
                walker.local = walker_restore;
                _ = push_runtime(node);
                _ = push_runtime(walker);
            }
            set_active_tool("vp_scale");
            host_logger.write("Editor selftest: solid box authoring, capsule wall and corner sweep, offset doorway, floor landing, edge fall, moving solid and a lifted door passed");
            return script_status.success;
        }

        private static bool requested_audio_source_selftest;

        private script_status run_audio_source_selftest() {
            if (document == null || selection_state == null || undo_history == null ||
                !handle_menu_click("menu_add_audio_source") ||
                document.find(selection_state.primary) is not { audio_source: not null } source) {
                return selftest_failure("could not add an audio source through the editor");
            }
            audio_fields["audio_file"] = "audio/tone.wav";
            audio_fields["audio_volume"] = "0.4";
            audio_fields["audio_loop"] = "on";
            handle_audio_source("audio_apply");
            var expected = new audio_source_component("audio/tone.wav", 0.4f, true);
            if (source.audio_source != expected ||
                !undo_history.try_undo() || source.audio_source != audio_source_component.unassigned ||
                !undo_history.try_redo() || source.audio_source != expected) {
                return selftest_failure("audio source settings did not participate in Undo and Redo");
            }
            handle_audio_source("audio_preview");
            if (audio_preview == null ||
                !audio_preview.try_get_duration(out var duration) || duration <= 0 ||
                !audio_preview.try_get_volume(out var volume) || MathF.Abs(volume - 0.4f) > 0.0001f ||
                !audio_preview.try_get_looping(out var loop) || !loop) {
                return selftest_failure("audio preview did not apply the clip, volume, and loop settings");
            }
            handle_audio_source("audio_stop");
            if (audio_preview != null) { return selftest_failure("stopping the preview retained its player"); }
            var project = managed_bootstrap.project_directory;
            var path = Path.Combine(project, "scenes", "audio-source-selftest.scene.toml");
            try {
                if (!document_io.try_save(document, path, out var error) ||
                    !document_io.try_load(path, out var loaded, out error) ||
                    loaded?.find(source.uid)?.audio_source != expected) {
                    return selftest_failure($"audio source settings did not round trip: {error}");
                }
            } finally {
                File.Delete(path);
            }
            host_logger.write("Editor selftest: audio source authoring, preview, undo and storage passed");
            return script_status.success;
        }

        private static bool requested_trigger_selftest;

        private script_status run_trigger_selftest() {
            if (document == null || selection_state == null || undo_history == null || tree == null ||
                document.roots.Count == 0) {
                return selftest_failure("the trigger test needs a document and scene tree");
            }
            var moving = document.roots[0];
            if (!moving.trigger_target) {
                selection_state.select(moving.uid);
                toggle_trigger_target();
                if (!moving.trigger_target || !undo_history.try_undo() || moving.trigger_target || !undo_history.try_redo()) {
                    return selftest_failure("trigger target state did not participate in undo and redo");
                }
            }
            if (!handle_menu_click("menu_add_box_trigger") ||
                document.find(selection_state.primary) is not { box_trigger: true } volume ||
                !apply_detail_change(detail_field.position_x, "2") ||
                !apply_detail_change(detail_field.scale_x, "0.5") ||
                !apply_detail_change(detail_field.scale_z, "2") ||
                !apply_detail_change(detail_field.rotation_y, "90")) {
                return selftest_failure("the editor could not place and size a rotated trigger");
            }
            var tracker = new scene_triggers(document);
            var events = new List<bool>();
            void notify(document_node trigger, document_node actor, bool entered) {
                if (trigger == volume && actor == moving) { events.Add(entered); }
            }
            tracker.update(tree, notify);
            var pose = moving.local;
            pose.position = volume.local.position;
            pose.position.x += 0.85f;
            if (!tree.try_set_transform(moving.runtime, pose)) {
                return selftest_failure("the trigger test could not move its target");
            }
            tracker.update(tree, notify);
            tracker.update(tree, notify);
            pose.position.z += 0.6f;
            if (!tree.try_set_transform(moving.runtime, pose)) {
                return selftest_failure("the trigger test could not move its target outside the box");
            }
            tracker.update(tree, notify);
            tracker.update(tree, notify);
            if (events.Count != 2 || !events[0] || events[1] || !tree.try_set_transform(moving.runtime, moving.local)) {
                return selftest_failure("the runtime did not emit exactly one entry and exit across the rotated box");
            }
            if (!document_io.try_serialize(document, out var data, out var error) ||
                !data.Contains("box_trigger = true", StringComparison.Ordinal) ||
                !data.Contains("trigger_target = true", StringComparison.Ordinal)) {
                return selftest_failure($"the authoring state did not serialize: {error}");
            }
            set_active_tool("vp_scale");
            host_logger.write("Editor selftest: trigger authoring, rotated box, transition edges and serialization passed");
            return script_status.success;
        }

        private static bool requested_input_action_selftest;

        private script_status run_input_action_selftest() {
            var source = File.ReadAllText(project_manifest_path);
            show_project_settings();
            handle_project_settings(new(rml_event_type.click, "project_settings_input", ""));
            if (open_project_panel != project_panel.input_actions) {
                return selftest_failure("the input actions panel did not open from project settings");
            }
            int row = Array.IndexOf(action_rows, "next_scene");
            if (row < 0) { return selftest_failure("the input action fixture needs next_scene"); }
            handle_input_actions(new(rml_event_type.click, $"input_row_{row}", ""));
            handle_input_actions(new(rml_event_type.click, "input_action_record", ""));
            if (!capture_input_binding(new(input_type.key, 0, true, 'J', 0, 0, 0, 0, 0, 0))) {
                return selftest_failure("the input action panel did not capture the key");
            }
            finish_input_capture();
            handle_input_actions(new(rml_event_type.click, "input_key_0", ""));
            handle_input_actions(new(rml_event_type.change, "input_action_name", "test_unbound"));
            handle_input_actions(new(rml_event_type.click, "input_action_add", ""));
            handle_input_actions(new(rml_event_type.click, "input_actions_save", ""));
            if (open_project_panel != project_panel.input_actions || File.ReadAllText(project_manifest_path) != source) {
                return selftest_failure("an unbound action was written to the manifest");
            }
            handle_input_actions(new(rml_event_type.click, "input_action_remove", ""));
            handle_input_actions(new(rml_event_type.click, "input_actions_save", ""));
            var saved = File.ReadAllText(project_manifest_path);
            if (open_project_panel != project_panel.settings ||
                !input_actions_file.try_read(saved, out var actions, out _) ||
                actions["next_scene"] is not ["j"] ||
                !actions.ContainsKey("move_left") || !saved.Contains("# input actions fixture", StringComparison.Ordinal) ||
                !saved.Contains("# unchanged project marker", StringComparison.Ordinal)) {
                return selftest_failure("recording or saving input actions lost manifest data");
            }
            host_logger.write("Editor selftest: input recording, invalid binding refusal and manifest save passed");
            show_input_actions();
            return script_status.success;
        }

        private script_status run_project_selftest() {
            show_project_scenes();
            int index = Array.IndexOf(project_scenes, "scenes/second.scene.toml");
            if (open_project_panel != project_panel.scenes || index < 0) {
                return selftest_failure("the project scene list did not include second.scene.toml");
            }
            handle_project_scenes(new(rml_event_type.click, $"project_scene_{index}", string.Empty));
            handle_project_scenes(new(rml_event_type.click, "project_scene_startup", string.Empty));
            var manifest = toml_table.parse(File.ReadAllText(project_manifest_path), out _);
            if (manifest?.table("game")?.text("scene") != "scenes/second.scene.toml") {
                return selftest_failure("the startup scene was not saved in the manifest");
            }
            handle_project_scenes(new(rml_event_type.click, "project_scene_open", string.Empty));
            if (open_project_panel != project_panel.none || document?.path == null ||
                Path.GetFileName(document.path) != "second.scene.toml") {
                return selftest_failure("opening from the scene list failed");
            }

            show_project_settings();
            if (open_project_panel != project_panel.settings || look_path == null) {
                return selftest_failure("the project settings panel did not open");
            }
            var settings_path = look_path;
            var original = File.ReadAllText(settings_path);
            handle_project_settings(new(rml_event_type.change, "look_exposure", "1.42"));
            handle_project_settings(new(rml_event_type.click, "project_settings_preview", string.Empty));
            if (!look_previewed || File.ReadAllText(settings_path) != original) {
                return selftest_failure("preview changed the renderer settings file");
            }
            handle_project_settings(new(rml_event_type.click, "project_settings_cancel", string.Empty));
            if (open_project_panel != project_panel.none || File.ReadAllText(settings_path) != original) {
                return selftest_failure("cancel did not close the preview without writing");
            }

            show_project_settings();
            handle_project_settings(new(rml_event_type.change, "look_exposure", "invalid"));
            handle_project_settings(new(rml_event_type.click, "project_settings_save", string.Empty));
            if (open_project_panel != project_panel.settings || File.ReadAllText(settings_path) != original) {
                return selftest_failure("an invalid preview value changed the renderer settings file");
            }
            set_look_field("look_exposure", "1.42");
            set_look_field("look_background", "1");
            set_look_field("look_color", "0.14, 0.27, 0.42");
            set_look_field("look_fxaa", "off");
            set_look_field("look_taa", "on");
            handle_project_settings(new(rml_event_type.click, "project_settings_save", string.Empty));
            var saved = File.ReadAllText(settings_path);
            var config = toml_table.parse(saved, out _);
            if (open_project_panel != project_panel.none ||
                config?.table("render")?.number("exposure") != 1.42f ||
                config.table("render")?.flag("fxaa") != false || config.table("render")?.flag("taa") != true ||
                config.table("lighting")?.integer("environment_background") != 1 ||
                !saved.Contains("# Preserve this project note", StringComparison.Ordinal)) {
                return selftest_failure("the saved runtime settings or unrelated notes did not round-trip");
            }
            show_project_settings();
            if (open_project_panel != project_panel.settings || look_fields["look_taa"] != "on" ||
                look_fields["look_exposure"] != "1.42") {
                return selftest_failure("reopening the settings did not read the saved render values");
            }
            host_logger.write("Editor selftest: project scenes, preview, cancel and settings save passed");
            return script_status.success;
        }

        private static bool requested_authoring_selftest;
        private static bool requested_lights_selftest;

        private script_status run_authoring_selftest() {
            if (document == null || selection_state == null || undo_history == null ||
                !orbit_camera.try_get(out var view)) {
                return selftest_failure("authoring fixture is unavailable");
            }
            view.target = new vector_3 { x = 0.3f, y = 0.15f, z = -0.3f };
            view.yaw_degrees = 17;
            view.pitch_degrees = -9;
            view.distance = 4;
            if (!view.try_apply() || !handle_menu_click("menu_add_camera") ||
                document.find(selection_state.primary) is not { camera: true } camera) {
                return selftest_failure("could not create an authored camera");
            }
            camera_command("camera_main");
            camera_fov_draft = "58";
            camera_command("camera_apply_fov");
            if (!scene_cameras.try_pose(document, camera, out var authored) ||
                MathF.Abs(authored.yaw_degrees - view.yaw_degrees) > 0.001f ||
                MathF.Abs(authored.pitch_degrees - view.pitch_degrees) > 0.001f ||
                authored.vertical_fov_degrees != 58 ||
                !duplicate_selection() ||
                document.find(selection_state.primary) is not { camera: true, main_camera: false, camera_fov: 58 } ||
                scene_cameras.main(document.roots) != camera || !undo_history.try_undo()) {
                return selftest_failure("camera pose, FOV, or main camera duplication failed");
            }
            camera_command("camera_view");
            if (!orbit_camera.try_get(out var preview) || preview.vertical_fov_degrees != 58 ||
                MathF.Abs(preview.yaw_degrees - authored.yaw_degrees) > 0.001f) {
                return selftest_failure("camera preview did not reach the runtime");
            }
            if (!add_node("damaged-helmet", new vector_3 { x = -0.6f, y = 1.55f }) ||
                document.find(selection_state.primary) is not { asset: "damaged-helmet" } first ||
                !add_node("damaged-helmet", new vector_3 { x = 0.6f, y = 1.55f }) ||
                document.find(selection_state.primary) is not { asset: "damaged-helmet" } second) {
                return selftest_failure("could not place two material instances");
            }
            refresh_camera_material_details(second);
            material_fields["material_tint"] = "0.07, 0.3, 1";
            material_fields["material_roughness"] = "0.35";
            apply_material_override(reset: false);
            if (second.material is not { } override_value || override_value.tint.x != 0.07f ||
                override_value.roughness != 0.35f || first.material.HasValue ||
                !undo_history.try_undo() || second.material.HasValue ||
                !undo_history.try_redo() || second.material != override_value) {
                return selftest_failure("the material override was not confined to the selected instance");
            }
            apply_material_override(reset: true);
            if (second.material.HasValue || !undo_history.try_undo() || second.material != override_value) {
                return selftest_failure("material reset or undo failed");
            }

            var path = Path.Combine(managed_bootstrap.project_directory, "scenes", "authoring.scene.toml");
            if (!document_io.try_save(document, path, out var error) ||
                !document_io.try_load(path, out var loaded, out error) || loaded == null ||
                !same_document(document, loaded, out error) ||
                scene_cameras.main(loaded.roots)?.uid != camera.uid) {
                return selftest_failure($"camera and material round trip failed: {error}");
            }
            host_logger.write("Editor selftest: camera pose, FOV, main camera and instance material round trip passed");
            return script_status.success;
        }

        private script_status run_lights_selftest() {
            if (document == null || undo_history == null || scene_lights.count(document.roots) != 0 ||
                !handle_menu_click("menu_add_point_light") ||
                document.find(selection_state?.primary ?? Guid.Empty) is not { light: not null } first) {
                return selftest_failure("failed to add a point light to the fixture");
            }

            bool configure(document_node node, string colour, string intensity, string range) {
                refresh_point_light_details(node);
                light_fields["light_colour"] = colour;
                light_fields["light_intensity"] = intensity;
                light_fields["light_range"] = range;
                apply_point_light();
                return node.light is { } light && light != point_light_component.default_light;
            }

            if (!configure(first, "1, 0.25, 0.05", "30", "6") ||
                !handle_menu_click("menu_add_point_light") ||
                document.find(selection_state?.primary ?? Guid.Empty) is not { light: not null } second ||
                !configure(second, "0.05, 0.4, 1", "30", "6")) {
                return selftest_failure("failed to configure two distinct point lights");
            }
            var original_x = second.local.position.x;
            if (!apply_detail_change(detail_field.position_x, "2.5") ||
                scene_lights.collect(document)[1].position.x != 2.5F ||
                !undo_history.try_undo() || scene_lights.collect(document)[1].position.x != original_x ||
                !undo_history.try_redo() || scene_lights.collect(document)[1].position.x != 2.5F ||
                !duplicate_selection() || scene_lights.count(document.roots) != 3 ||
                !undo_history.try_undo() || scene_lights.count(document.roots) != 2 ||
                !delete_selected() || scene_lights.count(document.roots) != 1 ||
                !undo_history.try_undo() || scene_lights.count(document.roots) != 2) {
                return selftest_failure("point light movement, duplication, deletion or undo failed");
            }

            var path = Path.Combine(managed_bootstrap.project_directory, "scenes", "lights.scene.toml");
            if (!document_io.try_save(document, path, out var error) ||
                !document_io.try_load(path, out var loaded, out error) || loaded == null ||
                !same_document(document, loaded, out error) || scene_lights.collect(loaded).Length != 2) {
                return selftest_failure($"point lights did not reload: {error}");
            }
            host_logger.write("Editor selftest: point light creation, editing, movement, undo, duplication, deletion and round trip passed");
            return script_status.success;
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
                    before.visible != after.visible || before.flags != after.flags ||
                    !before.script_parameters.SequenceEqual(after.script_parameters)) {
                    mismatch = $"node '{before.name}' properties changed on reload";
                    return false;
                }
                if (before.children.Count != after.children.Count) {
                    mismatch = $"node '{before.name}' hierarchy changed on reload";
                    return false;
                }
                if (before.camera != after.camera || before.main_camera != after.main_camera ||
                    before.camera_fov != after.camera_fov ||
                    before.material != after.material) {
                    mismatch = $"node '{before.name}' camera or material changed on reload";
                    return false;
                }
                if (before.light != after.light) {
                    mismatch = $"node '{before.name}' point light changed on reload";
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
