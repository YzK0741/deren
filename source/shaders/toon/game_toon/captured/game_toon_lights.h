#ifndef DEREN_GAME_TOON_GAME_TOON_LIGHTS_H
#define DEREN_GAME_TOON_GAME_TOON_LIGHTS_H

// The captured GameToon viewer's punctual-light loop, driven by deren's ordinary
// `LightUBO.punctual_lights`. The app injects the captured Laevatain character-only rig into that UBO
// when the GameToon preview model is loaded (main.cpp), so the positions here are in the same world
// space as `world_pos` and no second coordinate system is invented.
//
// The captured fields that deren's `punctual_light` does not name are carried in its unused lanes:
//   color.w    = captured npr mode (0 wrap, 2 specular, 3 rim)
//   spot_dir.w = captured nprData.x (rim width / spec edge)
//   position.w = captured nprData.y (rim strength)
//
// Translated from `character.frag.readable.glsl:188-327`; mode 4 (fog) is the captured `mix` arm and
// is left to the scene fog, exactly as the capture's `Q260` list is character-only.
#ifndef __cplusplus
#include "toon/game_toon/captured/encoding.h"

// The current NVIDIA 617.42 descriptor-heap path drops the enclosing 352-byte
// offset when reading nested light structs. Minimal Slang AND glslang shaders
// reproduce it; both pass spirv-val, and GPU comparison matches buffer bytes
// 0..63 instead of 352..415. Use byte-addressed reads of the same descriptor.
// See docs/diagnostics/descriptor-heap-nested-light-read.md. The host pins the 352-byte
// array offset, 64-byte stride and field offsets in primitive.cppm.
// GPU regression: scripts/gates/game_toon_light_reads.py.
static PunctualLight game_toon_load_light(uint index) {
    const uint base = 352u + index * 64u;
    PunctualLight light;
    light.position = asfloat(heap_at<ByteAddressBuffer>(heap_light_slot).Load4(base));
    light.color = asfloat(heap_at<ByteAddressBuffer>(heap_light_slot).Load4(base + 16u));
    light.spot_dir = asfloat(heap_at<ByteAddressBuffer>(heap_light_slot).Load4(base + 32u));
    light.params = asfloat(heap_at<ByteAddressBuffer>(heap_light_slot).Load4(base + 48u));
    return light;
}

/// Shared primary hair lobe: viewer R72 (readable:124-138), game PS 37900 DXBC 2042-2055.
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

