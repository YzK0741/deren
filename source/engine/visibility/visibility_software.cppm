/**
 * @file visibility_software.cppm
 * @defgroup engine_visibility_software M4 subpixel software raster: the CPU front half
 * @brief The conservative subpixel-cluster criterion, the stable [hardware prefix | software suffix] split and
 *        the 16x16 tile binning of the M4 hybrid visibility producer (spec §3.1/§3.3/§3.4).
 *
 * WHY THIS IS PURE CPU, AND WHY IT IS A MODULE OF ITS OWN. The M4 hybrid decides ON THE HOST, in the M1c planning
 * phase, which of a cut item's selected meshlet records the hardware raster will no longer dispatch and which ones a
 * tiled software raster would have to produce instead. That decision is arithmetic over the same view/projection/
 * model matrices the cut already used, and its failure mode is a HOLE: moving a cluster to the software path when
 * its sphere is not PROVABLY wholly in front of the near plane (the v1 software raster has no clipper) or not
 * provably smaller than the threshold loses visible geometry on a frame the class didn't cover. Keeping the
 * arithmetic free of Vulkan, of a device and of the shader compiler is what lets the headless tests pin the
 * conservative form: a too-eager classification is therefore a test failure, not a picture nobody understood.
 *
 * THE CONSERVATIVE CRITERION (spec §3.1), exactly:
 *
 *   world_center   = model * record.center
 *   model_scale    = ||model's linear 3x3 part||_F      (Frobenius norm, >= the operator norm -> conservative)
 *   world_radius   = record.radius * model_scale       (record.radius is already an outward-rounded bound)
 *   w              = -(view * world_center).z          (depth in front of the eye; standard perspective/ortho view)
 *   front_of_near  = world_radius < 0.98 * (w - near_plane)   AND   (w - world_radius) > 0
 *   focal          = max(|proj[0][0]| * width, |proj[1][1]| * height) * 0.5
 *   diameter_upper = 2 * world_radius * focal / (w - world_radius)      (perspective)
 *                  = world_radius * max(|proj[0][0]|*width, |proj[1][1]|*height)   (orthographic)
 *   subpixel       = front_of_near AND diameter_upper < threshold_px
 *
 * Every term is an UPPER bound on the true projected diameter: the sphere radius is scaled by the linear part's
 * Frobenius norm, and the perspective diameter uses the sphere's NEAREST point (w - radius), where the projection is
 * largest. Any error therefore leaves the cluster on the hardware path, which is the safe direction. The strict `<`
 * is deliberate: a cluster exactly at the threshold is not provably under it, so it stays hardware.
 *
 * THE SPLIT (spec §3.1/§3.3) is stable: hardware records keep their relative order as the run's PREFIX, software
 * records keep theirs as the SUFFIX. The packed 32-bit visibility id's `meshlet_in_item` field then needs no new
 * decode - the software record at suffix index k is called `hardware_count + k`, i.e. its lane position in the same
 * run the hardware ids index. The hardware dispatch becomes `hardware_count` groups (the prefix), which is the
 * "holes control" Task 1 measures.
 *
 * THE TILE BINNING (spec §3.4) is deterministic and never silently truncates: a record is filed under every 16x16
 * tile its conservative screen-space bounding box covers (clamped to the frame), in item order then record order,
 * with a fixed per-tile capacity. The MEASUREMENT entry point reports an over-capacity tile exactly (counts, max,
 * overflow flag) so Task 1 can size the default capacity from data; the PRODUCTION entry point refuses the whole
 * frame instead, because the hybrid must fall back to the complete hardware path rather than write a partial list.
 */
module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <vector>

export module deren.engine.visibility.software;

import deren.engine.visibility;
import deren.engine.meshlet;
import deren.utility.shared;

