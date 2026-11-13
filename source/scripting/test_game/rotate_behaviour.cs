using System;
using deren;

namespace game {
    public sealed class rotate_behaviour : script_behaviour {
        private float degrees_per_second;

        public override void on_create() {
            if (!self.is_valid || scene.self != self || !self.is_alive() || default(node_id).is_alive()) {
                throw new InvalidOperationException("scene self query failed");
            }
            if (!self.try_get_transform(out var queried) ||
                queried.position.x != transform.position.x ||
                queried.position.y != transform.position.y ||
                queried.position.z != transform.position.z ||
                queried.scale.x != transform.scale.x ||
                queried.scale.y != transform.scale.y ||
                queried.scale.z != transform.scale.z) {
                throw new InvalidOperationException("scene transform read failed");
            }
            if (!self.try_set_transform(queried) ||
                default(node_id).try_get_transform(out _) ||
                default(node_id).try_set_transform(queried)) {
                throw new InvalidOperationException("scene transform write failed");
            }
            var zero_scale = queried;
            zero_scale.scale = default;
            if (self.try_set_transform(zero_scale)) {
                throw new InvalidOperationException("scene transform accepted a zero scale");
            }
            degrees_per_second = transform.position.x < 0.0f ? 30.0f : 60.0f;
        }

        public override void on_update(float delta_time) {
            var pose = transform;
            pose.rotation_degrees.y += degrees_per_second * delta_time;
            transform = pose;
        }

        public override void on_destroy() {
            var pose = transform;
            pose.position.z += 1.0f;
            transform = pose;
        }
    }
}
