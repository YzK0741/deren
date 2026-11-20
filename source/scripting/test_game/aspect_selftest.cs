using deren.scripting;
using System.Runtime.InteropServices;

namespace deren {
    // `--scene-aspect-selftest`: exercises the ABI 41 projection-aspect boundary on the first frame.
    // The shape an editor viewport would name and the explicit "follow the window" zero are accepted;
    // a negative aspect and NaN are refused by the host. The host half of the test asserts that
    // exactly the two accepted values arrived and that the last one was the reset.
    internal static unsafe class aspect_selftest {
        internal static bool requested;

        private static bool checked_once;
        private static scene_tree? tree;

        internal static script_status initialize(managed_api* scripts) {
            // Like the rename self check: the host refuses a project that never activated a tree.
            if (!scene_tree.try_create(out var created) || created == null) {
                return script_status.script_failure;
            }
            tree = created;
            if (!tree.try_activate()) {
                return script_status.script_failure;
            }
            scripts->update = &update;
            scripts->stop = &stop;
            return script_status.success;
        }

        [UnmanagedCallersOnly]
        private static script_status update(frame_info* frame) {
            if (checked_once) {
                return script_status.success;
            }
            checked_once = true;
            // The projection aspect of the editor's 960x836 viewport column.
            if (!camera_projection.try_set_aspect(960.0f / 836.0f)) {
                return script_status.script_failure;
            }
            // Zero is the explicit "back to the window's aspect" value, not an error.
            if (!camera_projection.try_set_aspect(0.0f)) {
                return script_status.script_failure;
            }
            if (camera_projection.try_set_aspect(-1.0f, out var negative) ||
                negative != script_status.invalid_argument) {
                return script_status.script_failure;
            }
            if (camera_projection.try_set_aspect(float.NaN, out var nan) ||
                nan != script_status.invalid_argument) {
                return script_status.script_failure;
            }
            return script_status.success;
        }

        [UnmanagedCallersOnly]
        private static script_status stop() {
            return script_status.success;
        }
    }
}
