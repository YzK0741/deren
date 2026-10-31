// ============================ THE GAME'S HAIR SURFACE (game_toon family) ============================
//
// `game_toon_hair_shade` is the FIRST-PASS port of the original game's hair fragment shader, reconstructed in
// `docs/references/toon/hair_forward_spirv_decompiled.hlsl` from the Vulkan block
// `shader_8fa556110aa47b6f_lod600_Vulkan_1_block.bin` (record 2,252,396; module 2,252,796; keywords
// `_DIFF_RAMP_ON _METALLICSPECGLOSSMAP _NORMALMAP _SPECULAR_LINE _STROKE_ON`). It is a separate function rather
// than a branch in the cloth one because the game's hair is a different program: its shading normal is the
// game's packed `_BumpMap`, its highlight is TWO INTEGER-EXPONENT anisotropic (Kajiya-Kay) lobes weighted by a
// UV line pattern and the `_LineMap`/`_StrokeMap` pair, and its diffuse pair is built from the same two ramp
// reads on the light and camera axes the other families use.
//
// WHAT THIS FUNCTION IS NOT: a claim of visual accuracy. It is the game's formula structure with deren's
// lighting inputs substituted; every input whose game carrier has no deren equivalent is named at its use with
// `// MISSING:` plus a documented fallback, and every substituted frame/coordinate is called a BRIDGE.
//
// LINE NUMBERS: every `HLSL nnn` below is a line of `hair_forward_spirv_decompiled.hlsl` AS CHECKED IN. That
// file's provenance header adds 21 lines before its `struct _22`, so the provenance header's own KEY SITES are
// +21 here: its "450 normal reconstruction" is line 471, its "994-996 P/diffuse/F0" is 1015-1017, its
// "1047-1052 the two lobes and the line/stroke weighting" is 1068-1073, and its "1391 the summed anisotropic
// highlight" is 1412 - the per-light accumulation of the SAME lobe inside the mesh-light loop (see the notes'
// uncovered table; the single-sun form ported here is the one at HLSL 1073).
//
// THE BINDINGS: the decompiled file names its textures `t1`..`t46`, but its own header says those numbers are
// VULKAN DESCRIPTOR BINDINGS that spirv-cross re-emitted as t-slots, NOT D3D `_BaseMap`-style t registers - and
// the `t-register-anchors.tsv` artefact records that the numbers are PER-VARIANT. Every texture below is
// therefore matched by WHAT ITS SAMPLE FEEDS and by the per-material table's BYTE OFFSETS (`hair-reflection.tsv`,
// checked against the 464-byte `UnityPerMaterial` layout: `_BumpScale` at byte 12, `_ShadowColorBrightness` /
// `_ShadowColorSaturation` at 72/76, the anisotropy set at 208-260, `_AnisotropyColor2` at 240, `_StrokeMap_ST`
// at 256, `_LineAmount`..`_UseLineMap` at 272..292, `_LineMap_ST` at 304), never by the emitted number.
//
// THE INPUTS THIS FILE CANNOT TAKE FROM deren, NAMED ONCE SO THE FALLBACK IS ONE EDIT EACH:
//   * `_LineMap` / `_LineMap_ST` / `_LineAmount` / `_LineValue` / `_LineRange` / `_LineIntensity` /
//     `_LineSaturation` / `_UseLineMap` - the authored strand-line map and its seven scalars. deren's toon lanes
//     carry the ramp, shadow LUT, spec ramp, matcap, metallic/gloss, the face SDF set and the split normal, and
//     NOT these (see `deren::engine::toon_slot`). The fallback is the shader's OWN no-map degeneration
//     (`_UseLineMap = 0` leaves the UV stripe pattern) with the scalars recorded from the game's material row
//     for this asset's character (`M_actor_laevat_hair_01`).
//   * `_StrokeMap` / `_StrokeMap_ST` / `_StrokeScale` - same absence; with no map the offset term collapses to
//     its own neutral 0, which is the state the reference hair rows are in (`_StrokeOn = 0`).
//   * `_AnisotropyValue` / `_AnisotropyValue2` / `_AnisotropyIntensity` / `_AnisotropyRange2` /
//     `_AnisotropyEdgeFade` / `_AnisotropyColor2` / `_AnisotropyDirX` - per-material hair scalars with no deren
//     lane; recorded from the same material row (chen's row is recorded in the notes for comparison).
//   * `_HairBrowMask` / `_HairBrowMaskThreshold` and the strand-shine block that produces `_1975`/`_1977`/
//     `_1978`/`_1166`/`_1167` (HLSL 675-971) - not ported; the neutral values below keep the formulas the
//     game's shape (see the notes' "uncovered" table).
//   * the per-frame character globals `_20_m79`..`_20_m92` and the 3D hair light/occlusion volumes
//     (`_44`..`_49`, `_64`; HLSL 507-627, 1452-1504) - deren has no equivalent; the light model is deren's (see
//     the BRIDGE list) and the volume terms are NOT approximated.
//
// DELIBERATE BRIDGES (each a difference from the game, not a claim of equality):
//   * `_ScreenSpaceShadowMask` (HLSL 1031, the t22 `Load`) -> deren `calc_shadow`, sharpened by the hair
//     family's own `center`/`softness`. This is the approved "keep deren's shadow" boundary: the game's mask is
//     the deferred pass's, and this stage has no screen-space shadow resource.
//   * the game's vertex tangent frame `_6` and its `cross(_5, _6.xyz) * _6.w` bitangent -> `surface_tbn`'s
//     UV-derivative frame, with the same Unity/glTF tangent-space Y flip `game_toon_cloth_shade` documents (the
//     game's normal maps are stored in Unity's convention, and the original cloth port measured that flip).
//   * the game's OBJECT-space transforms (`_585` = the per-draw world-to-object 3x3) for the highlight's H
//     vector and for the edge-fade dot -> WORLD space, because deren's fragment path has no object basis and
//     every character in this repository is Y-up in world space (replacing the world Y is therefore the same
//     arithmetic the game performs with the object's up axis).
//   * `_AnisotropyDirX`'s object-space direction (HLSL 486-489) -> the surface's own derivative tangent,
//     blended toward the derivative bitangent by the P map's R mask - the same tangent-plane direction the
//     game's two cross products build. The object-space `(dirX, 1, 0)` the game rotates cannot be reproduced.
//   * `_ShadowColorBrightness` / `_ShadowColorSaturation` (HLSL 464-465) -> the family's
//     `albedo_dark_strength` / `albedo_dark_saturation`, the same substitution `game_toon_eye_shade` records
//     (the game's hair row states 0.55/1.1, the family table 0.5/1.0 - the family's numbers are used).
//   * `_DiffuseBlendEffect` (HLSL 1079, byte 24 of UnityPerMaterial) -> the rig's `env.w`, the lane every other
//     family reads.
//   * the two light-colour lerps the game builds from per-frame globals (HLSL 1053 and 1057) -> deren's
//     `main_light_final`, the cloth path's day/head-light model.
//   * the V coordinate of the ramp -> `character_ramp_band_v` (deren's constant), whose header
//     records that a single-row 256x1 ramp lands on the same texel anyway; the game hardcodes 0.5 (HLSL 1040).

