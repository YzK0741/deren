// ============================ THE GAME-SHADER TOON MODEL (opt-in) ============================
//
// `[render] game_toon` selects this module's fragment stage instead of the old
// `character_forward.slang` body. Each family is now a separate port of the ORIGINAL GAME PROGRAM that was
// decompiled from the game's Vulkan SMOL-V blocks (not the MyZmdShaders reconstruction that the first version
// used):
//
//   cloth : module 5,164,112  (`shader_9371..._Vulkan_2`)   source `the local decompiled reference shader tree (not distributed): cloth_forward_readable.hlsl`
//   hair  : module 2,252,796  (`shader_8fa556..._Vulkan_1`)  source `the local decompiled reference shader tree (not distributed): hair_forward_spirv_decompiled.hlsl`
//   face  : module 1,758,400  (`shader_3e3d05..._Vulkan_0`)  source `the local decompiled reference shader tree (not distributed): face_forward_spirv_decompiled.hlsl`
//   eye   : module 216,204    (`shader_e85249..._Vulkan_0`)  source `the local decompiled reference shader tree (not distributed): eye_forward_spirv_decompiled.hlsl`
//   skin  : module 1,012,976 (`shader_3e3d05..._Vulkan_0`), the original non-SDF skin variant.
//
// The shared deren lighting/shadow/IBL inputs stand in for Unity's globals (the approved "material only, keep
// deren lighting" boundary): `calc_shadow`, `light_at`, `environment_irradiance`, `environment_specular_sample`.
// The old author body remains the fallback only for families that have no game port yet (outline/base surface).
//
// THIS IS STILL EXPERIMENTAL: every per-family file documents the inputs deren does not carry as `// MISSING:`
// with a fallback, and the Lead has not done the final per-family A/B / frozen-gate acceptance. It is opt-in and
// OFF by default, so selecting it cannot change the default chain's pixels.
#define VR_GAME_TOON_CHAIN 1
#include "toon/character_forward.slang"

/// the author's `MyCalculateLightStrength` (`ZmdToonUtils.hlsl:18-22`).
static float game_light_strength(float3 colour) {
    return dot(colour, float3(0.212672904, 0.715152204, 0.0721750036));
}

/// the author's `UnpackNormalFromTex` (`ZmdToonUtils.hlsl:9-16`): RG -> [-1,1], reconstruct +Z,
/// then scale XY. The stored B/A are ignored, which is what the original cloth N texture needs
/// (B=0, A=255 everywhere).
static float3 game_unpack_normal(float2 rg, float scale) {
    float3 normal;
    normal.xy = rg * 2.0 - 1.0;
    normal.z = max(1.0e-16, sqrt(1.0 - saturate(dot(normal.xy, normal.xy))));
    normal.xy *= scale;
    return normal;
}

/// the author's `MySmoothstep` (`ZmdToonUtils.hlsl:24-30`): t*t*(3-2t), composed by hand.
static float game_smoothstep(float x) {
    const float t = x * -2.0 + 3.0;
    return t * x * x;
}

// ---- THE PER-FAMILY PORTS OF THE ORIGINAL GAME PROGRAMS ----
//
// Each file is a function with the same signature, ported from the game's decompiled Vulkan program rather than
// from the MyZmdShaders reconstruction:
//   cloth : module 5,164,112 (shader_9371..._Vulkan_2)
//   hair  : module 2,252,796 (shader_8fa556..._Vulkan_1)
//   face  : module 1,758,400 (shader_3e3d05..._Vulkan_0)
//   eye   : module 216,204 (shader_e85249..._Vulkan_0)
// The files are included after `character_forward.slang` and these helpers, so the shared deren functions/types
// are in scope. Each file is self-contained; the Lead only dispatches below.
#include "toon/game_toon/game_toon_profile.h"
#include "toon/game_toon/cloth.slang"
#include "toon/game_toon/hair.slang"
#include "toon/game_toon/face.slang"
#include "toon/game_toon/eye.slang"

#include "toon/game_toon/skin.slang"
#include "toon/game_toon/captured/skin_face.slang"
#include "toon/game_toon/captured/hair.slang"
#include "toon/game_toon/captured/cloth.slang"
#include "toon/game_toon/captured/eye.slang"


