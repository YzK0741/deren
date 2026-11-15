using System.Text;
using deren.scripting;

namespace deren {
    /// <summary>
    /// Lighting as runtime state: the sun's direction, the optional square area emitter, and the
    /// clustered-light switch.
    ///
    /// These used to be read once from deren.toml at startup; the game now moves them between
    /// frames. The native side validates every call (rejecting instead of clamping) and answers on
    /// the next frame, so each method returns whether the change was accepted.
    /// </summary>
    public static class lighting {
        private static void report(script_status status, string what) {
            if (status == script_status.success) {
                return;
            }
            managed_bootstrap.core.write_log(Encoding.UTF8.GetBytes($"Game: {what} rejected ({status})"));
        }

        /// <summary>
        /// Points the sun: the vector goes FROM the surface TOWARD the sun and must be non-zero.
        /// The shadow frustum follows it, so the next frame relights.
        /// </summary>
        public static bool try_set_sun_direction(vector_3 to_sun) {
            var status = scene_api.assign_sun_direction(to_sun.x, to_sun.y, to_sun.z);
            report(status, "set_sun_direction");
            return status == script_status.success;
        }

        /// <summary>
        /// Places the square area emitter: centre, half side length (half &lt;= 0 clears it), the
        /// irradiance energy mode, the emitter's normal (axis) and the softness penumbra.
        /// </summary>
        public static bool try_set_area_light(vector_3 centre, float half, bool irradiance, vector_3 axis,
                                              float penumbra = 0.0f) {
            var status = scene_api.assign_area_light(centre.x, centre.y, centre.z, half, irradiance,
                                                     axis.x, axis.y, axis.z, penumbra);
            report(status, "set_area_light");
            return status == script_status.success;
        }

        /// <summary>Removes the area emitter (equivalent to a half length of zero).</summary>
        public static bool try_clear_area_light() {
            var status = scene_api.assign_area_light(0.0f, 0.0f, 0.0f, 0.0f, false,
                                                     0.0f, 0.0f, 0.0f, 0.0f);
            report(status, "clear_area_light");
            return status == script_status.success;
        }

        /// <summary>Toggles the clustered-light path (the few-light forward loop is the alternative).</summary>
        public static bool try_set_clustered_lights(bool enabled) {
            var status = scene_api.assign_clustered_lights(enabled);
            report(status, "set_clustered_lights");
            return status == script_status.success;
        }

        /// <summary>The largest set the engine's light UBO can hold; a longer rig is rejected, not truncated.</summary>
        public const int max_point_lights = 128;

        /// <summary>
        /// Replaces the WHOLE punctual light set. An empty array (or null) clears every light, which is
        /// the way back to "sun and environment only". The host validates every field and answers on
        /// the next frame, so a false return leaves the previous rig in place.
        ///
        /// The array is copied into a managed scratch buffer before the call, so callers may reuse
        /// their own array immediately afterwards.
        /// </summary>
        public static unsafe bool try_set_point_lights(point_light[]? lights) {
            var count = lights == null ? 0 : lights.Length;
            if (count > max_point_lights) {
                report(script_status.invalid_argument, "set_point_lights (too many lights)");
                return false;
            }
            var records = stackalloc point_light_desc[count == 0 ? 1 : count];
            for (var index = 0; index < count; ++index) {
                lights![index].write_to(&records[index]);
            }
            var status = scene_api.assign_point_lights(records, (uint)count);
            report(status, "set_point_lights");
            return status == script_status.success;
        }
        /// <summary>
        /// Turns directional shadow mapping on or off. Turning it off clears the shadow maps and drops
        /// the pass, so a project that does not want cast shadows does not pay for them.
        /// </summary>
        public static bool try_set_shadow_enabled(bool enabled) {
            var status = scene_api.assign_shadow_enabled(enabled);
            report(status, "set_shadow_enabled");
            return status == script_status.success;
        }
    }
}