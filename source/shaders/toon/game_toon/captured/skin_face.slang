// ============================ GAME_TOON VIEWER: SKIN + FACE ============================
//
// THE SOURCE IS THE CAPTURED GAME_TOON STUDIO VIEWER, not the decompiled game program. The captured fragment
// shader is ONE template with `FAMILY_SKIN / FAMILY_HAIR / FAMILY_CLOTH / FAMILY_EYE` branches
// (`tmp/game_toon-studio-audit/final/shaders/character.frag.readable.glsl`); this file is the SKIN family's
// arm plus the face-specific terms that the viewer compiles into it. The reference for every number below is
// that file, with the feature aliases of `.../shaders/aliases.json`.
//
// WHAT THIS FILE IS: `captured_skin_face_shade(...)` - the viewer's skin/face surface response, returned as
// LINEAR HDR radiance. Tone mapping and the sRGB encode are the post chain's (mode 2), so neither appears here
// (`character.frag.readable.glsl:1189-1190` includes them; this port deliberately omits both).
//
// THE VIEWER'S BRANCH STRUCTURE maps onto deren's runtime lanes like this - the `#ifdef`s are compile-time in
// the captured template and per-material here:
//
//   FAMILY_SKIN                -> this function is the `VR_FAMILY_SKIN` / `VR_FAMILY_FACE` viewer arm; the caller
//                                 dispatches it only when `game_toon_profile_enabled(push.material_index)`.
//   USE_DIFFUSE_RAMP (:787)    -> the material's `_UseDiffRampMap == 1` (sidecar flag), carried by
//                                 `surface_sample::toon_indices.x` (`_DiffRampMap`). Its absence has a
//                                 documented family fallback, not a second shading model.
//   USE_FACE_SDF (:791)        -> the viewer's per-material `face` flag; deren's equivalent input is the
//                                 `_SDFLightmap` texture lane (`toon_lanes_at(..).x`, `toon_slot::sdf_lightmap`),
//                                 exactly as `game_toon/face.slang:202` reads it. The laevat face states
//                                 `_UseSDFLightmap = 1`; the body states 0, i.e. the body takes the no-SDF arm.
//   USE_FACE_MASK (:794)       -> `_SDFMask` present, deren `toon_lanes_at(..).z` (`toon_slot::sdf_mask`), and
//                                 only meaningful together with USE_FACE_SDF (the captured define is nested in it).
//   USE_FACE_HIGHLIGHT (:849)  -> `_HighlightMap` present (the captured feature builder enables it whenever the
//                                 material has the map: `material-feature-defines.js`'s `o && r._HighlightMap`).
//                                 deren has no `_HighlightMap` toon slot, but the staged preview routes it through
//                                 the FACE material's otherwise-unused METALLIC/GLOSS lane (sidecar
//                                 `_MetallicGlossMap` row, written by `prepare_game_toon_preview.py
//                                 --face-highlight-metallic-lane`; `mapping-evidence.json
//                                 .face_highlight_uses_metallic_lane = true`). That lane is `rgba8_unorm`
//                                 (`runtime.constructor.cppm:2546`), so it is sampled RAW exactly like the
//                                 captured map; `game_toon_skin_face_highlight_lane` below reads it for the FACE
//                                 family only (body skin keeps the lane's real metallic/gloss meaning).
//   USE_SHADOW_LUT (:827)      -> `_ShadowLutTex` + `_UseShadowLutTex == 1`, deren `toon_indices.y`
//                                 (`toon_slot::shadow_lut`). The LUT is sampled through
//                                 `game_face_exported_lut_v` (the export PNG flip), exactly as the contract requires.
//   USE_SATURATION_LIFT (:1140)-> the viewer look option `saturationLift` (default ON in the captured JS
//                                 defaults object `fS`, `eadba1af4f-DIaZSo8d.js` around offset 368965) and the
//                                 `USE_DIFFUSE_RAMP` family set; applied at the end of the composition.
//
// WHAT IS NOT PORTED, and why (all detailed again in `tmp/game_toon-port/skin-face-notes.md`):
//   * `R114` (:188-327) - the viewer's PUNCTUAL/OTHER-lights loop (`Q260` lights, modes 0..4). It is ported in
//     its own header (`toon/game_toon/captured/game_toon_lights.h`, fed by deren's ordinary punctual-light UBO and
//     the captured Laevatain rig injected by the app), NOT in this file: the skin arm below is the
//     main/skin-light composition `:787-889`, and it hands `game_toon_lights.h` the captured `R114` call's
//     arguments (N, `R252`, `R333`). The block's other globals (`R45`, `R134`, `R151`, `R162`, `R268`) are the
//     punctual loop's own and are not recomputed here.
//   * `Q132` (:805-826) - the viewer's SELF-SHADOW arm. It is a LOOK TOGGLE (`setSelfShadow`), and the captured
//     default is OFF (`selfShadow:false` in the same defaults object). Its absence is what makes the captured
//     lit weights `R144 = min(base.a, ramp_alpha)` and `R204 = base.a` (:822-826), which is what this port
//     implements. Turning it on needs the viewer's view-matrix-row vector `R33` (:810-813), whose world
//     direction in deren's matrix convention is an ambiguity; the notes record the formula rather than guessing.
//     The `USE_SHADOW_LUT` LUT is NOT this toggle and IS ported.
//   * `Q185`/`Q164`/`R75` (:331-348, :690, :1177) - the viewer's depth fade / clip feature; its enable is a
//     viewer feature flag whose captured state is not recorded, and it contributes 0 in the template's
//     default. No deren lane exists.
//   * `R77` (:427-435) - the SDF's angle/normal-B terminator. It is DEAD in the captured fragment: the SKIN
//     branch calls `evaluateFaceSdfTerm` (:798), never `R77`; the port keeps only the one the branch calls.
//   * The `#include <tonemapping_fragment>` / `<colorspace_fragment>` (:1189-1190) - post owns both.
//
// AN IMPORTANT VIEWER-SPECIFIC DETAIL the reader would otherwise have to rediscover: THE SHADOW TINT IS
// ALIASED FOR THE SKIN FAMILY. `character.frag.readable.glsl:845` mixes `uShadowTint` (`Q62`), and
// `material-uniform-bindings.js` `T3` binds `Q62 = fam==="skin" ? uSkinShadowTint : Q62`. So on a skin material
// the captured `uShadowTint` IS the skin shadow tint, which is why this port reads profile lane 4
// (`game_toon_skin_shadow_tint`) and not lane 3 here.
//
// ONE SENTENCE A READER SHOULD LEAVE WITH: the shadow side's colour is the material's `_ShadowLutTex` lookup
// (srgb-decoded by the lane upload), the ramp selects how much of it is lit, the face's 2D SDF replaces the
// geometric `NdotL` where `_SDFMask.g` says so, and the light is the viewer's own two-part skin composition
// (`wf_skin_light`, pinned by `tools/game_shader_audit/test_game_toon_math.cpp`) - not deren's environment.