[shader("fragment")]
float4 game_toon_frag_main(
    [[vk::location(0)]] float3 v_world_pos : TEXCOORD0,
    [[vk::location(1)]] float3 v_normal : TEXCOORD1,
    [[vk::location(2)]] float2 v_uv : TEXCOORD2,
    // THE MESH'S OWN TANGENT FRAME (pbr.slang's `pbr_vertex_output.tangent`, location 4): xyz the world-space
    // tangent, w the authored handedness. It is declared HERE, in the game chain's only entry, because the
    // GameToon hair's split-normal decode is the consumer - and NOT in the stages that do not read it, which is
    // the same rule the location's appending was chosen by (see pbr.slang's note). `w == 0` is the loader's
    // "this primitive has no TANGENT attribute" record, and game_toon_hair_shade falls back to the derivative
    // frame for it.
    [[vk::location(4)]] float4 v_tangent : TEXCOORD4,
    bool v_is_front : SV_IsFrontFace) : SV_Target {
    // THE SAME SURFACE THE G-BUFFER WROTE, gathered by the same function (normal-map fetch, the
    // double-sided flip and the material's factors cannot disagree with the G-buffer if there is
    // one implementation of them).
    const surface_sample s = gather_surface(v_world_pos, v_normal, v_uv, v_is_front);

    // CLOTH: the original game cloth PS (module 5,164,112), ported in game_toon/cloth.slang.
    if (s.toon_family == VR_FAMILY_CLOTH) {
        const float3 lit = game_toon_profile_enabled(push.material_index) ? captured_cloth_shade(s, v_world_pos, v_normal, v_uv, v_is_front) : game_toon_cloth_shade(s, v_world_pos, v_normal, v_uv, v_is_front);
        const bool toon_transparent = (s.flags & 256u) != 0u;
        const float out_alpha = ((s.flags & 32u) != 0u || toon_transparent) ? s.alpha : 1.0;
        return float4(lit, out_alpha);
    }

    // BODY/SKIN: original non-SDF game skin variant (same shader family as face).
    if (s.toon_family == VR_FAMILY_SKIN) {
        const float3 lit = game_toon_profile_enabled(push.material_index) ? captured_skin_face_shade(s, v_world_pos, v_normal, v_uv, v_is_front) : game_toon_skin_shade(s, v_world_pos, v_normal, v_uv, v_is_front);
        const bool toon_transparent = (s.flags & 256u) != 0u;
        const float out_alpha = ((s.flags & 32u) != 0u || toon_transparent) ? s.alpha : 1.0;
        return float4(lit, out_alpha);
    }

    // HAIR: the original game hair PS (module 2,252,796), including its two integer-exponent highlight lobes.
    if (s.toon_family == VR_FAMILY_HAIR) {
        const float3 lit = game_toon_profile_enabled(push.material_index) ? captured_hair_shade(s, v_world_pos, v_normal, v_uv, v_tangent, v_is_front) : game_toon_hair_shade(s, v_world_pos, v_normal, v_uv, v_is_front);
        const bool toon_transparent = (s.flags & 256u) != 0u;
        const float out_alpha = ((s.flags & 32u) != 0u || toon_transparent) ? s.alpha : 1.0;
        return float4(lit, out_alpha);
    }

    // FACE: the original game face/SDF PS (module 1,758,400).
    if (s.toon_family == VR_FAMILY_FACE) {
        const float3 lit = game_toon_profile_enabled(push.material_index) ? captured_skin_face_shade(s, v_world_pos, v_normal, v_uv, v_is_front) : game_toon_face_shade(s, v_world_pos, v_normal, v_uv, v_is_front);
        const bool toon_transparent = (s.flags & 256u) != 0u;
        const float out_alpha = ((s.flags & 32u) != 0u || toon_transparent) ? s.alpha : 1.0;
        return float4(lit, out_alpha);
    }

    // EYE/IRIS: the original game eye/matcap PS (module 216,204).
    if (s.toon_family == VR_FAMILY_EYE) {
        const float3 lit = game_toon_profile_enabled(push.material_index) ? captured_eye_shade(s, v_world_pos, v_normal, v_uv, v_is_front) : game_toon_eye_shade(s, v_world_pos, v_normal, v_uv, v_is_front);
        const bool toon_transparent = (s.flags & 256u) != 0u;
        const float out_alpha = ((s.flags & 32u) != 0u || toon_transparent) ? s.alpha : 1.0;
        return float4(lit, out_alpha);
    }

    // ---- REMAINING FAMILIES (base/outline surface, and anything without a game port yet) ----
    //
    // Cloth, skin, hair, face and eye now have their own functions above. The shared author body remains the
    // fallback for the families this pass has not ported yet (outline/base surface); it is not used as a
    // stand-in for a ported family. FUR is still not ported: the loader has no fur family.
    const toon_params params = toon_params_for(s.toon_family);
    const uint4 toon_lanes = toon_lanes_at(heap_slots_toon_lanes, push.material_index);
    float desaturation = 0.0;
    const float3 colour = toon_diffuse(s.albedo, s.alpha, s.toon_family, goo_toon_shading_normal(v_world_pos, v_normal, v_uv, normalize(s.normal), v_is_front), v_world_pos, s.ao, params, s.toon_indices, v_uv, toon_lanes, push.material_index, desaturation);
    const float3 lit = colour + s.emissive;
    const float3 lit_final = goo_hsv_desaturate(lit, desaturation);
    // the BLEND rule, copied from `character_forward.slang`'s own entry: an alphaMode BLEND
    // material carries its own coverage, every other material is fully opaque.
    const bool toon_transparent = (s.flags & 256u) != 0u;
    const float out_alpha = ((s.flags & 32u) != 0u || toon_transparent) ? s.alpha : 1.0;
    return float4(lit_final, out_alpha);
}
