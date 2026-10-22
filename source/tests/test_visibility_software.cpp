// Headless unit tests: deren.engine.visibility.software (M4 Task 1, pure CPU) =========================================
// The subpixel criterion, the stable [hardware prefix | software suffix] split and the 16x16 tile binning are the
// parts of the M4 hybrid producer that are decided on the CPU BEFORE any pass runs. Their bugs are invisible in a
// picture in the safe direction (a cluster left on the hardware path is just work; a cluster moved to the software
// path when its sphere is NOT provably in front of the near plane / small enough is a hole), so they are pinned here:
//
//   1. THE CRITERION IS THE CONSERVATIVE ONE (spec §3.1): diameter_upper = 2*r*focal/(w-r) from the sphere's
//      NEAREST point (perspective) / r*max(|p00|*w, |p11|*h) (orthographic), with the model's Frobenius 3x3 norm as
//      the radius upper bound (it is >= the operator norm, so non-uniform scale and shear are covered with no case
//      analysis - a rigid identity model therefore carries a sqrt(3) radius inflation, which is the PRICE of the
//      conservative bound and is asserted here so the measurement's counts stay explainable); the sphere must be
//      wholly in front of the near plane with the 0.98 margin; and `diameter < threshold` is STRICT, so a cluster
//      exactly at the threshold stays on the hardware path.
//   2. THE SPLIT IS STABLE AND THE ID MAP IS THE SPEC'S: hardware records keep their relative order as the prefix,
//      software records keep theirs as the suffix, and the software record at suffix index k is what the packed id's
//      `meshlet_in_item` field will call `hardware_count + k` (spec §3.1/§3.3: the id decode needs no extension).
//      1.0 px and 2.0 px are both exercised, because Task 1 decides the default threshold/tile capacity from them.
//   3. TILE BINNING IS DETERMINISTIC AND NEVER SILENTLY TRUNCATES: the same input gives the same tile counts/entries;
//      a tile over capacity is reported (`overflow` + exact counts + max) by the MEASUREMENT entry point and refused
//      by the production entry point, because production falls back to the whole-frame hardware path (§5.4).
//
// The camera matrices are constructed in the renderer's own layout: a 16-float COLUMN-MAJOR array (glm::mat4 storage,
// which is what the runtime memcpys into the push block/UBO) applied to a column vector. The view here is the
// identity (camera at the origin looking down -z), so a record's world z=-10 is 10 metres in front of the eye.
#include "vk_test.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

import deren.utility.shared;
import deren.engine.meshlet;
import deren.engine.visibility;
import deren.engine.visibility.software;

namespace sw = deren::engine::visibility::software;
using deren::engine::meshlet;

namespace {
    std::array<float, 16> identity() {
        std::array<float, 16> m{};
        m[0] = 1.0f;
        m[5] = 1.0f;
        m[10] = 1.0f;
        m[15] = 1.0f;
        return m;
    }

    /// a 4x4 in the renderer's layout: element [column][row] at index column*4 + row
    float& at(std::array<float, 16>& m, int column, int row) {
        return m[static_cast<std::size_t>(column) * 4u + static_cast<std::size_t>(row)];
    }

    /// diag(sx, sy, sz) with an optional translation
    std::array<float, 16> scaled_model(float sx, float sy, float sz, float tx = 0.0f, float ty = 0.0f, float tz = 0.0f) {
        auto m = identity();
        at(m, 0, 0) = sx;
        at(m, 1, 1) = sy;
        at(m, 2, 2) = sz;
        at(m, 3, 0) = tx;
        at(m, 3, 1) = ty;
        at(m, 3, 2) = tz;
        return m;
    }

    /// the renderer's standard perspective: near n, infinite far, p00/p11 as given
    std::array<float, 16> perspective(float near_plane, float p00, float p11) {
        std::array<float, 16> m{};
        at(m, 0, 0) = p00;
        at(m, 1, 1) = p11;
        at(m, 2, 2) = -1.0f;
        at(m, 2, 3) = -1.0f;
        at(m, 3, 2) = -2.0f * near_plane;
        return m;
    }

    /// a standard GL-style orthographic projection [near, far]; m22 = -2/(f-n), m32 = -(f+n)/(f-n)
    std::array<float, 16> orthographic(float near_plane, float far_plane, float p00, float p11) {
        std::array<float, 16> m{};
        float const depth = far_plane - near_plane;
        at(m, 0, 0) = p00;
        at(m, 1, 1) = p11;
        at(m, 2, 2) = -2.0f / depth;
        at(m, 3, 2) = -(far_plane + near_plane) / depth;
        at(m, 3, 3) = 1.0f;
        return m;
    }

    sw::camera_parameters camera_parameters_for(std::array<float, 16> const& proj, std::uint32_t const width,
                                                std::uint32_t const height) {
        return sw::camera_parameters{.view = identity(), .proj = proj, .width = width, .height = height};
    }

    sw::camera_metrics prepared(sw::camera_parameters const& parameters) {
        auto const camera = sw::prepare_camera(parameters);
        CHECK(camera.has_value());
        return camera.value();
    }

