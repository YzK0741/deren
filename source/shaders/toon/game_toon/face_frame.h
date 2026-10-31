#ifndef DEREN_GAME_FACE_FRAME_H
#define DEREN_GAME_FACE_FRAME_H
#ifdef __cplusplus
#define FACE_FRAME_INLINE inline
#else
#define FACE_FRAME_INLINE
#endif
// Game object matrix local +X opposes deren anatomical head.right (-X for a +Z face).
// This reflection converts components in either direction; front/up remain unchanged.
FACE_FRAME_INLINE float game_face_object_x(float head_right_component) { return -head_right_component; }
FACE_FRAME_INLINE float game_face_sdf_mirror(float object_x) { return object_x > 0.f ? 1.f : 0.f; }
FACE_FRAME_INLINE float game_face_lut_v(float srgb_green) { return (srgb_green*31.f + .5f)/32.f; }
// The archive exporter saves Texture2D PNGs with ConvertToImage(flip: true).
// Keep the game's raw coordinate above, and bridge its storage convention here.
FACE_FRAME_INLINE float game_face_exported_lut_v(float srgb_green) { return 1.f - game_face_lut_v(srgb_green); }
#undef FACE_FRAME_INLINE
#endif
