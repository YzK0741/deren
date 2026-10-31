// =============================================================================================================
// game_toon/outline.slang - THE CAPTURED GAME_TOON VIEWER'S INVERTED-HULL OUTLINE, AS A deren OUTLINE STAGE
// =============================================================================================================
//
// WHAT THIS IS. The captured GameToon Studio viewer draws every outlined material a SECOND time as an inverted
// hull: one vertex stage pushes each vertex along its projected normal and offsets its clip-space z, and one
// fragment stage paints the ring. The two halves are captured in
//
//   tmp/game_toon-studio-audit/final/shaders/outline.vert.readable.glsl   (cited below as `vert:NN`)
//   tmp/game_toon-studio-audit/final/shaders/outline.frag.readable.glsl   (cited below as `frag:NN`)
//
// and this file is that pair, transcribed formula-for-formula, as the two entry points deren's EXISTING outline
// pipeline consumes: a MESH stage (dere's geometry path has no input assembler any more, so the per-vertex work
// that the captured `vertexShader` did must be a mesh stage - exactly the shape `scene/pbr.slang:outline_mesh_main`
// has) and a FRAGMENT stage (`game_toon_outline_frag_main`).
//
// -------------------------------------------------------------------------------------------------------------
// THE CAPTURED MATERIAL, FROM THE VIEWER'S OWN JS (this is where the uniforms' NAMES come from; the GLSL only
// carries the obfuscated ids). `eadba1af4f-DIaZSo8d.js`'s `addOutline` (~byte offset 246420) builds the outline
// mesh as a SECOND `nh(geometry, material)` under the character mesh (`visible = false`, added to `outlines`):
//
//   Q121 = _OutlineMask texture, Q195 = _OutlineWidth ?? .5, Q206 = _OutlineOffsetZ ?? 0,
//   Q76  = the RENDER TARGET size (set per frame in onBeforeRender), Q102 = _OutlineColorBrightness ?? .5,
//   Q319 = _OutlineColorSaturation ?? 1, Q20 = _AlphaPremultiply ?? 0,
//   Q334 = _OutlineTintColor ?? [1,1,1,1] (sRGB->linear through `Na`/`It`, ~offset 186518),
//   side = BackSide (i.e. CULL FRONT), and the material is OPAQUE unless the character material is transparent.
//
// Its defines, and the condition each one is emitted under (`addOutline` + `material-feature-defines.js`'s `Zm`):
//   Q140 base map / Q42 alpha test / Q142 transparent  <- copied from the character material (`Zm`)
//   Q243 Q273 Q154 Q178                                <- copied from the character material's defines
//   Q198 lit outlines                                  <- `this.litOutlines`, viewer default TRUE
//   Q326 outline mask                                  <- `_EnableOutlineMask === 1 && _OutlineMask`
//   Q274 average/smooth normal                          <- `_OutlineAverageNormal > .5 && geometry has _outline_normal + tangent`
//   Q73  outline tint                                    <- `_OutlineTintEnable !== 0`
// and the ten per-material uniforms `P3 = [Q111,Q231,Q316,Q347,Q348,Q62,Q353,Q291,Q301,Q49]` are inherited from
// the character material whenever it has a diffuse ramp.
//
// -------------------------------------------------------------------------------------------------------------
// WHAT DEREN'S OUTLINE PIPELINE ALREADY STATES, and what this stage therefore does NOT have to:
//
//   * CULL FRONT is the mechanism (`vert:45-66` pushes the hull out, and the far side survives the depth test).
//     SATISFIED: `render_environment::forced_cull_front` (set around the outline group only,
//     `character_forward.cpp:140`), because culling is DYNAMIC state in this renderer - no pipeline bit.
//   * ONE HDR COLOUR TARGET + the G-buffer DEPTH, OPAQUE blend, depth compare LESS_OR_EQUAL - all stated by
//     `core::make_outline_pipeline` (`core.cpp:759-809`) and the character-forward pass's LOCKED depth write off.
//   * The captured shader's own `ZTest`/`ZWrite`/`Blend`: the viewer material's THREE defaults are depthTest on,
//     depthFunc LessEqual, depthWrite ON for an opaque character material, NoBlending for the opaque case. deren
//     reproduces the test and the blend and DEVIATES on the write (off + locked) - the same recorded deviation
//     `toon/outline.slang`'s header states, with the same consequence (the hull does not occlude the overlay group
//     or the post chain).
//
// WHAT THIS STAGE MUST NOT DO, and why it is a Vulkan rule rather than a choice: it must not sample the DEPTH
// image - the pass holds `gbuffer_depth` as its depth attachment (`render_resource.cppm`'s
// `character_forward_targets`), and an image cannot be an attachment and a sampled texture in one instance. The
// captured fragment does not read depth either (`R200` is the depth CUTOFF, disabled in the capture: `Q185 = 0`).
//
// -------------------------------------------------------------------------------------------------------------
// THE THREE LAYERS THIS FILE HAS TO BRIDGE, stated once so the rest of the file can just do them:
//
//   1. MESH ATTRIBUTES. The captured vertex stage reads `position`, `normal`, `uv`, `tangent`, and a custom
//      `Q194` vec4 attribute that the viewer's loader fills from the geometry's `_outline_normal` attribute
//      (`E3="_outline_normal"`, `Vf="Q194"`, ~offset 240341; `B3` at ~250033 gates Q274 on both existing).
//      deren's `MeshVertex` is the 80-byte interleaved record of `mesh_geometry.slang:66-73`: position, normal,
//      uv, joints, weights, tangent - but NO second/outline-normal attribute. So:
//        * `position/normal/uv` -> `MeshVertex` + deren's own `pbr_shade_vertex` (morph -> skin -> world ->
//          projection), which is the adapter for the captured `<skinning_pars_vertex>`/`<skinning_vertex>` chain
//          and `modelMatrix`/`modelViewMatrix`/`projectionMatrix`;
//        * `tangent` + `Q194` (the Q274 smooth-normal expansion) -> NOT EXPRESSIBLE. The TANGENT now exists, but
//          the custom `Q194` outline-normal attribute the expansion needs does not, so it is transcribed nowhere
//          and the vertex normal is used instead; see the notes for what that changes and why it is recorded
//          rather than guessed.
//   2. MATERIAL / LANE VALUES. deren has no per-material outline struct. The captured row rides the two carriers
//      this renderer already has plus the game_toon profile lanes; every read is spelled once in the "ADAPTERS"
//      block below and each carries its captured source and its capture-default fallback.
//   3. THE TWO CAPTURED PATHS. `Q73` selects a flat tint (Q334) or the desaturated-then-brightened base colour
//      (`R113(base.rgb,Q319)*Q102`); `Q198` then re-shades that colour through the captured `R198` toon chain
//      (default ON), and `Q198` itself is only emitted for a material with a diffuse ramp. All three are
//      transcribed; nothing is invented for a case the capture does not reach.
//
// HARD BOUNDARY, per tmp/game_toon-port/CONTRACT.md: every number below is cited to a captured file+line/field or
// to a deren lane; a captured value whose lane is absent is marked `// MISSING PROFILE LANE` and never silently
// replaced by a taste. The viewer's studio punctual rig (`R114`, frag:150-289) is NOT ported - it is the
// capture's SCENE rig, the same decision all four game_toon family ports take.
//
// -------------------------------------------------------------------------------------------------------------
// THE ONE THING A READER OF THE OTHER GAME_TOON FILES MIGHT EXPECT AND WILL NOT FIND HERE: the outline fragment
// does NOT shade the surface BEHIND the ring from the G-buffer (that is what deren's own `toon/outline.slang`
// does for the ARTICLE's outline, and it is a different mechanism). The captured outline fragment shades the
// OUTLINE MESH's own material: its base colour, its family, its ramp, its own world normal `Q35`. The G-buffer
// normal slot is therefore read by NEITHER entry here - the mesh entry only asks it for its SIZE (`Q76`, see the
// adapters), which is the frame's render extent.
// =============================================================================================================

