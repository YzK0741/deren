using System;
using System.Text;
using deren.scripting;

namespace deren {
    /// <summary>
    /// The image-based lighting of a rendered scene, one environment slot at a time.
    /// </summary>
    /// <remarks>
    /// Slot 0 is the scene-wide environment every material samples unless its record names another
    /// slot. Slots 1..3 are the per-material lanes: a <see cref="material_override"/> carrying that
    /// slot number makes its primitives sample that slot instead (the engine has four slots,
    /// `layout::heap_environment_capacity`).
    /// </remarks>
    public static unsafe class global_environment {
        /// <summary>The number of environment slots the renderer has (0 is the scene-wide one).</summary>
        public const int slot_count = 4;

        /// <summary>
        /// Replaces the SCENE-WIDE environment (slot 0) with the file at <paramref name="path"/>: a
        /// baked .ibl or an equirectangular .hdr, resolved against the executable directory when
        /// relative. An empty path returns that slot to the procedural sky.
        /// </summary>
        public static bool try_set_hdr(string? path) {
            return try_set_slot_hdr(0, path);
        }

        /// <summary>
        /// Replaces ONE environment slot with the file at <paramref name="path"/>: a baked .ibl or an
        /// equirectangular .hdr, resolved against the executable directory when relative. An empty
        /// path loads the procedural sky into that slot.
        ///
        /// The host applies the change between frames, so a successful call only means the slot and
        /// the path were accepted; if the later decode fails, that slot's current environment stays.
        /// Returns false for an out-of-range slot, a host without the environment entry, a missing
        /// scene, or a file that is not there.
        /// </summary>
        public static bool try_set_slot_hdr(int environment_slot, string? path) {
            if (environment_slot < 0 || environment_slot >= slot_count) {
                managed_bootstrap.core.write_log(Encoding.UTF8.GetBytes(
                    $"Game: environment slot {environment_slot} is out of range (0..{slot_count - 1})"));
                return false;
            }
            path ??= string.Empty;
            try {
                var bytes = Encoding.UTF8.GetBytes(path);
                fixed (byte* data = bytes) {
                    var view = new utf8_view { data = data, length = (uint)bytes.Length };
                    var status = scene_api.assign_environment_hdr(view, (uint)environment_slot);
                    if (status == script_status.success) {
                        return true;
                    }
                    managed_bootstrap.core.write_log(Encoding.UTF8.GetBytes(
                        $"Game: cannot set the environment of slot {environment_slot} '{path}' ({status})"));
                    return false;
                }
            } catch (Exception error) {
                managed_bootstrap.core.write_log(Encoding.UTF8.GetBytes(
                    $"Game: environment slot {environment_slot} request failed: {error.GetBaseException().Message}"));
                return false;
            }
        }
    }
}
