using System;
using System.Text;
using deren.scripting;

namespace deren {
    /// <summary>
    /// Post-processing and environment calibration as runtime state.
    ///
    /// These were startup-only knobs of the native host ([render]/[lighting] in deren.toml); the
    /// game now owns them and can move them between frames. Every call validates its arguments on
    /// the native side (an out-of-range value is rejected rather than clamped) and applies the
    /// change on the next frame, so the methods return whether the call was accepted.
    /// </summary>
    public static class post_processing {
        private static void report(script_status status, string what) {
            if (status == script_status.success) {
                return;
            }
            managed_bootstrap.core.write_log(Encoding.UTF8.GetBytes($"Game: {what} rejected ({status})"));
        }

        /// <summary>Exposure scale, 0.05..20 (1.0 = unchanged).</summary>
        public static bool try_set_exposure(float exposure) {
            var status = scene_api.assign_exposure(exposure);
            report(status, "set_exposure");
            return status == script_status.success;
        }

        /// <summary>Tone mapping: 0 = linear/none, 1 = ACES, 2 = character-forward.</summary>
        public static bool try_set_tone_mapping(int mode) {
            var status = scene_api.assign_tone_mapping(mode);
            report(status, "set_tone_mapping");
            return status == script_status.success;
        }

        /// <summary>
        /// IBL calibration: rotation in degrees (wraps), strength 0..64, background mode 0..3,
        /// background colour 0..1 per channel, blur 0..1.
        /// </summary>
        public static bool try_set_environment(float rotation_degrees, float strength, int background,
                                               float color_r, float color_g, float color_b, float blur) {
            var status = scene_api.assign_environment(rotation_degrees, strength, background, color_r, color_g,
                                                      color_b, blur);
            report(status, "set_environment");
            return status == script_status.success;
        }

        /// <summary>Sun radiance scale, 0..3 (1.0 = unchanged).</summary>
        public static bool try_set_sun_intensity(float scale) {
            var status = scene_api.assign_sun_intensity(scale);
            report(status, "set_sun_intensity");
            return status == script_status.success;
        }

        /// <summary>Bloom blend weight 0..4 (0 disables) and bright-pass threshold 0..0.75.</summary>
        public static bool try_set_bloom(float intensity, float threshold) {
            var status = scene_api.assign_bloom(intensity, threshold);
            report(status, "set_bloom");
            return status == script_status.success;
        }

        /// <summary>FXAA on/off with its two shader knobs (defaults match the shader: 0.75, 0.166).</summary>
        public static bool try_set_fxaa(bool enabled, float subpixel = 0.75f, float edge_threshold = 0.166f) {
            var status = scene_api.assign_fxaa(enabled, subpixel, edge_threshold);
            report(status, "set_fxaa");
            return status == script_status.success;
        }

        /// <summary>TAA on/off with its blend weights (defaults match deren.toml: 0.9, 0.5).</summary>
        public static bool try_set_taa(bool enabled, float blend_static = 0.9f, float blend_min = 0.5f) {
            var status = scene_api.assign_taa(enabled, blend_static, blend_min);
            report(status, "set_taa");
            return status == script_status.success;
        }

        /// <summary>SSAO on/off with radius, intensity (0..1) and sample count (0..16).</summary>
        public static bool try_set_ssao(bool enabled, float radius = 0.5f, float intensity = 1.0f,
                                        uint samples = 8) {
            var status = scene_api.assign_ssao(enabled, radius, intensity, samples);
            report(status, "set_ssao");
            return status == script_status.success;
        }

        /// <summary>The G-buffer diagnostic view (a debug switch, not a shading mode).</summary>
        public static bool try_set_gbuffer_debug(bool enabled) {
            var status = scene_api.assign_gbuffer_debug(enabled);
            report(status, "set_gbuffer_debug");
            return status == script_status.success;
        }
        /// <summary>
        /// Cel shading for the toon stages: the number of diffuse bands (0 keeps the shading smooth)
        /// and how soft each step is (0..1). A band count outside 0..32 or a softness outside 0..1 is
        /// rejected by the host instead of being clamped.
        /// </summary>
        public static bool try_set_toon_shading(float steps, float softness = 0.15f) {
            var status = scene_api.assign_toon_shading(steps, softness);
            report(status, "set_toon_shading");
            return status == script_status.success;
        }
    }
}
