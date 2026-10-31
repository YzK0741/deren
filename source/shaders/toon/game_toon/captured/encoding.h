#ifndef DEREN_GAME_TOON_GAME_TOON_ENCODING_H
#define DEREN_GAME_TOON_GAME_TOON_ENCODING_H

// ===================================================================================================================
// THE CAPTURED GAME_TOON VIEWER'S COLOUR-SPACE CONTRACT, FOR THE TOON CHARACTER STAGE.
//
// WHAT THE CAPTURE DOES, AND WHAT IT DOES NOT DO. The captured viewer (`tmp/game_toon-studio-audit/final/shaders/
// character.frag.readable.glsl`) performs exactly TWO colour-space conversions by hand, both in its shadow-LUT
// lookup:
//
//   * `linearToSrgb` (:403-406) - encode the LINEAR albedo into the LUT's DISPLAY-SPACE coordinate (:412), after
//     clamping it to [0,1];
//   * `srgbToLinear` (:407-409) - decode the two RAW LUT taps AFTER they have been linearly interpolated in the
//     encoded bytes (:418).
//
// EVERY OTHER TEXTURE IS DECODED BY THE SAMPLER, NOT BY THE SHADER. The viewer uploads a texture as sRGB only
// when the CAPTURED MATERIAL's `extras.srgb` names that slot, and as data/no-colour otherwise
// (`eadba1af4f-DIaZSo8d.js`'s `loadTextures`: `const m = i.has(a) ? Vt : Ht`). The captured lists are:
//
//     M_actor_laevat_cloth_01/02/05  {"srgb": ["_EmissionMap"]}
//     M_actor_laevat_face_01         {"srgb": ["_EmotionMap"]}
//     M_actor_laevat_iris_01         {"srgb": ["_MatcapTex"]}
//     every other laevat material    (no `srgb` key = empty set)
//
// so the RAMPS (`_DiffRampMap`, `_SpecRampMap`), the SHADOW LUT (`_ShadowLutTex`, decoded only AFTER the LUT
// interpolation), the metallic/gloss map, the SDF and the SDF mask, the split normal, the bump map, the
// `_HighlightMap` and the hair line map are all sampled as DATA. The base colour is the exception outside that
// list: it is the glTF `baseColorTexture`, which `THREE.GLTFLoader` assigns SRGBColorSpace unconditionally
// (`assignTexture(o, "map", u.baseColorTexture, St)` with `St = "srgb"`), and the viewer's
// `bQ` downgrade cannot match it because the captured `extras.textures` rows are plain indices, not
// `{index, srgb}` records.
//
// THE OUTPUT ENCODE IS NOT IN THIS STAGE. The captured fragment ends with `<tonemapping_fragment>` then
// `<colorspace_fragment>` (:1189-1190); for the toon material built by `tu.build` the `Q332` define is NOT set,
// so the app's custom tone-map chunk runs, and `colorspace_fragment` applies the renderer's output transfer
// (`sRGBTransferOETF`, `outputColorSpace = "srgb"`) on the way to the canvas. Deren's post composite (mode 2)
// owns both steps. This header therefore contains NO tone map and NO output transfer - adding either here would
// produce the double transform the contract forbids.
//
// DEREN STAGING, AND WHY THIS HEADER EXISTS. `runtime.constructor.cppm:2537-2540` stages the four toon COLOUR
// lanes as `rgba8_srgb` and `:2541/:2546/:2549/:2555` stage the SDF / metallic-gloss / SDF mask / split normal as
// `rgba8_unorm`. That is correct for matcap, emotion and emissive and for every data lane; it is NOT what the
// capture does for the two ramps and (in interpolation order) for the shadow LUT. The families therefore either
// (a) ask the host to stage the captured-raw lane as UNORM (the exact fix; see `tmp/game_toon-port/encoding-notes.md`
// section "host change required"), or (b) keep deren's sRGB lane and undo the sampler decode with
// `game_toon_encoding_display_value_from_srgb_lane` below. Path (b) recovers the stored byte value but cannot undo
// the fact that the texture filter ran on the decoded values - that residual is recorded in the notes.
//
// CAPTURED VALUES ARE TRANSCRIBED BELOW WITHOUT "CLEANING": the clamp before the encode and the `step` thresholds
// are the capture's, the decode has no clamp (the capture's does not), and the LUT decode happens AFTER the mix
// because that is where the capture puts it. The one addition is the explicitly-labelled BRIDGE, which is deren's
// staging difference and not something the capture contains.
// ===================================================================================================================

