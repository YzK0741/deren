using deren;

namespace game {
#pragma warning disable CS8981
    public sealed class rotate : behaviour {
#pragma warning restore CS8981
        private float degrees_per_second;

        public override void on_create() {
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
