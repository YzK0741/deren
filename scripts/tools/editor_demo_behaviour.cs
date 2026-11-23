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
        [editor_parameter("Move speed")]
        public float speed = 0.8f;

        [editor_parameter("Second scene")]
        public bool second_scene;

        [editor_parameter("Manual test sound")]
        public node_id sound;

        private float start_x;
        private bool blocked_last_frame;
        private bool sliding_last_frame;
        private bool aligned_last_frame;

        public override void on_create() {
            start_x = transform.position.x;
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
                if (scene.try_move(self, step, out var movement, radius: 0.08f)) {
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
