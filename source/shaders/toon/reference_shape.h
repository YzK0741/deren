#ifndef DEREN_REFERENCE_SHAPE_H
#define DEREN_REFERENCE_SHAPE_H
// Scalar art controls shared by CPU tests and Slang. These are not game-derived formulas.
#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#define SHAPE_INLINE inline
#define SHAPE_MIN std::min
#define SHAPE_MAX std::max
#define SHAPE_POW std::pow
#define SHAPE_FINITE std::isfinite
#define SHAPE_ATAN2 std::atan2
#define SHAPE_ABS std::abs
#else
#define SHAPE_INLINE
#define SHAPE_MIN min
#define SHAPE_MAX max
#define SHAPE_POW pow
#define SHAPE_FINITE isfinite
#define SHAPE_ATAN2 atan2
#define SHAPE_ABS abs
#endif
SHAPE_INLINE bool reference_strand_frame_valid(float strand_squared, float shifted_squared, float half_squared) {
    return SHAPE_FINITE(strand_squared) && SHAPE_FINITE(shifted_squared) && SHAPE_FINITE(half_squared)
        && strand_squared>1e-8f && shifted_squared>1e-8f && half_squared>1e-8f;
}
SHAPE_INLINE float reference_hair_band(float tangent_dot_half, float width) {
    if (!SHAPE_FINITE(tangent_dot_half) || !SHAPE_FINITE(width) || width <= 0.0f) return 0.0f;
    float w=SHAPE_MAX(0.01f,SHAPE_MIN(width,1.0f));
    float sine_squared=SHAPE_MAX(0.0f,1.0f-tangent_dot_half*tangent_dot_half);
    return SHAPE_POW(sine_squared,2.0f/(w*w));
}
SHAPE_INLINE float reference_masked_hair_lobe(float band, float mask, float intensity, float no_v) {
    if (!SHAPE_FINITE(band) || !SHAPE_FINITE(mask) || !SHAPE_FINITE(intensity) || !SHAPE_FINITE(no_v)) return 0.f;
    float view=SHAPE_MAX(0.f,SHAPE_MIN(no_v/0.25f,1.f));
    view=view*view*(3.f-2.f*view);
    return SHAPE_MAX(0.f,SHAPE_MIN(band,SHAPE_MIN(mask,1.f)))*SHAPE_MAX(0.f,SHAPE_MIN(intensity,1.f))*view;
}
SHAPE_INLINE float reference_face_sdf_weight(float sdf, float front, float side) {
    if (!SHAPE_FINITE(sdf) || !SHAPE_FINITE(front) || !SHAPE_FINITE(side) || front*front+side*side<1e-8f) return 1.f;
    float threshold=1.f-SHAPE_ABS(SHAPE_ATAN2(side,-front))/3.14159265358979323846f;
    float t=SHAPE_MAX(0.f,SHAPE_MIN((sdf-threshold+0.04f)/0.08f,1.f));
    return t*t*(3.f-2.f*t);
}
SHAPE_INLINE float reference_nose_weight(float alpha, float amount) {
    if (!SHAPE_FINITE(alpha) || !SHAPE_FINITE(amount))return 0.0f;
    float t=SHAPE_MAX(0.0f,SHAPE_MIN((0.98f-alpha)/0.18f,1.0f));
    return t*t*(3.0f-2.0f*t)*SHAPE_MAX(0.0f,SHAPE_MIN(amount,1.0f));
}
SHAPE_INLINE float reference_nose_diffuse_blend(float alpha, float diffuse_blend_effect, float amount) {
    float original=1.0f-diffuse_blend_effect*(1.0f-alpha);
    float weight=reference_nose_weight(alpha,amount);
    return original+(1.0f-original)*weight;
}
#undef SHAPE_INLINE
#undef SHAPE_MIN
#undef SHAPE_MAX
#undef SHAPE_POW
#undef SHAPE_FINITE
#undef SHAPE_ATAN2
#undef SHAPE_ABS
#endif