#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#define GAME_TOON_ENCODING_FLOAT3 glm::vec3
#define GAME_TOON_ENCODING_INLINE inline
#define GAME_TOON_ENCODING_POW(base, exponent) static_cast<float>(std::pow((base), (exponent)))
#define GAME_TOON_ENCODING_CLAMP(value) std::min(std::max((value), 0.0f), 1.0f)
#else
#define GAME_TOON_ENCODING_FLOAT3 float3
#define GAME_TOON_ENCODING_INLINE
#define GAME_TOON_ENCODING_POW(base, exponent) pow((base), (exponent))
#define GAME_TOON_ENCODING_CLAMP(value) clamp((value), 0.0f, 1.0f)
#endif

// -------------------------------------------------------------------------------------------------------------------
// `game_toon_encoding_srgb_to_linear` - the capture's `srgbToLinear`, readable:407-409.
//
//     vec3 srgbToLinear(vec3 c){
//         return mix(c/12.92,pow((c+0.055)/1.055,vec3(2.4)),step(0.04045,c));
//     }
//
// NO CLAMP, exactly like the capture: a negative channel takes the straight segment (the capture's `step` sends
// anything below 0.04045, including negatives, to `c/12.92`). Used by the shadow-LUT decode (:418) and by nothing
// else in the capture.
// -------------------------------------------------------------------------------------------------------------------
GAME_TOON_ENCODING_INLINE float game_toon_encoding_srgb_to_linear_channel(const float channel) {
    return channel < 0.04045f
               ? channel / 12.92f
               : GAME_TOON_ENCODING_POW((channel + 0.055f) / 1.055f, 2.4f);
}

GAME_TOON_ENCODING_INLINE GAME_TOON_ENCODING_FLOAT3 game_toon_encoding_srgb_to_linear(
    const GAME_TOON_ENCODING_FLOAT3 colour) {
    return GAME_TOON_ENCODING_FLOAT3(game_toon_encoding_srgb_to_linear_channel(colour.x),
                                    game_toon_encoding_srgb_to_linear_channel(colour.y),
                                    game_toon_encoding_srgb_to_linear_channel(colour.z));
}

// -------------------------------------------------------------------------------------------------------------------
// `game_toon_encoding_linear_to_srgb` - the capture's `linearToSrgb`, readable:403-406.
//
//     vec3 linearToSrgb(vec3 c){
//         c=clamp(c,0.0,1.0);
//         return mix(c*12.92,1.055*pow(c,vec3(1.0/2.4))-0.055,step(0.0031308,c));
//     }
//
// THE CLAMP IS PART OF THE CAPTURE (`c=clamp(c,0.0,1.0)` on :404) and is kept here: a display-space LUT coordinate
// above 1 is clamped before it becomes an index. `step(0.0031308,c)` is reproduced as `x < 0.0031308f`, i.e. the
// curve segment at and above the threshold.
// -------------------------------------------------------------------------------------------------------------------
GAME_TOON_ENCODING_INLINE float game_toon_encoding_linear_to_srgb_channel(const float channel) {
    const float bounded = GAME_TOON_ENCODING_CLAMP(channel);
    return bounded < 0.0031308f
               ? bounded * 12.92f
               : 1.055f * GAME_TOON_ENCODING_POW(bounded, 1.0f / 2.4f) - 0.055f;
}

GAME_TOON_ENCODING_INLINE GAME_TOON_ENCODING_FLOAT3 game_toon_encoding_linear_to_srgb(
    const GAME_TOON_ENCODING_FLOAT3 colour) {
    return GAME_TOON_ENCODING_FLOAT3(game_toon_encoding_linear_to_srgb_channel(colour.x),
                                    game_toon_encoding_linear_to_srgb_channel(colour.y),
                                    game_toon_encoding_linear_to_srgb_channel(colour.z));
}