#include "toon/game_toon_math.h"
#include "toon/game_toon/face_frame.h"
#include "toon/game_toon/captured/encoding.h"
#include "toon/game_toon/game_toon_profile.h"
#include "toon/game_toon/captured/game_toon_lights.h"

// ---- CAPTURED RIG CONSTANTS --------------------------------------------------------------------------------
//
// The two light-composition constants are inside `wf_skin_light` (`toon/game_toon_math.h:29-34`):
// `VIEWER_AMBIENT = 0.725` and `VIEWER_LIGHT_SCALE = 0.2877` (`character.frag.readable.glsl:420-421`,
// `comparison-summary.json` `shader_constants.ri.ambient` / `.o3`). They are NOT re-declared here; the helper
// owns that expression so its numerical pin is the one the test asserts.
//
// Every constant in THIS block is a RIG value of the captured Laevatain configuration (viewer-lighting.js /
// `comparison-summary.json` `laevat_volume`), not a per-material row: the per-material payload arrives in the
// profile lanes and is read below (see "THE CAPTURED MATERIAL PAYLOAD"). A rig value answers only when the lane
// that should carry the equivalent is deren's "not stated" zero.

/// The captured skin light colour (`laevat_volume.skinLightColor`); lane 2 is this colour times the volume
/// multiplier (`viewer-lighting.js` `update()`: `Q87 = skinLightColor * intensity`). Used only for an absent lane.
static const float3 game_toon_skin_face_skin_light_colour = float3(1.0, 0.9262, 0.8667);
/// The captured SKIN shadow tint (`laevat_volume.skinShadowTint`); lane 4 carries the same value. Absent-lane only.
static const float3 game_toon_skin_face_skin_shadow_tint = float3(1.187, 0.9272, 0.813);
/// `laevat_volume.multiplier`; already folded into lane 2 by the staged packing, used only for the absent-lane
/// fallback so the fallback has the same scale as the lane.
static const float game_toon_skin_face_volume_multiplier = 1.05;
/// `laevat_volume.envMul` (`Q283`); lane 0.z carries the same number. Absent-lane only.
static const float game_toon_skin_face_environment_multiplier = 0.55;

/// `uMainLightIntensity` (`Q143`): `viewer-lighting.js` initialises `zf = 2` and `Q143:{value:zf}`; the
/// comparison records it as the captured "main light intensity 2.0". It multiplies ONLY the skin light inside
/// `R153`/`R208`, never deren's sun. Lane 0.y carries `Q143`; this is the absent-lane default (the rig's own
/// initialiser, not a material row).
static const float game_toon_skin_face_captured_main_light_intensity = 2.0;

/// `uShadowLutSize` (`M3`'s `Q316` = `_ShadowLutTex.image.height`): the `32^3`-cube-in-a-`1024x32`-strip size
/// (`comparison-summary.json` `face_textures[_ShadowLutTex].size = [1024, 32]`).
static const float game_toon_skin_face_shadow_lut_size = 32.0;

/// `ri.sdfBias` (`comparison-summary.json` `shader_constants.ri.sdfBias = 0.0`, readable GLSL `:436`).
static const float game_toon_skin_face_sdf_bias = 0.0;

// ---- THE CAPTURED MATERIAL PAYLOAD (profile lanes 5/6, plus deren's existing sdf_rim colour lane) ----------
//
// THE PACKING IS THE STAGED ONE, NOT A RESERVATION. `tmp/game_toon-reference/assets/uniform-packing.json`
// (`rows`, also written into the preview sidecar as the `_GameToon5..15` colour rows) is the authority; rows
// 0..4 are named by `game_toon_profile.h`. The two rows this family consumes are:
//
//   lane 5: x = `_ShadowColorBrightness`   y = `_ShadowColorSaturation`
//           z = `_SkinRimOffScale`         w = `_FaceRimOffScale`
//   lane 6: x = `_HighlightMapVector.x`    y = `_HighlightMapVector.y`
//           z = `_BumpScale`               w = `_SpecBumpScale`
//
// lane 7 is the ANISOTROPY payload (`Q120` values in `uniform-packing.json` row 7), not this family's; it is
// deliberately NOT read here.
//
// `_SDFRimColor` IS NOT A GAME_TOON LANE: it is deren's existing `toon_colour_lane::sdf_rim` (colour lane 0),
// the lane the game already carries it in (`uniform-packing.json` `existing_lanes.Q353`). The captured `T3`
// binding wraps that row in `Le(...)`, i.e. the viewer's `Na` sRGB->linear conversion, so the lane holds the
// AUTHORED value and this file applies the same curve once before use (`game_toon_encoding_srgb_to_linear`, the
// captured `It`: `c<=.04045 ? c/12.92 : ((c+.055)/1.055)^2.4`). This is also why the OLD code's
// `lane5.rgb` read produced the green cast: lane 5.rgb is `(0.55, 1.2, 0.7)` (brightness, saturation, skin
// rim scale), not a colour, and the skin arm multiplies it into the base at grazing angles.
//
// A ZERO WORD MEANS "NOT STATED" (`game_toon_profile.h`), and every accessor below answers the captured VIEWER's
// own nullish default (`M3`/`T3`'s `??` values) rather than a laevat material constant. The laevat rows are
// staged, so the lanes are what run on the captured asset; the defaults exist only so a model without a payload
// does not read a zero as art.
static float4 game_toon_skin_face_payload(const uint material_index, const uint lane) {
    return game_toon_profile_lane(material_index, lane);
}

