#ifndef DEREN_GAME_FACE_RIM_H
#define DEREN_GAME_FACE_RIM_H
#ifdef __cplusplus
#define FACE_RIM_INLINE inline
#else
#define FACE_RIM_INLINE
#endif
// Conservative directional fallback while game rim art-direction globals are absent.
FACE_RIM_INLINE float game_face_rim_light_weight(float light_dot_view, float geometric_no_l) { 
    if (!(light_dot_view < 0.f && geometric_no_l > 0.f)) return 0.f;
    float backlight = -light_dot_view;
    backlight = backlight < 1.f ? backlight : 1.f;
    float facing = geometric_no_l < 1.f ? geometric_no_l : 1.f;
    return backlight * facing;
 }
#undef FACE_RIM_INLINE
#endif
