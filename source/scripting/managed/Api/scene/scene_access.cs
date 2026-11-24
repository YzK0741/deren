using deren.scripting;

namespace deren {
    /// <summary>The outcome of one <see cref="scene_access.try_move"/> call.
    /// <paramref name="displacement"/> is what the sweep actually applied, <paramref name="blocked"/> is set
    /// when a surface stopped part of the move, <paramref name="normal"/> is the surface of the last blocked
    /// step (zero when nothing was hit), and <paramref name="grounded"/> is set when a blocked step ran into
    /// ground a character could stand on. A downward probe therefore reports a landing, while a downward
    /// probe with no contact means the node is standing over an edge or in the air.</summary>
    public readonly record struct collision_move(vector_3 displacement, bool blocked, vector_3 normal, bool grounded);

    internal delegate bool collision_move_handler(node_id node, vector_3 displacement, float radius,
                                                  float half_height, float step_height, float delta_time,
                                                  out collision_move result);

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

        /// <summary>Moves an authored node by a world-space displacement, sliding it along the scene's solid
        /// boxes. Radius and half_height describe the character capsule: the swept shape is the vertical
        /// segment of that half height through the node's origin, dilated by the radius. A step_height above
        /// zero lets the move rise onto a kerb instead of stopping. The result reports the applied
        /// displacement, the last blocking surface normal, and whether the move ended against ground a
        /// character could stand on: a downward probe that lands reports grounded, and one that finds nothing
        /// means the node walked off an edge. Delta time is optional: when it is passed the move is split
        /// into slices of game time (Unreal's MaxSimulationTimeStep, 0.05 s), so a slow frame is walked in
        /// several steps instead of one long jump; without it the slices are sized from the radius.</summary>
        public bool try_move(node_id node, vector_3 displacement, out collision_move result, float radius = 0,
                             float half_height = 0, float step_height = 0, float delta_time = 0) {
            result = default;
            if (owner == null || !node.is_valid) { return false; }
            owner.ensure_bound();
            return owner.move_with_collision?.Invoke(node, displacement, radius, half_height, step_height,
                                                     delta_time, out result) == true;
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
