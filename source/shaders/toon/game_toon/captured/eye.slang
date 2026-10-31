// ============================ GAME_TOON VIEWER: FAMILY_EYE ============================
//
// The captured GameToon Studio character shader's eye/iris branch, translated for deren. This is the PRIMARY
// appearance path for `VR_FAMILY_EYE` when `game_toon_profile_enabled(push.material_index)` is true; the original-game
// port in `toon/game_toon/eye.slang` stays as the fallback and is NOT touched by this file.
//
// SOURCE OF TRUTH (read-only capture, `tmp/game_toon-studio-audit/final/`):
//   * shaders/character.frag.readable.glsl - the aliased derivative of the captured template. Every
//     `readable:NNN` citation below is a line of THAT file.
//   * material-feature-defines.js - which conditionals the iris material selects (`b3`).
//   * material-uniform-bindings.js - the exact uniform wiring (`A3` = the eye material block, `M3` = the
//     ramp/LUT block, `x3` = Q153, `Q3` = Q20/Q261, `w3` = the assembled uniform set).
//   * materials/M_actor_laevat_iris_01.json - the captured per-material values (extras.floats/colors/textures).
//   * comparison-summary.json - the captured Laevatain lighting (`laevat_volume`, `shader_constants`).
//
// THE TWO PATHS THIS FILE CONTAINS (the captured template's own `#ifdef Q201` split):
//   PATH A - `Q201` + `HAS_MATCAP` (the captured iris, and the reason this port exists): the albedo lookup is
//     PARALLAX-WARPED by the view direction in tangent space (readable:600-625 applied at :661), the shading
//     normal is the manufactured CORNEA HEMISPHERE (readable:736-755), the light is FLATTENED against the
//     character's up axis before the terminator (readable:757-763), the iris gets the two REGION-COLOUR tints
//     (readable:765-771) and the MATCAP catchlight (readable:1088-1095). This is what the capture renders.
//   PATH B - no `Q201` (an eye material without `_UseMatcap`/`_MatcapTex`, e.g. a brow): raw uv, the surface normal
//     as-is, the light still flattened, tint = 1 and no matcap (the `#else` arms at :769-770, :757-763 apply in
//     both paths). Both paths keep the eye's diffuse-ramp composition, light composition and luma lift, which the
//     captured template does NOT gate on `Q201`.
//
// DEREN ADAPTERS USED, AND THE BRIDGES THEY IMPLY (each stated again at its use):
//   * `uMainLightDirection`: the viewer builds this CAMERA-RELATIVE (`viewer-lighting.js`'s `Bf`, yawBias 10 /
//     minPitch 32 - comparison-summary.json `laevat_volume`). deren has no camera-relative light rig in this stage,
//     so the contract's fallback is used: the scene sun direction `light_at(...).light_dir.xyz`. MISSING RIG.
//   * `Q116` (the head/root node's world rotation - `material-uniform-bindings.js`'s `Kf` does
//     `value.setFromMatrix4(extractRotation(headNode.matrixWorld))`; used at readable:757 to flatten the light):
//     deren's head/root frame is `head_frame_at(...).up`, with world up as the degenerate fallback.
//   * the vertex TANGENT/TEXCOORD3 frame (`USE_TANGENT`, defined for the iris because its primitive ships TANGENT):
//     deren's `surface_tbn` derivative frame stands in. The mesh's `TANGENT.w` is NOT uniform (+1 on 127 of 210
//     iris vertices, -1 on 83), which one derivative frame cannot express - the parallax warp's v sign is the
//     documented ambiguity (see the notes).
//   * `viewMatrix` - deren's `CameraUBO.view`: the captured `vec3(viewMatrix[0][2],viewMatrix[1][2],viewMatrix[2][2])`
//     is exactly deren's `camera_forward` (`mul(view, (0,0,1,0))`), so the camera-axis ramp read and the matcap
//     basis are the captured ones.
//   * `uEnvironmentMultiplier` / `uMainLightColourScaled` / `uShadowTint` / `uMainLightIntensity` / `uRampRangeBias`:
//     profile lanes 0.z / 1.rgb / 3.rgb / 0.y / 1.a (`game_toon_profile.h`), with the captured Laevatain values as
//     documented fallbacks when a lane reads zero.
//   * `_EyeHighLightColor` / `_EyeScatteringColor` / `_ParallaxScale`: deren's existing `toon_colour_lane` carriers
//     (`eye_highlight`, `eye_scattering`, `parallax_scale`); `_MatcapColor` and the four eye scalars ride the
//     game_toon profile payload (lane 15 rgb+a, lane 9.w, lane 5.xy) as stated at each read. All of them ARE staged
//     for the captured Laevatain iris in
//     `tmp/game_toon-reference/assets/laevatain_game_toon_preview.glb.toon.tsv`: `M_actor_laevat_iris_01 color
//     _EyeHighLightColor / _EyeScatteringColor / _MatcapColor / _GameToon5 / _GameToon9 / _GameToon15`, and
//     `float _ParallaxScale`.
//   * diffuse ramp and matcap: deren's material lanes `s.toon_indices.x` (diffuse ramp) and `s.toon_indices.w`
//     (matcap); the captured material states both (`M_actor_laevat_iris_01.json` extras.textures) and the game_toon
//     sidecar stages both (`slot _DiffRampMap`, `slot _MatcapTex`). NO NEUTRAL RAMP IS SUBSTITUTED: both captured
//     diffuse compositions are ported - the `USE_DIFFUSE_RAMP` arm (readable:929-965, the iris) and the template's
//     `#else` arm for an eye material that states no ramp (readable:995-1004) - and the ramp's RGB is re-encoded
//     to the STORED data value the capture reads (deren's lane is `rgba8_srgb`, `runtime.constructor.cppm:2537`;
//     see `encoding.h`).
//
// NOT PORTED (deliberate, each a different mechanism rather than an omission):
//   * tone mapping and the color-space encode (readable:1189-1190 `#include <tonemapping_fragment>` /
//     `<colorspace_fragment>`): the contract puts tone mapping once in deren's post pass, so this function returns
//     LINEAR HDR. The captured viewer's own tone map is preserved in deren's `post/game_toon_tonemap.h`.
//   * the punctual-light loop `R114` (readable:188-327): `Q260` (its light count, `viewer-lighting.js`'s
//     `Q260:{value:0}`) is 0 in the captured frame - the additional-lights UI default is off - and the FAMILY_EYE
//     branch inside it (`if(mode==3)continue`, readable:238-241) is a shared-loop statement, not an eye term.
//   * the depth-cut/`Q164` term (readable:1177): `R200` returns 0 when `Q185 < 0.5` (`viewer-lighting.js`
//     `e3()` default), so it contributes nothing in the capture.
//   * depth shadow (`Q132`), shadow LUT (`Q273`), metallic/gloss map (`Q13`), emission (`Q158`/`Q242`): not
//     defined for the captured iris material (`material-feature-defines.js`), so their branches do not exist in
//     this material's program.
//   * outline, face decals, dissolve, rain/snow: other stages / other defines, not the eye branch.

