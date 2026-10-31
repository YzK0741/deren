// ================== game_toon/cloth.slang - CAPTURED GAME_TOON VIEWER, FAMILY_CLOTH ==================
//
// SOURCE OF TRUTH (read-only capture, "the captured source is the appearance target"):
//   tmp/game_toon-studio-audit/final/shaders/character.frag.readable.glsl  (cited below as `readable:NNN`)
//   tmp/game_toon-studio-audit/final/shaders/aliases.json
//   tmp/game_toon-studio-audit/final/materials/M_actor_laevat_cloth_0{1..5}.json
//   tmp/game_toon-studio-audit/final/comparison-summary.json + 2fcd9c7ad0-1619b7616e7ad936.json-laevat.json
//   tmp/game_toon-studio-audit/final/viewer-lighting.js, material-feature-defines.js, material-uniform-bindings.js
//
// WHAT THIS IS: the viewer's ONE template has FAMILY_SKIN / FAMILY_HAIR / FAMILY_CLOTH / FAMILY_EYE branches.
// This file is the CLOTH branch plus the shared helpers it calls, on deren's adapters, and returns LINEAR HDR
// radiance (no tone map, no output sRGB encode - post pass mode 2 owns both; CONTRACT §Required API).
//
// WHICH CAPTURED BRANCHES ARE ACTIVE FOR LAEVATAIN'S CLOTH (evidence, not assumption):
//   FAMILY_CLOTH                        family = cloth (material JSON name / feature-defines `Ym.cloth = "Q30"`)
//   USE_BASE_MAP                        the glTF material has a baseColorTexture (`Jm` -> Q21, readable:62-70)
//   DOUBLE_SIDED                        every cloth material states `doubleSided: true` (readable:694-698, 722)
//   USE_NORMAL_MAP + USE_TANGENT        `_UseBumpMap = 1` and a `_BumpMap` image (readable:712-722)
//   USE_DIFFUSE_RAMP (Q243)             `_UseDiffRampMap = 1` + `_DiffRampMap` image (readable:929-965)
//   USE_SHADOW_LUT   (Q273)             cloth_01 ONLY: `_UseShadowLutTex = 1` and a 1024=32*32 strip
//                                       (readable:904-910; feature-defines line 1)
//   USE_SPECULAR_RAMP (Q38)             `_UseSpecRampMap = 1` + image (readable:1158-1171)
//   metallic gloss Q84 + Q13            `_UseMetallicGlossMap = 1` + image (readable:773-782, 930-935)
//   emission Q158                       cloth_01, cloth_02, cloth_05 (`_UseEmission = 1` + `_EmissionMap`)
//   USE_SATURATION_LIFT (Q178)          viewer default `saturationLift: true` (`viewer-lighting` builder
//                                       `new r3` -> `this.saturationLift = !0`; feature-defines last line)
//   NOT Q132 (main-light self-shadow)   viewer default `selfShadow: false` and `laevat.volume.mainLightShadow
//                                       = false`, so the captured lit weight has NO shadow-map term.
//   NOT Q299 / Q141 (the metallic-gloss cloth NPR path): `w` requires NOT Q243, and every Laevatain cloth has
//                                       `_UseDiffRampMap = 1`, so Q243 is emitted and Q299/Q141 are not.
//   NOT Q118 silk, NOT Q100 customize, NOT Q247 extra-alpha, NOT Q11 parallax, NOT Q42/Q142 blend alpha path:
//                                       `_SilkStockings`, `_AvatarCustomizeEnable`, `_UseUV2Color`,
//                                       `_UseParallax` are 0.0 on all five cloth materials.
//
// ---- THE LANE PACKING THIS REVISION IS WRITTEN AGAINST, AND WHAT IT COMPLETED ----
//
// The authority for the captured per-material payload is `tmp/game_toon-reference/assets/uniform-packing.json` (the
// staged `laevatain_game_toon_preview.glb` sidecar - the same file `game_toon_profile.h` names as its authority),
// and it assigns ALL sixteen profile lanes. This revision reconciles the cloth branch with that packing and
// completes the captured sub-branches it names, each with its captured lines:
//   * `Q302 _BumpScale` (readable:716) -> profile lane 6`.z`, replacing the glTF `normal_scale` proxy;
//   * `Q347`/`Q348` shadow brightness/saturation (readable:908) -> lane 5`.x`/5`.y`, replacing lane 7 (which is
//     the hair anisotropy row, not a cloth value);
//   * the silk values `Q238`/`Q349`/`Q190` (readable:911-917) -> lane 12`.z`/12`.w`, lane 13, lane 14`.rgb`;
//   * `Q20 _AlphaPremultiply` (readable:1148) -> lane 14`.w`, applied to the emission term as the capture does;
//   * `Q215 uRampRangeBias` (readable:892) -> lane 1`.a`;
//   * the ramp composition's chroma and tint now read ONE display-space value (readable:962-963);
//   * `_EmissionBrightness` (readable:1148) is folded into `Material::emissive_factor` at staging, because the
//     packing gives it no lane and `s.emissive` is the standard glTF factor (see the constants block);
//   * `Q118` silk and `Q100` customize have no lane in the packing and stay at the captured enable 0.0
//     (readable:911 / readable:663) - both formulas are transcribed verbatim behind that constant.
//
// HARD BOUNDARY: every number below is cited to the capture (file + line/field) or to a deren profile lane.
// Where a captured value has no whose lane, the fallback is the captured constant and is marked
// `// MISSING PROFILE LANE`. The 10 studio punctual lights (`R114`, readable:188-327) are NOT ported - they are
// the capture's SCENE rig (RimLight_2/SpecLight_1/Point Light_overview/FogLight_1 in
// 2fcd9c7a...json-laevat.json), not a material response; deren's own lights cannot reproduce them.

