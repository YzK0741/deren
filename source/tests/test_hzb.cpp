// Headless unit tests: the HZB occlusion-culling CPU reference =================
// This is the C++ half of the HZB design (historical design record §6, §9.2,
// §10.1). The GPU build/cull passes mirror source/engine/visibility/hzb.cppm's formulas BY HAND (a shader cannot
// import a C++ module), so these tests pin the formulas themselves, not a picture:
//   1. the max-pyramid layout (level sizes, word offsets, word counts) against a brute-force ceil recurrence;
//   2. the pyramid BUILDER's texels against the definition "level L tile = max of its 2^L pixel block";
//   3. the conservative sphere-vs-HZB reference: every "occluded" answer must have all old depth under the ball's
//      footprint strictly nearer than the ball's nearest point, the five fail-open constructs must keep, and the
//      two approximations the HOST PROTOTYPE got wrong (§6.4 near-depth bound, §6.5 screen rect) must be REPRODUCED
//      locally and shown to cull a visible sphere - so the test goes red if production regresses to them;
//   4. the phase-B byte-exact scene gate and the deterministic ascending compaction reference.
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

import deren.engine.visibility.hzb;
import deren.engine.visibility;
import deren.engine.meshlet;

namespace hzb = deren::engine::visibility::hzb;
namespace vis = deren::engine::visibility;

namespace {
    using matrix = hzb::hzb_matrix;

    // ================================================================================================
    // THE MATRIX CONVENTION, spelled once. hzb.cppm documents it: a matrix is glm's COLUMN-MAJOR layout
    // (element (row, column) is `m[column * 4 + row]`), which is what the shaders' `VR_MAT4` consumes and what
    // `transform()` below (the CPU mirror of Slang's `mul(vector, matrix)`) operates on. The helpers build the
    // matrices from MATHEMATICAL ROWS so the formulas below read like the spec's.
    // ================================================================================================
    [[nodiscard]] double element(matrix const& m, std::uint32_t const row, std::uint32_t const column) {
        return m[static_cast<std::size_t>(column) * 4u + row];
    }

    [[nodiscard]] matrix from_rows(std::array<std::array<double, 4>, 4> const& rows) {
        matrix m{};
        for (std::uint32_t row = 0; row < 4u; ++row) {
            for (std::uint32_t column = 0; column < 4u; ++column) {
                m[static_cast<std::size_t>(column) * 4u + row] = rows[row][column];
            }
        }
        return m;
    }

    [[nodiscard]] matrix identity_matrix() {
        matrix m{};
        m[0] = m[5] = m[10] = m[15] = 1.0;
        return m;
    }

    [[nodiscard]] matrix translation_matrix(double const x, double const y, double const z) {
        matrix m = identity_matrix();
        m[12] = x;
        m[13] = y;
        m[14] = z;
        return m;
    }

    /// the renderer's projection: `perspectiveRH_ZO` with the Vulkan Y flip (`proj[1][1] < 0`), built the way
    /// primitive.cpp/shadow_fit.cpp build it - clip z maps [near, far] to [0, 1]
    [[nodiscard]] matrix perspective_matrix(double const near_plane, double const far_plane, double const fov_y, double const aspect) {
        double const tan_half = std::tan(fov_y * 0.5);
        return from_rows({{
            {1.0 / (aspect * tan_half), 0.0, 0.0, 0.0},
            {0.0, -1.0 / tan_half, 0.0, 0.0},
            {0.0, 0.0, far_plane / (near_plane - far_plane), -(far_plane * near_plane) / (far_plane - near_plane)},
            {0.0, 0.0, -1.0, 0.0},
        }});
    }

    /// T * R_y(radians) * uniform_scale: exercises the radius bound with a rotation and a scale
    [[nodiscard]] matrix rotated_scaled_translation(double const scale, double const radians, std::array<double, 3> const& t) {
        double const c = std::cos(radians);
        double const s = std::sin(radians);
        return from_rows({{
            {scale * c, 0.0, scale * s, t[0]},
            {0.0, scale, 0.0, t[1]},
            {-scale * s, 0.0, scale * c, t[2]},
            {0.0, 0.0, 0.0, 1.0},
        }});
    }

    [[nodiscard]] std::array<double, 4> transform(matrix const& m, std::array<double, 4> const& v) {
        std::array<double, 4> result{};
        for (std::uint32_t row = 0; row < 4u; ++row) {
            result[row] = element(m, row, 0u) * v[0] + element(m, row, 1u) * v[1] + element(m, row, 2u) * v[2] + element(m, row, 3u) * v[3];
        }
        return result;
    }

    [[nodiscard]] matrix multiply(matrix const& a, matrix const& b) {
        matrix result{};
        for (std::uint32_t column = 0; column < 4u; ++column) {
            for (std::uint32_t row = 0; row < 4u; ++row) {
                double sum = 0.0;
                for (std::uint32_t inner = 0; inner < 4u; ++inner) {
                    sum += element(a, row, inner) * element(b, inner, column);
                }
                result[static_cast<std::size_t>(column) * 4u + row] = sum;
            }
        }
        return result;
    }

    [[nodiscard]] double max_axis_scale(matrix const& m) {
        double largest = 0.0;
        for (std::uint32_t row = 0; row < 3u; ++row) {
            double const sum = std::abs(element(m, row, 0u)) + std::abs(element(m, row, 1u)) + std::abs(element(m, row, 2u));
            largest = std::max(largest, sum);
        }
        return largest;
    }

    // ================================================================================================
    // THE TEST'S OWN SECOND IMPLEMENTATION OF §6.2-§6.5, used by the property and oracle checks below. It is
    // deliberately a SEPARATE copy of the formulas: a property check that called the production function would
    // only say "the function equals itself". If production regresses to a prototype-style formula, the footprint
    // here still describes the real one and the comparison fails.
    // ================================================================================================
    struct projection_planes {
        double near_plane = 0.0;
        double far_plane = 0.0;
    };

    [[nodiscard]] std::optional<projection_planes> test_projection_planes(matrix const& projection) {
        double const z_row = element(projection, 2u, 2u);
        double const z_constant = element(projection, 2u, 3u);
        if (!(z_row < 0.0) || !(z_constant < 0.0) || element(projection, 3u, 2u) != -1.0) {
            return std::nullopt;
        }
        double const near_plane = z_constant / z_row;
        double const denom = 1.0 + z_row;
        if (denom == 0.0) {
            return std::nullopt;
        }
        double const far_plane = z_constant / denom;
        if (!(near_plane > 0.0) || !(far_plane > near_plane) || !std::isfinite(near_plane) || !std::isfinite(far_plane)) {
            return std::nullopt;
        }
        return projection_planes{near_plane, far_plane};
    }

    /// §6.4: the NDC z at an eye-space depth (the monotone formula, NOT the prototype's radius/w approximation)
    [[nodiscard]] double test_ndc_z(matrix const& projection, double const depth) {
        return (element(projection, 2u, 2u) * (-depth) + element(projection, 2u, 3u)) / depth;
    }

    /// the footprint of a ball in the OLD image, as the spec defines it; nullopt means "the test must fail open"
    struct test_footprint {
        double near_ndc = 0.0;
        std::uint32_t x0 = 0;
        std::uint32_t y0 = 0;
        std::uint32_t x1 = 0;
        std::uint32_t y1 = 0;
    };

