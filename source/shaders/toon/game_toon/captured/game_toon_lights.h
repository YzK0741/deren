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
static float3 game_toon_add_lights(float3 colour, float3 N, float3 V, float3 P, float3 diffuse,
                                  float3 F0, float roughness) {
    const int count = int(light_at(heap_light_slot).light_count.x);
    const float3 camera_axis = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz);
    const float no_v = clamp(dot(N, V), 0.0, 1.0);
    const float a = max(roughness * roughness, 0.0078125);
    const float a2 = a * a;
    const float u = 1.0 - abs(dot(N, V));
    const float diffuse_luma = dot(diffuse, float3(0.2126, 0.7152, 0.0722));

    for (int i = 0; i < count; ++i) {
        const PunctualLight pl = light_at(heap_light_slot).punctual_lights[i];
        const float mode = pl.color.w;
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
                      (0.5 + npr_y * (diffuse_luma - 0.5));
        } else if (mode > 1.5) { // captured npr:"specular"
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
#endif

#endif