    meshlet record(float const cx, float const cy, float const cz, float const radius,
                   std::uint32_t const index_count = 3u) {
        return meshlet{.first_index = 0u, .index_count = index_count, .base_vertex = 0, ._pad = 0u, .center_x = cx, .center_y = cy, .center_z = cz, .radius = radius};
    }

    bool nearly(double const a, double const b, double const tolerance = 1e-9) {
        return std::abs(a - b) <= tolerance * std::max({1.0, std::abs(a), std::abs(b)});
    }

    /// a rigid (identity) model's linear part is the 3x3 identity, whose Frobenius norm is sqrt(3) - the criterion's
    /// conservative radius scale for EVERY un-scaled item (spec §3.1 chooses Frobenius on purpose)
    constexpr double rigid_scale = 1.7320508075688772;

    /// the spec §3.1 perspective diameter upper bound, written independently here so the formula is pinned twice
    double expected_perspective_diameter(double const world_radius, double const w, double const proj_scale) {
        return 2.0 * world_radius * (proj_scale * 0.5) / (w - world_radius);
    }

    std::span<meshlet const> span_of(std::vector<meshlet> const& records) {
        return std::span<meshlet const>(records.data(), records.size());
    }

    /// the flat index of the tile (x, y) in a plan's tile-major arrays
    std::size_t tile_index(sw::software_tile_plan const& plan, std::uint32_t const x, std::uint32_t const y) {
        return static_cast<std::size_t>(y) * plan.grid.tiles_x + x;
    }

    sw::tile_entry entry_at(sw::software_tile_plan const& plan, std::uint32_t const x, std::uint32_t const y,
                            std::uint32_t const slot) {
        return plan.entries[tile_index(plan, x, y) * plan.entry_capacity + slot];
    }

    // ============================================================================================
    // 1. THE PROJECTION ARTIFACT: what kind of camera is this, and where is its near plane?
    // ============================================================================================
    void projection_artifact() {
        auto const perspective_camera = sw::prepare_camera(camera_parameters_for(perspective(2.0f, 1.0f, 1.0f), 640u, 360u));
        CHECK(perspective_camera.has_value());
        CHECK(perspective_camera->kind == sw::projection_kind::perspective);
        CHECK(perspective_camera->near_plane_known);
        CHECK(nearly(perspective_camera->near_plane, 2.0, 1e-6));
        CHECK(nearly(perspective_camera->proj_scale, 640.0)); // max(1*640, 1*360)
        CHECK(perspective_camera->width == 640u && perspective_camera->height == 360u);

        // a finite-far glm perspective must derive the same near plane: m22 = -(f+n)/(f-n), m32 = -2fn/(f-n)
        auto finite_far = perspective(2.0f, 1.0f, 1.0f);
        float const far_plane = 500.0f;
        at(finite_far, 2, 2) = -(far_plane + 2.0f) / (far_plane - 2.0f);
        at(finite_far, 3, 2) = -2.0f * far_plane * 2.0f / (far_plane - 2.0f);
        auto const finite_far_camera = sw::prepare_camera(camera_parameters_for(finite_far, 640u, 360u));
        CHECK(finite_far_camera.has_value());
        CHECK(finite_far_camera->kind == sw::projection_kind::perspective);
        CHECK(nearly(finite_far_camera->near_plane, 2.0, 1e-5));

        auto const ortho_camera = sw::prepare_camera(camera_parameters_for(orthographic(1.0f, 10.0f, 1.0f, 1.0f), 100u, 100u));
        CHECK(ortho_camera.has_value());
        CHECK(ortho_camera->kind == sw::projection_kind::orthographic);
        CHECK(ortho_camera->near_plane_known);
        CHECK(nearly(ortho_camera->near_plane, 1.0, 1e-5));
        CHECK(nearly(ortho_camera->proj_scale, 100.0));

        // an unsupported projection is REFUSED, not guessed: it can never move a cluster to software
        auto const unsupported = sw::prepare_camera(camera_parameters_for(std::array<float, 16>{}, 640u, 360u));
        CHECK(!unsupported.has_value());
        CHECK(unsupported.error() == sw::software_error::unsupported_projection);

        // invalid extents/cameras are named errors, never a silently empty plan
        CHECK(!sw::prepare_camera(sw::camera_parameters{}).has_value());
        auto bad_camera = camera_parameters_for(perspective(2.0f, 1.0f, 1.0f), 0u, 360u);
        auto const zero_extent = sw::prepare_camera(bad_camera);
        CHECK(!zero_extent.has_value() && zero_extent.error() == sw::software_error::invalid_extent);
        bad_camera = camera_parameters_for(perspective(2.0f, 1.0f, 1.0f), 640u, 360u);
        bad_camera.proj[0] = std::numeric_limits<float>::quiet_NaN();
        auto const nan_camera = sw::prepare_camera(bad_camera);
        CHECK(!nan_camera.has_value() && nan_camera.error() == sw::software_error::invalid_camera);
    }

