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
        private string? pending_scene;
        private bool accept_scene_requests;

        private scene_runtime(string project_root, scene_tree tree, scene_document document,
                              IReadOnlyDictionary<string, int> actions, string? stop_marker) {
            this.project_root = project_root;
            this.tree = tree;
            this.document = document;
            this.actions = actions;
            this.stop_marker = stop_marker;
            catalog = new script_catalog(Path.Combine(project_root, "scripts"));
        }

        internal static bool try_start(string project_root, string path, IReadOnlyDictionary<string, int> actions,
                                       string? stop_marker, out scene_runtime? runtime, out script_status status) {
            runtime = null;
            status = script_status.script_failure;
            if (!document_io.try_load(path, out var loaded, out var error) || loaded == null) {
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
                catalog.reload();
                foreach (var error in catalog.errors) { host_logger.write($"Editor play: {error}"); }
                var initial = new play_behaviours(catalog);
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
            if (!document_io.try_load(next, out var loaded, out var error) || loaded == null) {
                host_logger.write($"Scene runtime: cannot load '{next}': {error}");
                return true;
            }
            if (!scene_sync.try_add_document(loaded, tree, out var status)) {
                host_logger.write($"Scene runtime: cannot build '{next}' ({status})");
                return !has_runtime_roots(loaded);
            }
            var replacement = new play_behaviours(catalog);
            if (!replacement.try_attach_nodes(loaded, scene, queue_scene, out var message)) {
                replacement.Dispose();
                host_logger.write(message);
                return scene_sync.remove_document(loaded, tree);
            }

            behaviours?.Dispose();
            var removed = scene_sync.remove_document(document, tree);
            document = loaded;
            behaviours = replacement;
            if (!removed) {
                host_logger.write("Scene runtime: cannot remove previous scene nodes");
                return false;
            }
            entered_scene();
            return true;
        }

        private static bool has_runtime_roots(scene_document scene) {
            foreach (var root in scene.roots) {
                if (root.runtime.is_valid) { return true; }
            }
            return false;
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