export namespace deren::engine::visibility::software {
    /// the software raster's tile edge in pixels (spec §3.4: 16x16, one workgroup per tile)
    inline constexpr std::uint32_t tile_size = 16u;
    /// the initial per-tile entry capacity; Task 1 measures the real per-tile maximum at 1.0/2.0 px and decides 32/64
    inline constexpr std::uint32_t tile_entry_capacity_default = 32u;
    /// the primary subpixel threshold (M1c's 1 px headline; 2.0 px is the measurement's second band, spec §9 Q1)
    inline constexpr float threshold_px_default = 1.0f;
    inline constexpr float threshold_px_secondary = 2.0f;
    /// how far in front of the near plane the sphere must be, conservatively: w-r > 0.98*(w-near) leaves room for the
    /// sphere's front-facing triangles not to be clipped, with a 2% margin on the depth span (spec §3.1: 0.98)
    inline constexpr double near_plane_safety_factor = 0.98;
    /// the extra pixels added to every side of a record's conservative screen box before tile selection: a guard for
    /// the float rounding of the bound and for a tile boundary landing exactly on the box edge. Conservative only.
    inline constexpr double tile_bounds_guard_px = 1e-6;
    /// the screen-space bounds of a projected sphere cannot be smaller than this (pixels); zero is legal (a point)
    inline constexpr float minimum_screen_radius_px = 0.0f;

    enum class projection_kind { perspective,
                                 orthographic,
                                 unsupported };

    /// why the CPU front half refused an input; a refused tile list means "this frame runs the complete hardware path"
    enum class software_error {
        invalid_extent,
        invalid_camera,
        invalid_threshold,
        invalid_capacity,
        unsupported_projection,
        tile_overflow,
    };

    /// the matrices and extent the caller classifies with, in the renderer's own storage convention
    struct camera_parameters {
        std::array<float, 16> view{}; ///< world -> view, column-major storage as glm::mat4 (the runtime memcpys it)
        std::array<float, 16> proj{}; ///< view -> clip, column-major storage as glm::mat4
        std::uint32_t width = 0u;
        std::uint32_t height = 0u;
    };

    /// the per-frame derivation of the camera artifact: nothing here changes while the frame's camera does not, so a
    /// caller that classifies a whole run calls prepare_camera once instead of re-deriving it 41,000 times
    struct camera_metrics {
        projection_kind kind = projection_kind::unsupported;
        bool near_plane_known = false;
        double near_plane = 0.0;
        double proj_scale = 0.0; ///< max(|proj[0][0]| * width, |proj[1][1]| * height) (pixels per NDC unit)
        std::array<float, 16> view{};
        std::array<float, 16> proj{};
        std::uint32_t width = 0u;
        std::uint32_t height = 0u;
    };

    [[nodiscard]] inline bool matrix_finite(std::array<float, 16> const& matrix) noexcept {
        return std::all_of(matrix.begin(), matrix.end(), [](float const value) { return std::isfinite(value); });
    }

    /**
     * @brief the Frobenius norm of a matrix's linear 3x3 part, i.e. a conservative upper bound on the operator norm
     * @note this is the radius scale the criterion needs: for any direction, |M*x| <= ||M||_F * |x|, so a sphere of
     *       radius r stays inside a sphere of radius r*||M||_F. Non-uniform scale and shear are both covered with no
     *       case analysis (a diagonal matrix's operator norm is its longest axis; a shear's is bigger than 1).
     */
    [[nodiscard]] inline double model_radius_scale(std::array<float, 16> const& model) noexcept {
        // column-major storage: element [column][row] lives at column*4 + row; the linear part is rows/columns 0..2
        double squared = 0.0;
        for (std::uint32_t column = 0u; column < 3u; ++column) {
            for (std::uint32_t row = 0u; row < 3u; ++row) {
                double const value = static_cast<double>(model[column * 4u + row]);
                squared += value * value;
            }
        }
        return std::sqrt(squared);
    }

    /// transform a point (w = 1) by one matrix in the renderer's column-major convention: result[row] =
    /// sum(column) m[column*4+row] * point[column]
    [[nodiscard]] inline std::array<double, 4> transform_point(std::array<float, 16> const& matrix,
                                                               std::array<double, 4> const& point) noexcept {
        std::array<double, 4> result{};
        for (std::uint32_t row = 0u; row < 4u; ++row) {
            double value = 0.0;
            for (std::uint32_t column = 0u; column < 4u; ++column) {
                value += static_cast<double>(matrix[column * 4u + row]) * point[column];
            }
            result[row] = value;
        }
        return result;
    }