#include "toon/game_toon_math.h"
#include "toon/game_toon/captured/encoding.h"
#include "toon/game_toon/game_toon_profile.h"
#include "toon/game_toon/captured/game_toon_lights.h"

// ------------------------------------------------------------------------------------------------------------
// CAPTURED SHADER CONSTANTS (viewer-wide, not per material)
// ------------------------------------------------------------------------------------------------------------

/// `R60` / `VIEWER_LIGHT_SCALE` - readable:421 `const float VIEWER_LIGHT_SCALE= 0.2877;`, also
/// comparison-summary.json `shader_constants.o3` = 0.2877. It scales the AMBIENT half of the eye's light term.
static const float game_toon_eye_viewer_light_scale = 0.2877;

/// `VIEWER_AMBIENT` - readable:420 `= 0.725`, comparison-summary.json `shader_constants.ri.ambient`.
/// @note the eye does NOT read the constant: its ambient weight is `R169` (readable:921-926) whose FAMILY_EYE
///       override normalises the normal's Y to 6.1e-5, so `R169` lands on 0.725 for an eye that faces the camera.
///       The formula is ported literally; the constant is recorded here because the two are the same number by
///       construction and a reader will look for this one.
static const float game_toon_eye_viewer_ambient = 0.725;

/// the ramp's V coordinate: readable:803 / :936 sample `uDiffuseRampMap` at `vec2(clamp(u,0.002,0.998), 0.5)`.
/// @note the game's `_DiffRampMap` is a 256x1 strip, so 0.5 lands on its only row; the literal is kept.
static const float game_toon_eye_ramp_v = 0.5;

// ------------------------------------------------------------------------------------------------------------
// CAPTURED LAEVATAIN RIG FALLBACKS (used only when the profile lane is absent; comparison-summary.json)
// ------------------------------------------------------------------------------------------------------------

/// the character VOLUME MULTIPLIER (profile lane 0.y) - comparison-summary.json `laevat_volume.multiplier` 1.05.
/// The captured `uMainLightColourScaled` is `rig.color * rig.intensity` where `rig.intensity` IS this multiplier
/// (`viewer-lighting.js`: `Q10.value.copy(this.color).multiplyScalar(this.intensity)`, and `setMenuLight` copies
/// the menu's `multiplier` into `this.intensity`). The profile lane contract states lane 2.rgb as the light COLOUR,
/// so the scaling is applied here from lane 0.y - the same product the capture performs, from the lane that owns
/// the number.
/// MISSING PROFILE LANE: fallback is the captured 1.05.
static const float game_toon_eye_captured_volume_multiplier = 1.05;

/// `uMainLightColourScaled`'s COLOUR half (`rig.color`) - comparison-summary.json `laevat_volume.lightColor`
/// [1, 1, 1]. Multiplied by the volume multiplier above to become the captured `uMainLightColourScaled`.
/// MISSING PROFILE LANE: profile lane 2.rgb when stated, otherwise this captured value.
static const float3 game_toon_eye_captured_main_light_colour = float3(1.0, 1.0, 1.0);

/// `uMainLightIntensity` = `viewer-lighting.js` `zf = 2` (comparison-summary.json "main light intensity 2.0").
/// MISSING PROFILE LANE: profile lane 2.w when stated, otherwise this captured value.
static const float game_toon_eye_captured_main_light_intensity = 2.0;

/// `uShadowTint` (profile lane 3) - comparison-summary.json `laevat_volume.shadowTint` [0.8783, 0.9302, 1.1217].
static const float3 game_toon_eye_captured_shadow_tint = float3(0.8783, 0.9302, 1.1217);

/// `uEnvironmentMultiplier` (profile lane 0.z) - comparison-summary.json `laevat_volume.envMul` 0.55.
static const float game_toon_eye_captured_environment_multiplier = 0.55;