// Optional hair lanes: direction = (primary axis, edge fade),
// material = (P.g, anisotropy strength, line gain, enabled). Zero defaults keep
// skin, face, eye and cloth on their existing GGX path.
static float3 game_toon_add_lights(float3 colour, float3 N, float3 V, float3 P, float3 diffuse,
                                  float3 F0, float roughness, float3 rim_base,
                                  float4 hair_direction = float4(0.0),
                                  float4 hair_material = float4(0.0), uint hair_ramp_lane = 0u,
                                  bool eye_diffuse_only = false) {
    const int count = clamp(int(light_at(heap_light_slot).light_count.x), 0, MAX_PUNCTUAL_LIGHTS);
    const float3 camera_axis = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz);
    const float no_v = clamp(dot(N, V), 0.0, 1.0);
    const float a = max(roughness * roughness, 0.0078125);
    const float a2 = a * a;
    const float u = 1.0 - abs(dot(N, V));
    const float rim_luma = dot(rim_base, float3(0.2126, 0.7152, 0.0722));

    for (int i = 0; i < count; ++i) {
        const PunctualLight pl = game_toon_load_light(uint(i));
        const float mode = pl.color.w;
        // Eye PS 37822/37856: no GGX term, and the rim branch's
        // attenuation is explicitly cleared before accumulation.
        if (eye_diffuse_only && mode > 1.5 && mode <= 3.5) continue;
        const float npr_x = pl.spot_dir.w;
        const float npr_y = pl.position.w;
        const float npr_z = pl.params.z;
        const float npr_w = pl.params.w;
        const float3 to_light = pl.position.xyz - P;
        const float d2 = max(dot(to_light, to_light), 1.0e-8);
        const float3 L = to_light * rsqrt(d2);
        const float range = pl.params.x;
        float attenuation = 1.0;
        if (range > 0.0) {
            const float t = d2 / (range * range);
            const float falloff = max(1.0 - t * t, 0.0);
            attenuation = falloff * falloff / (d2 + 1.0);
        }
        if (pl.params.y > 0.5) {
            const float spot = clamp((dot(L, -normalize(pl.spot_dir.xyz)) - pl.params.w) /
                                         max(pl.params.z - pl.params.w, 1.0e-4), 0.0, 1.0);
            attenuation *= spot * spot;
        }
        if (attenuation <= 1.0e-4) continue;

        if (mode > 3.5) { // captured npr:"fog" (readable:327-343); fog's nprData is the captured {0.7,1,1,0}
            float3 fog_light = L;
            if (pl.spot_dir.x != 0.0 || pl.spot_dir.y != 0.0 || pl.spot_dir.z != 0.0) {
                fog_light = normalize(pl.spot_dir.xyz) * dot(to_light, normalize(pl.spot_dir.xyz));
            }
            const float fog_d2 = max(dot(fog_light, fog_light), 1.0e-8);
            const float3 fog_l = fog_light * rsqrt(fog_d2);
            const float fog_e = max(2.0 * npr_y, 0.1);
            const float fog_t = fog_d2 * pl.params.x * pl.params.x;
            const float fog_a = pow(max(1.0 - min(fog_t, 1.0), 0.0), fog_e);
            const float fog_spot = clamp((dot(fog_l, -normalize(pl.spot_dir.xyz)) - pl.params.w) /
                                             max(pl.params.z - pl.params.w, 1.0e-4), 0.0, 1.0);
            const float3 up = float3(0.0, 1.0, 0.0);
            const float fog_x = clamp(dot(up, fog_l) + 0.5, 0.0, 1.0);
            const float fog_smooth = fog_x * fog_x * (3.0 - 2.0 * fog_x);
            const float fog_k = npr_x * fog_a * (1.0 + 0.0 * fog_smooth) * (fog_spot * fog_spot);
            colour = lerp(colour, pl.color.rgb, clamp(fog_k, 0.0, 1.0));
            continue;
        }
        float3 hair_specular = float3(0.0);
        if (hair_material.w != 0.0 && mode <= 2.5) {
            // Game PS 37900 DXBC 2037-2069: strand sine lobe, palette,
            // edge fade, F0, strength, gain 5, per-light specular scale and N.L.
            const float3 half_sum = L + V;
            const float3 half_dir = half_sum * rsqrt(max(dot(half_sum, half_sum), 6.10351563e-5));
            hair_specular = game_toon_hair_specular_lobe(hair_direction.xyz, half_dir,
                hair_material.x, hair_direction.w, hair_ramp_lane) * F0 *
                (hair_material.y * 5.0 * hair_material.z * npr_z * clamp(dot(N, L), 0.0, 1.0));
        }
        if (mode > 2.5) { // captured npr:"rim"
            const float3 Z = camera_axis;
            const float3 projected = cross(Z, cross(Z, L));
            const float projected_len = length(projected);
            const float facing = projected_len > 1.0e-5 ? clamp(dot(N, -projected / projected_len), 0.0, 1.0) : 0.0;
            const float centre = 0.8 - 0.6 * npr_x;
            const float width = 0.1 + 0.2 * npr_x;
            const float band_x = clamp((u - centre) / width, 0.0, 1.0);
            const float band = band_x * band_x * (3.0 - 2.0 * band_x);
            const float visibility = clamp(dot(N, L) + 1.0, 0.0, 1.0);
            colour += attenuation * band * visibility * pl.color.rgb * facing *
                      (0.5 + npr_y * (rim_luma - 0.5));
        } else if (mode > 1.5) { // captured npr:"specular"
            if (hair_material.w != 0.0) {
                colour += attenuation * pl.color.rgb * hair_specular;
                continue;
            }
            const float3 half_dir = normalize(L + V);
            const float no_h = dot(N, half_dir);
            const float dd = no_h * no_h * (a2 - 1.0) + 1.0;
            const float dd2 = dd * dd;
            const float d_term = dd2 != a2 ? a2 / dd2 : 1.0;
            const float spec = clamp(d_term * 0.5 / (2.0 * no_v + a + 0.0001) - 0.000061, 0.0, 20.0);
            const float spec_x = clamp(-10.0 * (roughness - npr_x - 0.05), 0.0, 1.0);
            const float spec_attenuation = spec_x * spec_x * (3.0 - 2.0 * spec_x) *
                                           lerp(1.0, npr_z, 0.5);
            colour += attenuation * pl.color.rgb * spec * F0 * spec_attenuation;
        } else { // captured npr:"wrap" (readable:247-252)
            const float3 light_colour = pl.color.rgb * attenuation;
            const float normalise = (1.0 - npr_y) +
                npr_y / max(max(light_colour.r, max(light_colour.g, light_colour.b)) * 0.75, 1.0);
            const float wrap = 0.25 * npr_x + (1.0 - 0.25 * npr_x) * clamp(dot(N, L) + 0.5, 0.0, 1.0);
            if (hair_material.w != 0.0) {
                colour += light_colour * (normalise * wrap * diffuse + hair_specular);
                continue;
            }
            const float lit = clamp(dot(N, L), 0.0, 1.0);
            const float3 half_dir = normalize(L + V);
            const float no_h = dot(N, half_dir);
            const float dd = no_h * no_h * (a2 - 1.0) + 1.0;
            const float dd2 = dd * dd;
            const float d_term = dd2 != a2 ? a2 / dd2 : 1.0;
            const float spec = clamp(d_term * 0.5 / (2.0 * no_v + a + 0.0001) - 0.000061, 0.0, 20.0);
            colour += light_colour * normalise * wrap * diffuse;
            colour += light_colour * lit * spec * F0;
        }
    }
    return colour;
}

static float3 game_toon_add_eye_lights(float3 colour, float3 N, float3 V, float3 P, float3 diffuse) {
    return game_toon_add_lights(colour, N, V, P, diffuse, float3(0.0), 1.0, float3(0.0),
                                float4(0.0), float4(0.0), 0u, true);
}
#endif

#endif