    /// which of the two supported projection shapes this matrix is, from its own last two lanes (the same tell
    /// deren.engine.subtilis.selection uses to build the cut's view_parameters, so both agree on the artifact)
    [[nodiscard]] inline projection_kind projection_kind_of(std::array<float, 16> const& proj) noexcept {
        // perspective: m[2][3] = -1, m[3][3] = 0. orthographic: m[2][3] = 0, m[3][3] = 1.
        if (proj[2u * 4u + 3u] == -1.0f && proj[3u * 4u + 3u] == 0.0f) {
            return projection_kind::perspective;
        }
        if (proj[2u * 4u + 3u] == 0.0f && proj[3u * 4u + 3u] == 1.0f) {
            return projection_kind::orthographic;
        }
        return projection_kind::unsupported;
    }

    /**
     * @brief the camera artifact a whole frame's classification uses, or the named reason it is not classifiable
     * @note near plane derivation, from the renderer's two standard shapes:
     *       - perspective (finite or infinite far): n = m[3][2] / (m[2][2] - 1)     (m32/(m22-1) = n for
     *         m22 = -(f+n)/(f-n), m32 = -2fn/(f-n); the infinite-far limit keeps the same ratio),
     *       - orthographic: n = (m[3][2] + 1) / m[2][2]                             ((m32+1)/m22 = n for
     *         m22 = -2/(f-n), m32 = -(f+n)/(f-n)).
     *       The answer is rounded UP (nextafter toward +infinity): a larger near plane makes the front-of-near test
     *       stricter, which is the conservative direction. Any other shape is refused, so an unclassifiable camera
     *       never moves a cluster to software.
     */
    [[nodiscard]] inline std::expected<camera_metrics, software_error> prepare_camera(camera_parameters const& parameters) noexcept {
        if (parameters.width == 0u || parameters.height == 0u) {
            return std::unexpected{software_error::invalid_extent};
        }
        if (!matrix_finite(parameters.view) || !matrix_finite(parameters.proj)) {
            return std::unexpected{software_error::invalid_camera};
        }
        projection_kind const kind = projection_kind_of(parameters.proj);
        if (kind == projection_kind::unsupported) {
            return std::unexpected{software_error::unsupported_projection};
        }
        double const m22 = static_cast<double>(parameters.proj[2u * 4u + 2u]);
        double const m32 = static_cast<double>(parameters.proj[3u * 4u + 2u]);
        double near_plane = 0.0;
        if (kind == projection_kind::perspective) {
            double const denominator = m22 - 1.0;
            if (denominator == 0.0) {
                return std::unexpected{software_error::unsupported_projection};
            }
            near_plane = m32 / denominator;
        } else {
            if (m22 == 0.0) {
                return std::unexpected{software_error::unsupported_projection};
            }
            near_plane = (m32 + 1.0) / m22;
        }
        if (!std::isfinite(near_plane) || near_plane < 0.0) {
            return std::unexpected{software_error::unsupported_projection};
        }
        // conservative: the near plane used by the test is never smaller than the camera's own
        near_plane = std::nextafter(near_plane, std::numeric_limits<double>::infinity());
        double const proj_scale = std::max(std::abs(static_cast<double>(parameters.proj[0u]) * parameters.width),
                                           std::abs(static_cast<double>(parameters.proj[1u * 4u + 1u]) * parameters.height));
        if (!std::isfinite(proj_scale) || proj_scale <= 0.0) {
            return std::unexpected{software_error::unsupported_projection};
        }
        return camera_metrics{.kind = kind,
                              .near_plane_known = true,
                              .near_plane = near_plane,
                              .proj_scale = proj_scale,
                              .view = parameters.view,
                              .proj = parameters.proj,
                              .width = parameters.width,
                              .height = parameters.height};
    }

    /// one record's conservative projected measurements (the criterion itself; no screen mapping)
    struct record_measurement {
        bool finite = false;        ///< the inputs and every intermediate were finite: the numbers below are usable
        bool front_of_near = false; ///< the sphere is WHOLLY in front of the near plane with the 0.98 margin
        double world_radius = 0.0;  ///< record.radius * ||linear 3x3||_F
        double view_depth = 0.0;    ///< w = -(view * world_center).z
        double diameter_upper_px = std::numeric_limits<double>::infinity();
    };