/// @brief `_SDFRimColor` (`Q353`), deren colour lane 0 (`toon_colour_lane::sdf_rim`), linearized by the viewer's
///        `Na` (`T3`: `Le(...)`), exactly as the captured uniform is bound.
/// @note the captured viewer's `T3` default for an absent row is WHITE (`Le(n,"_SDFRimColor",[1,1,1])`), which
///       is also deren's neutral for this lane - so an absent row is the identity tint, not the laevat colour.
static float3 game_toon_skin_face_sdf_rim_colour(const uint material_index) {
    const float3 authored = toon_colour_at(heap_slots_toon_colours,
                                           material_index * character_toon_colour_lanes + 0u).rgb;
    return game_toon_encoding_srgb_to_linear(authored);
}

/// @brief `_SkinRimOffScale` (`Q291`): lane 5.z; absent-lane default 1.0 (`T3`: `_SkinRimOffScale ?? 1`)
static float game_toon_skin_face_skin_rim_off_scale(const uint material_index) {
    const float lane = game_toon_skin_face_payload(material_index, 5u).z;
    return lane > 0.0 ? lane : 1.0;
}

/// @brief `_FaceRimOffScale` (`Q301`): lane 5.w; absent-lane default 1.0 (`T3`: `_FaceRimOffScale ?? 1`)
static float game_toon_skin_face_face_rim_off_scale(const uint material_index) {
    const float lane = game_toon_skin_face_payload(material_index, 5u).w;
    return lane > 0.0 ? lane : 1.0;
}

/// @brief `_ShadowColorBrightness` (`Q347`, the USE_SHADOW_LUT-OFF arm only): lane 5.x; default 0.5 (`M3`)
static float game_toon_skin_face_shadow_brightness(const uint material_index) {
    const float lane = game_toon_skin_face_payload(material_index, 5u).x;
    return lane > 0.0 ? lane : 0.5;
}

/// @brief `_ShadowColorSaturation` (`Q348`, the USE_SHADOW_LUT-OFF arm only): lane 5.y; default 1.0 (`M3`)
static float game_toon_skin_face_shadow_saturation(const uint material_index) {
    const float lane = game_toon_skin_face_payload(material_index, 5u).y;
    return lane > 0.0 ? lane : 1.0;
}

/// @brief `_HighlightMapVector.x` (`Q162.x`, readable:850): lane 6.x; default 0.0 (`T3`: `?? [0,0]`)
static float game_toon_skin_face_highlight_vector_x(const uint material_index) {
    const float lane = game_toon_skin_face_payload(material_index, 6u).x;
    return lane != 0.0 ? lane : 0.0;
}

/// @brief `uMainLightIntensity` (`Q143`): lane 0.y; absent-lane default is the rig's own initialiser (2.0)
static float game_toon_skin_face_main_light_intensity(const uint material_index) {
    const float lane = game_toon_main_light_intensity(material_index);
    return lane > 0.0 ? lane : game_toon_skin_face_captured_main_light_intensity;
}

/// @brief `uRampRangeBias` (`Q215`): deren lane 1.a (`uniform-packing.json` row 1.a), the viewer's own rig row.
/// @note the captured Laevatain configuration is 0.0 (`laevat_volume.rangeBias`), which is also the lane word
///       when no rig states one - so the lane value is returned as-is and no fallback is invented.
static float game_toon_skin_face_ramp_range_bias(const uint material_index) {
    return game_toon_range_bias(material_index);
}

/// `_HighlightMap` texture lane. The captured feature is `o && r._HighlightMap` (`material-feature-defines.js`),
/// i.e. the map is read whenever the face SDF arm is on and the material has the texture. deren has no
/// `_HighlightMap` toon slot; the staged preview puts it in the FACE material's metallic/gloss lane (see the
/// header note). The lane is `rgba8_unorm`, so the sample is RAW, exactly as the captured non-sRGB texture read.
static uint game_toon_skin_face_highlight_lane(const surface_sample s, const uint4 toon_lanes) {
    return s.toon_family == VR_FAMILY_FACE ? toon_lanes.y : 0u;
}

// ---- SMALL HELPERS ----------------------------------------------------------------------------------------

/// @brief the captured viewer's packed-normal decode for skin (`character.frag.readable.glsl:712-721`)
/// @note `bump.r * bump.a` is the game's X, `bump.g` the Y, and Z is reconstructed (the body/face normal maps
///       store a constant blue - see `game_toon/face.slang:65-78`, whose decode this is the Slang mirror of).
static float3 game_toon_skin_face_unpack_normal(const float2 rg, const float scale) {
    float3 normal;
    normal.xy = rg * 2.0 - 1.0;
    normal.z = max(1.0000000168623835e-16, sqrt(1.0 - saturate(dot(normal.xy, normal.xy))));
    normal.xy *= scale;
    return normal;
}

/// @brief the captured viewer's `R218` (`character.frag.readable.glsl:422-424`), Rec.709 luminance
/// @note These weights are the viewer's and DIFFER from `R113`'s (:59) in the last three digits; both are kept
///       with their own values rather than unified, because each one is cited.
static float game_toon_skin_face_luma(const float3 colour) {
    return dot(colour, float3(0.212673, 0.715152, 0.072175));
}

/// @brief the captured viewer's `R113` (`character.frag.readable.glsl:58-61`), the saturation-preserving
///        desaturate used by the no-LUT fallback shadow colour
static float3 game_toon_skin_face_desaturate(const float3 colour, const float saturation) {
    const float luma = dot(colour, float3(0.2126, 0.7152, 0.0722));
    return max(lerp(float3(luma), colour, saturation), float3(0.0));
}