// -------------------------------------------------------------------------------------------------------------
// INCLUDE ORDER. `scene/pbr.slang` FIRST and ONLY, because this TU needs `pbr_shade_vertex`/`pbr_vertex_output`/
// `MeshVertex` for the geometry half and `gather_surface`/`toon_colour_at` for the colour half, and it is the one
// file that already carries BOTH without a second copy. The TU CANNOT also take `toon/character_forward.slang`:
// both include `common/heap_access.slang`, which has NO include guard, and a measured scratch compile of the pair
// fails with `E30201: function 'heap_at' already has a body` (and nine more) - so the fragment half does not
// include the character stage and does not reuse its functions; the captured functions this stage needs are
// transcribed locally under the `game_toon_outline_` prefix (the same arrangement `toon/outline.slang` avoids by
// including the character stage instead of the pbr stage; a file can have one or the other, not both).
//
// `toon/toon_params.slang` and `toon/game_toon_math.h` are safe additions: neither includes anything, and
// `toon_params.slang` says so itself ("it may be included at any point in a stage's include order").
// -------------------------------------------------------------------------------------------------------------
#include "scene/pbr.slang"

#include "toon/toon_params.slang"
#include "toon/game_toon_math.h"
#include "toon/game_toon/captured/game_toon_lights.h"

// `toon/game_toon/game_toon_profile.h` expects two prerequisites in scope: `toon_colour_at` (provided by the heap
// shim, through `scene/pbr.slang`) and `character_toon_colour_lanes`. The header says it is included by
// `game_toon.slang` after `character_forward.slang`; this TU includes the pbr stage instead, so the stride is
// bound to the ONE literal this TU already carries - `pbr_toon_colour_lanes` (`source/shaders/scene/pbr.slang:343`,
// `46u`, the host's `deren::vulkan::toon_colour_lane::count`). One literal, two spellings of the same number:
// `test_toon_material_sidecar.cpp` pins `pbr_toon_colour_lanes = 46u` against that enum, so this alias cannot drift
// silently without that test failing first.
static const uint character_toon_colour_lanes = pbr_toon_colour_lanes;
#include "toon/game_toon/captured/encoding.h"
#include "toon/game_toon/game_toon_profile.h"

// ---- CAPTURED VIEWER CONSTANTS (frag:361-362; the same numbers as `comparison-summary.json` `shader_constants`) --
// `const float VIEWER_AMBIENT = 0.725;`   (frag:361) - the skin-arm light formula's ambient factor
// `const float VIEWER_LIGHT_SCALE = 0.2877;` (frag:362)
// The captured names are kept so the `R198` transcription below reads against the source line for line.
static const float R326 = 0.725;  // VIEWER_AMBIENT (frag:361)
static const float R60 = 0.2877;  // VIEWER_LIGHT_SCALE (frag:362)

// ---- CAPTURED LAEVATAIN VOLUME VALUES (comparison-summary.json `laevat_volume`,
//      2fcd9c7ad0-1619b7616e7ad936.json-laevat.json `volume`) - the fallback of the profile lanes below, every one
//      marked `// MISSING PROFILE LANE`. Same values/sources as the four family ports. ----
static const float game_toon_outline_captured_volume_mult = 1.05;                 // volume.multiplier
static const float game_toon_outline_captured_env_mul = 0.55;                     // volume.envMul (profile 0.z)
static const float3 game_toon_outline_captured_shadow_tint = float3(0.8783, 0.9302, 1.1217);      // volume.shadowTint
static const float3 game_toon_outline_captured_skin_shadow_tint = float3(1.187, 0.9272, 0.813);   // volume.skinShadowTint
static const float3 game_toon_outline_captured_skin_light = float3(1.0, 0.9262, 0.8667);          // volume.skinLightColour
static const float game_toon_outline_captured_main_light_intensity = 2.0;          // viewer-lighting.js `zf = 2` (Q143)
static const float game_toon_outline_captured_range_bias = 0.0;                    // volume.rangeBias (Q215)

// Per-material rows of the captured Laevatain SKIN materials (body and face state the SAME values, so one constant
// is exact for both; the outline's skin arm is reached only by those two families in the capture). Material JSON
// `extras.colors._SDFRimColor` / `extras.floats._SkinRimOffScale` / `_FaceRimOffScale`.
// // MISSING PROFILE LANE (all three: the rows ride the reserved game_toon payload lanes 5/6 until the Lead stages
// // them; see the notes' packing table).
static const float3 game_toon_outline_captured_sdf_rim_colour = float3(0.651, 0.3882, 0.4078);
static const float game_toon_outline_captured_skin_rim_off = 0.699999988079071;
static const float game_toon_outline_captured_face_rim_off = 1.0;

// The viewer's OWN binding defaults for a material row it does not state (material-uniform-bindings.js `M3`,
// ~offset 237996: `Q347:{value:_ShadowColorBrightness ?? .5}`, `Q348:{value:_ShadowColorSaturation ?? 1}`).
// Those two rows ride the FINAL packing's lane 5`.x`/`.y` (`uniform-packing.json` row 5), and a zero word
// ("not stated") falls back to the JS defaults below.
static const float game_toon_outline_captured_shadow_brightness = 0.5;
static const float game_toon_outline_captured_shadow_saturation = 1.0;

// ---- THE STAGED CAPTURED OUTLINE ROWS, AND WHICH OF THEM HAVE A SHADER-VISIBLE LANE ----
//
// The staged sidecar (`tmp/game_toon-reference/assets/laevatain_game_toon_preview.glb.toon.tsv`) carries every
// outline material row as a RAW row: `_OutlineOffsetZ`, `_OutlineColorBrightness`, `_OutlineColorSaturation`,
// `_OutlineTintEnable`, `_OutlineMask` (a texture `slot` row) and `_EnableOutlineMask` (its float switch). THE
// FINAL PROFILE PACKING has no lane for the four NUMBERS (its rows 0..15 are fully assigned - see the ADAPTERS
// block), so those are the staged rows' captured values, per family where the staged asset differs by family:
//
//   `_OutlineOffsetZ`          body_01 / face_01 = 0.07999999821186066; cloth_01..05, hair_01, hairt_01 = 0.0
//   `_OutlineColorBrightness`  all ten outlined materials = 0.5 (viewer `?? .5`)
//   `_OutlineColorSaturation`  body/face/cloth = 1.5; hair/hairt = 1.2 (absent -> viewer `?? 1` = 1.0)
//   `_AlphaPremultiply`        all = 0.0 (viewer `?? 0`)
//   `_OutlineTintEnable`       cloth = 0.0, the rest absent (=> `?? 0`) -> the tint branch is OFF in the capture
//
// `_OutlineMask` IS THE ONE OUTLINE ROW THAT IS NOT A CONSTANT: it is a per-material IMAGE whose texels modulate the
// hull per vertex, so it needs a real texture lane rather than a number this file could transcribe. It has one:
// `toon_slot::outline_mask` (lane 16, `toon_lanes4_at(..).x`), and `_EnableOutlineMask` is that lane's host gate
// (`main.cpp`'s `toon_lane` table) - face/hair/hairt = 1.0 there, body/cloth = 0.0, which is exactly the viewer's
// `_EnableOutlineMask === 1 && _OutlineMask` condition for its `#ifdef Q326`. See `game_toon_outline_mask_texture`.
static const float game_toon_outline_captured_offset_z_skin = 0.07999999821186066; // body_01 + face_01
static const float game_toon_outline_captured_colour_brightness = 0.5;             // all ten outlined materials
static const float game_toon_outline_captured_colour_saturation_skin = 1.5;        // body + face
static const float game_toon_outline_captured_colour_saturation_cloth = 1.5;       // cloth_01..05
static const float game_toon_outline_captured_colour_saturation_hair = 1.2;        // hair_01 + hairt_01
static const float game_toon_outline_captured_alpha_premultiply = 0.0;             // `_AlphaPremultiply`
static const bool game_toon_outline_captured_tint_enable = false;                  // `_OutlineTintEnable`
static const uint game_toon_outline_sdf_rim_lane = 0u;                             // `toon_colour_lane::sdf_rim`

