using deren.scripting;

namespace deren {
    public readonly record struct collision_move(vector_3 displacement, bool blocked);

    internal delegate bool collision_move_handler(node_id node, vector_3 displacement, float radius, out collision_move result);

    public readonly struct scene_access {
        private readonly script_base? owner;

        internal scene_access(script_base owner) {
            this.owner = owner;
        }

        /// <summary>The scene node this script_base is attached to.</summary>
        public node_id self => owner is null ? default : new node_id(owner.self_handle, owner);

        /// <summary>Plays an authored audio source in this scene. By default playback restarts at the beginning.</summary>
        public bool try_play_audio_source(node_id source, bool restart = true) {
            if (owner == null || !source.is_valid) { return false; }
            owner.ensure_bound();
            return owner.play_audio_source?.Invoke(source, restart) == true;
        }

        /// <summary>Stops an authored audio source and resets its playback position.</summary>
        public bool try_stop_audio_source(node_id source) {
            if (owner == null || !source.is_valid) { return false; }
            owner.ensure_bound();
            return owner.stop_audio_source?.Invoke(source) == true;
        }

        /// <summary>Moves an authored node by a world-space displacement, sweeping its origin
        /// against the scene's static boxes. Radius adds a world-space clearance around the origin.</summary>
        public bool try_move(node_id node, vector_3 displacement, out collision_move result, float radius = 0) {
            result = default;
            if (owner == null || !node.is_valid) { return false; }
            owner.ensure_bound();
            return owner.move_with_collision?.Invoke(node, displacement, radius, out result) == true;
        }

        public unsafe bool try_spawn(string asset_id, transform_value pose, out node_id target) {
            return try_spawn(asset_id, pose, out target, out _);
        }

        public unsafe bool try_spawn(string asset_id, transform_value pose, out node_id target,
                                     out script_status status) {
            target = default;
            if (owner == null || string.IsNullOrEmpty(asset_id)) {
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
            // A SUCCESS MUST CARRY A NODE: handle 0 is not a spawn result but the ABI's "nothing
            // written", and a node_id built from it would be permanently invalid while every later
            // call reported invalid_node instead of the real failure. Same rule as try_create_node.
            if (handle.value == 0) {
                status = script_status.script_failure;
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