    [[nodiscard]] std::optional<test_footprint> test_footprint_of(hzb::hzb_camera_snapshot const& camera, matrix const& model, hzb::hzb_sphere const& sphere,
                                                                  std::uint32_t const width, std::uint32_t const height) {
        std::optional<projection_planes> const planes = test_projection_planes(camera.proj);
        if (!planes.has_value()) {
            return std::nullopt;
        }
        std::array<double, 3> const center = [&] {
            std::array<double, 4> const world = transform(model, std::array<double, 4>{sphere.center[0], sphere.center[1], sphere.center[2], 1.0});
            return std::array<double, 3>{world[0], world[1], world[2]};
        }();
        double const radius = sphere.radius * max_axis_scale(model);
        matrix const view_projection = multiply(camera.proj, camera.view);
        std::array<double, 4> const clip = transform(view_projection, std::array<double, 4>{center[0], center[1], center[2], 1.0});
        if (!(clip[3] > hzb::hzb_w_epsilon)) {
            return std::nullopt;
        }
        double const eye_x = center[0] - camera.position[0];
        double const eye_y = center[1] - camera.position[1];
        double const eye_z = center[2] - camera.position[2];
        if (std::sqrt(eye_x * eye_x + eye_y * eye_y + eye_z * eye_z) <= radius) {
            return std::nullopt;
        }
        double const nearest_depth = clip[3] - radius;
        if (nearest_depth <= planes->near_plane * hzb::hzb_near_plane_scale || nearest_depth >= planes->far_plane) {
            return std::nullopt;
        }
        // §6.5: the projected corners of the world AABB (the ball is inside it), then the pixel mapping
        double min_x = std::numeric_limits<double>::infinity();
        double min_y = std::numeric_limits<double>::infinity();
        double max_x = -std::numeric_limits<double>::infinity();
        double max_y = -std::numeric_limits<double>::infinity();
        for (std::uint32_t corner = 0; corner < 8u; ++corner) {
            std::array<double, 4> const position{
                center[0] + ((corner & 1u) != 0u ? radius : -radius),
                center[1] + ((corner & 2u) != 0u ? radius : -radius),
                center[2] + ((corner & 4u) != 0u ? radius : -radius),
                1.0,
            };
            std::array<double, 4> const corner_clip = transform(view_projection, position);
            if (corner_clip[3] <= hzb::hzb_w_epsilon) {
                return std::nullopt;
            }
            double const ndc_x = corner_clip[0] / corner_clip[3];
            double const ndc_y = corner_clip[1] / corner_clip[3];
            if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y)) {
                return std::nullopt;
            }
            min_x = std::min(min_x, ndc_x);
            min_y = std::min(min_y, ndc_y);
            max_x = std::max(max_x, ndc_x);
            max_y = std::max(max_y, ndc_y);
        }
        double const left = std::floor((min_x * 0.5 + 0.5) * static_cast<double>(width));
        double const top = std::floor((min_y * 0.5 + 0.5) * static_cast<double>(height));
        double const right = std::ceil((max_x * 0.5 + 0.5) * static_cast<double>(width));
        double const bottom = std::ceil((max_y * 0.5 + 0.5) * static_cast<double>(height));
        if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom)) {
            return std::nullopt;
        }
        double const clamped_left = std::max(0.0, left);
        double const clamped_top = std::max(0.0, top);
        double const clamped_right = std::min(static_cast<double>(width) - 1.0, right);
        double const clamped_bottom = std::min(static_cast<double>(height) - 1.0, bottom);
        if (clamped_right < clamped_left || clamped_bottom < clamped_top) {
            return std::nullopt;
        }
        return test_footprint{
            test_ndc_z(camera.proj, nearest_depth),
            static_cast<std::uint32_t>(clamped_left),
            static_cast<std::uint32_t>(clamped_top),
            static_cast<std::uint32_t>(clamped_right),
            static_cast<std::uint32_t>(clamped_bottom),
        };
    }

    // ================================================================================================
    // THE PROTOTYPE'S TWO WRONG FORMULAS, reproduced locally (spec §6.4/§6.5, "the host prototype's two bugs").
    // The RED cases below run a culler made of these and assert it WOULD cull a sphere the real reference keeps:
    // that is the evidence the test really tests conservatism, and it makes a regression to either formula fail.
    // ================================================================================================
    struct prototype_rect {
        std::uint32_t x0 = 0;
        std::uint32_t y0 = 0;
        std::uint32_t x1 = 0;
        std::uint32_t y1 = 0;
    };

    /// §6.5's bug: a circle of `radius_px = world_radius * focal / clip.w` around the projected centre, where
    /// `focal` is the pixel-space focal length (`|proj[1][1]| * height / 2`)
    [[nodiscard]] std::optional<prototype_rect> prototype_circle_rect(hzb::hzb_camera_snapshot const& camera, matrix const& model, hzb::hzb_sphere const& sphere,
                                                                      std::uint32_t const width, std::uint32_t const height) {
        std::array<double, 3> const center = [&] {
            std::array<double, 4> const world = transform(model, std::array<double, 4>{sphere.center[0], sphere.center[1], sphere.center[2], 1.0});
            return std::array<double, 3>{world[0], world[1], world[2]};
        }();
        double const radius = sphere.radius * max_axis_scale(model);
        matrix const view_projection = multiply(camera.proj, camera.view);
        std::array<double, 4> const clip = transform(view_projection, std::array<double, 4>{center[0], center[1], center[2], 1.0});
        if (!(clip[3] > hzb::hzb_w_epsilon) || !std::isfinite(clip[0]) || !std::isfinite(clip[1])) {
            return std::nullopt;
        }
        double const center_x = (clip[0] / clip[3] * 0.5 + 0.5) * static_cast<double>(width);
        double const center_y = (clip[1] / clip[3] * 0.5 + 0.5) * static_cast<double>(height);
        double const focal = std::abs(element(camera.proj, 1u, 1u)) * static_cast<double>(height) * 0.5;
        double const radius_px = radius * focal / clip[3];
        auto const clamp_pixel = [](double const value, double const last) { return static_cast<std::uint32_t>(std::clamp(value, 0.0, last)); };
        return prototype_rect{
            clamp_pixel(std::floor(center_x - radius_px), static_cast<double>(width) - 1.0),
            clamp_pixel(std::floor(center_y - radius_px), static_cast<double>(height) - 1.0),
            clamp_pixel(std::ceil(center_x + radius_px), static_cast<double>(width) - 1.0),
            clamp_pixel(std::ceil(center_y + radius_px), static_cast<double>(height) - 1.0),
        };
    }

    /// §6.4's bug: the centre's own NDC z minus `world_radius / clip.w`, instead of the monotone formula at the
    /// ball's nearest depth `clip.w - world_radius`
    [[nodiscard]] double prototype_near_ndc(hzb::hzb_camera_snapshot const& camera, matrix const& model, hzb::hzb_sphere const& sphere) {
        std::array<double, 4> const world = transform(model, std::array<double, 4>{sphere.center[0], sphere.center[1], sphere.center[2], 1.0});
        double const radius = sphere.radius * max_axis_scale(model);
        matrix const view_projection = multiply(camera.proj, camera.view);
        std::array<double, 4> const clip = transform(view_projection, std::array<double, 4>{world[0], world[1], world[2], 1.0});
        return clip[2] / clip[3] - radius / clip[3];
    }

    /// §6.6's level selection and tile max, sampled over a rectangle - the shared tail of both variants
    [[nodiscard]] float level_max(hzb::max_pyramid_view const& pyramid, std::uint32_t const x0, std::uint32_t const y0, std::uint32_t const x1, std::uint32_t const y1) {
        std::uint32_t const horizontal = x1 - x0;
        std::uint32_t const vertical = y1 - y0;
        std::uint32_t const widest = std::max({horizontal, vertical, 1u});
        std::uint32_t level = 0u;
        if (widest > 1u) {
            level = static_cast<std::uint32_t>(std::bit_width(widest)) - 2u;
        }
        level = std::min(level, pyramid.levels - 1u);
        std::uint32_t const level_width = hzb::level_width(pyramid.width, level);
        std::uint32_t const level_height = hzb::level_height(pyramid.height, level);
        std::uint32_t const tile_x1 = std::min(x1 >> level, level_width - 1u);
        std::uint32_t const tile_y1 = std::min(y1 >> level, level_height - 1u);
        float farthest = 0.0f;
        for (std::uint32_t tile_y = y0 >> level; tile_y <= tile_y1; ++tile_y) {
            for (std::uint32_t tile_x = x0 >> level; tile_x <= tile_x1; ++tile_x) {
                farthest = std::max(farthest, pyramid.at(level, tile_x, tile_y));
            }
        }
        return farthest;
    }

    /// the whole prototype pipeline: its near bound + its circle rect + the HZB max under that rect
    [[nodiscard]] bool prototype_culls(hzb::hzb_camera_snapshot const& camera, matrix const& model, hzb::hzb_sphere const& sphere, hzb::max_pyramid_view const& pyramid) {
        std::optional<prototype_rect> const rect = prototype_circle_rect(camera, model, sphere, pyramid.width, pyramid.height);
        if (!rect.has_value()) {
            return false;
        }
        float const farthest = level_max(pyramid, rect->x0, rect->y0, rect->x1, rect->y1);
        return prototype_near_ndc(camera, model, sphere) > static_cast<double>(farthest) + hzb::hzb_ndc_epsilon;
    }

    // ================================================================================================
    // SCENES: a synthetic depth image plus the max pyramid built from it by the module under test
    // ================================================================================================
    struct depth_scene {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<float> depth;
        std::vector<float> words;
        hzb::max_pyramid_view pyramid{};
    };

    void scene_build(depth_scene& scene, std::uint32_t const width, std::uint32_t const height, float const clear = 1.0f) {
        scene.width = width;
        scene.height = height;
        scene.depth.assign(std::size_t(width) * height, clear);
        scene.words.assign(hzb::pyramid_words(width, height), 0.0f);
        scene.pyramid = hzb::build_max_pyramid(scene.depth, width, height, scene.words);
        CHECK(scene.pyramid.valid());
    }

    void scene_rebuild(depth_scene& scene) {
        std::fill(scene.words.begin(), scene.words.end(), 0.0f);
        scene.pyramid = hzb::build_max_pyramid(scene.depth, scene.width, scene.height, scene.words);
        CHECK(scene.pyramid.valid());
    }

    void scene_fill_rect(depth_scene& scene, std::uint32_t const x0, std::uint32_t const y0, std::uint32_t const x1, std::uint32_t const y1, float const value) {
        for (std::uint32_t y = y0; y <= std::min(y1, scene.height - 1u); ++y) {
            for (std::uint32_t x = x0; x <= std::min(x1, scene.width - 1u); ++x) {
                scene.depth[std::size_t(y) * scene.width + x] = value;
            }
        }
    }

    [[nodiscard]] hzb::hzb_camera_snapshot camera_at(std::array<double, 3> const& position, matrix const& projection) {
        return hzb::hzb_camera_snapshot{translation_matrix(-position[0], -position[1], -position[2]), projection, position};
    }

    [[nodiscard]] hzb::hzb_camera_snapshot camera_at_origin(matrix const& projection) {
        return camera_at({0.0, 0.0, 0.0}, projection);
    }

    [[nodiscard]] hzb::hzb_decision decide(hzb::hzb_camera_snapshot const& camera, matrix const& model, hzb::hzb_sphere const& sphere, depth_scene const& scene) {
        return hzb::hzb_test_sphere(camera, model, sphere, scene.pyramid);
    }

    [[nodiscard]] bool keeps(hzb::hzb_camera_snapshot const& camera, matrix const& model, hzb::hzb_sphere const& sphere, depth_scene const& scene) {
        return decide(camera, model, sphere, scene) == hzb::hzb_decision::visible;
    }

    /// THE LARGE OFF-AXIS SCENE shared by the fail-open case and the RED rect case: a 512x288 image whose clear sky
    /// is everything except a patch that would cover the prototype's circle, and a ball whose correct footprint
    /// reaches past that patch. Its parameters are pinned by the numbers in the comments below:
    ///   - ball: world centre (3, 0, -2.5), radius 0.7, camera at the origin, near = 0.5, far = 100 (60 deg,
    ///     16:9) -> the exact near bound is z_ndc(1.8) = 0.7259, well in front of the patch's 0.5;
    ///   - the prototype's circle: centre pixel (555, 144), radius_px = 69.8 -> its rectangle is x 485..511,
    ///     y 74..214, and at level 6 that samples columns 448..511, rows 64..255 - all inside the patch;
    ///   - the correct corner rectangle: x 435..511, y 47..241, and at level 6 that samples rows 0..255, i.e. it
    ///     reaches the clear sky (1.0) above the patch, which is why the reference keeps the ball.
    struct off_axis_case {
        depth_scene scene;
        hzb::hzb_camera_snapshot camera;
        matrix model;
        hzb::hzb_sphere sphere;
    };

    [[nodiscard]] off_axis_case make_off_axis_case() {
        off_axis_case current;
        scene_build(current.scene, 512u, 288u, 1.0f);
        scene_fill_rect(current.scene, 448u, 64u, 511u, 255u, 0.5f);
        scene_rebuild(current.scene);
        current.camera = camera_at_origin(perspective_matrix(0.5, 100.0, 1.0471975511965976, 16.0 / 9.0));
        current.model = translation_matrix(3.0, 0.0, -2.5);
        current.sphere = hzb::hzb_sphere{{0.0, 0.0, 0.0}, 0.7};
        return current;
    }

    // ================================================================================================
    // 1. THE PYRAMID LAYOUT (§4.1, §10.1 item 1)
    // ================================================================================================
    void test_pyramid_layout() {
        // the grid of extents the spec asks for: even/odd, 1xN/Nx1, the two acceptance resolutions
        struct extent {
            std::uint32_t width;
            std::uint32_t height;
        };
        extent const extents[] = {
            {1u, 1u},
            {2u, 1u},
            {1u, 2u},
            {3u, 5u},
            {5u, 3u},
            {8u, 8u},
            {7u, 9u},
            {64u, 1u},
            {1u, 64u},
            {17u, 33u},
            {1280u, 720u},
            {1920u, 1080u},
            {3840u, 2160u},
            {251u, 377u},
            {4096u, 2160u},
        };
        for (extent const& current : extents) {
            // the brute-force ceil recurrence, computed INDEPENDENTLY of level_width/level_height
            std::uint32_t expected_levels = 1u;
            std::uint32_t brute_width = current.width;
            std::uint32_t brute_height = current.height;
            std::uint32_t expected_words = 0u;
            std::vector<std::uint32_t> widths{current.width};
            std::vector<std::uint32_t> heights{current.height};
            while (brute_width > 1u || brute_height > 1u) {
                brute_width = (brute_width + 1u) / 2u;
                brute_height = (brute_height + 1u) / 2u;
                widths.push_back(brute_width);
                heights.push_back(brute_height);
                ++expected_levels;
            }
            for (std::size_t level = 0; level < widths.size(); ++level) {
                expected_words += widths[level] * heights[level];
            }
            CHECK_MSG(hzb::level_count(current.width, current.height) == expected_levels, "level_count disagrees with the brute-force ceil recurrence");
            CHECK_MSG(hzb::pyramid_words(current.width, current.height) == expected_words, "pyramid_words disagrees with the brute-force sum");
            CHECK_MSG(hzb::pyramid_bytes(current.width, current.height) == expected_words * hzb::hzb_word_bytes, "pyramid_bytes is not pyramid_words * 4");
            // the levels are continuous and non-overlapping, and the last one is 1x1
            bool layout_ok = true;
            std::uint32_t accumulated = 0u;
            for (std::uint32_t level = 0; level < expected_levels; ++level) {
                bool const size_ok = hzb::level_width(current.width, level) == widths[level] && hzb::level_height(current.height, level) == heights[level];
                bool const offset_ok = hzb::level_offset(current.width, current.height, level) == accumulated;
                hzb::level_size const size = hzb::level_size_of(current.width, current.height, level);
                bool const struct_ok = size.width == widths[level] && size.height == heights[level];
                layout_ok = layout_ok && size_ok && offset_ok && struct_ok;
                accumulated += widths[level] * heights[level];
            }
            CHECK_MSG(layout_ok, "a pyramid level's size or offset is not the brute-force one");
            CHECK_MSG(hzb::level_width(current.width, expected_levels - 1u) == 1u && hzb::level_height(current.height, expected_levels - 1u) == 1u,
                      "the pyramid's last level is not 1x1");
            CHECK_MSG(hzb::pyramid_words(current.width, current.height) == hzb::level_offset(current.width, current.height, expected_levels),
                      "pyramid_words is not the offset past the last level");
            // asking past the last level is defined as "the total", never as an uninitialised offset
            CHECK_MSG(hzb::level_offset(current.width, current.height, expected_levels + 5u) == hzb::pyramid_words(current.width, current.height),
                      "level_offset past the last level is not clamped to the total");
        }

        // the extent the shader's level cap covers: 16 levels reach 1x1 from 32768, and 32769 is one level past it
        static_assert(hzb::level_count(32768u, 32768u) <= hzb::hzb_max_levels);
        static_assert(hzb::level_count(3840u, 2160u) <= hzb::hzb_max_levels);
        static_assert(hzb::level_count(32769u, 1u) == hzb::hzb_max_levels + 1u);

        // a degenerate extent has no pyramid at all (and the builder/ref use that as "unusable")
        CHECK(hzb::level_count(0u, 720u) == 0u);
        CHECK(hzb::level_count(1280u, 0u) == 0u);
        CHECK(hzb::pyramid_words(0u, 720u) == 0u);
        CHECK(hzb::level_width(0u, 3u) == 0u);
        CHECK(hzb::level_height(0u, 0u) == 0u);
        CHECK(hzb::level_width(1u, 40u) == 1u);
        CHECK(hzb::level_height(7u, 0u) == 7u);

        // ... and the capacities this module declares are the OTHER modules' numbers, not second literals
        static_assert(hzb::hzb_item_candidate_capacity == vis::id_meshlet_capacity);
        static_assert(hzb::hzb_item_candidate_capacity == vis::id_item_capacity);
        static_assert(hzb::hzb_candidate_capacity == deren::engine::meshlet_capacity);
        static_assert(hzb::hzb_word_bytes == sizeof(float));
        // the decision's values ARE the cull pass's flags word (§4.3: 0 = survivor, 1 = culled)
        static_assert(static_cast<std::uint32_t>(hzb::hzb_decision::visible) == 0u);
        static_assert(static_cast<std::uint32_t>(hzb::hzb_decision::occluded) == 1u);
    }

    /// the DEFINITION of a max pyramid: level L's texel (tx, ty) is the max of the level-0 pixels in its
    /// 2^L x 2^L block (clipped to the image). A builder with a wrong ceil/offset/edge rule fails this.
    [[nodiscard]] float brute_tile_max(std::vector<float> const& depth, std::uint32_t const width, std::uint32_t const height, std::uint32_t const level,
                                       std::uint32_t const tile_x, std::uint32_t const tile_y) {
        std::uint32_t const begin_x = tile_x << level;
        std::uint32_t const begin_y = tile_y << level;
        std::uint32_t const end_x = std::min<std::uint32_t>(begin_x + (1u << level), width);
        std::uint32_t const end_y = std::min<std::uint32_t>(begin_y + (1u << level), height);
        float largest = 0.0f;
        for (std::uint32_t y = begin_y; y < end_y; ++y) {
            for (std::uint32_t x = begin_x; x < end_x; ++x) {
                largest = std::max(largest, depth[std::size_t(y) * width + x]);
            }
        }
        return largest;
    }

    // ================================================================================================
    // 2. THE PYRAMID BUILDER (§4.1/§4.2, §10.1 item 1 continued)
    // ================================================================================================
    void test_pyramid_builder() {
        struct extent {
            std::uint32_t width;
            std::uint32_t height;
        };
        extent const extents[] = {{1u, 1u}, {2u, 2u}, {3u, 3u}, {5u, 3u}, {7u, 5u}, {4u, 1u}, {1u, 6u}, {128u, 72u}, {129u, 73u}};
        for (extent const& current : extents) {
            depth_scene scene;
            scene.width = current.width;
            scene.height = current.height;
            scene.depth.resize(std::size_t(current.width) * current.height);
            for (std::uint32_t y = 0; y < current.height; ++y) {
                for (std::uint32_t x = 0; x < current.width; ++x) {
                    // a pattern that is not monotone in x or y, so a transposed or shifted builder cannot match it
                    scene.depth[std::size_t(y) * current.width + x] = static_cast<float>((x * 37u + y * 91u + x * y * 7u) % 101u) / 101.0f;
                }
            }
            std::vector<float> const source_depth = scene.depth;
            scene.words.assign(hzb::pyramid_words(current.width, current.height), 1234.5f); // garbage the builder must overwrite
            hzb::max_pyramid_view const pyramid = hzb::build_max_pyramid(scene.depth, current.width, current.height, scene.words);
            CHECK_MSG(pyramid.valid(), "the builder produced an invalid view for a valid extent");
            CHECK_MSG(pyramid.levels == hzb::level_count(current.width, current.height), "the builder's level count is not the layout's");
            // level 0 is the depth image VERBATIM (full-resolution HZB, §12 Q2)
            bool level_zero_ok = true;
            for (std::uint32_t y = 0; y < current.height; ++y) {
                for (std::uint32_t x = 0; x < current.width; ++x) {
                    level_zero_ok = level_zero_ok && pyramid.at(0u, x, y) == source_depth[std::size_t(y) * current.width + x];
                }
            }
            CHECK_MSG(level_zero_ok, "pyramid level 0 is not a copy of the depth image");
            // every level is the 2x2 max of the level below, i.e. the max of its pixel block; and a level tile can
            // only be LARGER than (or equal to) every pixel it covers - the conservative direction (§4.2)
            bool levels_ok = true;
            bool conservative = true;
            for (std::uint32_t level = 1u; level < pyramid.levels; ++level) {
                hzb::level_size const size = hzb::level_size_of(current.width, current.height, level);
                for (std::uint32_t tile_y = 0; tile_y < size.height; ++tile_y) {
                    for (std::uint32_t tile_x = 0; tile_x < size.width; ++tile_x) {
                        float const expected = brute_tile_max(source_depth, current.width, current.height, level, tile_x, tile_y);
                        float const actual = pyramid.at(level, tile_x, tile_y);
                        levels_ok = levels_ok && actual == expected;
                        conservative = conservative && actual >= expected;
                    }
                }
            }
            CHECK_MSG(levels_ok, "a pyramid level is not the max of its 2^L pixel block (ceil/edge rule)");
            CHECK_MSG(conservative, "a pyramid tile is smaller than a pixel it covers");
            // level_words is that level's prefix of the same buffer
            bool windows_ok = true;
            for (std::uint32_t level = 0; level < pyramid.levels; ++level) {
                std::span<float const> const words = pyramid.level_words(level);
                hzb::level_size const size = hzb::level_size_of(current.width, current.height, level);
                windows_ok = windows_ok && words.size() == std::size_t(size.width) * size.height;
                windows_ok = windows_ok && words[0] == pyramid.at(level, 0u, 0u);
            }
            CHECK_MSG(windows_ok, "a level's word window is not its slice of the pyramid");
        }

        // the odd boundary's REPLICATION, stated as a case a sentinel or a zero fill would get wrong: 3x3 depth where
        // only the last column and row carry the large value - the last 2x2 tile must still see it
        {
            depth_scene scene;
            scene.width = 3u;
            scene.height = 3u;
            scene.depth.assign(9u, 0.0f);
            scene.depth[2] = 0.5f; // (x=2, y=0)
            scene.depth[8] = 0.9f; // (x=2, y=2)
            scene.words.assign(hzb::pyramid_words(3u, 3u), 0.0f);
            hzb::max_pyramid_view const pyramid = hzb::build_max_pyramid(scene.depth, 3u, 3u, scene.words);
            CHECK(pyramid.valid());
            // level 1 is 2x2; its bottom-right tile covers pixel (2, 2) only, and its replicated neighbours are that
            // same pixel - an out-of-range read (or a zero) would give 0.0 here
            CHECK(pyramid.at(1u, 1u, 1u) == 0.9f);
            CHECK(pyramid.at(1u, 1u, 0u) == 0.5f);
            // level 2 is 1x1 = the max of the whole image
            CHECK(pyramid.at(2u, 0u, 0u) == 0.9f);
        }

        // the builder refuses what it cannot hold, and refuses nothing it can
        {
            depth_scene scene;
            scene_build(scene, 8u, 4u);
            std::vector<float> too_small(hzb::pyramid_words(8u, 4u) - 1u, 0.0f);
            CHECK(!hzb::build_max_pyramid(scene.depth, 8u, 4u, too_small).valid());
            std::vector<float> depth_too_small(31u, 0.0f);
            std::vector<float> storage(hzb::pyramid_words(8u, 4u), 0.0f);
            CHECK(!hzb::build_max_pyramid(depth_too_small, 8u, 4u, storage).valid());
            CHECK(!hzb::build_max_pyramid(scene.depth, 0u, 4u, storage).valid());
            CHECK(hzb::build_max_pyramid(scene.depth, 8u, 4u, storage).valid());
            // an exactly-sized depth span is enough: the builder must not require slack
            std::vector<float> exact(32u, 0.25f);
            CHECK(hzb::build_max_pyramid(std::span<float const>(exact), 8u, 4u, storage).valid());
            CHECK(hzb::max_pyramid_view{}.valid() == false);
        }
    }
    // ================================================================================================
    // 3. THE CONSERVATIVE TEST'S FAIL-OPEN CONSTRUCTS (§6.3-§6.6, §10.1 item 2)
    //
    //    Each block is one of the situations in which a bound in §6 stops being a bound. The required answer is
    //    always "visible" (draw it); the scene around it is built so that a reference WITHOUT that protection
    //    would find an occluder and cull, which is what makes each CHECK a real clause of the design.
    // ================================================================================================
    void test_fail_open_constructs() {
        std::uint32_t const width = 128u;
        std::uint32_t const height = 72u;
        double const fov_y = 1.0471975511965976;                                     // 60 degrees
        matrix const projection = perspective_matrix(1.0, 100.0, fov_y, 16.0 / 9.0); // near = 1
        hzb::hzb_camera_snapshot const camera = camera_at_origin(projection);

        // 1. the eye is INSIDE the ball: no projection bound means anything
        {
            depth_scene scene;
            scene_build(scene, width, height, 0.2f);
            CHECK_MSG(keeps(camera, translation_matrix(0.0, 0.0, -0.5), hzb::hzb_sphere{{0.0, 0.0, 0.0}, 1.5}, scene),
                      "a ball that contains the eye must be kept");
        }
        // 2. NEAR-PLANE protection: a ball that crosses or hugs the near plane is kept even with an occluder over
        //    the whole screen (the monotone formula of §6.4 is not a bound for it)
        {
            depth_scene scene;
            scene_build(scene, width, height, 0.9f);
            // d_near = 1.05 - 0.1 = 0.95 <= 1.001 * near: the bound is evaluated outside its domain
            CHECK_MSG(keeps(camera, translation_matrix(0.0, 0.0, -1.05), hzb::hzb_sphere{{0.0, 0.0, 0.0}, 0.1}, scene),
                      "a ball hugging the near plane must be kept");
            // ... and one that swallows the eye plane (d_near < 0, its AABB corners are behind the eye as well)
            CHECK_MSG(keeps(camera, translation_matrix(0.0, 0.0, -0.3), hzb::hzb_sphere{{0.0, 0.0, 0.0}, 0.6}, scene),
                      "a ball that swallows the eye plane must be kept");
        }
        // 3. entirely OUTSIDE THE OLD FRUSTUM: an occluder covering the screen cannot hide what was not on screen
        {
            depth_scene scene;
            scene_build(scene, width, height, 0.2f);
            // far to the side: the projected AABB lies entirely past the right edge, so the rect is empty
            CHECK_MSG(keeps(camera, translation_matrix(50.0, 0.0, -5.0), hzb::hzb_sphere{{0.0, 0.0, 0.0}, 0.5}, scene),
                      "a ball entirely right of the old view must be kept");
            // behind the eye: clip.w <= 0
            CHECK_MSG(keeps(camera, translation_matrix(0.0, 0.0, 5.0), hzb::hzb_sphere{{0.0, 0.0, 0.0}, 0.1}, scene),
                      "a ball behind the eye must be kept");
            // entirely past the far plane: it was not in the old image (without the explicit far bound the sampled
            // occluder would satisfy `near_ndc > far + eps`, because NDC z past 1 is still positive)
            CHECK_MSG(keeps(camera, translation_matrix(0.0, 0.0, -150.0), hzb::hzb_sphere{{0.0, 0.0, 0.0}, 1.0}, scene),
                      "a ball entirely past the old far plane must be kept");
        }
        // 4. NO GEOMETRY: the clear value 1.0 IS the far plane, so a ball over the sky is kept without a sentinel
        {
            depth_scene scene;
            scene_build(scene, width, height, 1.0f);
            CHECK_MSG(keeps(camera, translation_matrix(0.0, 0.0, -40.0), hzb::hzb_sphere{{0.0, 0.0, 0.0}, 1.0}, scene),
                      "a ball over the clear sky must be kept");
            depth_scene sparse;
            scene_build(sparse, width, height, 1.0f);
            scene_fill_rect(sparse, 0u, 0u, 10u, 10u, 0.1f);
            scene_rebuild(sparse);
            CHECK_MSG(keeps(camera, translation_matrix(0.0, 0.0, -40.0), hzb::hzb_sphere{{0.0, 0.0, 0.0}, 1.0}, sparse),
                      "a ball over the sky with unrelated occluders elsewhere must be kept");
        }
        // 5. a LARGE OFF-AXIS ball: constructing it is shared with the RED rect case below - the correct rectangle
        //    reaches past the occluder into the clear sky, so the ball is kept
        {
            off_axis_case const current = make_off_axis_case();
            CHECK_MSG(keeps(current.camera, current.model, current.sphere, current.scene), "a large off-axis ball whose correct footprint reaches the sky must be kept");
        }
        // 6. a projection the formulas are not derived for: everything is kept (a bound from the wrong formula is
        //    not a conservative bound - §6.3 validates the projection shape)
        {
            depth_scene scene;
            scene_build(scene, width, height, 0.2f);
            matrix const orthographic = from_rows({{{1.0, 0.0, 0.0, 0.0}, {0.0, 1.0, 0.0, 0.0}, {0.0, 0.0, 0.5, 0.5}, {0.0, 0.0, 0.0, 1.0}}});
            CHECK_MSG(keeps(camera_at_origin(orthographic), translation_matrix(0.0, 0.0, -5.0), hzb::hzb_sphere{{0.0, 0.0, 0.0}, 0.5}, scene),
                      "an orthographic projection must fail open");
        }
        // 7. invalid inputs fail open: a bound that is not a number is not a bound (subtilis::sphere_visible's rule)
        {
            depth_scene scene;
            scene_build(scene, width, height, 0.2f);
            matrix nan_model = translation_matrix(0.0, 0.0, -30.0);
            nan_model[12] = std::numeric_limits<double>::quiet_NaN();
            CHECK_MSG(keeps(camera, nan_model, hzb::hzb_sphere{{0.0, 0.0, 0.0}, 0.5}, scene), "a non-finite model matrix must fail open");
            CHECK_MSG(keeps(camera, translation_matrix(0.0, 0.0, -30.0), hzb::hzb_sphere{{0.0, 0.0, 0.0}, std::numeric_limits<double>::quiet_NaN()}, scene),
                      "a NaN radius must fail open");
            CHECK_MSG(keeps(camera, translation_matrix(0.0, 0.0, -30.0), hzb::hzb_sphere{{0.0, 0.0, 0.0}, -0.5}, scene), "a negative radius must fail open");
            hzb::max_pyramid_view const empty{};
            CHECK_MSG(hzb::hzb_test_sphere(camera, translation_matrix(0.0, 0.0, -30.0), hzb::hzb_sphere{{0.0, 0.0, 0.0}, 0.5}, empty) == hzb::hzb_decision::visible,
                      "an unusable pyramid must fail open");
        }
    }

    // ================================================================================================
    // 4. COMPLETENESS: a full-screen occluder must CULL the balls behind it (§10.1 item 2's other direction)
    //
    //    The fail-open cases above prove the test errs toward drawing; a reference that ALWAYS answered "visible"
    //    would pass all of them. With the whole old image at one depth the cull condition is exactly the spec's
    //    `near_ndc > depth + epsilon` (plus the §6.3 early-outs), so every case below has one expected answer and
    //    a formula regression - including an epsilon or a monotone-formula change - moves it.
    // ================================================================================================
    void test_plane_completeness() {
        std::uint32_t const width = 128u;
        std::uint32_t const height = 72u;
        double const fov_y = 1.0471975511965976;
        double const aspect = 16.0 / 9.0;
        double const near_planes[] = {0.1, 0.5, 1.0};
        double const far_planes[] = {10.0, 100.0, 2000.0};
        double const distances[] = {2.0, 5.0, 20.0};
        double const radii[] = {0.1, 0.5, 1.0};
        float const plane_depths[] = {0.2f, 0.5f, 0.8f};
        for (double const near_plane : near_planes) {
            for (double const far_plane : far_planes) {
                if (!(far_plane > near_plane)) {
                    continue;
                }
                matrix const projection = perspective_matrix(near_plane, far_plane, fov_y, aspect);
                hzb::hzb_camera_snapshot const camera = camera_at_origin(projection);
                for (double const distance : distances) {
                    for (double const radius : radii) {
                        for (float const plane_depth : plane_depths) {
                            depth_scene scene;
                            scene_build(scene, width, height, plane_depth);
                            matrix const model = translation_matrix(0.0, 0.0, -distance);
                            hzb::hzb_sphere const sphere{{0.0, 0.0, 0.0}, radius};
                            std::optional<test_footprint> const footprint = test_footprint_of(camera, model, sphere, width, height);
                            bool const expected = footprint.has_value() && footprint->near_ndc > static_cast<double>(plane_depth) + hzb::hzb_ndc_epsilon;
                            CHECK_MSG(decide(camera, model, sphere, scene) == (expected ? hzb::hzb_decision::occluded : hzb::hzb_decision::visible),
                                      "a full-screen occluder: the decision is not the exact §6 one");
                        }
                    }
                }
            }
        }
        // ... and with the CLEAR sky instead of a plane, the same geometry must ALWAYS be kept (1.0 is not a depth
        // anyone's geometry occupies, it is the "nothing here" marker)
        {
            depth_scene scene;
            scene_build(scene, width, height, 1.0f);
            matrix const projection = perspective_matrix(0.1, 100.0, fov_y, aspect);
            CHECK(keeps(camera_at_origin(projection), translation_matrix(0.0, 0.0, -50.0), hzb::hzb_sphere{{0.0, 0.0, 0.0}, 1.0}, scene));
        }
    }

    // ================================================================================================
    // 5. THE CONSERVATIVE PROPERTY ON RANDOM SCENES (§6.7 property 1, §10.1 item 2)
    //
    //    Random cameras, transforms, balls and synthetic depth (planes, boxes, holes). For every "occluded" answer:
    //    recompute the ball's footprint with the TEST's independent §6.2-§6.5 code, and check that EVERY level-0
    //    depth under that footprint is strictly nearer than the ball's nearest possible NDC z. That is the safety
    //    property the GPU pass will rely on, written as a per-pixel brute force (the module only ever samples one
    //    level's tiles, so this check does not reuse its sampling).
    // ================================================================================================
    struct random_source {
        std::uint32_t state = 0x9E3779B9u;

        [[nodiscard]] std::uint32_t next() {
            this->state ^= this->state << 13u;
            this->state ^= this->state >> 17u;
            this->state ^= this->state << 5u;
            return this->state;
        }

        [[nodiscard]] double unit() {
            return static_cast<double>(this->next() >> 8u) / 16777216.0;
        }

        [[nodiscard]] double range(double const low, double const high) {
            return low + (high - low) * this->unit();
        }
    };

    void test_conservative_property() {
        std::uint32_t const width = 128u;
        std::uint32_t const height = 72u;
        random_source random;
        std::uint32_t occluded_cases = 0u;
        for (std::uint32_t round = 0; round < 64u; ++round) {
            double const near_plane = random.range(0.1, 1.0);
            double const far_plane = near_plane * random.range(10.0, 200.0);
            matrix projection = perspective_matrix(near_plane, far_plane, random.range(0.6, 1.4), random.range(1.0, 2.0));
            projection[8] = random.range(-2. / width, 2. / width);
            projection[9] = random.range(-2. / height, 2. / height);
            std::array<double, 3> const position{random.range(-4.0, 4.0), random.range(-4.0, 4.0), random.range(-4.0, 4.0)};
            hzb::hzb_camera_snapshot const camera = camera_at(position, projection);
            matrix const model = rotated_scaled_translation(random.range(0.5, 2.0), random.range(-3.14, 3.14),
                                                            {random.range(-6.0, 6.0), random.range(-3.0, 3.0), random.range(-6.0, -0.5)});
            hzb::hzb_sphere const sphere{{random.range(-1.0, 1.0), random.range(-1.0, 1.0), random.range(-1.0, 1.0)}, random.range(0.1, 1.5)};
            depth_scene scene;
            bool const plane_case = (round % 2u) == 0u;
            float plane_depth = 0.5f;
            if (plane_case) {
                // draw the occluder's depth from where the ball actually lands, so about half of these rounds reach
                // the cull path instead of only ever checking "visible" answers
                std::optional<test_footprint> const footprint = test_footprint_of(camera, model, sphere, width, height);
                double const plane = footprint.has_value() ? std::clamp(footprint->near_ndc - random.range(0.02, 0.15), 0.05, 0.95) : random.range(0.05, 0.95);
                plane_depth = static_cast<float>(plane);
                scene_build(scene, width, height, plane_depth);
            } else {
                scene_build(scene, width, height, 1.0f);
                std::uint32_t const rectangles = 1u + random.next() % 4u;
                for (std::uint32_t rectangle = 0; rectangle < rectangles; ++rectangle) {
                    std::uint32_t const x0 = random.next() % width;
                    std::uint32_t const y0 = random.next() % height;
                    std::uint32_t const x1 = std::min(width - 1u, x0 + random.next() % (width / 2u));
                    std::uint32_t const y1 = std::min(height - 1u, y0 + random.next() % (height / 2u));
                    scene_fill_rect(scene, x0, y0, x1, y1, static_cast<float>(random.range(0.05, 0.9)));
                }
                scene_rebuild(scene);
            }
            std::optional<test_footprint> const footprint = test_footprint_of(camera, model, sphere, width, height);
            hzb::hzb_decision const decision = decide(camera, model, sphere, scene);
            if (plane_case) {
                // the exact oracle of section 4, now over random cameras: the plane covers the whole image
                bool const expected = footprint.has_value() && footprint->near_ndc > static_cast<double>(plane_depth) + hzb::hzb_ndc_epsilon;
                CHECK_MSG(decision == (expected ? hzb::hzb_decision::occluded : hzb::hzb_decision::visible), "random full-screen plane: the decision is not the exact §6 one");
            }
            if (decision == hzb::hzb_decision::occluded) {
                ++occluded_cases;
                CHECK_MSG(footprint.has_value(), "an occluded answer must have a bounded footprint");
                if (footprint.has_value()) {
                    bool all_nearer = true;
                    for (std::uint32_t y = footprint->y0; y <= footprint->y1; ++y) {
                        for (std::uint32_t x = footprint->x0; x <= footprint->x1; ++x) {
                            all_nearer = all_nearer && static_cast<double>(scene.pyramid.at(0u, x, y)) < footprint->near_ndc;
                        }
                    }
                    CHECK_MSG(all_nearer, "NON-CONSERVATIVE: an occluded ball has old depth at or beyond its nearest point under its footprint");
                    CHECK_MSG(footprint->near_ndc > 0.0, "an occluded answer must have a positive NDC near bound");
                }
            }
        }
        // the generator must actually produce culled cases: a property only observed on "visible" answers is no evidence
        CHECK_MSG(occluded_cases >= 8u, "the random scenes did not exercise the cull path");
    }

    // ================================================================================================
    // 6. THE TWO RED CASES (§6.4 and §6.5, §10.1 item 2's "reverse RED")
    //
    //    Each case builds a scene in which the HOST PROTOTYPE'S formula returns "occluded" while the real reference
    //    must return "visible". `prototype_culls` is the prototype's pipeline reproduced locally from the spec's
    //    description (§6.4's `ndc_z - r/w`, §6.5's circle); the module under test is what the CHECKs assert against.
    //    If production ever regresses to either formula, its verdict flips to "occluded" and these CHECKs fail.
    // ================================================================================================

    /// the §6.4 RED: the prototype's near bound is the FARTHER (non-conservative) one for a ball that hugs the near
    /// plane; the ball here is 0.0002 NDC in front of the occluder plane, i.e. genuinely nearer than the depth the
    /// prototype would compare it against
    void test_red_near_depth_bound() {
        std::uint32_t const width = 128u;
        std::uint32_t const height = 72u;
        constexpr float plane_depth = 0.0022f;
        depth_scene scene;
        scene_build(scene, width, height, plane_depth);
        matrix const projection = perspective_matrix(1.0, 100.0, 1.0471975511965976, 16.0 / 9.0); // near = 1
        hzb::hzb_camera_snapshot const camera = camera_at_origin(projection);
        matrix const model = translation_matrix(0.0, 0.0, -1.052);
        hzb::hzb_sphere const sphere{{0.0, 0.0, 0.0}, 0.05};
        std::optional<test_footprint> const footprint = test_footprint_of(camera, model, sphere, width, height);
        CHECK_MSG(footprint.has_value(), "the RED near-bound scene's footprint must be bounded (the ball is beyond 1.001*near)");
        double const exact = footprint.has_value() ? footprint->near_ndc : 0.0;
        double const prototype = prototype_near_ndc(camera, model, sphere);
        CHECK_MSG(exact < static_cast<double>(plane_depth) + hzb::hzb_ndc_epsilon, "the exact near bound must NOT cull this ball: it is nearer than the occluder");
        CHECK_MSG(prototype > static_cast<double>(plane_depth) + hzb::hzb_ndc_epsilon, "the prototype's near bound must WRONGLY cull this ball");
        CHECK_MSG(prototype > exact, "the prototype's near bound is not the farther (non-conservative) one in this case");
        CHECK_MSG(prototype_culls(camera, model, sphere, scene.pyramid), "RED: the prototype pipeline must return OCCLUDED for this ball");
        CHECK_MSG(keeps(camera, model, sphere, scene), "RED: the reference must PRESERVE the ball the prototype's near bound would cull");
    }

    /// the §6.5 RED: a large off-axis ball's perspective footprint is not the prototype's circle. The circle sits
    /// entirely inside a patch of occluder; the correct corner rectangle reaches the clear sky next to it, so the
    /// real reference keeps a ball the circle-based culler would reject.
    void test_red_screen_rect() {
        off_axis_case const current = make_off_axis_case();
        std::optional<test_footprint> const footprint = test_footprint_of(current.camera, current.model, current.sphere, current.scene.width, current.scene.height);
        std::optional<prototype_rect> const circle = prototype_circle_rect(current.camera, current.model, current.sphere, current.scene.width, current.scene.height);
        CHECK_MSG(footprint.has_value(), "the RED rect scene's footprint must be bounded");
        CHECK_MSG(circle.has_value(), "the RED rect scene's prototype circle must be computable");
        if (footprint.has_value() && circle.has_value()) {
            float const correct_max = level_max(current.scene.pyramid, footprint->x0, footprint->y0, footprint->x1, footprint->y1);
            float const circle_max = level_max(current.scene.pyramid, circle->x0, circle->y0, circle->x1, circle->y1);
            CHECK_MSG(correct_max >= 1.0f, "the correct rectangle must reach the clear sky (otherwise the scene proves nothing)");
            CHECK_MSG(circle_max < 1.0f, "the prototype circle must sit entirely inside the occluder patch");
            CHECK_MSG(correct_max != circle_max, "the two rectangles sample different depths in the RED rect scene");
        }
        CHECK_MSG(prototype_culls(current.camera, current.model, current.sphere, current.scene.pyramid), "RED: the prototype pipeline must return OCCLUDED for the off-axis ball");
        CHECK_MSG(keeps(current.camera, current.model, current.sphere, current.scene), "RED: the reference must PRESERVE the ball the prototype's circle would cull");
    }

    // ================================================================================================
    // 7. THE PHASE-B SCENE GATE (§9.2, §10.1 item 4)
    //
    //    Byte equality, not value equality: a 1-ulp camera move, an item reorder, a model matrix that moves one ulp
    //    or a changed identity all have to close the gate, because the gate's whole claim is "the previous frame's
    //    depth IS this frame's depth". +0.0/-0.0 and NaN payloads are the two cases a value comparison would blur.
    // ================================================================================================
    void test_scene_gate() {
        std::array<float, 16> const view{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.25f, -1.5f, 3.0f, 1.0f};
        std::array<float, 16> const projection{0.974f, 0.0f, 0.0f, 0.0f, 0.0f, -1.732f, 0.0f, 0.0f, 0.0f, 0.0f, -1.005f, -1.0f, 0.0f, 0.0f, -0.5025f, 0.0f};
        std::array<float, 16> const model_a{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 2.0f, 3.0f, 1.0f};
        std::array<float, 16> const model_b{0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -4.0f, 0.5f, 2.0f, 1.0f};

        // the snapshot really is the bit pattern, not the value
        hzb::hzb_matrix_bits const view_bits = hzb::hzb_matrix_bits_of(view);
        CHECK(view_bits[0] == std::bit_cast<std::uint32_t>(view[0]));
        CHECK(view_bits[12] == std::bit_cast<std::uint32_t>(view[12]));
        CHECK(view_bits.size() == 16u);

        std::array<hzb::hzb_scene_item, 2> const previous_items{hzb::hzb_capture_item(0x1000u, model_a), hzb::hzb_capture_item(0x2000u, model_b)};
        std::array<hzb::hzb_scene_item, 2> current_items = previous_items;
        hzb::hzb_scene_snapshot const previous = hzb::hzb_capture_scene(view, projection, previous_items);
        hzb::hzb_scene_snapshot const current = hzb::hzb_capture_scene(view, projection, current_items);
        CHECK_MSG(hzb::hzb_scene_gate(previous, current), "identical frames must pass the gate");

        // a 1-ULP camera move closes it, on view AND on proj (TAA's jitter lands in proj, §9.2)
        {
            std::array<float, 16> moved = view;
            moved[13] = std::nextafter(moved[13], std::numeric_limits<float>::infinity());
            hzb::hzb_scene_snapshot const snapshot = hzb::hzb_capture_scene(moved, projection, current_items);
            CHECK_MSG(!hzb::hzb_scene_gate(previous, snapshot), "a 1-ulp view move must close the gate");
            std::array<float, 16> jittered = projection;
            jittered[12] = std::nextafter(jittered[12], std::numeric_limits<float>::infinity());
            hzb::hzb_scene_snapshot const jitter = hzb::hzb_capture_scene(view, jittered, current_items);
            CHECK_MSG(!hzb::hzb_scene_gate(previous, jitter), "a 1-ulp proj move (TAA jitter) must close the gate");
        }
        // a model matrix that moves one ulp closes it, wherever it sits in the list
        {
            std::array<hzb::hzb_scene_item, 2> moved_items = previous_items;
            std::array<float, 16> moved_model = model_b;
            moved_model[14] = std::nextafter(moved_model[14], std::numeric_limits<float>::infinity());
            moved_items[1] = hzb::hzb_capture_item(0x2000u, moved_model);
            hzb::hzb_scene_snapshot const snapshot = hzb::hzb_capture_scene(view, projection, moved_items);
            CHECK_MSG(!hzb::hzb_scene_gate(previous, snapshot), "a 1-ulp model move must close the gate");
        }
        // item ORDER is part of the contract (the gate remembers the visible item list as ordered)
        {
            std::array<hzb::hzb_scene_item, 2> reordered{previous_items[1], previous_items[0]};
            hzb::hzb_scene_snapshot const snapshot = hzb::hzb_capture_scene(view, projection, reordered);
            CHECK_MSG(!hzb::hzb_scene_gate(previous, snapshot), "a reordered item list must close the gate");
        }
        // item identity and item count
        {
            std::array<hzb::hzb_scene_item, 2> other_leaf = previous_items;
            other_leaf[0].leaf = 0x1001u;
            hzb::hzb_scene_snapshot const snapshot = hzb::hzb_capture_scene(view, projection, other_leaf);
            CHECK_MSG(!hzb::hzb_scene_gate(previous, snapshot), "a changed item identity must close the gate");
            std::array<hzb::hzb_scene_item, 1> fewer{previous_items[0]};
            hzb::hzb_scene_snapshot const short_list = hzb::hzb_capture_scene(view, projection, fewer);
            CHECK_MSG(!hzb::hzb_scene_gate(previous, short_list), "a different item count must close the gate");
        }
        // an EMPTY visible list on both sides is identical (there is nothing the cull could disagree about)
        {
            std::span<hzb::hzb_scene_item const> const none{};
            hzb::hzb_scene_snapshot const empty = hzb::hzb_capture_scene(view, projection, none);
            CHECK_MSG(hzb::hzb_scene_gate(empty, empty), "two empty item lists must pass the gate");
            CHECK_MSG(!hzb::hzb_scene_gate(empty, previous), "an empty list and a populated one must not pass");
        }
        // BYTE equality, stated with the two values a value comparison blurs: -0.0 vs +0.0, and NaN payloads
        {
            std::array<float, 16> positive_zero = view;
            std::array<float, 16> negative_zero = view;
            positive_zero[0] = 0.0f;
            negative_zero[0] = -0.0f;
            CHECK_MSG(positive_zero[0] == negative_zero[0], "the two zeroes compare equal as VALUES (that is why bytes are required)");
            std::span<hzb::hzb_scene_item const> const none{};
            hzb::hzb_scene_snapshot const positive_snapshot = hzb::hzb_capture_scene(positive_zero, projection, none);
            hzb::hzb_scene_snapshot const negative_snapshot = hzb::hzb_capture_scene(negative_zero, projection, none);
            CHECK_MSG(!hzb::hzb_scene_gate(positive_snapshot, negative_snapshot), "-0.0 and +0.0 must NOT pass a byte-exact gate");
            std::array<float, 16> nan_a = view;
            nan_a[5] = std::bit_cast<float>(0x7FC00001u);
            std::array<float, 16> nan_b = nan_a;
            hzb::hzb_scene_snapshot const nan_snapshot_a = hzb::hzb_capture_scene(nan_a, projection, none);
            hzb::hzb_scene_snapshot const nan_snapshot_b = hzb::hzb_capture_scene(nan_b, projection, none);
            CHECK_MSG(hzb::hzb_scene_gate(nan_snapshot_a, nan_snapshot_b), "the same NaN bit pattern must pass a byte-exact gate");
            nan_b[5] = std::bit_cast<float>(0x7FC00002u);
            hzb::hzb_scene_snapshot const nan_snapshot_c = hzb::hzb_capture_scene(nan_b, projection, none);
            CHECK_MSG(!hzb::hzb_scene_gate(nan_snapshot_a, nan_snapshot_c), "a different NaN payload must close a byte-exact gate");
        }
    }

    // ================================================================================================
    // 8. THE DETERMINISTIC COMPACTION REFERENCE (§4.3, §10.1 item 3)
    // ================================================================================================
    void test_compaction() {
        random_source random;
        random.state = 0x0DDB1A5Eu;
        std::array<std::uint32_t, 513> candidate_ids{};
        std::array<bool, 513> keep{};
        std::array<std::uint32_t, 513> survivors{};
        for (std::uint32_t round = 0; round < 24u; ++round) {
            std::size_t const count = random.next() % 513u;
            for (std::size_t index = 0; index < count; ++index) {
                candidate_ids[index] = 7u + static_cast<std::uint32_t>(index) * 3u; // ascending: the GPU's candidate order
                keep[index] = (random.next() & 1u) != 0u;
            }
            std::fill(survivors.begin(), survivors.end(), 0xDEADBEEFu);
            std::optional<hzb::hzb_compaction> const result = hzb::hzb_compact(std::span<std::uint32_t const>(candidate_ids.data(), count),
                                                                               std::span<bool const>(keep.data(), count), std::span<std::uint32_t>(survivors.data(), count));
            CHECK_MSG(result.has_value(), "a valid candidate list must compact");
            if (result.has_value()) {
                std::vector<std::uint32_t> expected;
                for (std::size_t index = 0; index < count; ++index) {
                    if (keep[index]) {
                        expected.push_back(candidate_ids[index]);
                    }
                }
                CHECK_MSG(result->candidates == count, "the compaction's candidate count is the input count");
                CHECK_MSG(result->survivors == expected.size(), "the compaction's survivor count is the kept count");
                bool prefix_ok = true;
                for (std::size_t index = 0; index < expected.size(); ++index) {
                    prefix_ok = prefix_ok && survivors[index] == expected[index];
                }
                CHECK_MSG(prefix_ok, "the survivors are not the kept candidates in ascending input order");
                CHECK_MSG(survivors[expected.size()] == 0xDEADBEEFu, "the compaction wrote past its survivor prefix");
            }
            // determinism: the same input gives byte-identical output
            std::array<std::uint32_t, 513> again{};
            std::optional<hzb::hzb_compaction> const second = hzb::hzb_compact(std::span<std::uint32_t const>(candidate_ids.data(), count),
                                                                               std::span<bool const>(keep.data(), count), std::span<std::uint32_t>(again.data(), count));
            CHECK_MSG(second.has_value() && result.has_value() && second->survivors == result->survivors &&
                          std::equal(again.begin(), again.begin() + second->survivors, survivors.begin()),
                      "the compaction is not deterministic");
        }
        // an empty run is a valid (zero-survivor) result
        {
            std::optional<hzb::hzb_compaction> const result = hzb::hzb_compact(std::span<std::uint32_t const>{}, std::span<bool const>{}, std::span<std::uint32_t>{});
            CHECK_MSG(result.has_value() && result->candidates == 0u && result->survivors == 0u, "an empty candidate run must compact to zero");
        }
        // the inputs the cull pass could not have produced are refused, not truncated (§9.4)
        {
            std::array<std::uint32_t, 2> ids{1u, 2u};
            std::array<bool, 2> mask{true, true};
            std::array<std::uint32_t, 2> output{};
            CHECK_MSG(!hzb::hzb_compact(std::span<std::uint32_t const>(ids.data(), 2u), std::span<bool const>(mask.data(), 1u), std::span<std::uint32_t>(output.data(), 2u)).has_value(),
                      "a mask that is not the candidate count must be refused");
            CHECK_MSG(!hzb::hzb_compact(std::span<std::uint32_t const>(ids.data(), 2u), std::span<bool const>(mask.data(), 2u), std::span<std::uint32_t>(output.data(), 1u)).has_value(),
                      "an output too small for the survivors must be refused");
        }
        // the per-item capacity is the id's meshlet field: 4096 compacts, 4097 is refused
        {
            std::vector<std::uint32_t> ids(hzb::hzb_item_candidate_capacity, 0x11u);
            std::array<bool, 4097> mask{}; // 4096 plus the one-past case, on the stack: one bool each
            for (std::size_t index = 0; index + 1u < mask.size(); ++index) {
                ids[index] = static_cast<std::uint32_t>(index);
                mask[index] = true;
            }
            std::vector<std::uint32_t> output(hzb::hzb_item_candidate_capacity, 0u);
            std::optional<hzb::hzb_compaction> const full = hzb::hzb_compact(std::span<std::uint32_t const>(ids.data(), ids.size()), std::span<bool const>(mask.data(), ids.size()),
                                                                             std::span<std::uint32_t>(output.data(), output.size()));
            CHECK_MSG(full.has_value() && full->survivors == hzb::hzb_item_candidate_capacity, "a 4096-candidate run must compact fully");
            std::vector<std::uint32_t> one_more(hzb::hzb_item_candidate_capacity + 1u, 0u);
            std::vector<std::uint32_t> one_more_output(hzb::hzb_item_candidate_capacity + 1u, 0u);
            bool const refused = !hzb::hzb_compact(std::span<std::uint32_t const>(one_more.data(), one_more.size()),
                                                   std::span<bool const>(mask.data(), one_more.size()), std::span<std::uint32_t>(one_more_output.data(), one_more_output.size()))
                                      .has_value();
            CHECK_MSG(refused, "a 4097-candidate run is past the id's meshlet field and must be refused");
            CHECK_MSG(mask[4096] == false, "the one-past case's mask must be false so the capacity is the only reason for the refusal");
        }
        // the order is the INPUT order: passing descending ids keeps them descending (the output is a compaction, not a sort)
        {
            std::array<std::uint32_t, 3> ids{9u, 5u, 1u};
            std::array<bool, 3> mask{true, true, true};
            std::array<std::uint32_t, 3> output{};
            auto const result = hzb::hzb_compact(std::span<std::uint32_t const>(ids), std::span<bool const>(mask), std::span<std::uint32_t>(output));
            CHECK(result.has_value() && output[0] == 9u && output[1] == 5u && output[2] == 1u);
        }
    }
} // namespace

