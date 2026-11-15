using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using deren.scripting;

namespace deren {
    /// <summary>
    /// A light rig read from a TOML file and pushed as one punctual light set.
    ///
    /// This exists because the character lighting in the reference render is a rig, not one sun: the
    /// GameToon preview is lit by two spots and eight points placed around the head. Keeping the rig
    /// in a data file means the staging can be edited without touching the renderer, and the managed
    /// side owns both the file and the push.
    ///
    /// The file is `[[light]]` tables plus a `[rig]` table with two staging keys:
    /// `up_axis` ("y" or "z"; "z" rotates the positions into the engine's Y-up world) and `offset`
    /// (world position of the model's local origin, which the asset binder decides when it grounds an
    /// import). Positions and directions are authored in the model's space, so a rig copied from a
    /// reference render only needs its staging keys updated.
    /// </summary>
    public static class light_rig {
        /// <summary>The file name <see cref="try_load_project_rig"/> looks for in the project directory.</summary>
        public const string file_name = "toon-light-rig.toml";

        /// <summary>
        /// Loads `toon-light-rig.toml` from the project's DATA directory - the entry scene's own
        /// directory, which is where the manifest points the scene - and replaces the punctual light
        /// set. False (with a log line) when the file is absent or unreadable; a missing rig is not an
        /// error, because most demos light themselves with the sun alone.
        /// </summary>
        public static bool try_load_project_rig() {
            var directory = project_config.scene_directory;
            if (string.IsNullOrEmpty(directory)) {
                directory = managed_bootstrap.project_directory;
            }
            if (string.IsNullOrEmpty(directory)) {
                return false;
            }
            return try_load(Path.Combine(directory, file_name));
        }

        /// <summary>Loads one rig file and replaces the punctual light set with its lights.</summary>
        public static unsafe bool try_load(string path) {
            string document;
            try {
                document = File.ReadAllText(path);
            } catch (Exception error) {
                log($"light rig: cannot read '{path}' ({error.GetBaseException().Message})");
                return false;
            }
            var root = toml_table.parse(document, out var complaint);
            if (root == null) {
                log($"light rig: '{path}' is not TOML ({complaint})");
                return false;
            }
            if (complaint != null) {
                log($"light rig: '{path}' parsed with a complaint: {complaint}");
            }
            var entries = root.tables("light");
            if (entries == null || entries.Count == 0) {
                log($"light rig: '{path}' declares no [[light]] entries");
                return false;
            }
            var staging = root.table("rig");
            var z_up = string.Equals(staging?.text("up_axis") ?? "y", "z", StringComparison.OrdinalIgnoreCase);
            var offset = staging?.numbers("offset") is { Count: 3 } given ? given : new List<float> { 0.0f, 0.0f, 0.0f };

            var lights = new point_light[entries.Count];
            for (var index = 0; index < entries.Count; ++index) {
                var entry = entries[index];
                if (!try_read_light(entry, index, z_up, offset, out lights[index])) {
                    return false;
                }
            }
            if (!lighting.try_set_point_lights(lights)) {
                log($"light rig: the host rejected the {lights.Length} light(s) of '{path}'");
                return false;
            }
            log($"light rig: '{Path.GetFileName(path)}' published {lights.Length} light(s)");
            return true;
        }

        private static bool try_read_light(toml_table entry, int index, bool z_up, List<float> offset,
                                           out point_light light) {
            light = default;
            if (!try_position(entry.numbers("position"), z_up, offset, out var placement)) {
                log($"light rig: light {index} needs a 'position' of three numbers");
                return false;
            }
            // A direction is only meaningful for a spot, and a spot without one would silently light
            // straight down, so the default is the engine's own -Y and a stated one is rotated.
            var direction = new vector_3 { x = 0.0f, y = -1.0f, z = 0.0f };
            if (entry.numbers("direction") is { Count: 3 } stated) {
                direction = new vector_3 { x = stated[0], y = stated[1], z = stated[2] };
                if (z_up) {
                    direction = new vector_3 { x = stated[0], y = stated[2], z = -stated[1] };
                }
            }
            light.position = placement;
            light.direction = direction;
            light.colour = colour(entry.numbers("colour"));
            light.intensity = entry.number("intensity") ?? 1.0f;
            light.range = entry.number("range") ?? 10.0f;
            light.spot = entry.flag("spot") ?? false;
            light.outer_degrees = entry.number("outer_degrees") ?? 45.0f;
            light.inner_degrees = entry.number("inner_degrees") ?? 0.0f;
            light.npr_mode = entry.number("npr_mode") ?? 0.0f;
            if (entry.numbers("npr") is { Count: 4 } npr) {
                light.npr_data_x = npr[0];
                light.npr_data_y = npr[1];
                light.npr_data_z = npr[2];
                light.npr_data_w = npr[3];
            }
            return true;
        }

        private static bool try_position(List<float>? given, bool z_up, List<float> offset, out vector_3 value) {
            value = default;
            if (given is not { Count: 3 }) {
                return false;
            }
            // Rotate first, then translate: `offset` is a world-space placement, while the authored
            // numbers are in the model's space. Z-up to Y-up is a -90 degree turn about X.
            var rotated = z_up
                ? new vector_3 { x = given[0], y = given[2], z = -given[1] }
                : new vector_3 { x = given[0], y = given[1], z = given[2] };
            value = new vector_3 { x = rotated.x + offset[0], y = rotated.y + offset[1], z = rotated.z + offset[2] };
            return float.IsFinite(value.x) && float.IsFinite(value.y) && float.IsFinite(value.z);
        }

        private static vector_3 colour(List<float>? given) {
            return given is { Count: 3 }
                ? new vector_3 { x = given[0], y = given[1], z = given[2] }
                : new vector_3 { x = 1.0f, y = 1.0f, z = 1.0f };
        }

        private static void log(string message) {
            managed_bootstrap.core.write_log(Encoding.UTF8.GetBytes($"Game: {message}"));
        }
    }
}
