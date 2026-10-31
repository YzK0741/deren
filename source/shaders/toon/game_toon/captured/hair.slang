// ============================ GAME_TOON VIEWER: FAMILY_HAIR ============================
//
// The captured GameToon Studio character shader's HAIR branch, translated for deren. This is the PRIMARY
// appearance path for `VR_FAMILY_HAIR` when `game_toon_profile_enabled(push.material_index)` is true; the original
// game port in `toon/game_toon/hair.slang` stays as the fallback and is NOT touched by this file.
//
// SOURCE OF TRUTH (read-only capture, `tmp/game_toon-studio-audit/final/`):
//   * shaders/character.frag.readable.glsl - the aliased derivative of the captured template. All citations
//     below are `readable:NNN` = a line of THAT file (the contract says to use it).
//   * shaders/aliases.json - the Q-number <-> viewer-style uniform aliases.
//   * material-feature-defines.js - which conditionals the captured hair material selects.
//   * material-uniform-bindings.js - the exact uniform wiring: `k3` = the hair payload block
//     (Q120/Q146/Q212/Q44/Q113/Q72/Q152/Q116), `M3` = Q347/Q348 (shadow brightness/saturation), `R3` = the
//     split-normal block (`Q151`/`Q251`/`Q302`), `x3` = Q153.
//   * materials/M_actor_laevat_hair_01.json - the captured per-material values.
//   * comparison-summary.json - the captured Laevatain rig (`laevat_volume`, `shader_constants`).
//   * viewer-lighting.js - the viewer rig's uniform writes (`Q10`, `Q62`, `Q87`, `Q215`, `Q283`, `Q338`, `Q143`).
//
// WHAT IS PORTED, AND WHERE IT LIVES IN THE CAPTURE:
//   * the hair payload uniforms `readable:107-140` (`Q120` anisotropy, `Q146` anisotropy colour/range,
//     `Q212` direction, `Q44`+`Q113` LineMap + ST, `Q72` line scalars, `Q152` line range/saturation);
//   * the anisotropy/hair-shine specular function `R72` (`readable:124-138`) and its globals `R308`/`R73`/`R92`;
//   * the hair block `readable:1009-1066` - `Q156` anisotropy (lobes, half vector, edge fade), the `Q346`
//     LineMap/strand terms, and the second (broader) anisotropic lobe;
//   * the shared diffuse composition the hair branch sits on: the non-skin `USE_DIFFUSE_RAMP` arm
//     `readable:892-1007` (dark-albedo fallback `R113`/`R152`, ramp coordinate `R78`, the two ramp reads
//     `R32`/`R215`, the layer composite `R130`, the ramp-chroma term `R150`), the viewer light `readable:1006`,
//     and the saturation lift `readable:1140-1145`.
//   * the base colour: `R38`/`R99` (`readable:62-70`, `:88-102`). The hair tint arm is `Q284`
//     (`_HairBaseTintColor`/`_HairAddTintColor`, `readable:93-94`), gated by `_AvatarCustomizeEnable`:
//     the captured laevat hair states 0, so the compiled program has no tint arm and `s.albedo` is the base.
//     (Both tint colours are white on this material, so even the tinted arm would be the identity.)
//
// THE VIEWER'S COMPILE-TIME BRANCHES, AND WHICH MATERIAL STATE THIS FILE ASSUMES (from
// `material-feature-defines.js`'s `b3` on `M_actor_laevat_hair_01`):
//   FAMILY_HAIR (Q128)                         - yes, this file.
//   Q156 "hair shine"                          - a VIEWER LOOK TOGGLE (`setHairShine`), not a material field.
//                                                The captured app default look (`fS` in `eadba1af4f-DIaZSo8d.js`,
//                                                `hairShine:!0`) is ON, so the Q156 arm is the ACTIVE one; the
//                                                `!Q156` arm (`readable:1172-1176`) is ported beside it and selected
//                                                by `game_toon_hair_shine_enabled` (captured default `true`).
//   Q346 `_LineMap` present                    - yes in the capture (`extras.textures._LineMap`). deren has no
//                                                LineMap `toon_slot`, but the staged preview sidecar aliases the
//                                                image onto `_EmotionMap` (`uniform-packing.json`
//                                                `texture_lanes.Q44`, `_UseEmotionMap = 1`), whose slot is block-0
//                                                `.w`; that lane is sampled as DATA (the sRGB sampler decode is
//                                                undone), exactly like the ramp lanes. A zero lane keeps the
//                                                viewer's own no-texture arm (`R48 = 0`).
//   Q263 `_SplitNormalMap` present             - yes in the capture, and deren has `toon_slot::split_normal`
//                                                (lane 8, `toon_lanes2_at(...).x`, staged UNORM as DATA). The
//                                                captured channel dictionary (`readable:700-711`) is `.xy` = the
//                                                MODEL normal and `.zw` = the SMOOTH highlight normal, each
//                                                renormalised with its UNSCALED magnitude while the frame's xy are
//                                                scaled by `Q302`/`Q251` (profile lane 6 `.z`/`.w`). Both are
//                                                decoded below with the mesh's TANGENT frame.
//   Q243 `_UseDiffRampMap = 1`                 - yes, `s.toon_indices.x` is the lane.
//   Q38 `_UseSpecRampMap = 1`                  - yes, `s.toon_indices.z` is the lane.
//   Q13 `_UseMetallicGlossMap = 1`             - yes, `toon_lanes_at(...).y` is the lane (the game's P map).
//   Q132 self-shadow                           - a VIEW TOGGLE (`setSelfShadow`) whose captured default is OFF
//                                                (`fS.selfShadow:false`), so the ported lit weight is
//                                                `R144 = min(ramp.a, P.b)` and NOT deren's `calc_shadow`
//                                                (`readable:822-826`, `:940-948`).
//   Q178 saturation lift                       - a LOOK TOGGLE whose captured default is ON (`fS.saturationLift:!0`);
//                                                applied at the end (`readable:1140-1145`).
//   Q2 USE_TANGENT                             - deren CARRIES the vertex TANGENT (`pbr_vertex_output.tangent`,
//                                                location 4), and the preview asset this port is compared on ships
//                                                a TANGENT VEC4 on every primitive, so the captured `USE_TANGENT`
//                                                arm is the ported one for BOTH of its consumers:
//                                                  * the split-normal decode's frame (`cross(N,T)*w`,
//                                                    `readable:700-711`),
//                                                  * the strand frame's `R292` blend (`readable:1023-1032`:
//                                                    `cross(R159, mix(R201,-vWorldTangent.xyz,R202)) *
//                                                    mix(1.0,-vWorldTangent.w,R202)`).
//                                                The derivative frame remains only as the fallback for a primitive
//                                                with no TANGENT attribute (`vertex_tangent.w == 0`), which is the
//                                                viewer's own no-TANGENT arm (`R292 = cross(R159,R201)`).
//
// DEREN ADAPTERS USED, AND THE BRIDGES THEY IMPLY:
//   * `L` (`uMainLightDirection`): the viewer builds it CAMERA-RELATIVE (`viewer-lighting.js`'s `Bf`, yawBias 10,
//     minPitch 32). deren has no camera-relative rig in this stage, so the contract's fallback - the scene sun
//     `light_at(...).light_dir.xyz` - is used. MISSING RIG; `wf_camera_light(...,10,32)` in `toon/game_toon_math.h`
//     is the one-line swap if the Lead's A/B wants capture-aligned light.
//   * `V` is `normalize(cameraPosition - worldPosition)` on both sides (`readable:729`).
//   * `R141` (`vec3(viewMatrix[0][2],viewMatrix[1][2],viewMatrix[2][2])`, `readable:937`): deren's third view
//     column `mul(camera_at(...).view, (0,0,1,0))`, the SAME adapter `captured_eye_shade` documents.
//   * `Q116` (the hair node's WORLD ROTATION, `extractRotation(hairNode.matrixWorld)` via the viewer's `Kf`/`qf`,
//     read at `readable:1016-1021`): deren's per-draw model matrix IS the hair primitive's node world transform
//     (`runtime.cpp`'s `result->push.model = info.model_matrix`, from the loader's `node::transform_matrix`), so
//     its normalised basis is the captured frame. The staged preview's hair node has an IDENTITY rotation, so this
//     reproduces the captured frame exactly on the asset this port is compared on; a rotated node now rotates the
//     frame with it. A SKINNED character has no per-bone hair-node frame in this stage (the capture's `qf` would
//     return `Head_Local`/`Root`) - documented MISSING lane.
//   * `uMainLightColourScaled` / `uMainLightIntensity` / `uShadowTint` / `uEnvironmentMultiplier`: profile lanes
//     2.rgb / (captured 2.0, no lane yet) / 3.rgb / 0.z, with the captured Laevatain values as fallbacks.
//     The colour is scaled by the character volume multiplier (lane 0.y), which is how the viewer builds
//     `Q10 = rig.color * rig.intensity` (`viewer-lighting.js`'s `update`).
//   * the hair payload values (anisotropy set, anisotropy colour, LineMap ST/scalars, line range/saturation) are
//     read from profile lanes 7..12 when the Lead stages them, with the captured material JSON values as the
//     documented fallback (packing in `tmp/game_toon-port/hair-notes.md`).
//
// MISSING INPUTS (each has a documented neutral behaviour, marked `// MISSING` at its use):
//   * `_LineMap` texture (`extras.textures._LineMap`, `T_actor_common_hairline_03_M`): no `toon_slot` of its own;
//     the staged preview sidecar aliases it onto `_EmotionMap` (block-0 `.w`, staged sRGB) - see the Q346 note.
//     A zero lane keeps the viewer's own no-texture arm (`R48 = 0` -> `R257 = 1` -> the block is the identity).
//   * `_StrokeMap` / `_StrokeOn` / `_StrokeScale`: NOT part of the captured viewer's program - the hair payload
//     binder (`material-uniform-bindings.js`'s `k3`) binds only `_LineMap`/`_LineMap_ST`/`_UseLineMap`/
//     `_LineAmount`/`_LineValue`/`_LineIntensity`/`_LineRange`/`_LineSaturation`, a grep of the recovered module
//     (`eadba1af4f-DIaZSo8d.js`) for `_StrokeMap` / `_StrokeOn` / `_StrokeScale` / `_Stroke` has ZERO hits, and
//     the captured laevat hair material states `_StrokeOn = 0` and ships no `_StrokeMap` texture. The captured
//     program therefore has no stroke term and this port applies none - which IS that `_StrokeOn = 0` state. (The
//     GAME program's stroke is a different program - `the local decompiled reference shader tree (not distributed): hair_forward_spirv_decompiled.hlsl`.)
//   * `_SplitNormalMap`: decoded from `toon_slot::split_normal` with the mesh TANGENT frame (see Q263 above); the
//     only caveat is that the captured `.xy/.zw` dictionary comes from the viewer code (`readable:700-711`), not
//     from a per-texel comparison, and a primitive with no TANGENT falls back to the derivative frame.
//   * a SKINNED character's hair-node / `Head_Local` frame (`Q116`): this stage has the draw's model matrix only,
//     so the mesh node's rotation stands in for the captured bone-node rotation (exact for the static preview).
//   * the viewer's punctual-light loop `R114` (`readable:188-327`) and its `Q260` light count: the captured app
//     default has the additional-lights UI off (`Q260 = 0`; the same conclusion `captured_eye_shade` records), so
//     the loop contributes nothing in the captured frame. NOT ported.
//   * the depth-fade/clip term `R75 * Q164` (`readable:1177`): `R200` returns 0 while `Q185 < 0.5` (the captured
//     `e3()` default), so it is 0 in the capture. NOT ported.
//
// NOT PORTED, deliberately: tone mapping (`readable:1189`) and the sRGB encode (`readable:1190`) - the contract
// puts both in deren's post pass, so this function returns LINEAR HDR. Alpha/blend is the caller's. Emission is
// not in the captured hair program (`_UseEmission = 0`; the template's emission arm `Q158` is CLOTH-only).
//
// REQUIRED INCLUDE ORDER: after `toon/character_forward.slang` (for `surface_sample`, `frame_texture_sample`,
// `toon_lanes_at`, `push`, the camera/light UBOs) and after `toon/game_toon/game_toon_profile.h`. The header is
// included below too; its guard makes that harmless when the Lead's include order already provides it.

