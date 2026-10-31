// ==================== game_toon/cloth.slang - THE ORIGINAL GAME CLOTH PS, FIRST-PASS PORT ====================
//
// SOURCE OF TRUTH (read, not guessed):
//   docs/references/toon/cloth_forward_readable.hlsl - the readable reconstruction of the ORIGINAL game cloth
//   fragment shader from SPIR-V:
//     binary : tmp/game-shader-audit/archive/_npr_package_20260925/_npr_recon/artifacts/
//              npr_main_shaders/code/shader_9371ff9c9e74391e_lod600_Vulkan_2_0_raw.bin (16,736,244 B)
//     record : offset 5,163,656; module: SMOL-V at offset 5,164,112 (Tesearis.SmolVDecoder 1.0.0)
//     output : spirv-cross 1.4.357.0 --hlsl --shader-model 50
//     keywords: _DIFF_RAMP_ON _EMISSION _METALLICSPECGLOSSMAP _NORMALMAP _SPEC_RAMP_ON,
//               plus _ANISOTROPY_SPECULAR_ON _CLEARCOAT DITHER on the record
//   channel/formula map: docs/references/toon/cloth_port_map.md
//   material lanes:      build-release-dyn-clang64/chars/laevatain_goo.glb.toon.tsv (+ the other characters')
//
// THE BOUNDARY THIS FILE KEEPS:
//  * the GAME'S SURFACE RESPONSE is translated: the P/ORM channel dictionary, the packed normal decode, the
//    diffuse-base and F0 formulas, and the two ramp lookups at the coordinates the readable file verified
//    against the original D3D ASM (DiffRamp at the MESH uv, SpecRamp at v = 0.5);
//  * deren's lighting/shadow/IBL stands in for Unity's: `calc_shadow`, `light_at`, `environment_irradiance`,
//    `environment_specular_sample`. NO Unity constant/matrix is copied - the readable file's `_19_mNN`
//    frame globals (exposure, day/night weights, screen-space shadow mask, fog, the reflection probe) have no
//    counterpart here and are deliberately not ported; where a deren lane performs their ROLE it is named at
//    the use site;
//  * THIS IS A FIRST PASS. No A/B against the game render has been made for this file, and no visual accuracy
//    is claimed. Every input the decompiled module keeps in a stripped per-material member (`_48_mNN`) is
//    either absent (`// MISSING:`) or replaced by a documented deren fallback - never by an invented
//    per-material value;
//  * the old procedural family-tint cloth body is NOT used: a missing ramp degrades to a NEUTRAL ramp, never
//    to `params.shadow_tint`, and `toon_diffuse` is not called.
//
// The function is included from `game_toon.slang` after `character_forward.slang`, so every deren helper/type
// below (`surface_sample`, `Material`, `toon_params`, `ToonRigUBO`, `heap_*`, `toon_lanes*_at`, `surface_tbn`,
// `calc_shadow`, `light_at`, `environment_*`) is already in scope. This file adds no include of its own; the
// guard is only so an accidental double include reports nothing rather than a redefinition.
#ifndef DEREN_GAME_TOON_CLOTH_SLANG
#define DEREN_GAME_TOON_CLOTH_SLANG

/// @brief the game's luminance weights, used verbatim from the readable file (lines 457, 687, 961, ...)
/// @note the numbers are `_488`'s dot product in `cloth_forward_readable.hlsl`
///       (`0.2126729/0.7151522/0.0721750`), which is the SAME weighting deren's toon chain calls `toon_luminance`.
static float cloth_port_luma(float3 colour) {
    return dot(colour, float3(0.212672904, 0.715152204, 0.0721750036));
}

/// @brief the packed tangent-space normal decode of `cloth_forward_readable.hlsl:458-465`
/// @param rg the packed XY: the readable code computes `_514.w = _514.w * _514.x` and then reads `_514.wy`,
///           i.e. `(R * A, G)`; it never reads the stored blue. This asset's normal maps store B = 0
///           everywhere (verified per channel on the shipped PNGs - see `character_forward.slang`'s
///           `DecodeNormal` note), so the reconstruction is required rather than optional.
/// @param bump_scale `_48_m3` = `_BumpScale`; deren's `Material::normal_scale` is the glTF normal scale,
///        which is where the asset pipeline writes that number.
/// @return the tangent-space normal with `z = max(1e-16, sqrt(1 - clamp(dot(xy,xy), 0, 1)))`
static float3 cloth_port_unpack_normal(float2 rg, float bump_scale) {
    float3 normal;
    normal.xy = rg * 2.0 - 1.0;
    normal.z = max(1.000000016862383526387164645044e-16, sqrt(1.0 - saturate(dot(normal.xy, normal.xy))));
    normal.xy *= bump_scale;
    return normal;
}

