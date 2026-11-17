using deren.scripting;

namespace deren {
    /// <summary>
    /// The `[render]` keys whose value is a runtime knob rather than a startup/creation parameter:
    /// exposure, tone mapping, bloom, FXAA, TAA, SSAO, the G-buffer debug view and clustered light
    /// culling. The managed framework reads them from the project's scene file and applies them
    /// through the runtime setters before the project's initialize, so the game owns the look of its
    /// frame.
    ///
    /// The keys that describe the WINDOW or the FRAME'S SIZING (window_width/height/title, vsync,
    /// max_fps, clear_color, render_scale, upscale, offscreen) are creation parameters the native
    /// host must fix before the swapchain exists, so they stay native-only. Shadow cascades, shadow
    /// map size and depth bias are applied before the first scene set binds the layered image, and
    /// the GPU-driven paths (rt_*, visibility_buffer, subtilis, software_raster) are device
    /// features; neither is a per-frame knob here yet.
    ///
    /// Defaults match `source/configuration/configuration.cppm` exactly, so a file that omits a key
    /// behaves as it always did.
    /// </summary>
    public sealed class render_settings {
        /// <summary>`[render] exposure`: the linear multiplier the tone mapping stage takes.</summary>
        public float exposure = 1.0f;

        /// <summary>`[render] tone_mapping`: 0 = none, 1 = ACES, 2 = the game-toon curve.</summary>
        public int tone_mapping = 0;

        /// <summary>`[render] bloom`: whether the four-level bright-pass chain records.</summary>
        public bool bloom = true;

        /// <summary>`[render] fxaa`: one extra fullscreen edge-adaptive pass.</summary>
        public bool fxaa = false;
        /// <summary>`[render] shadow`, applied by the native host before the scene is bound.</summary>
        public bool shadow = true;

        /// <summary>`[render] taa` plus its two history weights.</summary>
        public bool taa = false;
        public float taa_blend_static = 0.9f;
        public float taa_blend_min = 0.5f;

        /// <summary>`[render] ssao` plus the hemisphere the lighting stage traces.</summary>
        public bool ssao = true;
        public float ssao_radius = 0.5f;
        public float ssao_intensity = 1.0f;
        public int ssao_samples = 8;

        /// <summary>`[debug] gbuffer_debug`: draws the opaque scene into the G-buffer targets.</summary>
        public bool gbuffer_debug = false;

        /// <summary>`[render] clustered_lights`: the screen-tile light culling path.</summary>
        public bool clustered_lights = true;

        /// <summary>
        /// `[render] character_forward` / `game_toon`: the stage selection stays native (it must be
        /// fixed before the scene binds its layered pipelines); parsed here only because the
        /// game-toon frame forces the tone mapping curve through <see cref="effective_tone_mapping"/>.
        /// </summary>
        public bool character_forward = false;
        public bool game_toon = false;

        /// <summary>Reads the `[render]` table of one scene file.</summary>
        internal static render_settings from_document(toml_table document) {
            var result = new render_settings();
            if (document.table("render") is not { } render) {
                return result;
            }
            result.exposure = render.number("exposure") ?? result.exposure;
            result.tone_mapping = render.integer("tone_mapping") ?? result.tone_mapping;
            result.bloom = render.flag("bloom") ?? result.bloom;
            result.fxaa = render.flag("fxaa") ?? result.fxaa;
            result.shadow = render.flag("shadow") ?? result.shadow;
            result.taa = render.flag("taa") ?? result.taa;
            result.taa_blend_static = render.number("taa_blend_static") ?? result.taa_blend_static;
            result.taa_blend_min = render.number("taa_blend_min") ?? result.taa_blend_min;
            result.ssao = render.flag("ssao") ?? result.ssao;
            result.ssao_radius = render.number("ssao_radius") ?? result.ssao_radius;
            result.ssao_intensity = render.number("ssao_intensity") ?? result.ssao_intensity;
            result.ssao_samples = render.integer("ssao_samples") ?? result.ssao_samples;
            result.gbuffer_debug = document.table("debug")?.flag("gbuffer_debug") ?? result.gbuffer_debug;
            result.clustered_lights = render.flag("clustered_lights") ?? result.clustered_lights;
            result.character_forward = render.flag("character_forward") ?? result.character_forward;
            result.game_toon = render.flag("game_toon") ?? result.game_toon;
            // The shader maximum is 16 samples; the native loader clamps it the same way.
            result.ssao_samples = result.ssao_samples < 0 ? 0 : result.ssao_samples > 16 ? 16 : result.ssao_samples;
            return result;
        }

        /// <summary>The tone mapping lane the frame ends up on: the game-toon model forces its curve.</summary>
        public int effective_tone_mapping() {
            return game_toon && character_forward ? 2 : tone_mapping;
        }
    }
}