#include "toon/game_toon_math.h"
#include "toon/game_toon/captured/encoding.h"
#include "toon/game_toon/game_toon_profile.h"
#include "toon/game_toon/captured/game_toon_lights.h"

#ifndef DEREN_GAME_TOON_GAME_TOON_HAIR_SLANG
#define DEREN_GAME_TOON_GAME_TOON_HAIR_SLANG

// ============================================================================================================
// CAPTURED SHADER CONSTANTS (viewer-wide, not per material)
// ============================================================================================================

/// `R60` / `VIEWER_LIGHT_SCALE` - `readable:421` `const float VIEWER_LIGHT_SCALE= 0.2877;`, and
/// `comparison-summary.json` `shader_constants.o3` = 0.2877. It scales the shadow-tinted half of the light term
/// (`readable:1006`).
static const float game_toon_hair_viewer_light_scale = 0.2877;

/// The diffuse ramp's V coordinate: `readable:936` samples `uDiffuseRampMap` at `vec2(clamp(R78,0.002,0.998),0.5)`
/// (and `readable:938` does the same for the camera-axis read). The captured hair ramp
/// `T_actor_common_hair_01_RD` is `256x1` (`tmp/game_toon-reference/assets/mapping-evidence.json`, its `size`),
/// so 0.5 lands on its only row; the literal is kept rather than deren's banded-atlas V.
static const float game_toon_hair_ramp_v = 0.5;

/// The captured viewer LOOK TOGGLE `Q156` ("hair shine", `setHairShine`), whose captured default is ON:
/// `eadba1af4f-DIaZSo8d.js`'s look defaults object carries `hairShine:!0`, and no material row feeds the define
/// (`material-feature-defines.js`'s `b3` never adds `Q156`; `tu.setHairShine` toggles it). ON is the active arm
/// (`readable:1009-1066`); OFF is the template's `!Q156` arm (`readable:1172-1176`), ported below beside it.
/// There is no deren lane for a viewer look toggle, so the captured default is the compile-time choice.
static const bool game_toon_hair_shine_enabled = true;

/// `Q22` - the viewer RIG's specular multiplier, `viewer-lighting.js`'s uniform table (`Q22:{value:.5}`; it is
/// viewer-wide, so the captured character JSON does not carry it). Read by the `!Q156` hair band at
/// `readable:1174` (and by the cloth spec block at `:1169`).
static const float game_toon_hair_captured_specular_multiplier = 0.5;

// ============================================================================================================
// CAPTURED LAEVATAIN RIG FALLBACKS (used only when the profile lane is not stated; comparison-summary.json)
// ============================================================================================================

