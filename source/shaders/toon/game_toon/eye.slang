// ============================ THE GAME'S EYE / IRIS SURFACE (game_toon family) ============================
//
// `game_toon_eye_shade` is the FIRST-PASS port of the original game's eye/iris fragment shader, reconstructed in
// `docs/references/toon/eye_forward_spirv_decompiled.hlsl` from the Vulkan block `shader_e852494d61d6f176_
// lod600_Vulkan_0_block.bin` (record 215,832; module 216,204; keywords _DIFF_RAMP_ON _EMISSION _EYE_HIGHLIGHT
// _MATCAP_ON). It is a separate function rather than a branch in the cloth one because the game's eye is a
// different program: the surface normal is MANUFACTURED from the material's UVs (a cornea hemisphere), the
// albedo lookup is PARALLAXED behind it, the highlight is a MATCAP indexed by that normal rather than a lobe, the
// iris' two region colours are multiplicative tints on the lit layer, and the light/diffuse pair is built from a
// pair of ramp reads on the light and camera axes.
//
// WHAT THIS FUNCTION IS NOT: a claim of visual accuracy. It is the game's formula structure with deren's lighting
// inputs substituted, and every input whose game carrier has no deren equivalent is named at its use with
// `// MISSING:` plus a documented fallback. The Lead runs the A/B.
//
// LINE NUMBERS: every `HLSL nnn` below is a line of `eye_forward_spirv_decompiled.hlsl` AS CHECKED IN. That file's
// provenance header adds 17 lines, so the header's own KEY SITES are +17 here: its "435 normal reconstruction" is
// line 452, its "651 P/F0 formula" is line 668 and its "1113 entry" is line 1130.
//
// THE INPUTS THIS FILE CANNOT TAKE FROM deren, NAMED ONCE SO THE FALLBACK IS ONE EDIT EACH:
//   * `_MatcapNormalScale` (HLSL 453) - the cornea hemisphere's strength. No deren lane (the material's colour
//     lanes carry `_EyeHighLightColor` / `_EyeScatteringColor` / `_Specular` / `_ParallaxScale` and nothing else);
//     the game's value is recorded in `toon_params.slang`'s note (0.9) and is the constant below.
//   * `_MatcapColor` (HLSL 715) - the matcap's tint and its alpha weight. No deren lane (the `toon_colour_lane`
//     enum says why: nothing consumed it), so the neutral white is used.
//   * `_EmissionBrightness` (HLSL 721) - 1.0. deren's emissive slot carries `_EmissionColor * _EmissionMap`.
//   * `_EyeHighLight` (the float that gates the eye's special zone through `_536`) - not carried, and NOT NEEDED:
//     its only read feeds the branch below at HLSL 632-660, which is dead in this compile (see the P/F0 block's
//     note), so no live term depends on it. The two region colours and the matcap are ungated in the game's live
//     path too; deren's lanes for them are neutral when a material states nothing, so no gate is needed here.
//   * The game's per-frame CHARACTER globals (`_19_m79`, `_19_m80`, `_19_m81`, `_19_m85`, `_19_m86`, `_19_m89`..
//     `_19_m92`): the eye-highlight direction/bias, the light and albedo scales, the volume switch and the eye's
//     extra light colours. deren's toon rig has no lanes for any of them; each is resolved below to the identity
//     value that keeps the formula the game's shape (documented at the use).
//   * The game's character DIRECTIONAL-OCCLUSION VOLUME (HLSL 467-626: three Texture3D sampling levels driven by
//     the world position, producing `_1042`..`_1045`). deren has no volume for a character. THE FALLBACK IS THE
//     GAME'S OWN "NO VOLUME" ARM (`_19_m80.y >= 0.5`, HLSL 620-626) rather than an invention: it is the program
//     code for exactly this situation, and it yields `_1042 = 0`, `_1043 = 1`, `_1044 = _19_m81.xyz`, `_1045 =
//     _544`. Only the two globals' values are then deren-side fallbacks.
//
// DELIBERATE BRIDGES (each a difference from the game, not a claim of equality):
//   * the game's vertex TANGENT/TEXCOORD3 (`_6`) and `cross(N, T) * w` frame -> `surface_tbn`'s UV-derivative
//     frame. The glTF files here DO ship TANGENT, but deren's shared frame is the derivative one, and every
//     other family already lives with that substitution. If the cornea's tilt comes out mirrored the frame's
//     bitangent is the one sign to flip.
//   * `_ScreenSpaceShadowMask` (HLSL 688) -> deren `calc_shadow`, except by default 1.0: see the measurement at
//     the shadow read (the eye sits inside the socket and the engine's map reads it as occluded over 100% of the
//     iris). The override constant is the switch, and its comment says which value is which.
//   * `_DiffRampMap` sampling: the game samples the ramp at v = 0.5 (HLSL 694/700) - a single-row 256x1 image, so
//     the v is not a band selector there. deren's lane carries the same image and the stage's own band constant
//     (`character_ramp_band_v`) is used, which lands on the same texel of a 1-row ramp.
//   * a missing per-material `_ShadowColorBrightness` / `_ShadowColorSaturation` (HLSL 440-441) -> the family's
//     `params.albedo_dark_strength` / `albedo_dark_saturation`. deren's eye family deliberately sets the
//     saturation to the no-op 1.0 (the game's iris states 1.4), so that term is quieter here than in the game;
//     it is the family table's documented decision, not this file's.
//   * `_DiffuseBlendEffect` (HLSL 714) -> the rig's `env.w`, the same lane every other family reads.

