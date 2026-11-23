using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using deren.scripting;

namespace deren.editor {
    /// <summary>Runs a saved scene or a Play snapshot without creating the editor UI.</summary>
    internal sealed class scene_runtime : IDisposable {
        private readonly string project_root;
        private readonly scene_tree tree;
        private readonly script_catalog catalog;
        private readonly IReadOnlyDictionary<string, int> actions;
        private readonly string? stop_marker;
        private long last_stop_check;
        private scene_document document;
        private play_behaviours? behaviours;
        private scene_triggers triggers;
        private string? pending_scene;
        private bool has_authored_lights;
        private readonly string?[] applied_environments = new string?[global_environment.slot_count - 1];
        private bool accept_scene_requests;

        private scene_runtime(string project_root, scene_tree tree, scene_document document,
                              IReadOnlyDictionary<string, int> actions, string? stop_marker) {
            this.project_root = project_root;
            this.tree = tree;
            this.document = document;
            triggers = new scene_triggers(document);
            this.actions = actions;
            this.stop_marker = stop_marker;
            catalog = new script_catalog(Path.Combine(project_root, "scripts"));
        }

        internal static bool try_start(string project_root, string path, IReadOnlyDictionary<string, int> actions,
                                       string? stop_marker, out scene_runtime? runtime, out script_status status) {
            runtime = null;
            status = script_status.script_failure;
            if (!scene_instances.try_load(project_root, path, out var loaded, out var error) || loaded == null) {
                host_logger.write($"Scene runtime: cannot load '{path}': {error}");
                return false;
            }
            if (!scene_sync.rebuild(loaded, out var tree, out status) || tree == null) {
                host_logger.write($"Scene runtime: cannot build '{path}' ({status})");
                return false;
            }
            runtime = new scene_runtime(project_root, tree, loaded, actions, stop_marker);
            return true;
        }

        internal bool apply_main_camera() {
            if (scene_cameras.main(document.roots) is not { } camera) { return false; }
            if (scene_cameras.try_pose(document, camera, out var pose) && pose.try_apply()) {
                host_logger.write($"Scene runtime: using main camera '{camera.name}' ({project_look.number(camera.camera_fov)} degrees vertical FOV)");
                return true;
            }
            host_logger.write($"Scene runtime: could not apply main camera '{camera.name}'");
            return false;
        }

        internal static Dictionary<string, int> action_indices(toml_table? manifest) {
            var result = new Dictionary<string, int>(StringComparer.Ordinal);
            if (manifest?.table("input")?.table("actions") is { } actions) {
                var names = new List<string>(actions.keys);
                // Native toml++ walks its table in UTF-8 byte order, independent of declaration order.
                names.Sort((left, right) => Encoding.UTF8.GetBytes(left).AsSpan().SequenceCompareTo(Encoding.UTF8.GetBytes(right)));
                foreach (var name in names) { result.Add(name, result.Count); }
            }
            return result;
        }

        internal unsafe script_status update(frame_info* frame) {
            if (stop_marker != null) {
                long now = Stopwatch.GetTimestamp();
                if (Stopwatch.GetElapsedTime(last_stop_check, now).TotalMilliseconds >= 100) {
                    last_stop_check = now;
                    if (File.Exists(stop_marker)) {
                        window_control.try_close();
                        return script_status.success;
                    }
                }
            }

            if (behaviours == null) {
                if (!apply_scene_lights()) { return script_status.script_failure; }
                apply_scene_environments();
                catalog.reload();
                foreach (var error in catalog.errors) { host_logger.write($"Editor play: {error}"); }
                var initial = new play_behaviours(catalog, document, tree);
                if (!initial.try_attach_nodes(document, frame->scene, queue_scene, out var message)) {
                    initial.Dispose();
                    host_logger.write(message);
                    return script_status.script_failure;
                }
                behaviours = initial;
                entered_scene();
            }

            if (pending_scene is { } next) {
                pending_scene = null;
                if (!switch_scene(next, frame->scene)) {
                    return script_status.script_failure;
                }
            }

            input_state.begin_frame(actions, frame->input, checked((int)frame->input_count));
            accept_scene_requests = true;
            try {
                behaviours.update(frame->delta_time);
                triggers.update(tree, behaviours.notify_trigger);
            } finally {
                accept_scene_requests = false;
                input_state.end_frame();
            }
            return script_status.success;
        }