// The captured JS's own `??` defaults for the outline row (`addOutline`, `Q195/Q206/Q102/Q319/Q334` bindings).
// `_OutlineWidth`'s .5 is deliberately NOT used as a fallback: deren's outline GROUP is gated by a NON-ZERO width
// (`primitive::outline_width`, `character_forward.cpp:134-145`), so "no width row" is "not in this list", and the
// viewer default would draw a hull the frame never selected. Documented in the notes.
static const float game_toon_outline_default_offset_z = 0.0;             // `_OutlineOffsetZ ?? 0`
static const float game_toon_outline_default_colour_brightness = 0.5;    // `_OutlineColorBrightness ?? .5`
static const float game_toon_outline_default_colour_saturation = 1.0;    // `_OutlineColorSaturation ?? 1`
static const float3 game_toon_outline_default_tint = float3(1.0, 1.0, 1.0); // `_OutlineTintColor ?? [1,1,1,1]`

// =============================================================================================================
// ADAPTERS: THE CAPTURED UNIFORMS, SPELLED ONCE EACH
// =============================================================================================================
//
// THE FINAL PACKING LEAVES THE OUTLINE EXACTLY ONE LANE, and this block is written against
// `tmp/game_toon-reference/assets/uniform-packing.json` (the staged `laevatain_game_toon_preview.glb` sidecar): all
// sixteen profile lanes (30..45) belong to the character shading, and the ONLY outline row deren stages is the
// dedicated `toon_colour_lane::outline_edge` (lane 3, `pbr.slang:321-326`):
//
//   * `Q195` `_OutlineWidth` is that lane's `.w` - the captured `float _OutlineWidth` row, which `main.cpp`'s
//     outline branch reads (`material->scalar("_OutlineWidth", 0.0f)`) and which the frame's own
//     `primitive::outline_width` gate consumes, so the hull's size and its presence cannot disagree;
//   * `Q334` `_OutlineTintColor` is that lane's `.rgb` - the captured `color _OutlineTintColor` row, linearised by
//     the loader exactly as `Na`/`It` linearises it in the viewer.
//
// EVERY OTHER OUTLINE ROW HAS NO SHADER-VISIBLE LANE: the packing's rows 0..15 are fully assigned (0 enable/light,
// 1 Q10/rangeBias, 2 Q87/minPitch, 3 Q62/yawBias, 4 skin shadow/follow, 5 Q347/Q348/Q291/Q301, 6 Q162/Q302/Q251,
// 7 Q120, 8 Q146, 9 Q212/Q160/Q255/Q32, 10 Q113, 11 Q72, 12 Q152/Q238, 13 Q349, 14 Q190/Q20, 15 Q93) and NONE is
// `_Outline*`. The captured values therefore come from the STAGED CAPTURED ROWS (the constants block above), not
// from a lane - which is the whole point of this file's reconciliation: it no longer reads lane 12`.z`/`.w` (the
// silk Q238 values cloth owns) or lane 14`.x/.y/.z/.w` (the silk Q190 colour and Q20 premultiply). The one row
// that is NOT a number and therefore could not be transcribed is `_OutlineMask`; it is read from the dedicated
// TEXTURE lane `toon_slot::outline_mask` (lane 16) below - the 17th texture lane, which is why `toon_lane_blocks`
// is now 4 and `toon_lanes4_at` exists (see `deren::engine::toon_slot`).

/// @brief the viewer's FAMILY_SKIN - ONE family covering deren's separate VR_FAMILY_SKIN and VR_FAMILY_FACE
/// @note the captured outline inherits the CHARACTER material's family define (`addOutline` copies it), and the
///       viewer has no face family of its own - so the two deren families are one captured branch.
static bool game_toon_outline_skin_family(uint family) {
    return family == VR_FAMILY_SKIN || family == VR_FAMILY_FACE;
}

/// @brief `Q206` `_OutlineOffsetZ`: 0.08 on the skin family, 0.0 elsewhere - the staged captured rows
/// @param family the material's toon family (`Material::toon_family` in the mesh stage, `s.toon_family` in the
///        fragment stage - both read the same record)
/// @note NO LANE. Per-family rather than per-material because the staged asset's rows are: body_01/face_01 =
///       0.07999999821186066, every other outlined material 0.0.
static float game_toon_outline_offset_z(uint family) {
    return game_toon_outline_skin_family(family) ? game_toon_outline_captured_offset_z_skin
                                                : game_toon_outline_default_offset_z;
}

/// @brief `Q102` `_OutlineColorBrightness`: 0.5 on every captured outlined material (no lane; viewer `?? .5`)
static float game_toon_outline_colour_brightness() {
    return game_toon_outline_captured_colour_brightness;
}

/// @brief `Q319` `_OutlineColorSaturation`: 1.5 on skins/cloth, 1.2 on hair - the staged captured rows (no lane)
static float game_toon_outline_colour_saturation(uint family) {
    if (game_toon_outline_skin_family(family)) {
        return game_toon_outline_captured_colour_saturation_skin;
    }
    if (family == VR_FAMILY_CLOTH) {
        return game_toon_outline_captured_colour_saturation_cloth;
    }
    if (family == VR_FAMILY_HAIR) {
        return game_toon_outline_captured_colour_saturation_hair;
    }
    return game_toon_outline_default_colour_saturation; // the viewer's `?? 1`
}

/// @brief `Q20` `_AlphaPremultiply`: 0.0 on every captured outlined material (no lane; viewer `?? 0`)
static float game_toon_outline_alpha_premultiply() {
    return game_toon_outline_captured_alpha_premultiply;
}

/// @brief `Q334` `_OutlineTintColor`: the existing outline lane's `.rgb`
/// @note the viewer linearises the JSON row with `It` (sRGB->linear); deren's loader binds this lane from the game's
///       own `_OutlineTintColor` row, so the encoding is the loader's - the shader does not decode again.
static float3 game_toon_outline_tint_colour(uint material_index) {
    const float3 lane = toon_colour_at(heap_slots_toon_colours, material_index * character_toon_colour_lanes + pbr_outline_edge_lane).rgb;
    return dot(lane, lane) > 0.0 ? lane : game_toon_outline_default_tint;
}

/// @brief `Q121` `_OutlineMask` texture index: `toon_slot::outline_mask` (lane 16), the table's FOURTH `uvec4`
/// @param material_index the hull material's record index - the same index the fragment stage gathers with
/// @return the texture-array index, or 0 for the captured Q326-ABSENT arm
/// @note THIS IS THE ONE OUTLINE CARRIER THAT IS NOT A NUMBER, and the reason it needed the 17th texture lane: the
///       sidecar stages the row (`slot _OutlineMask` -> `WF_T_actor_common_female_face_01_ST` for the face,
///       `WF_T_actor_laevat_hair_01_ST` for the hair) and the artist's switch (`_EnableOutlineMask` 1.0 on
///       face/hair/hairt, 0.0 on body/cloth) is that lane's host gate in `main.cpp`'s `toon_lane` table, so a
///       material whose flag is off stores 0 here. The mesh stage samples the lane PER VERTEX (the captured
///       `#ifdef Q326` scales the hull's width by `.r` and its depth offset by `.g`, `vert:60-61`), which is why
///       the lane has to reach a shader and could not be resolved on the host.
static uint game_toon_outline_mask_texture(uint material_index) {
    return toon_lanes4_at(heap_slots_toon_lanes, material_index).x;
}