    // ============================================================================================
    // 2. THE CONSERVATIVE CRITERION: the formula, the strict threshold boundary, the model scale
    // ============================================================================================
    void classification_formula() {
        auto const camera = prepared(camera_parameters_for(perspective(1.0f, 1.0f, 1.0f), 1000u, 1000u));
        auto const model = identity();
        double const proj_scale = 1000.0;
        double const w = 10.0;
        double const world_radius = 0.005 * rigid_scale;

        // a radius whose diameter is well below 1 px: 2*(r*sqrt3)*(proj_scale/2)/(w - r*sqrt3)
        auto const small = sw::classify(record(0.0f, 0.0f, -10.0f, 0.005f), model, camera, 1.0f);
        CHECK(small.finite);
        CHECK(small.front_of_near);
        CHECK(nearly(small.view_depth, w, 1e-6));
        CHECK(nearly(small.world_radius, world_radius, 1e-6));
        CHECK(nearly(small.diameter_upper_px, expected_perspective_diameter(world_radius, w, proj_scale), 1e-6));
        CHECK(small.diameter_upper_px < 1.0);
        CHECK(small.subpixel);

        // the threshold comparison is STRICT (`diameter_upper < threshold_px`): the reported diameter, cast to the
        // threshold's float, is a boundary the classification may or may not clear - the CONDITION below is exact
        float const at_threshold = static_cast<float>(small.diameter_upper_px);
        auto const boundary = sw::classify(record(0.0f, 0.0f, -10.0f, 0.005f), model, camera, at_threshold);
        CHECK(boundary.diameter_upper_px == small.diameter_upper_px);
        CHECK(boundary.subpixel == (small.diameter_upper_px < static_cast<double>(at_threshold)));
        // the next float above the reported diameter always clears the threshold (it is strictly greater than it)
        float const just_above = std::nextafter(at_threshold, std::numeric_limits<float>::infinity());
        CHECK(sw::classify(record(0.0f, 0.0f, -10.0f, 0.005f), model, camera, just_above).subpixel);

        // a radius whose diameter is over 1 px stays on the hardware path
        auto const big = sw::classify(record(0.0f, 0.0f, -10.0f, 0.02f), model, camera, 1.0f);
        CHECK(big.finite && big.front_of_near);
        CHECK(big.diameter_upper_px > 1.0);
        CHECK(!big.subpixel);
        // ... and the 2.0 px band classifies a diameter between the two thresholds
        auto const between = sw::classify(record(0.0f, 0.0f, -10.0f, 0.009f), model, camera, 1.0f);
        CHECK(!between.subpixel);
        CHECK(sw::classify(record(0.0f, 0.0f, -10.0f, 0.009f), model, camera, 2.0f).subpixel);

        // an infinite/zero/negative threshold never classifies anything (the caller rejects those configs)
        CHECK(!sw::classify(record(0.0f, 0.0f, -10.0f, 0.0f), model, camera, 0.0f).subpixel);
        CHECK(!sw::classify(record(0.0f, 0.0f, -10.0f, 0.005f), model, camera, -1.0f).subpixel);
        CHECK(!sw::classify(record(0.0f, 0.0f, -10.0f, 0.005f), model, camera, std::numeric_limits<float>::quiet_NaN()).subpixel);
    }

    void classification_orthographic() {
        auto const camera = prepared(camera_parameters_for(orthographic(1.0f, 100.0f, 1.0f, 1.0f), 1000u, 1000u));
        auto const model = identity();
        // spec §3.1 orthographic: diameter_upper = 2*r*max(|p00|*w, |p11|*h)*0.5 = world_radius * proj_scale
        double const world_radius = 0.0005 * rigid_scale;
        auto const small = sw::classify(record(0.0f, 0.0f, -10.0f, 0.0005f), model, camera, 1.0f);
        CHECK(small.finite && small.front_of_near);
        CHECK(nearly(small.diameter_upper_px, world_radius * 1000.0, 1e-6));
        CHECK(small.diameter_upper_px < 1.0);
        CHECK(small.subpixel);
        // the same record under a perspective camera of the same proj_scale has diameter / (w-r) - the formulas differ
        auto const perspective_camera = prepared(camera_parameters_for(perspective(1.0f, 1.0f, 1.0f), 1000u, 1000u));
        auto const persp = sw::classify(record(0.0f, 0.0f, -10.0f, 0.0005f), model, perspective_camera, 1.0f);
        CHECK(nearly(persp.diameter_upper_px, expected_perspective_diameter(world_radius, 10.0, 1000.0), 1e-6));
        CHECK(persp.diameter_upper_px < small.diameter_upper_px * 0.2);
        // a record whose orthographic diameter is well over 1 px is hardware at 1.0 and software at a wide threshold
        auto const wide = sw::classify(record(0.0f, 0.0f, -10.0f, 0.009f), model, camera, 1.0f);
        CHECK(nearly(wide.diameter_upper_px, 0.009 * rigid_scale * 1000.0, 1e-6));
        CHECK(wide.diameter_upper_px > 1.0);
        CHECK(!wide.subpixel);
        CHECK(wide.diameter_upper_px < 20.0);
        CHECK(sw::classify(record(0.0f, 0.0f, -10.0f, 0.009f), model, camera, 20.0f).subpixel);
    }

