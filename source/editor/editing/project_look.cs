using System;
using System.Collections.Generic;
using System.Globalization;

namespace deren.editor {
    internal readonly record struct project_look(float exposure, float sun_intensity, vector_3 sun_direction,
                                                 int background, vector_3 background_color, bool fxaa, bool taa) {
        internal static project_look read(render_settings render, lighting_settings lighting) =>
            new(render.exposure, lighting.sun_intensity, lighting.sun_direction,
                lighting.environment_background, lighting.environment_color, render.fxaa, render.taa);

        internal bool apply(render_settings render, lighting_settings lighting) {
            var area = lighting.derive_area_light(new vector_3());
            var use_area_light = area.enabled && lighting.area_light_irradiance;
            return post_processing.try_set_exposure(exposure) &
                   scene_lighting.try_set_sun_direction(use_area_light ? area.to_light_dir : sun_direction) &
                   post_processing.try_set_sun_intensity(use_area_light ? MathF.Min(3, area.radiance) : sun_intensity) &
                   post_processing.try_set_environment(lighting.environment_rotation, lighting.environment_strength, background,
                                                       background_color.x, background_color.y, background_color.z, lighting.environment_blur) &
                   post_processing.try_set_fxaa(fxaa) &
                   post_processing.try_set_taa(taa, render.taa_blend_static, render.taa_blend_min);
        }

        internal Dictionary<string, Dictionary<string, string>> changes_from(project_look original) {
            var render = new Dictionary<string, string>(StringComparer.Ordinal);
            var lighting = new Dictionary<string, string>(StringComparer.Ordinal);
            if (exposure != original.exposure) { render.Add("exposure", number(exposure)); }
            if (sun_intensity != original.sun_intensity) { render.Add("sun_intensity", number(sun_intensity)); }
            if (fxaa != original.fxaa) { render.Add("fxaa", fxaa ? "true" : "false"); }
            if (taa != original.taa) { render.Add("taa", taa ? "true" : "false"); }
            if (!sun_direction.Equals(original.sun_direction)) { lighting.Add("sun_direction", $"[{vector(sun_direction)}]"); }
            if (background != original.background) { lighting.Add("environment_background", background.ToString(CultureInfo.InvariantCulture)); }
            if (!background_color.Equals(original.background_color)) { lighting.Add("environment_color", $"[{vector(background_color)}]"); }
            var settings = new Dictionary<string, Dictionary<string, string>>(StringComparer.Ordinal);
            if (render.Count > 0) { settings.Add("render", render); }
            if (lighting.Count > 0) { settings.Add("lighting", lighting); }
            return settings;
        }

        internal static string number(float value) => value.ToString("R", CultureInfo.InvariantCulture);
        internal static string vector(vector_3 value) => $"{number(value.x)}, {number(value.y)}, {number(value.z)}";

        internal static bool try_number(string text, float min, float max, out float value) =>
            float.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out value) &&
            float.IsFinite(value) && value >= min && value <= max;

        internal static bool try_vector(string text, float min, float max, out vector_3 value) {
            value = default;
            var fields = text.Split(',');
            return fields.Length == 3 &&
                   try_number(fields[0], min, max, out value.x) &&
                   try_number(fields[1], min, max, out value.y) &&
                   try_number(fields[2], min, max, out value.z);
        }
    }
}