// ------------------------------------------------------------------------------------------------------------
// THE NAMED FALLBACKS. Each is a value the game states per material (byte offsets below are the `_51_mN` slots of
// the decompiled cbuffer, mapped through `hair-reflection.tsv`'s 464-byte UnityPerMaterial layout) and deren does
// not carry; they are constants rather than literals so a Lead A/B is one edit and the provenance stays next to
// the number. THE VALUES ARE `M_actor_laevat_hair_01`'s OWN ROW from the game's material table -
// `_npr_package_20260925/_npr_recon/character/params/material-params.tsv` - i.e. the character this port is
// compared on, not a generic default. Another character's hair states different values (chen's row is in the
// notes); a per-material lane is the edit that stops this being one row for every hair material.
// ------------------------------------------------------------------------------------------------------------

/// `_AnisotropyValue` (HLSL 1064, byte 208): the first lobe's tilt of its normal out of the tangent plane.
/// MISSING: no deren lane; 0.49 is the laevat hair row.
static const float game_hair_anisotropy_value = 0.49;
/// `_AnisotropyValue2` (HLSL 1070, byte 212): the second lobe's tilt.
/// MISSING: no deren lane; 0.51 is the laevat hair row.
static const float game_hair_anisotropy_value2 = 0.51;
/// `_AnisotropyIntensity` (HLSL 1073, byte 216): the first lobe's strength (`* 5.0` in the program).
/// MISSING: no deren lane; 1.3 is the laevat hair row.
static const float game_hair_anisotropy_intensity = 1.3;
/// `_AnisotropyRange2` (HLSL 1073, byte 224): the second lobe's exponent control. The program's exponent is
/// `int(200 * max(1 - this, 0))`, so 0.5 makes the second lobe's exponent 100 - a hard integer, which is what
/// makes a strand's highlight a line rather than a falloff.
/// MISSING: no deren lane; 0.5 is the laevat hair row.
static const float game_hair_anisotropy_range2 = 0.5;
/// `_AnisotropyEdgeFade` (HLSL 491, byte 220): the exponent of the edge-of-strand fade (`pow(dot,
/// _AnisotropyEdgeFade)`) that gates both lobes.
/// MISSING: no deren lane; 5.0 is the laevat hair row.
static const float game_hair_anisotropy_edge_fade = 5.0;
/// `_AnisotropyColor2.rgb` (HLSL 1073, byte 240): the second lobe's tint, multiplied by the P map's smoothness.
/// MISSING: no deren lane; the laevat hair row's colour (`_AnisotropyColor2 = 0.672, 0, 0.004363678, 1`).
static const float3 game_hair_anisotropy_colour2 = float3(0.671999990940094, 0.0, 0.004363677930086851);
/// `_LineAmount` (HLSL 1072, byte 272): the UV stripe frequency of the line pattern.
/// MISSING: no deren lane; 300.0 is the laevat hair row.
static const float game_hair_line_amount = 300.0;
/// `_LineValue` (HLSL 1072, byte 276): the third (broad) lobe's tilt, `2 * this - 1`.
/// MISSING: no deren lane; 0.7 is the laevat hair row.
static const float game_hair_line_value = 0.7;
/// `_LineRange` (HLSL 1072, byte 280): the broad lobe's exponent control, `int(200 * max(1 - this, 0))`.
/// MISSING: no deren lane; 0.95 is the laevat hair row.
static const float game_hair_line_range = 0.95;
/// `_LineIntensity` (HLSL 1072, byte 284): how far the line pattern darkens the lit colour (`1 - this`).
/// MISSING: no deren lane; 0.22 is the laevat hair row.
static const float game_hair_line_intensity = 0.22;
/// `_LineSaturation` (HLSL 1075, byte 288): the saturation move the line weight drives.
/// MISSING: no deren lane; 1.5 is the laevat hair row.
static const float game_hair_line_saturation = 1.5;
/// `_535 = (stroke.x * 2 - 1) * _StrokeScale` (HLSL 474, bytes 256/232): the stroke map's tilt of the lobes'
/// normals. MISSING: no lane and no stroke map. 0.0 is the term's own neutral (a stroke texel of 0.5, and the
/// `_StrokeScale = 0` state chen's hair row is in); the laevat row's 0.2 arrives with `_StrokeOn = 0`.
static const float game_hair_stroke_offset = 0.0;
/// `_1975 = _1261 + 2 * _662` (HLSL 953): the strand block's gain on BOTH lobes. MISSING: the block at
/// HLSL 675-971 that produces it needs the game's extra vertex streams. 1.0 is the neutral of its shape
/// (`_1261 + 0.8 * _662` with the block's mask zero), so the lobes keep the program's form without its gain.
static const float game_hair_strand_gain = 1.0;
/// `_20_m92.w` (HLSL 1073): the per-frame specular scale on the summed highlight. MISSING: deren's light model
/// carries intensity on the sun instead, so 1.0. A per-family specular gate would be the rig's lane, not this.
static const float game_hair_pass_specular_scale = 1.0;

