using System;
using deren.scripting;

namespace deren {
    // Single-word lowercase type names are part of deren's public naming convention.
    public abstract class script_base {
        private node_handle node;
        private int scene_thread;
        internal bool updates_enabled = true;

        protected scene_access scene => new(this);

        /// <summary>The scene node this script_base instance is attached to.</summary>
        protected node_id self => new(node, this);

        internal node_handle self_handle => node;

        internal ulong scene_id { get; private set; }

        public virtual void on_create() {
        }

        public virtual void on_update(float delta_time) {
        }

        public virtual void on_destroy() {
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

        internal void attach(ulong id, node_handle handle) {
            scene_id = id;
            node = handle;
            scene_thread = Environment.CurrentManagedThreadId;
        }

        internal void detach() {
            node = default;
            scene_id = 0;
        }

        internal void ensure_bound() {
            if (Environment.CurrentManagedThreadId != scene_thread || node.value == 0) {
                throw new InvalidOperationException("Scene access requires an attached script on its scene thread.");
            }
        }
    }
}
