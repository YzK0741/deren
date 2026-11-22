using System;
using System.IO;
using deren.scripting;

namespace deren {
    /// <summary>
    /// The `[lighting]` section of a project's scene file, plus `[render] sun_intensity` (the
    /// radiance scale the area light multiplies when it takes over the main light).
    ///
    /// These keys used to be read only by the native host. The managed side now parses them from
    /// the same files so the project keeps ownership of its lighting, and applies them through the
    /// runtime setters (`deren.lighting`, `deren.post_processing`). Defaults match
    /// `source/configuration/configuration.cppm` exactly, so a file that omits a key behaves as it
    /// always did.
    /// </summary>
    public sealed class lighting_settings {
        /// <summary>Direction FROM the surface TOWARD the sun, unnormalized (default: the historic vector).</summary>
        public vector_3 sun_direction = new() { x = 0.3f, y = 1.0f, z = 0.5f };

        /// <summary>`[render] sun_intensity`: the base the area light's radiance scales (0..3).</summary>
        public float sun_intensity = 1.0f;

        /// <summary>Equirectangular HDR environment; empty keeps the procedural sky.</summary>
        public string environment_hdr = string.Empty;

        // `environment_intensity` used to sit here: a bake-time multiplier for the HDR. The bake is gone
        // with the native preview path, so the key is neither parsed nor applied; a profile that still
        // carries it loads fine and the value is ignored. Turn `environment_strength` instead.

        public float environment_rotation = 0.0f;
        public float environment_strength = 1.0f;
        public int environment_background = 0;
        public vector_3 environment_color = new() { x = 0.18f, y = 0.18f, z = 0.18f };
        public float environment_blur = 0.5f;

        /// <summary>The calibration file `[lighting] profile` named, once it loaded; empty otherwise.</summary>
        public string profile = string.Empty;

        /// <summary>Side of the square emitter in metres; 0 (the default) is OFF.</summary>
        public float area_light_size = 0.0f;

        /// <summary>The emitter's power in watts.</summary>
        public float area_light_power = 0.0f;

        /// <summary>Emitter centre, relative to the author's origin (the character's feet).</summary>
        public vector_3 area_light_position = new();

        /// <summary>The point the emitter is aimed at, in the same frame as the centre.</summary>
        public vector_3 area_light_target = new();

        /// <summary>Multiplier on the emitter's radiance.</summary>
        public float area_light_intensity = 1.0f;

        /// <summary>True = the emitter takes over the main light (its direction, its radiance).</summary>
        public bool area_light_irradiance = true;

        /// <summary>The area light's soft visibility (false = a hard edge).</summary>
        public bool area_light_shadow = true;

        /// <summary>Penumbra radius in metres; 0 = the automatic heuristic.</summary>
        public float area_light_softness = 0.0f;

        /// <summary>
        /// Reads one scene file's `[lighting]` table and the calibration profile it names. The strict
        /// layout keeps the calibration in the profile. The scene may override the background
        /// mode and color. `config_directory` resolves a relative profile path the way the native
        /// resolver does - against the config file's own directory.
        /// </summary>
        internal static lighting_settings from_document(toml_table document, string config_directory) {
            var result = new lighting_settings();
            if (document.table("lighting") is { } lighting) {
                result.sun_direction = read_vector(lighting, "sun_direction") ?? result.sun_direction;
                result.environment_hdr = lighting.text("environment_hdr") ?? result.environment_hdr;
                if (lighting.text("profile") is { Length: > 0 } profile) {
                    result.read_profile(profile, config_directory);
                }
                if (lighting.integer("environment_background") is >= 0 and <= 3 and { } mode) {
                    result.environment_background = mode;
                }
                if (read_vector(lighting, "environment_color") is { } color &&
                    float.IsFinite(color.x) && float.IsFinite(color.y) && float.IsFinite(color.z)) {
                    result.environment_color = new vector_3 {
                        x = Math.Clamp(color.x, 0.0f, 1.0f),
                        y = Math.Clamp(color.y, 0.0f, 1.0f),
                        z = Math.Clamp(color.z, 0.0f, 1.0f)
                    };
                }
            }
            if (document.table("render") is { } render) {
                result.sun_intensity = render.number("sun_intensity") ?? result.sun_intensity;
            }
            return result;
        }

        /// <summary>
        /// Loads the `[lighting] profile` calibration file and reads its top-level numeric keys with
        /// the same sanitizing the native loader applies (finite checks, ranges, angle wrap).
        /// </summary>
        private void read_profile(string profile, string config_directory) {
            var profile_path = Path.IsPathRooted(profile) ? profile : Path.Combine(config_directory, profile);
            if (!File.Exists(profile_path)) {
                report($"app_config: cannot load the lighting profile '{profile_path}': file not found");
                return;
            }
            try {
                var table = toml_table.parse(File.ReadAllText(profile_path), out var error);
                if (error != null) {
                    report($"app_config: lighting profile '{profile_path}' parsed with issues ({error})");
                }
                if (table != null) {
                    read_profile_table(table);
                    this.profile = profile_path;
                }
            } catch (IOException exception) {
                report($"app_config: cannot load the lighting profile '{profile_path}': {exception.GetBaseException().Message}");
            } catch (UnauthorizedAccessException exception) {
                report($"app_config: cannot load the lighting profile '{profile_path}': {exception.GetBaseException().Message}");
            }
        }