/// @brief `Q73` `_OutlineTintEnable`: false on every captured outlined material (no lane; viewer `?? 0`)
/// @note the tint BRANCH is therefore off, and the captured outline colour is the desaturated/brightened base
///       (`R113(base.rgb,Q319)*Q102`, frag:466) - which is what all ten captured outlines use.
static bool game_toon_outline_tint_enabled() {
    return game_toon_outline_captured_tint_enable;
}

// ---- `R198`'s inherited per-material uniforms. The outline material copies them from the character material
//      (`P3` in `addOutline`), so their captured values are the character's per-material rows; the deren lanes
//      below are the ones the four family ports read them through, repeated here because this stage cannot include
//      `character_forward.slang` (see the include-order note). Every fallback is `// MISSING PROFILE LANE`. ----

/// @brief `Q143` `uMainLightIntensity` (profile lane 0.y; else the captured 2.0)
static float game_toon_outline_main_light_intensity(uint material_index) {
    const float lane = game_toon_main_light_intensity(material_index);
    return lane > 0.0 ? lane : game_toon_outline_captured_main_light_intensity;
}

/// @brief `Q283` `uEnvironmentMultiplier` (profile lane 0.z; else the captured laevat 0.55)
static float game_toon_outline_environment_multiplier(uint material_index) {
    const float lane = game_toon_environment_multiplier(material_index);
    return lane > 0.0 ? lane : game_toon_outline_captured_env_mul;
}

/// @brief `Q10` `uMainLightColourScaled` (profile lane 1.rgb; else lightColour x volume.multiplier = 1.05 grey)
static float3 game_toon_outline_main_light_colour(uint material_index) {
    const float3 lane = game_toon_main_light(material_index).rgb;
    return dot(lane, lane) > 0.0 ? lane : float3(game_toon_outline_captured_volume_mult,
                                                  game_toon_outline_captured_volume_mult,
                                                  game_toon_outline_captured_volume_mult);
}

/// @brief `Q215` `uRampRangeBias` (profile lane 1.a; the captured laevat value is 0.0, which is also the
///        absent-word, so the fallback is written out rather than inferred)
static float game_toon_outline_range_bias(uint material_index) {
    const float lane = game_toon_range_bias(material_index);
    return lane != 0.0 ? lane : game_toon_outline_captured_range_bias;
}

/// @brief `Q87` `uSkinLightColourScaled` (profile lane 2.rgb; else the captured laevat skin light colour)
static float3 game_toon_outline_skin_light_colour(uint material_index) {
    const float3 lane = game_toon_skin_light(material_index).rgb;
    return dot(lane, lane) > 0.0 ? lane : game_toon_outline_captured_skin_light;
}

/// @brief `Q62` `uShadowTint`: the character material's own `Q62`, which `T3` (~offset 239127) aliases to
///        `uSkinShadowTint` for a skin material - so the outline of a body/face takes profile lane 4 and every
///        other family takes lane 3, exactly as the captured `f[Q62]=r.uniforms[Q62]` copy does
static float3 game_toon_outline_shadow_tint(uint material_index, bool skin_family) {
    const float3 lane = skin_family ? game_toon_skin_shadow_tint(material_index) : game_toon_shadow_tint(material_index);
    if (dot(lane, lane) > 0.0) {
        return lane;
    }
    return skin_family ? game_toon_outline_captured_skin_shadow_tint : game_toon_outline_captured_shadow_tint;
}

/// @brief `Q347` `_ShadowColorBrightness` (profile lane 5`.x` per the FINAL packing; else the captured viewer .5)
/// @note `uniform-packing.json` row 5: `Q347 _ShadowColorBrightness ?? .5`, `Q348 _ShadowColorSaturation ?? 1`,
///       `Q291 _SkinRimOffScale ?? 1`, `Q301 _FaceRimOffScale ?? 1`. The captured staged rows are 0.55/1.4 (body),
///       0.55/1.2 (face), 0.5/1.0 (cloth), 0.55/1.1 (hair); a staged lane carries them, the zero word falls back to
///       the viewer defaults here.
static float game_toon_outline_shadow_brightness(uint material_index) {
    const float lane = game_toon_profile_lane(material_index, 5u).x;
    return lane > 0.0 ? lane : game_toon_outline_captured_shadow_brightness;
}

/// @brief `Q348` `_ShadowColorSaturation` (profile lane 5`.y`; else the captured viewer default 1)
static float game_toon_outline_shadow_saturation(uint material_index) {
    const float lane = game_toon_profile_lane(material_index, 5u).y;
    return lane > 0.0 ? lane : game_toon_outline_captured_shadow_saturation;
}

/// @brief `Q353` `_SDFRimColor`: the EXISTING `toon_colour_lane::sdf_rim` lane (0), per `uniform-packing.json`
///        `existing_lanes.Q353` ("existing _SDFRimColor lane; apply Na conversion")
/// @note the fallback is the captured laevat body/face row, used when the lane holds the app's white neutral.
static float3 game_toon_outline_sdf_rim_colour(uint material_index) {
    const float3 lane = toon_colour_at(heap_slots_toon_colours, material_index * character_toon_colour_lanes + game_toon_outline_sdf_rim_lane).rgb;
    return dot(lane, lane) > 0.0 ? lane : game_toon_outline_captured_sdf_rim_colour;
}

/// @brief `Q291` `_SkinRimOffScale` (profile lane 5`.z`; else the captured laevat row)
static float game_toon_outline_skin_rim_off_scale(uint material_index) {
    const float lane = game_toon_profile_lane(material_index, 5u).z;
    return lane > 0.0 ? lane : game_toon_outline_captured_skin_rim_off;
}

/// @brief `Q301` `_FaceRimOffScale` (profile lane 5`.w`; else the captured laevat row)
static float game_toon_outline_face_rim_off_scale(uint material_index) {
    const float lane = game_toon_profile_lane(material_index, 5u).w;
    return lane > 0.0 ? lane : game_toon_outline_captured_face_rim_off;
}

// =============================================================================================================
// THE CAPTURED HELPERS (transcribed; they are the viewer's own functions, not deren's)
// =============================================================================================================

/// @brief the viewer's `R218` (frag:363-365): the LINEAR-LUMINANCE weights
/// @note `R113` below uses a DIFFERENT, coarser weight set (0.2126/0.7152/0.0722); both are the capture's, and
///       keeping both is why the two functions are separate here instead of one "luma".
static float game_toon_outline_luma(float3 colour) {
    return dot(colour, float3(0.212673, 0.715152, 0.072175));
}

/// @brief the viewer's `R113` (frag:17-20): `max(mix(vec3(l), c, s), 0)` with `l = dot(c, 0.2126/0.7152/0.0722)`
/// @note this is BOTH the outline's own tint operation (`outline = R113(base.rgb, Q319) * Q102`, frag:466) and
///       `R198`'s no-LUT shadow-side fallback (frag:382). One transcription, two captured call sites.
static float3 game_toon_outline_saturation_to_luma(float3 colour, float saturation) {
    const float l = dot(colour, float3(0.2126, 0.7152, 0.0722));
    return max(lerp(float3(l, l, l), colour, saturation), float3(0.0, 0.0, 0.0));
}

