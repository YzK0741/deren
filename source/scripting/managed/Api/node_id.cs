using System;
using deren.scripting;

namespace deren {
    public readonly struct node_id : IEquatable<node_id> {
        internal readonly node_handle handle;
        private readonly script_behaviour? owner;

        internal node_id(node_handle handle, script_behaviour? owner = null) {
            this.handle = handle;
            this.owner = owner;
        }

        /// <summary>
        /// True when this handle names a scene node. A syntactically valid handle can still be
        /// stale; use <see cref="is_alive"/> for a liveness check.
        /// </summary>
        public bool is_valid => handle.scene != 0 && handle.generation != 0 && handle.index != uint.MaxValue;

        /// <summary>
        /// True when the handle is still the live node in this scene. A stale or foreign handle
        /// returns false instead of throwing.
        /// </summary>
        public unsafe bool is_alive() {
            if (owner is null || !is_valid) {
                return false;
            }
            var api = owner.bound_api();
            transform_value ignored;
            return api.get_transform(api.context, handle, &ignored) == script_status.success;
        }

        /// <summary>
        /// Reads this node's local transform. Returns false for a stale, foreign, or otherwise
        /// unavailable node; it does not throw.
        /// </summary>
        public unsafe bool try_get_transform(out transform_value value) {
            value = default;
            if (owner is null || !is_valid) {
                return false;
            }
            var api = owner.bound_api();
            transform_value result;
            if (api.get_transform(api.context, handle, &result) != script_status.success) {
                return false;
            }
            value = result;
            return true;
        }

        /// <summary>
        /// Writes this node's local transform. Returns false when the node is unavailable or
        /// the native side rejects the pose; it does not throw.
        /// </summary>
        public unsafe bool try_set_transform(in transform_value value) {
            if (owner is null || !is_valid) {
                return false;
            }
            var api = owner.bound_api();
            var copy = value;
            return api.set_transform(api.context, handle, &copy) == script_status.success;
        }

        public bool Equals(node_id other) {
            return handle.scene == other.handle.scene &&
                handle.index == other.handle.index &&
                handle.generation == other.handle.generation;
        }

        public override bool Equals(object? obj) {
            return obj is node_id other && Equals(other);
        }

        public override int GetHashCode() {
            return HashCode.Combine(handle.scene, handle.index, handle.generation);
        }

        public static bool operator ==(node_id left, node_id right) {
            return left.Equals(right);
        }

        public static bool operator !=(node_id left, node_id right) {
            return !left.Equals(right);
        }
    }
}