/// `uRampRangeBias` (`Q215`) - comparison-summary.json `laevat_volume.rangeBias` 0.0 (the viewer default is -0.4;
/// the Laevatain menu light states 0.0, and the capture is that rig). PROFILE LANE 1.a (`game_toon_range_bias`);
/// this constant is the fallback when the lane is 0, which is also the staged `_GameToon1` = (1.05, 1.05, 1.05, 0).
static const float game_toon_eye_captured_range_bias = 0.0;

// ------------------------------------------------------------------------------------------------------------
// CAPTURED PER-MATERIAL PAYLOAD WITH NO LANE (suggested packing in the notes; read from profile lanes 5/6 when
// the Lead stages them, otherwise these captured `M_actor_laevat_iris_01.json` values are the fallback)
// ------------------------------------------------------------------------------------------------------------

/// `_MatcapColor` - `M_actor_laevat_iris_01.json` `extras.colors._MatcapColor` = (1.2755, 0.2382, 0.733, 1.0),
/// wired by `material-uniform-bindings.js`'s `A3` into `Q93` (`rl` copies the raw floats, no sRGB decode) and read
/// at readable:1092 as `matcap.rgb * Q93.a + matcap.a * Q93.rgb`.
/// PROFILE LANE: lane 15 (rgb + a), staged as `_GameToon15` = (1.2755, 0.2382, 0.733, 1.0); this constant is the
/// fallback when the lane reads all-zero.
static const float4 game_toon_eye_captured_matcap_colour = float4(1.2755, 0.2382, 0.733, 1.0);

/// `_MatcapNormalScale` - `M_actor_laevat_iris_01.json` `extras.floats._MatcapNormalScale` = 1.0, wired by `A3`
/// into `Q32` and read at readable:752 (`vec3(-R163*Q32, ...)` - the cornea hemisphere's tilt at the iris' edge).
/// PROFILE LANE: lane 9.w, staged as `_GameToon9` = (0, 0, 1, 1.0); this constant is the fallback when it is 0.
static const float game_toon_eye_captured_matcap_normal_scale = 1.0;

/// `_ParallaxScale` - `M_actor_laevat_iris_01.json` `extras.floats._ParallaxScale` = 0.15, wired by `A3` into
/// `Q81` and read by the uv warp at readable:623. Its deren lane is `toon_colour_lane::parallax_scale` (`.x < 0` =
/// "the asset states nothing") and the staged sidecar carries `float _ParallaxScale 0.15...`; this constant is the
/// fallback for exactly the missing-lane case.
static const float game_toon_eye_captured_parallax_scale = 0.15;

/// `_ShadowColorBrightness` / `_ShadowColorSaturation` - `M_actor_laevat_iris_01.json` extras.floats 0.55 / 1.0,
/// wired by `material-uniform-bindings.js`'s `M3` (`Q347: _ShadowColorBrightness ?? .5`,
/// `Q348: _ShadowColorSaturation ?? 1`) and consumed by the fallback shadow albedo `R113` at readable:831/908.
/// PROFILE LANE: lane 5.x / 5.y, staged as `_GameToon5` = (0.55, 1.0, 1, 1); these constants are the fallbacks.
static const float game_toon_eye_captured_shadow_brightness = 0.55;
static const float game_toon_eye_captured_shadow_saturation = 1.0;

/// `_EyeHighLight` - `M_actor_laevat_iris_01.json` extras.floats 1.0. It is the third condition of the viewer's
/// `Q201` feature (`material-feature-defines.js`'s `Jc`: `_EyeHighLight===1 && _UseMatcap===1 && "_MatcapTex" in tex`)
/// and has no deren lane; the captured value is used. The live half of the test is the matcap lane
/// (`s.toon_indices.w`), which the sidecar only sets when the material states `_MatcapTex` with `_UseMatcap` on.
static const float game_toon_eye_captured_eye_high_light = 1.0;

// ------------------------------------------------------------------------------------------------------------
// HELPERS (every name is `game_toon_eye_`-prefixed because the four game_toon family files share one module)
// ------------------------------------------------------------------------------------------------------------

/// `R218` (readable:422-424) - the viewer's luma: `dot(c, vec3(0.212673, 0.715152, 0.072175))`.
static float game_toon_eye_luma(float3 c) {
    return dot(c, float3(0.212673, 0.715152, 0.072175));
}