/// @brief the viewer's `R309` (frag:344-346): linearToSrgb, needed only to build the shadow-LUT COORDINATE
/// @note the capture's `R63` samples a non-sRGB LUT and decodes the result itself (`R26`, frag:359). deren's
///       `toon_slot::shadow_lut` lane is uploaded `rgba8_srgb`, so the sampler already returns the linear value
///       `R26` would compute - the same reasoning `toon/game_toon/captured/cloth.slang:92-105` records. The
///       forward transform IS still needed, because the cube's coordinate is built in display space (frag:353).
static float3 game_toon_outline_linear_to_srgb(float3 c) {
    c = clamp(c, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0));
    const float3 low = c * 12.92;
    const float3 high = 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055;
    return lerp(low, high, step(float3(0.0031308), c));
}

/// @brief the viewer's `sampleShadowLutLinear` (`R63`, frag:351-360): a 32^3 cube packed as `n` tiles of `n`x`n`
/// @param albedo_linear `R40` in `R198` (the OUTLINE colour, not the surface behind it - that is the capture's
///        own argument, frag:379/382)
/// @param lane deren's `toon_slot::shadow_lut` texture index (callers must pass a non-zero lane)
/// @note the tile layout/coordinate are the captured expressions field for field; `n` is the image HEIGHT, queried
///       rather than assumed, and `v = 1 - (g+0.5)/n` already carries the archive export's vertical flip.
static float3 game_toon_outline_shadow_lut(float3 albedo_linear, uint lane) {
    uint width = 0u;
    uint height = 0u;
    uint levels = 0u;
    heap_at<Texture2D<float4>>(scene_texture_base() + lane).GetDimensions(0u, width, height, levels);
    const float n = max(float(height), 1.0);
    const float3 c = saturate(game_toon_outline_linear_to_srgb(albedo_linear)) * (n - 1.0); // frag:353
    const float tile0 = floor(c.b);                                                        // frag:354
    const float tile1 = min(tile0 + 1.0, n - 1.0);                                         // frag:355
    const float v = 1.0 - (c.g + 0.5) / n;                                                 // frag:356
    const float u0 = (tile0 * n + c.r + 0.5) / (n * n);                                    // frag:357
    const float u1 = (tile1 * n + c.r + 0.5) / (n * n);                                    // frag:357
    const float3 s0 = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + lane), heap_sampler_texture, float2(u0, v)).rgb; // frag:357
    const float3 s1 = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + lane), heap_sampler_texture, float2(u1, v)).rgb; // frag:358
    return lerp(s0, s1, c.b - tile0);                                                      // frag:359, sampler decodes sRGB
}

// =============================================================================================================
// THE VERTEX HALF: THE CAPTURED NORMAL EXPANSION (`vert:23-66`)
// =============================================================================================================
//
// The captured vertex shader, in its own order:
//   vUv=uv; objectNormal=normalize(normal);                                  (vert:24-25)
//   [Q274 smooth normal from tangent + Q194]                                 (vert:27-33)   NOT EXPRESSIBLE, see above
//   <skinning_pars_vertex> <skinbase_vertex> <skinnormal_vertex>              (vert:35-38)   deren: pbr_shade_vertex
//   <begin_vertex> <skinning_vertex>  Q166=(modelMatrix*vec4(transformed,1)).xyz (vert:37-39) deren: pbr_shade_vertex
//   [Q198] Q35=normalize(mat3(modelMatrix)*objectNormal)                     (vert:41-43)   deren: pbr_vertex_output.normal
//   mvPosition=modelViewMatrix*vec4(transformed,1); gl_Position=P*mvPosition (vert:45-46)   deren: pbr_vertex_output.position
//   n=normalize(normalMatrix*objectNormal); R320=(P*vec4(n,0)).xy            (vert:47-48)
//   R320*=inversesqrt(max(dot(R320,R320),1e-12)); R320.x*=Q76.y/Q76.x        (vert:48-50)
//   R255=atan(1/projectionMatrix[1][1]); w=gl_Position.w                     (vert:51-52)
//   R18=R320*(0.392699/R255*Q195*clamp(w*R255*4.583663,0,1)*0.005)           (vert:53)
//   R199=min(max(w,0),1.570796/R255)/Q76; R18=max_magnitude(R18,R199)        (vert:54-55)
//   R136=Q206; [Q326: R18*=mask.r, R136*=mask.g]                             (vert:56-62)
//   gl_Position.xy+=R18; gl_Position.z = (P*vec4(mv.xy,mv.z-0.1*R136,1)).z*w/(...).w (vert:64-66)
//
// THE SUBSTITUTIONS THIS PORT HAS TO MAKE, each stated at its use:
//   * `normalMatrix * objectNormal` -> `(float3x3)view * world_normal`. `normalMatrix` is
//     inverse-transpose(modelView) applied to an OBJECT normal; deren's normal is already world-space and the
//     view transform is rigid, so the inverse transpose of its 3x3 IS its 3x3. The same substitution, with the same
//     wording, is `scene/pbr.slang:281-295`'s (the existing article-outline mesh entry), and it is what keeps this
//     hull attached to the surface it outlines.
//   * `(projectionMatrix * vec4(n,0.0)).xy` -> `dot(proj[0].xyz, n_view)`, `dot(proj[1].xyz, n_view)`: for a
//     projection without shear (this engine's upper-left 3x3 is diagonal) that is the same pair of products.
//     `scene/pbr.slang:386-387` spells it the same way.
//   * `projectionMatrix[1][1]` is NEGATIVE in this engine (Vulkan's Y flip, `scene/pbr.slang:300-302` states it for
//     the same projection), so the captured `atan(1.0/projectionMatrix[1][1])` is taken on the ABSOLUTE value: the
//     captured quantity is the half-FOV magnitude, while the sign is the flip the rest of the chain already carries.
//   * `Q76` is the viewer's RENDER-TARGET pixel size. This stage has no such uniform; the adapter is
//     `heap_size(gbuffer_normal)` - the frame's render extent, i.e. exactly the image the outline pass writes over
//     (see the notes; the post chain owns any upscale, so the two are the same extent).
//   * `isOrthographic ? 1.0/proj[1][1] : atan(1.0/proj[1][1])`: the character pass only ever draws through the
//     PERSPECTIVE camera (the orthographic projection in this renderer belongs to the shadow pass, which never
//     draws a character hull), so the perspective arm is transcribed and the orthographic arm is recorded in the
//     notes as not reachable from this pass. Transcribing a branch with no caller would be dead code with a
//     fabricated trigger.
//   * `texture2D(Q121, uv)` -> `frame_texture_sample_lod0(index, uv)`: the captured fetch is a VERTEX-shader
//     texture read, which in WebGL2 is base-level; the explicit-LOD form states that instead of relying on a
//     derivative a mesh stage does not have.
//
// THE Z OFFSET LINE IS NOT A COMPOSITION OF PROJECTIONS AND IS KEPT AS ITS OWN EXPRESSION (vert:63-66): the point
// is moved along VIEW z by `-0.1*R136`, projected, and its clip z is re-expressed AT THE ORIGINAL `w`
// (`R285.z * w / R285.w` = new_ndc_z * old_w), so the expansion's xy is not re-scaled by the moved depth.