/// @brief the game's environment-BRDF fit (`cloth_forward_readable.hlsl:1050-1051`), as (brdf, bias)
/// @note `_2539`/`_2544` are the two matrix expressions the readable file writes out; this is their
///       expanded scalar form, and it is the SAME algebra the author chain's cloth main carries
///       (`character_forward.slang`'s `fit_a..env_bias` block), so the two families' environment fits
///       cannot drift apart without one of them being edited.
/// @param no_v N.V, saturated
/// @param roughness2 the game's `_2525`, i.e. ROUGHNESS SQUARED (not `_2055`'s max-clamped form)
static float2 cloth_port_env_brdf_fit(float no_v, float roughness2) {
    const float no_v2 = no_v * no_v;
    const float no_v3 = no_v2 * no_v;
    const float r6 = roughness2 * roughness2 * roughness2;
    // `_2539`: `mul(float2(1, NoV), mat2(...)) . (1, r2)` over its own denominator
    const float fit_a = 3.32707 * no_v + 0.0365463;
    const float fit_b = -9.04755 * no_v + 9.0632;
    const float fit_x = 3.59685 * no_v2 - 1.36772 * no_v3 + 1.0;
    const float fit_y = 9.22949 * no_v3 - 16.3174 * no_v2 + 9.04401;
    const float fit_z = -20.2123 * no_v3 + 19.7886 * no_v2 + 5.56589;
    const float brdf = (fit_a + fit_b * roughness2) / dot(float3(fit_x, fit_y, fit_z), float3(1.0, roughness2, r6));
    // `_2544`: the bias fit, same shape (`_2547`/`_2548` combine the two)
    const float bias_x = dot(float3(2.92338, 59.4188, 1.0), float3(no_v, no_v3, 1.0));
    const float bias_y = dot(float3(1.0, -27.0302, 222.592), float3(20.3225, no_v, no_v3));
    const float bias_z = dot(float3(626.130, 316.627, 1.0), float3(no_v, no_v3, 121.563004));
    const float bias = (dot(float2(-1.28514, 1.0), float2(no_v, 0.990440011)) +
                        dot(float2(1.0, -0.75591), float2(1.29678, no_v)) * roughness2) /
                       dot(float3(bias_x, bias_y, bias_z), float3(1.0, roughness2, r6));
    return float2(brdf, bias);
}

