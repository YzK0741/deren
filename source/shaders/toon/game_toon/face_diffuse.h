#ifndef DEREN_GAME_FACE_DIFFUSE_H
#define DEREN_GAME_FACE_DIFFUSE_H
#ifdef __cplusplus
#include <algorithm>
#include <glm/glm.hpp>
using FaceFloat3 = glm::vec3;
#define FACE_INLINE inline
#define FACE_MIX glm::mix
#define FACE_DOT glm::dot
#define FACE_MIN std::min
#define FACE_MAX std::max
#else
#define FaceFloat3 float3
#define FACE_INLINE
#define FACE_MIX lerp
#define FACE_DOT dot
#define FACE_MIN min
#define FACE_MAX max
#endif
// Shared CPU/Slang material-colour block, from face HLSL _1647/_1653/_1662/_1668.
// Inputs are separate: ring visibility is already in shadow_area; light_visibility is screen X.
struct FaceDiffuseResult { FaceFloat3 colour; float surface_lit; };
struct FaceDirectLight { FaceFloat3 diffuse; FaceFloat3 specular; };
// Original :810 uses _1646 for diffuse, but _1674 for specular (:791).
FACE_INLINE FaceDirectLight game_face_direct_light(FaceFloat3 main_light, float surface_lit) {
    FaceDirectLight result;
    result.diffuse = main_light;
    result.specular = main_light * (surface_lit * 0.5f + 0.5f);
    return result;
}
FACE_INLINE float face_diffuse_luma(FaceFloat3 value) {
    return FACE_DOT(value, FaceFloat3(0.212672904f,0.715152204f,0.0721750036f));
}
FACE_INLINE FaceDiffuseResult game_face_diffuse_composition(FaceFloat3 diffuse, FaceFloat3 shadow_albedo,
    FaceFloat3 ramp, float ramp_alpha, float occlusion, float shadow_area, float mask_y, float light_visibility) {
    float lit_weight=FACE_MIN(FACE_MIN(shadow_area,occlusion),ramp_alpha);
    float lit_shadow=occlusion*shadow_area;
    // Original _1647: dark-in-dark -> LUT albedo -> lit albedo.
    FaceFloat3 dark=shadow_albedo*0.65f;
    dark=FACE_MIX(FaceFloat3(face_diffuse_luma(dark)),dark,1.2f);
    float dark_blend=FACE_MAX(0.f,FACE_MIN(occlusion*((1.f-mask_y)+shadow_area*mask_y)+ramp_alpha,1.f));
    FaceFloat3 base=FACE_MIX(FACE_MIX(dark,shadow_albedo,dark_blend),diffuse,lit_weight);
    // Original _1653 and _1662: tint, then restore base luminance (bounded gain).
    float spread=FACE_MAX(FACE_MAX(ramp.x,ramp.y),ramp.z)-FACE_MIN(FACE_MIN(ramp.x,ramp.y),ramp.z);
    FaceFloat3 tinted=base*FACE_MIX(FaceFloat3(1.f),ramp,spread);
    float gain=FACE_MAX(0.f,FACE_MIN(face_diffuse_luma(base)/FACE_MAX(face_diffuse_luma(tinted),0.001f),1.5f));
    FaceFloat3 contrast=FACE_MIX(FaceFloat3(face_diffuse_luma(diffuse)),diffuse,1.2f);
    FaceDiffuseResult result;
    result.colour=FACE_MIX(FACE_MIX(shadow_albedo,contrast,lit_shadow),tinted*gain,light_visibility);
    result.surface_lit=FACE_MIX(lit_shadow,lit_weight,light_visibility);
    return result;
}
#undef FACE_INLINE
#undef FACE_MIX
#undef FACE_DOT
#undef FACE_MIN
#undef FACE_MAX
#ifndef __cplusplus
#undef FaceFloat3
#endif
#endif