/// the character VOLUME MULTIPLIER (profile lane 0.y) - `comparison-summary.json` `laevat_volume.multiplier`
/// 1.05. The viewer folds it into `Q10 = rig.color * rig.intensity` (`viewer-lighting.js`'s `update`:
/// `Q10.value.copy(this.color).multiplyScalar(this.intensity)`, with the menu's `multiplier` copied into
/// `this.intensity` by `setMenuLight`), so it scales the main-light colour here.
/// MISSING PROFILE LANE: fallback is the captured 1.05.
static const float game_toon_hair_captured_volume_multiplier = 1.05;

/// `uMainLightColourScaled`'s COLOUR half (`rig.color`) - `comparison-summary.json`
/// `laevat_volume.lightColor` [1, 1, 1]. Profile lane 2.rgb when stated, otherwise this captured value.
static const float3 game_toon_hair_captured_main_light_colour = float3(1.0, 1.0, 1.0);

/// `uMainLightIntensity` (`Q143`) = `viewer-lighting.js`'s `zf = 2` initializer
/// (`Q143:{value:zf}`), recorded as the captured "main light intensity 2.0".
/// MISSING PROFILE LANE: no lane carries it unambiguously (lane 2.w is read by
/// `captured_eye_shade` as this value and by `captured_skin_face_shade` as the 1.05 colour multiplier; the
/// captured rig has BOTH numbers). The constant is therefore used, and the notes record the conflict.
static const float game_toon_hair_captured_main_light_intensity = 2.0;

/// `uShadowTint` (profile lane 3) - `comparison-summary.json` `laevat_volume.shadowTint`
/// [0.8783, 0.9302, 1.1217]. For hair the captured `Q62` is NOT aliased to the skin tint (`T3`:
/// `Q62 = fam==="skin" ? uSkinShadowTint : Q62`), so lane 3 is the correct lane here.
static const float3 game_toon_hair_captured_shadow_tint = float3(0.8783, 0.9302, 1.1217);

/// `uEnvironmentMultiplier` (`Q283`, profile lane 0.z) - `comparison-summary.json`
/// `laevat_volume.envMul` 0.55.
static const float game_toon_hair_captured_environment_multiplier = 0.55;

/// `uRampRangeBias` (`Q215`) for the captured Laevatain configuration: `laevat_volume.rangeBias` 0.0
/// (the viewer's code default is -0.4 in `viewer-lighting.js`, the captured per-character value is 0.0).
/// MISSING PROFILE LANE: no game_toon lane carries a per-viewer bias yet.
static const float game_toon_hair_captured_range_bias = 0.0;

// ============================================================================================================
// CAPTURED PER-MATERIAL PAYLOAD (M_actor_laevat_hair_01.json). Where a profile lane exists it is read when
// stated; these constants are the fallback for a sidecar that has not staged the lane yet.
// Suggested packing (readable:107-119): lanes 7..12, see `tmp/game_toon-port/hair-notes.md`:
//   lane 7  = Q120 (anisotropy value, value2, intensity, edge fade)
//   lane 8  = Q146 (anisotropy colour2 LINEAR rgb, range exponent)
//   lane 9  = Q212 (-_AnisotropyDirX, ...)
//   lane 10 = Q113 (_LineMap ST)
//   lane 11 = Q72  (_UseLineMap, _LineAmount, _LineValue, _LineIntensity)
//   lane 12 = Q152.x/.y (line range exponent, line saturation)
// ============================================================================================================

/// `Q120 = vec4(_AnisotropyValue, _AnisotropyValue2, _AnisotropyIntensity, _AnisotropyEdgeFade)`, wired by
/// `material-uniform-bindings.js`'s `k3` from `M_actor_laevat_hair_01.json` extras.floats
/// 0.49 / 0.51 / 1.2999999523162842 / 5.0; read at `readable:1038`, `:1041`, `:1042`, `:1060`.
/// MISSING PROFILE LANE: fallback is the captured row.
static const float4 game_toon_hair_captured_anisotropy =
    float4(0.49000000953674316, 0.5099999904632568, 1.2999999523162842, 5.0);

/// `Q146 = vec4(Na(_AnisotropyColor2).rgb, trunc(200*max(1-_AnisotropyRange2,0)))`, wired by `k3`.
/// `Na`/`Le` linearize the captured sRGB colour (`material-uniform-bindings.js`'s `color_conversion`:
/// `c<=.04045 ? c/12.92 : ((c+.055)/1.055)^2.4`), so `_AnisotropyColor2 = (0.672, 0, 0.0044)`
/// (`M_actor_laevat_hair_01.json` extras.colors) becomes (0.4091444674721517, 0.0, 0.00034055727554179566);
/// `_AnisotropyRange2 = 0.5` gives `trunc(200*0.5) = 100`.
/// MISSING PROFILE LANE: fallback is the captured row.
static const float3 game_toon_hair_captured_anisotropy_colour2 =
    float3(0.4091444674721517, 0.0, 0.00034055727554179566);
static const float game_toon_hair_captured_anisotropy_range_exponent = 100.0;

/// `Q212 = -(_AnisotropyDirX)` (`material-uniform-bindings.js`'s `k3`); `_AnisotropyDirX = 0.0`
/// (`M_actor_laevat_hair_01.json` extras.floats), so `Q212 = -0.0`, read at `readable:1019`.
/// MISSING PROFILE LANE: fallback is the captured 0.
static const float game_toon_hair_captured_anisotropy_dir_x = 0.0;

/// `Q113 = vec4(_LineMap_ST)` from `M_actor_laevat_hair_01.json` extras.st `_LineMap`
/// [9.300000190734863, 1.0, -0.3199999928474426, 0.0], read at `readable:1046`.
/// MISSING PROFILE LANE: fallback is the captured row.
static const float4 game_toon_hair_captured_line_st =
    float4(9.300000190734863, 1.0, -0.3199999928474426, 0.0);

/// `Q72 = vec4(_UseLineMap, _LineAmount, _LineValue, _LineIntensity)`, wired by `k3` from
/// `M_actor_laevat_hair_01.json` extras.floats 1.0 / 300.0 / 0.699999988079071 / 0.2199999988079071,
/// read at `readable:1048-1051`.
/// MISSING PROFILE LANE: fallback is the captured row.
static const float4 game_toon_hair_captured_line_scalars =
    float4(1.0, 300.0, 0.699999988079071, 0.2199999988079071);

/// `Q152.x = trunc(200*max(1-_LineRange,0))` and `Q152.y = _LineSaturation`, wired by `k3`;
/// `_LineRange = 0.949999988079071` gives `trunc(200*0.050000011920929) = 10`, `_LineSaturation = 1.5`.
/// Read at `readable:1053` and `:1057`.
/// MISSING PROFILE LANE: fallback is the captured row.
static const float game_toon_hair_captured_line_range_exponent = 10.0;
static const float game_toon_hair_captured_line_saturation = 1.5;

/// `uFallbackShadowBrightness` / `uFallbackShadowSaturation` (`Q347`/`Q348`, wired by `material-uniform-bindings.js`'s
/// `M3` from `_ShadowColorBrightness ?? .5` / `_ShadowColorSaturation ?? 1`): the captured hair row states
/// 0.550000011920929 / 1.100000023841858 (`M_actor_laevat_hair_01.json` extras.floats), consumed by `R113` at
/// `readable:908` because the hair material states `_UseShadowLutTex = 0`.
/// MISSING PROFILE LANE: no hair lane claims these; lanes 5/6 are eye/skin's payload in the sibling ports.
static const float game_toon_hair_captured_shadow_brightness = 0.550000011920929;
static const float game_toon_hair_captured_shadow_saturation = 1.100000023841858;