/// @brief the captured vertex expansion (vert:47-66), applied to a vertex deren's shared body has already placed
/// @param o whose `position` is the clip-space result of `pbr_shade_vertex` (world_pos/normal/uv are its outputs)
/// @param uv the interpolated-style uv the captured shader samples its mask with (`vert:59`)
/// @param Q76 the frame's render extent in pixels (the captured `d` set in `onBeforeRender`)
/// @param Q195 `_OutlineWidth` (dere's outline lane `.w`)
/// @param Q206 `_OutlineOffsetZ` (profile lane 14.x)
/// @param R255 `atan(1.0/abs(projectionMatrix[1][1]))` (the captured `vert:51` perspective arm)
/// @param Q121 the `_OutlineMask` texture index, 0 = the captured Q326 define is off
/// @note THE `inversesqrt(max(dot,1e-12))` IS SAFE AT ZERO and deliberately NOT replaced by the existing mesh
///       entry's `> 1e-5` guard (`pbr.slang:388-394`): the captured expression MULTIPLIES by the inverse square
///       root, so a zero projection gives `0 * 1e6 = 0` rather than a NaN. Keeping the captured form is what lets a
///       grazing normal expand by nothing, which is the article's zero-offset case.
static void game_toon_outline_expand(inout pbr_vertex_output o, const float2 uv, const float2 Q76,
                                    const float Q195, const float Q206, const float R255, const uint Q121) {
    const VR_MAT4 view = camera_at(heap_camera_slot).view;
    const float3 n_view = normalize(mul(o.normal, (float3x3)view));                       // vert:47
    float2 R320 = float2(dot(camera_at(heap_camera_slot).proj[0].xyz, n_view),               // vert:48
                         dot(camera_at(heap_camera_slot).proj[1].xyz, n_view));
    R320 *= rsqrt(max(dot(R320, R320), 1e-12));                                            // vert:49
    R320.x *= Q76.y / Q76.x;                                                               // vert:50
    const float w = o.position.w;                                                          // vert:52
    float2 R18 = R320 * (0.392699 / R255 * Q195 * clamp(w * R255 * 4.583663, 0.0, 1.0) * 0.005); // vert:53
    const float2 R199 = float2(min(max(w, 0.0), 1.570796 / R255)) / Q76;                   // vert:54
    // `R18 = mix(R18, sign(R18)*R199, vec2(lessThan(abs(R18),R199)))` (vert:55), written per component because
    // that IS the captured expression: below the clamp each component becomes `sign(x)*R199`, and at exactly zero
    // that is zero - not `R199`.
    if (abs(R18.x) < R199.x) {
        R18.x = sign(R18.x) * R199.x;
    }
    if (abs(R18.y) < R199.y) {
        R18.y = sign(R18.y) * R199.y;
    }
    float R136 = Q206;                                                                     // vert:56
    if (Q121 != 0u) {                                                                      // #ifdef Q326 (vert:57)
        const float2 R139 = frame_texture_sample_lod0(Q121, uv).rg;                        // vert:59
        R18 *= R139.r;                                                                     // vert:60
        R136 *= R139.g;                                                                    // vert:61
    }
    o.position.xy += R18;                                                                  // vert:64
    // vert:45 + 63-66: the captured `mvPosition` is `modelViewMatrix * transformed`, whose deren equivalent is
    // `view * world_pos` - the SAME quantity `pbr_shade_vertex` multiplied by the projection to make o.position.
    const float4 mvPosition = mul(float4(o.world_pos, 1.0), view);
    const float4 R285 = mul(float4(mvPosition.xy, mvPosition.z - 0.1 * R136, 1.0), camera_at(heap_camera_slot).proj);
    o.position.z = R285.z * w / R285.w;                                                    // vert:65-66
}