    void classification_model_scale() {
        auto const camera = prepared(camera_parameters_for(perspective(1.0f, 1.0f, 1.0f), 1000u, 1000u));
        // non-uniform scale: the Frobenius norm of diag(3,1,1) is sqrt(11), an upper bound on the operator norm
        auto const scaled = scaled_model(3.0f, 1.0f, 1.0f);
        CHECK(nearly(sw::model_radius_scale(scaled), std::sqrt(11.0), 1e-6));
        auto const scaled_classification = sw::classify(record(0.0f, 0.0f, -10.0f, 0.001f), scaled, camera, 1.0f);
        CHECK(nearly(scaled_classification.world_radius, 0.001 * std::sqrt(11.0), 1e-6));
        auto const uniform = sw::classify(record(0.0f, 0.0f, -10.0f, 0.001f), identity(), camera, 1.0f);
        CHECK(nearly(uniform.world_radius, 0.001 * rigid_scale, 1e-6));
        CHECK(scaled_classification.diameter_upper_px > uniform.diameter_upper_px);
        // shear: linear part [[1,s],[0,1]] (plus the z axis) has Frobenius sqrt(3+s^2) >= its operator norm
        auto sheared = identity();
        at(sheared, 0, 1) = 0.75f; // column 0, row 1
        CHECK(nearly(sw::model_radius_scale(sheared), std::sqrt(3.0 + 0.75 * 0.75), 1e-6));
        CHECK(sw::model_radius_scale(sheared) > rigid_scale);
        // translation moves the world centre but not the linear part: z=-10 stays 10 m in front of an identity view
        auto translated = scaled_model(1.0f, 1.0f, 1.0f, 0.0f, 0.0f, -10.0f);
        auto const moved = sw::classify(record(0.0f, 0.0f, 0.0f, 0.001f), translated, camera, 1.0f);
        CHECK(nearly(moved.view_depth, 10.0, 1e-6));
        CHECK(nearly(sw::model_radius_scale(translated), rigid_scale, 1e-6));
        // non-finite model scale fails OPEN (kept on hardware)
        auto nan_model = scaled_model(1.0f, 1.0f, 1.0f);
        at(nan_model, 0, 0) = std::numeric_limits<float>::quiet_NaN();
        auto const nan = sw::classify(record(0.0f, 0.0f, -10.0f, 0.001f), nan_model, camera, 1.0f);
        CHECK(!nan.finite);
        CHECK(!nan.subpixel);
    }

    void classification_near_and_degenerate() {
        // near plane 2: a sphere that crosses the near plane is NOT software-eligible (the software raster v1 has no
        // near-plane clipper; its triangles are clipped by the hardware path and must stay there)
        auto const camera = prepared(camera_parameters_for(perspective(2.0f, 1.0f, 1.0f), 1000u, 1000u));
        auto const model = identity();
        auto const crossing = sw::classify(record(0.0f, 0.0f, -2.5f, 0.6f), model, camera, 100.0f);
        CHECK(crossing.finite);
        CHECK(!crossing.front_of_near);
        CHECK(!crossing.subpixel);
        auto const behind = sw::classify(record(0.0f, 0.0f, 3.0f, 0.01f), model, camera, 100.0f);
        CHECK(behind.finite);
        CHECK(!behind.front_of_near);
        CHECK(!behind.subpixel);
        // w - world_radius <= 0: the sphere reaches behind the eye even though its centre is in front
        auto const eye_crossing = sw::classify(record(0.0f, 0.0f, -1.0f, 2.0f), model, camera, 100.0f);
        CHECK(!eye_crossing.front_of_near);
        // the 0.98 margin: w - r must clear the near plane by more than 2% of the depth span
        auto const at_margin = sw::classify(record(0.0f, 0.0f, -10.0f, 0.0f), model, camera, 100.0f);
        CHECK(at_margin.front_of_near);
        auto const tiny = sw::classify(record(0.0f, 0.0f, -10.0f, 0.001f), model, camera, 100.0f);
        CHECK(tiny.front_of_near && tiny.subpixel);
        // a sphere just behind the near plane (w-r <= near) is refused: world_radius >= w - near = 8
        auto const hugging_near = sw::classify(record(0.0f, 0.0f, -10.0f, 8.0f), model, camera, 100.0f);
        CHECK(!hugging_near.front_of_near);
        // NaN / negative / infinite radius and NaN centre: all fail OPEN (never classified as software)
        for (meshlet const degenerate : {record(0.0f, 0.0f, -10.0f, std::numeric_limits<float>::quiet_NaN()),
                                         record(0.0f, 0.0f, -10.0f, -1.0f),
                                         record(0.0f, 0.0f, -10.0f, std::numeric_limits<float>::infinity()),
                                         record(std::numeric_limits<float>::quiet_NaN(), 0.0f, -10.0f, 0.001f)}) {
            auto const result = sw::classify(degenerate, model, camera, 100.0f);
            CHECK(!result.subpixel);
        }
        // a zero-radius record at a valid depth is a point: subpixel and in front (a cut cluster can be tiny but
        // still a real triangle window; the criterion is about projected size)
        auto const point = sw::classify(record(0.0f, 0.0f, -10.0f, 0.0f), model, camera, 1.0f);
        CHECK(point.finite && point.front_of_near && point.subpixel);
    }

