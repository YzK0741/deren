#include "toon/game_toon/face_diffuse.h"
#include "toon/game_toon/face_frame.h"
#include "toon/game_toon/face_rim.h"
// ============================ THE GAME-SHADER TOON MODEL: FACE ============================
//
// THE SOURCE IS THE ORIGINAL GAME PROGRAM, not the author chain. `docs/references/toon/
// face_forward_spirv_decompiled.hlsl` is the decompile of the game's HGRP/CharacterNPR_Skin FACE fragment
// shader (`npr_skin_blocks/code/shader_3e3d05cf72d25122_lod600_Vulkan_0_block.bin`, SPIR-V record offset
// 1,757,992, SMOL-V module at 1,758,400, decompiled with spirv-cross; keyword set
// `_DIFF_RAMP_ON _EMOTION_MAP _HIGHLIGHT_MAP _NORMALMAP _SDFLIGHTMAP _SHADOW_LUT_TEX`; TWO render targets:
// SV_Target0 = colour, SV_Target1 = `_1427`).
//
// LINE NUMBERS. The decompiled file's own banner cites "KEY SITES" in BODY-relative lines (444 normal
// reconstruction, 712 P/F0, 1255 entry). The checked-in file carries a 17-line provenance banner, so its
// line numbers are body + 18. EVERY line number below is the CHECKED-IN FILE's, and the header's numbers are
// +18 (so `:730` here is the header's "712", the P/F0 block).
//
// WHAT IS PORTED (the surface response, formula-for-formula where deren has the input):
//  * the packed-normal decode, the tangent frame and the front-face flip (:455-472);
//  * the base/emotion albedo (`_BaseMap` * `_BaseColor`, then the `_EmotionMap` 2x2 atlas cell, :438-442);
//  * the `_ShadowLutTex` 32^3-in-a-strip lookup (:443-452), through deren's `toon_shadow_lut`;
//  * the FACE SDF lightmap terminator: mirrored lookup, the light-angle window and the signed light term
//    (:1504-1589, esp. 1529/1547/1549/1555/1589) - deren's `head_frame_at` supplies the head axes;
//  * the `_SDFMask` channels as the game reads them (:508-512, 643-645, 766-771, 817): `.x` the SSS strength,
//    `.y` the SDF-vs-NoL blend (and the SDF normal blend), `.z` the shadow-ring weight, `.w` the rim region;
//  * SSS: the albedo tint toward the material's SSS colour (:643-645);
//  * the P/F0 block (`:730-734`): `0.96 - 0.96*M`, `F0 = lerp(0.04*_Specular, albedo, M)`, the shadow
//    albedo's metallic discount and `max(roughness^2, 0.0078125)`;
//  * the ramp coordinate and the `_DiffRampMap` read (:771) and the dark-layer composition (:776-791);
//  * the direct specular's D*V fit (:793-810), the exact same constants the cloth/skin arms ported;
//  * deren's lighting only: `light_at` for the sun, `calc_shadow` for the cast shadow, `environment_*` for
//    the environment. NO Unity matrices, NO `_MainLight*` globals, NO `toon_diffuse` call.
//
// WHAT IS NOT PORTED, with the reason (all are `// MISSING:` in the body too):
//  * NAME ALIASES FIRST, because the task and the author chain use different names for the same game inputs
//    (`deren-ab/face_shader_compare.md` §1 maps them): `_SDFLightmap` == the author's `_SdfTex`, `_SDFMask`
//    == the author's `_SdfRefineTex` (inferred), `_ShadowLutTex` == `_LutColorTex`, `_DiffRampMap` ==
//    `_RampTex`, `_EmotionMap` == `_TrickTex` + `_TrickType`, and `_FaceCenter` / `_FaceRight` / `_FaceUp` /
//    `_FaceForward` == deren's `head_frame_at(...).center` / `.right` / `.up` / `.front` (so those four DO
//    have an equivalent and are not MISSING). What is missing is the 3D volume's own parameterisation
//    (`_FaceCenter` offsets/scales `_19_m105.._19_m107`), see the point below.
//  * IRRADIANCE CLIPMAPS, NOT A HEAD DISTANCE FIELD. The six Texture3Ds at t30..t35
//    (:511-601) carry RGB directional irradiance coefficients at three spatial scales.
//    The following block (:613-636) adds default SH coefficients, evaluates three
//    float4 coefficient vectors against float4(normal, 1), and derives colour/strength
//    (_1135/_1136). _19_m105..107 parameterise the world-space clipmaps;
//    _19_m108..110 are default RGB SH coefficients, not a per-object colour basis.
//    These missing lighting inputs are bridged by deren lighting, not by a baked
//    head proximity field. The 2D _SDFLightmap is the face terminator input sampled
//    at mirrored UV (:1508-1555); it remains a separate material mechanism.
//  * THE FACE HIGHLIGHT/DECAL BLOCK (:655-720 + :689-729): it needs `_HighlightMap`/`_FaceDecalMap`/`_TrickTex`
//    rows and the packed per-material bytes (`_601`); deren carries no lane for any of them. Its outputs are
//    held at the block's OWN "skipped" values (`_629 = 0`, `_1268 = 0`, `_1335 = 0`), i.e. exactly the
//    else-branch the decompiled program takes when the gates are closed - not invented numbers.
//  * THE FACE DECAL and `_HighlightMapVector` (:655-720) and the second render target `_1427` (the game writes
//    a screen-space SDF direction; deren's stage has one target).
//  * THE SCREEN-SPACE READS: `_39` (t22, the shadow mask at `:746-748`) and `_50` (t4, SSR at `:800`) are
//    screen textures; calc_shadow stands in for the first, the second is 0 (MISSING).
//  * THE ENVIRONMENT/3D BLOCK (:1203-1268, `t36`, `_19_m4*`/`_19_m5*`): the volumetric fog/IBL chain is not
//    a face mechanism and its inputs are absent; deren's env helpers stand in on the approved boundary.
//  * THE ENGINE'S ART-DIRECTION GLOBALS `_19_m79.._19_m110`, `_48_m2x.._48_m34` (light overrides, tone,
//    material SSS scalars): neutral identities 1.0/0.0 are written at each use and marked MISSING. They are
//    NOT game values recovered from somewhere else.

