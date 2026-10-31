#ifndef DEREN_GAME_TOON_TONEMAP_H
#define DEREN_GAME_TOON_TONEMAP_H

#ifdef __cplusplus
#include <glm/glm.hpp>
using GameToonFloat3 = glm::vec3;
#define GAME_TOON_INLINE inline
#define GAME_TOON_DOT glm::dot
#define GAME_TOON_MAX glm::max
#define GAME_TOON_MIN glm::min
#define GAME_TOON_CLAMP glm::clamp
#else
#define GameToonFloat3 float3
#define GAME_TOON_INLINE
#define GAME_TOON_DOT dot
#define GAME_TOON_MAX max
#define GAME_TOON_MIN min
#define GAME_TOON_CLAMP clamp
#endif

// Captured GameToon viewer's Q332-disabled tone-map block, preserved in
// tmp/game_toon-studio-audit/final/shaders/viewer-tonemapping.glsl.
// Linear HDR in, bounded linear display color out. Display encoding belongs
// to the existing post pass/sRGB attachment, and must happen only once.
// Explicit row dot products preserve GLSL's matrix orientation.
GAME_TOON_INLINE GameToonFloat3 game_toon_tone_mapping(GameToonFloat3 color)
{
    const GameToonFloat3 luma = GameToonFloat3(0.272229f, 0.674082f, 0.053689f);
    GameToonFloat3 working = GAME_TOON_MAX(GameToonFloat3(
        GAME_TOON_DOT(GameToonFloat3(0.613192f, 0.339512f, 0.047366f), color),
        GAME_TOON_DOT(GameToonFloat3(0.070206f, 0.916336f, 0.013450f), color),
        GAME_TOON_DOT(GameToonFloat3(0.020619f, 0.109568f, 0.869606f), color)), 0.0f);
    const float toe = GAME_TOON_CLAMP(GAME_TOON_DOT(luma, working) / 0.3f, 0.0f, 1.0f);
    working *= 1.0f - 0.105f * (1.0f - toe * toe * (3.0f - 2.0f * toe));
    const float working_luma = GAME_TOON_DOT(luma, working);
    working = GAME_TOON_MAX(working_luma + 1.08f * (working - working_luma), 0.0f);

    GameToonFloat3 mapped = GAME_TOON_MIN(working * (2.785085f * working + 0.107772f) /
        GAME_TOON_MAX(working * (2.936045f * working + 0.887122f) + 0.806889f, 1e-4f), 1.0f);
    const float mapped_luma = GAME_TOON_DOT(luma, mapped);
    mapped = mapped_luma + 0.93f * (mapped - mapped_luma);
    const GameToonFloat3 display = GameToonFloat3(
        GAME_TOON_DOT(GameToonFloat3(1.705052f, -0.621791f, -0.083259f), mapped),
        GAME_TOON_DOT(GameToonFloat3(-0.130257f, 1.140803f, -0.010548f), mapped),
        GAME_TOON_DOT(GameToonFloat3(-0.024003f, -0.128969f, 1.152972f), mapped));
    const float shoulder = GAME_TOON_CLAMP((GAME_TOON_DOT(luma, working) - 0.5f) / 1.5f, 0.0f, 1.0f);
    const GameToonFloat3 normalized = GAME_TOON_CLAMP(display /
        GAME_TOON_MAX(GAME_TOON_MAX(display.x, GAME_TOON_MAX(display.y, display.z)), 1e-5f), 0.0f, 1.0f);
    return GAME_TOON_CLAMP(display + shoulder * (normalized - display), 0.0f, 1.0f);
}

#undef GAME_TOON_INLINE
#undef GAME_TOON_DOT
#undef GAME_TOON_MAX
#undef GAME_TOON_MIN
#undef GAME_TOON_CLAMP
#ifndef __cplusplus
#undef GameToonFloat3
#endif
#endif
