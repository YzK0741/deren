// Headless unit tests: vulkan.math module (pure CPU - IBL precompute helpers) ===
#include "vk_test.h"

#include <cmath>
#include <cstddef>
#include <glm/glm.hpp>
#include <vector>

import vulkan.math;
import vulkan.primitive; // orbit_camera_pan_delta: the arrow-key camera pan

namespace {
    constexpr std::size_t cubemap_float_count(int32_t size) {
        return static_cast<std::size_t>(6) * static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4;
    }

    void test_environment_cubemap_shape() {
        std::vector<float> const env = vulkan::generate_environment_cubemap(16);
        CHECK(env.size() == cubemap_float_count(16));
        bool any_positive = false;
        bool all_finite = true;
        for (float const value : env) {
            all_finite &= std::isfinite(value) != 0;
            any_positive |= value > 0.0f;
        }
        CHECK(all_finite);
        CHECK(any_positive); // a light source exists somewhere in the procedural sky
    }

    void test_irradiance_map_shape() {
        std::vector<float> const env = vulkan::generate_environment_cubemap(8);
        std::vector<float> const irr = vulkan::generate_irradiance_map(env, 8, 4);
        CHECK(irr.size() == cubemap_float_count(4));
    }

    void test_brdf_lut_shape() {
        // RG32F: scale + bias per texel
        std::vector<float> const lut = vulkan::generate_brdf_lut(16);
        CHECK(lut.size() == static_cast<std::size_t>(16) * 16 * 2);
    }

    // The prefiltered environment must be SMOOTH, not just average the right colour: one coarse-level
    // texel covers a large area on screen, so an isolated bright texel (the sun disc landing on some
    // Monte-Carlo taps and missing their neighbours) reads as a big highlight patch sweeping across a
    // rotating metal surface. The variant this replaced had such texels - at 16^2 the brightest texel
    // was 7x the level mean and 2.3x its own 3x3 neighbourhood. Sampling a source mip per tap fixes
    // it, and these bounds are the regression test: they fail loudly on the old behaviour and hold
    // with room to spare on the new one.
    void test_prefiltered_environment_is_smooth() {
        constexpr int32_t env_size = 256;
        constexpr int32_t mip_count = 5;
        std::vector<float> const env = vulkan::generate_environment_cubemap(env_size);
        std::vector<float> const prefiltered = vulkan::prefilter_environment(env, env_size, mip_count);
        std::size_t cursor = 0;
        for (int32_t mip = 0; mip < mip_count; ++mip) {
            int32_t const size = std::max(1, env_size >> mip);
            auto const luminance = [&](int32_t const face, int32_t const x, int32_t const y) {
                std::size_t const at = cursor + (static_cast<std::size_t>(face) * size * size + static_cast<std::size_t>(y) * size + x) * 4;
                return 0.2126 * prefiltered[at] + 0.7152 * prefiltered[at + 1] + 0.0722 * prefiltered[at + 2];
            };
            double sum = 0.0;
            double brightest = 0.0;
            double worst_local_ratio = 0.0;
            int32_t counted = 0;
            for (int32_t face = 0; face < 6; ++face) {
                for (int32_t y = 1; y < size - 1; ++y) {
                    for (int32_t x = 1; x < size - 1; ++x) {
                        double const centre = luminance(face, x, y);
                        double const neighbourhood = (luminance(face, x - 1, y - 1) + luminance(face, x, y - 1) + luminance(face, x + 1, y - 1) +
                                                      luminance(face, x - 1, y) + luminance(face, x + 1, y) +
                                                      luminance(face, x - 1, y + 1) + luminance(face, x, y + 1) + luminance(face, x + 1, y + 1)) /
                                                     8.0;
                        sum += centre;
                        counted += 1;
                        brightest = std::max(brightest, centre);
                        if (neighbourhood > 1e-6) {
                            worst_local_ratio = std::max(worst_local_ratio, centre / neighbourhood);
                        }
                    }
                }
            }
            double const mean = sum / static_cast<double>(counted);
            // Levels 0 and 1 keep the sun disc itself (a real, smooth feature); the coarse levels are
            // where an unsmoothed disc shows up as a hotspot.
            // The bounds separate the two variants by measurement, not by taste: brightest/mean per
            // coarse level was 3.93 / 3.67 / 7.10 with the old level-0 sampling and 3.10 / 2.4 / 2.5
            // with the source-mip one, the local ratio 1.20 / 1.81 / 2.27 against 1.1 / 1.1 / 1.3.
            if (mip >= 2) {
                CHECK(brightest <= 3.5 * mean);
                CHECK(worst_local_ratio <= 1.5);
            }
            cursor += static_cast<std::size_t>(6) * size * size * 4;
        }
        CHECK(cursor == prefiltered.size());
    }
} // namespace