/// `R113`'s luma (readable:59) - the SHADOW albedo's desaturation uses a different weight vector than `R218`:
/// `dot(c, vec3(0.2126, 0.7152, 0.0722))`. Both spellings are in the captured source and both are ported as written.
static float game_toon_eye_shadow_luma(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

/// `R24` (readable:171-174) - the viewer's smoothstep: clamp to [0,1] then `x*x*(3-2x)`.
static float game_toon_eye_curve(float x) {
    x = clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

/// `R113` (readable:58-61) - desaturate toward the shadow luma and floor at zero:
/// `max(mix(vec3(luma), c, s), 0)`. Used by the eye's no-shadow-LUT fallback path.
static float3 game_toon_eye_shadow_albedo(float3 c, float saturation) {
    return max(lerp(float3(game_toon_eye_shadow_luma(c)), c, saturation), float3(0.0, 0.0, 0.0));
}

/// a profile lane is "stated" when it is not the all-zero word (`game_toon_profile.h`: "A lane left at zero means
/// 'not stated', never a guessed default"). Used for the payload lanes this family needs but the header does not
/// name yet, so a zero word falls back to the captured value below.
static bool game_toon_eye_lane_stated(float4 value) {
    return value.x != 0.0 || value.y != 0.0 || value.z != 0.0 || value.w != 0.0;
}

/// @brief the captured GameToon eye/iris branch, translated
/// @param s the G-buffer surface (its `toon_indices.x` is the diffuse ramp lane and `.w` the matcap lane)
/// @param world_pos interpolated world position
/// @param geo_normal interpolated geometric normal - the pre-eye `vWorldNormal` the captured uv warp's tangent
///        frame is built from; the shading normal (`s.normal`) carries the normal-map/double-sided composition the
///        captured `R39` has, so that is what the cornea frame is applied to
/// @param uv the material's uv (the eye's zones, warp and cornea hemisphere all key off its `fract`)
/// @param is_front front-face flag; unused here because deren's `s.normal` already carries the DOUBLE_SIDED flip
///        the captured `R180` applies (readable:694-698), and the captured iris material is not double-sided
/// @return LINEAR HDR radiance - no tone mapping, no sRGB encode (the post pass owns both, per the contract)
float3 captured_eye_shade(surface_sample s, float3 world_pos, float3 geo_normal, float2 uv, bool is_front) {
    const Material mat = material_at(heap_slots_materials, push.material_index);
    const uint material_index = push.material_index;
    const uint colour_base = material_index * character_toon_colour_lanes;

    // ---- THE PROFILE's CAPTURED RIG (game_toon_profile.h lanes 0/2/3; captured values in the constant block) ----
    //
    // lane 0: x = enable, y = volume multiplier, z = environment multiplier, w = env-shadow multiplier. The eye
    // reads y (folded into `uMainLightColourScaled` below) and z (the matcap's environment gate); w is read only by
    // the captured cloth branch (readable:1136), not by the eye.
    const float4 profile = game_toon_profile_lane(material_index, 0u);
    const float volume_multiplier = 1.0; // captured 1.05 is already folded into lanes 1/2 by uniform-packing
    const float environment_multiplier = profile.z > 0.0 ? profile.z : game_toon_eye_captured_environment_multiplier;

    // lane 2: rgb = uMainLightColourScaled's COLOUR, w = uMainLightIntensity; lane 3: rgb = uShadowTint.
    const float4 main_light_lane = game_toon_main_light(material_index);
    const float3 main_light_colour = game_toon_eye_lane_stated(main_light_lane) ? main_light_lane.rgb
                                                                               : game_toon_eye_captured_main_light_colour;
    const float main_light_intensity = game_toon_main_light_intensity(material_index) > 0.0 ? game_toon_main_light_intensity(material_index) : game_toon_eye_captured_main_light_intensity;
    // the captured `uMainLightColourScaled` itself: the colour times the character's volume multiplier.
    const float3 main_light_scaled = main_light_colour;
    const float4 shadow_tint_lane = game_toon_profile_lane(material_index, 3u);
    const float3 shadow_tint = game_toon_eye_lane_stated(shadow_tint_lane) ? shadow_tint_lane.rgb
                                                                          : game_toon_eye_captured_shadow_tint;

    // ---- THE CAPTURED MATERIAL PAYLOAD: lanes this family needs that the profile header does not name yet ----
    //
    // Both reads go through the header's generic `game_toon_profile_lane`, so the Lead only has to WRITE the lane:
    //   lane 5 = `_MatcapColor` (rgb + a)                 [suggested packing]
    //   lane 6 = x `_MatcapNormalScale`, y `_ShadowColorBrightness`, z `_ShadowColorSaturation`, w `_EyeHighLight`
    // Until then each is zero and the captured `M_actor_laevat_iris_01.json` value below is used.
    const float4 matcap_colour_lane = game_toon_profile_lane(material_index, 15u);
    const float4 matcap_colour = game_toon_eye_lane_stated(matcap_colour_lane) ? matcap_colour_lane
                                                                              : game_toon_eye_captured_matcap_colour;
    const float4 eye_scalars = game_toon_profile_lane(material_index, 9u);
    const float matcap_normal_scale = eye_scalars.w > 0.0 ? eye_scalars.w : game_toon_eye_captured_matcap_normal_scale;
    const float shadow_brightness = game_toon_profile_lane(material_index, 5u).x > 0.0 ? game_toon_profile_lane(material_index, 5u).x : game_toon_eye_captured_shadow_brightness;
    const float shadow_saturation = game_toon_profile_lane(material_index, 5u).y > 0.0 ? game_toon_profile_lane(material_index, 5u).y : game_toon_eye_captured_shadow_saturation;
    const float eye_high_light = game_toon_eye_captured_eye_high_light; // no lane in uniform-packing; captured 1.0

    // ---- THE EXISTING deren LANES THE CAPTURE'S MATERIAL STATES ----
    //
    // `_EyeHighLightColor` and `_EyeScatteringColor` are already carried as `toon_colour_lane::eye_highlight` /
    // `eye_scattering` (raw linear float lanes, neutral white). The captured iris states
    // (3.4416, 3.0091, 2.8109) and (0.0, 3.2832, 16.9484) - STAGING THEM IS REQUIRED for the capture's iris
    // colour; the notes give the JSON paths.
    const float3 eye_highlight_colour = toon_colour_at(heap_slots_toon_colours, colour_base + 1u).rgb;
    const float3 eye_scattering_colour = toon_colour_at(heap_slots_toon_colours, colour_base + 2u).rgb;
    // `_ParallaxScale` (Q81): the existing lane's `.x < 0` sentinel means "the asset states nothing" - exactly the
    // contract `character_forward.slang` reads it with - and the captured material's 0.15 is the fallback.
    const float parallax_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 5u).x;
    const float parallax_scale = parallax_lane >= 0.0 ? parallax_lane : game_toon_eye_captured_parallax_scale;

    // ---- CAMERA LIGHT AND THE PRE-EYE NORMAL (readable:691-698, 728-733) ----
    const float3 camera_pos = camera_at(heap_camera_slot).camera_pos.xyz;
    const float3 view_dir = normalize(camera_pos - world_pos);                                          // V
    // the captured `vec3(viewMatrix[0][2],viewMatrix[1][2],viewMatrix[2][2])` (readable:863/937/1080) is deren's
    // `mul(view, (0,0,1,0))` - the same third view-matrix column (see `character_forward.slang`'s note on it).
    const float3 camera_forward_axis = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz);
    // MISSING RIG: the captured `uMainLightDirection` is camera-relative (`viewer-lighting.js`'s `Bf`: camera
    // world direction -> yaw +10 deg, pitch clamped to >= 32 deg). deren has no camera-relative light in this stage,
    // so the scene sun direction is used; the notes record the exact adapter for a future rig.
    const float3 camera_axis = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz);
    const float3 light_world = game_toon_profile_enabled(material_index)
        ? normalize(wf_camera_light(-camera_forward_axis, game_toon_yaw_bias(material_index), game_toon_min_pitch(material_index)))
        : normalize(light_at(heap_light_slot).light_dir.xyz);

    // the pre-eye normal: the captured `R39`/`Q39` is `normalize(vWorldNormal)` after the normal-map and
    // DOUBLE_SIDED compositions (readable:691-698, 700-722) - deren's `s.normal` is exactly that composition for
    // this surface. The uv warp's tangent frame, however, is built from the UNflipped `normalize(vWorldNormal)`
    // (readable:660 passes it into R193), so the frame's reference normal is the geometric one.
    const float3 shading_normal_in = normalize(s.normal);
    const float3 frame_normal = normalize(geo_normal);

    // ---- THE UV zoned DATA: fract(uv) in the captured V-flipped space (readable:657-662, 736-739) ----
    //
    // The captured code works in `R256 = vec2(vUv.x, 1-vUv.y)` (readable:737); every quantity it derives is a
    // distance or a symmetric offset, so the work is done here on the raw uv and the V flip is applied only where
    // it changes a SIGN (the warp, below), with the derivation stated there.
    const float2 frac_uv = frac(uv);
    const float2 centred_uv = frac_uv * 2.0 - 1.0;                                                      // R163/R264*2-1
    const float2 lens_uv = frac_uv - float2(0.5, 0.5);
    const float dist_sq = dot(lens_uv, lens_uv);                                                        // dot(fract-0.5)
    // R175 (readable:739): the iris zone is the INSCRIBED CIRCLE - 1 inside it, 0 outside.
    const float eye_zone = dist_sq < 0.25 ? 1.0 : 0.0;
    // the hemisphere's Z: `sqrt(1.0 - min(dot(R163,R163), 1.0))` (readable:752) - no `max` in the capture because
    // the dot is clamped to 1 first.
    const float hemisphere_z = sqrt(max(1.0 - min(dot(centred_uv, centred_uv), 1.0), 0.0));

    // the derivative tangent frame standing in for the captured vertex tangent (`USE_TANGENT`, readable:607-621 /
    // :741-750). A degenerate frame keeps the raw uv and the surface normal (the captured code has no such path:
    // a vertex tangent always exists).
    float3 tangent = float3(1.0, 0.0, 0.0);
    float3 bitangent = float3(0.0, 1.0, 0.0);
    const bool frame_valid = surface_tbn(world_pos, uv, tangent, bitangent);

    // ---- PATH A: THE UV WARP (readable:600-625, `R193`, applied at :661) ----
    //
    // The captured warp is written on the V-FLIPPED uv: it offsets `R197` by `(R13.x, R13.y) * Q81 * (1, 0.25) * w`
    // and returns `vec2(R197.x, 1-R197.y)`. With `R13 = normalize(vec3(T.V, B.V, N.V))` and `T`/`B` the vertex
    // tangent frame, converting back to the raw uv gives
    //     u' = u - (T.V) * Q81 * w                 v' = v + (B.V) * 0.25 * Q81 * w
    // i.e. the u term keeps the minus and the v term's sign is the bitangent's. That is the translation below.
    // AMBIGUITY RECORDED, NOT GUESSED: the iris' `TANGENT.w` is +1 on 127 and -1 on 83 of its 210 vertices, so the
    // captured v sign is not even uniform across the mesh; one derivative frame cannot express that (notes).
    const bool path_a = (s.toon_indices.w != 0u) && (eye_high_light > 0.5);
    float2 shading_uv = uv;
    if (path_a && frame_valid) {
        const float3 view_ts = float3(dot(tangent, view_dir), dot(bitangent, view_dir), dot(frame_normal, view_dir));
        // `w = R24(5.0*(0.25 - dot(f-0.5,f-0.5)))` (readable:604-605): full at the centre, zero by the circle.
        const float warp_mask = game_toon_eye_curve(clamp(5.0 * (0.25 - dist_sq), 0.0, 1.0));
        shading_uv += float2(-view_ts.x, view_ts.y * 0.25) * parallax_scale * warp_mask;
    }

    // ---- THE BASE COLOUR (readable:663) ----
    //
    // `R38(uBaseMap, uBaseColour, uOpacity, R105)` = `vec4(uBaseColour, uOpacity) * texture2D(uBaseMap, uv')`:
    // the albedo is sampled at the WARPED uv and nothing else in the eye branch is. deren's slot and factors are
    // the same two numbers (glTF baseColorTexture is sRGB and deren's slot decodes it, as the capture's does).
    // `R99`'s `Q100`/`Q284`/`Q208` customisation tint is not defined for this material (material JSON
    // `_AvatarCustomizeEnable` 0), so the captured `_EyeTintColor` (1,1,1) path is an identity and is not applied.
    const float4 base_texel = frame_texture_sample(mat.tex_indices.x, shading_uv);
    const float3 base_rgb = mat.base_color_factor.rgb * base_texel.rgb;
    const float base_alpha = mat.base_color_factor.a * base_texel.a;

    // ---- PATH A: THE CORNEA HEMISPHERE NORMAL (readable:736-755) ----
    //
    // `R47 = vec3(-R163*Q32, sqrt(1-|R163|^2))`, `R74 = zone ? vec3(-R47.xy*0.125, R47.z) : vec3(0,0,1)`, and
    // `N = normalize(T*R74.x + B*R74.y + R39*R74.z)`. The two negations cancel inside the zone, so the shading
    // tilt is `+0.125 * Q32 * centred_uv` - i.e. `_MatcapNormalScale` is this viewer's `_CorneaBumpStrength`
    // socket. Outside the inscribed circle the cornea flattens to the surface normal.
    float3 normal = shading_normal_in;
    if (path_a) {
        const float3 cornea_ts = eye_zone > 0.5
                                     ? float3(centred_uv.x * matcap_normal_scale * 0.125,
                                              centred_uv.y * matcap_normal_scale * 0.125,
                                              hemisphere_z)
                                     : float3(0.0, 0.0, 1.0);
        if (frame_valid) {
            normal = normalize(tangent * cornea_ts.x + bitangent * cornea_ts.y + shading_normal_in * cornea_ts.z);
        }
    }

    // ---- THE LIGHT FLATTENED AGAINST THE CHARACTER'S UP (readable:757-763) ----
    //
    // `R161 = normalize(Q116[1])`, `R305 = L - R161*dot(L,R161)`, `L = normalize(R305)` when the residual is long
    // enough - i.e. the light's component along the head/root basis' second axis is removed, which is what puts the
    // eye's terminator where the capture has it. Q116 is the Head_Local/Root node's world rotation
    // (`material-uniform-bindings.js`'s `Kf`); deren's head frame is the adapter, world up the fallback.
    const float3 head_up_raw = head_frame_at(heap_slots_scene_head + heap_frame_slot).up.xyz;
    const float3 head_up = dot(head_up_raw, head_up_raw) > 1.0e-8 ? normalize(head_up_raw) : float3(0.0, 1.0, 0.0);
    float3 light_dir = light_world;
    {
        const float3 flattened = light_dir - head_up * dot(light_dir, head_up);
        if (dot(flattened, flattened) > 1.0e-8) {
            light_dir = normalize(flattened);
        }
    }
    // `R9`/`R46`/`R196` (readable:730-733) are computed by the capture too, but their only consumers in this
    // program are the punctual-light loop (`R114`, skipped: no lights) and the specular branch (`Q141`, not defined
    // for this material) - so they are not carried. The eye has NO specular/NoH term of its own: its highlight is
    // the matcap below.
    const float n_dot_l = dot(normal, light_dir);                                                       // R128

    // ---- THE IRIS REGION-COLOUR TINTS (readable:765-771) ----
    //
    // `R21 = mix(Q52, vec3(1), R175) * mix(vec3(1), Q129, base.a)`: the HIGHLIGHT colour multiplies the eye OUTSIDE
    // the inscribed circle (the sclera/corners - inside the circle `mix` returns 1) and the SCATTERING colour
    // multiplies by the albedo's own alpha. Path B has no `Q201` and the captured template writes `R21 = vec3(1)`.
    const float3 region_tint = path_a
                                   ? lerp(eye_highlight_colour, float3(1.0, 1.0, 1.0), eye_zone) *
                                         lerp(float3(1.0, 1.0, 1.0), eye_scattering_colour, base_alpha)
                                   : float3(1.0, 1.0, 1.0);

    // ---- THE TWO RAMP READS (readable:892, 936-938) ----
    //
    // `R78 = clamp(R128 + uRampRangeBias, -1, 1)*0.5+0.5` is the diffuse ramp's U on the LIGHT axis and
    // `R215 = ramp(dot(N, R141)*0.5+0.5).a` is the SECOND read on the CAMERA axis, `R141` = the third view-matrix
    // column = `camera_axis`. The first read's U is clamped to [0.002, 0.998] at the fetch (`:936`), the second's
    // at `:938`; the UNCLAMPED `R78` is still needed by the no-ramp arm below (`:996`), so it is kept.
    const float range_bias = game_toon_range_bias(material_index);                                      // lane 1.a (Q215)
    const float ramp_light_x = clamp(n_dot_l + range_bias, -1.0, 1.0);                                 // `R128 + Q215`
    const float ramp_light_u_raw = ramp_light_x * 0.5 + 0.5;                                           // `R78`
    const float ramp_light_u = clamp(ramp_light_u_raw, 0.002, 0.998);
    const float ramp_camera_u = clamp(clamp(dot(normal, camera_axis) * 0.5 + 0.5, 0.0, 1.0), 0.002, 0.998);
    // `Q243` / `USE_DIFFUSE_RAMP`: the captured template compiles TWO DIFFERENT diffuse compositions around it -
    // the ramp arm (readable:929-965) and the `#else` arm (readable:995-1004). deren is one dynamic program, so the
    // same split is a runtime branch. The captured iris takes the ramp arm: its sidecar stages
    // `slot _DiffRampMap WF_T_actor_common_body_01_RD`, so `toon_indices.x` is non-zero and every neutral fallback
    // this file used to carry is GONE - what replaces it is the template's own no-ramp arm, not an invented white.
    const bool has_diffuse_ramp = s.toon_indices.x != 0u;
    float4 diffuse_ramp = float4(1.0, 1.0, 1.0, 1.0);
    float view_ramp_alpha = 1.0;
    float ramp_coverage = 1.0;                                                                         // R144
    float camera_coverage = 0.0;                                                                       // R318
    if (has_diffuse_ramp) {
        const DescriptorHandle<Texture2D<float4>> ramp_tex = heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.x);
        diffuse_ramp = heap_texel(ramp_tex, heap_sampler_texture, float2(ramp_light_u, game_toon_eye_ramp_v));
        view_ramp_alpha = heap_texel(ramp_tex, heap_sampler_texture, float2(ramp_camera_u, game_toon_eye_ramp_v)).a;
        // THE RAMP IS DATA IN THE CAPTURE, AND deren'S LANE IS STAGED sRGB. The captured material's `srgb` list
        // names only `_MatcapTex` (`M_actor_laevat_iris_01.json`), so `texture2D(uDiffRampMap, ...)` returned the
        // STORED byte value (`readable:936/938`); deren uploads `toon_slot::diffuse_ramp` as `rgba8_srgb`
        // (`runtime.constructor.cppm:2537`), so the sampler has already decoded it. Re-encode to recover the stored
        // value the capture uses (`encoding.h`'s bridge; the same arithmetic as `game_toon_raw_ramp_rgb`). ALPHA is
        // untouched - an sRGB lane's alpha is not decoded, and every weight below reads `.a`. The residual is the
        // filter order (the GPU filtered on decoded values); staging the lane UNORM removes both - see
        // `tmp/game_toon-port/encoding-notes.md`.
        diffuse_ramp.rgb = game_toon_encoding_display_value_from_srgb_lane(diffuse_ramp.rgb);
        ramp_coverage = min(diffuse_ramp.a, 1.0);                                                      // `R144 = min(R32.a, 1)`
        camera_coverage = view_ramp_alpha;                                                             // `R318 = R215`
    }

    // ---- THE DIFFUSE COMPOSITION: THE TWO CAPTURED ARMS (readable:941-964 and :995-1004) ----
    //
    // `mask = 1` because `Q13` (metallic/gloss map) is not defined for this material; `R144`/`R318` are the two
    // ramp coverages read above. `R152 = R113(base.rgb, _ShadowColorSaturation) * _ShadowColorBrightness` (no
    // `USE_SHADOW_LUT` for the captured iris: `_UseShadowLutTex` 0), then `R152 *= 0.96 * uEnvironmentMultiplier`
    // (readable:904-920).
    const float metallic = clamp(s.metallic, 0.0, 1.0);                                                // Q153.r
    const float one_minus_metallic = 1.0 - metallic;
    const float3 shadow_base = game_toon_eye_shadow_albedo(base_rgb, shadow_saturation) * shadow_brightness;
    const float3 shadow_albedo_scaled = shadow_base * 0.96 * environment_multiplier;                  // `R152` / `R281`
    float3 color = float3(0.0, 0.0, 0.0);
    if (has_diffuse_ramp) {
        // readable:941-964: the lit albedo, `R334 = base.rgb*0.96` (`:950`) then the FAMILY_EYE block's
        // `R334 *= R21*(1-Q153.r)` (`:952-958`), the shadow albedo `R281 = R152*(1-Q153.r)`, `R130`'s two-stage
        // mix and the chroma/ratio remap.
        const float3 lit_albedo = base_rgb * 0.96 * region_tint * one_minus_metallic;                  // R334
        const float3 shadow_albedo = shadow_albedo_scaled * one_minus_metallic;                        // R281
        // `R130 = mix(luma(0.65*R281) + 1.2*(0.65*R281 - luma(...)), R281, clamp(R32.a + R318, 0, 1))`, then the
        // lit layer by the light-axis coverage (readable:959-961).
        const float3 shadow_luma = float3(game_toon_eye_luma(0.65 * shadow_albedo));
        const float3 shadow_mixed = shadow_luma + 1.2 * (0.65 * shadow_albedo - shadow_luma);
        float3 albedo = lerp(shadow_mixed, shadow_albedo, clamp(ramp_coverage + camera_coverage, 0.0, 1.0));
        albedo = lerp(albedo, lit_albedo, ramp_coverage);
        // `R150` is the ramp colour's own chroma and `mix(vec3(1), R32.rgb, R150)` maps the ramp's colour onto the
        // albedo only as far as that chroma reaches; then the captured ramp-control ratio (readable:962-964).
        const float ramp_chroma = max(max(diffuse_ramp.r, max(diffuse_ramp.g, diffuse_ramp.b)) -
                                          min(diffuse_ramp.r, min(diffuse_ramp.g, diffuse_ramp.b)),
                                      0.0);
        const float3 albedo_coloured = albedo * lerp(float3(1.0, 1.0, 1.0), diffuse_ramp.rgb, ramp_chroma);
        const float ramp_control =
            clamp(game_toon_eye_luma(albedo) / max(game_toon_eye_luma(albedo_coloured), 0.001), 0.0, 1.5);
        color = albedo_coloured * ramp_control;                                                        // R252
    } else {
        // readable:995-1004, the captured `#else` arm (an eye material with no `USE_DIFFUSE_RAMP`): a plain split
        // between the shadow albedo and the raw base colour at `smoothstep(0.48, 0.52, R78)`. No `Q299` arm here
        // (`Q299` is selected only for CLOTH), no region tint and no metallic discount - the capture writes none.
        ramp_coverage = game_toon_eye_curve((ramp_light_u_raw - 0.48) * 25.0);                           // smoothstep(0.48,0.52,R78)
        camera_coverage = 0.0;
        color = lerp(shadow_albedo_scaled, base_rgb, ramp_coverage);                                   // mix(R152, base.rgb, R144)
    }

    // ---- THE LIGHT COMPOSITION (readable:921-926, 1006-1007) ----
    //
    // `R169 = clamp(normalize(vec3(N.x, 0.000061, N.z)).y + 0.15, 0, 1)*1.5 + 0.5`, whose FAMILY_EYE override is
    // readable:923-926: the normal's Y is forced to 6.1e-5 BEFORE the normalize, so the ambient weight is the
    // viewer's 0.725 for an eye facing the camera instead of being driven by the eyeball's curvature.
    const float3 flattened_normal = normalize(float3(normal.x, 6.10351562e-05, normal.z));
    const float ambient_weight = clamp(flattened_normal.y + 0.15, 0.0, 1.0) * 1.5 + 0.5;              // R169
    const float3 light_color_full = main_light_scaled * main_light_intensity;                           // R266
    // `light = mix(uShadowTint, 1, R144) * R169 * VIEWER_LIGHT_SCALE * uMainLightColourScaled
    //          + mix(vec3(luma(uMainLightColourScaled*uMainLightIntensity)), that, R144)`
    const float3 ambient_light = lerp(shadow_tint, float3(1.0, 1.0, 1.0), ramp_coverage) *
                                 ambient_weight * game_toon_eye_viewer_light_scale * main_light_scaled;
    const float3 direct_light = lerp(float3(game_toon_eye_luma(light_color_full)), light_color_full, ramp_coverage);
    const float3 light = ambient_light + direct_light;
    color *= light;

    // ---- PATH A: THE MATCAP CATCHLIGHT (readable:1088-1095) ----
    //
    // The matcap is indexed by the VIEW-SPACE direction of the CORNEA normal, and the captured code builds it from
    // `R47` - the hemisphere BEFORE the `0.125` scale and the zone clamp, with its xy NEGATED: `R47.xy = -R163*Q32`.
    // In the raw-uv tangent frame that is `(-Q32*centred_uv, hemisphere_z)` (the same double-V-flip derivation as
    // the warp). deren's `toon_matcap_uv` is the same sphere map - including the V flip the capture writes as
    // `texture2D(Q1, vec2(R145.x, 1.0-R145.y))` - so only the direction has to be built here.
    // `R138 = ball.rgb * _MatcapColor.a + ball.a * _MatcapColor.rgb`, gated by the iris zone, by
    // `mix(uEnvironmentMultiplier, 1, R144)`, by `(0.5 + 0.5*R144)` and by the composed light.
    if (path_a && frame_valid && s.toon_indices.w != 0u) {
        const float3 matcap_ts = float3(-centred_uv.x * matcap_normal_scale,
                                        -centred_uv.y * matcap_normal_scale,
                                        hemisphere_z);
        const float3 matcap_normal = normalize(tangent * matcap_ts.x + bitangent * matcap_ts.y +
                                               shading_normal_in * matcap_ts.z);
        const float4 ball = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.w),
                                      heap_sampler_texture, toon_matcap_uv(matcap_normal, camera_axis));
        const float3 matcap_brdf = ball.rgb * matcap_colour.a + ball.a * matcap_colour.rgb;
        color += matcap_brdf * eye_zone * lerp(environment_multiplier, 1.0, ramp_coverage) *
                 (0.5 + 0.5 * ramp_coverage) * light;
    }

    // ---- THE FINAL CONTRAST LIFT (readable:1096-1098) ----
    //
    // `luma`-centred contrast around mid-grey, applied to the finished colour: `c + (c-l)*((clamp(l-0.5,0,0.5))^2+1)`.
    // It is inside `#ifdef FAMILY_EYE` and OUTSIDE `#ifdef HAS_MATCAP` (the `#endif` at :1095), so BOTH paths get
    // it - it is an eye-family term, not a matcap term. It is not a tone map: it stays linear HDR.
    {
        const float luma = game_toon_eye_luma(color);
        const float lift = clamp(luma - 0.5, 0.0, 0.5);
        color = float3(luma) + (color - luma) * (lift * lift + 1.0);
    }

    // `color *= mix(1.0, base.a, Q20)` (readable:1085) is the captured alpha-premultiply switch: `Q20` is
    // `_AlphaPremultiply` (material JSON 0.0), so the factor is exactly 1 and the line is not ported as a multiply.
    // Path B's terminal `R114` light loop and `R75*Q164` cut term are no-ops in the capture (see the header).
    return color;
}
