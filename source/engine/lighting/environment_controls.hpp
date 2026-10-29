#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace deren::engine::environment_detail {
    inline bool valid_half_radiance(std::span<float const> values) noexcept {
        return std::ranges::all_of(values, [](float v) { return std::isfinite(v) && v >= 0.0f && v <= 65504.0f; });
    }
    inline std::array<float, 4> pack_environment(float degrees, float strength, int mode) noexcept {
        degrees = std::isfinite(degrees) ? std::remainder(degrees, 360.0f) : 0.0f;
        strength = std::isfinite(strength) ? std::clamp(strength, 0.0f, 64.0f) : 1.0f;
        float const radians = degrees * 0.017453292519943295f;
        return {std::cos(radians), std::sin(radians), strength, mode >= 0 && mode <= 3 ? float(mode) : 0.0f};
    }
    inline std::array<float, 4> pack_background(std::array<float, 3> color, float blur) noexcept {
        for (auto& value : color) value = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.18f;
        return {color[0], color[1], color[2], std::isfinite(blur) ? std::clamp(blur, 0.0f, 1.0f) : 0.5f};
    }
}
