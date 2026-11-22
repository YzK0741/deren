using System;
using deren;
using deren.scripting;

namespace editor_demo {
    public sealed class helmet_behaviour : script_base {
        [editor_parameter("Move speed")]
        public float speed = 0.8f;

        [editor_parameter("Second scene")]
        public bool second_scene;

        private audio? tone;
        private float start_x;

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
            if (!audio.try_create_from_file("audio/tone.wav", out tone, out var status)) {
                host_logger.write($"Helmet demo: audio unavailable ({status})");
            }
            host_logger.write($"Helmet demo: entered {(second_scene ? "Hangar" : "Workshop")}");
        }

        public override void on_update(float delta_time) {
            int direction = (input_state.is_down("move_right") ? 1 : 0) -
                            (input_state.is_down("move_left") ? 1 : 0);
            int pressed = (input_state.was_pressed("move_right") ? 1 : 0) -
                          (input_state.was_pressed("move_left") ? 1 : 0);
            if (direction != 0 || pressed != 0) {
                var pose = transform;
                var next_x = pose.position.x + direction * speed * delta_time + pressed * 0.12f;
                pose.position.x = Math.Clamp(next_x, start_x - 0.65f, start_x + 0.65f);
                transform = pose;
            }
            if (input_state.was_pressed("play_tone") && tone != null) {
                if (tone.stop() && tone.play()) {
                    host_logger.write("Helmet demo: played tone");
                }
            }
            if (input_state.was_pressed("next_scene")) {
                var next = second_scene ? "scenes/main.scene.toml" : "scenes/hangar.scene.toml";
                if (!try_change_scene(next)) {
                    host_logger.write($"Helmet demo: could not queue '{next}'");
                }
            }
        }

        public override void on_destroy() {
            tone?.Dispose();
            host_logger.write($"Helmet demo: left {(second_scene ? "Hangar" : "Workshop")}");
        }
    }
}
