// Headless unit tests: deren.engine.math module (pure CPU - IBL precompute helpers) ===
#include "../engine/lighting/cubemap_sampling.hpp"
#include "../engine/lighting/environment_controls.hpp"
#include "../engine/lighting/radiance_capture.hpp"
#include "../engine/lighting/reflection_probe.hpp"
#include "../shaders/toon/reference_shape.h"
#include "vk_test.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <glm/glm.hpp>
#include <vector>

import deren.engine.math;
import deren.engine.primitive; // orbit_camera_pan_delta: the arrow-key camera pan

namespace {
    void test_radiance_cube_camera_and_pixels() {
        using namespace deren::engine::environment_detail;
        using namespace deren::engine::radiance_detail;
        glm::vec3 const position(2, -3, 5);
        for (unsigned face = 0; face < 6; ++face) {
            auto camera = deren::engine::make_cube_capture_camera(position, face, .05f, 50.0f);
            CHECK(glm::length(camera.camera_pos - position) < 1e-6f);
            auto inv = glm::inverse(camera.proj * camera.view);
            for (float u : {-.75f, 0.0f, .75f})
                for (float v : {-.75f, 0.0f, .75f}) {
                    auto p = inv * glm::vec4(-u, v, 1, 1);
                    auto ray = glm::normalize(glm::vec3(p) / p.w - position);
                    auto expected = cube_direction(face, u, v);
                    CHECK(glm::length(ray - glm::normalize(glm::vec3(expected.x, expected.y, expected.z))) < 2e-4f);
                }
            auto near_point = inv * glm::vec4(0, 0, 0, 1), far_point = inv * glm::vec4(0, 0, 1, 1);
            CHECK(std::abs(glm::length(glm::vec3(near_point) / near_point.w - position) - .05f) < 1e-5f);
            CHECK(std::abs(glm::length(glm::vec3(far_point) / far_point.w - position) - 50.0f) < .05f);
        }
        CHECK(decode_binary16(0x0000) == 0);
        CHECK(decode_binary16(0x3c00) == 1);
        CHECK(decode_binary16(0x5000) == 32);
        CHECK(decode_binary16(0x7bff) == 65504);
        CHECK(decode_binary16(0x8001) == -std::ldexp(1.0f, -24));
        CHECK(std::isinf(decode_binary16(0x7c00)));
        CHECK(std::isnan(decode_binary16(0x7e00)));
        std::array<float, 16> source{1, 2, 3, 2, 4, 5, 6, 2, 7, 8, 9, 2, 10, 11, 12, 2}, result{};
        CHECK(canonicalize_face(source, 2, result));
        CHECK(result[0] == 4 && result[4] == 1 && result[8] == 10 && result[12] == 7);
        CHECK(result[3] == 1 && result[7] == 1 && result[11] == 1 && result[15] == 1);
        source[0] = NAN;
        CHECK(!canonicalize_face(source, 2, result));
        CHECK(!canonicalize_face(source, 0, result));
    }
    void test_cube_filter_continuity() {
        using namespace deren::engine::environment_detail;
        auto max_delta = [](cube_vector a, cube_vector b) { return std::max({std::abs(a.x - b.x), std::abs(a.y - b.y), std::abs(a.z - b.z)}); };
        struct edge_mapping {
            int face, edge;
            bool reverse;
        };
        constexpr edge_mapping mappings[6][4] = {
            {{4, 1, false}, {5, 0, false}, {2, 1, true}, {3, 1, false}},
            {{5, 1, false}, {4, 0, false}, {2, 0, false}, {3, 0, true}},
            {{1, 2, false}, {0, 2, true}, {5, 2, true}, {4, 2, false}},
            {{1, 3, true}, {0, 3, false}, {4, 3, false}, {5, 3, true}},
            {{1, 1, false}, {0, 0, false}, {2, 3, false}, {3, 2, false}},
            {{0, 1, false}, {1, 0, false}, {2, 2, true}, {3, 3, true}},
        };
        for (int size : {1, 2, 4, 16}) {
            bool bounded = true;
            auto fetch = [&](int face, int x, int y) {
                bounded &= face >= 0 && face < 6 && x >= 0 && x < size && y >= 0 && y < size;
                return cube_vector{float(face + 1), 3.0f * face + (x + .5f) / size, .3f * face + (y + .5f) / size};
            };
            float edge_error = 0, corner_error = 0;
            for (int face = 0; face < 6; ++face)
                for (int edge = 0; edge < 4; ++edge)
                    for (float along : {-.99f, -.5f, 0.0f, .5f, .99f}) {
                        float u = edge == 0 ? -1.0f : edge == 1 ? 1.0f
                                                                : along;
                        float v = edge == 2 ? -1.0f : edge == 3 ? 1.0f
                                                                : along;
                        auto a = cube_direction(face, edge < 2 ? u * (1 - 1e-6f) : u, edge >= 2 ? v * (1 - 1e-6f) : v);
                        auto b = cube_direction(face, edge < 2 ? u * (1 + 1e-6f) : u, edge >= 2 ? v * (1 + 1e-6f) : v);
                        edge_error = std::max(edge_error, max_delta(cube_sample_level(fetch, size, a), cube_sample_level(fetch, size, b)));
                    }
            for (float sx : {-1.0f, 1.0f})
                for (float sy : {-1.0f, 1.0f})
                    for (float sz : {-1.0f, 1.0f}) {
                        auto a = cube_sample_level(fetch, size, cube_vector{sx * (1 + 1e-6f), sy, sz});
                        auto b = cube_sample_level(fetch, size, cube_vector{sx, sy * (1 + 1e-6f), sz});
                        auto c = cube_sample_level(fetch, size, cube_vector{sx, sy, sz * (1 + 1e-6f)});
                        corner_error = std::max({corner_error, max_delta(a, b), max_delta(b, c), max_delta(c, a)});
                    }
            CHECK_MSG(edge_error < .002f, "all twelve cube edges remain continuous when the selected major face changes");
            CHECK_MSG(corner_error < .002f, "all eight cube corners have the same three-face limit");
            CHECK(bounded);
            float orientation_error = 0, center_error = 0, constant_error = 0;
            for (int face = 0; face < 6; ++face)
                for (int edge = 0; edge < 4; ++edge)
                    for (int i = 0; i < size; ++i) {
                        int x = edge == 0 ? -1 : edge == 1 ? size
                                                           : i;
                        int y = edge == 2 ? -1 : edge == 3 ? size
                                                           : i;
                        auto m = mappings[face][edge];
                        int along = m.reverse ? size - 1 - i : i;
                        int nx = m.edge == 0 ? 0 : m.edge == 1 ? size - 1
                                                               : along;
                        int ny = m.edge == 2 ? 0 : m.edge == 3 ? size - 1
                                                               : along;
                        orientation_error = std::max(orientation_error, max_delta(cube_border_texel(fetch, size, face, x, y), fetch(m.face, nx, ny)));
                    }
            auto constant = [](int, int, int) { return cube_vector{.25f, 1, 4}; };
            for (int face = 0; face < 6; ++face)
                for (int y = 0; y < size; ++y)
                    for (int x = 0; x < size; ++x) {
                        auto d = cube_direction(face, (x + .5f) / size * 2 - 1, (y + .5f) / size * 2 - 1);
                        center_error = std::max(center_error, max_delta(cube_sample_level(fetch, size, d), fetch(face, x, y)));
                        constant_error = std::max(constant_error, max_delta(cube_sample_level(constant, size, d), {.25f, 1, 4}));
                    }
            CHECK(orientation_error < 1e-6f);
            CHECK(center_error < 1e-5f);
            CHECK(constant_error < 1e-6f);
            CHECK(bounded);
            auto face_constant = [](int face, int, int) { return cube_vector{float(face + 1), 0, 0}; };
            CHECK(std::abs(cube_sample_level(face_constant, size, {1, 1, 0}).x - 2.0f) < 1e-5f);
            CHECK(std::abs(cube_sample_level(face_constant, size, {1, 1, 1}).x - 3.0f) < 1e-5f);
        }
    }
    constexpr std::size_t cubemap_float_count(int32_t size) {
        return static_cast<std::size_t>(6) * static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4;
    }