namespace {
    hzb::hzb_matrix_bits bits(matrix const& value) {
        hzb::hzb_matrix_bits result{};
        for (unsigned i = 0; i < 16; ++i)
            result[i] = std::bit_cast<std::uint32_t>(static_cast<float>(value[i]));
        return result;
    }
    void test_jittered_projection() {
        depth_scene scene;
        scene_build(scene, 128, 72, .2f);
        for (double jitter : {-.002, .002}) {
            auto projection = perspective_matrix(.1, 100., 1.0471975511965976, 16. / 9.);
            projection[8] = jitter;
            projection[9] = -jitter;
            CHECK_MSG(!keeps(camera_at_origin(projection), translation_matrix(0., 0., -20.), hzb::hzb_sphere{{0., 0., 0.}, .5}, scene), "jittered perspective must retain useful conservative occlusion");
        }
        auto z_shear = perspective_matrix(.1, 100., 1.0471975511965976, 16. / 9.);
        z_shear[2] = .02;
        CHECK_MSG(keeps(camera_at_origin(z_shear), translation_matrix(0., 0., -20.), hzb::hzb_sphere{{0., 0., 0.}, .5}, scene), "clip-Z mixing XY must fail open");
    }
    void test_recovery_contract() {
        CHECK(hzb::hzb_mesh_command_capacity == 4u * 65536u);
        CHECK(hzb::hzb_recovery_command_word_offset(0, 0) == 3u * 65536u * 3u);
        CHECK(hzb::hzb_recovery_command_word_offset(1, 4095) == (4u * 65536u + 3u * 65536u + 4095u) * 3u);
        std::array<std::uint32_t, 5> ids{10, 11, 12, 13, 14}, flags{0, 1, 0, 1, 1}, output{10, 12, 99, 99, 99};
        std::array<bool, 5> keep{true, true, true, false, true};
        auto r = hzb::hzb_compact_recovery(ids, flags, keep, 2, output);
        CHECK(r && r->retested == 3 && r->recovered == 2);
        CHECK((output == std::array<std::uint32_t, 5>{10, 12, 11, 14, 99}));
        auto const saved = output;
        CHECK(!hzb::hzb_compact_recovery(ids, flags, keep, 1, output));
        CHECK(output == saved);
        CHECK(!hzb::hzb_compact_recovery(ids, flags, keep, hzb::hzb_count_fallback, output));
        CHECK(!hzb::hzb_compact_recovery(ids, flags, keep, 2, std::span<std::uint32_t>(output).first(3)));
        CHECK(output == saved);
        flags[3] = 7;
        CHECK(!hzb::hzb_compact_recovery(ids, flags, keep, 2, output));
        CHECK(output == saved);
        flags.fill(0);
        output = ids;
        r = hzb::hzb_compact_recovery(ids, flags, keep, 5, output);
        CHECK(r && r->retested == 0 && r->recovered == 0 && output == ids);
        flags.fill(1);
        keep.fill(true);
        r = hzb::hzb_compact_recovery(ids, flags, keep, 0, output);
        CHECK(r && r->retested == 5 && r->recovered == 5 && output == ids);
        keep.fill(false);
        r = hzb::hzb_compact_recovery(ids, flags, keep, 0, output);
        CHECK(r && r->retested == 5 && r->recovered == 0);
        hzb::hzb_scene_snapshot previous{bits(identity_matrix()), bits(identity_matrix()), {}};
        auto current = previous;
        hzb::hzb_motion_limits limits{};
        CHECK(hzb::hzb_recovery_gate(previous, current, 100., limits));
        current.view = bits(translation_matrix(4, 0, 0));
        CHECK(hzb::hzb_recovery_gate(previous, current, 100., limits));
        current.view = bits(translation_matrix(6, 0, 0));
        CHECK(!hzb::hzb_recovery_gate(previous, current, 100., limits));
        current = previous;
        current.proj[8] = std::bit_cast<std::uint32_t>(0.001f);
        CHECK(hzb::hzb_recovery_gate(previous, current, 100., limits));
        current.proj[0] = std::bit_cast<std::uint32_t>(1.1f);
        CHECK(!hzb::hzb_recovery_gate(previous, current, 100., limits));
        current = previous;
        current.view[0] = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
        CHECK(!hzb::hzb_recovery_gate(previous, current, 100., limits));
        CHECK(!hzb::hzb_recovery_gate(previous, previous, 0., limits));
        current = previous;
        auto rotation = identity_matrix();
        double angle = 2. * 3.141592653589793 / 180.;
        rotation[0] = rotation[10] = std::cos(angle);
        rotation[2] = -std::sin(angle);
        rotation[8] = std::sin(angle);
        current.view = bits(rotation);
        CHECK(hzb::hzb_recovery_gate(previous, current, 100., limits));
        angle = 6. * 3.141592653589793 / 180.;
        rotation[0] = rotation[10] = std::cos(angle);
        rotation[2] = -std::sin(angle);
        rotation[8] = std::sin(angle);
        current.view = bits(rotation);
        CHECK(!hzb::hzb_recovery_gate(previous, current, 100., limits));
        current = previous;
        current.view[0] = std::bit_cast<std::uint32_t>(1.1f);
        CHECK(!hzb::hzb_recovery_gate(previous, current, 100., limits));
        CHECK(!hzb::hzb_recovery_gate(previous, previous, std::numeric_limits<double>::quiet_NaN(), limits));
        auto empty = hzb::hzb_compact_recovery({}, {}, {}, 0, {});
        CHECK(empty && empty->retested == 0 && empty->recovered == 0);
        std::array<std::uint32_t, 4097> many_ids{}, many_flags{}, many_output{};
        std::array<bool, 4097> many_keep{};
        many_keep.fill(true);
        many_flags.fill(1u);
        for (unsigned i = 0; i < many_ids.size(); ++i)
            many_ids[i] = i;
        CHECK(!hzb::hzb_compact_recovery(many_ids, many_flags, many_keep, 0, many_output));
        auto maximum = hzb::hzb_compact_recovery(std::span(many_ids).first(4096), std::span(many_flags).first(4096), std::span(many_keep).first(4096), 0, std::span(many_output).first(4096));
        CHECK(maximum && maximum->recovered == 4096 && many_output[4095] == 4095);
        limits.rotation_degrees = 0.;
        CHECK(!hzb::hzb_recovery_gate(previous, previous, 100., limits));
    }
} // namespace

int main() {
    CHECK(hzb::hzb_command_word_offset(0, 0) == 2u * 65536u * 3u);
    CHECK(hzb::hzb_command_word_offset(1, 4095) == (4u * 65536u + 2u * 65536u + 4095u) * 3u);
    CHECK(hzb::hzb_dispatch_count(7, 0) == 0);
    CHECK(hzb::hzb_dispatch_count(7, 3) == 3);
    CHECK(hzb::hzb_dispatch_count(7, hzb::hzb_count_fallback) == 7);
    test_jittered_projection();
    test_recovery_contract();
    test_pyramid_layout();
    test_pyramid_builder();
    test_fail_open_constructs();
    test_plane_completeness();
    test_conservative_property();
    test_red_near_depth_bound();
    test_red_screen_rect();
    test_scene_gate();
    test_compaction();
    return deren::vk_test::finish("test_hzb");
}