    /**
     * @brief the criterion's arithmetic (spec §3.1), without the screen-space bounds: what the whole-frame
     *        classification needs, and the only part a per-frame split pays for
     * @note every early return leaves finite = false, i.e. the safe "not subpixel" answer; a bad record never throws
     */
    [[nodiscard]] inline record_measurement measure_record(meshlet const& record, std::array<float, 16> const& model,
                                                           camera_metrics const& camera) noexcept {
        record_measurement measurement{};
        if (camera.kind == projection_kind::unsupported || !camera.near_plane_known) {
            return measurement;
        }
        if (!matrix_finite(model)) {
            return measurement;
        }
        auto const record_finite = std::isfinite(record.center_x) && std::isfinite(record.center_y) &&
                                   std::isfinite(record.center_z) && std::isfinite(record.radius) && record.radius >= 0.0f;
        if (!record_finite) {
            return measurement;
        }
        double const scale = model_radius_scale(model);
        double const world_radius = static_cast<double>(record.radius) * scale;
        std::array<double, 4> const world_center =
            transform_point(model, {static_cast<double>(record.center_x), static_cast<double>(record.center_y),
                                    static_cast<double>(record.center_z), 1.0});
        if (!std::isfinite(world_center[0]) || !std::isfinite(world_center[1]) || !std::isfinite(world_center[2]) ||
            !std::isfinite(world_radius)) {
            return measurement;
        }
        std::array<double, 4> const view_center = transform_point(camera.view, world_center);
        if (!std::isfinite(view_center[0]) || !std::isfinite(view_center[1]) || !std::isfinite(view_center[2])) {
            return measurement;
        }
        double const w = -view_center[2];
        if (!std::isfinite(w)) {
            return measurement;
        }
        double const before_near_margin = near_plane_safety_factor * (w - camera.near_plane);
        bool const front_of_near = (w - world_radius) > 0.0 && world_radius < before_near_margin;
        double diameter = std::numeric_limits<double>::infinity();
        if (w - world_radius > 0.0) {
            if (camera.kind == projection_kind::perspective) {
                diameter = 2.0 * world_radius * (camera.proj_scale * 0.5) / (w - world_radius);
            } else {
                diameter = world_radius * camera.proj_scale;
            }
        }
        measurement.finite = true;
        measurement.front_of_near = front_of_near;
        measurement.world_radius = world_radius;
        measurement.view_depth = w;
        measurement.diameter_upper_px = diameter;
        return measurement;
    }

    struct subpixel_classification {
        bool finite = false;
        bool front_of_near = false;
        bool subpixel = false;
        double diameter_upper_px = std::numeric_limits<double>::infinity();
        double world_radius = 0.0;
        double view_depth = 0.0;
    };

    /**
     * @brief THE criterion (spec §3.1): is this record's projected diameter provably under @p threshold_px, with the
     *        sphere provably wholly in front of the near plane?
     * @param threshold_px the primary threshold (1.0 = M1c's headline; 2.0 is the measurement's second band)
     * @note a non-finite or non-positive threshold classifies NOTHING (the caller refuses the config; this overload
     *       is the convenience form used by tests)
     */
    [[nodiscard]] inline subpixel_classification classify(meshlet const& record, std::array<float, 16> const& model,
                                                          camera_metrics const& camera, float threshold_px) noexcept {
        subpixel_classification result{};
        if (!std::isfinite(threshold_px) || threshold_px <= 0.0f) {
            return result;
        }
        auto const measurement = measure_record(record, model, camera);
        result.finite = measurement.finite;
        result.front_of_near = measurement.front_of_near;
        result.diameter_upper_px = measurement.diameter_upper_px;
        result.world_radius = measurement.world_radius;
        result.view_depth = measurement.view_depth;
        result.subpixel = measurement.finite && measurement.front_of_near && measurement.diameter_upper_px < static_cast<double>(threshold_px);
        return result;
    }