    // ============================================================================================
    // 3. PROJECTED BOUNDS: screen centre and the conservative pixel radius / tile range
    // ============================================================================================
    void projected_bounds() {
        // orthographic p00=p11=1 over 64x64 px (4x4 tiles of 16 px): pixel = (world_x*0.5 + 0.5) * 64 = 32*x + 32
        auto const parameters = camera_parameters_for(orthographic(1.0f, 10.0f, 1.0f, 1.0f), 64u, 64u);
        auto const camera = prepared(parameters);
        auto const grid = sw::make_tile_grid(64u, 64u);
        CHECK(grid.size == 16u);
        CHECK(grid.tiles_x == 4u && grid.tiles_y == 4u);
        CHECK(grid.tile_count() == 16u);
        // world (1/64, 1/64, -10) -> pixel (32.5, 32.5) -> tile (2,2); a tiny record covers exactly one tile
        auto const projected = sw::project(record(0.015625f, 0.015625f, -10.0f, 0.0001f), identity(), camera, grid);
        CHECK(projected.finite && projected.front_of_near && projected.on_screen);
        CHECK(nearly(projected.center_px[0], 32.5, 1e-5));
        CHECK(nearly(projected.center_px[1], 32.5, 1e-5));
        CHECK(projected.tile_min_x == 2 && projected.tile_max_x == 2);
        CHECK(projected.tile_min_y == 2 && projected.tile_max_y == 2);
        // world x = 0.8 -> pixel 32*0.8 + 32 = 57.6 -> tile 3
        auto const right = sw::project(record(0.8f, 0.015625f, -10.0f, 0.0001f), identity(), camera, grid);
        CHECK(right.tile_min_x == 3 && right.tile_max_x == 3);
        CHECK(right.tile_min_y == 2 && right.tile_max_y == 2);
        // a record whose conservative radius is ~10 px spans tiles 1..2 around the centre (22.5 .. 42.5)
        auto const wide = sw::project(record(0.015625f, 0.015625f, -10.0f, 0.1804f), identity(), camera, grid);
        CHECK(wide.radius_px > 9.0f && wide.radius_px < 11.0f);
        CHECK(wide.tile_min_x == 1 && wide.tile_max_x == 2);
        CHECK(wide.tile_min_y == 1 && wide.tile_max_y == 2);
        // off-screen: world x = 5 -> pixel far beyond 64; the record is classified but covers no tile
        auto const off = sw::project(record(5.0f, 0.015625f, -10.0f, 0.0001f), identity(), camera, grid);
        CHECK(off.finite);
        CHECK(!off.on_screen);
        CHECK(off.tile_max_x < off.tile_min_x);
        // a point EXACTLY on a tile boundary is conservatively filed under both neighbouring tiles: the box is rounded
        // out, so a rounding of the bound can never leave a covered tile out (world x=0 -> pixel 32.0)
        auto const boundary = sw::project(record(0.0f, 0.015625f, -10.0f, 0.0001f), identity(), camera, grid);
        CHECK(nearly(boundary.center_px[0], 32.0, 1e-5));
        CHECK(boundary.tile_min_x == 1 && boundary.tile_max_x == 2);
        // the projected bounds use the near-point formula: the SAME record is larger in pixels nearer the eye
        auto const perspective_camera = prepared(camera_parameters_for(perspective(0.5f, 1.5f, 1.5f), 64u, 64u));
        auto const near_1 = sw::project(record(0.0f, 0.0f, -5.0f, 0.1f), identity(), perspective_camera, grid);
        auto const far_1 = sw::project(record(0.0f, 0.0f, -20.0f, 0.1f), identity(), perspective_camera, grid);
        CHECK(near_1.radius_px > far_1.radius_px);
        CHECK(nearly(near_1.center_px[0], 32.0, 1e-5));
        CHECK(nearly(far_1.center_px[0], 32.0, 1e-5));
    }