// ------------------------------------------------------------------------------------------------------------
// THE NAMED FALLBACKS. Each is a value the game states per material or per frame and deren does not carry; they
// are constants rather than literals inside the function so that a Lead A/B is one edit and the provenance stays
// next to the number.
// ------------------------------------------------------------------------------------------------------------

/// `_MatcapNormalScale` (HLSL 453) - how far the cornea hemisphere tilts at the iris' edge.
/// MISSING: no deren lane; 0.9 is the value `toon_params.slang` records for the game's iris material (the game's
/// own 0.125 factor is applied to it below, matching the article's `_CorneaBumpStrength` form).
static const float game_eye_matcap_normal_scale = 0.9;

/// `_MatcapColor.rgb` / `_MatcapColor.a` (HLSL 715) - the matcap's tint and the weight of its RGB.
/// MISSING: no deren lane (see `deren::vulkan::toon_colour_lane`); white with alpha 1 is the neutral, which makes
/// the matcap read `ball.rgb + ball.a` - the game's own two-term form with no artist tint.
static const float3 game_eye_matcap_colour_rgb = float3(1.0, 1.0, 1.0);
static const float game_eye_matcap_colour_alpha = 1.0;

/// `_19_m81.xyz` (HLSL 624) - the character volume's "no volume" colour, which the game's own volume-off arm uses
/// in place of the sampled volume. MISSING: no deren equivalent for the volume; white is the neutral.
static const float3 game_eye_volume_colour = float3(1.0, 1.0, 1.0);

/// `_1045`, the volume's strength, on the same volume-off arm (`_544`, HLSL 460/625). MISSING: 1.0 is the neutral.
static const float game_eye_volume_strength = 1.0;

/// The shadow the eye is shaded by. MISSING: the game reads the frame's screen-space shadow mask at the fragment's
/// pixel (HLSL 688, texture t22 `_ScreenSpaceShadowMask`); deren's nearest input is `calc_shadow`.
/// FALLBACK 1.0 = LIT, and it is the measured answer rather than a preference: an eyeball is INSIDE the socket, so
/// the engine's shadow map's depth at the iris' own screen position belongs to the face plate in front of it -
/// `character_forward.slang` records that the iris reads `shadow = 0.00` over 100% of its pixels while the cheek a
/// centimetre away reads 1.00. The game's mask is the frame's screen-space term applied to the eye's own pixel, and
/// the eye material is lit in the game's reference frames; the engine's cascaded map is the same geometry read
/// against the wrong depth. A negative value takes the engine's `calc_shadow` instead - that is the A/B this
/// constant exists for.
static const float game_eye_shadow_override = 1.0;

// ------------------------------------------------------------------------------------------------------------
// ASSET-SPECIFIC FALLBACK: THE GOO MATCAP LANE
//
// The laevatain iris asset carries NO game eye lanes (its sidecar has only `_GooMatcap05` / `_UseGooMatcap05` /
// `_GooEyeBrightness`). The game eye port above reads `toon_indices.w` (the game matcap lane), which is 0 here, so
// without this branch the iris falls back to the game albedo/ramp path and reads dark red instead of the reference's
// purple. This helper reproduces the Goo reference's iris emission from the lanes the asset DOES carry: the
// `toon_lanes2.y` matcap texture and the `goo_eye_brightness` colour lane. It is a BRIDGE (Goo reference, not the
// decompiled game eye program), used only when the game matcap lane is absent; a model that states the game lanes
// still takes the game path.
// ------------------------------------------------------------------------------------------------------------
static const float game_eye_goo_angle_center = 1.0;
static const float game_eye_goo_window_min = 0.5;
static const float game_eye_goo_window_max = 1.0;
static const float game_eye_goo_ball_weight = 0.5666666626930237;
static const float game_eye_goo_half_turn = 3.141592502593994;
static const float game_eye_goo_forward_sign = -1.0;

