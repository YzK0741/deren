using deren.scripting;
using System.Runtime.InteropServices;

namespace deren {
    // `--toon-selftest`: exercises the ABI 39 toon light rig boundary on the first frame and then
    // keeps succeeding. It never depends on an asset or a toon pipeline: the rig is a host-side value,
    // so the same checks run against the default demo.
    internal static unsafe class toon_selftest {
        internal static bool requested;
        private static bool checked_once;

        internal static script_status initialize(managed_api* scripts) {
            // The host refuses to run a project that never activated a scene tree, and this check
            // needs no assets: an empty tree is enough to satisfy that contract and still render the
            // analytic sky behind the rig.
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

        private static scene_tree? tree;

        [UnmanagedCallersOnly]
        private static script_status update(frame_info* frame) {
            if (checked_once) {
                return script_status.success;
            }
            checked_once = true;
            if (!toon_rig.try_current(out var current)) {
                return script_status.script_failure;
            }
            var original_day = current.day_strength;
            var original_rim = current.rim_strength;
            var original_softness = current.shadow_softness;
            // A non-finite lane is rejected, not clamped, and the queued value stays where it was.
            current.rim_strength = float.NaN;
            if (current.try_apply(out var nan_status) || nan_status != script_status.invalid_argument) {
                return script_status.script_failure;
            }
            if (!toon_rig.try_current(out var unchanged) || unchanged.rim_strength != original_rim ||
                unchanged.day_strength != original_day) {
                return script_status.script_failure;
            }
            // A change is accepted and the next read reflects it (pending is what get returns).
            current.rim_strength = original_rim;
            current.day_strength = 0.25f;
            if (!current.try_apply()) {
                return script_status.script_failure;
            }
            if (!toon_rig.try_current(out var quarter) || quarter.day_strength != 0.25f) {
                return script_status.script_failure;
            }
            // Out-of-range lanes map onto the documented ranges instead of failing.
            quarter.day_strength = 5.0f;
            quarter.shadow_softness = 99;
            if (!quarter.try_apply()) {
                return script_status.script_failure;
            }
            if (!toon_rig.try_current(out var clamped) || clamped.day_strength != 1.0f ||
                clamped.shadow_softness != 4) {
                return script_status.script_failure;
            }
            // Leave the demo on the rig the configuration asked for.
            clamped.day_strength = original_day;
            clamped.shadow_softness = original_softness;
            return clamped.try_apply() ? script_status.success : script_status.script_failure;
        }

        [UnmanagedCallersOnly]
        private static script_status stop() {
            return script_status.success;
        }
    }
}