// -------------------------------------------------------------------------------------------------------------------
// `game_toon_encoding_shadow_lut_coordinate` - readable:412, the LUT lookup's DISPLAY-SPACE index:
//
//     vec3 c = linearToSrgb(R233) * (n - 1.0);     // R233 = the LINEAR albedo; n = uShadowLutSize
//
// The caller splits `c` into the blue tile, the in-tile red and the green row exactly as the capture does
// (`b0 = floor(c.b)`, `v = 1 - (c.g + 0.5)/n`, `u0 = (b0*n + c.r + 0.5)/(n*n)`; readable:413-417). deren's family
// ports already do that; this helper is the one part of it the capture performs with a conversion function.
// -------------------------------------------------------------------------------------------------------------------
GAME_TOON_ENCODING_INLINE GAME_TOON_ENCODING_FLOAT3 game_toon_encoding_shadow_lut_coordinate(
    const GAME_TOON_ENCODING_FLOAT3 linear_albedo, const float lut_size) {
    return game_toon_encoding_linear_to_srgb(linear_albedo) * (lut_size - 1.0f);
}

// -------------------------------------------------------------------------------------------------------------------
// `game_toon_encoding_shadow_lut_decode` - readable:418, the decode of the two RAW taps AFTER their interpolation:
//
//     vec3 s0 = texture2D(uShadowLutMap, ...).rgb;     // both taps sampled from a NON-sRGB texture
//     vec3 s1 = texture2D(uShadowLutMap, ...).rgb;
//     return srgbToLinear(mix(s0, s1, c.b - b0));
//
// USE THIS ONLY WHEN THE LUT LANE IS STAGED UNORM/RAW. Deren currently stages `toon_slot::shadow_lut` as
// `rgba8_srgb` (`runtime.constructor.cppm:2538`), so its sampler already returns LINEAR taps; the current family
// ports correctly return `lerp(low, high, w)` WITHOUT this decode (see `skin_face.slang:272-276` and
// `cloth.slang:118-120`). Calling this helper on the sRGB lane would decode a second time - the exact mistake the
// contract forbids. It is here for the faithful UNORM path the notes document.
// -------------------------------------------------------------------------------------------------------------------
GAME_TOON_ENCODING_INLINE GAME_TOON_ENCODING_FLOAT3 game_toon_encoding_shadow_lut_decode(
    const GAME_TOON_ENCODING_FLOAT3 raw_mix) {
    return game_toon_encoding_srgb_to_linear(raw_mix);
}

GAME_TOON_ENCODING_INLINE GAME_TOON_ENCODING_FLOAT3 game_toon_encoding_shadow_lut_mix(
    const GAME_TOON_ENCODING_FLOAT3 raw_low, const GAME_TOON_ENCODING_FLOAT3 raw_high,
    const float tile_weight) {
    return game_toon_encoding_shadow_lut_decode(raw_low + (raw_high - raw_low) * tile_weight);
}

// -------------------------------------------------------------------------------------------------------------------
// `game_toon_encoding_display_value_from_srgb_lane` - THE BRIDGE, NOT THE CAPTURE.
//
// Some captured-DATA lanes are currently staged as sRGB by deren (the two ramps; the hair line map through the
// emotion lane - see the notes' slot table). Sampling such a lane returns the sRGB-DECODED value, while the
// capture's `texture2D(...)` returned the stored byte value. Re-encoding recovers that stored value:
//
//     stored = linearToSrgb(sampler_decoded)
//
// This is the same arithmetic as `game_toon_raw_ramp_rgb` (`game_toon_lights.h:29-33`), which the cloth/skin/hair ports
// already call; the two names exist because this file cannot edit that header. It is NOT exact for a filtered
// fetch: the GPU filtered on the decoded values, so a bilinearly interpolated ramp texel differs from the stored
// texel by the curve's bend between the two taps. Staging the lane UNORM removes both the bridge and the residual.
//
// DO NOT apply this to a lane the captured `srgb` list actually names - `_EmotionMap` on the face, `_EmissionMap`
// on the cloth, `_MatcapTex` on the iris - or to the base colour, emissive, SDF, masks, normals or the metallic/
// gloss map: their sampler encoding already matches the capture and the extra encode would be the double transform.
// -------------------------------------------------------------------------------------------------------------------
GAME_TOON_ENCODING_INLINE GAME_TOON_ENCODING_FLOAT3 game_toon_encoding_display_value_from_srgb_lane(
    const GAME_TOON_ENCODING_FLOAT3 sampler_decoded) {
    return game_toon_encoding_linear_to_srgb(sampler_decoded);
}

#undef GAME_TOON_ENCODING_FLOAT3
#undef GAME_TOON_ENCODING_INLINE
#undef GAME_TOON_ENCODING_POW
#undef GAME_TOON_ENCODING_CLAMP

#endif // DEREN_GAME_TOON_GAME_TOON_ENCODING_H