/// The hair's sky-diffuse bridge is multiplied by this when comparing against the reference/goo_ton emission.
/// The game's own environment term is the 3D integrated-light-scattering volume (HLSL 1452-1504), which deren
/// does not carry; the spherical-harmonics sky bridge under-reads the reference's hair brightness (measured mean
/// RGB over the hair crop: v2 [112.6,75.4,94.0] vs goo [121.1,80.9,99.6]). This is a CALIBRATION BRIDGE, not
/// the game's value: the game's missing volume is the real cause, and this factor is only here so the A/B is
/// readable until that lane exists.
static const float game_hair_ambient_scale = 2.5;

/// @brief the game's hair shading, `hair_forward_spirv_decompiled.hlsl` (record 2,252,396 / module 2,252,796),
///        with deren's light/shadow/IBL inputs substituted for the game's frame globals
/// @param s the gathered surface (albedo = `_BaseMap * _BaseColor`, alpha = its coverage)
/// @param world_pos interpolated world position
/// @param geo_normal interpolated geometric normal
/// @param uv the material's uv (the line pattern, the ramp axes and every ST-transformed map key off it)
/// @param is_front front-face flag, used only by deren's double-sided normal rule
float3 game_toon_hair_shade(surface_sample s, float3 world_pos, float3 geo_normal, float2 uv, bool is_front) {
    const ToonRigUBO rig = toon_rig_value();
    const Material mat = material_at(heap_slots_materials, push.material_index);
    const uint4 lanes = toon_lanes_at(heap_slots_toon_lanes, push.material_index);
    const toon_params params = toon_params_for(s.toon_family);

    // ---- THE MAPS (HLSL 457-475) ----
    //
    // `_56` (the program's base map sample) is `_457` and its colour/alpha are `_462`/`_473`; deren's
    // `gather_surface` has already done that exact product (`_BaseMap * _BaseColor`), so `s` carries them.
    const float3 albedo = s.albedo;                                                         // `_462`
    const float albedo_alpha = s.alpha;                                                     // `_473`

    // `_57` (HLSL 459) is the game hair's `_MetallicGlossMap` (`_METALLICSPECGLOSSMAP`): its four channels are
    // NUMBERS, not colour, and the program reads them as a strand/tangent blend mask (`.x`/`_467`), the F0
    // scale (`.y`/`_468`), its occlusion mask (`.z`/`_469`) and a smoothness tint (`.w`). deren's toon lane 5
    // (`toon_lanes_at(...).y`, `toon_slot::metallic_gloss`) is the equivalent carrier, uploaded as UNORM so
    // the raw numbers survive - and `character_forward.slang` already records that hair's `.x` is a strand
    // mask and NOT metallic (HLSL 1015 below folds metallic to zero anyway).
    // THE FALLBACK IS THE CHAIN'S OWN RULE for a material with no P lane: the family's `reflectivity` /
    // `roughness` and unoccluded/unglossed neutrals, not a baked texel. On `chars\laevatain_goo.glb` the hair
    // material states no `_MetallicGlossMap` sidecar row (its game-sidecar rows are the Goo chain's), so this
    // is the live path on the reference asset.
    float4 orm = float4(0.0, params.reflectivity, 1.0, 1.0 - params.roughness);
    if (lanes.y != 0u) {
        orm = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + lanes.y), heap_sampler_texture, uv);
    }
    const float strand_mask = orm.r;      // `_466.x` / `_467` - the strand/tangent blend weight
    const float reflectivity = orm.g;     // `_466.y` / `_468` - the F0 scale
    const float ao_mask = orm.b;          // `_466.z` / `_469` - the game's AO (deren's `s.ao` is glTF's and is 1 here)
    const float smoothness = orm.w;       // `_466.w` - tints the second lobe (HLSL 1073)

    // ---- THE SHADING NORMAL (HLSL 466-485; provenance header "450") ----
    //
    // HLSL 466-471 is the game's packed normal decode: `_499.w *= _499.x`, `_508 = (_499.wy * 2 - 1)`,
    // `_509.z = sqrt(1 - clamp(dot(_508,_508),0,1))` - X lives in R*A and Y in G, the same packing the cloth
    // reconstruction's `game_unpack_normal` records. HLSL 472 then scales XY by `_BumpScale` WITHOUT rescaling
    // Z (the length is fixed by the later `normalize`), which is why the order below is kept.
    float3 normal = normalize(geo_normal);
    float3 tangent = float3(1.0, 0.0, 0.0);
    float3 bitangent = float3(0.0, 1.0, 0.0);
    const bool frame_valid = surface_tbn(world_pos, uv, tangent, bitangent);
    if (frame_valid) {
        // THE TANGENT-SPACE Y BRIDGE: `game_toon_cloth_shade` documents that the game's maps are stored in
        // Unity's tangent-space convention, which is the opposite Y direction from the derivative frame here.
        bitangent = -bitangent;
        if ((mat.flags & 1u) != 0u) {   // deren's "the material has a normal map" bit (the game's `_NORMALMAP`)
            // `_58` (HLSL 466) is the game hair's `_BumpMap`. deren's equivalent is the material's normal
            // texture lane, `mat.tex_indices.z` - on `chars\laevatain_goo.glb` that texture is
            // `T_actor_laevat_hair_01_HN`, which the game's texture table names `_SplitNormalMap`. WHETHER the
            // game's `_BumpMap` and that HN map are the same image, and whether the HN pack's channel
            // dictionary leaves `R*A, G` meaningful, is UNRESOLVED (the notes list it under compile/render
            // risks); the decompiled decode is used as written rather than guessed again.
            const float4 n_texel = frame_texture_sample(mat.tex_indices.z, uv);
            const float2 n_packed = float2(n_texel.r * n_texel.a, n_texel.g);                // `_499.wy`
            const float2 n_raw = n_packed * 2.0 - 1.0;                                       // `_508`
            const float n_z = max(1.0e-16, sqrt(1.0 - saturate(dot(n_raw, n_raw))));         // `_509.z`
            const float3 n_ts = float3(n_raw * mat.normal_scale, n_z);                       // `_518` (`_BumpScale`)
            normal = normalize(mul(n_ts, float3x3(tangent, bitangent, normal)));             // `_567`
        }
    }
    // HLSL 484-485: `_576 = gl_FrontFacing ? 1 : (-1 + 2 * _BackFaceNormalFlip)`, applied to both `_577` (the
    // shading normal) and `_578` (the geometric one). deren's version of that statement is the model's
    // double-sided flag (bit3), which is what a glTF file states; `_BackFaceNormalFlip`'s own 0/1 has no lane.
    if ((mat.flags & 8u) != 0u && !is_front) {
        normal = -normal;
    }

    // ---- LIGHT, CAMERA AND THE TWO FLATTENED AXES (HLSL 476-479, 490-491) ----
    //
    // `_2148` is the main light direction and `_2152 = normalize(float3(_2148.x, 6.1e-05, _2148.z))` is the
    // SAME direction flattened on Y - the game uses the flattened one for the back-light test and the
    // diffuse/specular gate. deren's `light_dir` is the surface-to-light vector (`shading.slang`), the same
    // convention the game's `_2148` enters the dot products with.
    const float3 light_dir = normalize(light_at(heap_light_slot).light_dir.xyz);                  // `_2148`
    const float3 flat_light = normalize(float3(light_dir.x, 6.10351562e-05, light_dir.z));        // `_2152`
    const float3 camera_pos = camera_at(heap_camera_slot).camera_pos.xyz;
    const float3 view_dir = normalize(camera_pos - world_pos);                                    // `_423`
    const float3 camera_forward = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz); // `_625`
    const float main_light_intensity = max(0.001, light_at(heap_light_slot).sun_intensity);
    // deren's sun is white with `[render] sun_intensity` as its magnitude - the same bridge
    // `game_toon_cloth_shade` records. The game's `_20_m79` per-frame light colour has no deren lane.
    const float3 sun_colour = float3(1.0, 1.0, 1.0);

    // ---- THE SHADOW (HLSL 1031-1033) ----
    //
    // The game reads the deferred pass's `_ScreenSpaceShadowMask` (t22) at the fragment's pixel: `_2180` is its
    // `.y` and `_2183` its `.x` through the light's own shadow strength. deren's equivalent input is
    // `calc_shadow`, and the hair family's own edge (`center`/`softness`: a strand's terminator is a drawn
    // line, 0.52/0.015) sharpens it into the game's hard hair shadow. This is the approved "keep deren's
    // shadow" boundary rather than a tuned sign.
    const float shadow_scene = saturate(smoothstep(params.center - params.softness,
                                                   params.center + params.softness,
                                                   calc_shadow(world_pos, normal)));              // `_2180`/`_2183`

    // ---- THE STRAND FRAME AND ITS EDGE FADE (HLSL 486-491) ----
    //
    // `_586 = mul(_585, float3(_AnisotropyDirX, 1, 0))` rotates the material's object-space anisotropy
    // direction into the world; `_598 = cross(_577, lerp(cross(_577, normalize(_586)), _6.xyz, _467)) *
    // lerp(1, _6.w, _467)` then builds a TANGENT-PLANE direction that the two lobes tilt their normals along.
    // BRIDGE: the object basis and `_AnisotropyDirX` are unavailable, so the derivative tangent (the game's
    // `_6.xyz` role) blended toward the derivative bitangent by the P map's R mask is used.
    float3 strand = lerp(tangent, bitangent, strand_mask);
    strand = strand - normal * dot(strand, normal);
    if (dot(strand, strand) < 1.0e-8) {
        strand = bitangent - normal * dot(bitangent, normal);
    }
    strand = normalize(strand);
    // HLSL 490-491: `_609 = pow(clamp(dot(normalize(worldToObject(_577).xz), normalize(worldToObject(_600).xz)),
    // 0, 1), _AnisotropyEdgeFade)`. The object-space xz dot is the world xz dot under the Y-up bridge above.
    const float2 normal_xz = normalize(normal.xz + float2(1.0e-6, 0.0));
    const float2 view_xz = normalize(view_dir.xz + float2(1.0e-6, 0.0));
    const float edge_fade = pow(saturate(dot(normal_xz, view_xz)), game_hair_anisotropy_edge_fade);  // `_609`

    // ---- THE RAMP (HLSL 1038-1054) ----
    //
    // `_2184 = dot(_2096, _2148)` is NoL on the UNflattened light; the ramp's U is that value blended toward
    // the game's back-light curve `(-n) * (n * 0.5 - 1) + 0.5` (HLSL 1040) by the back-light weight, then
    // remapped to [0,1]. deren's chain writes the same polynomial as `no_l + (0.5 - 0.5 n^2) * back_light`
    // (its cloth path does), so the two forms are one expression.
    const float no_l = dot(normal, light_dir);                                               // `_2184`
    const float2 light_xz = normalize(flat_light.xz + float2(1.0e-6, 0.0));
    const float2 camera_xz = normalize(camera_forward.xz + float2(1.0e-6, 0.0));
    // `_2209 = clamp(-dot(_2152.xz, normalize(_625.xz)), 0, 1)` and the camera's own
    // `smoothstep(0.25, 0.75, 1 - abs(_625.y))`; `_2213 = 1 - _20_m91.x` is a per-frame weight with no deren
    // lane and is resolved to its neutral 1 here.
    const float back_light = saturate(-dot(light_xz, camera_xz)) *
                             smoothstep(0.25, 0.75, saturate(1.0 - abs(camera_forward.y)));
    float ramp_u = clamp(no_l + (0.5 - 0.5 * no_l * no_l) * back_light, -1.0, 1.0) * 0.5 + 0.5; // HLSL 1040
    // `_2245`'s second read: the same ramp on the CAMERA axis, `dot(_2096, _625) * 0.5 + 0.5` (HLSL 1046).
    const float no_f = saturate(dot(normal, camera_forward) * 0.5 + 0.5);
    float3 ramp_colour = float3(1.0, 1.0, 1.0);
    float ramp_alpha_nol = 1.0;
    float ramp_alpha_nof = 1.0;
    if (s.toon_indices.x != 0u) {
        // `_54` (HLSL 1040/1046) is `_DiffRampMap`; deren's lane 0 is that same map. The game's V is 0.5
        // (HLSL 1040) - a single-row 256x1 image whose only texel both coordinates reach - and deren's own band
        // constant lands on that texel too (see `character_ramp_band_v`).
        const float4 ramp_nol = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.x),
                                           heap_sampler_texture, float2(ramp_u, character_ramp_band_v));
        ramp_colour = ramp_nol.rgb;                                                          // `_2230`..`_2232`
        ramp_alpha_nol = ramp_nol.w;                                                         // `_2228`
        ramp_alpha_nof = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.x),
                                    heap_sampler_texture, float2(no_f, character_ramp_band_v)).w; // `_2246`
    } else {
        // MISSING: `chars\laevatain_goo.glb`'s hair material states no `_DiffRampMap` sidecar row (the game's
        // own hair rows always name one), so the lane is empty while the game reads it unconditionally.
        // FALLBACK: the family's own procedural edge (the same shape the cloth path uses for a no-ramp
        // material) with a NEUTRAL ramp colour, so the ramp's colour term below is an identity - no invented
        // tint replaces the artist's map.
        const float ramp_edge = smoothstep(params.center - params.softness, params.center + params.softness, ramp_u);
        ramp_colour = float3(1.0, 1.0, 1.0);
        ramp_alpha_nol = ramp_edge;
        ramp_alpha_nof = smoothstep(params.center - params.softness, params.center + params.softness, no_f);
    }
    // `_2237`: the ramp's own chroma is how much of its colour reaches the result (HLSL 1045).
    const float ramp_colour_strength = max(max(ramp_colour.x, ramp_colour.y), ramp_colour.z) -
                                       min(min(ramp_colour.x, ramp_colour.y), ramp_colour.z);    // `_2237`
    // The lit/dark layer selector with the ramp's alpha in it: `_2254` (HLSL 1050) AND `_2247`/`_2255`
    // (HLSL 1048-1050).
    const float ramp_layer_selector = min(min(shadow_scene, ao_mask), ramp_alpha_nol);           // `_2254`
    const float ao_shadow = ao_mask * shadow_scene;                                              // `_2247`
    const float ramp_ao_shadow = ramp_alpha_nof * ao_shadow;                                     // `_2255`

    // ---- THE P / DIFFUSE / F0 BLOCK (HLSL 1015-1018; provenance header "994-996") ----
    //
    // `_2101 = 0.96 - 0.96 * _2099` is the metallic energy distribution, `_2102 = _2098 * _2101` the lit albedo
    // layer, `_2105 = lerp(0.04 * _468, _2098, _2099)` the F0, `_2106 = _2097 * _2101` the dark layer.
    // `_2099` IS LITERALLY ZERO IN THIS COMPILED VARIANT: HLSL 1006/1013 write `lerp(0.0, 0.0, _2046)` and
    // `0.0`, so the game's hair is dielectric and its metallic is not a material input at all - the P map's R
    // channel is the strand mask above, which is why `character_forward.slang` forces hair's metallic to 0 too.
    // F0 therefore reduces to `0.04 * reflectivity`, and both albedo layers to `albedo * 0.96`.
    const float metallic = 0.0;                                                                   // `_2099`
    const float energy_distribution_metallic = 0.96 - 0.96 * metallic;                            // `_2101`
    const float3 diffuse_light = albedo * energy_distribution_metallic;                           // `_2102`
    const float3 f0 = float3(0.04 * reflectivity);                                                // `_2105`
    // HLSL 463-465: `_491 = _462 * _ShadowColorBrightness` then
    // `_495 = lerp(dot(_491, luma).xxx, _491, _ShadowColorSaturation)` - the game's DARK ALBEDO. deren has no
    // carrier for the two properties; the family's `albedo_dark_strength`/`albedo_dark_saturation` are the
    // documented bridge (the header says which game row states 0.55/1.1 instead).
    const float3 albedo_dark = lerp(float3(toon_luminance(albedo * params.albedo_dark_strength)),
                                    albedo * params.albedo_dark_strength,
                                    params.albedo_dark_saturation);                                   // `_495`
    const float3 diffuse_dark = albedo_dark * energy_distribution_metallic;                        // `_2106`

    // ---- THE THREE ALBEDO LAYERS AND THE RAMP TINT (HLSL 1055-1061) ----
    //
    // `_2191 = _2106 * _20_m79.z` and `_2192 = _2191 * 0.65` (the third "dark in dark" layer, deren's
    // `params.dark_in_dark` = the game's 0.65). `_2286` then pulls the dark-in-dark layer to 1.2x contrast,
    // blends the dark layer in by `_2247 * _2246 + _2228` and the lit layer by `_2254`.
    const float3 dark_in_dark = diffuse_dark * params.dark_in_dark;                               // `_2192`
    const float3 dark_mix = lerp(float3(toon_luminance(dark_in_dark)), dark_in_dark, 1.2);         // `_2286` inner
    const float layer_gate = saturate(ao_shadow * ramp_alpha_nof + ramp_alpha_nol);               // `_2247 * _2246 + _2228`
    const float3 albedo_mix = lerp(lerp(dark_mix, diffuse_dark, layer_gate),                       // `_2286`
                                   diffuse_light,
                                   ramp_layer_selector);
    // `_2292 = _2286 * ((1 - _2237) + _2227.xyz * _2237)` - the ramp's colour mapping, i.e.
    // `lerp(1, ramp_colour, chroma)`, the same expression the cloth path writes as `ramp_effect`.
    const float ramp_mix = clamp(ramp_colour_strength * params.ramp_colour_strength, 0.0, 1.0);
    const float3 coloured_mix = albedo_mix * lerp(float3(1.0, 1.0, 1.0), ramp_colour, ramp_mix);   // `_2292`
    // `_2301`'s ramp control: the albedo's own luminance over the coloured one's, clamped at 1.5.
    const float ramp_control = clamp(toon_luminance(albedo_mix) / max(toon_luminance(coloured_mix), 0.001), 0.0, 1.5);
    // `_2301`: the dark layer vs the 1.2x-contrast lit layer by the ramp/AO/shadow weight, then the ramp-
    // coloured albedo by the SHADOW (`_2183`), which is what makes the terminator hard.
    const float3 lit_layer = lerp(float3(toon_luminance(diffuse_light)), diffuse_light, 1.2);
    const float3 low_light = lerp(diffuse_dark, lit_layer, ramp_ao_shadow);
    const float3 diffuse_final = lerp(low_light, coloured_mix * ramp_control, shadow_scene);      // `_2301`

    // ---- THE LIGHT MODEL (deren's, the cloth path's day/head-light model) ----
    //
    // BRIDGE: HLSL 1053/1057 build `_2285` (the light's colour) from per-frame globals the game's engine writes
    // (`_20_m79`, `_20_m84`..`_20_m92`); deren carries the same split in the toon rig, so this is the
    // `game_toon_cloth_shade` construction of `main_light_final` with the hair path's own shadow selector.
    const float other_strength = rig.day.z > 0.0 ? rig.day.z : 1.0;
    const float3 main_light_colour_dark = rig.main_dark.rgb;
    const float3 main_light_color_strength = float3(toon_luminance(sun_colour * main_light_colour_dark));
    const float other_no_l = saturate(dot(float3(0.0, 1.0, 0.0), normal) + rig.day.y) * other_strength + rig.day.w;
    const float3 other_colour = lerp(rig.other_colour.rgb, float3(1.0, 1.0, 1.0), ramp_layer_selector) * other_no_l;
    const float3 other_day1 = other_colour * rig.other_light.y;
    const float3 other_day0 = other_colour * rig.other_light.x;
    const float3 main_light_new = lerp(main_light_color_strength, sun_colour, ramp_layer_selector) * main_light_intensity;
    const float3 main_light_final = lerp(other_day0, main_light_new + other_day1, rig.day.x);

    // ---- THE TWO ANISOTROPIC HIGHLIGHT LOBES (HLSL 1062-1075; provenance header "1047-1052") ----
    //
    // `_2333` is the program's half vector: `_2319` is the view direction with its Y replaced by
    // `lerp(0.5, _2148.y, _2183)`, `_2321 = _2148 * _2183` is the shadowed light, and
    // `_2329 = normalize(_2321 + 2 * _2319) + _423`, `_2333 = normalize(_2329)`. The Y replacement is done in
    // OBJECT space in the game (`_585`); the world Y is used here (see the BRIDGE list).
    const float3 v_shift = float3(view_dir.x, lerp(0.5, light_dir.y, shadow_scene), view_dir.z);  // `_2319`
    const float3 half_basis = normalize(light_dir * shadow_scene + 2.0 * v_shift);                // `_2329`
    const float3 half_dir = normalize(half_basis + view_dir);                                     // `_2333`
    // `_2328` / `_2363` / `_2403`: the strand direction tilted out of the plane by each lobe's own parameter.
    // The stroke map's `_535` offset is added here - it is `game_hair_stroke_offset` with no stroke lane.
    const float3 lobe_normal_1 = normalize(strand + normal * (2.0 * game_hair_anisotropy_value - 1.0 +
                                                              game_hair_stroke_offset));           // `_2328`
    const float3 lobe_normal_2 = normalize(strand + normal * (2.0 * game_hair_anisotropy_value2 - 1.0 +
                                                              game_hair_stroke_offset));           // `_2363`
    const float3 lobe_normal_3 = normalize(strand + normal * (2.0 * game_hair_line_value - 1.0));  // `_2403`
    // THE INTEGER EXPONENTS: HLSL 1068 is `pow(max(sqrt(1 - NdotH^2), 1e-4), 200.0)` and HLSL 1072/1073 are
    // `pow(..., float(int(200.0 * max(1 - _AnisotropyRange2/_LineRange, 0.0))))` - the truncation is the
    // program's, and 200/100/10 are its integers with the recorded rows' Range2 0.5 and LineRange 0.95.
    const float n1_dot_h = dot(lobe_normal_1, half_dir);
    const float n2_dot_h = dot(lobe_normal_2, half_dir);
    const float n3_dot_h = dot(lobe_normal_3, half_dir);
    const float lobe_1 = pow(max(sqrt(max(0.0, 1.0 - n1_dot_h * n1_dot_h)), 1.0e-4), 200.0) * edge_fade;  // `_2341`
    const float lobe_2 = pow(max(sqrt(max(0.0, 1.0 - n2_dot_h * n2_dot_h)), 1.0e-4),
                             float(int(200.0 * max(1.0 - game_hair_anisotropy_range2, 0.0)))) * edge_fade;   // `_2363`+1073
    const float lobe_broad = pow(max(sqrt(max(0.0, 1.0 - n3_dot_h * n3_dot_h)), 1.0e-4),
                                 float(int(200.0 * max(1.0 - game_hair_line_range, 0.0))));                     // `_2403`
    // `_2346 = max3(_2341)`: all three components of the primary lobe are the same scalar, so its max is lobe_1.
    const float primary_lobe = lobe_1;                                                            // `_2346`

    // ---- THE LineMap / StrokeMap WEIGHTING (HLSL 1068-1072) ----
    //
    // HLSL 1072's `lerp(ceil(clamp(frac(uv.x * _LineAmount) - 0.5, 0, 1)), 1 - _LineMap.x, _UseLineMap)` is
    // the line pattern, and `_535 = (stroke.x * 2 - 1) * _StrokeScale` (HLSL 474) is the stroke offset already
    // applied to the lobe normals above.
    // MISSING both maps (see the header): the pattern falls back to the program's OWN no-map statement
    // (`_UseLineMap = 0` leaves the UV stripe pattern) and the stroke offset to its neutral.
    const float line_pattern = ceil(clamp(frac(uv.x * game_hair_line_amount) - 0.5, 0.0, 1.0));   // HLSL 1072
    const float line_band = lerp(1.0 - game_hair_line_intensity, 1.0, line_pattern);              // `_51_m47` arm
    // `_2417 = lerp(1.0, lerp(1.0, _line_band-with-primary, _broad), _468)`: the pattern darkens the lit
    // colour, the primary lobe removes that darkening where the sharp lobe is, the broad lobe (the LineRange
    // term) does the same more widely, and the P map's reflectivity is the outer weight.
    const float line_band_with_primary = lerp(line_band, 1.0, primary_lobe);                      // `_2346` arm
    const float line_weight = lerp(1.0, lerp(1.0, line_band_with_primary, lobe_broad), reflectivity);  // `_2417`

    // ---- THE SUMMED HIGHLIGHT (HLSL 1073; provenance header "1391") ----
    //
    // `_2421 = ((((_2341 * _2105) * _51_m36) * 5.0) * _1975) + lerp((((..._2363...)* _609) *
    // (_51_m42.xyz * _466.w)) * _1975, 0, _2346)`, all multiplied by `_2313` (the light colour times the
    // shadow/AO shaping) and `_20_m92.w`. THE SUM IS OF TWO LOBES FOR THE MAIN LIGHT: the sharp primary tinted
    // by F0, and the broader second tinted by `_AnisotropyColor2` x smoothness and SUPPRESSED where the
    // primary is strong (`lerp(secondary, 0, max3(_2341))`). The game's per-light "sum" at HLSL 1412 repeats
    // this same lobe inside the mesh-light loop; deren contributes one sun here (see the notes).
    const float3 primary_highlight = lobe_1 * f0 * game_hair_anisotropy_intensity * 5.0 * game_hair_strand_gain;
    const float3 secondary_raw = lobe_2 * game_hair_anisotropy_colour2 * smoothness * game_hair_strand_gain;
    const float3 secondary_highlight = lerp(secondary_raw, float3(0.0, 0.0, 0.0), primary_lobe);
    // `_2313 = _2285 * ((_2307 * 0.5 + 0.5) * lerp(_20_m79.z, 1, _2307))`; `_2307 = lerp(_2255, _2254, _2183)`
    // is the ramp-AO-shadow weight pulled to the shadow term, and `_20_m79.z` has no deren lane, so the second
    // factor is the neutral 1.
    const float ao_shadow_mix = lerp(ramp_ao_shadow, ramp_layer_selector, shadow_scene);
    const float3 spec_light = main_light_final * (ao_shadow_mix * 0.5 + 0.5);
    const float3 highlight = (primary_highlight + secondary_highlight) * spec_light * game_hair_pass_specular_scale;

    // ---- THE COMPOSITION (HLSL 1074-1080) ----
    //
    // `_2425 = _2285 * _2301 * _2417` - light colour, the diffuse layer composite and the line weight - and
    // `_2429` then moves its saturation by `lerp(_LineSaturation, 1, _2417)`.
    const float3 diffuse_lit = main_light_final * diffuse_final * line_weight;                    // `_2425`
    const float3 diffused = lerp(float3(toon_luminance(diffuse_lit)), diffuse_lit,
                                 lerp(game_hair_line_saturation, 1.0, line_weight));              // `_2429`
    // `_2472 = (1 - alpha * _DiffuseBlendEffect) + alpha`, i.e. `1 - effect * (1 - alpha)`, and
    // `_2474 = _2429 * _2472 + _2421 * _1978 + (_2429 + _2421) * _1977`. THE TWO STRAND-BLOCK FACTORS ARE THE
    // BYPASS ARM'S VALUES: `_1978 = 1.0` and `_1977 = 0.0` (HLSL 963-971), which is the program's own "block
    // not taken" state - the block needs the game's extra vertex streams (MISSING, see the header).
    const float diffuse_specular_blend = 1.0 - rig.env.w * (1.0 - albedo_alpha);                 // `_2472`
    float3 result = diffused * diffuse_specular_blend + highlight;                                // `_2474`

    // HLSL 1081-1082: `_2475 = luma(_2474)`, `_2478 = clamp(_2475 - 0.5, 0, 0.5)`, and the colour is lerped
    // from its own luminance by `_2478^2 + 1` - a contrast lift around mid-grey, not a tone map.
    const float result_luma = toon_luminance(result);                                             // `_2475`
    const float luma_lift = clamp(result_luma - 0.5, 0.0, 0.5);                                   // `_2478`
    result = lerp(float3(result_luma), result, luma_lift * luma_lift + 1.0);                      // `_2621` first term

    // ---- THE ONE DEREN-SIDE ADDITION, AND THE ENVIRONMENT'S TWO UNPORTED TERMS ----
    //
    // The game hair's environment lives in the 3D light/occlusion VOLUMES (`_44`..`_49`, HLSL 507-627, and the
    // specular/SSS volume at HLSL 1452-1504): none of those has a deren equivalent, so they are NOT
    // approximated here. deren's sky DIFFUSE is added on the same terms as every other family - an explicit
    // bridge, not the game's formula. deren's sky SPECULAR is offered through the family's IBL switch
    // (`params.ibl_specular_strength`, 0 for hair), so it is present but off at the shipped numbers; the game's
    // own environment specular is the volume term that is missing.
    const float3 ambient_bridge = environment_irradiance(normal) * albedo * ao_mask * params.ambient_strength *
                                  game_hair_ambient_scale;
    const float env_lod = log2(max(0.01, 1.0 - smoothness)) * 1.2 + 5.0;
    const float3 env_spec_bridge = environment_specular_sample(reflect(-view_dir, normal), env_lod) * f0 *
                                   params.ibl_specular_strength;
    // deren's material-level emissive is added at the tail like every family; THIS HAIR VARIANT's keywords have
    // `_EMISSION` off, so the game program itself would add nothing - on the reference asset the slot is empty.
    return result + ambient_bridge + env_spec_bridge + s.emissive;
}