    // ============================================================================================
    // 4. THE STABLE SPLIT AND THE ID MAPPING (spec §3.1/§3.3)
    // ============================================================================================
    void stable_split() {
        // orthographic 1000x1000 px camera: diameter = world_radius * 1000 px, so r=0.004 -> ~6.9 px (hardware) and
        // r=0.0004 -> ~0.69 px (software) under the rigid-model sqrt(3) radius scale
        auto const camera = prepared(camera_parameters_for(orthographic(1.0f, 100.0f, 1.0f, 1.0f), 1000u, 1000u));
        std::vector<meshlet> const records = {
            record(0.0f, 0.0f, -10.0f, 0.004f),  // hardware
            record(0.0f, 0.0f, -10.0f, 0.0004f), // software
            record(0.0f, 0.0f, -10.0f, 0.004f),  // hardware
            record(0.0f, 0.0f, -10.0f, 0.0004f), // software
            record(0.0f, 0.0f, -10.0f, 0.0004f), // software
        };
        sw::item_records const item{.item_index = 7u, .model = identity(), .records = span_of(records)};
        deren::utility::vessel::vector<std::uint32_t> order;
        auto const split = sw::split_records(item, camera, 1.0f, order);
        CHECK(split.item_index == 7u);
        CHECK(split.record_count == 5u);
        CHECK(split.hardware_count == 2u);
        CHECK(split.software_count == 3u);
        // the prefix is the hardware records in their ORIGINAL relative order; the suffix the software ones likewise;
        // the id of suffix k is hardware_count + k, i.e. its lane position
        CHECK(order.size() == 5u);
        CHECK(order[0] == 0u && order[1] == 2u);                   // hardware: original indices 0, 2
        CHECK(order[2] == 1u && order[3] == 3u && order[4] == 4u); // software: 1, 3, 4
        CHECK(split.hardware_triangles == 2u);                     // two hardware records, one triangle each
        CHECK(split.software_triangles == 3u);                     // three software records, one triangle each
        // the split is deterministic: the same input gives the same order twice
        deren::utility::vessel::vector<std::uint32_t> order_again;
        auto const again = sw::split_records(item, camera, 1.0f, order_again);
        CHECK(again.hardware_count == split.hardware_count && again.software_count == split.software_count);
        CHECK(std::equal(order.begin(), order.end(), order_again.begin(), order_again.end()));
        // at 2.0 px the 6.9 px records stay hardware and the 0.69 px records stay software: the threshold moves the
        // boundary, not the stable order
        deren::utility::vessel::vector<std::uint32_t> order_two;
        auto const split_two = sw::split_records(item, camera, 2.0f, order_two);
        CHECK(split_two.hardware_count == 2u && split_two.software_count == 3u);
        // all-hardware and all-software inputs are the trivial splits
        std::vector<meshlet> const all_hw = {record(0.0f, 0.0f, -10.0f, 0.01f), record(0.0f, 0.0f, -10.0f, 0.01f)};
        std::vector<meshlet> const all_sw = {record(0.0f, 0.0f, -10.0f, 0.0001f), record(0.0f, 0.0f, -10.0f, 0.0001f)};
        deren::utility::vessel::vector<std::uint32_t> scratch;
        auto const hw_split = sw::split_records(
            sw::item_records{.item_index = 0u, .model = identity(), .records = span_of(all_hw)}, camera, 1.0f, scratch);
        CHECK(hw_split.hardware_count == 2u && hw_split.software_count == 0u);
        CHECK(scratch.size() == 2u && scratch[0] == 0u && scratch[1] == 1u);
        auto const sw_split = sw::split_records(
            sw::item_records{.item_index = 1u, .model = identity(), .records = span_of(all_sw)}, camera, 1.0f, scratch);
        CHECK(sw_split.hardware_count == 0u && sw_split.software_count == 2u);
        CHECK(scratch.size() == 2u && scratch[0] == 0u && scratch[1] == 1u);
    }