/// `R113`'s own luma (`readable:59`) - `vec3(0.2126, 0.7152, 0.0722)`, deliberately different in the last
/// digits from `R218`'s weights (`readable:423`); both are ported as written (the same split the eye/skin files
/// record).
static float game_toon_hair_shadow_luma(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

/// `R218` (`readable:422-424`) - the viewer's luma used by the layer composite and the luma lift.
static float game_toon_hair_luma(float3 c) {
    return dot(c, float3(0.212673, 0.715152, 0.072175));
}

/// `R113` (`readable:58-61`) - desaturate toward the shadow luma and floor at zero:
/// `max(mix(vec3(luma(c)), c, saturation), 0.0)`.
static float3 game_toon_hair_shadow_albedo(float3 c, float saturation) {
    const float luma = game_toon_hair_shadow_luma(c);
    return max(lerp(float3(luma, luma, luma), c, saturation), float3(0.0, 0.0, 0.0));
}

/// `max3(v)` as the capture writes it (`readable:1050`, `:1063`).
static float game_toon_hair_max3(float3 v) {
    return max(v.x, max(v.y, v.z));
}

/// `min3(v)` as the capture writes it (`readable:841`, `:962`).
static float game_toon_hair_min3(float3 v) {
    return min(v.x, min(v.y, v.z));
}

/// A profile lane is "stated" when it is not the all-zero word (`game_toon_profile.h`: "A lane left at zero means
/// 'not stated', never a guessed default"). Used for the payload lanes the header does not name yet.
static bool game_toon_hair_lane_stated(float4 value) {
    return value.x != 0.0 || value.y != 0.0 || value.z != 0.0 || value.w != 0.0;
}

/// `R72` (`readable:124-138`) - the viewer's anisotropic specular LOBE for the Q156 hair-shine path.
///
/// `x = dot(R308, half)`; `s = max(sqrt(max(1-x*x,0)),1e-4)`; `u = clamp(R73.g * s^200, 0, 1)`; `v = x>0 ?
/// R92^2 : 0`; the specular ramp is read at `(u, 1-v)` and the result is `tint * u * R92`. `R73.g` is the P
/// map's reflectivity and `R92` is the strand edge fade; both are parameters here so the capture's globals stay
/// visible at the call site.
///
/// The specular-ramp lane is `s.toon_indices.z` (`toon_slot::specular_ramp`). When it is absent the viewer's own
/// `!USE_SPECULAR_RAMP` arm is used (`tint = vec3(1)`, `readable:134`), so no colour is invented.
static float3 game_toon_hair_specular_lobe(const float3 lobe_normal, const float3 half_dir, const float p_map_g,
                                          const float strand_fade, const uint spec_ramp_lane) {
    const float x = dot(lobe_normal, half_dir);                                                          // readable:125
    const float s = max(sqrt(max(1.0 - x * x, 0.0)), 1e-4);                                              // readable:126
    const float u = clamp(p_map_g * exp2(200.0 * log2(s)), 0.0, 1.0);                                    // readable:127
    const float v = x > 0.0 ? strand_fade * strand_fade : 0.0;                                           // readable:128
    const float3 tint = (spec_ramp_lane != 0u)
        ? game_toon_encoding_linear_to_srgb(frame_texture_sample(spec_ramp_lane,                                      // readable:131
                               clamp(float2(u, 1.0 - v), float2(0.002, 0.002), float2(0.998, 0.998))).rgb)
        : float3(1.0, 1.0, 1.0);                                                                         // readable:134
    return tint * u * strand_fade;                                                                       // readable:137
}

// ============================================================================================================
// THE SHADE
// ============================================================================================================

/// @brief the captured viewer's FAMILY_HAIR response (`character.frag.readable.glsl:892-1007` + `:1009-1066` +
///        `:1140-1145`), returned as LINEAR HDR radiance
/// @param s the gathered surface; `s.albedo` is `_BaseMap * _BaseColor` (linear), `s.normal` is deren's decoded
///        shading normal (for this material the geometric one - the captured hair has no `_BumpMap`),
///        `s.toon_indices.x` the diffuse ramp lane and `.z` the specular ramp lane
/// @param world_pos interpolated world position (the view vector and the derivative-frame-free strand frame)
/// @param geo_normal interpolated geometric normal (only a degenerate-normal fallback; the captured hair's N is
///        `normalize(vWorldNormal)` with no normal map)
/// @param uv the material uv (base map, P map, ramps and the line stripe all key off it)
/// @param vertex_tangent the mesh's own TANGENT FRAME from the vertex stage: xyz the world-space tangent, w the
///        authored glTF handedness (+1/-1). ALL ZERO (`w == 0`) when the primitive carries no TANGENT attribute;
///        the split-normal decode then falls back to `surface_tbn`'s derivative frame, which is the behaviour
///        this stage had before the vertex path existed (see the header's Q2 note).
/// @param is_front the rasterizer's front/back decision. UNUSED: the captured hair material is single-sided
///        (`M_actor_laevat_hair_01.json` `"doubleSided": false`) and deren's `s.normal` already carries the
///        double-sided flip for a double-sided material (`readable:694-698`'s `R180` is 1 here).
/// @return LINEAR HDR radiance - no tone map, no sRGB encode (the post pass owns both, per the contract)
float3 captured_hair_shade(surface_sample s, float3 world_pos, float3 geo_normal, float2 uv, float4 vertex_tangent, bool is_front) {
    const uint material_index = push.material_index;
    const uint4 toon_lanes = toon_lanes_at(heap_slots_toon_lanes, material_index);
    const uint4 toon_lanes2 = toon_lanes2_at(heap_slots_toon_lanes, material_index);

    // ---- THE PROFILE LANES (game_toon_profile.h) ----
    //
    // lane 0: x = enable, y = character volume multiplier, z = environment multiplier, w = env-shadow multiplier.
    // lane 2: rgb = main-light colour, w = the light's own intensity (the two captured values this branch needs
    //         are 1.05 - folded into rgb by the scene rig - and 2.0; see the MISSING note on the constant).
    // lane 3: rgb = shadow tint. Hair does NOT take the skin alias (`T3`), so lane 3 is right here.
    // lanes 7..12: this family's captured payload (a zero word = "not stated" -> captured fallback).
    const float4 profile0 = game_toon_profile_lane(material_index, 0u);
    const float4 profile1 = game_toon_profile_lane(material_index, 1u);
    const float4 profile3 = game_toon_profile_lane(material_index, 3u);
    const float4 profile7 = game_toon_profile_lane(material_index, 7u);
    const float4 profile8 = game_toon_profile_lane(material_index, 8u);
    const float4 profile9 = game_toon_profile_lane(material_index, 9u);
    const float4 profile10 = game_toon_profile_lane(material_index, 10u);
    const float4 profile11 = game_toon_profile_lane(material_index, 11u);
    const float4 profile12 = game_toon_profile_lane(material_index, 12u);

    const float volume_multiplier = 1.0; // captured 1.05 is already folded into lane 1 by uniform-packing
    const float environment_multiplier =
        profile0.z > 0.0 ? profile0.z : game_toon_hair_captured_environment_multiplier;
    // MISSING PROFILE LANE: the separate `uMainLightIntensity`; see the constant's note.
    const float main_light_intensity = game_toon_main_light_intensity(material_index);
    const float3 main_light_colour =
        dot(profile1.rgb, profile1.rgb) > 0.0 ? profile1.rgb : game_toon_hair_captured_main_light_colour;
    const float3 main_light_scaled = main_light_colour;                            // `Q10` (`:1006`); 1.05 already in lane 1
    const float3 shadow_tint =
        game_toon_hair_lane_stated(profile3) ? profile3.rgb : game_toon_hair_captured_shadow_tint;      // `Q62`
    // MISSING PROFILE LANE: no hair lane carries `Q347`/`Q348` (see the constants).
    const float shadow_brightness = game_toon_hair_captured_shadow_brightness;                         // `Q347`
    const float shadow_saturation = game_toon_hair_captured_shadow_saturation;                         // `Q348`

    // The captured payload (Q120 / Q146 / Q212 / Q113 / Q72 / Q152), read from lanes 7..12 when staged.
    const float4 anisotropy = game_toon_hair_lane_stated(profile7) ? profile7 : game_toon_hair_captured_anisotropy;
    const float3 anisotropy_colour2 = game_toon_hair_lane_stated(profile8)
        ? profile8.rgb : game_toon_hair_captured_anisotropy_colour2;
    const float anisotropy_range_exponent = dot(profile8, profile8) > 0.0
        ? profile8.w : game_toon_hair_captured_anisotropy_range_exponent;
    const float anisotropy_dir_x = game_toon_hair_lane_stated(profile9)
        ? profile9.x : game_toon_hair_captured_anisotropy_dir_x;                                       // `Q212`
    const float4 line_st = game_toon_hair_lane_stated(profile10) ? profile10 : game_toon_hair_captured_line_st;
    const float4 line_scalars = game_toon_hair_lane_stated(profile11)
        ? profile11 : game_toon_hair_captured_line_scalars;
    const float line_range_exponent = dot(profile12, profile12) > 0.0
        ? profile12.x : game_toon_hair_captured_line_range_exponent;
    const float line_saturation = dot(profile12, profile12) > 0.0
        ? profile12.y : game_toon_hair_captured_line_saturation;

    // ---- THE MATERIAL'S P MAP (`_MetallicGlossMap`, the game's `P`/ORM map) ----
    //
    // `Q13` in the capture (`readable:773-782`, `:1010-1015`): `.r` is the strand/tangent blend mask (unused by
    // this branch's no-TANGENT arm), `.g` the F0/reflectivity scale, `.b` the AO/occlusion mask and `.a` the
    // smoothness. deren's carrier is `toon_slot::metallic_gloss`, `toon_lanes_at(...).y` (block 0, element 1 -
    // the same lane `game_toon/hair.slang` and `character_forward.slang` read).
    // A zero lane is the capture's own `#ifdef Q13`-off state, where `R73 = vec4(1.0)` (`readable:1010`), so no
    // material value is invented: reflectivity 1, AO 1, smoothness 1.
    const bool has_p_map = toon_lanes.y != 0u;
    const float4 p_map = has_p_map
        ? frame_texture_sample(toon_lanes.y, uv)                                                      // readable:779
        : float4(1.0, 1.0, 1.0, 1.0);                                                                 // readable:1010
    const float p_map_occlusion = has_p_map ? p_map.b : 1.0;                                          // `R299.b` (`:933`)

    // ---- N, L, V AND THE CAMERA AXIS ----
    //
    // The captured hair has no `_BumpMap` (`Q98` undefined: `material-feature-defines.js` gates it on
    // `_BumpMap`, and `M_actor_laevat_hair_01.json` has no `_BumpMap` row), so `N = normalize(vWorldNormal)`
    // with the double-sided factor is exactly deren's `s.normal` (`readable:691-698`, `:721-725`).
    const float normal_len_sq = dot(s.normal, s.normal);
    float3 shading_normal = normal_len_sq > 1.0e-12 ? s.normal * rsqrt(normal_len_sq)
                                                    : normalize(geo_normal);
    float3 highlight_normal = shading_normal;                                                          // `R159`
    const float3 base_normal = normalize(geo_normal);
    // ---- THE MESH'S OWN TANGENT FRAME (`Q2` USE_TANGENT, readable:1023-1033) ----
    //
    // TWO consumers take the captured `USE_TANGENT` frame: the split-normal decode below (`readable:700-711`)
    // and the strand frame's `R292` blend (`readable:1023-1032`). The preview GLB
    // (`laevatain_game_toon_preview.glb`) ships a TANGENT VEC4 on every primitive and deren's loader keeps it
    // (`pbr.slang`'s `pbr_vertex_output.tangent`, location 4), so this is the faithful arm, not a substitute:
    //   T = normalise(T_vertex - N * dot(N, T_vertex))      (Gram-Schmidt: the capture's VERTEX stage performs
    //                                                          exactly this against vWorldNormal, and deren's
    //                                                          vertex output is only normalised - see
    //                                                          `character.vert.readable.glsl:47-49`)
    //   B = cross(N, T) * T_vertex.w                       (the glTF handedness, as the spec defines it -
    //                                                          NO Unity/glTF Y bridge here: the authored w
    //                                                          already states the map's handedness)
    // `vertex_tangent.w == 0` is the loader's "no TANGENT attribute" record; then and only then the derivative
    // frame stands in (the viewer's own no-TANGENT arm) and the Y bridge applies to it.
    float3 mesh_tangent = float3(0.0, 0.0, 0.0);
    bool mesh_tangent_valid = false;
    if (abs(vertex_tangent.w) > 0.5) {
        const float tangent_len_sq = dot(vertex_tangent.xyz, vertex_tangent.xyz);
        if (tangent_len_sq > 1.0e-12) {
            const float3 unit_tangent = vertex_tangent.xyz * rsqrt(tangent_len_sq);
            const float3 orthogonal = unit_tangent - base_normal * dot(base_normal, unit_tangent);
            const float orthogonal_len_sq = dot(orthogonal, orthogonal);
            if (orthogonal_len_sq > 1.0e-12) {
                mesh_tangent = orthogonal * rsqrt(orthogonal_len_sq);
                mesh_tangent_valid = true;
            }
        }
    }
    // `Q263` `_SplitNormalMap`: deren's block-1 `.x` is `toon_slot::split_normal`, staged UNORM as DATA; the
    // preview sidecar stages the captured `_SplitNormalMap` there. The captured decode (`readable:700-711`) is
    // transcribed channel by channel:
    //   `split = texel*2-1`; `R231 = sqrt(max(1-min(dot(split.xy,split.xy),1),0))` - the UNSCALED xy magnitude -
    //   `R210` the same for `.zw`; `R101 = split.xy*Q302` and `R168 = split.zw*Q251` are the SCALED xy;
    //   `R121 = cross(N, T_vertex)*T_vertex.w`; the model normal is `R101.x*T + R101.y*R121 + R231*N` and the
    //   smooth highlight normal `R159` is `R168.x*T + R168.y*R121 + R210*N`.
    // `Q302`/`Q251` are profile lane 6 `.z`/`.w` (captured 1.0 each; the capture's own defaults are
    // `_BumpScale ?? 1` / `_SpecBumpScale ?? 1`).
    const uint split_normal_lane = toon_lanes2.x;
    if (split_normal_lane != 0u) {
        float3 frame_tangent = float3(1.0, 0.0, 0.0);
        float3 frame_bitangent = float3(0.0, 1.0, 0.0);
        bool frame_valid = false;
        if (mesh_tangent_valid) {
            frame_tangent = mesh_tangent;
            frame_bitangent = cross(base_normal, mesh_tangent) * vertex_tangent.w;
            frame_valid = true;
        }
        if (!frame_valid && surface_tbn(world_pos, uv, frame_tangent, frame_bitangent)) {
            frame_bitangent = -frame_bitangent; // Unity/glTF tangent-space Y bridge (derivative frame only)
            frame_valid = true;
        }
        if (frame_valid) {
            const float4 split_texel = frame_texture_sample(split_normal_lane, uv);
            const float4 bump_scales = game_toon_profile_lane(material_index, 6u);
            const float model_scale = bump_scales.z > 0.0 ? bump_scales.z : 1.0;
            const float smooth_scale = bump_scales.w > 0.0 ? bump_scales.w : 1.0;
            const float2 model_raw = split_texel.xy * 2.0 - 1.0;                                       // `split.xy`
            const float2 smooth_raw = split_texel.zw * 2.0 - 1.0;                                      // `split.zw`
            const float2 model_xy = model_raw * model_scale;                                           // `R101`
            const float2 smooth_xy = smooth_raw * smooth_scale;                                        // `R168`
            // THE Z TERM IS THE UNSCALED MAGNITUDE (`R231`/`R210`, readable:702-703): the capture reconstructs
            // the missing component from the raw `*2-1` texel and applies `Q302`/`Q251` to the xy only.
            const float model_z = sqrt(max(1.0 - min(dot(model_raw, model_raw), 1.0), 0.0));           // `R231`
            const float smooth_z = sqrt(max(1.0 - min(dot(smooth_raw, smooth_raw), 1.0), 0.0));        // `R210`
            const float3 decoded_model = normalize(model_xy.x * frame_tangent + model_xy.y * frame_bitangent + model_z * base_normal);
            const float3 decoded_smooth = normalize(smooth_xy.x * frame_tangent + smooth_xy.y * frame_bitangent + smooth_z * base_normal);
            if (is_front) {
                shading_normal = decoded_model;
                highlight_normal = decoded_smooth;
            } else {
                shading_normal = -decoded_model;
                highlight_normal = -decoded_smooth;
            }
        }
    }
    const float3 camera_forward_axis = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz);
    const float3 light_dir = game_toon_profile_enabled(material_index)
        ? normalize(wf_camera_light(-camera_forward_axis, game_toon_yaw_bias(material_index), game_toon_min_pitch(material_index)))
        : normalize(light_at(heap_light_slot).light_dir.xyz);                                       // `L` (`:728`)
    const float3 camera_pos = camera_at(heap_camera_slot).camera_pos.xyz;
    const float3 view_dir = normalize(camera_pos - world_pos);                                         // `V` (`:729`)
    // `R141` (`readable:937`) - the camera's world axis, deren's third view column (the adapter
    // `captured_eye_shade` documents): used only by the camera-axis diffuse-ramp read below.
    const float3 camera_axis = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz);

    // ---- THE DARK ALBEDO AND THE RAMP COORDINATE (readable:892-919) ----
    //
    // `R78 = clamp(R128 + uRampRangeBias, -1, 1) * 0.5 + 0.5` (`:892`), where `R128 = dot(N,L)` (`:731`).
    const float no_l = dot(shading_normal, light_dir);                                                 // `R128`
    const float ramp_x = clamp(no_l + game_toon_hair_captured_range_bias, -1.0, 1.0) * 0.5 + 0.5;      // `R78`
    // `R152` = the no-shadow-LUT fallback dark albedo (`readable:904-910`, `:918-919`):
    // `R113(base.rgb, uFallbackShadowSaturation) * uFallbackShadowBrightness`, then `*= 0.96 *
    // uEnvironmentMultiplier`. The hair material states `_UseShadowLutTex = 0`, so this is its arm.
    const float3 dark_raw = game_toon_hair_shadow_albedo(s.albedo, shadow_saturation) * shadow_brightness;
    const float3 dark_albedo = dark_raw * 0.96 * environment_multiplier;                              // `R152`
    // `R169 = clamp(N.y + 0.15, 0, 1) * 1.5 + 0.5` (`readable:921`): the world-Y ambient term of the light.
    const float vertical_term = clamp(shading_normal.y + 0.15, 0.0, 1.0) * 1.5 + 0.5;                // `R169`
    // `R266 = uMainLightColourScaled * uMainLightIntensity` (`readable:927`).
    const float3 light_term = main_light_scaled * main_light_intensity;                               // `R266`

    // ---- THE DIFFUSE COMPOSITION (readable:929-1007) ----
    //
    // The hair material states `_UseDiffRampMap = 1`, so the captured `USE_DIFFUSE_RAMP` arm is the ported one.
    // `R144 = min(R32.a, mask)` is the LIT weight (`:944-948`, with `mask = P.b` when `Q13` is on, else 1);
    // `R215` is a second ramp read on the camera axis whose alpha enters the layer blend as `R318`.
    float lit_weight = 0.0;                                                                            // `R144`
    float3 pre_light_colour = float3(0.0, 0.0, 0.0);
    if (s.toon_indices.x != 0u) {
        const float4 ramp_nol = frame_texture_sample(
            s.toon_indices.x, clamp(float2(ramp_x, game_toon_hair_ramp_v), float2(0.002, 0.002),
                                    float2(0.998, 0.998)));                                         // `R32` (`:936`)
        const float4 ramp_cam = frame_texture_sample(
            s.toon_indices.x,
            clamp(float2(clamp(dot(shading_normal, camera_axis), -1.0, 1.0) * 0.5 + 0.5,
                         game_toon_hair_ramp_v), float2(0.002, 0.002), float2(0.998, 0.998)));     // `R215` (`:938`)
        lit_weight = min(ramp_nol.a, p_map_occlusion);                                              // `R144`
        const float ramp_cam_weight = ramp_cam.a * p_map_occlusion;                                 // `R318`
        const float3 lit_albedo = s.albedo * 0.96;                                                  // `R334`
        const float dark_luma = game_toon_hair_luma(0.65 * dark_albedo);                             // `R22`
        float3 colour_mix = lerp(float3(dark_luma, dark_luma, dark_luma) +
                                     1.2 * (0.65 * dark_albedo - float3(dark_luma, dark_luma, dark_luma)),
                                 dark_albedo,
                                 saturate(ramp_nol.a + ramp_cam_weight));                            // `R130` (`:960`)
        colour_mix = lerp(colour_mix, lit_albedo, lit_weight);                                      // `R130` (`:961`)
        const float ramp_chroma = game_toon_hair_max3(ramp_nol.rgb) - game_toon_hair_min3(ramp_nol.rgb); // `R150`
        pre_light_colour = colour_mix * lerp(float3(1.0, 1.0, 1.0), ramp_nol.rgb, ramp_chroma);      // `R130*...`
        pre_light_colour *= clamp(game_toon_hair_luma(colour_mix) /
                                       max(game_toon_hair_luma(pre_light_colour), 0.001), 0.0, 1.5);   // `:964`
    } else {
        // MISSING LANE for THIS MATERIAL'S RAMP ONLY (the capture states `_UseDiffRampMap = 1`): the viewer's own
        // no-ramp arm (`readable:995-1004`), whose lit weight is the hard 0.48/0.52 step and whose colour is
        // `mix(R152, base.rgb, R144)` - not a second shading model.
        lit_weight = smoothstep(0.48, 0.52, ramp_x);
        pre_light_colour = lerp(dark_albedo, s.albedo, lit_weight);
    }

    // ---- THE VIEWER LIGHT (readable:1005-1007) ----
    //
    // `light = mix(uShadowTint, 1, R144) * R169 * VIEWER_LIGHT_SCALE * uMainLightColourScaled
    //          + mix(vec3(luma(R266)), R266, R144)`. This is the captured shading colour; it is NOT deren's sun
    // (the contract's "keep deren's lighting" boundary applies to the INPUT, `light_dir` above).
    const float3 ambient_half = lerp(shadow_tint, float3(1.0, 1.0, 1.0), lit_weight) *
                                vertical_term * game_toon_hair_viewer_light_scale * main_light_scaled;
    const float r266_luma = game_toon_hair_luma(light_term);
    const float3 direct_half = lerp(float3(r266_luma, r266_luma, r266_luma), light_term, lit_weight);
    const float3 light = ambient_half + direct_half;                                                   // `readable:1006`
    float3 colour = pre_light_colour * light;                                                          // `readable:1007`

    // ========================================================================================================
    // THE Q156 ANISOTROPY / HAIR-SHINE BLOCK (readable:1009-1066) - the captured look's ACTIVE arm
    // ========================================================================================================
    // The `!Q156` arm's contribution is computed in the branch below but ADDED at the end, because the capture
    // puts that arm AFTER `R114` (the punctual lights, `readable:1156`) and AFTER the saturation lift
    // (`readable:1140-1145`): this variable is that placement. It stays zero while the captured default
    // `game_toon_hair_shine_enabled` is true.
    float3 hair_neg_q156_band = float3(0.0, 0.0, 0.0);
    if (game_toon_hair_shine_enabled) {
    //
    // ---- THE ROOT / HAIR FRAME (`Q116`, readable:1016-1019) ----
    //
    // `Q116` is the hair node's WORLD ROTATION: `material-uniform-bindings.js`'s `k3` `Q116:t`, with `t` from
    // `Kf`/`qf` (`extractRotation(hairNode.matrixWorld)`; for a static mesh `qf` returns the MESH node itself).
    // deren's per-draw model matrix IS that node's world transform (`runtime.cpp`'s
    // `result->push.model = info.model_matrix`, filled from the loader's `node::transform_matrix`), and
    // `three.js`'s `extractRotation` is its normalised rotation basis - so the three captured axes are the three
    // normalised basis vectors of `push.model`, using the SAME spelling `pbr.slang` gives a direction
    // (`mul(v, (float3x3)world)`): `push.model[i]` is the world image of the node's local axis i.
    //   `R86 = normalize(Q116[0])`, `R277 = normalize(Q116[2])`, `normalize(Q116[1])` (`readable:1017-1019,1039`).
    // The staged preview asset's hair node has an IDENTITY rotation (its node table carries a translation only),
    // so this reproduces the previous constants exactly on the asset this port is compared on; a rotated node
    // now rotates the captured frame with it. A SKINNED character has no per-bone hair-node frame in this stage
    // (the capture's `qf` would return `Head_Local`/`Root`) - documented MISSING lane.
    const float3 root_x = normalize(float3(push.model[0].xyz));                                        // `Q116[0]`
    const float3 root_y = normalize(float3(push.model[1].xyz));                                        // `Q116[1]`
    const float3 root_z = normalize(float3(push.model[2].xyz));                                        // `Q116[2]`
    // `R20 = normalize(Q116 * vec3(Q212, 1.0, 0.0))` (`readable:1019`) - the local (Q212,1,0) direction through
    // the captured frame; for the captured `_AnisotropyDirX = 0` this is `root_y`, but it follows the frame.
    const float3 root_tilt = normalize(root_x * anisotropy_dir_x + root_y);                            // `R20` (`:1019`)
    // `R201 = cross(R159, R20)`; `R292 = cross(R159, R201)` (`readable:1020-1021`), then the captured
    // `USE_TANGENT` arm (`readable:1023-1032`) replaces the bitangent with
    //     cross(R159, mix(R201, -vWorldTangent.xyz, R202)) * mix(1.0, -vWorldTangent.w, R202)
    // where the blend weight `R202` is the metallic/gloss map's RED channel (`Q13`: `R299.r`; the captured hair
    // material defines Q13 - `_UseMetallicGlossMap = 1`). `-vWorldTangent.xyz` is the VERTEX-stage orthogonalised
    // tangent, which is exactly `mesh_tangent` (`character.vert.readable.glsl:47-49` does the same
    // Gram-Schmidt); a primitive with no TANGENT keeps the viewer's no-TANGENT `R292 = cross(R159,R201)`.
    const float3 strand_tangent = cross(highlight_normal, root_tilt);                                  // `R201`
    float3 strand_bitangent = cross(highlight_normal, strand_tangent);                                 // `R292` (no-TANGENT)
    if (mesh_tangent_valid) {
        const float strand_blend = p_map.r;                                                            // `R202` (`:1025`)
        strand_bitangent =
            cross(highlight_normal, lerp(strand_tangent, -mesh_tangent, strand_blend)) *
            lerp(1.0, -vertex_tangent.w, strand_blend);                                                // `readable:1031`
    }
    // `R81`/`R147`/`R92` (`readable:1034-1038`): the edge fade is the XZ dot of the highlight normal and V,
    // raised to `Q120.w` (the captured `_AnisotropyEdgeFade = 5`).
    float2 normal_xz = float2(highlight_normal.x, highlight_normal.z);
    normal_xz *= rsqrt(max(dot(normal_xz, normal_xz), 1.0e-8));                                        // `R81`
    float2 view_xz = float2(view_dir.x, view_dir.z);
    view_xz *= rsqrt(max(dot(view_xz, view_xz), 1.0e-8));                                              // `R147`
    const float strand_fade = pow(clamp(dot(normal_xz, view_xz), 0.0, 1.0), anisotropy.w);            // `R92`
    // `R108 = R86*dot(V,R86) + normalize(Q116[1])*L.y + R277*dot(V,R277)` (`readable:1039`); with the identity
    // root frame `normalize(Q116[1])` is world Y, so this is V with its world-Y component replaced by L.y.
    const float3 tilted_view = root_x * dot(view_dir, root_x) + root_y * light_dir.y +
                               root_z * dot(view_dir, root_z);                                         // `R108`
    const float3 half_lobe = normalize(normalize(2.0 * tilted_view + light_dir) + view_dir);          // `R238` (`:1040`)
    // The primary lobe: `R308 = normalize(R159*(2*Q120.x-1) + R292)` (`readable:1041`) and `R72(R238)` at
    // `readable:1043`, scaled by `R133 = 0.2 * R73.g * Q120.z` (`readable:1042`).
    const float3 lobe_normal_primary = normalize(highlight_normal * (2.0 * anisotropy.x - 1.0) + strand_bitangent);
    const float primary_scale = 0.2 * p_map.g * anisotropy.z;                                          // `R133`
    const float3 primary_lobe =
        game_toon_hair_specular_lobe(lobe_normal_primary, half_lobe, p_map.g, strand_fade,
                                    s.toon_indices.z);                                                 // `R287`

    // ---- THE Q346 LineMap / STRAND TERMS (readable:1045-1059) ----
    //
    // The captured program has `Q346` (its hair material names `_LineMap`). The staged preview sidecar
    // aliases `_LineMap` onto `_EmotionMap` (uniform-packing.json `texture_lanes.Q44`), and deren carries
    // that slot as block-0 `.w`; sampling it here restores the viewer's strand lines. A zero lane keeps the
    // captured no-texture arm (identity).
    const float2 line_uv = float2(uv.x, 1.0 - uv.y) * line_st.xy + line_st.zw;                         // `R296` (`:1046`)
    const uint line_map_lane = toon_lanes.w;                                                           // `_LineMap` via `_EmotionMap` alias
    float line_map_sample = 0.0;
    if (line_map_lane != 0u) {
        // The preview sidecar aliases `_LineMap` to `_EmotionMap`; deren stages the emotion lane sRGB,
        // but the captured line map is Non-Color data. Undo the sampler decode like the ramp lanes.
        const float sampled = frame_texture_sample(line_map_lane, float2(line_uv.x, 1.0 - line_uv.y)).r;
        line_map_sample = game_toon_encoding_linear_to_srgb(float3(sampled, sampled, sampled)).x;  // `R48` (`:1047`)
    }
    const float line_stripe = ceil(max(frac(uv.x * line_scalars.y) - 0.5, 0.0));                      // `R192` (`:1048`)
    float line_weight = lerp(line_stripe, 1.0 - line_map_sample, line_scalars.x) * line_scalars.w +
                        (1.0 - line_scalars.w);                                                        // `R257` (`:1049`)
    line_weight = lerp(line_weight, 1.0, game_toon_hair_max3(primary_lobe));                            // `R257` (`:1050`)
    const float3 lobe_normal_broad =
        normalize(highlight_normal * (2.0 * line_scalars.z - 1.0) + strand_bitangent);                 // `R66` (`:1051`)
    const float broad_dot = dot(lobe_normal_broad, half_lobe);                                         // `R300`
    const float broad_shape = max(sqrt(max(1.0 - broad_dot * broad_dot, 0.0)), 1.0e-4);                // `:1053`
    const float broad_lobe = exp2(line_range_exponent * log2(broad_shape));                            // `R43`
    const float line_gain = 1.0 + p_map.g * broad_lobe * (line_weight - 1.0);                          // `R271` (`:1054`)
    const float3 line_colour = colour * line_gain;                                                     // `R337` (`:1055`)
    const float line_luma = game_toon_hair_luma(line_colour);                                           // `R56` (`:1056`)
    colour = float3(line_luma, line_luma, line_luma) +
             lerp(line_saturation, 1.0, line_gain) * (line_colour - float3(line_luma, line_luma, line_luma)); // `:1057`

    // ---- THE SECOND (BROADER) ANISOTROPIC LOBE (readable:1060-1064) ----
    //
    // `R336 = normalize(R159*(2*Q120.y-1) + R292)`; `R288 = max(sqrt(1-dot(R336,R238)^2),1e-4)`;
    // `R335 = R73.a * Q146.rgb * R288^Q146.w * R92 * (1 - max3(R287))` - tinted by the captured
    // `_AnisotropyColor2` (LINEAR) and weighted by the P map's smoothness, suppressed where the sharp lobe fires.
    const float3 lobe_normal_second =
        normalize(highlight_normal * (2.0 * anisotropy.y - 1.0) + strand_bitangent);                   // `R336` (`:1060`)
    const float second_dot = dot(lobe_normal_second, half_lobe);                                       // `R214`
    const float second_shape = max(sqrt(max(1.0 - second_dot * second_dot, 0.0)), 1.0e-4);             // `R288`
    const float3 second_lobe = p_map.a * anisotropy_colour2 *
                               exp2(anisotropy_range_exponent * log2(second_shape)) * strand_fade *
                               (1.0 - game_toon_hair_max3(primary_lobe));                               // `R335` (`:1063`)
    // `readable:1064`: `color += (R287*R133 + R335) * light * mix(uEnvironmentMultiplier,1,R144) *
    // (0.5 + 0.5*R144)`.
    colour += (primary_lobe * primary_scale + second_lobe) * light *
              lerp(environment_multiplier, 1.0, lit_weight) * (0.5 + 0.5 * lit_weight);
    } else {
        // ====================================================================================================
        // THE `!Q156` SPECULAR-RAMP ARM (readable:1172-1176) - the captured look with hair shine OFF
        // ====================================================================================================
        //     #if defined(FAMILY_HAIR) && defined(USE_SPECULAR_RAMP) && !defined(Q156)
        //     vec3 band=texture2D(uSpecularRampMap,vec2(R196,0.5)).rgb;
        //     color+=band*base.rgb*Q22*smoothstep(0.0,0.2,R128)*uMainLightColourScaled*uMainLightIntensity;
        //     #endif
        // `R196 = clamp(dot(N, normalize(L+V)),0,1)` (`readable:733`), `R128 = dot(N,L)` (`:731`), `Q22 = 0.5` is
        // the viewer RIG constant (`viewer-lighting.js`'s uniform table), and the product
        // `uMainLightColourScaled*uMainLightIntensity` is this file's `light_term` (`R266`, `:927`). The arm is
        // compiled only when the material states `USE_SPECULAR_RAMP` (Q38: `_UseSpecRampMap = 1`, whose lane is
        // `s.toon_indices.z`); its tap is RAW ramp data through deren's sRGB ramp lane, so the sampler decode is
        // undone exactly as `R72` does (`readable:131`).
        if (s.toon_indices.z != 0u) {
            const float no_h = clamp(dot(shading_normal, normalize(light_dir + view_dir)), 0.0, 1.0);  // `R196`
            const float3 spec_band = game_toon_encoding_linear_to_srgb(
                frame_texture_sample(s.toon_indices.z, float2(no_h, 0.5)).rgb);                        // `band`
            hair_neg_q156_band = spec_band * s.albedo * game_toon_hair_captured_specular_multiplier *
                                 smoothstep(0.0, 0.2, no_l) * light_term;                              // `readable:1174`
        }
    }

    // ---- `_StrokeMap` / `_StrokeOn` / `_StrokeScale`: VERIFIED NOT PART OF THE CAPTURED PROGRAM ----
    //
    // The captured viewer's hair payload binder (`material-uniform-bindings.js`'s `k3`) binds exactly
    // `_LineMap`/`_LineMap_ST`/`_UseLineMap`/`_LineAmount`/`_LineValue`/`_LineIntensity`/`_LineRange`/
    // `_LineSaturation` - no stroke uniform is bound at all, and a grep of the recovered module
    // (`eadba1af4f-DIaZSo8d.js`) for `_StrokeMap` / `_StrokeOn` / `_StrokeScale` / `_Stroke` has ZERO hits. The
    // captured `M_actor_laevat_hair_01.json` states `_StrokeOn = 0` and ships no `_StrokeMap` texture, so the
    // captured program has no stroke term; this port applies none, which IS that `_StrokeOn = 0` state. (The
    // GAME program's stroke offset is a different program - see
    // `the local decompiled reference shader tree (not distributed): hair_forward_spirv_decompiled.hlsl` - and is not the captured viewer.)

    // ---- THE SATURATION LIFT (readable:1140-1145) ----
    //
    // A LOOK TOGGLE (`setSaturationLift`), captured default ON (`fS.saturationLift:!0`), and gated in the
    // template on `USE_DIFFUSE_RAMP` + the family set (hair included). Luma is lerped around mid-grey, NOT a
    // tone map: the returned radiance stays linear.
    const float lift_luma = game_toon_hair_luma(colour);                                                // `R251` (`:1141`)
    const float lift = clamp(lift_luma - 0.5, 0.0, 0.5);                                               // `R174` (`:1142`)
    colour = float3(lift_luma, lift_luma, lift_luma) +
             (colour - float3(lift_luma, lift_luma, lift_luma)) * (lift * lift + 1.0);                 // `:1143`

    // THE VIEWER'S PUNCTUAL-LIGHT LOOP, `R114` (`readable:188-327`), driven by deren's `LightUBO` (the app stages
    // the captured Laevatain character-only rig into it for this preview). The capture runs it AFTER the
    // saturation lift (`:1140-1145` then `:1156`), which is exactly where this call sits.
    colour = game_toon_add_lights(colour, shading_normal, view_dir, world_pos, s.albedo, float3(0.04), s.roughness);
    // ... and the `!Q156` hair band is the capture's LAST addition after it (`readable:1172-1176`); it is zero
    // while the captured default hair-shine look (`game_toon_hair_shine_enabled = true`) is on.
    colour += hair_neg_q156_band;
    // No emission: the captured hair program has no emission arm (`_UseEmission = 0`; `Q158` is cloth-only).
    // No `R75 * Q164` depth-fade term: `Q185` is 0 in the capture (see the header).
    return colour;
}

#endif // DEREN_GAME_TOON_GAME_TOON_HAIR_SLANG
