#ifndef DEREN_GAME_SKIN_MATERIAL_H
#define DEREN_GAME_SKIN_MATERIAL_H
#ifdef __cplusplus
#include <glm/glm.hpp>
#include <algorithm>
using SkinFloat3 = glm::vec3;
#define SKIN_INLINE inline
#define SKIN_MIX glm::mix
#define SKIN_MAX std::max
#else
#define SkinFloat3 float3
#define SKIN_INLINE
#define SKIN_MIX lerp
#define SKIN_MAX max
#endif
struct GameSkinMaterial {
    SkinFloat3 diffuse;
    SkinFloat3 shadow;
    SkinFloat3 f0;
    float roughness2;
};
// Original non-SDF skin :966-969 (_2044/_2045/_2048/_2049/_2051).
SKIN_INLINE GameSkinMaterial game_skin_material(SkinFloat3 albedo, SkinFloat3 lut,
                                               float metallic, float roughness, float specular) {
    GameSkinMaterial result;
    float energy = 0.96f - metallic * 0.96f;
    result.diffuse = albedo * energy;
    result.shadow = lut * energy;
    result.f0 = SKIN_MIX(SkinFloat3(0.04f * specular), albedo, metallic);
    result.roughness2 = SKIN_MAX(roughness * roughness, 0.0078125f);
    return result;
}
#undef SKIN_INLINE
#undef SKIN_MIX
#undef SKIN_MAX
#ifndef __cplusplus
#undef SkinFloat3
#endif
#endif
