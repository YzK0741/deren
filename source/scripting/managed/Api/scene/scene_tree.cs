using System;
using deren.scripting;

namespace deren {
    /// <summary>
    /// A scene tree owned by managed code. Build the tree, activate it once to make it the
    /// rendered scene, then bind assets and create scripts.
    ///
    /// Every bool method has an <c>out script_status</c> overload: a dead tree or a bad argument
    /// reports <see cref="script_status.invalid_argument"/>, a stale or foreign node reports
    /// <see cref="script_status.invalid_node"/>, and a refused host call reports what it answered.
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
        public static bool try_create(out scene_tree? tree) {
            return try_create(out tree, out _);
        }

        public static unsafe bool try_create(out scene_tree? tree, out script_status status) {
            tree = null;
            ulong handle = 0;
            status = scene_api.create_tree(&handle);
            if (status != script_status.success) {
                return false;
            }
            if (handle == 0) {
                status = script_status.script_failure;
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
            return try_activate(out _);
        }

        public bool try_activate(out script_status status) {
            if (handle == 0) {
                status = script_status.invalid_argument;
                return false;
            }
            if (active) {
                status = script_status.scene_exists;
                return false;
            }
            status = scene_api.activate(handle);
            if (status != script_status.success) {
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
            return destroy(out _);
        }

        public bool destroy(out script_status status) {
            if (handle == 0 || active) {
                status = script_status.invalid_argument;
                return false;
            }

            status = scene_api.destroy_tree(handle);
            handle = 0;
            return status == script_status.success;
        }

        /// <summary>
        /// Appends a node under <paramref name="parent"/>. Pass a default node_id to create a root.
        /// The returned handle is a process-wide scene node id and remains valid until the node or
        /// one of its ancestors is destroyed.
        /// </summary>
        public bool try_create_node(node_id parent, string? name, in transform_value local, out node_id node) {
            return try_create_node(parent, name, local, out node, out _);
        }

        public unsafe bool try_create_node(node_id parent, string? name, in transform_value local, out node_id node,
                                           out script_status status) {
            node = default;
            if (handle == 0) {
                status = script_status.invalid_argument;
                return false;
            }
            using var text = native_string.create(name);
            var copy = local;
            node_handle created;
            status = scene_api.create_tree_node(handle, parent.handle, text.view, &copy, &created);
            if (status != script_status.success) {
                return false;
            }
            if (created.value == 0) {
                status = script_status.script_failure;
                return false;
            }
            node = new node_id(created);
            return true;
        }

        /// <summary>
        /// Destroys a node and its whole subtree. Returns false for a stale or foreign handle.
        /// </summary>
        public bool destroy_node(node_id node) {
            return destroy_node(node, out _);
        }

        public bool destroy_node(node_id node, out script_status status) {
            if (handle == 0) {
                status = script_status.invalid_argument;
                return false;
            }
            if (!node.is_valid) {
                status = script_status.invalid_node;
                return false;
            }
            status = scene_api.destroy_tree_node(handle, node.handle);
            return status == script_status.success;
        }

        /// <summary>
        /// Instantiates a loaded asset as a subtree under <paramref name="parent"/> and returns
        /// its instance root. Pass a default node_id to attach it as a root. The offset is the
        /// instance root's local transform. The copied node hierarchy stays valid after the asset
        /// handle is released.
        /// </summary>
        public bool try_bind_asset(node_id parent, asset_handle asset, in transform_value offset, out node_id root) {
            return try_bind_asset(parent, asset, offset, out root, out _);
        }

        public unsafe bool try_bind_asset(node_id parent, asset_handle asset, in transform_value offset, out node_id root,
                                          out script_status status) {
            root = default;
            if (handle == 0) {
                status = script_status.invalid_argument;
                return false;
            }
            if (!asset.is_valid) {
                status = script_status.asset_invalid;
                return false;
            }
            var copy = offset;
            node_handle created;
            status = scene_api.bind_tree_asset(handle, parent.handle, asset.value, &copy, &created);
            if (status != script_status.success) {
                return false;
            }
            if (created.value == 0) {
                status = script_status.script_failure;
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
        public bool try_set_pipeline(node_id node, string pipeline) {
            return try_set_pipeline(node, pipeline, out _);
        }

        public bool try_set_pipeline(node_id node, string pipeline, out script_status status) {
            if (handle == 0 || string.IsNullOrEmpty(pipeline)) {
                status = script_status.invalid_argument;
                return false;
            }
            if (!node.is_valid) {
                status = script_status.invalid_node;
                return false;
            }
            using var text = native_string.create(pipeline);
            status = scene_api.assign_pipeline(handle, node.handle, text.view);
            return status == script_status.success;
        }

        /// <summary>
        /// Reads a node's local transform from this tree. Returns false for a stale or foreign node.
        /// </summary>
        public bool try_get_transform(node_id node, out transform_value value) {
            return try_get_transform(node, out value, out _);
        }

        public unsafe bool try_get_transform(node_id node, out transform_value value, out script_status status) {
            value = default;
            if (handle == 0) {
                status = script_status.invalid_argument;
                return false;
            }
            if (!node.is_valid) {
                status = script_status.invalid_node;
                return false;
            }
            transform_value result;
            status = scene_api.query_tree_transform(handle, node.handle, &result);
            if (status != script_status.success) {
                return false;
            }
            value = result;
            return true;
        }

        /// <summary>
        /// Writes a node's local transform in this tree. Returns false for a stale node or an
        /// invalid pose.
        /// </summary>
        public bool try_set_transform(node_id node, in transform_value value) {
            return try_set_transform(node, value, out _);
        }

        public unsafe bool try_set_transform(node_id node, in transform_value value, out script_status status) {
            if (handle == 0) {
                status = script_status.invalid_argument;
                return false;
            }
            if (!node.is_valid) {
                status = script_status.invalid_node;
                return false;
            }
            var copy = value;
            status = scene_api.assign_tree_transform(handle, node.handle, &copy);
            return status == script_status.success;
        }

        /// <summary>
        /// Shows or hides a node and its whole subtree. Returns false for a stale or foreign node.
        /// </summary>
        public bool try_set_visible(node_id node, bool visible) {
            return try_set_visible(node, visible, out _);
        }

        public bool try_set_visible(node_id node, bool visible, out script_status status) {
            if (handle == 0) {
                status = script_status.invalid_argument;
                return false;
            }
            if (!node.is_valid) {
                status = script_status.invalid_node;
                return false;
            }
            status = scene_api.assign_tree_visible(handle, node.handle, visible);
            return status == script_status.success;
        }

        /// <summary>
        /// Applies per-object material factors to a node and its whole subtree, replacing whatever the
        /// imported materials carried. Pass <paramref name="enabled"/>=false to hand the subtree back
        /// to its imported materials. Returns false for a stale or foreign node, or a factor the host
        /// rejects (a non-finite value, an environment lane above 3).
        /// </summary>
        public bool try_set_material_override(node_id node, in material_override value, bool enabled = true) {
            return try_set_material_override(node, value, enabled, out _);
        }

        public unsafe bool try_set_material_override(node_id node, in material_override value, bool enabled,
                                                     out script_status status) {
            if (handle == 0) {
                status = script_status.invalid_argument;
                return false;
            }
            if (!node.is_valid) {
                status = script_status.invalid_node;
                return false;
            }
            material_override_desc record;
            value.write_to(&record, enabled);
            status = scene_api.assign_material_override(handle, node.handle, &record);
            return status == script_status.success;
        }

        /// <summary>
        /// Reads how many children a node has. Pass a default <paramref name="node"/> to count the
        /// tree's ROOTS instead, which together with <see cref="try_get_child"/> walks the whole graph
        /// from one starting handle. Returns false for a stale or foreign node.
        /// </summary>
        public bool try_get_child_count(node_id node, out uint count) {
            return try_get_child_count(node, out count, out _);
        }

        public bool try_get_child_count(node_id node, out uint count, out script_status status) {
            count = 0;
            if (handle == 0) {
                status = script_status.invalid_argument;
                return false;
            }
            // A default handle IS the tree's root list (see scene_api::node_child_count), so there is no
            // node to validate here.
            unsafe {
                uint result;
                status = scene_api.query_child_count(handle, node.handle, &result);                if (status != script_status.success) {
                    return false;
                }
                count = result;
            }
            return true;
        }

        /// <summary>
        /// Reads the child at <paramref name="index"/>; a default <paramref name="node"/> reads the
        /// tree's root at that index. Returns false when the index is past the end.
        /// </summary>
        public bool try_get_child(node_id node, uint index, out node_id child) {
            return try_get_child(node, index, out child, out _);
        }

        public bool try_get_child(node_id node, uint index, out node_id child, out script_status status) {
            child = default;
            if (handle == 0) {
                status = script_status.invalid_argument;
                return false;
            }
            unsafe {
                node_handle result;
                status = scene_api.query_child(handle, node.handle, index, &result);
                if (status != script_status.success) {
                    return false;
                }
                if (result.value == 0) {
                    status = script_status.script_failure;
                    return false;
                }
                child = new node_id(result);
            }
            return true;
        }

        /// <summary>
        /// Counts the tree's roots.
        /// </summary>
        public bool try_get_root_count(out uint count) {
            return try_get_child_count(default, out count);
        }

        /// <summary>
        /// Reads the root at <paramref name="index"/>.
        /// </summary>
        public bool try_get_root(uint index, out node_id root) {
            return try_get_child(default, index, out root);
        }

        /// <summary>
        /// Reads the name a node was created with. The bytes belong to the node, so they are copied out
        /// before this returns. Returns false for a stale or foreign node.
        /// </summary>
        public bool try_get_name(node_id node, out string? name) {
            return try_get_name(node, out name, out _);
        }

        public unsafe bool try_get_name(node_id node, out string? name, out script_status status) {
            name = null;
            if (handle == 0) {
                status = script_status.invalid_argument;
                return false;
            }
            if (!node.is_valid) {
                status = script_status.invalid_node;
                return false;
            }
            utf8_view view;
            status = scene_api.query_node_name(handle, node.handle, &view);
            if (status != script_status.success) {
                return false;
            }
            name = view.length == 0 ? string.Empty : System.Text.Encoding.UTF8.GetString(view.data, checked((int)view.length));
            return true;
        }

        /// <summary>
        /// Reads the opaque 32-bit word the game attached to a node (four are left alone by every host
        /// entry). They are free storage, not a host feature: nothing in the renderer reads them.
        /// </summary>
        public bool try_get_flags(node_id node, out uint flags) {
            return try_get_flags(node, out flags, out _);
        }

        public bool try_get_flags(node_id node, out uint flags, out script_status status) {
            flags = 0;
            if (handle == 0) {
                status = script_status.invalid_argument;
                return false;
            }
            if (!node.is_valid) {
                status = script_status.invalid_node;
                return false;
            }
            unsafe {
                uint result;
                status = scene_api.query_node_flags(handle, node.handle, &result);
                if (status != script_status.success) {
                    return false;
                }
                flags = result;
            }
            return true;
        }

        /// <summary>
        /// Writes the bits of <paramref name="flags"/> selected by <paramref name="mask"/>, leaving the
        /// others as they were. Pass a full mask to replace the whole word.
        /// </summary>
        public bool try_set_flags(node_id node, uint flags, uint mask = uint.MaxValue) {
            return try_set_flags(node, flags, mask, out _);
        }

        public bool try_set_flags(node_id node, uint flags, uint mask, out script_status status) {
            if (handle == 0) {
                status = script_status.invalid_argument;
                return false;
            }
            if (!node.is_valid) {
                status = script_status.invalid_node;
                return false;
            }
            status = scene_api.assign_node_flags(handle, node.handle, flags, mask);
            return status == script_status.success;
        }

        /// <summary>
        /// Moves a node under a new parent; a default <paramref name="parent"/> makes it a root. With
        /// <paramref name="keep_transform"/> (the default) the node stays where it is in the world and
        /// its local transform is recomputed, so a scene graph can be regrouped without anything
        /// moving. The move is refused for a stale or foreign handle, for a parent inside the node's own
        /// subtree, and for a target with a collapsed transform.
        /// </summary>
        public bool try_reparent(node_id node, node_id parent, bool keep_transform = true) {
            return try_reparent(node, parent, keep_transform, out _);
        }

        public bool try_reparent(node_id node, node_id parent, bool keep_transform, out script_status status) {
            if (handle == 0) {
                status = script_status.invalid_argument;
                return false;
            }
            if (!node.is_valid) {
                status = script_status.invalid_node;
                return false;
            }
            status = scene_api.assign_parent(handle, node.handle, parent.handle, keep_transform ? (byte)1 : (byte)0);
            return status == script_status.success;
        }

        public void Dispose() {
            destroy();
        }
    }
}
