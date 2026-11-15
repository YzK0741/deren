using System;
using deren.scripting;

namespace deren {
    /// <summary>
    /// A scene tree owned by managed code. Build the tree, activate it once to make it the
    /// rendered scene, then bind assets and create scripts.
    /// </summary>
    public sealed class scene_tree : IDisposable {
        private ulong handle;
        private bool active;

        private scene_tree(ulong handle) {
            this.handle = handle;
        }

        /// <summary>
        /// True while this object still holds a native tree.
        /// </summary>
        public bool is_valid => handle != 0;

        /// <summary>
        /// True after this tree has been activated as the rendered scene.
        /// </summary>
        public bool is_active => handle != 0 && active;

        /// <summary>
        /// Creates an empty tree. Returns false when the native scene module is unavailable.
        /// </summary>
        public static unsafe bool try_create(out scene_tree? tree) {
            tree = null;
            ulong handle = 0;
            if (scene_api.create_tree(&handle) != script_status.success || handle == 0) {
                return false;
            }
            tree = new scene_tree(handle);
            return true;
        }

        /// <summary>
        /// Makes this tree the scene rendered by the host. Activation is only valid while the
        /// project scene is starting and succeeds at most once per tree.
        /// </summary>
        public bool try_activate() {
            if (handle == 0 || active) {
                return false;
            }
            if (scene_api.activate(handle) != script_status.success) {
                return false;
            }
            active = true;
            return true;
        }

        internal bool detach() {
            if (handle == 0 || !active) {
                return false;
            }
            if (scene_api.deactivate(handle) != script_status.success) {
                return false;
            }
            active = false;
            return true;
        }

        /// <summary>
        /// Destroys an inactive managed tree. An active tree belongs to the running scene and is
        /// released by the host after scripts stop.
        /// </summary>
        public bool destroy() {
            if (handle == 0 || active) {
                return false;
            }
            var status = scene_api.destroy_tree(handle);
            handle = 0;
            return status == script_status.success;
        }

        /// <summary>
        /// Appends a node under <paramref name="parent"/>. Pass a default node_id to create a root.
        /// The returned handle is a process-wide scene node id and remains valid until the node or
        /// one of its ancestors is destroyed.
        /// </summary>
        public unsafe bool try_create_node(node_id parent, string? name, in transform_value local, out node_id node) {
            node = default;
            if (handle == 0) {
                return false;
            }
            using var text = native_string.create(name);
            var copy = local;
            node_handle created;
            var status = scene_api.create_tree_node(handle, parent.handle, text.view, &copy, &created);
            if (status != script_status.success || created.value == 0) {
                return false;
            }
            node = new node_id(created);
            return true;
        }

        /// <summary>
        /// Destroys a node and its whole subtree. Returns false for a stale or foreign handle.
        /// </summary>
        public bool destroy_node(node_id node) {
            if (handle == 0 || !node.is_valid) {
                return false;
            }
            return scene_api.destroy_tree_node(handle, node.handle) == script_status.success;
        }

        /// <summary>
        /// Instantiates a loaded asset as a subtree under <paramref name="parent"/> and returns
        /// its instance root. Pass a default node_id to attach it as a root. The offset is the
        /// instance root's local transform. The copied node hierarchy stays valid after the asset
        /// handle is released.
        /// </summary>
        public unsafe bool try_bind_asset(node_id parent, asset_handle asset, in transform_value offset, out node_id root) {
            root = default;
            if (handle == 0 || !asset.is_valid) {
                return false;
            }
            var copy = offset;
            node_handle created;
            if (scene_api.bind_tree_asset(handle, parent.handle, asset.value, &copy, &created) != script_status.success ||
                created.value == 0) {
                return false;
            }
            root = new node_id(created);
            return true;
        }

        /// <summary>
        /// Switches every imported primitive under a <see cref="try_bind_asset"/> root to one of the
        /// pipelines that asset declares. Returns false for an unknown name, a node that is not a
        /// bind root, or a tree that is not the active scene. Hidden subtrees switch too, so they
        /// render with the new pipeline once shown again.
        /// </summary>
        public unsafe bool try_set_pipeline(node_id node, string pipeline) {
            if (handle == 0 || !node.is_valid || string.IsNullOrEmpty(pipeline)) {
                return false;
            }
            using var text = native_string.create(pipeline);
            return scene_api.assign_pipeline(handle, node.handle, text.view) == script_status.success;
        }

        /// <summary>
        /// Reads a node's local transform from this tree. Returns false for a stale or foreign node.
        /// </summary>
        public unsafe bool try_get_transform(node_id node, out transform_value value) {
            value = default;
            if (handle == 0 || !node.is_valid) {
                return false;
            }
            transform_value result;
            if (scene_api.query_tree_transform(handle, node.handle, &result) != script_status.success) {
                return false;
            }
            value = result;
            return true;
        }

        /// <summary>
        /// Writes a node's local transform in this tree. Returns false for a stale node or an
        /// invalid pose.
        /// </summary>
        public unsafe bool try_set_transform(node_id node, in transform_value value) {
            if (handle == 0 || !node.is_valid) {
                return false;
            }
            var copy = value;
            return scene_api.assign_tree_transform(handle, node.handle, &copy) == script_status.success;
        }

        /// <summary>
        /// Shows or hides a node and its whole subtree. Returns false for a stale or foreign node.
        /// </summary>
        public bool try_set_visible(node_id node, bool visible) {
            if (handle == 0 || !node.is_valid) {
                return false;
            }
            return scene_api.assign_tree_visible(handle, node.handle, visible) == script_status.success;
        }

        /// <summary>
        /// Applies per-object material factors to a node and its whole subtree, replacing whatever the
        /// imported materials carried. Pass <paramref name="enabled"/>=false to hand the subtree back
        /// to its imported materials. Returns false for a stale or foreign node, or a factor the host
        /// rejects (a non-finite value, an environment lane above 3).
        /// </summary>
        public unsafe bool try_set_material_override(node_id node, in material_override value, bool enabled = true) {
            if (handle == 0 || !node.is_valid) {
                return false;
            }
            material_override_desc record;
            value.write_to(&record, enabled);
            return scene_api.assign_material_override(handle, node.handle, &record) == script_status.success;
        }

        public void Dispose() {
            destroy();
        }
    }
}
