using System.Runtime.InteropServices;
using deren.scripting;

namespace deren {
    /// <summary>
    /// Orbit camera pose owned by managed code. Angles are degrees and the target is in world space.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct camera {
        public float yaw_degrees;
        public float pitch_degrees;
        public float distance;
        public vector_3 target;

        /// <summary>
        /// Applies this pose to the active scene. Returns false when no scene is active.
        /// </summary>
        public unsafe bool try_apply() {
            var copy = this;
            return scene_api.assign_camera(&copy) == script_status.success;
        }

        /// <summary>
        /// Reads the active scene's camera pose. Returns false when no scene is active.
        /// </summary>
        public static unsafe bool try_get(out camera value) {
            camera result;
            if (scene_api.query_camera(&result) != script_status.success) {
                value = default;
                return false;
            }
            value = result;
            return true;
        }
    }
}