/// @brief the game's packed-normal decode for the FACE (`face_forward_spirv_decompiled.hlsl:457-463`)
///
/// `:458` `_516.w = _516.w * _516.x` - the normal map's X is R*A - `:459` `(wy * 2) - 1`, `:462` reconstruct
/// +Z from the clamped dot, `:463` `_535 = _526.xy * _48_m3` with `_48_m3` = `_BumpScale`. deren's normal
/// decode (`surface.slang`) reads `rgb * 2 - 1` and TRUSTS blue, which these maps do not store, so the face
/// arm decodes the game's own way, exactly as the cloth/skin arms do (`game_unpack_normal` in
/// `game_toon.slang`). Kept local to this file so the family does not depend on that file's helper order.
static float3 game_face_unpack_normal(const float2 rg, const float scale) {
    float3 normal;
    normal.xy = rg * 2.0 - 1.0;
    normal.z = max(1.0000000168623835e-16, sqrt(1.0 - saturate(dot(normal.xy, normal.xy))));
    normal.xy *= scale;
    return normal;
}

/// @brief the game's FACE shading, `face_forward_spirv_decompiled.hlsl:412-1271`, with deren's inputs
///
/// THE ONE SENTENCE A READER SHOULD LEAVE WITH: the shape of the terminator is the game's SDF
/// (`_SDFLightmap` R/G through a light-angle window, then mirror-signed), the colour of the two sides is the
/// game's `_ShadowLutTex` / `_DiffRampMap` pair, the P/F0 block is the game's, and the LIGHT is deren's.
///
/// @param s the gathered surface (albedo already includes `_BaseColor`; see `gather_surface`)
/// @param world_pos interpolated world position
/// @param geo_normal interpolated geometric normal
/// @param uv the material's uv
/// @param is_front front-face flag (the game flips the shading normal on a back face, :470-472)
/// Face family states `ambient_strength = 0.0`, so the port keeps the author's "no ambient" arrangement. The
/// game face program DOES read the character lightfield/volume; deren has no 3D lane, and the reference's face is
/// visibly lifted by that missing term. This is a CALIBRATION bridge for the A/B, not the game value: a minimum
/// share of the sky term keeps the face from reading darker than both references while the volume lane is absent.
static const float game_face_min_ambient = 0.5;