static float game_eye_goo_angle(const float3 light_direction, const float3 head_up, const float3 head_right,
                                const float3 head_forward) {
    const float3 projected = light_direction - dot(light_direction, head_up) * head_up;
    const float projected_length = length(projected);
    const float3 axis = projected_length > 1e-6 ? projected / projected_length : float3(0.0, 0.0, 0.0);
    const float right_component = dot(axis, head_right);
    const float forward_component = dot(axis, head_forward) * game_eye_goo_forward_sign;
    const float azimuth_magnitude = abs(right_component) + abs(forward_component);
    const float azimuth_turns = azimuth_magnitude > 1e-12
        ? atan2(right_component, forward_component) / game_eye_goo_half_turn
        : 1.0;
    return 1.0 - abs(azimuth_turns);
}

static float3 game_eye_goo_iris(const surface_sample s, float3 world_pos) {
    const uint4 block2 = toon_lanes2_at(heap_slots_toon_lanes, push.material_index);
    const float4 eyes = toon_colour_at(heap_slots_toon_colours, (push.material_index * character_toon_colour_lanes) + 6u);
    const float3 normal_view = mul(camera_at(heap_camera_slot).view, float4(normalize(s.normal), 0.0)).xyz;
    const float2 matcap_uv = float2(normal_view.x, -normal_view.y) * 0.5 + 0.5;
    const float3 ball = block2.y != 0u
        ? heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + block2.y), heap_sampler_texture, matcap_uv).rgb
        : float3(0.0, 0.0, 0.0);
    const float2 brightness = float2(eyes.x >= 0.0 ? eyes.x : 0.0, eyes.y >= 0.0 ? eyes.y : 0.0);
    const float3 head_up = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).up.xyz);
    const float3 head_right = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).right.xyz);
    const float3 head_forward = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).front.xyz);
    const float angle = game_eye_goo_angle(normalize(light_at(heap_light_slot).light_dir.xyz), head_up, head_right, head_forward);
    const float albedo_weight = clamp(game_eye_goo_angle_center - angle, game_eye_goo_window_min, game_eye_goo_window_max);
    const float3 albedo_layer = s.albedo * albedo_weight;
    const float3 colour = albedo_layer + game_eye_goo_ball_weight * (ball * s.albedo);
    const float strength = lerp(brightness.x, (game_eye_goo_angle_center - angle) * brightness.y, s.alpha);
    return colour * strength;
}

