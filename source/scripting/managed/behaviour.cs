using System;
using System.Runtime.InteropServices;
using deren.scripting;

namespace deren {
    [StructLayout(LayoutKind.Sequential)]
    public struct vector_3 {
        public float x;
        public float y;
        public float z;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct transform_value {
        public vector_3 position;
        public vector_3 rotation_degrees;
        public vector_3 scale;
    }

    // Single-word lowercase type names are part of deren's public naming convention.
#pragma warning disable CS8981
    public abstract class behaviour {
#pragma warning restore CS8981
        private native_api? callbacks;
        private entity_handle entity;
        private int scene_thread;
        internal bool updates_enabled = true;

        public virtual void on_create() {
        }

        public virtual void on_update(float delta_time) {
        }

        public virtual void on_destroy() {
        }

        protected unsafe transform_value transform {
            get {
                var api = bound_api();
                transform_value value;
                var status = api.get_transform(api.context, entity, &value);
                if (status != script_status.success) {
                    throw new InvalidOperationException($"get_transform failed ({status}).");
                }
                return value;
            }
            set {
                var api = bound_api();
                var status = api.set_transform(api.context, entity, &value);
                if (status != script_status.success) {
                    throw new InvalidOperationException($"set_transform failed ({status}).");
                }
            }
        }

        internal void attach(native_api api, entity_handle handle) {
            callbacks = api;
            entity = handle;
            scene_thread = Environment.CurrentManagedThreadId;
        }

        internal void detach() {
            callbacks = null;
        }

        private native_api bound_api() {
            if (Environment.CurrentManagedThreadId != scene_thread || callbacks is not { } api) {
                throw new InvalidOperationException("Transform access requires an attached script on its scene thread.");
            }
            return api;
        }
    }
}
