using deren.scripting;

namespace deren {
    public readonly struct scene_access {
        private readonly script_base? owner;

        internal scene_access(script_base owner) {
            this.owner = owner;
        }

        /// <summary>The scene node this script_base is attached to.</summary>
        public node_id self => owner is null ? default : new node_id(owner.self_handle, owner);

        public unsafe bool try_spawn(string asset_id, transform_value pose, out node_id target) {
            return try_spawn(asset_id, pose, out target, out _);
        }

        public unsafe bool try_spawn(string asset_id, transform_value pose, out node_id target,
                                     out script_status status) {
            target = default;
            if (owner == null) {
                status = script_status.invalid_argument;
                return false;
            }
            if (string.IsNullOrEmpty(asset_id)) {
                status = script_status.invalid_argument;
                return false;
            }
            owner.ensure_bound();
            using var asset = native_string.create(asset_id);
            node_handle handle;
            status = scene_api.spawn(owner.scene_id, asset.view, &pose, &handle);
            if (status != script_status.success) {
                return false;
            }
            target = new node_id(handle, owner);
            return true;
        }

        public unsafe bool try_despawn(node_id target) {
            return try_despawn(target, out _);
        }

        public unsafe bool try_despawn(node_id target, out script_status status) {
            if (owner == null) {
                status = script_status.invalid_argument;
                return false;
            }
            if (!target.is_valid) {
                status = script_status.invalid_node;
                return false;
            }
            owner.ensure_bound();
            status = scene_api.despawn(owner.scene_id, target.handle);
            return status == script_status.success;
        }
    }
}