/// @brief the game's eye/iris shading, `eye_forward_spirv_decompiled.hlsl` (record 215,832 / module 216,204),
///        with deren's light/shadow/IBL inputs substituted for the game's frame globals
///
/// @param s the gathered surface (its .toon_indices carry the ramp and matcap lanes; .albedo is NOT used - the
///        game re-reads the base map at the parallaxed uv, and that re-read is the whole point of the eye uv)
/// @param world_pos interpolated world position
/// @param geo_normal interpolated geometric normal (the frame's Z axis; the shading normal is manufactured from UV)
/// @param uv the material's uv (the game's `_3`, and the eye's cornea zone, parallax mask and matcap zone all key
///        off `frac(uv)`)
/// @param is_front front-face flag, used only by deren's double-sided normal rule
float3 game_toon_eye_shade(surface_sample s, float3 world_pos, float3 geo_normal, float2 uv, bool is_front) {
    const ToonRigUBO rig = toon_rig_value();
    const Material mat = material_at(heap_slots_materials, push.material_index);
    const toon_params params = toon_params_for(s.toon_family);
    const uint colour_base = push.material_index * character_toon_colour_lanes;

    // The laevatain iris has no game matcap lane; use the Goo lane the asset states instead of the dark-red
    // game-albedo fallback. See the helper's header for why this is a documented bridge.
    if (s.toon_indices.w == 0u) {
        const uint4 goo_block = toon_lanes2_at(heap_slots_toon_lanes, push.material_index);
        if (goo_block.y != 0u) {
            return game_eye_goo_iris(s, world_pos) + s.emissive;
        }
    }

    // ---- CAMERA, LIGHT AND THE TWO FLATTENED AXES (HLSL 407-412, 457, 677-680) ----
    const float3 camera_pos = camera_at(heap_camera_slot).camera_pos.xyz;
    const float3 view_dir = normalize(camera_pos - world_pos);                                          // `_372`
    const float3 camera_forward = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz); // `_521`
    const float3 light_dir = normalize(light_at(heap_light_slot).light_dir.xyz);                        // `_1216`
    const float main_light_intensity = max(0.001, light_at(heap_light_slot).sun_intensity);
    // `_1230`/`_1234` are the frame's main-light colour and intensity. deren's sun is white with `[render]
    // sun_intensity` as its magnitude (the same bridge `game_toon_cloth_shade` records), so its colour IS the
    // intensity broadcast.
    const float3 light_colour = float3(main_light_intensity, main_light_intensity, main_light_intensity);
    // THE GAME FLATTENS THE LIGHT ON THE OBJECT'S OWN Y before the ramp (`_1245.y = 0`) and flattens the SHADING
    // normal the same way for the highlight (`_548`). deren publishes no object basis and every character here is
    // Y-up in world space, so the bridge is the world Y axis for both - which is exactly what the game's matrix
    // multiplication collapses to when the object's up is world up.
    const float3 flat_light = normalize(float3(light_dir.x, 6.10351562e-05, light_dir.z));              // `_1220`

    // ---- THE UV, THE CORNEA'S ZONE AND ITS HEMISPHERE (HLSL 430-433, 449-453) ----
    //
    // `frac(uv)` is the game's own domain: `_401 = frac(_3)`, `_402 = _401 - 0.5`, `_403 = dot(_402,_402)` is the
    // distance-squared from the eye's centre, and `_404 = step(0.25, _403)` is the line between the two zones -
    // INSIDE the inscribed circle the cornea hemisphere stands, outside it the quad's flat normal does, which is
    // what keeps the curvature (and the catchlight) on the iris rather than on the sclera's rim.
    const float2 frac_uv = frac(uv);
    const float2 centred_uv = frac_uv * 2.0 - 1.0;                                                      // `_492`
    const float2 lens_uv = frac_uv - float2(0.5, 0.5);                                                  // `_402`
    const float dist_sq = dot(lens_uv, lens_uv);                                                        // `_403`
    const float flat_zone = step(0.25, dist_sq);                                                        // `_404`
    const float hemisphere_z = max(1.0000000168623835e-16, sqrt(1.0 - clamp(dot(centred_uv, centred_uv), 0.0, 1.0))); // `_493.z`

    // HLSL 449-455: `_493 = float3(centred, sqrt(1 - |centred|^2))`, `_502 = _493.xy * -_MatcapNormalScale`, and
    // the `lerp(_503 * float3(-0.125, -0.125, 1), float3(0,0,1), _404)` reconstructs the cornea's tangent-space
    // normal: inside the zone the two negations cancel, leaving `xy = 0.125 * scale * centredUV` (the article's
    // `_CorneaBumpStrength` form, and the reason it and `_MatcapNormalScale` are the same socket); outside it the
    // normal is the geometric one.
    // MISSING `_MatcapNormalScale` - see the constant above.
    float3 cornea_ts = float3(centred_uv.x * (game_eye_matcap_normal_scale * 0.125),
                              centred_uv.y * (game_eye_matcap_normal_scale * 0.125),
                              hemisphere_z);
    cornea_ts = lerp(cornea_ts, float3(0.0, 0.0, 1.0), flat_zone);

    // ---- THE MANUFACTURED NORMAL IN THE SURFACE'S OWN FRAME (HLSL 435, 448, 455) ----
    //
    // `_412 = cross(_5, _6.xyz)` and `_481 = float3x3(_6.xyz, _412 * _6.w, _5)` are the game's TBN built from the
    // VERTEX tangent; `mul(_503, _481)` is `corneaTS.x * T + .y * B + .z * N`. deren's equivalent frame is
    // `surface_tbn`'s, so this is `mul(cornea_ts, float3x3(tangent, bitangent, base_normal))` - a documented bridge.
    float3 tangent = float3(1.0, 0.0, 0.0);
    float3 bitangent = float3(0.0, 1.0, 0.0);
    const bool frame_valid = surface_tbn(world_pos, uv, tangent, bitangent);
    float3 base_normal = normalize(geo_normal);
    // `_448` normalises `_5` and multiplies it by `(gl_FrontFacing ? 1 : (-1 + 2*_BackFaceNormalFlip))`. deren's
    // version of that statement is the double-sided rule (`flags` bit 3), which is what a glTF model states.
    if ((mat.flags & 8u) != 0u && !is_front) {
        base_normal = -base_normal;
    }
    float3 normal = base_normal;
    if (frame_valid) {
        normal = normalize(mul(cornea_ts, float3x3(tangent, bitangent, base_normal)));
    }

    // ---- THE PARALLAX (HLSL 434-436) ----
    //
    // `_427 = _3 - normalize(TBN * viewDir).xy * _ParallaxScale * float2(1, 0.25) * smoothstep(0.25, 0.05,
    // _403)`: the iris' albedo slides behind the cornea by the view direction's TANGENT-SPACE xy, with the
    // quarter-strength y the game writes out, and only inside the cornea zone (full at the centre, zero by the
    // circle's edge - which is what keeps the sclera from sliding). `_ParallaxScale` is deren's
    // `toon_colour_lane::parallax_scale` (lane 5), whose `.x < 0` sentinel means "the asset states nothing" and
    // resolves to the stage's own constant, exactly as `character_forward.slang` reads it.
    float2 parallax_offset = float2(0.0, 0.0);
    if (frame_valid) {
        const float parallax_scale = toon_colour_at(heap_slots_toon_colours, colour_base + 5u).x;
        const float parallax_depth = parallax_scale >= 0.0 ? parallax_scale : character_eye_parallax_depth;
        const float3 view_ts = float3(dot(tangent, view_dir), dot(bitangent, view_dir), dot(base_normal, view_dir));
        const float2 offset_dir = normalize(view_ts).xy;
        // the game's `smoothstep(0.25, 0.05, _403)` has its edges REVERSED; it is written here with increasing
        // edges and the complement, which is the same polynomial without relying on undefined edge order.
        const float parallax_mask = 1.0 - smoothstep(0.05, 0.25, dist_sq);
        parallax_offset = offset_dir * parallax_depth * float2(1.0, 0.25) * parallax_mask;
    }
    const float2 eye_uv = uv - parallax_offset;                                                         // `_427`

    // ---- ALBEDO, ITS DARK LAYER AND EMISSION (HLSL 437-444, 721) ----
    const float4 base_texel = frame_texture_sample(mat.tex_indices.x, eye_uv);
    const float3 albedo = mat.base_color_factor.rgb * base_texel.rgb;                                   // `_438`
    const float albedo_alpha = mat.base_color_factor.a * base_texel.a;                                  // `_444`
    // HLSL 440-441: `_449 = _438 * _ShadowColorBrightness`, then
    // `_453 = lerp(luma(_449).xxx, _449, _ShadowColorSaturation)` - the eye's DARK ALBEDO, brightness and
    // saturation as the material states them. deren has no carrier for either property; the family's
    // `albedo_dark_strength` / `albedo_dark_saturation` are the documented bridge (see the header).
    const float3 dark_scaled = albedo * params.albedo_dark_strength;
    const float3 dark_albedo = lerp(float3(toon_luminance(dark_scaled)), dark_scaled, params.albedo_dark_saturation);
    // HLSL 721: `(_EmissionMap * _EmissionColor).xyz * _EmissionBrightness`, sampled at the SAME parallaxed uv.
    // deren's emissive slot and factor are the equivalents of the map and the colour; `_EmissionBrightness` has no
    // lane, and 1.0 is the game's own default (MISSING at the constant list).
    float3 emission = float3(0.0, 0.0, 0.0);
    if ((s.flags & 4u) != 0u) {
        emission = mat.emissive_factor.rgb * frame_texture_sample_lod0(mat.emissive_index, eye_uv).rgb;
    }

    // ---- THE P/F0 BLOCK (HLSL 668-670) ----
    //
    // `_1174 = 0.96 - 0.96 * _1172` is the metallic energy distribution - the same expression the cloth
    // reconstruction names `energyDistribution_metallic` - and `_1175` / `_1176` are the two albedo layers scaled
    // by it (the lit one and the dark one). `_1172` is the material's metallic float; deren's equivalent is the
    // glTF metallic factor (`s.metallic`), which `M_actor_laevat_iris_01` states as 0 - see the notes.
    // THE `0.308` / `0.88` LERPS AT HLSL 657-658 ARE NOT PORTED: they belong to the branch at HLSL 632-660 whose
    // weight `_1120 = smoothstep(2 - _1115, 2.35 - _1115, 0) * gl_FrontFacing` is identically zero (its x is 0 and
    // its edges are >= 1), so the whole branch is dead in this compiled variant - including the triplanar detail
    // normal at HLSL 645-656. See the notes' "uncovered / not ported" table.
    const float energy_distribution_metallic = 0.96 - 0.96 * saturate(s.metallic);
    const float3 lit_albedo = albedo * energy_distribution_metallic;                                    // `_1175`
    const float3 dark_layer = dark_albedo * energy_distribution_metallic;                               // `_1176`
    const float3 dark_in_dark = dark_layer * params.dark_in_dark;                                       // `_1276` (game's 0.65)

    // ---- THE IRIS TINT (HLSL 691-693) ----
    //
    // `_1285 = _EyeHighLightColor * _404`, `_1292 = _EyeScatteringColor * _444`, and
    // `_1295 = _1175 * ((1 - _404) + _1285) * ((1 - _444) + _1292)` - i.e. `lerp(1, colour, weight)` per zone:
    // the highlight colour tints the eye's CORNERS (the zone outside the inscribed circle) and the scattering
    // colour tints wherever the base map's ALPHA says the surface is thin. Both lanes are deren's own
    // (`toon_colour_lane::eye_highlight` / `eye_scattering`, whose neutral is white = no tint), read exactly as
    // `character_forward.slang` reads them.
    const float3 eye_highlight_colour = toon_colour_at(heap_slots_toon_colours, colour_base + 1u).rgb;
    const float3 eye_scattering_colour = toon_colour_at(heap_slots_toon_colours, colour_base + 2u).rgb;
    const float3 tinted_albedo = lit_albedo *
                                 lerp(float3(1.0, 1.0, 1.0), eye_highlight_colour, flat_zone) *
                                 lerp(float3(1.0, 1.0, 1.0), eye_scattering_colour, albedo_alpha);

    // ---- THE TWO RAMP READS: ONE ON THE LIGHT AXIS, ONE ON THE CAMERA AXIS (HLSL 694-700) ----
    //
    // The game samples ONE map twice: U is the NoL (light flattened on the object's Y, `_1247`) for the diffuse
    // ramp and the NoF (the same normal against the camera's forward, `_521`) for the second read, and its `.rgb`
    // is the ramp's colour while `.w` is the artist's lightness. deren's `toon_indices.x` is that same texture
    // (`_DiffRampMap`) - see the header on the v coordinate.
    const float3 flat_normal = normalize(float3(normal.x, 6.10351562e-05, normal.z));                  // `_548`
    const float ramp_u_nol = saturate(dot(normal, flat_light) * 0.5 + 0.5);                             // `_1309`
    const float ramp_u_nof = saturate(dot(normal, camera_forward) * 0.5 + 0.5);                         // `_1327`
    float3 ramp_colour = float3(1.0, 1.0, 1.0);
    float ramp_nol_weight = 1.0;
    float ramp_nof_weight = 1.0;
    if (s.toon_indices.x != 0u) {
        const float4 ramp_nol = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.x),
                                          heap_sampler_texture, float2(ramp_u_nol, character_ramp_band_v));
        ramp_colour = ramp_nol.rgb;
        ramp_nol_weight = ramp_nol.w;                                                                   // `_1310`
        ramp_nof_weight = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.x),
                                     heap_sampler_texture, float2(ramp_u_nof, character_ramp_band_v)).w; // `_1328`
    } else {
        // MISSING: the ramp lane is EMPTY on `chars\laevatain_goo.glb`'s iris (its sidecar states no
        // `_DiffRampMap`), while the game reads this map unconditionally. FALLBACK: the family's own procedural
        // edge - the same shape `game_toon_cloth_shade` uses for a material with no ramp - keeping a NEUTRAL ramp
        // colour (1,1,1) rather than inventing a tint, so the ramp's colour term below is an identity.
        ramp_nol_weight = smoothstep(params.center - params.softness, params.center + params.softness, ramp_u_nol);
        ramp_nof_weight = smoothstep(params.center - params.softness, params.center + params.softness, ramp_u_nof);
    }
    const float ramp_nol_lit = min(1.0, ramp_nol_weight);          // `_1334` / `_1340`
    const float ramp_colour_strength = max(max(ramp_colour.x, ramp_colour.y), ramp_colour.z) -
                                       min(min(ramp_colour.x, ramp_colour.y), ramp_colour.z);            // `_1319`

    // ---- THE EYE'S HIGHLIGHT STRENGTH (HLSL 704) ----
    //
    // `_1338 = ((clamp(dot(_548, _19_m85.xyz) + _19_m86.x, 0, 1) * _19_m86.y) + _19_m86.z) * lerp(_1044, 1,
    // (_19_m80.y * _1334))` - a per-frame highlight direction and a bias, whitened by the volume's colour.
    // MISSING `_19_m85.xyz` (direction), `_19_m86` (offset / gain / bias) and `_19_m80.y` (volume switch): no deren
    // rig lane carries any. FALLBACK: the sun direction flattened exactly like the normal, and the identity bias
    // (offset 0, gain 1, bias 0), with the game's own volume-off `_1044`.
    const float3 volume_tint = lerp(game_eye_volume_colour, float3(1.0, 1.0, 1.0), ramp_nol_lit);       // `_1044`
    const float3 eye_highlight = float3(saturate(dot(flat_normal, flat_light))) * volume_tint;           // `_1338`

    // ---- THE SHADOW (HLSL 688) ----
    // MISSING the frame's screen-space mask; the override's comment says why it defaults to LIT and how to take
    // deren's `calc_shadow` instead.
    const float shadow_scene = game_eye_shadow_override >= 0.0
                                   ? game_eye_shadow_override
                                   : saturate(calc_shadow(world_pos, geo_normal));                      // `_1267`

    // ---- THE LIGHT TERM (HLSL 686-707) ----
    //
    // `_1364 = lerp(shade_side, lit_side, shadow)` with the game's `_1267` as the third argument (1 = LIT, which is
    // the convention deren's `calc_shadow` returns too). BOTH sides are the `_OtherLight`-style block the game's eye
    // has INSTEAD of an ambient/IBL: the lit side is the sun's colour lerped with its own luminance by the ramp's
    // weight plus the highlight, the shadow side is the highlight alone - which is what makes a shaded iris read as
    // a lit ball of colour rather than as a dark disc.
    // MISSING `_19_m79.y` / `_19_m79.w` / `_19_m80.x` (the two light-side scales and the volume switch): each is
    // resolved to its identity (1.0, 1.0, and the `min(lerp(0.65, 1, _1045), 1.5)` arm that keeps the lit side's
    // scale at 1.0 with the volume-off `_1045`). `_19_m91.y` (a light-colour blend on the highlight) is the
    // identity at 0.
    const float3 lit_light = lerp(float3(toon_luminance(light_colour)), light_colour, ramp_nol_lit) +
                             eye_highlight * clamp(game_eye_volume_strength, 0.0, 1.5);
    const float3 shade_light = eye_highlight * min(lerp(0.65, 1.0, game_eye_volume_strength), 1.5);
    const float3 light_term = lerp(shade_light, lit_light, shadow_scene);                                // `_1364`

    // ---- THE DIFFUSE ALBEDO (HLSL 708-710) ----
    //
    // `_1365`: the dark-in-dark layer pulled to 1.2x contrast, then the dark layer by the two ramp weights, then
    // the TINTED lit albedo by the light-axis ramp weight. `_1371 = _1365 * (_1309.rgb * _1319 + 1 - _1319)` is the
    // ramp's colour mapping (its strength is the sampled colour's own chroma), and `_1380` puts them together with
    // the game's `rampControl` - the ratio of the albedo's luminance to the coloured one, clamped to 1.5.
    const float3 dark_mix = lerp(float3(toon_luminance(dark_in_dark)), dark_in_dark, 1.2);
    const float3 albedo_mix = lerp(lerp(dark_mix, dark_layer, saturate(ramp_nof_weight + ramp_nol_weight)),
                                   tinted_albedo, ramp_nol_lit);
    const float3 ramp_effect = ramp_colour * ramp_colour_strength + float3(1.0 - ramp_colour_strength); // `_1371`
    const float3 coloured_albedo = albedo_mix * ramp_effect;
    const float ramp_control = clamp(toon_luminance(albedo_mix) / max(toon_luminance(coloured_albedo), 0.001), 0.0, 1.5);
    const float3 diffuse_term = lerp(lerp(dark_layer, tinted_albedo, ramp_nof_weight),
                                     coloured_albedo * ramp_control, shadow_scene);                     // `_1380`

    // ---- THE MATCAP (HLSL 713-715) ----
    //
    // A MATCAP IS INDEXED BY WHERE THE SURFACE POINTS RELATIVE TO THE CAMERA, which for this family means the
    // manufactured cornea normal - the game transforms `mul(_503, _481)` by the VIEW matrix and takes the
    // normalised xy (`HLSL 713`). deren's `toon_matcap_uv` builds the same sphere map from the same two vectors
    // without needing the matrix, and its note records why the V sign is what it is on these textures.
    // `_1425`-style blend of the two terms (`ball.rgb * _MatcapColor.a + _MatcapColor.rgb * ball.a`) is the
    // game's own, and the matcap is multiplied by the light and by a shadow-gated `(_1386 * 0.5 + 0.5)` - the
    // program's way of keeping the catchlight out of the fully shadowed side.
    float3 matcap_term = float3(0.0, 0.0, 0.0);
    if (s.toon_indices.w != 0u) {
        const float4 ball = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.w),
                                      heap_sampler_texture, toon_matcap_uv(normal, camera_forward));
        // MISSING `_MatcapColor` - see the constant pair above.
        const float3 matcap_brdf = ball.rgb * game_eye_matcap_colour_alpha + game_eye_matcap_colour_rgb * ball.a;
        const float matcap_gate = lerp(ramp_nof_weight, ramp_nol_lit, shadow_scene);                    // `_1386`
        matcap_term = matcap_brdf * light_term * (matcap_gate * 0.5 + 0.5);
    }

    // ---- THE COMPOSITION (HLSL 714-721) ----
    //
    // `_1425 = (1 - _48_m6) + _444 * _48_m6` is the article's diffuse/specular blend on the albedo's alpha, and
    // `_48_m6` is the material's `_DiffuseBlendEffect`, which deren carries on the rig (`env.w`) for every family.
    const float diffuse_specular_blend = 1.0 - rig.env.w * (1.0 - albedo_alpha);                        // `_1425`
    const float3 main_colour = (light_term * diffuse_term) * diffuse_specular_blend + matcap_term;       // `_1427`

    // the game's brightness lift: `_1428 = luma(_1427)`, `_1431 = clamp(_1428 - 0.5, 0, 0.5)`,
    // `_1494 = lerp(_1428.xxx, _1427, _1431*_1431 + 1)`. It is a contrast boost around mid-grey, not a tone map.
    const float main_luma = toon_luminance(main_colour);                                                 // `_1428`
    const float luma_lift = clamp(main_luma - 0.5, 0.0, 0.5);                                            // `_1431`
    float3 result = lerp(float3(main_luma), main_colour, luma_lift * luma_lift + 1.0);

    // ---- THE SPECULAR HALF OF HLSL 721 ----
    //
    // `_1436 = dot(flat_light, normal)` and `_1447 = dot(view, normal)`; the game then builds a fresnel-band mask
    // `smoothstep(0.6, 0.8, 1 - |NoV|)`, a back-light weight and a shadow-side gate, and multiplies them by the
    // LIT ALBEDO floored at 0.15 - i.e. the eye's "shine" is its own albedo, the same arrangement the P/F0 block
    // above makes (the eye has no separate F0 term in this variant).
    // The volume terms the game multiplies in are the volume-off arm's: `_1043 = 1` (white) and `_1042 = 0`, the
    // latter replaced here by the game's own half-lambert-like polynomial so the term survives with its shape.
    // MISSING `_19_m91.x` (a specular switch, identity at 0) and `_19_m92` (the eye's extra light colours, whose
    // deren fallback below drops the additive block it drives - see the tail).
    const float no_v = dot(view_dir, normal);                                                           // `_1447`
    const float no_l = dot(flat_light, normal);                                                         // `_1436`
    const float shadow_weight = 1.0 - shadow_scene;                                                     // `_1453`
    const float spec_ndot = lerp(0.0, -no_l * (no_l * 0.5 - 1.0) + 0.5, shadow_scene);
    const float2 camera_xz = normalize(camera_forward.xz + float2(1e-6, 0.0));
    const float back_light = shadow_weight + saturate(-dot(flat_light.xz, camera_xz)) * shadow_scene;
    // the game's `smoothstep(0.1, 0.04, luma(lit_albedo))`: reversed edges, rewritten with increasing edges and
    // the complement so the polynomial is the same.
    const float dark_side = shadow_weight + (1.0 - smoothstep(0.04, 0.1, toon_luminance(lit_albedo))) * shadow_scene;
    const float fresnel = smoothstep(0.6, 0.8, 1.0 - abs(no_v));
    const float3 spec_light = lerp(float3(1.0, 1.0, 1.0), light_colour, shadow_scene);                  // `_1043` = 1
    result += spec_light * saturate(spec_ndot) * back_light * fresnel * dark_side *
              max(float3(0.15, 0.15, 0.15), lit_albedo);

    // ---- THE TAIL OF HLSL 721: EMISSION (AND WHAT THE GLOBAL EYE-LIGHT BLOCK WOULD ADD) ----
    //
    // The game adds `emission + (_EyeHighLightColor * _19_m92.y + _EyeScatteringColor * _19_m92.z +
    // _1171 * _19_m92.x) * _1425` - a per-frame eye light on the two region colours. MISSING `_19_m92.xyz`: the
    // deren fallback is ZERO rather than a colour invented there, so that additive block contributes nothing and
    // only the emission survives. Wiring `_19_m92` to a deren light colour is the edit that restores it.
    result += emission;

    return result;
}