// =============================================================================================================
// THE MESH ENTRY: `main`'s loop of `scene/pbr.slang:outline_mesh_main` (345-408) with the captured expansion
// =============================================================================================================
//
// THE LOOP IS DEREN'S, UNCHANGED, because the hull must be skinned/morphed/instanced EXACTLY like the surface it
// outlines or it detaches the moment the character deforms - the same argument `pbr.slang:263-271` makes. What is
// this stage's own is only the expansion call.
//
// THE NAME IS NOT `outline_mesh_main`: `scene/pbr.slang` is INCLUDED above and defines that entry already; two
// mesh entries may not share a name in one translation unit. The host does not name the entry - it loads the
// compiled module - so the compile rule's `-entry` is the only place this name appears (notes §6).
[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void game_toon_outline_mesh_main(out vertices pbr_vertex_output verts[mesh_vertices_per_workgroup],
                                out indices uint3 tris[mesh_triangles_per_workgroup], uint3 group_id : SV_GroupID) {
    const MeshGeometryLanes geometry = push.geometry;
    const uint triangle_count = geometry.index_count / mesh_indices_per_triangle;
    const uint first_triangle = group_id.x * mesh_triangles_per_workgroup;
    const uint remaining = first_triangle < triangle_count ? triangle_count - first_triangle : 0u;
    const uint emit = min(remaining, mesh_triangles_per_workgroup);

    SetMeshOutputCounts(emit * mesh_indices_per_triangle, emit);
    mesh_stat_add(push.frame_slot, mesh_stat_mesh_workgroups, 1u);

    // ---- THE PER-FRAME / PER-MATERIAL VALUES, hoisted exactly where `outline_mesh_main` hoists its own ----
    //
    // `Q195` `_OutlineWidth`: deren's outline lane, `.w` (the existing mesh entry reads the same word for the same
    // reason, and the frame's `outline_width > 0` filter is what puts this draw in the outline group at all).
    const uint colour_base = push.material_index * character_toon_colour_lanes;
    const float Q195 = toon_colour_at(heap_slots_toon_colours, colour_base + pbr_outline_edge_lane).w;
    // `Q206` `_OutlineOffsetZ`: NO LANE in the final packing - the staged captured row is 0.08 on the skin family
    // (body/face) and 0 elsewhere. The family comes from the material record, the same field the fragment stage
    // gathers, so the hull's depth offset and its shading cannot disagree about the material.
    const uint outline_family = material_at(heap_slots_materials, push.material_index).toon_family;
    const float Q206 = game_toon_outline_offset_z(outline_family);
    // `Q121` `_OutlineMask`: the dedicated texture lane (16) in the table's FOURTH block, or 0 when the material's
    // `_EnableOutlineMask` gate left it unset - which is the captured Q326-absent arm, i.e. the uniform hull.
    const uint Q121 = game_toon_outline_mask_texture(push.material_index);
    // `Q76`: the captured per-frame RENDER TARGET size (`onBeforeRender`). This stage's adapter is the frame's own
    // render extent, read off the G-buffer normal image the pass already publishes for its shaders (the outline
    // fragment of `toon/outline.slang` samples the same slot, so its descriptor is bound for this group too).
    const float2 Q76 = heap_size(heap_at<Texture2D<float4>>(heap_slots_gbuffer_normal + heap_image_index), 0u);
    // `R255` (vert:51, perspective arm): the half-FOV, taken on the magnitude of the engine's Y-flipped proj11.
    const float R255 = atan(1.0 / abs(camera_at(heap_camera_slot).proj[1][1]));

    for (uint t = 0u; t < emit; ++t) {
        for (uint corner = 0u; corner < mesh_indices_per_triangle; ++corner) {
            const uint raw_index = mesh_index_at(geometry, geometry.first_index + (first_triangle + t) * mesh_indices_per_triangle + corner);
            const uint vertex_index = uint(int(raw_index) + geometry.base_vertex);
            const MeshVertex v = mesh_vertex_at(geometry, vertex_index);
            pbr_vertex_output o = pbr_shade_vertex(v, vertex_index, group_id.y);
            // The width gate mirrors `outline_mesh_main`'s (`> 0`): a material with no outline emits its vertices
            // unexpanded, which the frame's own filter already keeps out of this group in practice.
            if (Q195 > 0.0) {
                game_toon_outline_expand(o, o.uv, Q76, Q195, Q206, R255, Q121);
            }
            verts[t * mesh_indices_per_triangle + corner] = o;
        }
        tris[t] = uint3(t * mesh_indices_per_triangle + 0u, t * mesh_indices_per_triangle + 1u, t * mesh_indices_per_triangle + 2u);
    }
}

// =============================================================================================================
// THE FRAGMENT HALF: THE CAPTURED OUTLINE FRAGMENT (frag:447-488)
// =============================================================================================================
//
// Captured main, in order:
//   base = R99(R38(Q21, Q304, Q249, vUv), vUv)                       (frag:448)  -> deren: s.albedo/s.alpha
//   [Q42] alpha test (with the A2C variant)                          (frag:450-457) -> gather_surface's MASK discard
//   R200(Q166, vUv) depth cutoff                                     (frag:458)  -> not ported, Q185 = 0 in the capture
//   [Q73] outline = Q334, else outline = R113(base.rgb,Q319)*Q102    (frag:461-466)
//   alpha = 1; [Q142] alpha = clamp(2*base.a,0,1)                    (frag:467-472)
//   R237 = mix(1, alpha, Q20)                                        (frag:473)
//   [Q198] gl_FragColor = vec4(R198(outline,R237), alpha)            (frag:475-477)
//   else   gl_FragColor = vec4(outline*R237, alpha)                  (frag:478-480)
//   #include <tonemapping_fragment> <colorspace_fragment>            (frag:486-487) -> post owns both; this stage
//                                                                                     returns LINEAR, like every
//                                                                                     other game_toon port.

/// @brief the captured `R198` (frag:366-446): the OUTLINE COLOUR run through the viewer's toon chain
/// @param R40 the outline colour the caller computed (frag:461-466) - `R198`'s own first argument
/// @param R237 `mix(1, alpha, Q20)` (frag:473)
/// @param s the gathered surface (family, ramp/LUT/face-mask lanes; the albedo is NOT read - `R40` already
///        contains the base colour, exactly as the capture passes it)
/// @param world_pos `Q166` (the hull's own interpolated world position)
/// @param Q35 the captured varying (vert:41-43) - the hull's own WORLD normal, NOT a G-buffer normal
/// @param uv the hull's uv (the face mask's coordinate, frag:395)
///
/// WHAT IS NOT IN THIS FUNCTION, and it is the whole list: `Q132` (self-shadow) - the viewer default is OFF and the
/// captured volume states `mainLightShadow: false`, so `R126 = 1.0` and `R51` is not called; the punctual rig
/// `R114` (frag:150-289) - a scene rig, not a material response, and the decision all four family ports take; and
/// the saturated-light globals `R58/R220/R338/R227/R45/R252` (frag:433-442) - they exist only to feed `R114`, so
/// with `R114` gone their assignments are gone too. Every other line of `frag:366-446` is below, in order.
static float3 game_toon_outline_lit(float3 R40, float R237, surface_sample s, float3 world_pos, float3 Q35, float2 uv) {
    const uint material_index = push.material_index;
    const uint4 toon_lanes = toon_lanes_at(heap_slots_toon_lanes, material_index);
    const bool skin_family = s.toon_family == VR_FAMILY_SKIN || s.toon_family == VR_FAMILY_FACE;

    // ---- `N`, `L`, `V` (frag:369-371) ----
    // `L` (`Q163`) is CAMERA-RELATIVE in the viewer; the game_toon family ports take the captured adapter when the
    // profile is on and deren's sun otherwise (`skin_face.slang:388-391`, `cloth.slang:243-246`). This stage keeps
    // that same adapter so an outline cannot be lit from a different direction than the surface it surrounds.
    const float3 camera_forward_axis = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz);
    const float3 N = normalize(Q35);                                                       // frag:369
    const float3 L = game_toon_profile_enabled(material_index)
                         ? normalize(wf_camera_light(-camera_forward_axis, game_toon_yaw_bias(material_index), game_toon_min_pitch(material_index)))
                         : normalize(light_at(heap_light_slot).light_dir.xyz);             // frag:370 (deren sun fallback)
    const float3 V = normalize(camera_at(heap_camera_slot).camera_pos.xyz - world_pos);    // frag:371
    const float R126 = 1.0;                                                                // frag:372 (`Q132` off)
    const float Q283 = game_toon_outline_environment_multiplier(material_index);            // `Q283` (frag:385)
    const float3 Q10 = game_toon_outline_main_light_colour(material_index);                // `Q10`  (frag:422)
    const float3 Q87 = game_toon_outline_skin_light_colour(material_index);                // `Q87`  (frag:418)
    const float Q143 = game_toon_outline_main_light_intensity(material_index);             // `Q143` (frag:418/422)
    const float3 Q62 = game_toon_outline_shadow_tint(material_index, skin_family);         // `Q62`  (frag:419/424)

    // ---- the shadow-side colour (frag:378-385) ----
    // `USE_SHADOW_LUT` is inherited from the character material (`Q273`); deren's lane for it is
    // `toon_indices.y` (a real index only when the map exists AND the artist's flag is on).
    float3 R281;
    if (s.toon_indices.y != 0u) {
        R281 = game_toon_outline_shadow_lut(R40, s.toon_indices.y);                         // frag:379 `R63(R40)`
    } else {
        R281 = game_toon_outline_saturation_to_luma(R40, game_toon_outline_shadow_saturation(material_index)) *
               game_toon_outline_shadow_brightness(material_index);                         // frag:382 `R113(R40,Q348)*Q347`
    }
    R281 *= 0.96 * Q283;                                                                   // frag:385

    // ---- the diffuse ramp (frag:386-388) ----
    // `R198` is only reached when the material HAS a diffuse ramp (the captured `Q198` define is added inside
    // `if (u = o.diffRamp)`, `addOutline`), so `toon_indices.x` is non-zero here; the lane's `!= 0` check is kept
    // anyway because it is deren's documented "do not read" contract and a sample of element 0 would be a constant.
    const float x = clamp(dot(N, L) + game_toon_outline_range_bias(material_index), -1.0, 1.0); // frag:386
    float4 R32 = float4(1.0, 1.0, 1.0, 1.0);
    if (s.toon_indices.x != 0u) {
        R32 = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.x), heap_sampler_texture,
                         float2(clamp(x * 0.5 + 0.5, 0.002, 0.998), 0.5));                    // frag:387, V = 0.5
    }
    const float R144 = min(R32.a, R126);                                                   // frag:388

    // ---- the family arm (frag:390-408) ----
    // `#ifdef Q3` is the viewer's FAMILY_SKIN, which covers BOTH the body and the face (the viewer has no separate
    // face family; deren splits them into VR_FAMILY_SKIN / VR_FAMILY_FACE).
    float3 R334;
    float deep;
    if (skin_family) {
        // `R140 = Q291; [#ifdef Q154 mask = texture2D(Q49,vUv); R140 = mix(Q301,Q291,mask.b)*mask.r]` (frag:391-397)
        float R140 = game_toon_outline_skin_rim_off_scale(material_index);                 // frag:391 `Q291`
        if (s.toon_family == VR_FAMILY_FACE && toon_lanes.z != 0u) {                       // #ifdef Q154 (`Q49` = _SDFMask)
            const float4 mask = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + toon_lanes.z),
                                           heap_sampler_texture, uv);                     // frag:394
            R140 = lerp(game_toon_outline_face_rim_off_scale(material_index),              // frag:395 `Q301`
                        game_toon_outline_skin_rim_off_scale(material_index), mask.b) * mask.r;
        }
        const float R29 = clamp(0.85 * (1.0 - saturate(dot(N, V))) * R140, 0.0, 1.0);     // frag:398
        R334 = R40 * lerp(float3(1.0, 1.0, 1.0), game_toon_outline_sdf_rim_colour(material_index), R29) * 0.96; // frag:399
        deep = clamp(R126 + R32.a, 0.0, 1.0);                                             // frag:400
    } else {
        // `R98 = vec3(viewMatrix[0][2], viewMatrix[1][2], viewMatrix[2][2])` (frag:403) is the view matrix's third
        // GLSL column = deren's `camera_forward` (the toward-camera axis) - the reading `cloth.slang:341-344` and
        // `hair.slang:67` derive for the same expression, with no sign flip.
        const float3 R98 = camera_forward_axis;                                           // frag:403
        const float vx = clamp((dot(N, R98) - 0.25) * 1.333333, 0.0, 1.0);                // frag:404
        R334 = R40 * 0.96;                                                                // frag:405
        deep = clamp(R32.a + vx * vx * (3.0 - 2.0 * vx) * R126, 0.0, 1.0);               // frag:406
    }

    // ---- the dark/lit composition (frag:409-414) ----
    const float R22 = game_toon_outline_luma(0.65 * R281);                                  // frag:409
    const float3 dark = lerp(float3(R22, R22, R22) + 1.2 * (0.65 * R281 - float3(R22, R22, R22)), R281, deep); // frag:410
    const float3 R130 = lerp(dark, R334, R144);                                            // frag:411
    // `R32.rgb` is the ramp's SAMPLE; deren uploads this lane `rgba8_srgb` while the viewer samples the map as raw
    // bytes, so the RGB is de-gamma'd once through the port's shared helper (`game_toon_lights.h:26-33`) before it is
    // used as a colour. The ALPHA is untouched on both sides (an sRGB format does not decode alpha).
    const float3 R32rgb = game_toon_encoding_linear_to_srgb(R32.rgb);                                    // frag:412-413
    const float R150 = max(max(R32rgb.r, R32rgb.g), R32rgb.b) -
                       min(min(R32rgb.r, R32rgb.g), R32rgb.b);                             // frag:412
    float3 color = R130 * lerp(float3(1.0, 1.0, 1.0), R32rgb, R150);                       // frag:413
    color *= clamp(game_toon_outline_luma(R130) / max(game_toon_outline_luma(color), 0.001), 0.0, 1.5); // frag:414

    // ---- the captured light formula (frag:417-426) ----
    if (skin_family) {
        const float3 R266 = Q87 * Q143;                                                    // frag:418
        color *= lerp(Q62, float3(1.0, 1.0, 1.0), R144) * R326 * R60 * Q87 +
                 lerp(float3(game_toon_outline_luma(R266), game_toon_outline_luma(R266), game_toon_outline_luma(R266)),
                      R266, R144);                                                         // frag:419
    } else {
        const float3 R266 = Q10 * Q143;                                                    // frag:422
        const float R169 = clamp(N.y + 0.15, 0.0, 1.0) * 1.5 + 0.5;                        // frag:423
        color *= lerp(Q62, float3(1.0, 1.0, 1.0), R144) * R169 * R60 * Q10 +
                 lerp(float3(game_toon_outline_luma(R266), game_toon_outline_luma(R266), game_toon_outline_luma(R266)),
                      R266, R144);                                                         // frag:424
    }
    color *= R237;                                                                         // frag:427

    // ---- `USE_SATURATION_LIFT` (frag:429-434) ----
    // The captured define is inherited from the character material, where the builder emits it for
    // `diffRamp && family in {cloth, hair, skin}` and the viewer default is `saturationLift: true`; the EYE family
    // is the one the builder excludes. deren has no look-knob lane for it, so the captured default is the port's.
    if (s.toon_family != VR_FAMILY_EYE) {
        const float R157 = game_toon_outline_luma(color);                                   // frag:430
        const float R232 = clamp(R157 - 0.5, 0.0, 0.5);                                    // frag:431
        color = R157 + (color - R157) * (R232 * R232 + 1.0);                               // frag:432
    }

    // frag:445 is `return R114(color, N, N, R45, V, R252, R40*0.96, 0.0, 0.0)`; with the punctual rig not ported
    // (see the function's header) `R114` is the identity, so the captured return value is `color`.
    return color;
}