/// @brief the captured viewer's `sampleShadowLutLinear` (`character.frag.readable.glsl:410-419`) on deren's
///        export-aware `_ShadowLutTex` lane
///
/// The lookup is a `32^3` colour cube flattened into a `1024x32` strip of `32x32` tiles: the tile is the BLUE
/// channel, the in-tile X the RED, the in-tile V the GREEN, and the fractional blue blends the two adjacent
/// tiles. `game_face_exported_lut_v` (`face_frame.h`) is the archive-PNG storage bridge - the exporter saves
/// Texture2D PNGs with `ConvertToImage(flip: true)`, so the analytical V is flipped once here and nowhere else.
///
/// THE sRGB DECODE IS THE LANE UPLOAD, NOT A LINE HERE: the viewer samples its LUT as a non-sRGB texture and
/// then calls `srgbToLinear` (:418); deren uploads `toon_slot::shadow_lut` as `rgba8_srgb`
/// (`source/engine/runtime/runtime.constructor.cppm:2538`), so the sampler performs exactly that decode and a
/// manual one would double-decode. The COORDINATE is display-space on both sides: the captured `linearToSrgb`
/// (`:403-406`, reproduced by `game_toon_encoding_linear_to_srgb`) encodes the clamped albedo; the old
/// `toon_linear_to_srgb` is the GAME's `linear2sRGB` with an `abs` and slightly different rounded constants.
static float3 game_toon_skin_face_sample_shadow_lut(const float3 albedo, const uint lane) {
    const float n = game_toon_skin_face_shadow_lut_size;
    const float3 cube = game_toon_encoding_linear_to_srgb(albedo); // :412 (`linearToSrgb`, clamped)
    const float tile_f = cube.z * (n - 1.0); // :412
    const float tile0 = floor(tile_f); // :413
    const float tile1 = min(tile0 + 1.0, n - 1.0); // :414
    const float v = game_face_exported_lut_v(cube.y); // :415, through the export bridge
    const float in_tile_x = cube.x * (n - 1.0); // :412
    const float u0 = (tile0 * n + in_tile_x + 0.5) / (n * n); // :416
    const float u1 = (tile1 * n + in_tile_x + 0.5) / (n * n); // :417
    const float3 low = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + lane),
                                  heap_sampler_texture, float2(u0, v)).rgb;
    const float3 high = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + lane),
                                   heap_sampler_texture, float2(u1, v)).rgb;
    // The viewer samples the LUT as a NON-sRGB texture, interpolates the RAW bytes, and only then calls
    // srgbToLinear (:418). deren's lane is rgba8_srgb, so undo the sampler decode on each tap, mix the stored
    // values, then decode the mix - exactly `game_toon_encoding_shadow_lut_mix` (mix raw, then `srgbToLinear`).
    return game_toon_encoding_shadow_lut_mix(game_toon_encoding_linear_to_srgb(low),
                                            game_toon_encoding_linear_to_srgb(high), tile_f - tile0);
}

/// @brief the captured viewer's `evaluateFaceSdfTerm` (`character.frag.readable.glsl:436-449`)
///
/// THE 2D FACE TERMINATOR. The light direction is projected into the face's own plane and turned into an angle
/// against the face's forward axis (`f`) and right axis (`s`); the SDF atlas is read MIRRORED when the light is
/// on the `s >= 0` side (`:442`), its R/G average is stretched over the window `[max(2e-1,0), min(2e,1)]` where
/// `e = 0.5 - 0.5t` (`:446-447`), and the signed result is `2k-1 + max(t,0)` (`:448`). It never reads the
/// surface normal - that is the whole point of a face SDF on a low-poly head.
///
/// THE AXES ARE THE VIEWER'S PER-FACE UNIFORMS: `Q324` is the face's forward direction and `Q80` its right
/// direction, both transformed by the head's world matrix every frame (`eadba1af4f-DIaZSo8d.js`'s
/// `update()`: `Q324 = forward.transformDirection(head.matrixWorld)`,
/// `Q80 = right.transformDirection(head.matrixWorld)`). deren publishes the same semantic frame in
/// `head_frame_at(...)` (`character_forward.slang:209-258`); the caller passes `.front` and the game object's
/// `+X` derived from `.right` through `game_face_object_x` (`face_frame.h:8-10`). This is the same bridge
/// `game_toon/face.slang:199-233` uses for the game's object-space SDF mirror.
///
/// THE 2D MAP GIVES NO NOSE/CHEEK GEOMETRY: the terminator is painted into the atlas and shaped only by this
/// angle/window, exactly as the captured viewer does. That limitation is the viewer's, and is kept.
///
/// @param light_dir the unit SURFACE-TO-LIGHT direction (the viewer's `L`, :728)
/// @param uv the surface uv (the SDF atlas's own coordinate)
/// @param lane the material's `_SDFLightmap` heap lane; the caller must not pass 0
/// @param face_forward the head's world forward axis (viewer `Q324`)
/// @param face_right the game object's world `+X` axis (viewer `Q80`; `-head_right` in deren's frame)
static float game_toon_skin_face_evaluate_sdf_term(const float3 light_dir, const float2 uv, const uint lane,
                                             const float3 face_forward, const float3 face_right) {
    const float f = dot(light_dir, face_forward); // :438
    const float s = dot(light_dir, face_right); // :439
    const float2 l = float2(s, f) / max(length(float2(s, f)), 1.0e-4); // :440
    const float t = clamp(l.y + game_toon_skin_face_sdf_bias, -1.0, 1.0); // :441, bias 0.0 at :436
    const float2 sdf_uv = float2(s < 0.0 ? uv.x : 1.0 - uv.x, uv.y); // :442
    const float4 sdf = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + lane),
                                  heap_sampler_texture, sdf_uv); // :443
    const float average = 0.5 * (sdf.r + sdf.g); // :445
    const float e = clamp(0.5 - 0.5 * t, 0.001, 0.999); // :446
    const float k = smoothstep(max(2.0 * e - 1.0, 0.0), min(2.0 * e, 1.0), average); // :447
    return 2.0 * k - 1.0 + max(t, 0.0); // :448
}

