using System;
using deren.scripting;

namespace deren {
    // Single-word lowercase type names are part of deren's public naming convention.
    public abstract class script_base {
        private node_handle node;
        private int scene_thread;
        private Func<string, bool>? request_scene;
        internal Func<node_id, bool, bool>? play_audio_source;
        internal Func<node_id, bool>? stop_audio_source;
        internal collision_move_handler? move_with_collision;

        protected scene_access scene => new(this);

        /// <summary>The scene node this script_base instance is attached to.</summary>
        protected node_id self => new(node, this);

        internal node_handle self_handle => node;

        internal ulong scene_id { get; private set; }

        public virtual void on_create() {
        }

        public virtual void on_update(float delta_time) {
        }

        /// <summary>A marked trigger target entered this node's authored box.</summary>
        public virtual void on_trigger_enter(node_id other) {
        }

        /// <summary>A marked trigger target left this node's authored box.</summary>
        public virtual void on_trigger_exit(node_id other) {
        }

        public virtual void on_destroy() {
        }

        /// <summary>During an update or trigger callback, queues a project-relative scene for the next frame.
        /// Returns false when switching is unavailable, the path is invalid, or a request is already queued.
        /// The current scene remains active if loading the requested scene fails.</summary>
        protected bool try_change_scene(string path) {
            ensure_bound();
            return request_scene?.Invoke(path) == true;
        }

        protected transform_value transform {
            get {
                if (!self.try_get_transform(out var value)) {
                    throw new InvalidOperationException("get_transform failed.");
                }
                return value;
            }
            set {
                if (!self.try_set_transform(value)) {
                    throw new InvalidOperationException("set_transform failed.");
                }
            }
        }

        internal void attach(ulong id, node_handle handle) => attach(id, handle, null);

        internal void attach(ulong id, node_handle handle, Func<string, bool>? change_scene) {
            request_scene = change_scene;
            scene_id = id;
            node = handle;
            scene_thread = Environment.CurrentManagedThreadId;
        }

        internal void detach() {
            node = default;
            scene_id = 0;
            request_scene = null;
            play_audio_source = null;
            stop_audio_source = null;
            move_with_collision = null;
        }

        internal void ensure_bound() {
            if (Environment.CurrentManagedThreadId != scene_thread || node.value == 0) {
                throw new InvalidOperationException("Scene access requires an attached script on its scene thread.");
            }
        }
    }
}
