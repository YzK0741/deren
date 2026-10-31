#ifndef DEREN_GAME_TOON_MATH_H
#define DEREN_GAME_TOON_MATH_H
#ifdef __cplusplus
#include <glm/glm.hpp>
#include <cmath>
#include <algorithm>
using WfFloat3 = glm::vec3;
#define WF_INLINE inline
#define WF_MIX glm::mix
#define WF_DOT glm::dot
#define WF_MAX std::max
#define WF_MIN std::min
#define WF_SIN std::sin
#define WF_COS std::cos
#define WF_ASIN std::asin
#define WF_ATAN2 std::atan2
#else
#define WfFloat3 float3
#define WF_INLINE
#define WF_MIX lerp
#define WF_DOT dot
#define WF_MAX max
#define WF_MIN min
#define WF_SIN sin
#define WF_COS cos
#define WF_ASIN asin
#define WF_ATAN2 atan2
#endif
WF_INLINE WfFloat3 wf_skin_light(WfFloat3 skin_light, WfFloat3 shadow_tint, float lit, float brightness) {
    WfFloat3 direct = skin_light * brightness;
    float y = WF_DOT(direct, WfFloat3(.212673f,.715152f,.072175f));
    return WF_MIX(shadow_tint,WfFloat3(1.f),lit) * (.725f*.2877f) * skin_light +
           WF_MIX(WfFloat3(y),direct,lit);
}
WF_INLINE WfFloat3 wf_camera_light(WfFloat3 forward, float yaw_degrees, float min_pitch_degrees) {
    float radians = .017453292519943295f;
    float yaw = WF_ATAN2(-forward.x,forward.z) + yaw_degrees*radians;
    float fy = WF_MAX(-1.f,WF_MIN(1.f,-forward.y));
    float pitch = WF_MAX(min_pitch_degrees*radians,WF_ASIN(fy));
    return WfFloat3(WF_SIN(yaw)*WF_COS(pitch),WF_SIN(pitch),-WF_COS(yaw)*WF_COS(pitch));
}
#undef WF_INLINE
#undef WF_MIX
#undef WF_DOT
#undef WF_MAX
#undef WF_MIN
#undef WF_SIN
#undef WF_COS
#undef WF_ASIN
#undef WF_ATAN2
#ifndef __cplusplus
#undef WfFloat3
#endif
#endif