    [[nodiscard]] inline subpixel_classification classify(meshlet const& record, std::array<float, 16> const& model,
                                                          camera_parameters const& parameters, float threshold_px) noexcept {
        auto const camera = prepare_camera(parameters);
        if (!camera.has_value()) {
            return {};
        }
        return classify(record, model, *camera, threshold_px);
    }

    struct tile_grid {
        std::uint32_t size = tile_size;
        std::uint32_t tiles_x = 0u;
        std::uint32_t tiles_y = 0u;
        [[nodiscard]] constexpr std::uint32_t tile_count() const noexcept {
            return this->tiles_x * this->tiles_y;
        }
    };

    [[nodiscard]] inline tile_grid make_tile_grid(std::uint32_t const width, std::uint32_t const height) noexcept {
        return tile_grid{.size = tile_size,
                         .tiles_x = (width + tile_size - 1u) / tile_size,
                         .tiles_y = (height + tile_size - 1u) / tile_size};
    }

    /// one record's conservative screen-space box and the inclusive tile range it covers (empty when off-screen)
    struct projected_record {
        bool finite = false;
        bool front_of_near = false;
        bool on_screen = false;
        double diameter_upper_px = std::numeric_limits<double>::infinity();
        std::array<float, 2> center_px{};
        float radius_px = 0.0f; ///< conservative projected radius (half the diameter bound)
        std::int32_t tile_min_x = 0;
        std::int32_t tile_min_y = 0;
        std::int32_t tile_max_x = -1; ///< tile_max < tile_min == no tiles covered
        std::int32_t tile_max_y = -1;
    };

