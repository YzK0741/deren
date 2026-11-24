using System;
using deren;
using deren.scripting;

namespace editor_demo {
    public sealed class door_trigger_behaviour : script_base {
        [editor_parameter("Door sound")]
        public node_id sound;

        [editor_parameter("Second scene")]
        public bool second_scene;

        [editor_parameter("Transition delay")]
        public float delay = 0.35f;

        private float remaining = -1;

        public override void on_trigger_enter(node_id other) {
            if (!other.is_alive() || remaining >= 0) { return; }
            host_logger.write("Helmet demo: entered door trigger");
            if (scene.try_play_audio_source(sound)) {
                host_logger.write("Helmet demo: played door audio source");
            }
            remaining = MathF.Max(0, delay);
        }

        public override void on_update(float delta_time) {
            if (remaining < 0) { return; }
            remaining -= delta_time;
            if (remaining > 0) { return; }
            remaining = -1;
            var next = second_scene ? "scenes/main.scene.toml" : "scenes/hangar.scene.toml";
            if (!try_change_scene(next)) {
                host_logger.write($"Helmet demo: could not queue '{next}'");
            }
        }

        public override void on_trigger_exit(node_id other) {
            if (other.try_get_transform(out _)) { host_logger.write("Helmet demo: left door trigger"); }
        }
    }

    public sealed class helmet_behaviour : script_base {
        // The sample keeps its own gravity tuning: the managed layer only reports contacts.
        private const float gravity = -18.0f;
        // The character is a capsule: a vertical segment of this half height through the node's origin,
        // dilated by the radius. A step height lets it climb the platform edge instead of stopping at it.
        private const float walk_radius = 0.3f;
        private const float capsule_half_height = 0.9f;
        private const float step_height = 0.45f;
        // A settled character only moves a fraction of a unit per frame, which is smaller than the
        // sweep's own separation skin, so the probe always reaches this far down to feel the ground.
        private const float ground_probe = 0.02f;

        [editor_parameter("Move speed")]
        public float speed = 0.8f;

        [editor_parameter("Second scene")]
        public bool second_scene;

        [editor_parameter("Manual test sound")]
        public node_id sound;

        [editor_parameter("Door")]
        public node_id door;

        // The door is a moving solid, so its collision box follows the node this behaviour lifts.
        private const float door_reach = 0.6f;
        private const float door_speed = 1.7f;
        // The door is taller than the character's capsule, so it has to clear the capsule's top
        // (origin + half height + radius = 2.28) before the opening stops blocking.
        private const float door_travel = 4.2f;
        private float door_start_y;
        private float door_lift;
        private bool door_opening;
        private bool door_block_reported;
        private bool door_passed;

        private float start_x;
        private float fall_speed;
        private bool grounded = true;
        private bool blocked_last_frame;
        private bool sliding_last_frame;
        private bool aligned_last_frame;

        public override void on_create() {
            start_x = transform.position.x;
            if (door.try_get_transform(out var door_pose)) { door_start_y = door_pose.position.y; }
            if (orbit_camera.try_get(out var camera)) {
                camera.yaw_degrees = 0.0f;
                camera.pitch_degrees = 0.0f;
                camera.distance = second_scene ? 2.3f : 4.5f;
                camera.target = new vector_3 { y = second_scene ? -0.62f : -1.64f };
                camera.try_apply();
            }
            var lighting = project_config.lighting;
            var background = second_scene ? (r: 0.07f, g: 0.17f, b: 0.26f) : (r: 0.24f, g: 0.13f, b: 0.07f);
            post_processing.try_set_environment(lighting.environment_rotation, lighting.environment_strength, 1,
                                               background.r, background.g, background.b, lighting.environment_blur);
            host_logger.write($"Helmet demo: entered {(second_scene ? "Hangar" : "Workshop")}");
        }