/// @brief `_SkinLightColourScaled` (`Q87`) from profile lane 2, with the captured rig value as the not-stated
///        fallback
///
/// The viewer's `Q87 = skinLightColor * intensity` (`viewer-lighting.js` `update()`:
/// `Q87.value.copy(this.skinLightColor).multiplyScalar(this.intensity)`), and the captured intensity is
/// `laevat_volume.multiplier = 1.05`. Lane 2 (uniform-packing.json row 2) already carries `Q87`, i.e. the
/// colour ALREADY multiplied by 1.05, so the lane is used as-is; the fallback multiplies the captured colour
/// by the same volume multiplier so both paths have the same scale.
static float3 game_toon_skin_face_light_scaled(const uint material_index) {
    const float4 lane = game_toon_skin_light(material_index);
    return dot(lane.rgb, lane.rgb) > 0.0
               ? lane.rgb
               : game_toon_skin_face_skin_light_colour * game_toon_skin_face_volume_multiplier;
}

/// @brief the skin family's shadow tint from profile lane 4, with the captured laevat value as the fallback
/// @note WHY LANE 4 AND NOT LANE 3: `material-uniform-bindings.js` `T3` binds the captured `Q62`
///       (`uShadowTint`) to `uSkinShadowTint` for `fam === "skin"`, so the value `R208` mixes with is the SKIN
///       shadow tint ([1.187, 0.9272, 0.813]), which the hero of `game_toon_profile.h` spells out separately.
static float3 game_toon_skin_face_shadow_tint_value(const uint material_index) {
    const float3 lane = game_toon_skin_shadow_tint(material_index);
    return dot(lane, lane) > 0.0 ? lane : game_toon_skin_face_skin_shadow_tint;
}