/// @brief one outline fragment: the captured `main` (frag:447-488) on deren's mesh/material adapters
/// @param v_world_pos/v_normal/v_uv the `pbr_vertex_output` varyings the mesh entry above emits (locations 0/1/2;
///        the fragment entry of the existing outline stage declares the same three, which is the interface contract)
/// @param v_is_front DECLARED FOR INTERFACE PARITY AND DELIBERATELY NOT PASSED ON: the captured outline fragment
///        has no `gl_FrontFacing` flip anywhere (`frag:447-488`), its normal is the vertex-varying `Q35`
///        (`vert:41-43`). Front faces are CULLED in this pass, so the rasterizer's flag is false for every hull
///        fragment; feeding it to `gather_surface` would flip the normal of every double-sided material, which is
///        the one thing the captured outline never does. `true` is therefore the captured no-flip value.
/// @return the captured `vec4(color, alpha)`, LINEAR (the captured `#include <tonemapping_fragment>` /
///         `<colorspace_fragment>` at frag:486-487 are the post chain's, mode 2)
[shader("fragment")]
float4 game_toon_outline_frag_main([[vk::location(0)]] float3 v_world_pos : TEXCOORD0,
                                  [[vk::location(1)]] float3 v_normal : TEXCOORD1,
                                  [[vk::location(2)]] float2 v_uv : TEXCOORD2,
                                  bool v_is_front : SV_IsFrontFace) : SV_Target {
    // `base = R99(R38(Q21,Q304,Q249,vUv), vUv)` (frag:448) is EXACTLY deren's gathered surface: R38 is
    // `vec4(uBaseColour,uOpacity) * texture2D(uBaseMap, uv)` (`frag:21-29`) and `gather_surface` computes the same
    // product from the material record; `R99`'s customisation tint (`Q100`/`Q284`/`Q208`) is INACTIVE in the
    // captured Laevatain materials (`_AvatarCustomizeEnable = 0`), which is why deren's customization-free albedo
    // is the captured value here rather than a simplification. The MASK discard inside `gather_surface` is the
    // captured `Q42` alpha test (frag:450-457).
    const surface_sample s = gather_surface(v_world_pos, v_normal, v_uv, /* is_front = captured no-flip */ true);

    // ---- THE OUTLINE COLOUR (frag:461-466) ----
    float3 outline;
    if (game_toon_outline_tint_enabled()) {
        outline = game_toon_outline_tint_colour(push.material_index);                        // frag:462 `outline = Q334`
    } else {
        outline = game_toon_outline_saturation_to_luma(s.albedo, game_toon_outline_colour_saturation(s.toon_family)) *
                  game_toon_outline_colour_brightness();                                 // frag:466 `R113(base.rgb,Q319)*Q102`
    }

    // ---- ALPHA (frag:467-473) ----
    // `Q142` is the viewer's `n.transparent` (`Zm`, ~offset 240003: `transparent && (Q142="")`), i.e. the character
    // material already blends; deren's two spellings of that are the glTF BLEND bit and the toon material's own
    // `_TRANSPARENT_ON` bit (`material_record.slang:44-55`). The captured `_OutlineTransparent` material row is NOT
    // read anywhere in the viewer's JS (verified by grep) and is not used here.
    float alpha = 1.0;
    if ((s.flags & 32u) != 0u || (s.flags & 256u) != 0u) {
        alpha = clamp(2.0 * s.alpha, 0.0, 1.0);                                            // frag:470
    }
    const float R237 = lerp(1.0, alpha, game_toon_outline_alpha_premultiply()); // frag:473

    // ---- THE OUTPUT (frag:475-481) ----
    // `Q198` is `this.litOutlines` (viewer default ON) AND only emitted for a material with a diffuse ramp
    // (`addOutline`: inside `if (u = o.diffRamp)`), which is what the `toon_indices.x` test below reproduces.
    if (s.toon_indices.x != 0u) {
        return float4(game_toon_outline_lit(outline, R237, s, v_world_pos, normalize(v_normal), v_uv), alpha); // frag:476
    }
    return float4(outline * R237, alpha);                                                   // frag:479
}