    void test_environment_cubemap_shape() {
        deren::utility::vessel::vector<float> const env = deren::engine::generate_environment_cubemap(16);
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
        deren::utility::vessel::vector<float> const env = deren::engine::generate_environment_cubemap(8);
        deren::utility::vessel::vector<float> const irr = deren::engine::generate_irradiance_map(env, 8, 4);
        CHECK(irr.size() == cubemap_float_count(4));
    }

    // ---- THE EQUIRECTANGULAR ENVIRONMENT (deren::engine::generate_environment_cubemap_from_equirect) ----
    //
    // The two axes of the lat-long mapping are the only thing here that can go wrong silently and neither
    // is visible in a frame as an error: a flipped v lights the ground from the ceiling, a flipped u rotates
    // the whole ambience. So the convention is pinned with sources whose texels SAY where they are, and the
    // assertions read the result back through the ONE thing that is part of the output's documented layout -
    // the face order (+X -X +Y -Y +Z -Z) - rather than through a second copy of the direction math.
    //
    // Source A is a ramp: red = the column's u, green = the row's v (row 0 = the image's top row, which is
    // the decoder's order). Its face means are arithmetic:
    //   +X face: u over 0.625..0.875 -> mean 0.75        -X face: 0.125..0.375 -> mean 0.25
    //   +Z face: u over 0.375..0.625 -> mean 0.50        -Z face: straddles the seam -> values near 0.125
    //                                                                  on one half and 0.875 on the other
    //   +Y face: v over 0..0.196 -> mean ~0.10 (the cap around the zenith)
    //   -Y face: v over 0.804..1 -> mean ~0.90 (the cap around the nadir, i.e. the image's bottom rows)
    // The u direction (u grows toward +X) and the up axis (v = 0 is +Y) are both fixed by those numbers.
    // Sources B and C are single bright stripes - at the image's centre column and at its seam column - and
    // must light exactly one face each (+Z and -Z respectively), which is what makes the horizontal origin
    // and the wrap unambiguous.
    deren::utility::vessel::vector<float> make_equirect_ramp(int32_t const width, int32_t const height) {
        deren::utility::vessel::vector<float> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        for (int32_t y = 0; y < height; ++y) {
            for (int32_t x = 0; x < width; ++x) {
                std::size_t const at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
                pixels[at + 0] = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);  // u
                pixels[at + 1] = (static_cast<float>(y) + 0.5f) / static_cast<float>(height); // v, row 0 = top
                pixels[at + 2] = 0.25f;                                                       // a channel the mapping must not touch
                pixels[at + 3] = 1.0f;
            }
        }
        return pixels;
    }

    deren::utility::vessel::vector<float> make_equirect_stripe(int32_t const width, int32_t const height, float const u_centre, float const half_width) {
        deren::utility::vessel::vector<float> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0.0f);
        for (int32_t y = 0; y < height; ++y) {
            for (int32_t x = 0; x < width; ++x) {
                float const u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
                float const distance = std::abs(u - u_centre);
                float const wrapped = std::min(distance, 1.0f - distance); // the stripe wraps with the image
                if (wrapped > half_width) {
                    continue;
                }
                std::size_t const at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
                pixels[at + 0] = 1.0f;
                pixels[at + 3] = 1.0f;
            }
        }
        return pixels;
    }

    struct face_ramp_stats {
        double mean_r;
        double mean_g;
        double min_r;
        double max_r;
    };

    face_ramp_stats face_stats(deren::utility::vessel::vector<float> const& cube, int32_t const size, int32_t const face) {
        face_ramp_stats stats{0.0, 0.0, 1.0, 0.0};
        int32_t const count = size * size;
        for (int32_t i = 0; i < count; ++i) {
            std::size_t const at = (static_cast<std::size_t>(face) * count + static_cast<std::size_t>(i)) * 4;
            stats.mean_r += cube[at + 0];
            stats.mean_g += cube[at + 1];
            stats.min_r = std::min(stats.min_r, static_cast<double>(cube[at + 0]));
            stats.max_r = std::max(stats.max_r, static_cast<double>(cube[at + 0]));
        }
        stats.mean_r /= static_cast<double>(count);
        stats.mean_g /= static_cast<double>(count);
        return stats;
    }

    void test_equirect_environment_axes() {
        constexpr int32_t source_width = 128;
        constexpr int32_t source_height = 64;
        constexpr int32_t size = 16;
        deren::utility::vessel::vector<float> const ramp = make_equirect_ramp(source_width, source_height);
        deren::utility::vessel::vector<float> const cube = deren::engine::generate_environment_cubemap_from_equirect(ramp, source_width, source_height, size);
        CHECK(cube.size() == cubemap_float_count(size));

        for (std::size_t i = 0; i + 3 < cube.size(); i += 4) {
            CHECK(std::isfinite(cube[i]) && std::isfinite(cube[i + 1]) && std::isfinite(cube[i + 2]));
            CHECK(cube[i + 3] == 1.0f);                   // alpha is filled, not sampled
            CHECK(std::abs(cube[i + 2] - 0.25f) < 1e-6f); // blue passes through untouched
        }

        face_ramp_stats const px = face_stats(cube, size, 0);
        face_ramp_stats const nx = face_stats(cube, size, 1);
        face_ramp_stats const py = face_stats(cube, size, 2);
        face_ramp_stats const ny = face_stats(cube, size, 3);
        face_ramp_stats const pz = face_stats(cube, size, 4);
        face_ramp_stats const nz = face_stats(cube, size, 5);
        CHECK_MSG(px.mean_r > 0.70 && px.mean_r < 0.80, "+X face samples u around 0.75 (u grows toward +X)");
        CHECK_MSG(nx.mean_r > 0.20 && nx.mean_r < 0.30, "-X face samples u around 0.25");
        CHECK_MSG(pz.mean_r > 0.45 && pz.mean_r < 0.55, "+Z face samples u around 0.5 (the image's centre column)");
        CHECK_MSG(nz.min_r < 0.20 && nz.max_r > 0.80, "-Z face straddles the image's seam, where u wraps from 1 to 0");
        // The two caps are the v axis. Their means are NOT 0 and 1: a face spans 45..90 degrees of
        // elevation, so - with texel centres uniform over the face's square - the caps average y = +-0.775,
        // i.e. v = acos(0.775)/pi = 0.218 for +Y and 0.787 for -Y. The bounds below are loose around those
        // two values on purpose; what they pin is that the pair STRADDLES the middle and sits on the
        // expected side of it. A flipped v would swap them and fail both.
        CHECK_MSG(py.mean_g < 0.30, "+Y samples the image's FIRST rows: v = 0 is the zenith");
        CHECK_MSG(ny.mean_g > 0.70, "-Y samples the image's LAST rows: v = 1 is the nadir");
        CHECK_MSG(ny.mean_g - py.mean_g > 0.5, "the two caps are half an image apart, not nearly equal");
        // and the pair whose means would be equal if the mapping were handedness-flipped
        CHECK(px.mean_r > nx.mean_r);

        // The stripes: the centre column lights +Z and nothing else; the seam column lights -Z and nothing
        // else. Together they fix the horizontal origin AND the wrap in one measurement each. Only the four
        // SIDE faces are inspected: a cap spans every azimuth (its corners reach all four quadrants), so it
        // sees every u value somewhere and cannot tell the stripes apart. The stripes are several columns
        // wide - a sub-texel stripe is cut in half by the bilinear sample and reads as ~0.5, which is
        // indistinguishable from "no stripe here".
        auto const stripe_lights_only = [&](deren::utility::vessel::vector<float> const& stripe_cube, int32_t const expected) {
            for (int32_t const face : {0, 1, 4, 5}) {
                face_ramp_stats const stats = face_stats(stripe_cube, size, face);
                if (face == expected) {
                    CHECK_MSG(stats.max_r > 0.9, "the stripe's own face sees it");
                } else {
                    CHECK_MSG(stats.max_r < 0.01, "no other SIDE face sees the stripe");
                }
            }
        };
        stripe_lights_only(deren::engine::generate_environment_cubemap_from_equirect(make_equirect_stripe(source_width, source_height, 0.5f, 4.0f / source_width), source_width, source_height, size), 4);
        stripe_lights_only(deren::engine::generate_environment_cubemap_from_equirect(make_equirect_stripe(source_width, source_height, 0.0f, 4.0f / source_width), source_width, source_height, size), 5);

        // The intensity is a plain multiplier on every texel (the reference's world_strength). The ALPHA
        // channel is the exception: it is filled with 1 by both runs, so it is compared for equality rather
        // than for the ratio - a scaled alpha would be a wasted channel and a silent precision loss.
        deren::utility::vessel::vector<float> const doubled = deren::engine::generate_environment_cubemap_from_equirect(ramp, source_width, source_height, size, 4.0f);
        CHECK(doubled.size() == cube.size());
        for (std::size_t i = 0; i < cube.size(); i += 4) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                CHECK(std::abs(doubled[i + channel] - 4.0f * cube[i + channel]) < 1e-4f);
            }
            CHECK(doubled[i + 3] == cube[i + 3]);
        }

        // A degenerate source is a zero buffer of the right shape, never a read out of bounds: the caller
        // panics long before this, and this is the belt to that pair of braces.
        deren::utility::vessel::vector<float> const degenerate = deren::engine::generate_environment_cubemap_from_equirect({}, 0, 0, 4);
        CHECK(degenerate.size() == cubemap_float_count(4));
        CHECK(std::all_of(degenerate.begin(), degenerate.end(), [](float const value) { return value == 0.0f; }));
    }

    void test_brdf_lut_shape() {
        // RG32F: scale + bias per texel
        deren::utility::vessel::vector<float> const lut = deren::engine::generate_brdf_lut(16);
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
        deren::utility::vessel::vector<float> const env = deren::engine::generate_environment_cubemap(env_size);
        deren::utility::vessel::vector<float> const prefiltered = deren::engine::prefilter_environment(env, env_size, mip_count);
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
    // ---- reflection probe math (source/engine/lighting/reflection_probe.hpp) ----
    // The probe answers "which way does this surface point's reflection land in the probe, and how much
    // does it count". Both halves are pure functions over a box: the runtime validates a probe box at
    // load and then calls lookup per fragment, so the reject cases must not be silently permissive.
    void test_reflection_probe_math() {
        using namespace deren::engine::probe_detail;
        box probe;
        probe.position = {0.f, 0.f, 0.f};
        probe.minimum = {-1.f, -1.f, -1.f};
        probe.maximum = {1.f, 1.f, 1.f};
        probe.fade = 0.5f;
        probe.gain = 1.f;
        CHECK(valid(probe));

        // the position must sit STRICTLY inside the box: on a wall or outside it is a loader error
        box on_the_wall = probe;
        on_the_wall.position = {1.f, 0.f, 0.f};
        CHECK(!valid(on_the_wall));
        box outside = probe;
        outside.position = {2.f, 0.f, 0.f};
        CHECK(!valid(outside));
        box inverted = probe;
        inverted.minimum = {2.f, -1.f, -1.f};
        CHECK(!valid(inverted));
        box bad_fade = probe;
        bad_fade.fade = -1.f;
        CHECK(!valid(bad_fade));
        box bad_gain = probe;
        bad_gain.gain = NAN;
        CHECK(!valid(bad_gain));

        // a lookup that cannot describe a reflection answers the null sample: a degenerate direction, a
        // point outside the box, a non-finite point, or an invalid box
        CHECK(lookup(probe, {0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}).weight == 0.f);
        CHECK(lookup(probe, {2.f, 0.f, 0.f}, {0.f, 0.f, -1.f}).weight == 0.f);
        CHECK(lookup(probe, {NAN, 0.f, 0.f}, {0.f, 0.f, -1.f}).weight == 0.f);
        CHECK(lookup(bad_gain, {0.f, 0.f, 0.f}, {0.f, 0.f, -1.f}).weight == 0.f);

        // a valid lookup: a finite direction, and a weight that is a smoothstep of the distance to the
        // nearest wall - 1 at the centre, 0 on the wall itself, 0.5 halfway to the centre
        auto const centre = lookup(probe, {0.f, 0.f, 0.f}, {0.f, 0.f, -1.f});
        CHECK(centre.weight == 1.f);
        CHECK(std::isfinite(centre.direction.x) && std::isfinite(centre.direction.y) && std::isfinite(centre.direction.z));
        auto const wall = lookup(probe, {1.f, 0.f, 0.f}, {0.f, 0.f, -1.f});
        CHECK(wall.weight == 0.f);
        CHECK(std::isfinite(wall.direction.x) && std::isfinite(wall.direction.y) && std::isfinite(wall.direction.z));
        auto const halfway = lookup(probe, {0.75f, 0.f, 0.f}, {0.f, 0.f, -1.f});
        CHECK(std::abs(halfway.weight - 0.5f) < 1e-6f);
    }

} // namespace

