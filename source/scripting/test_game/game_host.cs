using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using deren;
using deren.scripting;

namespace game {
    /// <summary>
    /// The project owns the whole managed side: it chooses assets from the library, builds and
    /// activates its scene tree, binds renderable assets, attaches scripts, and registers the
    /// frame/shutdown callbacks. The managed host only loads this type and forwards calls.
    /// </summary>
    internal sealed unsafe class game_host : game_entry {
        private static readonly Dictionary<string, int> action_indices = new() {
            ["move_left"] = 0,
            ["move_right"] = 1,
            ["toggle_spin"] = 2,
            ["spawn_helmet"] = 3
        };

        private static game_host? instance;
        private readonly List<script_base> scripts = [];
        private scene_tree? tree;

        public script_status initialize(native_api* api, managed_api* scripts) {
            if (api == null || scripts == null || instance != null) {
                return script_status.invalid_argument;
            }
            var module_status = scene_api.initialize(api);
            if (module_status != script_status.success) {
                return module_status;
            }
            if (!scene_tree.try_create(out var created) || created == null) {
                return script_status.script_failure;
            }
            tree = created;
            try {
                if (!asset_manager.try_load("damaged-helmet", out var helmet) || !helmet.is_valid) {
                    throw new InvalidOperationException("cannot load 'damaged-helmet'");
                }
                try {
                    var left = create_root("Left Helmet", -1.2f);
                    var right = create_root("Right Helmet", 1.2f);
                    var spawned = create_root("Spawned Helmet", 0.0f);
                    if (!tree.try_activate()) {
                        throw new InvalidOperationException("cannot activate the managed scene");
                    }
                    bind_helmet(left, helmet);
                    bind_helmet(right, helmet);
                    bind_helmet(spawned, helmet);
                    if (!tree.try_set_visible(spawned, false)) {
                        throw new InvalidOperationException("cannot hide the spawn root");
                    }
                    attach(new move_helmet(), left);
                    attach(new spin_helmet(), right);
                } finally {
                    if (!asset_manager.release(helmet)) {
                        throw new InvalidOperationException("cannot release 'damaged-helmet'");
                    }
                }
            } catch (Exception error) {
                api->write_log(System.Text.Encoding.UTF8.GetBytes(
                    $"Game: scene initialization failed: {error.GetBaseException().Message}"));
                cleanup();
                return script_status.script_failure;
            }

            instance = this;
            scripts->update = &update;
            scripts->stop = &stop;
            api->write_log("Game: helmet scene ready"u8);
            return script_status.success;
        }

        private node_id create_root(string name, float x) {
            var local = new transform_value { scale = new vector_3 { x = 1.0f, y = 1.0f, z = 1.0f } };
            local.position.x = x;
            if (!tree!.try_create_node(default, name, local, out var root) || !root.is_valid) {
                throw new InvalidOperationException($"cannot create root '{name}'");
            }
            return root;
        }

        private void bind_helmet(node_id root, asset_handle helmet) {
            var offset = new transform_value { scale = new vector_3 { x = 1.0f, y = 1.0f, z = 1.0f } };
            if (!tree!.try_bind_asset(root, helmet, offset, out var bound) || !bound.is_valid) {
                throw new InvalidOperationException("cannot bind 'damaged-helmet'");
            }
        }

        private void attach(script_base script, node_id node) {
            script.attach(0, node.handle);
            scripts.Add(script);
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static script_status update(frame_info* frame) {
            if (instance == null || frame == null) {
                return script_status.invalid_argument;
            }
            if (frame->input == null) {
                return script_status.success;
            }
            input_state.begin_frame(action_indices, frame->input);
            try {
                for (var index = 0; index < instance.scripts.Count; ++index) {
                    var script = instance.scripts[index];
                    if (script.updates_enabled) {
                        script.on_update(frame->delta_time);
                    }
                }
                return script_status.success;
            } catch (Exception error) {
                managed_bootstrap.core.write_log(System.Text.Encoding.UTF8.GetBytes(
                    $"Game: update failed: {error.GetBaseException().Message}"));
                return script_status.script_failure;
            } finally {
                input_state.end_frame();
            }
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static script_status stop() {
            if (instance == null) {
                return script_status.success;
            }
            var current = instance;
            instance = null;
            try {
                for (var index = current.scripts.Count - 1; index >= 0; --index) {
                    current.scripts[index].on_destroy();
                }
            } catch (Exception error) {
                managed_bootstrap.core.write_log(System.Text.Encoding.UTF8.GetBytes(
                    $"Game: shutdown failed: {error.GetBaseException().Message}"));
                current.cleanup();
                return script_status.script_failure;
            }
            current.cleanup();
            return script_status.success;
        }

        private void cleanup() {
            scripts.Clear();
            if (tree != null) {
                tree.detach();
                tree.destroy();
                tree = null;
            }
        }
    }
}