#ifndef DEREN_GAME_TOON_GAME_TOON_CLOTH_SLANG
#define DEREN_GAME_TOON_GAME_TOON_CLOTH_SLANG

// The profile header is included by `game_toon.slang` before this file per CONTRACT; the include here is
// guarded and keeps the file self-contained for a direct compile check.
#include "toon/game_toon/captured/encoding.h"
#include "toon/game_toon/game_toon_profile.h"
#include "toon/game_toon/captured/game_toon_lights.h"

// ---- CAPTURED COMPILE-TIME CONSTANTS (viewer globals, not per material) ----
// VIEWER_LIGHT_SCALE: readable:421 `const float VIEWER_LIGHT_SCALE= 0.2877;`; same value in
// comparison-summary.json `shader_constants."o3"` and viewer-lighting.js (`o3=.2877`).
static const float game_toon_cloth_viewer_light_scale = 0.2877;
// Q22: viewer-lighting.js uniform initializer `Q22:{value:.5}`; read by the cloth spec block (readable:1169).
static const float game_toon_cloth_viewer_spec_scale = 0.5;
// VIEWER_AMBIENT: readable:420. It enters the SKIN light function only (`R208`, readable:845); the cloth light
// formula (readable:1006) does NOT read it - recorded here so the absence is a statement, not an omission.
static const float game_toon_cloth_viewer_ambient = 0.725;

// ---- CAPTURED LAEVATAIN VOLUME VALUES (2fcd9c7a...json-laevat.json `volume`; comparison-summary.json
//      `laevat_volume`) - used where the profile lane is absent, each marked `// MISSING PROFILE LANE` ----
// The captured camera-relative light pair (volume.yawBias = 10, volume.minPitch = 32) is recorded in the
// section-4 comment only: this port takes deren's sun direction (the contract's fallback), so it is not read.
static const float game_toon_cloth_captured_volume_mult = 1.05;      // volume.multiplier
static const float game_toon_cloth_captured_env_mul = 0.55;          // volume.envMul (profile lane 0.z)
static const float3 game_toon_cloth_captured_shadow_tint = float3(0.8783, 0.9302, 1.1217); // volume.shadowTint
static const float game_toon_cloth_captured_main_light_intensity = 2.0; // viewer-lighting `zf = 2` (Q143)
// `Q215` `uRampRangeBias` now reads profile lane 1`.a` directly (`game_toon_range_bias`); its zero word IS this
// captured value (`volume.rangeBias = 0.0`), so the constant documents the captured number rather than supplying
// a separate fallback branch.
static const float game_toon_cloth_captured_range_bias = 0.0;        // volume.rangeBias (Q215)
// Per-material, and identical on all five Laevatain cloth materials (material JSON `extras.floats`). The FINAL
// profile packing (tmp/game_toon-reference/assets/uniform-packing.json rows 5.x/5.y) carries the captured JS
// defaults (`_ShadowColorBrightness ?? .5`, `_ShadowColorSaturation ?? 1`), which are exactly these numbers:
static const float game_toon_cloth_captured_shadow_brightness = 0.5;  // `_ShadowColorBrightness`, readable:908
static const float game_toon_cloth_captured_shadow_saturation = 1.0;  // `_ShadowColorSaturation`, readable:908

// ---- THE CAPTURED-INACTIVE BRANCHES AND THEIR ENABLES (readable:911-917 `Q118`, readable:88-102/663 `Q100`) ----
//
// `_SilkStockings` and `_AvatarCustomizeEnable` are real rows in the captured material JSON and both are `0.0` on
// all five Laevatain cloth materials, which is exactly why `material-feature-defines.js` emits neither Q118 nor
// Q100 for them. They have NO lane in the final profile packing (its rows 0..15 are fully assigned: 5 =
// Q347/Q348/Q291/Q301, 12 = Q152/Q238, 13 = Q349, 14 = Q190/Q20, 15 = Q93), so both branches below are
// transcribed VERBATIM and left at the captured enable value - which is the capture's answer for these
// materials, not a fallback. Turning either on needs a packing change (the Lead), not a shader constant.
static const float game_toon_cloth_captured_silk_stockings = 0.0;    // `_SilkStockings`
static const float game_toon_cloth_captured_customize_enable = 0.0;  // `_AvatarCustomizeEnable`

// `Q15 = _EmissionColor * _EmissionBrightness ?? 1` (material-uniform-bindings `Q3`, read at readable:1148) is the
// STANDARD glTF emissive factor in deren's staging (`uniform-packing.json` `existing_lanes.Q15`): the brightness is
// folded into `Material::emissive_factor` when the game_toon asset is staged, so `surface_sample::emissive` ALREADY
// carries it. There is no lane left to read a separate brightness from, and multiplying one would double it - the
// captured values (cloth_01 4.0, cloth_02 9.0, cloth_03 5.8999, cloth_04 5.8999, cloth_05 9.0, material
// `extras.floats._EmissionBrightness`) are therefore a STAGING table entry, recorded here so they are not lost.

/// @brief luminance with the viewer's weights: readable:422-424 `R218`
static float game_toon_cloth_luma(float3 colour) {
    return dot(colour, float3(0.212673, 0.715152, 0.072175));
}