// ---- ARROW-KEY CAMERA PAN (deren::engine::orbit_camera_pan_delta) ----
// The pan is the keyboard's camera movement. Four properties are the contract the runtime depends on:
// a frame with no arrow held must not touch the camera AT ALL (an idle frame stays byte-identical, which
// is how the pan stays provably inert for the pinned render frames), a diagonal press must not be faster
// than a straight one, the step must be speed x dt with the speed tied to the orbit distance and dt
// clamped so a stalled frame cannot teleport the rig, and the two axes must stay the gestures the keys
// name: LEFT/RIGHT strafes horizontally while UP/DOWN RISES ALONG WORLD UP. The last one is a rejection
// recorded as a test: the first cut moved UP/DOWN along the horizontal view direction, which slides the eye
// toward or away from the subject and therefore reads as a zoom. The expected vectors are recomputed here
// from the orbit sphere (eye = target + distance * (cp*sin yaw, sin pitch, cp*cos yaw)) independently of
// the implementation - the right vector and world up, for three yaws.
void test_orbit_camera_pan() {
    constexpr float half_pi = 1.5707963267948966f;
    struct frame_case {
        float yaw;
        glm::vec3 right; // cross(horizontal view direction, world up), i.e. what STRAFE = +1 moves along
    };
    frame_case const frame[] = {
        {0.0f, {1.0f, 0.0f, 0.0f}},
        {half_pi, {0.0f, 0.0f, -1.0f}},
        {3.0f, {std::cos(3.0f), 0.0f, -std::sin(3.0f)}},
    };
    glm::vec3 const up(0.0f, 1.0f, 0.0f); // what RISE = +1 moves along, at every yaw and every pitch

    constexpr float distance = 2.0f; // above the 0.1 floor, so the speed is distance * 0.75
    constexpr float clamped_dt = 0.25f;
    float const step = distance * 0.75f * clamped_dt;

    for (frame_case const& c : frame) {
        // dt = 4 s is clamped to 0.25 s, so the step is the same as a well-paced frame's.
        glm::vec3 const rise_move = deren::engine::orbit_camera_pan_delta(c.yaw, distance, 0.0f, 1.0f, 4.0f, false);
        glm::vec3 const right_move = deren::engine::orbit_camera_pan_delta(c.yaw, distance, 1.0f, 0.0f, 4.0f, false);
        CHECK(glm::length(rise_move - up * step) < 1e-5f);
        CHECK(glm::length(right_move - c.right * step) < 1e-5f);
        CHECK(right_move.y == 0.0f);                       // strafing never leaves the horizontal plane
        CHECK(rise_move.x == 0.0f && rise_move.z == 0.0f); // and rising never drifts horizontally
    }

    // The rejected axis, pinned as a NEGATIVE: at yaw 0 the horizontal view direction is (0, 0, -1), so a
    // walk-style UP would have moved along -Z. Rising must have no Z component, DOWN must be exactly -UP,
    // and the two axes must mix as an orthogonal pair (a (+1, +1) press is 45 degrees at the same length).
    glm::vec3 const down_move = deren::engine::orbit_camera_pan_delta(0.0f, distance, 0.0f, -1.0f, 4.0f, false);
    CHECK(down_move.z == 0.0f);
    CHECK(glm::length(down_move + up * step) < 1e-5f);
    glm::vec3 const diagonal_move = deren::engine::orbit_camera_pan_delta(0.0f, distance, 1.0f, 1.0f, 4.0f, false);
    CHECK(std::abs(diagonal_move.x - step / std::sqrt(2.0f)) < 1e-5f);
    CHECK(std::abs(diagonal_move.y - step / std::sqrt(2.0f)) < 1e-5f);

    // No arrow held: EXACTLY zero, so an idle frame never writes the target.
    CHECK(deren::engine::orbit_camera_pan_delta(0.7f, 2.0f, 0.0f, 0.0f, 0.016f, false) == glm::vec3(0.0f));
    // A zero step is zero too (the first frame has no previous clock reading).
    CHECK(deren::engine::orbit_camera_pan_delta(0.7f, 2.0f, 1.0f, 1.0f, 0.0f, false) == glm::vec3(0.0f));

    // The two axes are normalized TOGETHER: a diagonal press is not sqrt(2) times faster.
    float const straight = glm::length(deren::engine::orbit_camera_pan_delta(0.4f, 2.0f, 1.0f, 0.0f, 0.016f, false));
    float const diagonal = glm::length(deren::engine::orbit_camera_pan_delta(0.4f, 2.0f, 1.0f, 1.0f, 0.016f, false));
    CHECK(std::abs(straight - diagonal) < 1e-6f);

    // SHIFT multiplies by exactly 4, and the 0.1 floor keeps a fully zoomed-in rig (distance 0) moving.
    float const fast = glm::length(deren::engine::orbit_camera_pan_delta(0.4f, 2.0f, 1.0f, 0.0f, 0.016f, true));
    CHECK(std::abs(fast - 4.0f * straight) < 1e-6f);
    CHECK(glm::length(deren::engine::orbit_camera_pan_delta(0.4f, 0.0f, 1.0f, 0.0f, 1.0f, false)) > 0.0f);
}

