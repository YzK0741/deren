using System;
using deren.scripting;

namespace deren {
    // Single-word lowercase type names are part of deren's public naming convention.
    public abstract class script_behaviour {
        private native_api? callbacks;
        private node_handle node;
        private int scene_thread;
        internal bool updates_enabled = true;

        protected scene_access scene => new(this);

        /// <summary>The scene node this script_behaviour instance is attached to.</summary>
        protected node_id self => new(node, this);

        internal node_handle self_handle => node;

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

        internal void attach(native_api api, node_handle handle) {
            callbacks = api;
            node = handle;
            scene_thread = Environment.CurrentManagedThreadId;
        }

        internal void detach() {
            callbacks = null;
        }

        internal native_api bound_api() {
            if (Environment.CurrentManagedThreadId != scene_thread || callbacks is not { } api) {
                throw new InvalidOperationException("Scene access requires an attached script on its scene thread.");
            }
            return api;
        }
    }
}