    /**
     * @brief the criterion plus the conservative screen box, for binning
     * @note the box is the projected CENTRE expanded by half the diameter upper bound and a one-pixel guard, then
     *       clamped to the frame. The centre is exact for the projection (clip/w, the same transform the hardware
     *       raster applies), while the expansion is the same conservative bound the criterion used - so a record
     *       classified subpixel can only ever cover MORE tiles than its true footprint, never fewer.
     */
    [[nodiscard]] inline projected_record project(meshlet const& record, std::array<float, 16> const& model,
                                                  camera_metrics const& camera, tile_grid const& grid) noexcept {
        projected_record result{};
        auto const measurement = measure_record(record, model, camera);
        result.finite = measurement.finite;
        result.front_of_near = measurement.front_of_near;
        result.diameter_upper_px = measurement.diameter_upper_px;
        if (!measurement.finite) {
            return result;
        }
        std::array<double, 4> const world_center =
            transform_point(model, {static_cast<double>(record.center_x), static_cast<double>(record.center_y),
                                    static_cast<double>(record.center_z), 1.0});
        std::array<double, 4> const view_center = transform_point(camera.view, world_center);
        std::array<double, 4> const clip = transform_point(camera.proj, view_center);
        if (!std::isfinite(clip[0]) || !std::isfinite(clip[1]) || !std::isfinite(clip[2]) || !(clip[3] > 0.0)) {
            return result;
        }
        double const ndc_x = clip[0] / clip[3];
        double const ndc_y = clip[1] / clip[3];
        if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y)) {
            return result;
        }
        double const center_x = (ndc_x * 0.5 + 0.5) * static_cast<double>(camera.width);
        double const center_y = (ndc_y * 0.5 + 0.5) * static_cast<double>(camera.height);
        double const radius_px = measurement.front_of_near ? std::max<double>(minimum_screen_radius_px,
                                                                              measurement.diameter_upper_px * 0.5)
                                                           : std::numeric_limits<double>::infinity();
        if (!std::isfinite(radius_px) || !std::isfinite(center_x) || !std::isfinite(center_y)) {
            return result;
        }
        result.center_px = {static_cast<float>(center_x), static_cast<float>(center_y)};
        result.radius_px = static_cast<float>(radius_px);
        // the conservative pixel box, rounded OUT by the guard, then clipped to the frame
        double const box_min_x = center_x - radius_px - tile_bounds_guard_px;
        double const box_min_y = center_y - radius_px - tile_bounds_guard_px;
        double const box_max_x = center_x + radius_px + tile_bounds_guard_px;
        double const box_max_y = center_y + radius_px + tile_bounds_guard_px;
        if (grid.tiles_x == 0u || grid.tiles_y == 0u) {
            return result;
        }
        bool const intersects = box_max_x > 0.0 && box_max_y > 0.0 && box_min_x < static_cast<double>(camera.width) &&
                                box_min_y < static_cast<double>(camera.height);
        if (!intersects) {
            return result;
        }
        auto const clamp_tiles = [](double const low, double const high, std::uint32_t const tile_count) {
            double const low_tile = std::floor(low / static_cast<double>(tile_size));
            double const high_tile = std::floor(high / static_cast<double>(tile_size));
            auto const low_clamped = static_cast<std::int32_t>(std::clamp(low_tile, 0.0, static_cast<double>(tile_count - 1u)));
            auto const high_clamped = static_cast<std::int32_t>(std::clamp(high_tile, 0.0, static_cast<double>(tile_count - 1u)));
            return std::array<std::int32_t, 2>{low_clamped, high_clamped};
        };
        auto const [tile_min_x, tile_max_x] = clamp_tiles(box_min_x, box_max_x, grid.tiles_x);
        auto const [tile_min_y, tile_max_y] = clamp_tiles(box_min_y, box_max_y, grid.tiles_y);
        result.tile_min_x = tile_min_x;
        result.tile_max_x = tile_max_x;
        result.tile_min_y = tile_min_y;
        result.tile_max_y = tile_max_y;
        result.on_screen = true;
        return result;
    }

    [[nodiscard]] inline projected_record project(meshlet const& record, std::array<float, 16> const& model,
                                                  camera_parameters const& parameters, tile_grid const& grid) noexcept {
        auto const camera = prepare_camera(parameters);
        if (!camera.has_value()) {
            return {};
        }
        return project(record, model, *camera, grid);
    }

    /// one item's input records, with the model matrix that places them (the item's own, as the cut used)
    struct item_records {
        std::uint32_t item_index = 0u;
        std::array<float, 16> model{};
        std::span<meshlet const> records{};
    };

    /// one item's [hardware prefix | software suffix] split; `order_out` in split_records holds the new lane order
    struct item_split {
        std::uint32_t item_index = 0u;
        std::uint32_t record_count = 0u;
        std::uint32_t hardware_count = 0u;
        std::uint32_t software_count = 0u;
        std::uint64_t hardware_triangles = 0u;
        std::uint64_t software_triangles = 0u;
    };

    /**
     * @brief split one item's selected records, stably, into the hardware prefix and the software suffix
     * @param order_out the new lane order: order_out[new_position] = original record index. Reordered, the lane's
     *                  first hardware_count entries are exactly the hardware records in their original order, then the
     *                  software ones; a record at new position hardware_count + k is the packed id's meshlet_in_item
     *                  value for software record k (spec §3.3)
     * @note an item whose record count exceeds the packed id's meshlet field cannot be split - it is reported as ALL
     *       hardware (safe): the frame then dispatches the run it always did
     */
    [[nodiscard]] inline item_split split_records(item_records const& item, camera_metrics const& camera,
                                                  float const threshold_px,
                                                  deren::utility::vessel::vector<std::uint32_t>& order_out) {
        item_split split{};
        split.item_index = item.item_index;
        split.record_count = static_cast<std::uint32_t>(item.records.size());
        order_out.clear();
        order_out.reserve(item.records.size());
        if (item.records.size() > visibility::id_meshlet_capacity) {
            // cannot carry the suffix in the 12-bit field: refuse to move anything (the complete hardware run is drawn)
            for (std::uint32_t index = 0u; index < split.record_count; ++index) {
                order_out.push_back(index);
            }
            split.hardware_count = split.record_count;
            for (meshlet const& record : item.records) {
                split.hardware_triangles += record.index_count / 3u;
            }
            return split;
        }
        std::vector<std::uint8_t> software; // one byte per record: 0 = hardware prefix, 1 = software suffix
        software.resize(item.records.size(), 0u);
        for (std::size_t index = 0u; index < item.records.size(); ++index) {
            auto const classification = classify(item.records[index], item.model, camera, threshold_px);
            if (classification.subpixel) {
                software[index] = 1u;
                ++split.software_count;
                split.software_triangles += item.records[index].index_count / 3u;
            } else {
                ++split.hardware_count;
                split.hardware_triangles += item.records[index].index_count / 3u;
            }
        }
        order_out.clear();
        order_out.reserve(item.records.size());
        for (std::size_t index = 0u; index < item.records.size(); ++index) {
            if (software[index] == 0u) {
                order_out.push_back(static_cast<std::uint32_t>(index));
            }
        }
        for (std::size_t index = 0u; index < item.records.size(); ++index) {
            if (software[index] != 0u) {
                order_out.push_back(static_cast<std::uint32_t>(index));
            }
        }
        return split;
    }

    /// one (item, meshlet) pair filed under a tile; meshlet_id is the run-local index the packed id will carry
    struct tile_entry {
        std::uint32_t item_index = 0u;
        std::uint32_t meshlet_id = 0u;
    };

    /**
     * @brief the CPU tile list: header fields + per-tile exact counts + the fixed-capacity entry table, tile-major
     * @note `counts` is ALWAYS exact (measured without truncation); `entries` holds at most entry_capacity slots per
     *       tile, so it is complete only when !overflow. A production caller must fall back to the whole-frame
     *       hardware path on overflow (spec §5.4); the Task 1 probe uses the exact counts to size the default.
     */
    struct software_tile_plan {
        tile_grid grid{};
        std::uint32_t entry_capacity = tile_entry_capacity_default;
        std::uint32_t item_count = 0u;
        std::uint32_t cluster_count = 0u; ///< total software records filed
        std::uint64_t entry_count = 0u;   ///< sum of counts == cluster_count * covered tiles
        std::uint32_t max_tile_entries = 0u;
        std::uint32_t overflow_tiles = 0u;
        bool overflow = false;
        deren::utility::vessel::vector<item_split> items;
        deren::utility::vessel::vector<std::uint32_t> counts;
        deren::utility::vessel::vector<tile_entry> entries;
    };

    namespace detail {
        /// the one binning pass both entry points share; capacity overflow is REPORTED, never silently dropped
        [[nodiscard]] inline software_tile_plan bin_tile_lists(std::span<item_records const> const items,
                                                               camera_metrics const& camera, float const threshold_px,
                                                               std::uint32_t const entry_capacity) {
            software_tile_plan plan{};
            plan.grid = make_tile_grid(camera.width, camera.height);
            plan.entry_capacity = entry_capacity;
            plan.item_count = static_cast<std::uint32_t>(items.size());
            std::uint32_t const tiles = plan.grid.tile_count();
            plan.counts.assign(tiles, 0u);
            plan.entries.assign(static_cast<std::size_t>(tiles) * entry_capacity, tile_entry{});
            plan.items.reserve(items.size());
            deren::utility::vessel::vector<projected_record> projections;
            deren::utility::vessel::vector<std::uint8_t> software;
            for (item_records const& item : items) {
                item_split split{};
                split.item_index = item.item_index;
                split.record_count = static_cast<std::uint32_t>(item.records.size());
                projections.clear();
                projections.reserve(item.records.size());
                software.clear();
                software.resize(item.records.size(), 0u);
                if (item.records.size() > visibility::id_meshlet_capacity) {
                    // the packed id cannot carry a suffix: keep the item wholly on the hardware path (safe direction)
                    for (meshlet const& record : item.records) {
                        split.hardware_triangles += record.index_count / 3u;
                    }
                    split.hardware_count = split.record_count;
                    plan.items.push_back(split);
                    continue;
                }
                for (std::size_t index = 0u; index < item.records.size(); ++index) {
                    auto const projection = project(item.records[index], item.model, camera, plan.grid);
                    bool const subpixel = projection.finite && projection.front_of_near &&
                                          projection.diameter_upper_px < static_cast<double>(threshold_px);
                    software[index] = subpixel ? 1u : 0u;
                    if (subpixel) {
                        ++split.software_count;
                        split.software_triangles += item.records[index].index_count / 3u;
                    } else {
                        ++split.hardware_count;
                        split.hardware_triangles += item.records[index].index_count / 3u;
                    }
                    projections.push_back(projection);
                }
                plan.cluster_count += split.software_count;
                plan.items.push_back(split);
                // file the software suffix, in suffix order, with the id its lane position will carry
                std::uint32_t suffix = 0u;
                for (std::size_t index = 0u; index < item.records.size(); ++index) {
                    if (software[index] == 0u) {
                        continue;
                    }
                    projected_record const& projection = projections[index];
                    std::uint32_t const meshlet_id = split.hardware_count + suffix;
                    ++suffix;
                    if (!projection.on_screen) {
                        continue; // a subpixel record off the frame covers no tile (still counted/moved)
                    }
                    for (std::int32_t tile_y = projection.tile_min_y; tile_y <= projection.tile_max_y; ++tile_y) {
                        for (std::int32_t tile_x = projection.tile_min_x; tile_x <= projection.tile_max_x; ++tile_x) {
                            std::size_t const tile = static_cast<std::size_t>(tile_y) * plan.grid.tiles_x +
                                                     static_cast<std::size_t>(tile_x);
                            std::uint32_t const slot = plan.counts[tile];
                            ++plan.counts[tile];
                            ++plan.entry_count;
                            if (slot < entry_capacity) {
                                plan.entries[tile * entry_capacity + slot] = tile_entry{.item_index = item.item_index,
                                                                                        .meshlet_id = meshlet_id};
                            }
                        }
                    }
                }
            }
            for (std::uint32_t const count : plan.counts) {
                plan.max_tile_entries = std::max(plan.max_tile_entries, count);
                if (count > entry_capacity) {
                    ++plan.overflow_tiles;
                }
            }
            plan.overflow = plan.overflow_tiles != 0u;
            return plan;
        }
    } // namespace detail

    /**
     * @brief THE MEASUREMENT form (Task 1): bin the frame's subpixel records and report over-capacity tiles EXACTLY
     * @note this entry point exists for the GO/NO-GO workload probe and for the capacity decision (32 vs 64): it must
     *       see the true per-tile maximum even when it exceeds the production capacity, which the strict form refuses.
     *       It is NOT a production path - the probe is capture-only and eval-gated, and the production caller uses
     *       build_tile_lists, where overflow means "whole frame on the hardware path".
     */
    [[nodiscard]] inline software_tile_plan build_tile_lists_measure(std::span<item_records const> const items,
                                                                     camera_parameters const& parameters,
                                                                     float const threshold_px,
                                                                     std::uint32_t const entry_capacity = tile_entry_capacity_default) {
        auto const camera = prepare_camera(parameters);
        if (!camera.has_value()) {
            return software_tile_plan{};
        }
        return detail::bin_tile_lists(items, *camera, threshold_px, std::max(1u, entry_capacity));
    }

    /**
     * @brief THE PRODUCTION form: bin the frame's subpixel records, or refuse the frame
     * @return the plan, or the named reason this frame must run the COMPLETE hardware path
     * @note overflow is an ERROR here rather than a truncated list: a partial tile list would silently drop visible
     *       subpixel clusters (spec §5.4: no partial software image, no half-new frame). Task 1's measurement uses
     *       build_tile_lists_measure, which reports the true over-capacity counts instead.
     */
    [[nodiscard]] inline std::expected<software_tile_plan, software_error> build_tile_lists(
        std::span<item_records const> const items, camera_parameters const& parameters, float const threshold_px,
        std::uint32_t const entry_capacity = tile_entry_capacity_default) {
        if (!std::isfinite(threshold_px) || threshold_px <= 0.0f) {
            return std::unexpected{software_error::invalid_threshold};
        }
        if (entry_capacity == 0u) {
            return std::unexpected{software_error::invalid_capacity};
        }
        auto const camera = prepare_camera(parameters);
        if (!camera.has_value()) {
            return std::unexpected{camera.error()};
        }
        software_tile_plan plan = detail::bin_tile_lists(items, *camera, threshold_px, entry_capacity);
        if (plan.overflow) {
            return std::unexpected{software_error::tile_overflow};
        }
        return plan;
    }
} // namespace deren::engine::visibility::software
