#include "toon/game_toon/skin_material.h"
// Original HGRP/CharacterNPR_Skin NON-SDF program, not ZmdSkinToonMain.
// Source: docs/references/toon/skin_forward_spirv_decompiled.hlsl (unmodified spirv-cross output).
// Vulkan LOD600 record 1012620, fragment module 1012976:
// _DIFF_RAMP_ON _NORMALMAP _SHADOW_LUT_TEX, with the same three engine keywords as face.
// Face is included first: its packed-normal, export-aware LUT, diffuse composition and
// directional rim helpers describe the same operations in this non-SDF variant.
float3 game_toon_skin_shade(surface_sample s, float3 world_pos, float3 geo_normal, float2 uv, bool is_front) {
    const Material mat = material_at(heap_slots_materials, push.material_index);
    const ToonRigUBO rig = toon_rig_value();
    const toon_params params = toon_params_for(s.toon_family);
    const uint colour_base = push.material_index * character_toon_colour_lanes;
    const float lane_specular = toon_colour_at(heap_slots_toon_colours, colour_base + 4u).x;
    const float specular_strength = lane_specular >= 0.0 ? lane_specular : params.spec_strength;

    // Original :455-501, :505-557: R*A/G packed normal and material bump scale.
    float3 normal = normalize(geo_normal);
    if ((mat.flags & 1u) != 0u) {
        float3 sdir;
        float3 tdir;
        if (surface_tbn(world_pos, uv, sdir, tdir)) {
            tdir = -tdir; // Existing Unity/glTF tangent-space Y bridge, as on face.
            const float4 texel = frame_texture_sample(mat.tex_indices.z, uv);
            const float3 n_ts = game_face_unpack_normal(float2(texel.r * texel.a, texel.g), mat.normal_scale);
            normal = normalize(mul(n_ts, float3x3(sdir, tdir, normal)));
        }
    }
    if ((mat.flags & 8u) != 0u && !is_front) normal = -normal;

    const float3 light_dir = normalize(light_at(heap_light_slot).light_dir.xyz);
    const float3 view_dir = normalize(camera_at(heap_camera_slot).camera_pos.xyz - world_pos);
    const float3 camera_forward = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz);
    // _2204 is the same engine-light composition as face _1646. MISSING game
    // irradiance/art-direction uniforms: deren's sun substitutes, without the
    // author's skin-only 1.3/1.6 factor (absent from this original program).
    const float3 main_light = float3(max(0.001, light_at(heap_light_slot).sun_intensity));
    const float no_v = saturate(dot(normal, view_dir));

    // Original :617-619 (_1133/_1139/_1147). MISSING _49_m30/_49_m34 runtime SSS inputs:
    // use the same explicit family-strength / rig-colour bridge as the face.
    const float sss_area = saturate((1.0 - saturate(no_v * 0.85 + 0.15)) * params.sss_strength);
    const float3 albedo_sss = s.albedo * lerp(float3(1.0), rig.sss.rgb, sss_area);
    const float3 lut_colour = s.toon_indices.y != 0u ? game_face_shadow_lut(s.albedo, s.toon_indices.y) : s.albedo;
    // :966-969; glTF metallic/roughness and the material specular lane substitute
    // _49_m2 / (1-_49_m0) / _49_m1. Do not restore fixed author defaults here.
    const GameSkinMaterial skin = game_skin_material(albedo_sss, lut_colour, s.metallic, s.roughness, specular_strength);

    // :989: the geometric ramp (no face SDF / mask / emotion branch in this variant).
    // MISSING light-direction offset _20_m90.w*_20_m91.x: neutral zero.
    const float ramp_u = clamp(dot(normal, light_dir), -1.0, 1.0) * 0.5 + 0.5;
    float3 ramp_colour = float3(1.0);
    float ramp_alpha = 1.0;
    if (s.toon_indices.x != 0u) {
        const float4 ramp = heap_texel(heap_at<Texture2D<float4>>(scene_texture_base() + s.toon_indices.x),
                                        heap_sampler_texture, float2(ramp_u, character_ramp_band_v));
        ramp_colour = ramp.rgb;
        ramp_alpha = ramp.a;
    } else {
        ramp_alpha = smoothstep(params.center - params.softness, params.center + params.softness, ramp_u);
    }

    // :981-1006: screen-Y (_2125) is deren raster shadow, screen-X (_2128)
    // remains neutral 1, as on face. Body _2205/_2220 equals the shared face
    // material block at mask_y=1, so raster shadows directly affect its lit weights.
    // MISSING _20_m79.z (global dark scale): neutral 1, also matching the face.
    const float shadow_scene = saturate(calc_shadow(world_pos, normal));
    const float light_visibility = 1.0;
    const FaceDiffuseResult composition = game_face_diffuse_composition(
        skin.diffuse, skin.shadow, ramp_colour, ramp_alpha, s.alpha, shadow_scene, 1.0, light_visibility);
    const FaceDirectLight direct_light = game_face_direct_light(main_light, composition.surface_lit);
    const float3 main_diffuse_result = direct_light.diffuse * composition.colour;

    // :1007-1015. The material roughness/F0 now drive the original D/V fit;
    // only specular receives _2232's surface-lit attenuation.
    const float3 forward_light = normalize(float3(camera_forward.x, lerp(0.5, light_dir.y, light_visibility), camera_forward.z));
    const float3 half_dir = normalize(view_dir * (2.0 + light_visibility) + light_dir * light_visibility + forward_light * 2.0);
    const float no_h = dot(normal, half_dir);
    const float a2 = skin.roughness2 * skin.roughness2;
    const float d_base = (no_h * a2 - no_h) * no_h + 1.0;
    const float d_denom = d_base * d_base;
    const float d = a2 != d_denom ? a2 / d_denom : 1.0;
    const float v = 0.5 / (2.0 * no_v + skin.roughness2 + 9.99999975e-05);
    const float3 specular = skin.f0 * clamp(d * v - 6.10351562e-05, 0.0, 20.0) * direct_light.specular;
    // MISSING wet/snow globals (_592/_595/_605), procedural layers (:650-965),
    // SSR and punctual-light inputs: dry else branches; existing deren pipeline
    // retains ownership of scene lighting. No guessed rain textures are sampled.
    const float3 direct = main_diffuse_result + specular;
    // Original :1018-1025 applies this mild bright-region saturation expansion.
    const float luma = face_diffuse_luma(direct);
    const float contrast = clamp(luma - 0.5, 0.0, 0.5);
    const float3 main_light_result = lerp(float3(luma), direct, 1.0 + contrast * contrast);

    // MISSING artistic rim axis/colours _20_m87/_20_m88/_20_m93: retain the
    // conservative geometric/directional fallback accepted for face and skin.
    float3 rim_normal = normalize(geo_normal);
    if ((mat.flags & 8u) != 0u && !is_front) rim_normal = -rim_normal;
    const float rim_no_v = saturate(dot(rim_normal, view_dir));
    const float rim_weight = game_face_rim_light_weight(dot(light_dir, view_dir), dot(rim_normal, light_dir));
    const float rim_start = rig.rim.x * -0.6 + 0.8;
    const float rim_end = rig.rim.x * -0.4 + 0.9;
    const float rim_area = smoothstep(rim_start, max(rim_end, rim_start + 1.0e-6), 1.0 - rim_no_v);
    const float3 rim_brdf = lerp(float3(0.25), skin.diffuse, 0.1);
    const float3 rim_result = rim_brdf * (rim_area * rig.rim.y * min(s.alpha, shadow_scene) * rim_weight);

    // Explicit deren sky bridge retained at the existing skin-family strength.
    // Neither this nor the face's minimum ambient term is a recovered game value.
    const float3 ambient_bridge = environment_irradiance(normal) * s.albedo * s.alpha * params.ambient_strength;
    return main_light_result + max(rim_result, 0.0) + ambient_bridge + s.emissive;
}