/// @brief the viewer's `R113` (readable:58-61): `max(mix(vec3(luma), colour, saturation), 0)`
/// @note this is the shadow-side fallback of a material with NO `_ShadowLutTex` (readable:908); the five
///       Laevatain cloth materials all state `_ShadowColorSaturation = 1.0`, at which the mix is the colour.
static float3 game_toon_cloth_saturation_to_luma(float3 colour, float saturation) {
    return max(lerp(float3(game_toon_cloth_luma(colour), game_toon_cloth_luma(colour), game_toon_cloth_luma(colour)),
                    colour,
                    saturation),
               float3(0.0, 0.0, 0.0));
}

/// @brief the viewer's `sampleShadowLutLinear`, readable:410-419, through deren's `shadow_lut` lane
/// @param albedo_linear the LINEAR base colour (readable:412 encodes it to display space for the coordinate)
/// @param lane deren's `toon_slot::shadow_lut` lane (0 = the caller must not call this)
///
/// THE COORDINATE IS THE VIEWER'S, FIELD FOR FIELD, with `n = uShadowLutSize` = the LUT image HEIGHT
/// (material-uniform-bindings `Q316` = `i.image.height`), and the strip is `n` tiles of `n`x`n` (1024x32 for
/// n=32, the layout measured for the game's LUTs; the reader queries it so a differently sized strip still
/// addresses the same cube). `v = 1 - (g+0.5)/n` already contains the archive export's vertical flip: it is the
/// same value as `game_toon/face_frame.h`'s `game_face_exported_lut_v` at n=32, which is why the CONTRACT says
/// to sample the staged texture exactly as `game_toon/face.slang`'s `game_face_shadow_lut` does.
///
/// NO MANUAL sRGB DECODE HERE: deren uploads this lane as `rgba8_srgb` (`runtime.constructor.cppm`) so the
/// sampler already returns linear - which is exactly what the viewer's trailing `srgbToLinear(...)` (readable:418)
/// computes from a non-sRGB sample. Decoding again would be the double-decode the CONTRACT forbids.
static float3 game_toon_cloth_shadow_lut(float3 albedo_linear, uint lane) {
    uint width = 0u;
    uint height = 0u;
    uint levels = 0u;
    heap_at<Texture2D<float4>>(scene_texture_base() + lane).GetDimensions(0u, width, height, levels);
    const float n = max(float(height), 1.0);
    const float3 c = saturate(toon_linear_to_srgb(albedo_linear)) * (n - 1.0); // readable:412
    const float tile0 = floor(c.b);                                             // readable:413
    const float tile1 = min(tile0 + 1.0, n - 1.0);                              // readable:414
    const float v = 1.0 - (c.g + 0.5) / n;                                      // readable:415
    const float u0 = (tile0 * n + c.r + 0.5) / (n * n);                         // readable:416
    const float u1 = (tile1 * n + c.r + 0.5) / (n * n);                         // readable:417
    const float3 s0 = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + lane), heap_sampler_texture, float2(u0, v)).rgb;
    const float3 s1 = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + lane), heap_sampler_texture, float2(u1, v)).rgb;
    return lerp(s0, s1, c.b - tile0);                                           // readable:418
}

/// @brief the packed tangent-space normal decode of readable:712-719
/// @param bump the `_BumpMap` texel
/// @param bump_scale `Q302` = `_BumpScale` (uniform wiring `R3`: `Q302 = _BumpScale ?? 1`). deren's carrier is
///        profile lane 6`.z` (`uniform-packing.json` row 6: `Q302 _BumpScale ?? 1`); the `_BumpScale` row is 1.0
///        on all five Laevatain cloth materials, which is also the captured default, so a zero lane ("not
///        stated") uses 1.0. `Material::normal_scale` is the glTF-side equivalent, NOT the captured row - the
///        packing is the authority for the captured value.
/// @note the packing is `vec2(bump.r*bump.a, bump.g)*2-1` with z reconstructed from sqrt(1-dot) BEFORE the
///       scale is applied (`R135` is scaled after `R254` is computed) - the game-export convention, and NOT
///       the `rgb*2-1` decode whose `gather_surface` uses (that one trusts a blue channel these maps store as 0).
static float3 game_toon_cloth_unpack_normal(float4 bump, float bump_scale) {
    float2 rg = float2(bump.r * bump.a, bump.g) * 2.0 - 1.0;                 // readable:714
    const float z = sqrt(max(1.0 - min(dot(rg, rg), 1.0), 0.0));             // readable:715
    rg *= bump_scale;                                                        // readable:716
    return float3(rg, z);
}

/// @brief the reserved captured-material payload lanes (`game_toon_profile.h`: lanes 5..15)
/// @note this file reads them through the header's generic lane reader so the packing lives in ONE block here
///       and in the notes; the Lead can promote any of them to a named accessor in `game_toon_profile.h` once.
static float4 game_toon_cloth_payload(uint material_index, uint lane) {
    return game_toon_profile_lane(material_index, lane);
}