    // ============================================================================================
    // 5. TILE BINNING: determinism, the id at each tile slot, the measurement/strict capacity split
    // ============================================================================================
    void tile_binning() {
        // 1000x1000 px orthographic camera, tiles 63x63: pixel = 500*x + 500, so world (0,0) is pixel (500,500) ->
        // tile (31,31); world (0.8,0) is pixel (900,500) -> tile (56,31)
        auto const parameters = camera_parameters_for(orthographic(1.0f, 10.0f, 1.0f, 1.0f), 1000u, 1000u);
        std::vector<meshlet> const item_zero = {record(0.0f, 0.0f, -10.0f, 0.002f)};
        std::vector<meshlet> const item_one = {record(0.8f, 0.0f, -10.0f, 0.002f)};
        std::vector<sw::item_records> const items = {
            sw::item_records{.item_index = 0u, .model = identity(), .records = span_of(item_zero)},
            sw::item_records{.item_index = 1u, .model = identity(), .records = span_of(item_one)},
        };
        auto const plan = sw::build_tile_lists_measure(items, parameters, 4.0f, 32u); // 3.46 px diameter < 4
        CHECK(plan.grid.tiles_x == 63u && plan.grid.tiles_y == 63u);
        CHECK(plan.item_count == 2u);
        CHECK(plan.cluster_count == 2u);
        CHECK(plan.entry_count == 2u);
        CHECK(plan.overflow == false);
        CHECK(plan.max_tile_entries == 1u);
        CHECK(plan.counts[tile_index(plan, 31u, 31u)] == 1u);
        CHECK(plan.counts[tile_index(plan, 56u, 31u)] == 1u);
        auto const first = entry_at(plan, 56u, 31u, 0u);
        CHECK(first.item_index == 1u && first.meshlet_id == 0u);
        auto const second = entry_at(plan, 31u, 31u, 0u);
        CHECK(second.item_index == 0u && second.meshlet_id == 0u);
        CHECK(plan.items.size() == 2u && plan.items[0].software_count == 1u && plan.items[1].software_count == 1u);

        // determinism: a second build of the same input has identical counts and entries
        auto const again = sw::build_tile_lists_measure(items, parameters, 4.0f, 32u);
        CHECK(again.counts == plan.counts);
        CHECK(again.entries.size() == plan.entries.size());
        CHECK(std::equal(again.entries.begin(), again.entries.end(), plan.entries.begin(),
                         [](sw::tile_entry const& a, sw::tile_entry const& b) {
                             return a.item_index == b.item_index && a.meshlet_id == b.meshlet_id;
                         }));

        // capacity: three software records on ONE tile with capacity 2 -> the measurement plan reports the exact
        // count/max/overflow (so Task 1 can size the default), the production plan REFUSES the frame (no truncation)
        std::vector<meshlet> const three = {record(0.0f, 0.0f, -10.0f, 0.002f), record(0.0f, 0.0f, -10.0f, 0.002f),
                                            record(0.0f, 0.0f, -10.0f, 0.002f)};
        std::vector<sw::item_records> const crowded = {
            sw::item_records{.item_index = 0u, .model = identity(), .records = span_of(three)}};
        auto const measured = sw::build_tile_lists_measure(crowded, parameters, 4.0f, 2u);
        CHECK(measured.overflow);
        CHECK(measured.overflow_tiles == 1u);
        CHECK(measured.max_tile_entries == 3u);
        CHECK(measured.counts[tile_index(measured, 31u, 31u)] == 3u); // exact, NOT truncated to the capacity
        CHECK(measured.entry_count == 3u);
        auto const strict = sw::build_tile_lists(crowded, parameters, 4.0f, 2u);
        CHECK(!strict.has_value());
        CHECK(strict.error() == sw::software_error::tile_overflow);
        // the same three records with capacity 3 are accepted, slots in item/record order with ids 0,1,2
        auto const fits = sw::build_tile_lists(crowded, parameters, 4.0f, 3u);
        CHECK(fits.has_value());
        CHECK(!fits->overflow && fits->max_tile_entries == 3u);
        CHECK(entry_at(*fits, 31u, 31u, 0u).meshlet_id == 0u);
        CHECK(entry_at(*fits, 31u, 31u, 1u).meshlet_id == 1u);
        CHECK(entry_at(*fits, 31u, 31u, 2u).meshlet_id == 2u);

        // mixed item: 2 hardware + 3 software; the software ids in the tile are hardware_count + suffix index (2,3,4)
        std::vector<meshlet> const mixed = {
            record(0.0f, 0.0f, -10.0f, 0.004f),  // hardware
            record(0.0f, 0.0f, -10.0f, 0.0004f), // software k=0 -> id 2
            record(0.0f, 0.0f, -10.0f, 0.004f),  // hardware
            record(0.0f, 0.0f, -10.0f, 0.0004f), // software k=1 -> id 3
            record(0.0f, 0.0f, -10.0f, 0.0004f), // software k=2 -> id 4
        };
        std::vector<sw::item_records> const one_item = {
            sw::item_records{.item_index = 3u, .model = identity(), .records = span_of(mixed)}};
        auto const mixed_plan = sw::build_tile_lists_measure(one_item, parameters, 1.0f, 8u);
        CHECK(mixed_plan.items[0].hardware_count == 2u && mixed_plan.items[0].software_count == 3u);
        CHECK(mixed_plan.cluster_count == 3u);
        CHECK(entry_at(mixed_plan, 31u, 31u, 0u).item_index == 3u &&
              entry_at(mixed_plan, 31u, 31u, 0u).meshlet_id == 2u);
        CHECK(entry_at(mixed_plan, 31u, 31u, 1u).meshlet_id == 3u);
        CHECK(entry_at(mixed_plan, 31u, 31u, 2u).meshlet_id == 4u);

        // empty input: a well-formed empty plan, not an error (the frame then runs the hardware path)
        auto const empty = sw::build_tile_lists_measure({}, parameters, 1.0f, 32u);
        CHECK(empty.item_count == 0u && empty.cluster_count == 0u && empty.entry_count == 0u);
        CHECK(empty.counts.size() == 63u * 63u);
        CHECK(empty.entries.size() == 63u * 63u * 32u);
        CHECK(!empty.overflow);
    }