int32_t main() {
    using namespace deren::engine::environment_detail;
    auto base = pack_environment(0, 1, 0);
    CHECK(base[0] == 1 && base[1] == 0 && base[2] == 1 && base[3] == 0);
    auto quarter = pack_environment(90, 2, 2);
    CHECK(std::abs(quarter[0]) < 1e-6f && std::abs(quarter[1] - 1) < 1e-6f);
    auto bad = pack_environment(NAN, INFINITY, 99);
    CHECK(bad[0] == 1 && bad[1] == 0 && bad[2] == 1 && bad[3] == 0);
    CHECK(pack_environment(720, -1, 1)[2] == 0);
    auto background = pack_background({NAN, -1, 2}, INFINITY);
    CHECK(background[0] == 0.18f && background[1] == 0 && background[2] == 1 && background[3] == 0.5f);
    CHECK(valid_half_radiance(std::array<float, 2>{0, 65504}));
    CHECK(!valid_half_radiance(std::array<float, 2>{1, 65505}));
    CHECK(!valid_half_radiance(std::array<float, 2>{1, INFINITY}));
    CHECK(!valid_half_radiance(std::array<float, 2>{1, NAN}));
    CHECK(!reference_strand_frame_valid(0.f, 1.f, 1.f));
    CHECK(!reference_strand_frame_valid(1.f, 1.f, 0.f));
    CHECK(!reference_strand_frame_valid(INFINITY, 1.f, 1.f));
    CHECK(reference_strand_frame_valid(1.f, 1.f, 1.f));
    CHECK(reference_hair_band(0.f, .3f) == 1.f);
    CHECK(reference_hair_band(1.f, .3f) == 0.f);
    CHECK(reference_hair_band(.2f, .2f) < reference_hair_band(.2f, .5f));
    CHECK(reference_hair_band(.2f, .3f) == reference_hair_band(-.2f, .3f));
    CHECK(reference_hair_band(0.f, 0.f) == 0.f);
    CHECK(reference_nose_weight(1.f, 1.f) == 0.f);
    CHECK(reference_nose_weight(.8f, 1.f) == 1.f);
    CHECK(reference_nose_weight(.8f, 0.f) == 0.f);
    CHECK(reference_nose_weight(.85f, 1.f) > reference_nose_weight(.95f, 1.f));
    CHECK(reference_nose_weight(NAN, 1.f) == 0.f);
    CHECK(reference_nose_diffuse_blend(.7f, 1.f, 0.f) == .7f);
    CHECK(reference_nose_diffuse_blend(.7f, 1.f, 1.f) == 1.f);
    CHECK(reference_nose_diffuse_blend(1.f, 1.f, 1.f) == 1.f);
    CHECK(reference_hair_band(INFINITY, .3f) == 0.f);
    CHECK(reference_masked_hair_lobe(1.f, 0.f, 1.f, 1.f) == 0.f);
    CHECK(reference_masked_hair_lobe(1.f, 1.f, 0.f, 1.f) == 0.f);
    CHECK(reference_masked_hair_lobe(1.f, 1.f, 1.f, 0.f) == 0.f);
    CHECK(reference_masked_hair_lobe(1.f, .4f, .5f, 1.f) == .2f);
    CHECK(reference_masked_hair_lobe(NAN, 1.f, 1.f, 1.f) == 0.f);
    CHECK(reference_masked_hair_lobe(2.f, 2.f, 2.f, 2.f) == 1.f);
    CHECK(reference_masked_hair_lobe(1.f, -1.f, 1.f, 1.f) == 0.f);
    CHECK(reference_masked_hair_lobe(1.f, 1.f, INFINITY, 1.f) == 0.f);
    CHECK(reference_masked_hair_lobe(1.f, 1.f, 1.f, NAN) == 0.f);
    CHECK(reference_face_sdf_weight(.5f, 1.f, 0.f) == 1.f);
    CHECK(reference_face_sdf_weight(.5f, -1.f, 0.f) == 0.f);
    CHECK(std::abs(reference_face_sdf_weight(.5f, 0.f, 1.f) - .5f) < 1e-5f);
    CHECK(reference_face_sdf_weight(.7f, 0.f, 1.f) == reference_face_sdf_weight(.7f, 0.f, -1.f));
    CHECK(reference_face_sdf_weight(.5f, 0.f, 0.f) == 1.f);
    CHECK(reference_face_sdf_weight(NAN, 1.f, 0.f) == 1.f);
    test_environment_cubemap_shape();
    test_cube_filter_continuity();
    test_radiance_cube_camera_and_pixels();
    test_irradiance_map_shape();
    test_brdf_lut_shape();
    test_prefiltered_environment_is_smooth();
    test_equirect_environment_axes();
    test_reflection_probe_math();
    test_orbit_camera_pan();
    return deren::vk_test::finish("test_math");
}
