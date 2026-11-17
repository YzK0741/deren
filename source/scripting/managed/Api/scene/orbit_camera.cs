using System.Runtime.InteropServices;
using deren.scripting;

namespace deren {
    /// <summary>
    /// Orbit camera pose owned by managed code. Angles are degrees and the target is in world space.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct orbit_camera {
        public float yaw_degrees;
        public float pitch_degrees;
        public float distance;
        public vector_3 target;

        /// <summary>
        /// Applies this pose to the active scene. Returns false when no scene is active; the
        /// overload with an <c>out script_status</c> says which.
        /// </summary>
        public bool try_apply() {
            return try_apply(out _);
        }

        public unsafe bool try_apply(out script_status status) {
            var copy = this;
            status = scene_api.assign_camera(&copy);
            return status == script_status.success;
        }

        /// <summary>
        /// Reads the active scene's camera pose. Returns false when no scene is active.
        /// </summary>
        public static bool try_get(out orbit_camera value) {
            return try_get(out value, out _);
        }

        public static unsafe bool try_get(out orbit_camera value, out script_status status) {
            orbit_camera result;
            status = scene_api.query_camera(&result);
            if (status != script_status.success) {
                value = default;
                return false;
            }
            value = result;
            return true;
        }
    }
}