        public override void on_update(float delta_time) {
            apply_gravity(delta_time);
            float x = move_axis("move_right", "move_left", delta_time);
            float z = move_axis("move_back", "move_forward", delta_time);
            if (x != 0 && z != 0) {
                x *= 0.7071068f;
                z *= 0.7071068f;
            }
            bool blocked = false;
            bool sliding = false;
            if (x != 0 || z != 0) {
                var position = transform.position;
                var step = new vector_3 {
                    x = Math.Clamp(position.x + x, start_x - 0.8f, start_x + 0.8f) - position.x,
                    z = Math.Clamp(position.z + z, -0.8f, 0.85f) - position.z,
                };
                if (scene.try_move(self, step, out var movement, radius: walk_radius, half_height: capsule_half_height, step_height: step_height, delta_time: delta_time)) {
                    blocked = movement.blocked;
                    sliding = blocked && MathF.Abs(movement.displacement.z) > 0.0001f;
                }
            }
            if (blocked && !blocked_last_frame) { host_logger.write("Helmet demo: collision blocked movement"); }
            if (sliding && !sliding_last_frame) { host_logger.write("Helmet demo: slid along the wall"); }
            blocked_last_frame = blocked;
            sliding_last_frame = sliding;
            bool aligned = MathF.Abs(transform.position.z) < 0.10f;
            if (aligned && !aligned_last_frame) { host_logger.write("Helmet demo: aligned with the doorway"); }
            aligned_last_frame = aligned;
            update_door(delta_time, blocked);
            if (input_state.was_pressed("play_tone") && scene.try_play_audio_source(sound)) {
                host_logger.write("Helmet demo: played tone");
            }
            if (input_state.was_pressed("next_scene")) {
                var next = second_scene ? "scenes/main.scene.toml" : "scenes/hangar.scene.toml";
                if (!try_change_scene(next)) {
                    host_logger.write($"Helmet demo: could not queue '{next}'");
                }
            }
        }

        /// The door opens on the interact action when the character stands in the doorway, slides up as a
        /// moving solid, and reports the moment the same opening stops blocking the walk.
        private void update_door(float delta_time, bool blocked) {
            float door_x = door.try_get_transform(out var pose) ? pose.position.x : 0.32f;
            bool near_door = MathF.Abs(transform.position.x - door_x) < door_reach && MathF.Abs(transform.position.z) < door_reach;
            if (!door_opening && input_state.was_pressed("interact") && near_door && door.is_alive()) {
                door_opening = true;
                host_logger.write("Helmet demo: opened the door");
            }
            if (!door_block_reported && !door_opening && near_door && blocked) {
                door_block_reported = true;
                host_logger.write("Helmet demo: closed door blocked the passage");
            }
            if (door_opening && door_lift < door_travel && door.try_get_transform(out var lift_pose)) {
                door_lift = MathF.Min(door_travel, door_lift + door_speed * delta_time);
                lift_pose.position.y = door_start_y + door_lift;
                if (!door.try_set_transform(lift_pose)) {
                    host_logger.write("Helmet demo: cannot move the door");
                    door_opening = false;
                }
            }
            if (door_opening && !door_passed && MathF.Abs(transform.position.x) > 0.34f) {
                door_passed = true;
                host_logger.write("Helmet demo: passed through the opened door");
            }
        }

        /// Sweeps straight down so a floor stops the fall and an edge does not. The contact result is
        /// what tells the two apart: a blocked downward move with a grounded normal is a landing, and a
        /// downward move that finds nothing means the character walked off the platform.
        private void apply_gravity(float delta_time) {
            fall_speed += gravity * delta_time;
            float drop_distance = fall_speed * delta_time;
            if (drop_distance > -ground_probe) { drop_distance = -ground_probe; }
            var drop = new vector_3 { y = drop_distance };
            if (!scene.try_move(self, drop, out var movement, radius: walk_radius, half_height: capsule_half_height, delta_time: delta_time)) {
                return;
            }
            if (movement.grounded) {
                if (!grounded) { host_logger.write("Helmet demo: landed on the floor"); }
                grounded = true;
                fall_speed = 0;
            } else if (grounded && fall_speed < 0) {
                grounded = false;
                host_logger.write("Helmet demo: walked off an edge");
            }
        }

        private float move_axis(string positive, string negative, float delta_time) {
            int held = (input_state.is_down(positive) ? 1 : 0) - (input_state.is_down(negative) ? 1 : 0);
            int pressed = (input_state.was_pressed(positive) ? 1 : 0) - (input_state.was_pressed(negative) ? 1 : 0);
            return held * speed * delta_time + pressed * 0.12f;
        }

        public override void on_destroy() {
            host_logger.write($"Helmet demo: left {(second_scene ? "Hangar" : "Workshop")}");
        }
    }
}