/// @brief the ORIGINAL game cloth surface response, on deren's light/shadow/IBL - first-pass port
/// @param s the gathered surface (dere's albedo/alpha/AO/emissive/flags/family/toon lanes)
/// @param world_pos interpolated world position
/// @param geo_normal interpolated geometric normal
/// @param uv the material's uv
/// @param is_front the rasterizer's front/back decision (the game flips the normal on backsides,
///        readable `_576`, line 472)
/// @note citations are `cloth_forward_readable.hlsl` line numbers, referred to as `readable:NNN`.
float3 game_toon_cloth_shade(surface_sample s, float3 world_pos, float3 geo_normal, float2 uv, bool is_front) {
    const ToonRigUBO rig = toon_rig_value();
    const Material mat = material_at(heap_slots_materials, push.material_index);
    const toon_params params = toon_params_for(s.toon_family);

    // ========================================================================================================
    // 1. THE P/ORM MAP: the readable's `_ShadowLutTex` (t2), read as metallic/reflectivity/AO/smoothness
    //    (readable:450-454; `cloth_port_map.md` §1)
    // ========================================================================================================
    //
    // THE REFLECTION NAME IS THE TRAP, AND THE CODE SETTLES IT. The native bindings call t2 `_ShadowLutTex`,
    // but readable:450-454 read its RGBA as M / G / AO / smoothness, and the map NAMED `_MetallicGlossMap`
    // (t3) is then read only as an additive detail at readable:1086 - so the two REFLECTION names are swapped
    // relative to the data the code actually reads, which is exactly the conflict `cloth_port_map.md` §1/§5
    // leaves open. THE ASSET SIDE RESOLVES IT: every character sidecar binds the P image
    // (`T_actor_*_cloth_01_P`) to the property `_MetallicGlossMap`, and deren's `toon_slot::metallic_gloss`
    // lane is defined as exactly that image's four channels ("metallic / reflectivity / ambient occlusion /
    // smoothness", `primitive.cppm`). So this function reads the P LANE, not the shadow-LUT lane:
    // `toon_lanes2_at(...).y` is `toon_slot::metallic_gloss` (lane 5 = block 1, element 1 - `heap_access.slang`
    // names that block's y as the `_MetallicGlossMap` index).
    //
    // THE SECOND SOURCE IS MEASURED, NOT GUESSED. `laevatain_goo.glb.toon.tsv` carries ONLY the Goo chain's
    // rows for its cloth materials (no `_MetallicGlossMap`), so lane 5 is 0 there - but the glb's own P image
    // IS its glTF `metallicRoughnessTexture` (`T_actor_laevat_cloth_01_P`, image 5; channel means over 2048^2:
    // R 41.5, G 254.9, B 160.0, A 80.1, i.e. the P dictionary), and deren uploads the glTF MR slot as
    // `rgba8_unorm` (`runtime.constructor.cppm`). `mat.tex_indices.y` is that slot. The white fallback is
    // element 0, so `mat.tex_indices.y != 0u` means "a real MR texture exists" (deren's own "0 == do not read"
    // contract for the lane tables).
    //
    // THE THIRD SOURCE IS THE FAMILY TABLE, and it is deren's contract rather than a guess: `toon_params`'
    // `roughness`/`reflectivity` ARE documented as "what the family says a surface is when the model ships no
    // map" (`toon_params.slang`). Metallic has no family field:
    // // MISSING: per-material metallic term (dere's family table has none); 0.0 is used for the no-P case
    //    because these recipes' cloth is dielectric and the shipped P maps' R channel is the metallic channel.
    const uint p_lane = toon_lanes2_at(heap_slots_toon_lanes, push.material_index).y;
    float4 p_texel;
    if (p_lane != 0u) {
        p_texel = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + p_lane), heap_sampler_texture, uv);
    } else if (mat.tex_indices.y != 0u) {
        p_texel = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + mat.tex_indices.y), heap_sampler_texture, uv);
    } else {
        // // MISSING: the P map on this material. Documented family fallback, no fabricated per-material value.
        p_texel = float4(0.0, params.reflectivity, s.ao, 1.0 - params.roughness);
    }
    const float metallic = p_texel.r;       // `_493`, readable:451
    const float reflectivity = p_texel.g;   // `_494`, readable:452
    const float occlusion = p_texel.b;      // `_495`, readable:453
    const float smoothness = p_texel.w;     // `_492.w`, readable:454
    const float roughness = 1.0 - smoothness; // `_497`, readable:454 - the game's own definition
    // readable:466-467 samples the OTHER name in the swap (`_MetallicGlossMap`); per the resolution above it is
    // the shadow LUT, used only as the additive detail at readable:1086. That detail needs `_48_m25` and
    // `_48_m7` (both stripped), so it is not ported:
    // // MISSING: `_MetallicGlossMap` additive detail (readable:1086); not the P source.

    // ========================================================================================================
    // 2. NORMAL: `_BumpMap` decode + TBN + the back-face flip (readable:458-465, 471-481, 573-578)
    // ========================================================================================================
    //
    // THE TANGENT-SPACE Y AXIS IS THE ONE BRIDGE, and it is the same documented one the established cloth port
    // uses: the game builds `_567` from Unity's VERTEX tangent (`_6`), while deren has no vertex tangent and
    // builds the frame from UV derivatives (`surface_tbn`). The game's normal maps store their Y in the game's
    // (Unity) tangent-space convention, the opposite Y direction from the derivative frame, so the bitangent
    // is negated. A future vertex-tangent path can replace this with the mesh's own `TANGENT.w`.
    float3 n_geo = normalize(geo_normal); // `_5`
    float3 n_shading = n_geo;             // `_577` when no normal map is present
    if ((mat.flags & 1u) != 0u) {
        float3 sdir;
        float3 tdir;
        if (surface_tbn(world_pos, uv, sdir, tdir)) {
            tdir = -tdir; // Unity/glTF tangent-space Y bridge (see above)
            // `_514 = _BumpMap.SampleBias(...)`; `_514.w = _514.w * _514.x` -> RG = (R*A, G) (readable:458-460)
            const float4 bump = frame_texture_sample(mat.tex_indices.z, uv);
            // `_533 = _524.xy * _48_m3` -> deren `Material::normal_scale` (the glTF normal scale, where the
            // pipeline writes `_BumpScale`)
            const float3 n_ts = cloth_port_unpack_normal(float2(bump.r * bump.a, bump.g), mat.normal_scale);
            n_shading = normalize(mul(n_ts, float3x3(sdir, tdir, n_geo))); // `_567`
        }
    }
    // `_576 = gl_FrontFacing ? 1 : (-1 + 2 * _48_m5)` (readable:472), `_48_m5` = `_BackFaceNormalFlip`; then
    // `_577 = normalize(_567) * _576` and `_578 = normalize(_5) * _576` (readable:473-474).
    // deren's rule for the same decision is the glTF double-sided flag (record bit3), whose back-face branch is
    // the -1 this asset needs:
    // // MISSING: `_48_m5` (`_BackFaceNormalFlip`, per material): the sidecars read for this port state 0.0
    //    (chen cloth), i.e. the same -1; a material that states 1 would keep the normal on backs.
    const float face_flip = ((mat.flags & 8u) != 0u && !is_front) ? -1.0 : 1.0; // `_576`
    n_shading = normalize(n_shading * face_flip); // `_577`
    n_geo = n_geo * face_flip;                    // `_578`

    // ========================================================================================================
    // 3. THE DIFFUSE RAMP, READ AT THE MESH UV (readable:659-660; a fact the file's header verified against
    //    the original D3D ASM)
    // ========================================================================================================
    //
    // `_1175 = _DiffRampMap.SampleBias(_25, _3, ...)` - `_3` is TEXCOORD0, the MESH uv - and only `.x` is read
    // (`_1176 = _1175.x`, readable:660). It is a per-texel CONTROL, not the terminator; the game's terminator
    // is a separate 1-D ramp lookup on `_EmissionMap` (readable:964/970) whose map identity this port could not
    // recover, and whose deren lane is therefore absent (see §6 below).
    //
    // // MISSING: cloth_01..05 carry no `_DiffRampMap` row in `laevatain_goo.glb.toon.tsv`; their only ramp row
    //    is `_GooBaseRamp` = lane 10, the GOO CHAIN's ramp, whose own coordinate is a different mechanism -
    //    sampling it at this chain's uv would conflate two chains, so it is not done.
    //    THE FALLBACK IS A NEUTRAL VALUE OF 1.0, NOT THE OLD PROCEDURAL FAMILY TINT: the old body's
    //    `params.shadow_tint` + Hermite step was an invented colour, and "no ramp" is the honest degradation.
    const bool has_diff_ramp = s.toon_indices.x != 0u;
    const float diff_ramp = has_diff_ramp
                                ? heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.x), heap_sampler_texture, uv).r
                                : 1.0; // `_1176`

    // ========================================================================================================
    // 4. THE P FORMULAS THE READABLE HEADER VERIFIED (readable:931-937; `cloth_port_map.md` §2)
    // ========================================================================================================
    //
    // `_2033` is the base colour - BaseMap.xyz * _BaseColor.xyz (readable:449, `_488`) - which deren has
    // already gathered as `surface_sample::albedo`. `_2034` is M; the snow branch (readable:889-930, whose only
    // write is `_2034 = lerp(_493, 0, _1965)` at :921) is its sole other writer and is not ported (see the
    // notes' uncovered list), so `_2034 == metallic` here.
    // The four formulas below are ALSO verified instruction-for-instruction in the original D3D PS
    // (`tmp/game-shader-audit/program/original.ps.asm:1073-1080`), which is what settles the one place the
    // reconstructed HLSL's folding is ambiguous:
    //     :1073  mad r3.x, -r6.x, 0.96, 0.96          -> 0.96 - 0.96*M                 (`_2036`)
    //     :1074  mul r15.xyz, r3.x, r5.xyz            -> albedo * (0.96*(1-M))          (`_2049`)
    //     :1075  mul r3.z, r6.y, 0.04                 -> 0.04*G
    //     :1076  mad r17.xyz, -r6.y, 0.04, r5.xyz     -> albedo - 0.04*G
    //     :1077  mad r17.xyz, r6.x, r17.xyz, r3.zzz   -> 0.04G + M*(albedo - 0.04G)      (`_2052`)
    //     :1078  mul r7.xyz, r3.x, r7.xyz             -> darkBase * (0.96*(1-M))        (`_2053`)
    //     :1079  mul r3.z, r3.y, r3.y
    //     :1080  max r3.z, r3.z, 0.007812            -> max(roughness^2, 2^-7)        (`_2055`)
    // readable:936 writes `_2052` as `lerp(0.04*lerp(1,_48_m33,G), albedo, M)`; that inner lerp is a
    // decompiler folding of the two instructions above, NOT a free per-material strength - the ASM computes
    // `0.04*G` directly - so the ASM form (the one the task and `cloth_port_map.md` §2 state) is what is used:
    //     diffuseBase = 0.96*(1-M)*albedo        F0 = 0.04*G + M*(albedo - 0.04*G)
    const float3 base_colour = s.albedo;
    const float metallic_term = 0.96 - 0.96 * metallic;      // `_2036`, readable:931 / asm:1073
    const float3 diffuse_base = base_colour * metallic_term; // `_2049`, readable:933 / asm:1074
    const float3 f0_dielectric = 0.04 * reflectivity;
    const float3 f0 = f0_dielectric + metallic * (base_colour - f0_dielectric); // `_2052`, readable:936 / asm:1075-1077
    // `_2032` is the DARK base: `_506 = _488 * _48_m18` (`_ShadowColorBrightness`), then
    // `_510 = lerp(luma(_506), _506, _48_m19)` (`_ShadowColorSaturation`) - readable:456-457, 510.
    // Neither member has a deren lane; deren's family table carries the two values its own toon chain uses
    // for the same job, and they are used as the documented fallback:
    // // MISSING: `_48_m18` `_ShadowColorBrightness` / `_48_m19` `_ShadowColorSaturation`.
    float3 dark_base = base_colour * params.albedo_dark_strength;
    dark_base = lerp(cloth_port_luma(dark_base).xxx, dark_base, params.albedo_dark_saturation);
    const float3 diffuse_dark = dark_base * metallic_term;         // `_2053`, readable:937
    const float roughness2 = max(roughness * roughness, 0.0078125); // `_2055`, readable:938

    // ========================================================================================================
    // 5. deren's LIGHT, SHADOW AND VIEW, in the roles the readable's Unity globals play
    // ========================================================================================================
    //
    // THE GAME'S UNITY GLOBALS ARE NOT COPIED: `_2116`/`_2134` (the main light's direction/colour),
    // `_2148`/`_2151` (the screen-space shadow mask), `_19_m79*`/`_19_m20*`/`_19_m92*` (exposure and the
    // day/night weights) all come from Unity's frame data. deren's equivalents are `light_at` (direction and
    // scalar sun intensity), `calc_shadow` (visibility), `camera_at`, and - for the specular strength - the
    // rig's `_SpecularStrength` lane (`toon_rig::env.z`, default 1 = neutral).
    const float3 light_dir = normalize(light_at(heap_light_slot).light_dir.xyz); // `_2116`'s role
    const float3 camera_pos = camera_at(heap_camera_slot).camera_pos.xyz;
    const float3 camera_forward = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz); // `_609`
    const float3 view_dir = normalize(camera_pos - world_pos); // `_454`
    const float sun_intensity = max(0.001, light_at(heap_light_slot).sun_intensity);
    const float shadow_vis = calc_shadow(world_pos, n_shading); // `_2148`/`_2151`'s role
    const float no_l = dot(n_shading, light_dir);               // `_2152` (unclamped, as the game writes it)

    // ========================================================================================================
    // 6. THE DIFFUSE LAYERS (readable:956-987, 1035)
    // ========================================================================================================
    //
    // THE GAME'S LIT WEIGHT IS `_2222 = min(min(_2148_shadow, _495_ao), _2196_ramp_alpha)` (readable:973): the
    // third term is the 1-D ramp's ALPHA, and that map's identity/lane is the piece this port could not
    // recover (readable:964/970 sample `_EmissionMap` at a light-axis coordinate; see the notes). This port
    // therefore stops at min(shadow, AO), and its TERMINATOR is deren's half-Lambert on the same shading
    // normal the game's `_2152` uses.
    // // MISSING: the game's 1-D light ramp (its map identity, and therefore its deren lane); the half-Lambert
    //    below is a documented bridge, not the game's curve.
    // The game's diffuse also carries a ramp-COLOUR tint (`_2260 = _2254 * ((1-_2205) + _2195.xyz*_2205)`,
    // readable:980) - not ported for the same reason; the neutral (untinted) diffuse is used instead.
    const float lit_weight = min(shadow_vis, occlusion);                                       // `_2222` minus the ramp term
    const float half_lambert = saturate(no_l * 0.5 + 0.5);                                     // deren terminator bridge
    const float3 diffuse_layer = lerp(diffuse_dark, diffuse_base, lit_weight);                 // `_2254`'s layer lerp
    const float3 diffuse_direct = diffuse_layer * half_lambert;

    // ========================================================================================================
    // 7. THE SPEC HALF VECTOR AND THE D/V FIT (`_2297`, `_2309`-`_2313`, `_2350`; readable:986-1005)
    // ========================================================================================================
    //
    // `_2297 = normalize(_2116*_2151 + normalize(_2286)*2 + _454*(2+_2151))`, where
    // `_2286 = (cameraForward.x, lerp(0.5, lightDir.y, _2151), cameraForward.z)` (readable:984-986). deren's
    // light, camera and shadow take those three roles below; `_2151` is the shadow weight (readable:957).
    const float3 forward_light = normalize(float3(camera_forward.x, lerp(0.5, light_dir.y, shadow_vis), camera_forward.z)); // `_2286`
    const float3 spec_half = normalize(light_dir * shadow_vis + forward_light * 2.0 + view_dir * (2.0 + shadow_vis));         // `_2297`
    const float no_v = saturate(dot(n_shading, view_dir)); // `_2299`, readable:987
    const float no_h = dot(n_shading, spec_half);          // used inside `_2309`, readable:991
    // `_2039 = _48_m30 * G` (readable:933; the aspect-scaled roughness pair is at readable:939-941) is the
    // anisotropy's ASPECT. THE DECOMPILED CODE DOES CARRY THE ANISOTROPIC SPECULAR BRANCH, in two places this
    // port cannot follow without new lanes:
    //   * readable:988-1005 - two aspect-scaled roughnesses `_2058`/`_2060` and the tangent-space half vector
    //     `_2309`, which need `_48_m30` (the aspect) and `_48_m32` (the second half vector's view blend,
    //     readable:997);
    //   * readable:1005's SECOND additive lobe - `(_2052 * _48_m38.xyz) * G * clamp(D2,0,20)` - which needs the
    //     anisotropy colour `_48_m38`; the record also carries the `_ANISOTROPY_SPECULAR_ON` keyword.
    // // MISSING: `_48_m30` (aspect), `_48_m32` (view blend), `_48_m38` (anisotropy colour). With the aspect at
    //    its identity 0, and with an orthonormal TBN, the `_2309`/`_2310` expression collapses to the closed form
    //    below (a4 = roughness2^2) - which is why no tangent frame is fetched - and the second lobe is not added.
    // NO CLEARCOAT TERM IS ADDED, AND THAT IS A READING RATHER THAN AN OMISSION: the record carries a
    // `_CLEARCOAT` keyword, but this module's whole readable body (readable:426-1547) reads no clearcoat member -
    // the only writer that changes M is the snow branch at readable:921 (`_2034 = lerp(_493, 0, _1965)`), which
    // is not ported. Adding a clearcoat lobe here would be inventing a term the decompiled code does not show.
    const float a4 = roughness2 * roughness2;                      // `_2305` at aspect 0
    const float hv2 = a4 * (1.0 - no_h * no_h + a4 * no_h * no_h); // `_2310` at aspect 0
    const float hv3 = a4 * a4 * a4;                                // `_2312` = `_2305`^3
    const float hv4 = hv2 * hv2;                                   // `_2313`
    const float d_aniso = (hv3 != hv4) ? (hv3 / hv4) : 1.0;        // `_2312 / _2313`, with the game's own guard
    // readable:1005's V term: `_2317 = 2*NoV` and `_2319 = (1+NoV)-NoV = 1`, so `0.5/(2*NoV + roughness2 + eps)`
    const float spec_v = 0.5 / (2.0 * no_v + roughness2 + 9.99999975e-05);
    const float spec_dv = clamp(d_aniso * spec_v - 6.10351562e-05, 0.0, 20.0); // `_2350`'s clamp
    const float3 spec_brdf = f0 * spec_dv;                                    // `_2350`, readable:1005

    // ========================================================================================================
    // 8. THE SPEC RAMP: sampled at v = 0.5 (readable:1006-1026; the second verified ASM fact)
    // ========================================================================================================
    //
    // `_2351 = _1176 > 0.001` gates the whole block on the DIFFUSE ramp's value, and the spec ramp coordinate
    // is `float2(_2358*_2358, 0.5)` where `_2358 = clamp(dot(_1187, _454), 0, 1)` - NoV SQUARED along u at a
    // fixed v = 0.5 (the game's second ramp read, per the readable header). `_1187 = lerp(_578, _577, _48_m41)`
    // is a geometric/shading normal blend:
    // // MISSING: `_48_m41` (normal blend) - 1.0 (the shading normal) is used.
    // // MISSING: `_1183 = _48_m43.xyz * lerp(0.04, 1.0, _48_m40)` (the specular colour x strength): white,
    // //    i.e. lerp's 1.0 end, is used.
    // // MISSING: `_48_m39` (the per-material smoothness that builds the spec-ramp roughness `_1171`) - the
    // //    P map's own roughness is used instead.
    // The block is sampled only when BOTH ramp lanes exist: with no diffuse ramp the gate's value is unknown,
    // and the modulation `_2402` would not be the identity, so the conservative documented choice is off.
    const bool spec_ramp_on = has_diff_ramp && (s.toon_indices.z != 0u) && diff_ramp > 0.001; // `_2351`
    float3 spec_modulation = float3(1.0, 1.0, 1.0); // `_2402` - the identity when the block is skipped
    float3 spec_brdf_final = spec_brdf;             // `_2403` == `_2350` when the block is skipped
    if (spec_ramp_on) {
        const float spec_u = no_v; // `_2358` on the shading normal (blend 1.0 above)
        // `_2366 = _1183 * _SpecRampMap.SampleLevel(_24, float2(_2358*_2358, 0.5), 0).xyz`
        const float3 spec_ramp_colour = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.z),
                                                   heap_sampler_texture,
                                                   float2(spec_u * spec_u, 0.5))
                                            .rgb;
        const float one_minus_vh = 1.0 - saturate(dot(view_dir, spec_half));  // `_2367`
        const float fresnel5 = one_minus_vh * one_minus_vh * one_minus_vh * one_minus_vh * one_minus_vh; // `_2370`
        const float3 spec_tint = (spec_ramp_colour * (1.0 - fresnel5) + fresnel5) * diff_ramp; // `_2375`
        const float3 spec_inverse = float3(1.0, 1.0, 1.0) - spec_tint;                         // `_2376`
        // the spec-ramp lobe's own D/V (readable:1021-1026), on the P map's roughness in place of `_48_m39`
        const float spec_rough2 = roughness2;                       // `_1171`
        const float spec_rough4 = spec_rough2 * spec_rough2;        // `_2379`
        const float no_h_geo = dot(n_geo, spec_half);               // `_2356`
        const float ramp_den = ((no_h_geo * spec_rough4) - no_h_geo) * no_h_geo + 1.0; // `_2383`
        const float ramp_den2 = ramp_den * ramp_den;                                   // `_2384`
        const float ramp_d = (spec_rough4 != ramp_den2) ? (spec_rough4 / ramp_den2) : 1.0; // `_2379/_2384`
        // readable:1026's V term: `2*_2358 + _1171*((1+_2358)-_2358) + eps`, with `_1171 = spec_rough2`
        const float ramp_v = 0.5 / (2.0 * spec_u + spec_rough2 + 9.99999975e-05);
        // `_2402 = lerp(1, 1-_2375, _1176)` - the diffuse/lit colour's specular-ramp modulation
        spec_modulation = lerp(float3(1.0, 1.0, 1.0), spec_inverse, diff_ramp);
        // `_2403 = _2350*(_2376*_2376) + clamp(_2375 * (_2379/_2384) * V, 0, 20)`
        spec_brdf_final = spec_brdf * (spec_inverse * spec_inverse) +
                          clamp(spec_tint * (ramp_d * ramp_v), float3(0.0, 0.0, 0.0), float3(20.0, 20.0, 20.0));
    }

    // ========================================================================================================
    // 9. THE ENVIRONMENT: deren's cubemap and irradiance, at the game's own LOD fit
    // ========================================================================================================
    //
    // `_CharMaxCubemap.SampleLevel(reflect(-V, N), 1.2*log2(max(roughness, 0.001)) + 5.0)` (readable:1056-1058)
    // times the environment-BRDF fit at readable:1050-1051, which is `_2547`/`_2548` below. deren's
    // `environment_specular_sample` IS the cubemap (it applies the engine's environment intensity and
    // rotation), so the game's own probe/rotation globals `_19_m88`/`_19_m92` are not copied. The `_1176`-
    // scaled SECOND environment lobe of the spec-ramp path (`_2624`, readable:1059-1073) is not ported.
    const float2 env_fit = cloth_port_env_brdf_fit(no_v, roughness2); // `_2539`, `_2544`
    const float env_factor = env_fit.x + env_fit.y;                   // `_2548`
    // `_2547 = f0*_2539 + _2544`, and `_2578 = cubemap * (_2547 + (f0*((1-_2548)/_2548))*_2547)`
    const float3 env_brdf = (env_fit.x * f0 + env_fit.y) * (1.0 + f0 * ((1.0 - env_factor) / env_factor));
    const float env_lod = 1.2 * log2(max(roughness, 0.001)) + 5.0; // `_2578`
    const float3 reflect_dir = reflect(-view_dir, n_shading);       // `_2566`/`_2561`
    const float3 env_specular = environment_specular_sample(reflect_dir, env_lod) * env_brdf;

    // ========================================================================================================
    // 10. COMPOSITE (readable:1035 `_2415`): light colour * (diffuse * spec modulation + spec lobe)
    // ========================================================================================================
    //
    // deren's sun is white with a scalar intensity; the game's `_2253` (a shadow-weighted light colour) is its
    // role. The diffuse layer is already shadow/AO-gated by §6, so no second shadow multiply; the specular
    // keeps its own gate, and `rig.env.z` (`_SpecularStrength`, default 1) stands in for the game's
    // `_19_m92.w` specular-strength global.
    const float3 light_colour = float3(1.0, 1.0, 1.0) * sun_intensity;
    const float3 direct = light_colour * (diffuse_direct * spec_modulation + spec_brdf_final * (rig.env.z * shadow_vis));
    // THE ONE deren-SIDE ADDITION, AND IT IS A BRIDGE RATHER THAN THE GAME'S FORMULA: the readable cloth main
    // has NO ambient/IBL diffuse read of its own - its shadow side is meant to be lit by the game's own light
    // data (`_OtherLight*` / `_19_m90`, not ported) - so the engine's sky irradiance is added on the same terms
    // the character stage uses for every family:
    //     `environment_irradiance(n) * albedo * ao * ambient_strength`
    const float3 ambient = environment_irradiance(n_shading) * s.albedo * occlusion * params.ambient_strength;
    // `s.emissive` is deren's gathered emission. The readable's `_EmissionMap` samples at lines 964/970 are a
    // 1-D RAMP lookup in this variant (see §3/§6), not the emission texture; the gathered emission is the
    // engine's, and the readable's own emission composite is not traced - see the notes' uncovered list.
    // NOT PORTED: the rim term (readable:1078), the spec-ramp environment lobe (1059-1073), the metallic/gloss
    // additive (1086), snow/rain (readable:670-882, 889-930), the atmospheric pass (1488-1544) and the second
    // SV_Target (`_2103`) - all listed in tmp/game-toon-port/cloth-notes.md with the lanes they would need.
    return direct + env_specular + ambient + s.emissive;
}

#endif // DEREN_GAME_TOON_CLOTH_SLANG