        private bool queue_scene(string path) {
            if (!accept_scene_requests || pending_scene != null ||
                !try_resolve_scene(project_root, path, out var resolved) || !File.Exists(resolved)) {
                return false;
            }
            pending_scene = resolved;
            return true;
        }

        internal static bool try_resolve_scene(string root, string path, out string resolved) {
            resolved = string.Empty;
            if (string.IsNullOrWhiteSpace(path) || Path.IsPathRooted(path) ||
                !path.EndsWith(".scene.toml", StringComparison.OrdinalIgnoreCase)) {
                return false;
            }
            try {
                var candidate = Path.GetFullPath(path, root);
                var relative = Path.GetRelativePath(root, candidate);
                if (relative == ".." || relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal) ||
                    Path.IsPathRooted(relative)) {
                    return false;
                }
                resolved = candidate;
                return true;
            } catch (Exception error) when (error is ArgumentException or IOException or NotSupportedException) {
                return false;
            }
        }

        private bool switch_scene(string next, ulong scene) {
            if (!scene_instances.try_load(project_root, next, out var loaded, out var error) || loaded == null) {
                host_logger.write($"Scene runtime: cannot load '{next}': {error}");
                return true;
            }
            if (!scene_sync.try_add_document(loaded, tree, out var status)) {
                host_logger.write($"Scene runtime: cannot build '{next}' ({status})");
                return !has_runtime_roots(loaded);
            }
            var replacement = new play_behaviours(catalog, loaded, tree);
            if (!replacement.try_attach_nodes(loaded, scene, queue_scene, out var message)) {
                replacement.Dispose();
                host_logger.write(message);
                return scene_sync.remove_document(loaded, tree);
            }

            behaviours?.Dispose();
            var removed = scene_sync.remove_document(document, tree);
            document = loaded;
            triggers = new scene_triggers(document);
            behaviours = replacement;
            if (!removed) {
                host_logger.write("Scene runtime: cannot remove previous scene nodes");
                return false;
            }
            if (!apply_scene_lights()) { return false; }
            apply_scene_environments();
            apply_main_camera();
            entered_scene();
            return true;
        }

        private static bool has_runtime_roots(scene_document scene) {
            foreach (var root in scene.roots) {
                if (root.runtime.is_valid) { return true; }
            }
            return false;
        }

        private bool apply_scene_lights() {
            var lights = scene_lights.collect(document);
            if (lights.Length == 0 && !has_authored_lights) { return true; }
            if (!scene_lights.publish(lights)) { return false; }
            has_authored_lights = lights.Length != 0;
            host_logger.write($"Scene runtime: applied {lights.Length} authored point light(s)");
            return true;
        }

        private void apply_scene_environments() {
            for (int index = 0; index < applied_environments.Length; ++index) {
                string path = document.environment_slots[index] ?? string.Empty;
                string previous = applied_environments[index] ?? string.Empty;
                if (string.Equals(path, previous, StringComparison.Ordinal)) { continue; }
                int lane = index + 1;
                if (global_environment.try_set_slot_hdr(lane, scene_environment.resolve_path(path, project_root), out var status)) {
                    applied_environments[index] = path;
                    continue;
                }
                host_logger.write($"Scene runtime: cannot set environment lane {lane} '{path}' ({status})");
                if (previous.Length != 0 && global_environment.try_set_slot_hdr(lane, string.Empty)) {
                    applied_environments[index] = string.Empty;
                }
            }
        }

        private void entered_scene() {
            if (animation_player.try_get_info(out _)) { animation_player.try_play(); }
            host_logger.write($"Scene runtime: running '{document.name}'");
        }

        public void Dispose() {
            behaviours?.Dispose();
            behaviours = null;
            catalog.Dispose();
            tree.detach();
            tree.destroy();
        }
    }
}
