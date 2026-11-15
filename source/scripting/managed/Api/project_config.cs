using System;
using System.Globalization;
using System.IO;
using System.Text;
using deren.scripting;

namespace deren {
    /// <summary>
    /// The managed side's view of the project files. The host loads the assembly from the project
    /// directory, so `deren.toml` and the scene file it names sit next to it; this class parses them
    /// once, before the project's `preinit`, and exposes the `[lighting]` profile the framework
    /// applies to the runtime before the project's `initialize` runs (the project can still move
    /// anything later - the setters are runtime state).
    ///
    /// The native host keeps its own parse of the keys it consumes (window, paths, ...); the two
    /// readers deliberately agree on the keys they share rather than talking to each other.
    /// </summary>
    public static class project_config {
        /// <summary>The parsed `[lighting]` profile; built-in defaults when the files omit it.</summary>
        public static lighting_settings lighting { get; private set; } = new();

        /// <summary>The parsed `[render]` runtime knobs; built-in defaults when the files omit them.</summary>
        public static render_settings render { get; private set; } = new();

        /// <summary>
        /// The directory of the entry scene (`[project] main_scene`). It is the project's data root:
        /// the lighting profile and the character rig live next to the scene, which is where the
        /// manifest points them - so the build tree never needs a copy of them. Empty when the
        /// manifest or scene could not be read, in which case callers fall back to the directory the
        /// manifest lives in.
        /// </summary>
        internal static string scene_directory { get; private set; } = string.Empty;

        private static bool loaded;

        internal static void load(string project_directory) {
            loaded = false;
            lighting = new lighting_settings();
            render = new render_settings();
            scene_directory = string.Empty;
            var manifest = Path.Combine(project_directory, "deren.toml");
            if (!File.Exists(manifest)) {
                log($"config: no deren.toml in '{project_directory}'; keeping the built-in lighting defaults");
                return;
            }
            var root = read(manifest);
            if (root == null) {
                return;
            }
            var main_scene = root.table("project")?.text("main_scene") ?? "main.toml";
            var scene_path = Path.Combine(project_directory, main_scene);
            var scene = read(scene_path);
            if (scene == null) {
                return;
            }
            // `[lighting] profile` paths resolve against the scene file's own directory.
            scene_directory = Path.GetDirectoryName(scene_path) ?? project_directory;
            lighting = lighting_settings.from_document(scene, scene_directory);
            render = render_settings.from_document(scene);
            loaded = true;
            var profile = lighting;
            var profile_note = profile.profile.Length != 0
                ? $", profile '{Path.GetFileName(profile.profile)}'"
                : ", no profile";
            log(string.Create(CultureInfo.InvariantCulture,
                $"config: [lighting] from '{main_scene}': sun ({profile.sun_direction.x:F3}, {profile.sun_direction.y:F3}, {profile.sun_direction.z:F3}) x {profile.sun_intensity:F3}, " +
                $"environment rotation {profile.environment_rotation:F1} strength {profile.environment_strength:F3} background {profile.environment_background}, " +
                $"area light {(profile.area_light_size > 0.0f ? $"{profile.area_light_size:F1} m x {profile.area_light_power:F0} W" : "off")}{profile_note}"));
            var look = render;
            log(string.Create(CultureInfo.InvariantCulture,
                $"config: [render] from '{main_scene}': exposure {look.exposure:F3}, tone mapping {look.effective_tone_mapping()}, " +
                $"bloom {(look.bloom ? "on" : "off")}, fxaa {(look.fxaa ? "on" : "off")}, " +
                $"taa {(look.taa ? $"on ({look.taa_blend_static:F3}/{look.taa_blend_min:F3})" : "off")}, " +
                $"ssao {(look.ssao ? $"on ({look.ssao_radius:F3} m, {look.ssao_intensity:F3}, {look.ssao_samples} samples)" : "off")}, " +
                $"gbuffer debug {(look.gbuffer_debug ? "on" : "off")}, clustered lights {(look.clustered_lights ? "on" : "off")}"));
        }

        /// <summary>
        /// Applies the parsed profile to the runtime. Called once, after the scene API is bound and
        /// before the project's `initialize`, so the project's own calls can still win.
        /// </summary>
        internal static void apply() {
            if (!loaded) {
                return;
            }
            var profile = lighting;
            if (profile.environment_hdr.Length != 0) {
                global_environment.try_set_hdr(profile.environment_hdr);
            }
            post_processing.try_set_environment(profile.environment_rotation, profile.environment_strength,
                                                profile.environment_background, profile.environment_color.x,
                                                profile.environment_color.y, profile.environment_color.z,
                                                profile.environment_blur);
            // The author's origin is the world origin: scenes are authored with the character's
            // feet on the ground at y = 0, which is the frame the manifest's light numbers use.
            var derived = profile.derive_area_light(new vector_3());
            var takes_over = derived.enabled && profile.area_light_irradiance;
            var direction = takes_over ? derived.to_light_dir : profile.sun_direction;
            var intensity = takes_over
                ? MathF.Min(3.0f, profile.sun_intensity * derived.radiance)
                : profile.sun_intensity;
            deren.lighting.try_set_sun_direction(direction);
            post_processing.try_set_sun_intensity(intensity);
            if (derived.enabled) {
                deren.lighting.try_set_area_light(derived.world_centre, derived.half, profile.area_light_irradiance,
                                                  derived.axis, derived.penumbra);
                log(string.Create(CultureInfo.InvariantCulture,
                    $"config: area light centre ({derived.world_centre.x:F3}, {derived.world_centre.y:F3}, {derived.world_centre.z:F3}) half {derived.half:F3} m, " +
                    $"radiance {derived.radiance:F5}{(takes_over ? " (takes over the sun)" : " (penumbra only)")}, penumbra {derived.penumbra:F3} m"));
            } else {
                deren.lighting.try_clear_area_light();
            }
            // The `[render]` runtime knobs: the same lanes the native host used to set once at
            // startup, now owned by the managed framework so a project can move them any frame.
            var look = render;
            post_processing.try_set_exposure(look.exposure);
            post_processing.try_set_tone_mapping(look.effective_tone_mapping());
            // 0.75 is the brightest end of the documented visible threshold range (0..0.75); the old
            // native startup asked for 1.0 and the renderer clamped it to the same place.
            post_processing.try_set_bloom(look.bloom ? 1.0f : 0.0f, 0.75f);
            post_processing.try_set_fxaa(look.fxaa);
            post_processing.try_set_taa(look.taa, look.taa_blend_static, look.taa_blend_min);
            post_processing.try_set_ssao(look.ssao, look.ssao_radius, look.ssao_intensity,
                                         (uint)look.ssao_samples);
            post_processing.try_set_gbuffer_debug(look.gbuffer_debug);
            deren.lighting.try_set_clustered_lights(look.clustered_lights);
        }

        private static toml_table? read(string path) {
            try {
                var parsed = toml_table.parse(File.ReadAllText(path), out var error);
                if (error != null) {
                    log($"config: '{path}' parsed with issues ({error}); the keys read so far are used");
                }
                return parsed;
            } catch (IOException error) {
                log($"config: cannot read '{path}': {error.GetBaseException().Message}");
                return null;
            } catch (UnauthorizedAccessException error) {
                log($"config: cannot read '{path}': {error.GetBaseException().Message}");
                return null;
            }
        }

        private static void log(string message) {
            managed_bootstrap.core.write_log(Encoding.UTF8.GetBytes(message));
        }
    }
}