    // ============================================================================================
    // 6. THRESHOLD 1.0 vs 2.0: the two numbers Task 1 reports (and the capacity decision uses)
    // ============================================================================================
    void threshold_dual_mode() {
        // orthographic 1000 px: diameter = r * sqrt(3) * 1000; choose r for 0.5, 1.5 and 3.0 px
        auto const parameters = camera_parameters_for(orthographic(1.0f, 100.0f, 1.0f, 1.0f), 1000u, 1000u);
        std::vector<meshlet> const records = {
            record(0.0f, 0.0f, -10.0f, static_cast<float>(0.5 / (rigid_scale * 1000.0))), // 0.5 px
            record(0.0f, 0.0f, -10.0f, static_cast<float>(1.5 / (rigid_scale * 1000.0))), // 1.5 px
            record(0.0f, 0.0f, -10.0f, static_cast<float>(3.0 / (rigid_scale * 1000.0))), // 3.0 px
        };
        std::vector<sw::item_records> const items = {
            sw::item_records{.item_index = 0u, .model = identity(), .records = span_of(records)}};
        auto const one = sw::build_tile_lists_measure(items, parameters, 1.0f, 32u);
        auto const two = sw::build_tile_lists_measure(items, parameters, 2.0f, 32u);
        CHECK(one.items[0].software_count == 1u && one.items[0].hardware_count == 2u);
        CHECK(two.items[0].software_count == 2u && two.items[0].hardware_count == 1u);
        CHECK(one.cluster_count == 1u && two.cluster_count == 2u);
        CHECK(two.items[0].software_triangles == 2u); // two software records, one triangle each
        // the 2.0 split's software ids start at its own hardware count (1), so the tile entries are ids 1 and 2
        CHECK(two.entry_count == 2u);
        CHECK(entry_at(two, 31u, 31u, 0u).meshlet_id == 1u);
        CHECK(entry_at(two, 31u, 31u, 1u).meshlet_id == 2u);
    }

    // ============================================================================================
    // 7. ERRORS: every invalid configuration is a named error, never a silently empty/partial plan
    // ============================================================================================
    void errors() {
        std::vector<meshlet> const records = {record(0.0f, 0.0f, -10.0f, 0.001f)};
        std::vector<sw::item_records> const items = {
            sw::item_records{.item_index = 0u, .model = identity(), .records = span_of(records)}};
        auto const good = camera_parameters_for(perspective(1.0f, 1.0f, 1.0f), 64u, 64u);
        CHECK(sw::build_tile_lists_measure(items, good, 1.0f, 32u).cluster_count <= 1u);
        auto const zero_extent = sw::build_tile_lists_measure(
            items, camera_parameters_for(perspective(1.0f, 1.0f, 1.0f), 0u, 64u), 1.0f, 32u);
        CHECK(zero_extent.grid.tiles_x == 0u && zero_extent.grid.tiles_y == 0u);
        CHECK(zero_extent.entry_count == 0u);
        auto const bad_threshold = sw::build_tile_lists(items, good, 0.0f, 32u);
        CHECK(!bad_threshold.has_value() && bad_threshold.error() == sw::software_error::invalid_threshold);
        auto const nan_threshold = sw::build_tile_lists(items, good, std::numeric_limits<float>::quiet_NaN(), 32u);
        CHECK(!nan_threshold.has_value() && nan_threshold.error() == sw::software_error::invalid_threshold);
        auto const bad_capacity = sw::build_tile_lists(items, good, 1.0f, 0u);
        CHECK(!bad_capacity.has_value() && bad_capacity.error() == sw::software_error::invalid_capacity);
        auto const bad_extent = sw::build_tile_lists(
            items, camera_parameters_for(perspective(1.0f, 1.0f, 1.0f), 0u, 64u), 1.0f, 32u);
        CHECK(!bad_extent.has_value() && bad_extent.error() == sw::software_error::invalid_extent);
        auto const unsupported = sw::build_tile_lists(
            items, camera_parameters_for(std::array<float, 16>{}, 64u, 64u), 1.0f, 32u);
        CHECK(!unsupported.has_value() && (unsupported.error() == sw::software_error::unsupported_projection ||
                                           unsupported.error() == sw::software_error::invalid_camera));
        // a NaN record is fail-open (hardware), not an error: the plan is still produced, the record is just not moved
        std::vector<meshlet> const nan_records = {record(0.0f, 0.0f, -10.0f, std::numeric_limits<float>::quiet_NaN())};
        std::vector<sw::item_records> const nan_items = {
            sw::item_records{.item_index = 0u, .model = identity(), .records = span_of(nan_records)}};
        auto const nan_plan = sw::build_tile_lists_measure(nan_items, good, 1.0f, 32u);
        CHECK(nan_plan.items.size() == 1u && nan_plan.items[0].software_count == 0u);
        CHECK(nan_plan.cluster_count == 0u && nan_plan.entry_count == 0u);
    }
} // namespace

int main() {
    projection_artifact();
    classification_formula();
    classification_orthographic();
    classification_model_scale();
    classification_near_and_degenerate();
    projected_bounds();
    stable_split();
    tile_binning();
    threshold_dual_mode();
    errors();
    return deren::vk_test::finish("test_visibility_software");
}
