using deren.scripting;

namespace deren {
    /// <summary>
    /// The toon light rig as one editable value: the ten float4 lanes the character stage reads,
    /// with the configuration vocabulary naming each scalar, plus named defaults taken from the
    /// engine struct.
    ///
    /// This used to be a startup-only product: deren.toml's <c>[toon]</c> block built the rig once,
    /// before the first frame, because the lane block may not be written while a frame is in flight.
    /// The host now queues an apply and publishes it between frames, so the game can move any lane
    /// at runtime and the answer arrives with the next frame. The whole rig is replaced, never a
    /// single lane: read <see cref="try_current"/>, change what you want and apply it.
    ///
    /// The engine's contract still holds underneath: <see cref="try_apply"/> only queues the value,
    /// and the frame the game is currently scripting keeps the old rig. The native side validates
    /// every lane (rejecting instead of clamping) and answers on the next frame.
    /// </summary>
    public sealed class toon_rig {
        /// <summary>0 = the sun is gone and the head light carries the character; 1 = full sun.</summary>
        public float day_strength = 1.0f;

        /// <summary>The head light's offset along the character's depth.</summary>
        public float other_light_offset = 0.0f;

        /// <summary>The head light's share at day 1 (the sun up). Author default 0.3.</summary>
        public float other_light_strength = 1.0f;

        /// <summary>The head light's share at day 0 (the sun down). Author default 0.7.</summary>
        public float other_light_strength_offset = 0.0f;

        /// <summary>The head light's result strength at day 0. Author default 0.7.</summary>
        public float head_light_day0 = 0.7f;

        /// <summary>The head light's result strength at day 1. Author default 0.3.</summary>
        public float head_light_day1 = 0.3f;

        /// <summary>Self-occlusion on the toon shadow term.</summary>
        public float self_ao_shadow_strength = 1.0f;

        /// <summary>The metallic/gloss map's occlusion exponent.</summary>
        public float ao_strength = 1.0f;

        /// <summary>The head light's colour.</summary>
        public vector_3 head_light_colour = new() { x = 0.60f, y = 0.65f, z = 0.80f };

        /// <summary>The face expression index (0..3; the host clamps it).</summary>
        public int emotion_type;

        /// <summary>The sun's colour on a surface's shadow side.</summary>
        public vector_3 sun_dark_colour = new() { x = 0.60f, y = 0.65f, z = 0.80f };

        /// <summary>The shadow side's base colour exponent.</summary>
        public float base_colour_pow = 1.0f;

        /// <summary>Where the rim starts: larger moves it toward the silhouette.</summary>
        public float rim_area = 1.0f;

        /// <summary>The rim's strength.</summary>
        public float rim_strength = 1.0f;

        /// <summary>The rim's strength when the light is not along the camera's axis.</summary>
        public float rim_nolxz_strength = 1.0f;

        /// <summary>How strongly the rim tracks the material's lit layer. Author default 0.1.</summary>
        public float rim_diffuse_colour_effect = 0.1f;

        /// <summary>The subsurface colour.</summary>
        public vector_3 sss_colour = new() { x = 1.0f, y = 0.80f, z = 0.78f };

        /// <summary>The subsurface falloff's exponent.</summary>
        public float sss_pow_strength = 1.0f;

        /// <summary>The environment light's strength.</summary>
        public float env_strength = 1.0f;

        /// <summary>The environment's rotation in degrees.</summary>
        public float env_rotation = 0.0f;

        /// <summary>The environment's specular strength.</summary>
        public float specular_strength = 1.0f;

        /// <summary>How much the diffuse term blends toward the environment.</summary>
        public float diffuse_blend_effect = 1.0f;

        /// <summary>The backlight compensation's weight.</summary>
        public float backlight_strength = 1.0f;

        /// <summary>The NoF term's strength.</summary>
        public float nof_strength = 1.0f;

        /// <summary>The NoF term's exponent.</summary>
        public float nof_pow_strength = 1.0f;

        /// <summary>The ramp's NoL colour strength.</summary>
        public float ramp_colour_nol_strength = 1.0f;

        /// <summary>The toon shadow ladder (0..4; the host rounds and clamps it). 0 is the single-PCF path.</summary>
        public int shadow_softness;

        /// <summary>Optional nose shadow reduction on the face.</summary>
        public float nose_shadow_reduction;

        /// <summary>Optional hair highlight width.</summary>
        public float hair_highlight_width;

        /// <summary>Optional hair highlight shift.</summary>
        public float hair_highlight_shift;

        /// <summary>The authored hair P-mask mode.</summary>
        public bool hair_masked_highlights;

        /// <summary>The angular face SDF.</summary>
        public bool face_angular_sdf;

        /// <summary>
        /// Queues this rig as the value the next frame renders with. False leaves the previous rig in
        /// place: the host validated and rejected it, or no scene module is bound yet.
        /// </summary>
        public bool try_apply() {
            return try_apply(out _);
        }

