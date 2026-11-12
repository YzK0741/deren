using deren;

namespace game {
    public sealed class move_helmet : behaviour {
        private entity_ref? spawned_helmet;

        public override void on_update(float delta_time) {
            if (input.was_pressed("spawn_helmet")) {
                if (spawned_helmet is { } existing) {
                    if (scene.try_despawn(existing)) {
                        spawned_helmet = null;
                    }
                } else {
                    var spawn_pose = transform;
                    spawn_pose.position.x = 0.0f;
                    if (scene.try_spawn("damaged-helmet", spawn_pose, out var created)) {
                        spawned_helmet = created;
                    }
                }
            }
            var direction = (input.is_down("move_right") ? 1.0f : 0.0f) -
                (input.is_down("move_left") ? 1.0f : 0.0f);
            if (direction != 0.0f) {
                var pose = transform;
                pose.position.x += direction * 2.0f * delta_time;
                transform = pose;
            }
        }
    }

    public sealed class spin_helmet : behaviour {
        private bool spinning = true;

        public override void on_update(float delta_time) {
            if (input.was_pressed("toggle_spin")) {
                spinning = !spinning;
            }
            if (spinning) {
                var pose = transform;
                pose.rotation_degrees.y += 60.0f * delta_time;
                transform = pose;
            }
        }
    }
}