/// @brief the CAPTURED viewer cloth surface response for one fragment, on deren's adapters
/// @param s the G-buffer surface (whose albedo/normal/alpha are used; see the calls below for what is re-derived)
/// @param world_pos interpolated world position
/// @param geo_normal interpolated geometric normal
/// @param uv the material's uv
/// @param is_front the rasterizer's front/back decision (the captured viewer flips N on backsides, readable:694-698)
float3 captured_cloth_shade(surface_sample s, float3 world_pos, float3 geo_normal, float2 uv, bool is_front) {
    const Material mat = material_at(heap_slots_materials, push.material_index);
    const uint colour_base = push.material_index * character_toon_colour_lanes;

    // ========================================================================================================
    // 1. THE MATERIAL LANES THIS BRANCH READS (presence is the enable word; 0 = "do not read")
    // ========================================================================================================
    //
    // THE LANE ACCESSORS ARE THE REPO'S, NOT THE CONTRACT COMMENT'S SHORTHAND: `toon_lanes_at` is the material
    // record's `toon_indices` (lanes 0..3 = diffuse ramp, shadow LUT, specular ramp, matcap), `toon_lanes2_at`
    // is the beside-the-record block (lanes 4..7 = SDF lightmap, METALLIC/GLOSS, SDF mask, emotion). The
    // contract's "x = face SDF, y = metallic/gloss, ..." line describes block 1; this file uses the macro that
    // actually reaches it, `toon_lanes2_at(...).y`.
    const uint metallic_gloss_lane = toon_lanes2_at(heap_slots_toon_lanes, push.material_index).y;
    const bool has_metallic_gloss = metallic_gloss_lane != 0u;
    const bool has_diffuse_ramp = s.toon_indices.x != 0u;
    const bool has_shadow_lut = s.toon_indices.y != 0u;
    const bool has_spec_ramp = s.toon_indices.z != 0u;
    const bool has_normal_map = (mat.flags & 1u) != 0u;
    const bool has_emission = (mat.flags & 4u) != 0u;
    const bool is_double_sided = (mat.flags & 8u) != 0u;

    // ========================================================================================================
    // 2. BASE COLOUR (readable:657-663 + helpers 62-70, 88-102)
    // ========================================================================================================
    //
    // `base = R99(R38(uBaseMap, uBaseColour, uOpacity, R105), vUv)`; `R38` = `vec4(uBaseColour,uOpacity) *
    // texture2D(uBaseMap, uv)`, whose equivalent is gathered by deren as `surface_sample::albedo` (base colour
    // factor `_BaseColor` times the sRGB base map). The parallax variant `R105 = R193(...)` is Q201/Q81 (off).
    float3 base = s.albedo;
    const float base_alpha = s.alpha; // `base.a`; only the silk block reads it in this branch

    // ---- CUSTOMIZE (`Q100`; `R99`, readable:88-102, applied at readable:663) ----
    // CAPTURED INACTIVE: `_AvatarCustomizeEnable = 0.0` on all five Laevatain cloth materials, so Q100 is not
    // emitted and the capture never evaluates this. Its three uniforms are `y3`'s Q313/Q328/Q155, whose captured
    // JS defaults are white (`?? [1,1,1]`), and neither the enable nor the three colours has a lane in the final
    // profile packing - hence the captured enable constant and the captured defaults below.
    if (game_toon_cloth_captured_customize_enable > 0.5) {
        const float3 customize_base = float3(1.0, 1.0, 1.0); // `Q313 _CustomizeBaseColor ?? [1,1,1]`
        const float3 customize_tint = float3(1.0, 1.0, 1.0); // `Q328 _CustomizeBaseTintColor ?? [1,1,1]`
        const float3 customize_add = float3(1.0, 1.0, 1.0);  // `Q155 _CustomizeAddTintColor ?? [1,1,1]`
        const float3 customize_mix = lerp(lerp(customize_add, customize_tint, base.g), customize_base, base.b);
        base = base.r * customize_mix; // readable:91 VERBATIM, including the `c.r *` factor
    }

    // ========================================================================================================
    // 3. NORMAL (readable:690-698, 712-722)
    // ========================================================================================================
    //
    // The captured viewer builds the frame from the VERTEX tangent (`vWorldTangent`, orthogonalised against the
    // geometric normal) and `B = cross(N,T) * tangent.w`; deren has no vertex tangent, so the derivative frame
    // is used with the CONTRACT's documented Unity/glTF Y bridge (`tdir = -tdir`), the same bridge every
    // other family port uses. The normal map itself is the game-export map whose stored blue is constant 0.
    float3 n_geo = normalize(geo_normal);              // `N` before any flip
    float3 n = n_geo;
    if (has_normal_map) {
        float3 sdir;
        float3 tdir;
        if (surface_tbn(world_pos, uv, sdir, tdir)) {
            tdir = -tdir; // Unity/glTF tangent-space Y bridge (CONTRACT "Available deren adapters")
            // `Q302` `_BumpScale` (readable:716) from profile lane 6.z, the packing's carrier for it. A zero word
            // is "not stated" (game_toon_profile.h), and the captured JS default is `?? 1` - which is also the row
            // every Laevatain cloth material states, so the fallback is the captured value.
            const float bump_scale_lane = game_toon_cloth_payload(push.material_index, 6u).z;
            const float bump_scale = bump_scale_lane > 0.0 ? bump_scale_lane : 1.0; // `_BumpScale ?? 1`
            const float4 bump = frame_texture_sample(mat.tex_indices.z, uv);
            n = normalize(mul(game_toon_cloth_unpack_normal(bump, bump_scale), float3x3(sdir, tdir, n_geo)));
        }
    }
    // readable:694-698 `R180 = gl_FrontFacing ? 1 : -1` (DOUBLE_SIDED), readable:722 `N *= R180`.
    // deren's equivalent statement is the glTF double-sided flag (record bit3).
    const float face_sign = (is_double_sided && !is_front) ? -1.0 : 1.0;
    n = normalize(n * face_sign);

    // ========================================================================================================
    // 4. LIGHT AND VIEW (readable:728-733; the captured light rig, whose adapters on deren)
    // ========================================================================================================
    //
    // THE LIGHT DIRECTION IS DEREN'S SUN, WHICH IS THE CONTRACT'S STATED FALLBACK. The captured viewer derives
    // `uMainLightDirection` from the CAMERA each frame (`viewer-lighting.js` `Bf`: `yaw = atan2(-n.x, n.z) +
    // yawBias(10 deg)`, `pitch = max(minPitch(32 deg), asin(-n.y))`, with `n = getWorldDirection`); deren's toon
    // stage has no camera-follow light rig, and CONTRACT says "if deren has no camera-relative rig, document the
    // adapter and keep the sun direction as the fallback". This port takes the fallback, so the captured still's
    // terminator/ramp-U will differ from the capture - the same recorded limitation as the skin/face and eye
    // ports. The captured adapter exists (`wf_camera_light(-camera_forward, 10.0, 32.0)`, `toon/game_toon_math.h`,
    // which takes the camera LOOK direction - deren's `camera_forward` is the opposite, toward-camera axis); it
    // is the one-line swap if the A/B wants the captured light. See notes §lighting.
    const float3 camera_pos = camera_at(heap_camera_slot).camera_pos.xyz;
    // deren's `camera_forward` is the camera's world +Z axis (the direction TOWARD the camera - the same vector
    // the skin/face note identifies as the captured `viewMatrix` third row, `R141`/`R314`).
    const float3 camera_forward = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz);
    const float3 view_dir = normalize(camera_pos - world_pos); // `V` (readable:729)
    const float4 view_features = game_toon_cloth_payload(push.material_index, 0u);
    // Captured viewer light: camera-relative (`viewer-lighting.js` Bf, yaw/minPitch from profile). The
    // profile-off path keeps deren's scene sun so the game_toon fallback is unchanged.
    const float3 camera_forward_axis = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz);
    const float3 light_dir = game_toon_profile_enabled(push.material_index)
        ? normalize(wf_camera_light(-camera_forward_axis, game_toon_yaw_bias(push.material_index), game_toon_min_pitch(push.material_index)))
        : normalize(light_at(heap_light_slot).light_dir.xyz);
    // `calc_shadow` IS DELIBERATELY NOT MULTIPLIED IN: the captured frame has `mainLightShadow: false`
    // (`laevat_volume`) and the viewer's own `selfShadow` default is OFF, so the captured diffuse weight is
    // `R144 = min(ramp.a, metallic_mask)` with no shadow-map term; whose `calc_shadow` is fit to the engine sun
    // and would add a term the capture does not have. If the Lead turns engine shadows on, this is the site.
    const float no_l = dot(n, light_dir);                        // `R128` (readable:731)
    const float no_v = clamp(dot(n, view_dir), 0.0, 1.0);        // `R46` (readable:732)
    const float3 half_dir = normalize(light_dir + view_dir);     // `R9` (readable:730)
    const float no_h = clamp(dot(n, half_dir), 0.0, 1.0);        // `R196` (readable:733)

    // ========================================================================================================
    // 5. THE METALLIC/GLOSS MAP (`uMetallicGlossMap`, readable:773-782; `_UseMetallicGlossMap = 1`)
    // ========================================================================================================
    //
    // The captured lane dictionary (readable:913-935, 1158-1171): `.b` = the shadow/lit mask, `.a` = smoothness
    // (the spec exponent), `.r` = the metallic term of the specular tint. `.g` is read only by the inactive Q299
    // path. deren's `toon_lanes2_at(...).y` is the lane main.cpp fills from the sidecar's `_MetallicGlossMap` row
    // and uploads as `rgba8_unorm` - data, so no colour decode happens on either side.
    float4 metallic_gloss = float4(0.0, 0.0, 1.0, 0.0);
    if (has_metallic_gloss) {
        metallic_gloss = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + metallic_gloss_lane), heap_sampler_texture, uv);
    }
    // NO SECOND SOURCE: unlike the original game program, the captured viewer's `_MetallicGlossMap` is a texture
    // of its own and the material JSON declares no glTF metallicRoughness texture, so `mat.tex_indices.y` would
    // be the white fallback and is deliberately not used. With no lane the captured viewer would have emitted
    // no Q84/Q13 at all: `mask = 1` and the cloth spec block is skipped, which is what this port does.

    // ========================================================================================================
    // 6. THE SHADOW-SIDE COLOUR: `_ShadowLutTex`, or the luminance fallback (readable:904-920)
    // ========================================================================================================
    //
    // `R152 = sampleShadowLutLinear(base.rgb)` when the LUT is present, else
    // `R113(base.rgb, _ShadowColorSaturation) * _ShadowColorBrightness`. The LUT is per-material DATA; dessen
    // `shadow_lut` lane is its staging (CONTRACT "LUT/ramp are DATA").
    float3 shadow_colour;
    if (has_shadow_lut) {
        shadow_colour = game_toon_cloth_shadow_lut(base, s.toon_indices.y);
    } else {
        // `R152 = R113(base.rgb, Q348) * Q347` (readable:908, `R113` readable:58-61). `Q347` `_ShadowColorBrightness`
        // and `Q348` `_ShadowColorSaturation` are PER MATERIAL and ride profile lane 5`.x` / 5`.y` - the FINAL
        // packing's row 5 (`uniform-packing.json`: `Q347 _ShadowColorBrightness ?? .5`,
        // `Q348 _ShadowColorSaturation ?? 1`). A zero word is "not stated"; the captured JS defaults, which are
        // also the two Laevatain cloth rows (0.5 / 1.0), are the fallback.
        const float shadow_brightness = game_toon_cloth_payload(push.material_index, 5u).x > 0.0
                                            ? game_toon_cloth_payload(push.material_index, 5u).x
                                            : game_toon_cloth_captured_shadow_brightness;
        const float shadow_saturation = game_toon_cloth_payload(push.material_index, 5u).y > 0.0
                                            ? game_toon_cloth_payload(push.material_index, 5u).y
                                            : game_toon_cloth_captured_shadow_saturation;
        shadow_colour = game_toon_cloth_saturation_to_luma(base, shadow_saturation) * shadow_brightness;
    }

    // ---- SILK STOCKINGS (`Q118`, readable:911-917; the values' lanes are readable:454-459) ----
    // CAPTURED INACTIVE: `_SilkStockings = 0.0` on all five Laevatain cloth materials, so `material-feature-defines`
    // never emits Q118 and the capture never evaluates this; the gate is therefore the captured enable constant
    // above (no lane in the final packing carries `_SilkStockings`). The FORMULA is readable:911-917 VERBATIM:
    //   R327 = 2*clamp(base.a + 1 - Q349.a, 0, 1);
    //   R263 = mix(Q238.x, Q238.y, min(pow(1.05 - R46, R327), 1));
    //   base.rgb = mix(base.rgb * Q190, Q349.rgb, R263);
    //   R152     = mix(R152     * Q190, Q349.rgb, R263);
    // with the values on the FINAL packing's rows: Q238.x/`.y` = lane 12`.z`/12`.w` (min/max affect, JS default
    // 0 / .9; the Laevatain row is 0.05 / 0.9), Q349 = lane 13 (colour + its raw alpha, JS default [0,0,0] a=1),
    // Q190 = lane 14`.rgb` (dry colour, JS default [1,1,1]).
    if (game_toon_cloth_captured_silk_stockings > 0.5) {
        const float4 silk_lane = game_toon_cloth_payload(push.material_index, 13u);          // `Q349`
        const float3 silk_colour = silk_lane.rgb;
        const float silk_alpha = silk_lane.a > 0.0 ? silk_lane.a : 1.0;                     // `_SilkStockingsColor[3] ?? 1`
        const float3 silk_dry = game_toon_cloth_payload(push.material_index, 14u).rgb;        // `Q190 _SilkStockingsDryColor`
        const float2 silk_affect = game_toon_cloth_payload(push.material_index, 12u).zw;      // `Q238.x/.y` ?? 0 / .9
        const float wet_power = 2.0 * clamp(base_alpha + 1.0 - silk_alpha, 0.0, 1.0);        // `R327`, readable:912
        const float wet = lerp(silk_affect.x, silk_affect.y,
                               min(pow(1.05 - no_v, wet_power), 1.0));                       // `R263`, readable:913
        base = lerp(base * silk_dry, silk_colour, wet);                                      // readable:914
        shadow_colour = lerp(shadow_colour * silk_dry, silk_colour, wet);                    // readable:915
    }

    // ========================================================================================================
    // 7. THE DIFFUSE RESPONSE (readable:892-1004) — the `USE_DIFFUSE_RAMP` arm, which is the captured one
    // ========================================================================================================
    //
    // R78 = the ramp coordinate on the light axis; `uRampRangeBias` is the laevat volume's `rangeBias` (0.0 -
    // MISSING PROFILE LANE for the volume value; captured constant used).
    const float ramp_u = clamp(no_l + game_toon_range_bias(push.material_index), -1.0, 1.0) * 0.5 + 0.5; // readable:892
    // `R152 *= 0.96 * uEnvironmentMultiplier` (readable:920): THE ENVIRONMENT MULTIPLIER'S ONLY ROLE IN THE
    // CLOTH BRANCH. The captured viewer has NO environment/specular-IBL read here (the Q299 path that carries
    // `CharMaxCubemap`-style reflection is inactive); this port therefore does not add deren's
    // `environment_irradiance`/`environment_specular_sample` - that would be a term the capture does not have.
    const float environment_mul = view_features.z > 0.0 ? view_features.z : game_toon_cloth_captured_env_mul;
    shadow_colour *= 0.96 * environment_mul; // readable:920

    float3 colour;
    float lit_weight = 0.0; // `R144`: the lit/shadow mix weight the light formula and the spec mask consume
    if (has_diffuse_ramp) {
        // `R32 = texture2D(uDiffuseRampMap, vec2(clamp(R78,0.002,0.998), 0.5))` - v = 0.5 (readable:936)
        const float4 diffuse_ramp = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.x),
                                                heap_sampler_texture,
                                                float2(clamp(ramp_u, 0.002, 0.998), 0.5));
        const float metallic_mask = has_metallic_gloss ? metallic_gloss.b : 1.0; // readable:930-935
        // `R215`: the SAME ramp read at the view-axis coordinate, alpha only (readable:938). The viewer's
        // `R141` is the view matrix's third row = deren's `camera_forward` (toward-camera axis), so the
        // coordinate is `dot(N, camera_forward)*0.5+0.5` - no sign flip.
        const float view_axis_u = clamp(dot(n, camera_forward) * 0.5 + 0.5, 0.002, 0.998);
        const float view_alpha = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.x),
                                            heap_sampler_texture,
                                            float2(view_axis_u, 0.5))
                                     .a;
        lit_weight = min(diffuse_ramp.a, metallic_mask);   // `R144`, readable:945 (no Q132 shadow term)
        const float view_masked = view_alpha * metallic_mask; // `R318`, readable:946
        const float3 shadow_65 = 0.65 * shadow_colour;    // `R281`/`R22` inputs, readable:959
        const float shadow_luma = game_toon_cloth_luma(shadow_65);
        // `R130 = mix(R22 + 1.2*(0.65*R281 - R22), R281, clamp(R32.a + R318, 0, 1))` (readable:960)
        float3 dark_mix = float3(shadow_luma, shadow_luma, shadow_luma) + 1.2 * (shadow_65 - float3(shadow_luma, shadow_luma, shadow_luma));
        dark_mix = lerp(dark_mix, shadow_colour, clamp(diffuse_ramp.a + view_masked, 0.0, 1.0));
        const float3 base_dark = base * 0.96;             // `R334`, readable:950
        dark_mix = lerp(dark_mix, base_dark, lit_weight); // readable:961
        // `R150` chroma of the ramp, then the ramp's colour tint and the brightness guard (readable:962-964).
        // THE CAPTURE USES ONE VALUE FOR BOTH: `R150 = max(R32.rgb) - min(R32.rgb)` and
        // `color = R130 * mix(1, R32.rgb, R150)` are the SAME raw texel (readable:962-963). deren stages this
        // lane `rgba8_srgb`, so the sampler returns the DECODED value; `game_toon_encoding_linear_to_srgb`
        // recovers the stored display value ONCE (the bridge `encoding.h` documents), and the chroma must be
        // computed on that recovered value too - computing it on the decoded value is the one place the two
        // differ (a strongly bent channel).
        const float3 ramp_display_rgb = game_toon_encoding_linear_to_srgb(diffuse_ramp.rgb);
        const float ramp_chroma = max(max(ramp_display_rgb.r, ramp_display_rgb.g), ramp_display_rgb.b) -
                                  min(min(ramp_display_rgb.r, ramp_display_rgb.g), ramp_display_rgb.b);
        colour = dark_mix * lerp(float3(1.0, 1.0, 1.0), ramp_display_rgb, ramp_chroma);
        colour *= clamp(game_toon_cloth_luma(dark_mix) / max(game_toon_cloth_luma(colour), 0.001), 0.0, 1.5);
    } else {
        // The captured non-ramp variant (readable:995-1003): a hard two-tone mix at R78 = 0.48..0.52.
        // Its `R144` is a local variable in the captured branch, but the later light formula reads the same
        // function-scope name; keep it in the shared `lit_weight`.
        lit_weight = smoothstep(0.48, 0.52, ramp_u); // readable:996
        colour = lerp(shadow_colour, base, lit_weight); // readable:1002
    }

    // ========================================================================================================
    // 8. THE LIGHT FORMULA (readable:1005-1007) — the captured viewer's, driven by the profile lanes
    // ========================================================================================================
    //
    // `light = mix(uShadowTint, 1, R144) * R169 * VIEWER_LIGHT_SCALE * uMainLightColourScaled
    //        + mix(vec3(luma(R266)), R266, R144)` with `R169 = clamp(N.y+0.15,0,1)*1.5+0.5` and
    // `R266 = uMainLightColourScaled * uMainLightIntensity`.
    const float hemi = clamp(n.y + 0.15, 0.0, 1.0) * 1.5 + 0.5; // `R169`, readable:921
    // PROFILE LANES: lane 2 rgb = uMainLightColourScaled (the viewer's `Q10` = light colour * volume multiplier;
    // captured (1.05,1.05,1.05)), lane 2 w = uMainLightIntensity (captured 2.0); lane 3 rgb = uShadowTint
    // (captured 0.8783/0.9302/1.1217). A zero lane is "not stated" (game_toon_profile.h), so the captured
    // constants are the documented fallback:
    // // MISSING PROFILE LANE: lanes 0.y (volume multiplier), 2 and 3 when the profile is not filled.
    const float4 main_light_lane = game_toon_main_light(push.material_index);
    const float4 shadow_tint_lane = game_toon_profile_lane(push.material_index, 3u);
    float3 main_light_colour_scaled;
    if (dot(main_light_lane.rgb, float3(1.0, 1.0, 1.0)) > 0.0) {
        main_light_colour_scaled = main_light_lane.rgb;
    } else if (view_features.y > 0.0) {
        // lane 0.y is the volume multiplier; when the coloured lane is absent it is the captured `Q10` value.
        main_light_colour_scaled = float3(view_features.y, view_features.y, view_features.y);
    } else {
        main_light_colour_scaled = float3(game_toon_cloth_captured_volume_mult,
                                          game_toon_cloth_captured_volume_mult,
                                          game_toon_cloth_captured_volume_mult);
    }
    const float main_light_intensity = game_toon_main_light_intensity(push.material_index) > 0.0 ? game_toon_main_light_intensity(push.material_index) : game_toon_cloth_captured_main_light_intensity;
    const float3 shadow_tint = dot(shadow_tint_lane.rgb, float3(1.0, 1.0, 1.0)) > 0.0
                                   ? shadow_tint_lane.rgb
                                   : game_toon_cloth_captured_shadow_tint;
    const float3 main_radiance = main_light_colour_scaled * main_light_intensity; // `R266`, readable:927
    const float main_radiance_luma = game_toon_cloth_luma(main_radiance);
    const float3 light = lerp(shadow_tint, float3(1.0, 1.0, 1.0), lit_weight) * hemi * game_toon_cloth_viewer_light_scale * main_light_colour_scaled +
                         lerp(float3(main_radiance_luma, main_radiance_luma, main_radiance_luma), main_radiance, lit_weight); // readable:1006
    colour *= light; // readable:1007

    // ========================================================================================================
    // 9. SATURATION LIFT (readable:1140-1145; Q178, emitted because the viewer's `saturationLift` defaults on)
    // ========================================================================================================
    if (has_diffuse_ramp) {
        const float luma = game_toon_cloth_luma(colour);
        const float lift = clamp(luma - 0.5, 0.0, 0.5);
        colour = luma + (colour - luma) * (lift * lift + 1.0);
    }

    // ========================================================================================================
    // 10. EMISSION (readable:1146-1150, `FAMILY_CLOTH && !Q299`)
    // ========================================================================================================
    //
    // Captured, verbatim (readable:1148):
    //   color += texture2D(uEmissionMap, vUv).rgb * Q15 * mix(1.0, base.a, Q20);
    // `Q15 = _EmissionColor.rgb * _EmissionBrightness ?? 1` (material-uniform-bindings `Q3`: the viewer builds
    // `Le(...,_EmissionColor) * (_EmissionBrightness ?? 1)`), and `Q20 = _AlphaPremultiply ?? 0`
    // (`uniform-packing.json` row 14`.w`). deren's `s.emissive` IS `Q15 x _EmissionMap`: the gathered emissive
    // factor carries `_EmissionColor` AND the brightness (the packing's `existing_lanes.Q15`), and it carries
    // the map through the one texture the capture also declares sRGB - so the emission term is
    // `s.emissive * mix(1, base.a, Q20)` and no brightness is applied here a second time.
    if (has_emission) {
        const float Q20 = game_toon_cloth_payload(push.material_index, 14u).w; // `_AlphaPremultiply ?? 0`
        colour += s.emissive * lerp(1.0, base_alpha, Q20);                     // readable:1148
    }

    // ========================================================================================================
    // 11. THE CLOTH SPECULAR (`Q84`, readable:1158-1171) — the captured cloth branch's ONLY specular lobe
    // ========================================================================================================
    //
    // Q141 / Q299 are not emitted for these materials, so this block is the whole specular response; the
    // generic `R41` lobe and the Q299 environment lobe never run in the capture.
    //
    //   R82 = R299.a (smoothness); R181 = pow(NoH, exp2(mix(3,9,R82))) * R82;
    //   R181 = smoothstep(0.35,0.6,R181) * step(0,NoL);
    //   tint = SpecRamp(NoH, 1-R82) [Q38], tint = mix(tint, tint*base*2, R299.r);
    //   colour += R181 * tint * uSpecularStrength * Q22 * uMainLightColourScaled * uMainLightIntensity.
    if (has_metallic_gloss) {
        const float smoothness = metallic_gloss.a;                                     // `R82`, readable:1159
        // `uSpecularStrength` = `_Specular` (material JSON, 1.0), carried by deren's colour lane 4 `.x`
        // (`toon_colour_lane::specular_strength`; `.x < 0` = the asset states no row).
        const float lane_specular = toon_colour_at(heap_slots_toon_colours, colour_base + 4u).x;
        const float specular_strength = lane_specular >= 0.0 ? lane_specular : 1.0;
        float spec_shape = pow(no_h, exp2(lerp(3.0, 9.0, smoothness))) * smoothness;    // readable:1160
        spec_shape = smoothstep(0.35, 0.6, spec_shape) * step(0.0, no_l);               // readable:1161
        float3 tint = float3(1.0, 1.0, 1.0);
        if (has_spec_ramp) {
            // `tint = texture2D(uSpecularRampMap, vec2(R196, 1.0-R82)).rgb` (readable:1165). NO clamp: the
            // captured sampler is clamp-to-edge. deren's `specular_ramp` lane is the staged `_SpecRampMap`; the
            // captured read is raw (material JSON `srgb` lists only `_EmissionMap`), see notes §encoding.
            tint = game_toon_encoding_linear_to_srgb(heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.z),
                              heap_sampler_texture,
                              float2(no_h, 1.0 - smoothness))
                       .rgb);
        }
        tint = lerp(tint, tint * base * 2.0, metallic_gloss.r); // readable:1168
        colour += spec_shape * tint * specular_strength * game_toon_cloth_viewer_spec_scale * main_radiance; // readable:1169
    }

    // WHAT REMAINS OUTSIDE THE CAPTURE'S REACH IN THIS BRANCH (each recorded rather than silently dropped):
    //  * `R114` (readable:188-327, called at readable:1156): the capture's character-only punctual rig. It IS
    //    ported now - `game_toon_lights.h`'s `game_toon_add_lights` drives deren's ordinary `LightUBO.punctual_lights`
    //    and the app injects the captured Laevatain rig into that UBO - with two documented approximations: the
    //    helper's `diffuse` argument is `s.albedo`, where the capture passes `R283 = R252` (the pre-light colour)
    //    for the wrap mode and `R40 = R317 = base.rgb*0.96` for the rim mode's luma; and the capture's mode-4 fog
    //    arm is left to the scene fog.
    //  * `R75 = R200(...)` cutoff/dither (readable:340-348, readable:1177): `_UseCutOff = 0` -> `Q185 = 0` -> zero.
    //  * Q299 / Q141 environment + generic specular: not emitted for these materials (see the header).
    //  * Q118 silk / Q100 customize: transcribed verbatim above, but the capture states both enables as 0.0
    //    (`_SilkStockings`, `_AvatarCustomizeEnable`) and the final packing carries no lane for either.
    //  * Q247 extra-alpha: needs a second UV set deren does not carry, and `_UseUV2Color = 0` on all five.
    // No tone mapping and no output sRGB encode: this is LINEAR HDR; post pass mode 2 owns the transform.
    return game_toon_add_lights(colour, normalize(s.normal), view_dir, world_pos, s.albedo, float3(0.04), s.roughness);
}

#endif // DEREN_GAME_TOON_GAME_TOON_CLOTH_SLANG