        public unsafe bool try_apply(out script_status status) {
            scene_api.toon_rig_desc desc = default;
            write_to(&desc);
            status = scene_api.assign_toon_rig(&desc);
            if (status != script_status.success) {
                host_logger.write($"Game: set_toon_rig rejected ({status})");
            }
            return status == script_status.success;
        }

        /// <summary>
        /// The rig the next frame will use: a queued request if one is pending, otherwise the one the
        /// last frame rendered with. Starting a change from here keeps every lane the game does not
        /// touch at its current value.
        /// </summary>
        public static bool try_current(out toon_rig rig) {
            return try_current(out rig, out _);
        }

        public static unsafe bool try_current(out toon_rig rig, out script_status status) {
            scene_api.toon_rig_desc desc = default;
            status = scene_api.read_toon_rig(&desc);
            rig = status == script_status.success ? from_desc(&desc) : new toon_rig();
            if (status != script_status.success) {
                host_logger.write($"Game: get_toon_rig rejected ({status})");
            }
            return status == script_status.success;
        }

        private unsafe void write_to(scene_api.toon_rig_desc* desc) {
            desc->day[0] = day_strength;
            desc->day[1] = other_light_offset;
            desc->day[2] = other_light_strength;
            desc->day[3] = other_light_strength_offset;
            desc->other_light[0] = head_light_day0;
            desc->other_light[1] = head_light_day1;
            desc->other_light[2] = self_ao_shadow_strength;
            desc->other_light[3] = ao_strength;
            desc->other_colour[0] = head_light_colour.x;
            desc->other_colour[1] = head_light_colour.y;
            desc->other_colour[2] = head_light_colour.z;
            desc->other_colour[3] = emotion_type;
            desc->main_dark[0] = sun_dark_colour.x;
            desc->main_dark[1] = sun_dark_colour.y;
            desc->main_dark[2] = sun_dark_colour.z;
            desc->main_dark[3] = base_colour_pow;
            desc->rim[0] = rim_area;
            desc->rim[1] = rim_strength;
            desc->rim[2] = rim_nolxz_strength;
            desc->rim[3] = rim_diffuse_colour_effect;
            desc->sss[0] = sss_colour.x;
            desc->sss[1] = sss_colour.y;
            desc->sss[2] = sss_colour.z;
            desc->sss[3] = sss_pow_strength;
            desc->env[0] = env_strength;
            desc->env[1] = env_rotation;
            desc->env[2] = specular_strength;
            desc->env[3] = diffuse_blend_effect;
            desc->misc[0] = backlight_strength;
            desc->misc[1] = nof_strength;
            desc->misc[2] = nof_pow_strength;
            desc->misc[3] = ramp_colour_nol_strength;
            desc->shadow_softness[0] = shadow_softness;
            desc->shadow_softness[1] = nose_shadow_reduction;
            desc->shadow_softness[2] = hair_highlight_width;
            desc->shadow_softness[3] = hair_highlight_shift;
            desc->reference_modes[0] = hair_masked_highlights ? 1.0f : 0.0f;
            desc->reference_modes[1] = face_angular_sdf ? 1.0f : 0.0f;
            desc->reference_modes[2] = 0.0f;
            desc->reference_modes[3] = 0.0f;
        }

        private static unsafe toon_rig from_desc(scene_api.toon_rig_desc* desc) {
            return new toon_rig {
                day_strength = desc->day[0],
                other_light_offset = desc->day[1],
                other_light_strength = desc->day[2],
                other_light_strength_offset = desc->day[3],
                head_light_day0 = desc->other_light[0],
                head_light_day1 = desc->other_light[1],
                self_ao_shadow_strength = desc->other_light[2],
                ao_strength = desc->other_light[3],
                head_light_colour = new vector_3 { x = desc->other_colour[0], y = desc->other_colour[1], z = desc->other_colour[2] },
                emotion_type = (int)desc->other_colour[3],
                sun_dark_colour = new vector_3 { x = desc->main_dark[0], y = desc->main_dark[1], z = desc->main_dark[2] },
                base_colour_pow = desc->main_dark[3],
                rim_area = desc->rim[0],
                rim_strength = desc->rim[1],
                rim_nolxz_strength = desc->rim[2],
                rim_diffuse_colour_effect = desc->rim[3],
                sss_colour = new vector_3 { x = desc->sss[0], y = desc->sss[1], z = desc->sss[2] },
                sss_pow_strength = desc->sss[3],
                env_strength = desc->env[0],
                env_rotation = desc->env[1],
                specular_strength = desc->env[2],
                diffuse_blend_effect = desc->env[3],
                backlight_strength = desc->misc[0],
                nof_strength = desc->misc[1],
                nof_pow_strength = desc->misc[2],
                ramp_colour_nol_strength = desc->misc[3],
                shadow_softness = (int)desc->shadow_softness[0],
                nose_shadow_reduction = desc->shadow_softness[1],
                hair_highlight_width = desc->shadow_softness[2],
                hair_highlight_shift = desc->shadow_softness[3],
                hair_masked_highlights = desc->reference_modes[0] != 0.0f,
                face_angular_sdf = desc->reference_modes[1] != 0.0f,
            };
        }
    }
}
