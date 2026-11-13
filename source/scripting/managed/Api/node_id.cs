using System;
using deren.scripting;

namespace deren {
    public readonly struct node_id : IEquatable<node_id> {
        internal readonly node_handle handle;
        private readonly script_base? owner;

        internal node_id(node_handle handle, script_base? owner = null) {
            this.handle = handle;
            this.owner = owner;
        }

        /// <summary>
        /// True when this handle names a scene node. A syntactically valid handle can still be
        /// stale; use <see cref="is_alive"/> for a liveness check.
        /// </summary>
        public bool is_valid => handle.value != 0;

        /// <summary>
        /// True when the handle is still the live node. A stale or foreign handle returns false
        /// instead of throwing.
        /// </summary>
        public unsafe bool is_alive() {
            if (owner is null || !is_valid) {
                return false;
            }
            owner.ensure_bound();
            transform_value ignored;
            return scene_api.query_transform(handle, &ignored) == script_status.success;
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
            owner.ensure_bound();
            transform_value result;
            if (scene_api.query_transform(handle, &result) != script_status.success) {
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
            owner.ensure_bound();
            var copy = value;
            return scene_api.assign_transform(handle, &copy) == script_status.success;
        }

        public bool Equals(node_id other) {
            return handle.value == other.handle.value;
        }

        public override bool Equals(object? obj) {
            return obj is node_id other && Equals(other);
        }

        public override int GetHashCode() {
            return handle.value.GetHashCode();
        }

        public static bool operator ==(node_id left, node_id right) {
            return left.Equals(right);
        }

        public static bool operator !=(node_id left, node_id right) {
            return !left.Equals(right);
        }
    }
}
