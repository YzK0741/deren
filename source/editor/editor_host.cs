using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using deren;
using deren.scripting;

namespace deren.editor {
    /// <summary>
    /// The editor project's managed entry point. It owns one scene document, builds the activated
    /// scene tree once during startup (scene_sync.rebuild), and then mirrors every edit into that
    /// tree with scene_sync.create_one/destroy_one/push - the native host only allows activating a
    /// tree while the project scene is starting, so a rebuild is never repeated after startup.
    /// </summary>
    internal sealed unsafe class editor_host : game_entry {
        private const int event_budget = 128;
        private const string rename_field_id = "rename_field";
        private const string default_scene_file = "untitled.scene.toml";

        private static editor_host? instance;
        private static string? requested_scene;
        private static bool requested_selftest;

        private scene_document? document;
        private scene_tree? tree;
        private rml_document? ui;
        private outliner_panel? outliner;
        private details_panel? details;
        private status_bar? status_line;
        private undo_stack? undo_history;
        private selection? selection_state;
        private string rename_field = string.Empty;

        public script_status preinit(startup_options* options, int argc, byte** argv) {
            if (options == null || argc < 0 || (argc > 0 && argv == null)) {
                return script_status.invalid_argument;
            }
            var args = new string[argc];
            for (var i = 0; i < argc; ++i) {
                args[i] = argv[i] == null ? string.Empty : Marshal.PtrToStringUTF8((IntPtr)argv[i]) ?? string.Empty;
            }
            for (var i = 1; i < args.Length; ++i) {
                var argument = args[i];
                var separator = argument.IndexOf('=');
                var name = separator < 0 ? argument : argument[..separator];
                var inline_value = separator < 0 ? null : argument[(separator + 1)..];
                switch (name) {
                    case "--scene":
                        requested_scene = inline_value ?? take_value(args, ref i);
                        if (string.IsNullOrEmpty(requested_scene)) {
                            requested_scene = null;
                        }
                        break;
                    case "--editor-selftest":
                        requested_selftest = true;
                        break;
                    case "--capture-windowed":
                        options->capture_windowed = 1;
                        break;
                    case "--capture-frames": {
                        var value = inline_value ?? take_value(args, ref i);
                        if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var frames)) {
                            options->capture_frames = Math.Max(0, frames);
                        }
                        break;
                    }
                }
            }
            return script_status.success;
        }

        private static string take_value(string[] args, ref int index) =>
            index + 1 < args.Length ? args[++index] : string.Empty;

        public script_status initialize(native_api* api, managed_api* scripts) {
            if (api == null || scripts == null || instance != null) {
                return script_status.invalid_argument;
            }
            var module_status = scene_api.initialize(api);
            if (module_status != script_status.success) {
                return module_status;
            }

            if (requested_scene != null) {
                if (!document_io.try_load(requested_scene, out var loaded, out var load_error) || loaded == null) {
                    host_logger.write($"Editor: cannot load scene '{requested_scene}': {load_error}");
                    return script_status.script_failure;
                }
                document = loaded;
            } else {
                document = create_default_document();
            }

            // The one and only rebuild: it creates and activates the tree, which the native host
            // requires before it will run the project (see app_scripting.cpp activate_scene).
            if (!scene_sync.rebuild(document, out var rebuilt_tree, out var rebuild_status) || rebuilt_tree == null) {
                host_logger.write($"Editor: cannot rebuild the scene tree ({rebuild_status})");
                cleanup();
                return rebuild_status != script_status.success ? rebuild_status : script_status.script_failure;
            }
            tree = rebuilt_tree;

            if (!rml_document.try_open("ui/editor.rml", "ui/LatoLatin-Regular.ttf",
                                       out var opened_document, out var ui_status) || opened_document == null) {
                host_logger.write($"Editor: cannot open ui/editor.rml ({ui_status})");
                cleanup();
                return script_status.script_failure;
            }
            ui = opened_document;
            outliner = new outliner_panel(opened_document);
            details = new details_panel(opened_document);
            status_line = new status_bar(opened_document);
            undo_history = new undo_stack();
            selection_state = new selection();
            refresh_panels(force_details: true);
            host_logger.write($"Editor: document ready ({count_nodes(document.roots)} node(s))");

            if (requested_selftest) {
                var selftest_status = run_selftest();
                if (selftest_status != script_status.success) {
                    cleanup();
                    return selftest_status;
                }
            }

            instance = this;
            scripts->update = &update;
            scripts->stop = &stop;
            host_logger.write("Editor: ready");
            return script_status.success;
        }

        private static scene_document create_default_document() {
            var created = new scene_document();
            // document_node already defaults to a fresh uid, identity transform, visible, no flags.
            created.add_root(new document_node { name = "root" });
            return created;
        }

        [UnmanagedCallersOnly]
        private static script_status update(frame_info* frame) {
            var current = instance;
            if (current == null || frame == null) {
                return script_status.invalid_argument;
            }
            try {
                current.handle_ui_events();
                current.refresh_panels();
                return script_status.success;
            } catch (Exception error) {
                host_logger.write($"Editor: update failed: {error.GetBaseException().Message}");
                return script_status.script_failure;
            }
        }

        [UnmanagedCallersOnly]
        private static script_status stop() {
            if (instance == null) {
                return script_status.success;
            }
            var current = instance;
            instance = null;
            try {
                current.cleanup();
            } catch (Exception error) {
                host_logger.write($"Editor: shutdown failed: {error.GetBaseException().Message}");
                return script_status.script_failure;
            }
            return script_status.success;
        }

        private void handle_ui_events() {
            if (ui == null) {
                return;
            }
            // A bound keeps one busy document from monopolizing a frame.
            for (var count = 0; count < event_budget && ui.try_poll_event(out var input); ++count) {
                if (input.type == rml_event_type.change) {
                    if (input.id == rename_field_id) {
                        rename_field = input.value;
                    } else if (details_panel.try_field(input.id, out var field)) {
                        apply_detail_change(field, input.value);
                    }
                    continue;
                }
                if (input.type != rml_event_type.click) {
                    continue;
                }
                switch (input.id) {
                    case "btn_new":
                        add_node();
                        break;
                    case "btn_delete":
                        delete_selected();
                        break;
                    case "btn_save":
                        save_document();
                        break;
                    case "btn_rename":
                        apply_rename(rename_field);
                        break;
                    case "btn_undo":
                        if (undo_history?.try_undo() == true) {
                            refresh_panels(force_details: true);
                        }
                        break;
                    case "btn_redo":
                        if (undo_history?.try_redo() == true) {
                            refresh_panels(force_details: true);
                        }
                        break;
                    case "detail_visible":
                        toggle_selected_visibility();
                        break;
                    default:
                        select_row(input.id);
                        break;
                }
            }
        }

        /// <summary>A row click arrives as either the slot (#row_N) or its text (#row_N_name).</summary>
        private void select_row(string id) {
            if (!id.StartsWith("row_", StringComparison.Ordinal) || selection_state == null) {
                return;
            }
            var digits = id["row_".Length..];
            if (digits.EndsWith("_name", StringComparison.Ordinal)) {
                digits = digits[..^"_name".Length];
            }
            if (!int.TryParse(digits, NumberStyles.Integer, CultureInfo.InvariantCulture, out var row) ||
                outliner == null || !outliner.try_row_uid(row, out var uid)) {
                return;
            }
            selection_state.select(uid);
            refresh_panels();
        }

        private bool add_node() {
            if (document == null || tree == null) {
                return false;
            }
            var parent = selection_state != null && selection_state.primary != Guid.Empty
                ? document.find(selection_state.primary)
                : null;
            var node = new document_node { name = "node" };
            var index = parent != null ? parent.children.Count : document.roots.Count;
            if (!insert_document_node(parent, index, node) || !create_subtree(parent, node)) {
                // Best effort: drop whatever runtime nodes the failed subtree already created.
                destroy_runtime(node);
                remove_document_node(parent, node);
                refresh_panels();
                return false;
            }
            document.mark_dirty();
            record_undo("Create",
                () => {
                    remove_document_node(parent, node);
                    destroy_runtime(node);
                    mark_dirty_and_refresh();
                },
                () => {
                    insert_document_node(parent, index, node);
                    create_subtree(parent, node);
                    mark_dirty_and_refresh();
                });
            refresh_panels();
            return true;
        }

        private bool delete_selected() {
            if (document == null || selection_state == null) {
                return false;
            }
            var uid = selection_state.primary;
            if (uid == Guid.Empty) {
                return false;
            }
            var node = document.find(uid);
            if (node == null || !locate_node(node, out var parent, out var index)) {
                return false;
            }
            if (!destroy_runtime(node)) {
                return false;
            }
            if (!remove_document_node(parent, node)) {
                return false;
            }
            document.mark_dirty();
            record_undo("Delete",
                () => {
                    insert_document_node(parent, index, node);
                    create_subtree(parent, node);
                    mark_dirty_and_refresh();
                },
                () => {
                    remove_document_node(parent, node);
                    destroy_runtime(node);
                    mark_dirty_and_refresh();
                });
            selection_state.clear();
            refresh_panels();
            return true;
        }

        private bool save_document() {
            if (document == null) {
                return false;
            }
            var path = string.IsNullOrEmpty(document.path)
                ? Path.Combine(managed_bootstrap.project_directory, "scenes", default_scene_file)
                : document.path!;
            try {
                var directory = Path.GetDirectoryName(path);
                if (!string.IsNullOrEmpty(directory)) {
                    Directory.CreateDirectory(directory);
                }
            } catch (Exception error) {
                host_logger.write($"Editor: cannot create '{Path.GetDirectoryName(path)}': {error.GetBaseException().Message}");
                return false;
            }
            // document_io.try_save writes the file and marks the document saved itself.
            if (!document_io.try_save(document, path, out var save_error)) {
                host_logger.write($"Editor: cannot save '{path}': {save_error}");
                return false;
            }
            refresh_panels();
            host_logger.write($"Editor: saved {count_nodes(document.roots)} node(s) to '{path}'");
            return true;
        }

        /// <summary>The rename path shared by #btn_rename and --editor-selftest.</summary>
        private bool apply_rename(string name) {
            if (document == null || selection_state == null) {
                return false;
            }
            var node = selection_state.primary == Guid.Empty ? null : document.find(selection_state.primary);
            if (node == null) {
                return false;
            }
            var trimmed = name.Trim();
            if (trimmed.Length == 0 || trimmed == node.name) {
                return false;
            }
            var previous = node.name;
            node.name = trimmed;
            if (!push_runtime(node)) {
                node.name = previous;
                return false;
            }
            document.mark_dirty();
            record_undo("Rename",
                () => {
                    node.name = previous;
                    push_runtime(node);
                    mark_dirty_and_refresh();
                },
                () => {
                    node.name = trimmed;
                    push_runtime(node);
                    mark_dirty_and_refresh();
                });
            rename_field = string.Empty;
            ui?.set_value(rename_field_id, string.Empty);
            refresh_panels(force_details: true);
            return true;
        }

        /// <summary>
        /// Applies one details row: parse, mutate the document node, push it onto the runtime node and
        /// record a snapshot undo command. A refused value redraws the row from the document so the
        /// input never keeps text the document rejected.
        /// </summary>
        private bool apply_detail_change(detail_field field, string text) {
            if (document == null || selection_state == null) {
                return false;
            }
            var node = selection_state.primary == Guid.Empty ? null : document.find(selection_state.primary);
            if (node == null) {
                return false;
            }
            var before = new node_snapshot(node);
            var changed = false;
            switch (field) {
                case detail_field.name: {
                    var trimmed = text.Trim();
                    if (trimmed.Length != 0 && trimmed != node.name) {
                        node.name = trimmed;
                        changed = true;
                    }
                    break;
                }
                case detail_field.flags: {
                    if (uint.TryParse(text.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var flags) &&
                        flags != node.flags) {
                        node.flags = flags;
                        changed = true;
                    }
                    break;
                }
                default: {
                    if (!float.TryParse(text.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out var number) ||
                        !float.IsFinite(number)) {
                        break;
                    }
                    var local = node.local;
                    changed = set_lane(ref local, field, number);
                    if (changed) {
                        node.local = local;
                    }
                    break;
                }
            }
            if (!changed) {
                details?.refresh(document, selection_state, true);
                return false;
            }
            if (!push_runtime(node)) {
                restore_node(node, before, mark_dirty: false);
                details?.refresh(document, selection_state, true);
                return false;
            }
            var after = new node_snapshot(node);
            document.mark_dirty();
            record_undo(details_panel.label(field),
                () => restore_node(node, before, mark_dirty: true),
                () => restore_node(node, after, mark_dirty: true));
            details?.refresh(document, selection_state, true);
            refresh_panels();
            return true;
        }

        /// <summary>Writes one transform lane, returning false when the value was already there.</summary>
        private static bool set_lane(ref transform_value local, detail_field field, float value) {
            switch (field) {
                case detail_field.position_x: return set_axis(ref local.position.x, value);
                case detail_field.position_y: return set_axis(ref local.position.y, value);
                case detail_field.position_z: return set_axis(ref local.position.z, value);
                case detail_field.rotation_x: return set_axis(ref local.rotation_degrees.x, value);
                case detail_field.rotation_y: return set_axis(ref local.rotation_degrees.y, value);
                case detail_field.rotation_z: return set_axis(ref local.rotation_degrees.z, value);
                case detail_field.scale_x: return set_axis(ref local.scale.x, value);
                case detail_field.scale_y: return set_axis(ref local.scale.y, value);
                case detail_field.scale_z: return set_axis(ref local.scale.z, value);
                default: return false;
            }
        }

        private static bool set_axis(ref float lane, float value) {
            if (lane == value) {
                return false;
            }
            lane = value;
            return true;
        }

        /// <summary>Flips the selected node's visibility through the same undo path as the details rows.</summary>
        private bool toggle_selected_visibility() {
            if (document == null || selection_state == null) {
                return false;
            }
            var node = selection_state.primary == Guid.Empty ? null : document.find(selection_state.primary);
            if (node == null) {
                return false;
            }
            var before = new node_snapshot(node);
            node.visible = !node.visible;
            if (!push_runtime(node)) {
                restore_node(node, before, mark_dirty: false);
                return false;
            }
            var after = new node_snapshot(node);
            document.mark_dirty();
            record_undo(node.visible ? "Show Node" : "Hide Node",
                () => restore_node(node, before, mark_dirty: true),
                () => restore_node(node, after, mark_dirty: true));
            refresh_panels(force_details: true);
            return true;
        }

        /// <summary>Writes a snapshot back onto the document node and the runtime node.</summary>
        private void restore_node(document_node node, node_snapshot snapshot, bool mark_dirty) {
            node.name = snapshot.name;
            node.local = snapshot.local;
            node.visible = snapshot.visible;
            node.flags = snapshot.flags;
            push_runtime(node);
            if (mark_dirty) {
                mark_dirty_and_refresh();
            }
        }

        /// <summary>The editable values of one node, captured before and after an edit.</summary>
        private sealed class node_snapshot {
            internal node_snapshot(document_node node) {
                name = node.name;
                local = node.local;
                visible = node.visible;
                flags = node.flags;
            }

            internal readonly string name;
            internal readonly transform_value local;
            internal readonly bool visible;
            internal readonly uint flags;
        }

        private void record_undo(string label, Action undo, Action redo) {
            undo_history?.push(new delegate_undo_command(label, undo, redo));
        }

        private void mark_dirty_and_refresh() {
            document?.mark_dirty();
            refresh_panels(force_details: true);
        }

        private bool insert_document_node(document_node? parent, int index, document_node node) {
            if (document == null) {
                return false;
            }
            if (parent != null) {
                parent.children.Insert(Math.Clamp(index, 0, parent.children.Count), node);
                return true;
            }
            // Roots only expose add_root: an undone root deletion is appended at the end.
            document.add_root(node);
            return true;
        }

        private bool remove_document_node(document_node? parent, document_node node) {
            if (document == null) {
                return false;
            }
            if (parent != null) {
                return parent.children.Remove(node);
            }
            return document.remove_root(node);
        }

        /// <summary>Creates the runtime subtree top-down: a parent must exist before its children.</summary>
        private bool create_subtree(document_node? parent, document_node node) {
            if (!create_runtime(parent, node)) {
                return false;
            }
            for (var index = 0; index < node.children.Count; ++index) {
                if (!create_subtree(node, node.children[index])) {
                    return false;
                }
            }
            return true;
        }

        private bool create_runtime(document_node? parent, document_node node) {
            if (document == null || tree == null) {
                return false;
            }
            if (!scene_sync.create_one(document, tree, parent, node, out var status)) {
                host_logger.write($"Editor: cannot create node '{node.name}' in the scene ({status})");
                return false;
            }
            return true;
        }

        private bool destroy_runtime(document_node node) {
            if (document == null || tree == null) {
                return false;
            }
            if (!scene_sync.destroy_one(document, tree, node, out var status)) {
                host_logger.write($"Editor: cannot destroy node '{node.name}' in the scene ({status})");
                return false;
            }
            return true;
        }

        private bool push_runtime(document_node node) {
            if (document == null || tree == null) {
                return false;
            }
            if (!scene_sync.push(document, tree, node, out var status)) {
                host_logger.write($"Editor: cannot push node '{node.name}' to the scene ({status})");
                return false;
            }
            return true;
        }

        private bool locate_node(document_node target, out document_node? parent, out int index) {
            parent = null;
            index = -1;
            if (document == null) {
                return false;
            }
            for (var root_index = 0; root_index < document.roots.Count; ++root_index) {
                if (document.roots[root_index] == target) {
                    index = root_index;
                    return true;
                }
            }
            return locate_in_children(document.roots, target, ref parent, ref index);
        }

        private static bool locate_in_children(IReadOnlyList<document_node> nodes, document_node target,
                                               ref document_node? parent, ref int index) {
            for (var node_index = 0; node_index < nodes.Count; ++node_index) {
                var node = nodes[node_index];
                for (var child_index = 0; child_index < node.children.Count; ++child_index) {
                    if (node.children[child_index] == target) {
                        parent = node;
                        index = child_index;
                        return true;
                    }
                }
                if (locate_in_children(node.children, target, ref parent, ref index)) {
                    return true;
                }
            }
            return false;
        }

        private void refresh_panels(bool force_details = false) {
            if (document == null || selection_state == null || outliner == null || status_line == null) {
                return;
            }
            outliner.refresh(document, selection_state);
            details?.refresh(document, selection_state, force_details);
            status_line.refresh(document, selection_state);
        }

        private void cleanup() {
            ui?.Dispose();
            ui = null;
            outliner = null;
            details = null;
            status_line = null;
            undo_history?.clear();
            undo_history = null;
            selection_state = null;
            if (tree != null) {
                tree.detach();
                tree.destroy();
                tree = null;
            }
            document = null;
        }

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

                var root = document.roots.Count != 0 ? document.roots[0] : null;
                if (root == null || selection_state == null) {
                    return selftest_failure("the document has no root to edit");
                }
                selection_state.select(root.uid);
                if (!apply_rename("selftest-root") || root.name != "selftest-root") {
                    return selftest_failure("rename through the button path failed");
                }
                host_logger.write("Editor selftest: renamed the root through the button path");

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
                host_logger.write("Editor selftest: undo restored the root");

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
                host_logger.write("Editor selftest: passed");
                return script_status.success;
            } finally {
                document.path = original_path;
                try {
                    File.Delete(path);
                } catch (Exception) {
                    // The temp file is best-effort: a leftover must not hide the selftest result.
                }
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