// ============================ THE SKIN / FACE SHADE ============================
//
/// @brief the captured viewer's FAMILY_SKIN skin/face response (`character.frag.readable.glsl:787-889` + `:1140-1145`)
///
/// @param s the gathered surface (albedo, alpha, normal, flags, toon family, toon texture lanes)
/// @param world_pos interpolated world position (normal-map tangent frame)
/// @param geo_normal interpolated geometric normal
/// @param uv the material's uv (also the SDF/mask/highlight/ramp coordinate)
/// @param is_front the rasterizer's front/back decision (the double-sided normal flip, :694-698)
/// @return LINEAR HDR radiance - no tone map, no sRGB encode; the caller owns alpha and dispatch
///
/// THE INPUTS WHOSE CAPTURED COUNTERPART IS deren's RATHER THAN THE VIEWER'S, called out once instead of at
/// every use:
///  * `L` (`uMainLightDirection`) is CAMERA-RELATIVE in the viewer (`viewer-lighting.js`'s `Bf(...)`,
///    yawBias 10 deg, minPitch 32 deg, both in `comparison-summary.json` `laevat_volume`). The profile-on path
///    reproduces that mapping through `wf_camera_light(-camera_forward, game_toon_yaw_bias, game_toon_min_pitch)`
///    (`toon/game_toon_math.h:35-41`, numerically pinned by `test_game_toon_math.cpp:15-20`); the profile-off path
///    never reaches this function and keeps deren's sun. The two angles themselves are profile lanes 3.a / 2.a.
///  * `V` is `normalize(cameraPosition - worldPosition)` on both sides (:729).
///  * The face frame (`Q324`/`Q80`) comes from `head_frame_at(...)`: `front`/`right` carry the captured viewer's
///    measured face axes when the host published them (`front.w == 1.0`, `deren::gltf::face_frame`), and the
///    bone/fallback frame with the historical `-head_right` derivation otherwise. See
///    `game_toon_skin_face_evaluate_sdf_term` and the step-4 note.
///  * `Q91` (the head centre, :859) is read only by the viewer's punctual/rim loop (`R114`, via `R45`), which is
///    ported in `game_toon_lights.h`; this file does not fetch it.
float3 captured_skin_face_shade(surface_sample s, float3 world_pos, float3 geo_normal, float2 uv, bool is_front) {
    const Material mat = material_at(heap_slots_materials, push.material_index);
    const uint4 toon_lanes = toon_lanes_at(heap_slots_toon_lanes, push.material_index);
    const toon_params family_params = toon_params_for(s.toon_family);

    // ---- 1. NORMAL: the viewer's packed decode (`:712-721`), deren's derivative frame, the documented
    //         Unity/glTF Y bridge, then the double-sided flip (`:694-698`). The body states `_UseBumpMap = 1`
    //         (so `mat.flags` bit0 is set) and its normal map stores the game's X = R*A, Y = G packing; the
    //         face states `_UseBumpMap = 0` and keeps the geometric normal. ----
    float3 normal = normalize(geo_normal);
    if ((mat.flags & 1u) != 0u) {
        float3 sdir;
        float3 tdir;
        if (surface_tbn(world_pos, uv, sdir, tdir)) {
            tdir = -tdir; // existing Unity/glTF tangent-space Y bridge, as in game_toon/skin.slang:29
            const float4 texel = frame_texture_sample(mat.tex_indices.z, uv);
            const float3 n_ts = game_toon_skin_face_unpack_normal(float2(texel.r * texel.a, texel.g), mat.normal_scale);
            normal = normalize(mul(n_ts, float3x3(sdir, tdir, normal)));
        }
    }
    if ((mat.flags & 8u) != 0u && !is_front) {
        normal = -normal;
    }

    // ---- 2. L, V and the material's own white ----
    const float3 camera_forward_axis = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz);
    const float3 light_dir = game_toon_profile_enabled(push.material_index)
        ? normalize(wf_camera_light(-camera_forward_axis, game_toon_yaw_bias(push.material_index), game_toon_min_pitch(push.material_index)))
        : normalize(light_at(heap_light_slot).light_dir.xyz); // deren sun fallback
    const float3 camera_pos = camera_at(heap_camera_slot).camera_pos.xyz;
    const float3 view_dir = normalize(camera_pos - world_pos);
    const float no_v = saturate(dot(normal, view_dir)); // `R46` (`:732`: clamp(dot(N,V),0,1))
    const float3 base_rgb = s.albedo; // `R38`: uBaseColour * uBaseMap (`:62-70`), already linear
    const float base_alpha = saturate(s.alpha);

    // ---- 3. THE VIEWER'S PROFILE VALUES (lanes 0/2/4; lane 1.a is the ramp bias; lane 1.rgb is not read) ----
    const float3 skin_light_scaled = game_toon_skin_face_light_scaled(push.material_index); // `Q87` (`:844`)
    const float3 shadow_tint = game_toon_skin_face_shadow_tint_value(push.material_index); // `Q62` aliased to skin tint
    const float env_lane = game_toon_environment_multiplier(push.material_index);
    const float environment_multiplier =
        env_lane > 0.0 ? env_lane : game_toon_skin_face_environment_multiplier; // `Q283` (`:835`, `:851`)
    // Lane 1.rgb (`Q10` `uMainLightColourScaled`) is deliberately NOT read: the captured skin branch's light
    // uses `uSkinLightColourScaled` (`Q87`, lane 2) only; `Q10` appears nowhere in `:787-889`.

    // ---- 4. THE HEAD FRAME AND THE FACE AXES ----
    // THE CAPTURED VIEWER'S `faceFrame()` AXES, when the host measured them. `head_ubo::front` carries `Q324`
    // (the face mesh's averaged bind normal) and `head_ubo::right` carries `Q80` (the UV-side-band left/right,
    // orthogonalized) when `front.w == 1.0`; that is the captured frame, and the host's `deren::gltf::face_frame`
    // documents the measurement (`tmp/game_toon-studio-audit/final/responses/eadba1af4f-DIaZSo8d.js`, `faceFrame`).
    //
    // THE FALLBACK IS THE HISTORICAL DERIVATION AND IS KEPT FOR EVERY OTHER MODEL: `front` is then the head
    // bone's (or glTF's) forward and `right` its anatomical right, and `Q80` is the GAME OBJECT's local +X, which
    // `face_frame.h:8-10` says opposes deren's anatomical head.right - hence the reflection.
    // WHY `Q80` MUST BE THE CAPTURED ONE RATHER THAN A MIRROR OF THE BONE: the SDF atlas is MIRRORED about `Q80`
    // (`character.frag.readable.glsl:442`), so the light's `s = dot(L, Q80)` sign decides which half is read. A
    // bone-derived axis puts that threshold at the wrong camera yaw and the painted shadow pops when it crosses.
    const bool has_captured_face_frame = head_frame_at(heap_slots_scene_head + heap_frame_slot).front.w > 0.5;
    const float3 head_front = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).front.xyz);
    const float3 head_right = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).right.xyz);
    const float3 face_right = has_captured_face_frame
                                  ? head_right
                                  : float3(game_face_object_x(head_right.x), game_face_object_x(head_right.y),
                                           game_face_object_x(head_right.z));

    // ---- 5. THE FACE TERMS: USE_FACE_SDF / USE_FACE_MASK (`:791-801`) ----
    // deren's equivalent of the captured `face` define is the presence of the `_SDFLightmap` lane; the mask is
    // only read inside that arm, exactly as the captured `#ifdef USE_FACE_MASK` is nested in USE_FACE_SDF.
    const uint sdf_lane = toon_lanes.x;
    const bool use_face_sdf = sdf_lane != 0u;
    // `mask = vec4(1.0)` is the captured default when USE_FACE_MASK is off (`:792`): full SDF weight, no rim
    // mask attenuation, face rim scale selected. Kept bit-for-bit as the neutral.
    float4 face_mask = float4(1.0, 1.0, 1.0, 1.0);
    if (use_face_sdf && toon_lanes.z != 0u) {
        face_mask = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + toon_lanes.z),
                               heap_sampler_texture, uv); // `:795` sampled at the ORIGINAL uv
    }

    // ---- 6. THE RAMP COORDINATE (`:788-803`) ----
    // The captured `x` is the geometric half-Lambert WITH `uRampRangeBias`; where the mask says "use the painted
    // shape", the 2D SDF term replaces it. `rim_off_scale` selects the face's or the skin's rim-off scale by
    // mask.b and is attenuated by mask.r (`:799`). The two rim-off scales live in profile lane 5 and the SDF rim
    // colour in deren's `sdf_rim` colour lane; the ramp bias in lane 1.a (see the payload reader above).
    float ramp_x = clamp(dot(normal, light_dir) + game_toon_skin_face_ramp_range_bias(push.material_index), -1.0, 1.0); // `:788`
    float rim_off_scale = game_toon_skin_face_skin_rim_off_scale(push.material_index); // `:789` `R140 = uSkinRimOffScale`
    if (use_face_sdf) {
        const float sdf_term = game_toon_skin_face_evaluate_sdf_term(light_dir, uv, sdf_lane, head_front, face_right);
        ramp_x = lerp(sdf_term, ramp_x, face_mask.g); // `:798`
        rim_off_scale = lerp(game_toon_skin_face_face_rim_off_scale(push.material_index), game_toon_skin_face_skin_rim_off_scale(push.material_index),
                             face_mask.b) * face_mask.r; // `:799`
    }
    const float rim_off = clamp(0.85 * (1.0 - no_v) * rim_off_scale, 0.0, 1.0); // `:802` `R178`

    // `R32 = texture2D(uDiffuseRampMap, vec2(clamp(x*0.5+0.5, 0.002, 0.998), 0.5))` (`:803`). The captured
    // ramp is a 256x1 strip (`comparison-summary.json` `face_textures[_DiffRampMap].size = [256, 1]`), so the
    // captured V of 0.5 is kept rather than deren's banded-atlas `character_ramp_band_v`.
    float3 ramp_colour = float3(1.0, 1.0, 1.0);
    float ramp_alpha = 1.0;
    if (s.toon_indices.x != 0u) {
        const float4 ramp = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.x),
                                       heap_sampler_texture,
                                       float2(clamp(ramp_x * 0.5 + 0.5, 0.002, 0.998), 0.5));
        ramp_colour = game_toon_encoding_linear_to_srgb(ramp.rgb);
        ramp_alpha = ramp.a;
    } else {
        // `USE_DIFFUSE_RAMP` OFF. No laevat skin material takes this arm (`_UseDiffRampMap = 1` on both), so
        // this is the documented neutral behaviour for a material without the lane: the family's own Hermite
        // step (the same fallback the rest of this renderer uses when a ramp lane is absent), no tint.
        const float ramp_u = clamp(ramp_x * 0.5 + 0.5, 0.0, 1.0);
        ramp_alpha = smoothstep(family_params.center - family_params.softness,
                                family_params.center + family_params.softness, ramp_u);
    }

    // ---- 7. THE LIT WEIGHTS (`:805-826`) ----
    // The captured viewer's Q132 SELF-SHADOW arm is a look toggle whose default is OFF (see the header), so the
    // else-arm (`:822-826`) is the captured appearance this port implements:
    //     `R144 = min(base.a, R32.a)` and `R204 = base.a`.
    // With Q132 ON the viewer would min a shadow sample into R144 and apply a mask-shaped shadow ring in R204;
    // that formula and its R33 view-term are recorded in the notes as a documented non-port, not guessed here.
    const float lit_weight = min(base_alpha, ramp_alpha); // `R144`
    const float lut_weight = base_alpha; // `R204`

    // ---- 8. THE SHADOW SIDE'S COLOUR: USE_SHADOW_LUT (`:827-833`) ----
    float3 shadow_albedo = float3(1.0, 1.0, 1.0);
    if (s.toon_indices.y != 0u) {
        shadow_albedo = game_toon_skin_face_sample_shadow_lut(base_rgb, s.toon_indices.y); // `:828`, srgb lane decode
    } else {
        shadow_albedo = game_toon_skin_face_desaturate(base_rgb, game_toon_skin_face_shadow_saturation(push.material_index)) *
                        game_toon_skin_face_shadow_brightness(push.material_index); // `:831`
    }

    // ---- 9. THE ORIGINAL COMPOSITION (`:834-847`) ----
    // The environment multiplier scales the LUT side (`:835`); the SDF rim colour tints the base side (`:836`),
    // whose weight is the rim-off factor of step 6. The dark-in-dark lift, the LUT/base blend, the ramp's
    // saturation-as-tint and the bounded luma restore are the captured lines, verbatim.
    const float3 lut_scaled = shadow_albedo * 0.96 * environment_multiplier; // `:835` (`R281`)
    const float3 rim_tinted =
        base_rgb * lerp(float3(1.0), game_toon_skin_face_sdf_rim_colour(push.material_index), rim_off) * 0.96; // `:836` (`R334`)
    const float dark = game_toon_skin_face_luma(0.65 * lut_scaled); // `:838` (`R22`)
    const float3 dark_side = float3(dark) + 1.2 * (0.65 * lut_scaled - float3(dark)); // `:839`
    const float3 composed = lerp(dark_side, lut_scaled, saturate(lut_weight + ramp_alpha)); // `:839` (`R130`)
    const float3 composed_with_rim = lerp(composed, rim_tinted, lit_weight); // `:840`
    const float ramp_spread = max(max(ramp_colour.r, ramp_colour.g), ramp_colour.b) -
                              min(min(ramp_colour.r, ramp_colour.g), ramp_colour.b); // `:841` (`R150`)
    const float3 tinted = composed_with_rim * lerp(float3(1.0), ramp_colour, ramp_spread); // `:842`
    const float luma_gain =
        clamp(game_toon_skin_face_luma(composed_with_rim) / max(game_toon_skin_face_luma(tinted), 0.001), 0.0, 1.5); // `:843`
    float3 colour = tinted * luma_gain;

    // ---- 10. THE VIEWER'S SKIN LIGHT (`:844-847`) ----
    // `R153 = uSkinLightColourScaled * uMainLightIntensity`; `R208 = mix(uShadowTint, 1, R144) * 0.725 * 0.2877 *
    // uSkinLightColourScaled + mix(vec3(luma(R153)), R153, R144)`. That exact expression, including its two
    // constants, is `wf_skin_light` in `toon/game_toon_math.h:29-34` - numerically pinned against an independent
    // expansion in `tools/game_shader_audit/test_game_toon_math.cpp:4-14`, so the captured composition has ONE
    // implementation, shared by the shader and the test.
    const float3 light = wf_skin_light(skin_light_scaled, shadow_tint, lit_weight, game_toon_skin_face_main_light_intensity(push.material_index));
    // `R252 = color` (`:846`): the PRE-LIGHT composition, which the punctual loop later receives as its
    // `R114` `R283` (the wrap/diffuse colour of mode 0).
    const float3 pre_light_colour = colour;
    colour *= light;

    // ---- 11. USE_FACE_HIGHLIGHT (`:849-853`) ----
    // `vec2 R278 = vUv + vec2(-dot(V, Q80), 0) * uHighlightMapVector.x;`
    // `color += texture2D(uFaceHighlightMap, R278).rgb * R208 * mix(envMul, 1, R144) * (0.5 + 0.5*R144)`.
    // The map is the viewer's per-face painted catchlight, staged in the face's metallic/gloss lane (see
    // `game_toon_skin_face_highlight_lane` above); the lane is UNORM, so the sample is the captured RAW map.
    const uint highlight_lane = game_toon_skin_face_highlight_lane(s, toon_lanes);
    if (highlight_lane != 0u) {
        const float highlight_shift = -dot(view_dir, face_right) * game_toon_skin_face_highlight_vector_x(push.material_index);
        const float2 highlight_uv = float2(uv.x + highlight_shift, uv.y);
        const float3 highlight =
            heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + highlight_lane),
                       heap_sampler_texture, highlight_uv).rgb;
        colour += highlight * light * lerp(environment_multiplier, 1.0, lit_weight) *
                  (0.5 + 0.5 * lit_weight);
    }

    // ---- 12. THE SKIN ARM'S OWN DIRECT SPECULAR (`:854-889`) ----
    //
    // A TERM OF THE CAPTURED SKIN ARM, NOT OF THE PUNCTUAL LOOP, and the first version of this port omitted it
    // entirely: after the highlight the captured arm builds `R134` (the face mask, or `(1,1,1,0)` for body),
    // `Q153` (`x3`: `_Metallic`, `_Specular`, 1, `_Smoothness`), `R333 = mix(vec3(0.04*R187), base.rgb,
    // Q153.r)` and `R332 = max((1-Q153.a)^2, 0.007812)`, then adds the direct Blinn-Phong lobe scaled by
    // `R208 * (0.5+0.5*R144) * mix(envMul,1,R144)`. `R45`/`R268`/`R151`/`R162` in the same block feed only the
    // punctual loop's rim/spec attenuation and are NOT recomputed here.
    //
    // Q153 FOR THE CAPTURED SKIN MATERIALS: both laevat skin materials state NO `_UseMetallicGlossMap`, so the
    // captured `x3` arm is `vec4(It(_Metallic), _Specular, 1, _Smoothness)`. deren recovers `_Smoothness` as
    // `1 - roughness` because the preview mapping sets `roughnessFactor = 1 - _Smoothness`
    // (`tools/game_shader_audit/prepare_game_toon_preview.py:204`), and `_Specular` from its existing
    // `toon_colour_lane::specular_strength` (lane 4.x; `.x < 0` = the asset states nothing, so the family table
    // answers). The face's metallic/gloss LANE carries `_HighlightMap` in the staged preview and therefore must
    // NOT be read as `_UseMetallicGlossMap = 1` here.
    const uint colour_base = push.material_index * character_toon_colour_lanes;
    const float specular_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 4u).x;
    const float specular_strength = specular_lane >= 0.0 ? specular_lane : family_params.spec_strength;
    const float specular_metallic = game_toon_encoding_srgb_to_linear_channel(saturate(s.metallic)); // `Q153.r = It(_Metallic)`
    const float smoothness = saturate(1.0 - s.roughness); // `Q153.a = _Smoothness` (preview: roughness = 1 - _Smoothness)
    float spec_187 = specular_strength; // `R187 = Q153.g`
    if (use_face_sdf) {
        spec_187 *= face_mask.g; // `:874`
    }
    const float3 spec_f0 = lerp(float3(0.04 * spec_187), base_rgb, specular_metallic); // `:877` `R333`
    const float one_minus_smoothness = 1.0 - smoothness; // `:878` `R228`
    const float spec_alpha = max(one_minus_smoothness * one_minus_smoothness, 0.007812); // `:879` `R332`
    const float3 camera_row = camera_forward_axis; // `:881` `R127` = the view matrix's third column
    const float3 spec_half = normalize(light_dir + 2.0 * normalize(float3(camera_row.x, light_dir.y, camera_row.z)) +
                                       3.0 * view_dir); // `:882` `R106`
    const float spec_no_h = dot(normal, spec_half); // `:883` `R185`
    const float spec_alpha2 = spec_alpha * spec_alpha; // `:884` `R155`
    float spec_dd = spec_no_h * spec_no_h * (spec_alpha2 - 1.0) + 1.0; // `:885` `R245`
    spec_dd *= spec_dd; // `:886`
    const float spec_d = spec_dd != spec_alpha2 ? spec_alpha2 / spec_dd : 1.0; // `:887` `R55`
    const float spec_visibility = clamp(spec_d * 0.5 / (2.0 * no_v + spec_alpha + 0.0001) - 0.000061, 0.0, 20.0); // `:888` `R44`
    colour += light * (0.5 + 0.5 * lit_weight) * lerp(environment_multiplier, 1.0, lit_weight) *
              spec_visibility * spec_f0; // `:889`

    // ---- 13. USE_SATURATION_LIFT (`:1140-1145`) ----
    // The viewer's mild bright-region saturation expansion, applied after the skin composition (the captured
    // order: the block sits after the skin arm and before the cloth-only additions). Its enable is the look
    // option `saturationLift`, which is ON in the captured defaults. The luma weights are `R218`'s.
    const float lifted_luma = game_toon_skin_face_luma(colour);
    const float lift = clamp(lifted_luma - 0.5, 0.0, 0.5);
    colour = float3(lifted_luma) + (colour - float3(lifted_luma)) * (lift * lift + 1.0);

    // ---- 14. RETURN ----
    // The captured skin arm has no emission term (`_UseEmission = 0` on both laevat skin materials, and the
    // viewer's `_EmissionMap` addition is cloth-only, `:1127-1130`); deren's surface sample carries the
    // material's own emissive, which is 0 here and is added for stage consistency rather than invented.
    //
    // THE PUNCTUAL LOOP'S ARGUMENTS ARE THE CAPTURED `R114` CALL'S (`:1156`): N is the shading normal, `R283`
    // is `R252` (the PRE-LIGHT composition saved above, not the albedo), and the specular F0 is `R333` - the
    // same per-fragment colour step 12 computed. `R114` also receives `R40 = base.rgb*0.96` for its rim-mode
    // luma; deren's `game_toon_add_lights` has one `diffuse` parameter, so `pre_light_colour` serves both (the
    // captured punctual rig does contain rim lights, and this one-lane limitation is recorded in the notes).
    return game_toon_add_lights(colour, normal, view_dir, world_pos, pre_light_colour, spec_f0, s.roughness) + s.emissive;
}