        private void read_profile_table(toml_table profile) {
            // `environment_intensity` is not read: it was the bake-time multiplier of the HDR, the bake is
            // gone with the native preview path, and nothing consumed the value. A profile that still has
            // the key loads fine - the key is ignored, exactly like the native parser does.
            if (profile.number("environment_rotation") is { } rotation) {
                environment_rotation = float.IsFinite(rotation) ? (float)Math.IEEERemainder(rotation, 360.0) : 0.0f;
            }
            if (profile.number("environment_strength") is { } strength) {
                environment_strength = float.IsFinite(strength) ? Math.Clamp(strength, 0.0f, 64.0f) : 1.0f;
            }
            if (profile.number("environment_blur") is { } blur) {
                environment_blur = float.IsFinite(blur) ? Math.Clamp(blur, 0.0f, 1.0f) : 0.5f;
            }
            if (profile.integer("environment_background") is { } background) {
                environment_background = background is >= 0 and <= 3 ? background : 0;
            }
            if (read_vector(profile, "environment_color") is { } color) {
                environment_color = new vector_3 {
                    x = float.IsFinite(color.x) ? Math.Clamp(color.x, 0.0f, 1.0f) : 0.18f,
                    y = float.IsFinite(color.y) ? Math.Clamp(color.y, 0.0f, 1.0f) : 0.18f,
                    z = float.IsFinite(color.z) ? Math.Clamp(color.z, 0.0f, 1.0f) : 0.18f
                };
            }
            area_light_position = read_vector(profile, "area_light_position") ?? area_light_position;
            area_light_target = read_vector(profile, "area_light_target") ?? area_light_target;
            area_light_size = profile.number("area_light_size") ?? area_light_size;
            area_light_power = profile.number("area_light_power") ?? area_light_power;
            area_light_intensity = profile.number("area_light_intensity") ?? area_light_intensity;
            area_light_softness = profile.number("area_light_softness") ?? area_light_softness;
            area_light_irradiance = profile.flag("area_light_irradiance") ?? area_light_irradiance;
            area_light_shadow = profile.flag("area_light_shadow") ?? area_light_shadow;
        }

        private static void report(string message) {
            host_logger.write(message);
        }

        /// <summary>
        /// The frozen derivation from `deren::app_config::derive_area_light`: turns the
        /// `area_light_*` keys into the frame's one emitter, expressed through the sun.
        /// </summary>
        public area_light_derived derive_area_light(vector_3 scene_origin) {
            var derived = new area_light_derived();
            if (!(area_light_size > 0.0f)) {
                return derived;
            }
            derived.world_centre = new vector_3 {
                x = scene_origin.x + area_light_position.x,
                y = scene_origin.y + area_light_position.y,
                z = scene_origin.z + area_light_position.z
            };
            var world_target = new vector_3 {
                x = scene_origin.x + area_light_target.x,
                y = scene_origin.y + area_light_target.y,
                z = scene_origin.z + area_light_target.z
            };
            var to_light = normalize(new vector_3 {
                x = derived.world_centre.x - scene_origin.x,
                y = derived.world_centre.y - scene_origin.y,
                z = derived.world_centre.z - scene_origin.z
            });
            var to_target = new vector_3 {
                x = world_target.x - derived.world_centre.x,
                y = world_target.y - derived.world_centre.y,
                z = world_target.z - derived.world_centre.z
            };
            var axis = normalize(to_target);
            if (is_zero(to_light) || is_zero(axis)) {
                // An emitter at the author's origin, or aimed at itself: no honest direction to
                // hand the sun, so the area light stays off (a zero light pair, not a made-up frame).
                return derived;
            }
            derived.half = 0.5f * area_light_size;
            derived.axis = axis;
            derived.to_light_dir = to_light;
            derived.radiance = area_light_intensity * area_light_power / ((float)Math.PI * area_light_size * area_light_size);
            var distance_to_target = MathF.Sqrt(dot(to_target, to_target));
            derived.penumbra = 0.0f;
            if (area_light_shadow) {
                derived.penumbra = area_light_softness > 0.0f
                    ? area_light_softness
                    : 0.05f * area_light_size / MathF.Max(distance_to_target, 1e-3f);
            }
            derived.enabled = true;
            return derived;
        }

        private static vector_3? read_vector(toml_table table, string key) {
            var values = table.numbers(key);
            if (values == null || values.Count != 3) {
                return null;
            }
            return new vector_3 { x = values[0], y = values[1], z = values[2] };
        }

        private static float dot(vector_3 a, vector_3 b) {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        private static bool is_zero(vector_3 value) {
            return value is { x: 0.0f, y: 0.0f, z: 0.0f };
        }

        private static vector_3 normalize(vector_3 direction) {
            var length_sq = dot(direction, direction);
            if (!(length_sq > 0.0f)) {
                return new vector_3();
            }
            var inverse_length = 1.0f / MathF.Sqrt(length_sq);
            return new vector_3 {
                x = direction.x * inverse_length,
                y = direction.y * inverse_length,
                z = direction.z * inverse_length
            };
        }
    }

    /// <summary>The area light's per-frame values, in the engine's Y-up world frame.</summary>
    public struct area_light_derived {
        public bool enabled;
        public vector_3 world_centre;
        public float half;
        public vector_3 axis;
        public vector_3 to_light_dir;
        public float radiance;
        public float penumbra;
    }
}