// ---- ARROW-KEY CAMERA PAN (vulkan::orbit_camera_pan_delta) ----
// The pan is the keyboard's camera movement. Three properties are the contract the runtime depends on:
// a frame with no arrow held must not touch the camera AT ALL (an idle frame stays byte-identical, which
// is how the pan stays provably inert for the pinned render frames), a diagonal press must not be faster
// than a straight one, and the step must be speed x dt with the speed tied to the orbit distance and dt
// clamped, so a stalled frame cannot teleport the rig. The expected vectors are recomputed here from the
// orbit sphere (eye = target + distance * (cp*sin yaw, sin pitch, cp*cos yaw)) independently of the
// implementation - the view direction's horizontal part and its right vector, for three yaws.
void test_orbit_camera_pan() {
    constexpr float half_pi = 1.5707963267948966f;
    struct frame_case {
        float yaw;
        glm::vec3 forward; // where the camera looks (horizontal), i.e. what WALK = +1 moves along
        glm::vec3 right;   // cross(forward, world up), i.e. what STRAFE = +1 moves along
    };
    frame_case const frame[] = {
        {0.0f, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}},
        {half_pi, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
        {3.0f, {-std::sin(3.0f), 0.0f, -std::cos(3.0f)}, {std::cos(3.0f), 0.0f, -std::sin(3.0f)}},
    };

    constexpr float distance = 2.0f; // above the 0.1 floor, so the speed is distance * 0.75
    constexpr float clamped_dt = 0.25f;
    float const step = distance * 0.75f * clamped_dt;

    for (frame_case const& c : frame) {
        // dt = 4 s is clamped to 0.25 s, so the step is the same as a well-paced frame's.
        glm::vec3 const forward_move = vulkan::orbit_camera_pan_delta(c.yaw, distance, 0.0f, 1.0f, 4.0f, false);
        glm::vec3 const right_move = vulkan::orbit_camera_pan_delta(c.yaw, distance, 1.0f, 0.0f, 4.0f, false);
        CHECK(glm::length(forward_move - c.forward * step) < 1e-5f);
        CHECK(glm::length(right_move - c.right * step) < 1e-5f);
        CHECK(forward_move.y == 0.0f); // the pan is horizontal (the pitch is not an input to it)
    }

    // No arrow held: EXACTLY zero, so an idle frame never writes the target.
    CHECK(vulkan::orbit_camera_pan_delta(0.7f, 2.0f, 0.0f, 0.0f, 0.016f, false) == glm::vec3(0.0f));
    // A zero step is zero too (the first frame has no previous clock reading).
    CHECK(vulkan::orbit_camera_pan_delta(0.7f, 2.0f, 1.0f, 1.0f, 0.0f, false) == glm::vec3(0.0f));

    // The two axes are normalized TOGETHER: a diagonal press is not sqrt(2) times faster.
    float const straight = glm::length(vulkan::orbit_camera_pan_delta(0.4f, 2.0f, 1.0f, 0.0f, 0.016f, false));
    float const diagonal = glm::length(vulkan::orbit_camera_pan_delta(0.4f, 2.0f, 1.0f, 1.0f, 0.016f, false));
    CHECK(std::abs(straight - diagonal) < 1e-6f);

    // SHIFT multiplies by exactly 4, and the 0.1 floor keeps a fully zoomed-in rig (distance 0) moving.
    float const fast = glm::length(vulkan::orbit_camera_pan_delta(0.4f, 2.0f, 1.0f, 0.0f, 0.016f, true));
    CHECK(std::abs(fast - 4.0f * straight) < 1e-6f);
    CHECK(glm::length(vulkan::orbit_camera_pan_delta(0.4f, 0.0f, 1.0f, 0.0f, 1.0f, false)) > 0.0f);
}

int32_t main() {
    test_environment_cubemap_shape();
    test_irradiance_map_shape();
    test_brdf_lut_shape();
    test_prefiltered_environment_is_smooth();
    test_orbit_camera_pan();
    return vk_test::finish("test_math");
}