// Original _483/_488/_489/_504 uses increasing green in game texture storage.
// Archive PNGs were exported with ConvertToImage(flip: true); bridge that storage
// flip without changing the original analytical coordinate in face_frame.h.
float3 game_face_shadow_lut(float3 albedo, uint lane) {
    const float3 cube = saturate(toon_linear_to_srgb(albedo));
    const float tile_f = cube.z * 31.0;
    const float u = floor(tile_f) / 32.0 + (cube.x * 31.0 + 0.5) / 1024.0;
    const float v = game_face_exported_lut_v(cube.y);
    const float3 low = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + lane), heap_sampler_texture, float2(u,v)).rgb;
    const float3 high = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + lane), heap_sampler_texture, float2(u + 1.0/32.0,v)).rgb;
    return lerp(low,high,frac(tile_f));
}

float3 game_toon_face_shade(surface_sample s, float3 world_pos, float3 geo_normal, float2 uv, bool is_front) {
    const ToonRigUBO rig = toon_rig_value();
    const Material mat = material_at(heap_slots_materials, push.material_index);
    // BESIDE-THE-RECORD LANES (`deren::engine::toon_slot` 4..7): x = `_SDFLightmap`, y = `_MetallicGlossMap`,
    // z = `_SDFMask`, w = `_EmotionMap`. 0 is "do not read" on every one of them, so the presence tests below
    // are the enable words and no `_Use*` flag needs a lane.
    const uint4 toon_lanes = toon_lanes_at(heap_slots_toon_lanes, push.material_index);
    const toon_params params = toon_params_for(s.toon_family);
    const uint colour_base = push.material_index * character_toon_colour_lanes;

    // `_SDFRimColor` (colour lane 0, the game's own face-rim tint) and `_Specular` (colour lane 4, `.x`; a
    // negative `.x` means "the asset states no row" - the contract `deren::vulkan::toon_colour_lane` defines).
    const float3 sdf_rim_colour = toon_colour_at(heap_slots_toon_colours, colour_base + 0u).rgb;
    const float lane_specular = toon_colour_at(heap_slots_toon_colours, colour_base + 4u).x;

    // ---- 1. THE HEAD FRAME: the game derives every face angle in the head's own frame (`_580`, `_1501`,
    //         `_592` are all object/head-space transforms in the decompile). deren publishes that frame
    //         per frame for the head bone (`head_frame_at`), which is the same semantic input. The block's
    //         `center` is a head position, while `_19_m105` belongs to world-space irradiance clipmaps.
    //         No ported term here reads the head center, so it is not fetched.
    const float3 head_front = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).front.xyz);
    const float3 head_right = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).right.xyz);
    const float3 head_up = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).up.xyz);

    // ---- 2. LIGHT, CAMERA, VIEW (dere's lighting: the game's `_1440`/`_1454`/`_1458` are Unity globals
    //         with per-character overrides this renderer does not have; `light_at` IS the substitute). ----
    const float3 light_dir = normalize(light_at(heap_light_slot).light_dir.xyz);
    const float3 camera_pos = camera_at(heap_camera_slot).camera_pos.xyz;
    const float3 camera_forward = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz);
    const float3 view_dir = normalize(camera_pos - world_pos);
    const float3 main_light_colour = float3(1.0, 1.0, 1.0); // deren's sun is white
    const float main_light_intensity = max(0.001, light_at(heap_light_slot).sun_intensity) * params.light_intensity_scale;
    // `_592` = the camera's forward axis measured along the head's own forward axis. The decompile builds it
    // as `normalize(mul(viewForward, objectMatrix)).xz.y`; deren's head axes ARE the world directions of that
    // matrix's rows, so the dot is the same scalar without the matrix.
    const float head_face_dir_z = dot(camera_forward, head_front);

    // ---- 3. NORMAL: the game's packed decode, deren's derivative TBN, the documented Y bridge, the
    //         front-face flip (the same convention the cloth/skin arms ported). ----
    float3 normal = normalize(geo_normal);
    if ((mat.flags & 1u) != 0u) {
        float3 sdir;
        float3 tdir;
        if (surface_tbn(world_pos, uv, sdir, tdir)) {
            // UNITY/glTF TANGENT-SPACE Y BRIDGE, the same measured one the cloth arm records: deren has no
            // vertex tangent, the game stores its Y in Unity's handedness, and the derivative frame's Y is
            // the opposite direction. This is a convention bridge, not a tuned sign.
            tdir = -tdir;
            const float4 n_texel = frame_texture_sample(mat.tex_indices.z, uv);
            const float3 n_ts = game_face_unpack_normal(float2(n_texel.r * n_texel.a, n_texel.g), mat.normal_scale);
            normal = normalize(mul(n_ts, float3x3(sdir, tdir, normal)));
        }
    }
    // :470-472 `_566 = gl_FrontFacing ? 1 : (-1 + 2*_BackFaceNormalFlip)`; deren carries the game's
    // `_BackFaceNormalFlip = 0` as "double-sided material" (flags bit3) because the flag itself has no lane.
    if ((mat.flags & 8u) != 0u && !is_front) {
        normal = -normal;
    }

    // ---- 4. ALBEDO: `_BaseMap` * `_BaseColor` is `s.albedo` (`gather_surface` multiplies the factor), then
    //         the `_EmotionMap` 2x2 expression atlas (:438-442). ----
    float3 albedo = s.albedo;
    if (toon_lanes.w != 0u && rig.other_colour.w > 0.0) {
        // :441 the atlas coordinate is `cell(index) + 0.5 * uv`; the cell is
        // `(fmod(_EmotionIndex,2)*0.5, floor(_EmotionIndex*0.5)*0.5)`. `_EmotionIndex` is a per-material
        // scalar with no deren lane; the host's equivalent is `[toon] emotion_type` -> `rig.other_colour.w`,
        // which is the same expression's number one step earlier (the article chain reads it the same way).
        // MISSING: the game's `_EmotionBlend` (:442 `_464.w * _48_m33`) has no deren lane either; 1.0 is the
        // shader property's own default (`skin-variant-map.props.tsv` `_EmotionBlend Range 1,0,1`), i.e. the
        // full overlay, and it is what the article chain applies as well.
        const float emoji_type = rig.other_colour.w * 0.5;
        const float2 emoji_uv = float2(frac(abs(emoji_type)), floor(emoji_type) * 0.5);
        const float4 emotion = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + toon_lanes.w),
                                          heap_sampler_texture, uv * 0.5 + emoji_uv);
        albedo = lerp(albedo, emotion.rgb, emotion.a * 1.0);
    }

    // ---- 5. SHADOW LUT (`_ShadowLutTex`, :443-452): a 32^3 colour cube flattened into a 1024x32 strip of
    //         32x32 tiles, indexed by the DISPLAY-SPACE albedo; tile from Z, in-tile u from X, v from Y, with
    //         the manual bilinear hop between the tile and its right neighbour. ----
    //
    // Original cube lookup, with the exported PNG storage bridge above.
    float3 lut_colour = albedo;
    if (s.toon_indices.y != 0u) {
        lut_colour = game_face_shadow_lut(albedo, s.toon_indices.y);
    }

    // ---- 6. THE FACE SDF TERMINATOR (`_SDFLightmap`, :1504-1589) ----
    //
    // The game mirrors UV from object-space light X (:1504/1508), not anatomical right.
    // deren head_right is -X for a +Z-facing head; the game object frame uses +X.
    // Convert this component before the original strict >0 mirror test.
    const uint sdf_lane = toon_lanes.x;
    const float2 light_dir_xz = normalize(light_dir.xz + float2(1.0e-6, 0.0));
    const float2 camera_forward_xz = normalize(camera_forward.xz + float2(1.0e-6, 0.0));
    // `_19_m91.x` (the engine's "disable backlight compensation" lane) is MISSING: 0.0 keeps the term on,
    // which is the decompile's unswitched default. `rig.misc.x` is deren's own weight for the same idea.
    const float back_light = saturate(-dot(camera_forward_xz, light_dir_xz)) *
                             smoothstep(0.0, 1.0, saturate(-abs(camera_forward.y) + 0.75)) * rig.misc.x;
    // `_1531`/`_1532`: the light's forward component in the head frame with its up component removed
    // (:1498-1501 `mul(lightDir, objectMatrix)` then `normalize(float3(x, eps, z))`).
    const float light_head_x = game_face_object_x(dot(light_dir, head_right));
    const float light_head_z = dot(light_dir, head_front);
    const float light_head_flat_z = normalize(float3(light_head_x, 6.10351562e-05, light_head_z)).z;
    // :766 `_1547`: the HALF light term the SDF window is opened by, backlight-compensated. The compensation is
    // `clamp(-dot(light_xz, camera_forward_xz)) * clamp(-light_head_flat_z)`; the `(1 - _19_m91.x)` factor is 1
    // (MISSING, see above). The two lerp arms are `_1531` and `_1532*((_1531*0.5)-1)+0.5`, written out.
    const float back_comp = saturate(-dot(light_dir_xz, camera_forward_xz)) * saturate(-light_head_flat_z);
    const float half_face_no_l = lerp(light_head_flat_z,
                                      (-0.5 * light_head_flat_z * light_head_flat_z) + light_head_flat_z + 0.5,
                                      back_comp) * 0.5;
    // :1549 `_1549 = clamp(0.5 - _1547, 0.001, 0.999)`; :771's smoothstep window `[max(2*_1549-1,0),
    // min(2*_1549,1)]` + `t*t*(3-2t)`. THE WINDOW CAN DEGENERATE (`_1547` at/above 0.5 collapses or inverts
    // `sdf_lo`/`sdf_hi`), and the decompile leaves `smoothstep` unguarded there, which on this platform would
    // divide by zero. The epsilon below is a NUMERICAL guard, not a formula change - it is far below the
    // 8-bit SDF's own step - and everything else is the decompile's two edges and Hermite.
    const float sdf_half = clamp(0.5 - half_face_no_l, 0.001, 0.999);
    const float sdf_lo = max(sdf_half - (1.0 - sdf_half), 0.0);
    const float sdf_hi = min(sdf_half + sdf_half, 1.0);
    // :1504/1508/1512: the mirrored lookup is taken ONCE and its two channels are used by two different
    // statements below - R/G averaged for the terminator (`:1555`) and B for the normal term (`:1514`/`:1517`)
    // - exactly as the decompile's single `_1512` sample is.
    const float sdf_mirror = game_face_sdf_mirror(light_head_x);
    const float2 sdf_uv = float2(lerp(1.0 - uv.x, uv.x, sdf_mirror), uv.y);
    float4 sdf_texel = float4(1.0, 1.0, 0.0, 1.0); // no lane: "fully lit", B = 0 -> a flat normal term
    if (sdf_lane != 0u) {
        sdf_texel = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + sdf_lane),
                               heap_sampler_texture, sdf_uv);
    }
    const float sdf_average = saturate(0.5 * (sdf_texel.r + sdf_texel.g));
    const float sdf_span = max(sdf_hi - sdf_lo, 1.0e-6);
    const float sdf_t = saturate((sdf_average - sdf_lo) / sdf_span);
    const float sdf_smooth = sdf_t * sdf_t * (3.0 - 2.0 * sdf_t);
    // :771's signed tail: `abs(-smoothstep(...) - (_1547 * ceil(_1547))) * 2 - 1`. With no lane the geometric
    // terminator is the game's own answer (`toon_sdf_mask`'s no-lane contract is the caller's, and the article
    // chain makes the same substitution when a material has no SDF).
    const float sdf_signed = (sdf_lane != 0u)
                                 ? abs(-sdf_smooth - half_face_no_l * ceil(half_face_no_l)) * 2.0 - 1.0
                                 : dot(normal, light_dir);
    const float sdf_u = sdf_signed * 0.5 + 0.5;

    // ---- 7. THE FACE MASK (`_SDFMask`, sampled at the ORIGINAL uv; :508-512) ----
    //
    // THIS IS `_SdfRefineTex` IN THE AUTHOR'S VOCABULARY (the inference `face_shader_compare.md` §1 records);
    // the game name and deren's lane 6 are the same texture family on this asset
    // (`T_actor_common_female_face_01_cm_M`). Lane 5 (`_MetallicGlossMap`) is deliberately NOT read: the game
    // face keyword set has no `_METALLICSPECGLOSSMAP`, so metallic/roughness come from the glTF factors.
    //
    // The four channels are four different statements in the game's own code: `.x` scales the SSS strength
    // (:643 `_508.x`), `.y` blends SDF against geometric NoL for the ramp coordinate AND the shading normal
    // (:642/771/762 `_510`), `.z` is the shadow-ring weight (:777 `_511`), `.w` is the rim region
    // (:817 `_512`). The default below is the article chain's "mask absent" value (full SSS region, SDF term,
    // no ring, no rim gate); for a material with no mask that means the SDF alone decides the face, which is
    // the same reading deren's article chain documents.
    float4 face_mask = float4(1.0, 0.0, 0.0, 0.0);
    if (toon_lanes.z != 0u) {
        face_mask = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + toon_lanes.z),
                               heap_sampler_texture, uv);
    }

    // ---- 8. THE SDF NORMAL (:760-762 `_1522/_1529`) ----
    //
    // The SDF lightmap's B channel is a 2D normal term. The game mirrors it with the sample's flag, rebuilds
    // `normalize(float3(x, eps, 1 - |x|))`, rotates it by the object matrix and lerps it into the shading
    // normal by the mask's `.y`. deren's head axes are the world directions of that matrix for the head, so
    // the rebuild is `-head_right*x + head_up*y + head_front*z` (object +X bridge).
    float3 shading_normal = normal;
    if (sdf_lane != 0u) {
        const float sdf_b = sdf_texel.b;
        // :1514/:1517 `_1514 = _1512.z * 2`; `_1517 = lerp(1 - _1514, _1514 - 1, mirror)`.
        const float sdf_b_mirrored = lerp(1.0 - sdf_b * 2.0, sdf_b * 2.0 - 1.0, sdf_mirror);
        const float3 sdf_ts_normal = normalize(float3(sdf_b_mirrored, 6.10351562e-05, 1.0 - abs(sdf_b_mirrored)));
        const float3 sdf_world_normal = normalize(head_right * game_face_object_x(sdf_ts_normal.x) +
                                                  head_up * sdf_ts_normal.y +
                                                  head_front * sdf_ts_normal.z);
        shading_normal = normalize(lerp(sdf_world_normal, normal, face_mask.y));
    }

    // ---- 9. THE RAMP COORDINATE (:771) and the `_DiffRampMap` read ----
    //
    // The game's U is `lerp(sdf_term, clamp(NdotL + offset, -1, 1), mask.y) * 0.5 + 0.5`: the SDF term where
    // the mask says "use the shape", the geometric term elsewhere. The offset `_19_m90.w * _19_m91.x` is an
    // engine global (MISSING -> 0.0). The V is 0.5 in the decompile; the ramp atlases here are one-row
    // (256x1), so deren's named band constant is the same read and keeps the family consistent.
    const float no_l = dot(normal, light_dir);
    const float ramp_no_l = clamp(no_l + 0.0, -1.0, 1.0) * 0.5 + 0.5;
    const float ramp_u = saturate(lerp(sdf_u, ramp_no_l, face_mask.y));
    float3 ramp_colour = float3(1.0, 1.0, 1.0);
    float ramp_alpha = 1.0;
    if (s.toon_indices.x != 0u) {
        const float4 ramp_texel = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.x),
                                             heap_sampler_texture, float2(ramp_u, character_ramp_band_v));
        ramp_colour = ramp_texel.rgb;
        ramp_alpha = ramp_texel.w;
    } else {
        // NO RAMP LANE. The game samples `_DiffRampMap` unconditionally (`_DIFF_RAMP_ON`); with no lane the
        // faithful degradation is deren's family fallback (the same Hermite step the cloth arm uses for a
        // material whose lane is absent), NOT a colour: the tint stays neutral.
        ramp_alpha = smoothstep(params.center - params.softness, params.center + params.softness, ramp_u);
        ramp_colour = float3(1.0, 1.0, 1.0);
    }

    // ---- 10. SSS (:643-645) ----
    //
    // `_1155 = mask.x * lerp(saturate(cameraForward_faceDir.z + 0.5), 1, mask.y)` and
    // `sssArea = clamp((1 - clamp(NdotV*0.85 + 0.15)) * (_1155 * lerp(_48_m31, _48_m30, mask.z)), 0, 1)`.
    // MISSING: `_48_m30`/`_48_m31` are two per-material floats with no deren lane; deren's family
    // `sss_strength` (face 0.6) stands in for their mask-z lerp. MISSING: the SSS colour `_48_m34.xyz` is
    // per-material in the game; deren's rig carries ONE `_SssColor` (`rig.sss.rgb`), which is the substitute.
    const float no_v = saturate(dot(shading_normal, view_dir));
    const float sss_no_v = no_v * 0.85 + 0.15;
    const float sss_weight = face_mask.x * lerp(saturate(head_face_dir_z + 0.5), 1.0, face_mask.y);
    const float sss_area = saturate((1.0 - sss_no_v) * (sss_weight * params.sss_strength));
    const float3 albedo_sss = albedo * lerp(float3(1.0, 1.0, 1.0), rig.sss.rgb, sss_area);

    // ---- 11. THE P/F0 BLOCK (:730-734) ----
    //
    // `:730` 0.96 - 0.96*M; `:731` diffuse = albedo * that; `:732` F0 = lerp(0.04 * specular, albedo, M);
    // `:733` shadow albedo * that (the `_19_m79.z` scale on the same line is an engine global, MISSING -> 1);
    // `:734` roughness2 = max(occlusion^2, 0.0078125).
    // The game's `_Metallic` / `_Smoothness` (`_48_m2` / `_48_m0`) have no deren material lane, so deren's
    // glTF factors from `surface_sample` stand in - the same quantities the game reads when the exporter has
    // converted them. `_Specular` comes from its colour lane, with the family value as the "not stated" answer.
    const float metallic = s.metallic;
    const float roughness = max(s.roughness, 1.0e-4);
    const float specular_strength = (lane_specular >= 0.0) ? lane_specular : params.spec_strength;
    const float spec_scale = lerp(0.0, specular_strength, face_mask.y); // :646 `_1178`
    const float energy_metallic = 0.96 - 0.96 * metallic;
    const float3 diffuse_albedo = albedo_sss * energy_metallic;
    const float3 f0 = lerp(float3(0.04 * spec_scale, 0.04 * spec_scale, 0.04 * spec_scale), albedo_sss, metallic);
    const float3 shadow_albedo = lut_colour * energy_metallic;
    const float roughness2 = max(roughness * roughness, 0.0078125);

    // ---- 12. THE LIT WEIGHTS (:776-781) ----
    //
    // :777 `_1602 = max(mask.y, mask.z * smoothstep(0.75, 0.25, _592))` - the reversed-edge smoothstep is the
    // decompile's, written here as `1 - smoothstep(0.25, 0.75, x)` which is algebraically identical for both
    // arms and avoids an edge-order-dependent `smoothstep`. :778 `_1605 = (1 - _1602) + shadowMask * _1602`:
    // the cast shadow only reaches the ring the mask marks. MISSING: the shadow mask is deren's
    // `calc_shadow` here (`_39`, t22 at :746-748, is a screen texture this stage cannot sample - it holds its
    // own depth attachment, the same reason the article port documents for its depth rim).
    const float camera_ring = 1.0 - smoothstep(0.25, 0.75, head_face_dir_z);
    const float shadow_ring = max(face_mask.y, face_mask.z * camera_ring);
    const float shadow = saturate(calc_shadow(world_pos, normal));
    // Missing screen-X visibility and its enable/override globals: neutral 1.
    // Raster shadow remains the screen-Y/ring substitute; do not conflate them.
    const float light_visibility = 1.0;
    const float shadow_area = (1.0 - shadow_ring) + shadow * shadow_ring;
    // :671 `_1265` is the base map's alpha along the skipped-highlight branch; :780/781 use it as the
    // occlusion and the lit pair. MISSING: the highlight branch's occlusion override (it needs the absent
    // `_HighlightMap`); this is exactly the else-arm value.
    const float occlusion = s.alpha;
    const float lit_weight = min(min(shadow_area, occlusion), ramp_alpha);
    const float lit_shadow = occlusion * shadow_area;

    // ---- 13. ORIGINAL MATERIAL COLOUR COMPOSITION ----
    // _1647/_1653/_1662/_1668 are shared with numerical CPU checks.
    // _1646 still uses deren's main light; game irradiance/art-direction globals
    // remain bridged by the separately documented environment term below.
    const FaceDiffuseResult diffuse_composition = game_face_diffuse_composition(
        diffuse_albedo, shadow_albedo, ramp_colour, ramp_alpha,
        occlusion, shadow_area, face_mask.y, light_visibility);
    const float3 diffuse_final = diffuse_composition.colour;
    const float surface_lit = diffuse_composition.surface_lit;
    const FaceDirectLight direct_light = game_face_direct_light(main_light_colour * main_light_intensity, surface_lit);
    const float3 main_diffuse_result = direct_light.diffuse * diffuse_final;

    // ---- 14. SPECULAR: the game's D*V fit (:793-810) ----
    //
    // `_1693` builds the half vector from the light, a camera-forward "forward light" and the view vector, all
    // scaled by the shadow term; `:1694-1699` are `a2 = roughness2^2` and `(NdotH*a2 - NdotH)*NdotH + 1`
    // squared; `:810` clamps `D*V` to [0, 20] and multiplies `F0` by the lit light. `_1397` is the F0 block's
    // output. MISSING on the same line: `_19_m92.w` (an engine scale, 1.0 neutral), `_1728` (the screen-space
    // reflection sample, 0) and `_1760` (the face-highlight matcap, gated by `_1268 = 0` in the skipped branch).
    const float3 forward_light_dir = normalize(float3(camera_forward.x, lerp(0.5, light_dir.y, light_visibility), camera_forward.z));
    const float3 half_dir = normalize(view_dir * (2.0 + light_visibility) + light_dir * light_visibility + 2.0 * forward_light_dir);
    const float no_h = dot(shading_normal, half_dir);
    const float a2 = roughness2 * roughness2;
    float spec_d = (no_h * a2 - no_h) * no_h + 1.0;
    spec_d *= spec_d;
    spec_d = a2 / spec_d;
    const float spec_v = 0.5 / (no_v * 2.0 + roughness2 + 9.99999975e-05);
    const float spec_dv = clamp(spec_d * spec_v - 6.10351562e-05, 0.0, 20.0);
    const float3 direct_specular = f0 * spec_dv * direct_light.specular;

    // ---- 15. RIM (:813-817) ----
    //
    // MISSING: the game's rim AXIS `_1803` is `cross(cameraForward, lerp(_19_m88.xy, camRight*_19_m88.x +
    // camUp*_19_m88.y, _19_m94.w))` and its two colours/scales are `_19_m87` / `_19_m93` - per-material
    // art-direction vectors with no deren lane. The SHAPE below is deren's view-angle fresnel (the same one
    // the cloth/skin arms use); the TINT is the game's own `_SDFRimColor`; the MASK is the game's own
    // `_SDFMask.w` region and the same `min(occlusion, shadow)` gate the game applies at :817. The game's
    // second (backlight-facing) rim term is not ported because it reads `_19_m93`/`_19_m88`.
    // Directional fallback, not recovered game art-direction: front lighting must not
    // create a backlit halo. Use the geometric contour rather than the SDF normal,
    // whose authored internal changes can otherwise produce a bright nose patch.
    float3 rim_normal = normalize(geo_normal);
    if ((mat.flags & 8u) != 0u && !is_front) rim_normal = -rim_normal;
    const float rim_no_v = saturate(dot(rim_normal, view_dir));
    const float rim_light_weight = game_face_rim_light_weight(dot(light_dir, view_dir), dot(rim_normal, light_dir));
    const float rim_start = rig.rim.x * -0.6 + 0.8;
    const float rim_width = (rig.rim.x * -0.4 + 0.9) - rim_start;
    const float rim_t = saturate(((1.0 - rim_no_v) - rim_start) / max(rim_width, 1.0e-6));
    const float rim_area = rim_t * rim_t * (3.0 - 2.0 * rim_t);
    const float rim_result = rim_area * min(occlusion, shadow);
    const float3 rim = sdf_rim_colour * (rig.rim.y * saturate(face_mask.w) * rim_result * rim_light_weight);

    // ---- 16. ENVIRONMENT (dere's IBL on the approved "keep deren lighting" boundary) ----
    //
    // MISSING: the game's own environment block (:1203-1268) samples a 3D volume (`t36`) with the
    // `_19_m4*`/`_19_m5*` globals - there is no deren counterpart. deren's env helpers stand in, gated by the
    // family's own strengths; the FACE family states `ambient_strength = 0.0` and
    // `ibl_specular_strength = 0.0` (`toon_params.slang:338,362`), so on this asset both are inert and the
    // face keeps the author chain's "no ambient" arrangement rather than gaining a second environment.
    const float3 ambient = environment_irradiance(shading_normal) * albedo * s.ao *
                        max(params.ambient_strength, game_face_min_ambient);
    const float env_level = log2(max(0.01, roughness)) * 1.2 + 5.0;
    const float3 env_specular =
        environment_specular_sample(reflect(-view_dir, shading_normal), env_level) * f0 * params.ibl_specular_strength;

    // `_2776 * _19_m20.y` (:1203/1209): `_19_m20.y` is the engine's exposure; MISSING -> 1.0.
    // The game's `_48_m12 > 0.5` tone/rim-off block (:1199-1208) is a material toggle with no deren lane and
    // is NOT applied; `_2776`'s branch is a post-style desaturation, not a face term.
    return main_diffuse_result + direct_specular + rim + ambient + env_specular + s.emissive;
}
