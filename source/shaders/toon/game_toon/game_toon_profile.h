#ifndef DEREN_GAME_TOON_GAME_TOON_PROFILE_H
#define DEREN_GAME_TOON_GAME_TOON_PROFILE_H

// ============================================================================
// GAME_TOON VIEWER PROFILE CONTRACT (opt-in, captured reference)
// ============================================================================
//
// The captured viewer's material/profile data is packed into the sixteen
// `toon_colour_lane::game_toon_0..15` lanes (30..45), which are zero by default.
// `game_toon_0.x > 0.5` is the enable word; the remaining lanes are the captured
// per-material payload consumed by the viewer-translated branches.
//
// This header is the ONE place the lane packing is named. It is included by
// `game_toon.slang` after `character_forward.slang`, so `toon_colour_at` and
// `character_toon_colour_lanes` are in scope. It deliberately contains no
// shading: the translated branches read these accessors.
//
// LANE PACKING (authority: tmp/game_toon-reference/assets/uniform-packing.json, the
// already-staged `laevatain_game_toon_preview.glb` sidecar):
//   lane 0: x enabled, y Q143 mainLightIntensity, z Q283 envMul, w Q338 envShadowMul
//   lane 1: rgb Q10 uMainLightColourScaled, a Q215 rangeBias
//   lane 2: rgb Q87 uSkinLightColourScaled, a light minPitch degrees
//   lane 3: rgb Q62 uShadowTint, a light yawBias degrees
//   lane 4: rgb uSkinShadowTint, a followCamera
//   lane 5: x Q347 shadowBrightness, y Q348 shadowSaturation, z Q291 skinRimOff, w Q301 faceRimOff
//   lane 6: x/y Q162 highlight vector, z Q302 bumpScale, w Q251 specBumpScale
//   lane 7: Q120 anisotropy; lane 8: Q146; lane 9: Q212/Q160/Q255/Q32;
//   lane 10: Q113 line ST; lane 11: Q72 line scalars; lane 12: Q152/Q238;
//   lane 13: Q349 silk colour; lane 14: Q190/alphaPremultiply; lane 15: Q93 matcap colour.
// A lane left at zero means "not stated", never a guessed default. Do not add
// semantics here without updating the CPU side (`toon_colour_lane` in
// primitive.cppm), the sidecar vocabulary in `main.cpp`, and the numerical
// tests in `test_goo_toon_math.cpp`.
// ============================================================================

#ifndef __cplusplus
static const uint game_toon_profile_lane_base = 30u;
static const uint game_toon_profile_lane_count = 16u;
static const uint game_toon_profile_enable_lane = 0u;

static float4 game_toon_profile_lane(uint material_index, uint lane) {
    return toon_colour_at(heap_slots_toon_colours,
                          material_index * character_toon_colour_lanes + game_toon_profile_lane_base + lane);
}

static bool game_toon_profile_enabled(uint material_index) {
    return game_toon_profile_lane(material_index, game_toon_profile_enable_lane).x > 0.5;
}

// The captured volume multiplier (1.05) is already folded into lanes 1/2 by
// `uniform-packing.json`; this accessor stays 1.0 so no branch multiplies twice.
static float game_toon_volume_multiplier(uint material_index) { return 1.0; }
static float game_toon_main_light_intensity(uint material_index) { return game_toon_profile_lane(material_index, 0u).y; }
static float game_toon_environment_multiplier(uint material_index) { return game_toon_profile_lane(material_index, 0u).z; }
static float game_toon_environment_shadow_multiplier(uint material_index) { return game_toon_profile_lane(material_index, 0u).w; }
static float4 game_toon_main_light(uint material_index) { return game_toon_profile_lane(material_index, 1u); }
static float4 game_toon_skin_light(uint material_index) { return game_toon_profile_lane(material_index, 2u); }
static float3 game_toon_shadow_tint(uint material_index) { return game_toon_profile_lane(material_index, 3u).rgb; }
static float3 game_toon_skin_shadow_tint(uint material_index) { return game_toon_profile_lane(material_index, 4u).rgb; }
static float game_toon_range_bias(uint material_index) { return game_toon_profile_lane(material_index, 1u).a; }
static float game_toon_min_pitch(uint material_index) { return game_toon_profile_lane(material_index, 2u).a; }
static float game_toon_yaw_bias(uint material_index) { return game_toon_profile_lane(material_index, 3u).a; }
static float game_toon_follow_camera(uint material_index) { return game_toon_profile_lane(material_index, 4u).a; }
#endif

#endif
