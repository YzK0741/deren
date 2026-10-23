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
#include <bit>
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
    /// @note MEASUREMENT ONLY. The user approved the CSR sparse list (spec §3.4/§9 Q7): the production path has NO
    ///       per-tile capacity at all - it shares ONE global entry budget (`software_entry_budget`) between tiles via
    ///       a prefix sum. This constant survives only because the committed Task-1 capture probe
    ///       (`build_tile_lists_measure`, called from runtime.cpp outside this task's scope) reports the per-tile
    ///       maximum/overflow it measured, and that report is the evidence the CSR decision rests on.
    inline constexpr std::uint32_t tile_entry_capacity_default = 32u;
    /// THE PRODUCTION TILE-LIST CAPACITY (spec §3.4/§9 Q7, user-approved 2026-10-22): ONE budget of CSR entries per
    /// frame lane, shared by every tile through the prefix-sum offsets. Task 1 measured the real load at far 4,572
    /// (1.0 px) / 12,557 (2.0 px) entries, so 65,536 leaves ~5-14x headroom; exceeding it refuses the whole frame
    /// (hardware-only, spec §5.4) rather than truncating. The buffer's entries array is preallocated at exactly this
    /// many 8-byte entries (512 KiB/lane at 720p: spec §6.1) and the CSR builder is the only thing that reads it.
    inline constexpr std::uint32_t software_entry_budget = 65'536u;
    /// the entry's byte size on the GPU buffer (`{item_index:u32, meshlet_id:u32}`), the number the CSR buffer
    /// allocation is derived from (T4 allocates `header + counts + offsets + budget * this`)
    inline constexpr std::uint32_t tile_entry_bytes = 8u;
    // ---- THE CSR TILE-LIST BUFFER'S BYTE LAYOUT (spec §3.4/§9 Q7): the single source for the host writer (Task 4),
    //      the shader walk (Task 3) and the buffer's creation size (their consumers). The order is:
    //      header[software_tiles_header_words] {tiles_x, tiles_y, entry_count, overflow, cluster_count, active_tile_count},
    //      counts[tiles], offsets[tiles + 1] (exclusive prefix sum), active_tiles[active_tile_count], entries[entry_count] of 8 bytes
    //      {item_index, meshlet_id}. Counts/offsets are uint32 words; entries are counted in tile_entry units.
    //      NOTHING HERE IS A PER-TILE CAPACITY: the list is CSR, so only the global budget bounds a frame. ----
    inline constexpr std::uint32_t software_tiles_word_bytes = 4u;
    inline constexpr std::uint32_t software_tiles_header_words = 6u;
    inline constexpr std::uint32_t software_tiles_entry_bytes = tile_entry_bytes;
    /// the index of each header word, so the writer and the shader cannot disagree about the order
    enum class software_tiles_header_word : std::uint32_t {
        tiles_x = 0u,
        tiles_y = 1u,
        entry_count = 2u,
        overflow = 3u,
        cluster_count = 4u,
        active_tile_count = 5u,
    };
    /// the first counts[] word (header words precede it; 	iles is accepted so every accessor has the same shape)
    [[nodiscard]] constexpr std::uint32_t software_tiles_counts_word([[maybe_unused]] std::uint32_t const tiles) noexcept {
        return software_tiles_header_words;
    }
    /// the first offsets[] word (counts[] is one word per tile)
    [[nodiscard]] constexpr std::uint32_t software_tiles_offsets_word(std::uint32_t const tiles) noexcept {
        return software_tiles_counts_word(tiles) + tiles;
    }
    /// the first active-tile index (capacity is one uint per tile, actual count is in the header)
    [[nodiscard]] constexpr std::uint32_t software_tiles_active_tiles_word(std::uint32_t const tiles) noexcept {
        return software_tiles_offsets_word(tiles) + tiles + 1u;
    }
    /// the first entries[] slot (offsets[] is one word per tile PLUS the sentinel offset[tiles], then tile indices)
    [[nodiscard]] constexpr std::uint32_t software_tiles_entries_word(std::uint32_t const tiles) noexcept {
        return software_tiles_active_tiles_word(tiles) + tiles;
    }
    /// the BYTES a software_tiles buffer needs for an extent and a global entry budget (the creation size)
    [[nodiscard]] constexpr std::uint64_t software_tiles_bytes(std::uint32_t const width, std::uint32_t const height,
                                                               std::uint32_t const entry_budget = software_entry_budget) noexcept {
        // tile arithmetic inline: tile_size is declared above, so the layout constants stay together at the top of
        // the module (the tile_grid type itself lives further down, beside the binning it serves)
        std::uint32_t const tiles_x = (width + tile_size - 1u) / tile_size;
        std::uint32_t const tiles_y = (height + tile_size - 1u) / tile_size;
        std::uint32_t const tiles = tiles_x * tiles_y;
        std::uint64_t const words = static_cast<std::uint64_t>(software_tiles_entries_word(tiles));
        return words * software_tiles_word_bytes + static_cast<std::uint64_t>(entry_budget) * software_tiles_entry_bytes;
    }

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
        /// APPENDED FOR THE CSR PRODUCTION PATH (spec §3.4/§9 Q7): the frame's total CSR entries exceed
        /// `software_entry_budget`. There is NO per-tile failure any more: the API's whole point is that one tile may
        /// take thousands of entries, and the budget is the only capacity that exists. The caller falls the whole
        /// frame back to the hardware path (spec §5.4) - never truncates.
        entry_budget_exceeded,
        /// the CPU reference rasterizer was handed a plan entry that names an item/meshlet/vertex outside the
        /// caller-supplied expanded geometry (a fixture-wiring bug, not a GPU-possible state)
        invalid_reference,
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
        double model_scale = 0.0;
        double world_radius = 0.0; ///< record.radius * ||linear 3x3||_F
        double view_depth = 0.0;   ///< w = -(view * world_center).z
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
        measurement.model_scale = scale;
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

    enum class routing_metric { cluster_diameter,
                                triangle_edge };

    /// one record's conservative screen-space box and the inclusive tile range it covers (empty when off-screen)
    struct projected_record {
        bool finite = false;
        bool front_of_near = false;
        bool on_screen = false;
        double diameter_upper_px = std::numeric_limits<double>::infinity();
        double triangle_edge_upper_px = std::numeric_limits<double>::infinity();
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
                                                  camera_metrics const& camera, tile_grid const& grid,
                                                  double const object_max_edge = std::numeric_limits<double>::infinity()) noexcept {
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
        // Bound the perspective projection Jacobian over the entire sphere. Off-axis Z variation
        // expands screen-space edges AND the sphere footprint; center-only scale can falsely cull
        // a sphere whose center is offscreen but whose enclosed triangles cross the image boundary.
        double projection_bound = camera.proj_scale * 0.5;
        if (camera.kind == projection_kind::perspective) {
            double const nearest = measurement.view_depth - measurement.world_radius;
            if (nearest > 0.0) {
                double const off_axis = (std::hypot(view_center[0], view_center[1]) + measurement.world_radius) / nearest;
                projection_bound = projection_bound / nearest * std::sqrt(1.0 + off_axis * off_axis);
            } else
                projection_bound = std::numeric_limits<double>::infinity();
        }
        if (measurement.front_of_near && std::isfinite(object_max_edge) && object_max_edge >= 0.0)
            result.triangle_edge_upper_px = std::nextafter(object_max_edge * measurement.model_scale * projection_bound,
                                                           std::numeric_limits<double>::infinity());
        double const center_x = (ndc_x * 0.5 + 0.5) * static_cast<double>(camera.width);
        double const center_y = (ndc_y * 0.5 + 0.5) * static_cast<double>(camera.height);
        double const radius_px = measurement.front_of_near ? std::max<double>(minimum_screen_radius_px,
                                                                              std::nextafter(measurement.world_radius * projection_bound, std::numeric_limits<double>::infinity()))
                                                           : std::numeric_limits<double>::infinity();
        if (!std::isfinite(radius_px) || !std::isfinite(center_x) || !std::isfinite(center_y))
            return result;
        result.center_px = {static_cast<float>(center_x), static_cast<float>(center_y)};
        // Round outward when storing the double bound in the float footprint field.
        result.radius_px = std::nextafter(static_cast<float>(radius_px), std::numeric_limits<float>::infinity());
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
        // Optional transient cache for these exact records/model/camera; never reused across frames.
        std::span<projected_record const> cached_projections{};
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
                                                  deren::utility::vessel::vector<std::uint32_t>& order_out,
                                                  routing_metric metric = routing_metric::cluster_diameter) {
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
            bool subpixel;
            if (item.cached_projections.size() == item.records.size()) {
                auto const& p = item.cached_projections[index];
                subpixel = p.finite && p.front_of_near &&
                           (metric == routing_metric::triangle_edge ? p.triangle_edge_upper_px : p.diameter_upper_px) < static_cast<double>(threshold_px);
            } else {
                subpixel = metric == routing_metric::cluster_diameter && classify(item.records[index], item.model, camera, threshold_px).subpixel;
            }
            if (subpixel) {
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

    // ================================================================================================
    // THE PRODUCTION TILE LIST: CSR (spec §3.4/§9 Q7, user-approved 2026-10-22)
    //
    // Task 1's measurement (see build_tile_lists_measure above) FALSIFIED the fixed per-tile capacity: far's
    // densest tile held 977 entries at 1.0 px and 1,907 at 2.0 px while the frame's whole total was only 4,572 /
    // 12,557 entries over 3,600 tiles - a long-tailed distribution where any fixed per-tile cap either overflows
    // or wastes tens of MiB. The approved replacement is this CSR (compressed sparse row) list: every tile owns a
    // RANGE of ONE shared entries array, located by an exclusive prefix sum of the per-tile counts. There is NO
    // per-tile capacity and NO per-tile overflow anywhere on the production path; the only capacity is the global
    // `software_entry_budget`, and exceeding it refuses the WHOLE frame (never truncates: spec §5.4's "no partial
    // software image, no half-new frame"). The GPU software raster reads exactly `offsets[t] .. offsets[t+1]` for
    // its tile, so a sparse tile is a zero-length range rather than a gap in a fixed stride.
    //
    // THE ORDER IS A CONTRACT: entries are appended in ITEM order -> RUN order -> TILE order (the same order the
    // classification walks), so the list is byte-identical for identical inputs. That determinism is what lets
    // the CPU reference and the GPU probe compare a captured list entry by entry (Task 3) and what makes the
    // `(depth, id)` total order's independence from list order a property rather than a hope.
    // ================================================================================================
    struct software_tile_lists {
        tile_grid grid{};
        std::uint32_t entry_budget = software_entry_budget;    ///< the global cap `entry_count` was checked against
        std::uint32_t item_count = 0u;                         ///< items the builder was given
        std::uint32_t cluster_count = 0u;                      ///< software (subpixel) records filed into the list
        std::uint32_t entry_count = 0u;                        ///< CSR entries actually used (== offsets[tiles])
        std::uint32_t max_tile_entries = 0u;                   ///< DIAGNOSTICS only: the busiest tile's count
        bool overflow = false;                                 ///< measurement-only form; the production builder refuses instead
        deren::utility::vessel::vector<item_split> items;      ///< per item: the [hardware prefix | software suffix] split
        deren::utility::vessel::vector<std::uint32_t> counts;  ///< per tile: exact entry count (== offsets[t+1] - offsets[t])
        deren::utility::vessel::vector<std::uint32_t> offsets; ///< tiles + 1 exclusive prefix sums; offsets[0] == 0
        deren::utility::vessel::vector<tile_entry> entries;    ///< entry_count entries, grouped by tile, in the order above

        /// @brief the CSR range of tile (@p x, @p y), as a span of its entries (empty for a sparse tile)
        [[nodiscard]] inline std::span<tile_entry const> tile_span(std::uint32_t const x, std::uint32_t const y) const noexcept {
            if (x >= this->grid.tiles_x || y >= this->grid.tiles_y || this->offsets.size() != static_cast<std::size_t>(this->grid.tile_count()) + 1u) {
                return {};
            }
            std::size_t const tile = static_cast<std::size_t>(y) * this->grid.tiles_x + x;
            std::uint32_t const begin = this->offsets[tile];
            std::uint32_t const end = this->offsets[tile + 1u];
            if (begin > end || end > this->entries.size()) {
                return {};
            }
            return std::span<tile_entry const>(this->entries.data() + begin, end - begin);
        }

        [[nodiscard]] bool empty() const noexcept {
            return this->cluster_count == 0u || this->entry_count == 0u;
        }
    };

    /// @brief serialize one frame's CSR into the exact uint-word layout consumed by software_visibility.slang
    /// @return false for malformed lists or an undersized lane; no partial write occurs on failure
    [[nodiscard]] inline bool write_tile_lists(std::span<std::uint32_t> lane, software_tile_lists const& lists) noexcept {
        std::uint32_t const tiles = lists.grid.tile_count();
        std::uint64_t const needed_words = static_cast<std::uint64_t>(software_tiles_entries_word(tiles)) +
                                           static_cast<std::uint64_t>(lists.entry_budget) * (software_tiles_entry_bytes / sizeof(std::uint32_t));
        if (lists.overflow || lists.entry_count > lists.entry_budget || lists.entries.size() != lists.entry_count ||
            lists.counts.size() != tiles || lists.offsets.size() != static_cast<std::size_t>(tiles) + 1u || lists.offsets.empty() ||
            lists.offsets.front() != 0u || lists.offsets.back() != lists.entry_count || lane.size() < needed_words) {
            return false;
        }
        for (std::uint32_t tile = 0u; tile < tiles; ++tile) {
            if (lists.offsets[tile] > lists.offsets[tile + 1u] || lists.offsets[tile + 1u] > lists.entry_count ||
                lists.counts[tile] != lists.offsets[tile + 1u] - lists.offsets[tile]) {
                return false;
            }
        }
        std::fill(lane.begin(), lane.end(), 0u);
        lane[0] = lists.grid.tiles_x;
        lane[1] = lists.grid.tiles_y;
        lane[2] = lists.entry_count;
        lane[3] = 0u;
        lane[4] = lists.cluster_count;
        std::uint32_t active_tile_count = 0u;
        for (std::uint32_t tile = 0u; tile < tiles; ++tile) {
            if (lists.counts[tile] != 0u) {
                lane[software_tiles_active_tiles_word(tiles) + active_tile_count] = tile;
                ++active_tile_count;
            }
        }
        lane[5] = active_tile_count;
        std::copy(lists.counts.begin(), lists.counts.end(), lane.begin() + software_tiles_counts_word(tiles));
        std::copy(lists.offsets.begin(), lists.offsets.end(), lane.begin() + software_tiles_offsets_word(tiles));
        std::uint32_t const entries_word = software_tiles_entries_word(tiles);
        for (std::uint32_t index = 0u; index < lists.entry_count; ++index) {
            lane[entries_word + index * 2u] = lists.entries[index].item_index;
            lane[entries_word + index * 2u + 1u] = lists.entries[index].meshlet_id;
        }
        return true;
    }

    /// Packed ordering for normal-Z atomic min: depth and ID are one indivisible candidate.
    [[nodiscard]] constexpr std::uint64_t atomic_candidate(std::uint32_t depth, std::uint32_t id) noexcept {
        return (static_cast<std::uint64_t>(depth) << 32u) | id;
    }

    [[nodiscard]] inline std::expected<deren::utility::vessel::vector<tile_entry>, software_error>
    triangle_dispatch_entries(software_tile_lists const& lists) {
        if (lists.overflow || lists.entry_count != lists.entries.size() || lists.entry_count > software_entry_budget)
            return std::unexpected(software_error::invalid_reference);
        auto entries = lists.entries;
        auto less = [](tile_entry const& a, tile_entry const& b) {
            return a.item_index < b.item_index || (a.item_index == b.item_index && a.meshlet_id < b.meshlet_id);
        };
        std::sort(entries.begin(), entries.end(), less);
        entries.erase(std::unique(entries.begin(), entries.end(), [](tile_entry const& a, tile_entry const& b) {
                          return a.item_index == b.item_index && a.meshlet_id == b.meshlet_id;
                      }),
                      entries.end());
        return entries;
    }

    /// Number of non-empty CSR tiles dispatched by the sparse software raster pass.
    [[nodiscard]] inline std::uint32_t active_tile_count(software_tile_lists const& lists) noexcept {
        return static_cast<std::uint32_t>(std::count_if(lists.counts.begin(), lists.counts.end(), [](std::uint32_t const count) { return count != 0u; }));
    }

    struct gpu_partition_item {
        std::uint32_t item_index = 0u;
        std::span<meshlet const> candidates;
        std::span<std::uint32_t const> flags;
        std::span<meshlet const> hardware;
        std::uint32_t primary_count = 0u, recovery_count = 0u;
    };
    /// Capture-only independent coverage oracle. HW must equal the stable primary/recovery survivors
    /// after subtracting actual SW IDs; duplicates, missing candidates and temporary flags are refused.
    [[nodiscard]] inline bool validate_gpu_partition(std::span<tile_entry const> software, std::span<gpu_partition_item const> items) {
        std::array<int, visibility::id_item_capacity> lookup{};
        lookup.fill(-1);
        deren::utility::vessel::vector<deren::utility::vessel::vector<std::uint8_t>> masks(items.size());
        for (std::size_t i = 0u; i < items.size(); ++i) {
            auto const& item = items[i];
            if (item.item_index >= lookup.size() || lookup[item.item_index] >= 0 || item.candidates.size() > visibility::id_meshlet_capacity ||
                item.flags.size() != item.candidates.size() || item.primary_count > item.candidates.size() ||
                item.recovery_count > item.candidates.size() - item.primary_count ||
                item.hardware.size() < item.primary_count + item.recovery_count)
                return false;
            for (auto flag : item.flags)
                if (flag != 0u && flag != 1u && flag != 3u)
                    return false;
            lookup[item.item_index] = static_cast<int>(i);
            masks[i].resize(item.candidates.size(), 0u);
        }
        for (auto const& entry : software) {
            if (entry.item_index >= lookup.size() || lookup[entry.item_index] < 0)
                return false;
            auto const i = static_cast<std::size_t>(lookup[entry.item_index]);
            auto const& item = items[i];
            if (entry.meshlet_id >= item.candidates.size() || masks[i][entry.meshlet_id] || item.flags[entry.meshlet_id] == 1u)
                return false;
            masks[i][entry.meshlet_id] = 1u;
        }
        for (std::size_t i = 0u; i < items.size(); ++i) {
            auto const& item = items[i];
            for (std::uint32_t phase : {0u, 3u}) {
                auto const offset = phase == 0u ? 0u : item.primary_count;
                auto const expected = phase == 0u ? item.primary_count : item.recovery_count;
                std::uint32_t count = 0u;
                for (std::size_t c = 0u; c < item.candidates.size(); ++c) {
                    if (item.flags[c] != phase || masks[i][c])
                        continue;
                    if (count >= expected || std::bit_cast<std::array<std::uint32_t, 12>>(item.hardware[offset + count]) !=
                                                 std::bit_cast<std::array<std::uint32_t, 12>>(item.candidates[c]))
                        return false;
                    ++count;
                }
                if (count != expected)
                    return false;
            }
        }
        return true;
    }

    /// Append one already-classified item exactly once. IDs are suffix POSITIONS, including offscreen gaps.
    /// All validation/budget checks precede mutation; caller restores full hardware on false.
    [[nodiscard]] inline bool append_triangle_dispatch(item_split const& split, std::span<std::uint32_t const> order,
                                                       std::span<projected_record const> projections, deren::utility::vessel::vector<tile_entry>& output,
                                                       std::uint32_t budget = software_entry_budget) {
        if (budget == 0u || budget > software_entry_budget || output.size() > budget ||
            split.hardware_count > split.record_count || split.software_count != split.record_count - split.hardware_count)
            return false;
        if (split.software_count == 0u)
            return true; // includes oversized all-hardware runs
        if (split.item_index >= visibility::id_item_capacity || split.record_count > visibility::id_meshlet_capacity ||
            order.size() != split.record_count || projections.size() != split.record_count)
            return false;
        std::array<std::uint64_t, visibility::id_meshlet_capacity / 64u> seen{};
        std::uint32_t needed = 0u;
        for (std::uint32_t position = 0u; position < split.record_count; ++position) {
            auto const original = order[position];
            if (original >= split.record_count)
                return false;
            auto const mask = std::uint64_t{1u} << (original % 64u);
            if ((seen[original / 64u] & mask) != 0u)
                return false;
            seen[original / 64u] |= mask;
            if (position >= split.hardware_count) {
                auto const& projection = projections[original];
                if (!projection.finite || !projection.front_of_near)
                    return false;
                if (projection.on_screen)
                    ++needed;
            }
        }
        if (needed > budget - output.size())
            return false;
        output.reserve(output.size() + needed);
        for (std::uint32_t position = split.hardware_count; position < split.record_count; ++position)
            if (projections[order[position]].on_screen)
                output.push_back({.item_index = split.item_index, .meshlet_id = position});
        return true;
    }

    /// Triangle mode keeps the existing frame stride/entry offset but never reads CSR padding.
    /// Only header and used entries are written; unused counts/offsets/entries need no host clearing.
    [[nodiscard]] inline bool write_triangle_dispatch(std::span<std::uint32_t> lane, software_tile_lists const& list) noexcept {
        auto const offset = software_tiles_entries_word(list.grid.tile_count());
        if (list.overflow || list.entry_budget == 0u || list.entry_budget > software_entry_budget ||
            list.entry_count != list.entries.size() || list.entry_count > list.entry_budget ||
            static_cast<std::uint64_t>(offset) + list.entry_count * 2ull > lane.size())
            return false;
        for (auto const& entry : list.entries)
            if (entry.item_index >= visibility::id_item_capacity || entry.meshlet_id >= visibility::id_meshlet_capacity)
                return false;
        lane[0] = list.grid.tiles_x;
        lane[1] = list.grid.tiles_y;
        lane[2] = list.entry_count;
        lane[3] = 0u;
        lane[4] = list.cluster_count;
        lane[5] = list.entry_count;
        for (std::uint32_t i = 0u; i < list.entry_count; ++i) {
            lane[offset + i * 2u] = list.entries[i].item_index;
            lane[offset + i * 2u + 1u] = list.entries[i].meshlet_id;
        }
        return true;
    }

    namespace detail {
        /// the CSR producer's one binning body: two passes over deterministic order, prefix sum between them
        [[nodiscard]] inline std::expected<software_tile_lists, software_error> bin_csr_tile_lists(
            std::span<item_records const> const items, camera_metrics const& camera, float const threshold_px,
            std::uint32_t const entry_budget, routing_metric metric) {
            software_tile_lists lists{};
            lists.grid = make_tile_grid(camera.width, camera.height);
            lists.entry_budget = entry_budget;
            lists.item_count = static_cast<std::uint32_t>(items.size());
            std::uint32_t const tiles = lists.grid.tile_count();
            lists.counts.assign(tiles, 0u);
            lists.items.reserve(items.size());
            // what the second pass needs per item: its split, which original indices are software records, and each
            // software record's conservative tile box (computed once, in the first pass, from the same projection)
            struct filed_item {
                item_split split{};
                deren::utility::vessel::vector<std::uint8_t> software;
                deren::utility::vessel::vector<projected_record> owned_projections;
                std::span<projected_record const> projections; // indexed by ORIGINAL record index
            };
            deren::utility::vessel::vector<filed_item> filed;
            filed.reserve(items.size());
            std::uint64_t total_entries = 0u;
            for (item_records const& item : items) {
                filed_item entry_set{};
                if (!item.cached_projections.empty() && item.cached_projections.size() != item.records.size()) {
                    return std::unexpected{software_error::invalid_reference};
                }
                item_split& split = entry_set.split;
                split.item_index = item.item_index;
                split.record_count = static_cast<std::uint32_t>(item.records.size());
                entry_set.software.assign(item.records.size(), 0u);
                if (item.cached_projections.empty()) {
                    entry_set.owned_projections.resize(item.records.size());
                    entry_set.projections = entry_set.owned_projections;
                } else {
                    entry_set.projections = item.cached_projections;
                }
                if (item.records.size() > visibility::id_meshlet_capacity) {
                    // the packed id cannot carry a suffix: the item stays wholly on the hardware path (safe direction)
                    for (meshlet const& record : item.records) {
                        split.hardware_triangles += record.index_count / 3u;
                    }
                    split.hardware_count = split.record_count;
                    lists.items.push_back(split);
                    filed.push_back(std::move(entry_set));
                    continue;
                }
                for (std::size_t index = 0u; index < item.records.size(); ++index) {
                    auto const projection = item.cached_projections.empty()
                                                ? project(item.records[index], item.model, camera, lists.grid)
                                                : item.cached_projections[index];
                    bool const subpixel = projection.finite && projection.front_of_near &&
                                          (metric == routing_metric::triangle_edge ? projection.triangle_edge_upper_px : projection.diameter_upper_px) < static_cast<double>(threshold_px);
                    entry_set.software[index] = subpixel ? 1u : 0u;
                    if (item.cached_projections.empty())
                        entry_set.owned_projections[index] = projection;
                    if (subpixel) {
                        ++split.software_count;
                        split.software_triangles += item.records[index].index_count / 3u;
                    } else {
                        ++split.hardware_count;
                        split.hardware_triangles += item.records[index].index_count / 3u;
                    }
                }
                lists.cluster_count += split.software_count;
                // PASS 1: exact per-tile counts (no capacity to check against - the only check is the global budget)
                for (std::size_t index = 0u; index < item.records.size(); ++index) {
                    if (entry_set.software[index] == 0u) {
                        continue;
                    }
                    projected_record const& projection = entry_set.projections[index];
                    if (!projection.on_screen) {
                        continue; // a subpixel record off the frame covers no tile (still counted/moved)
                    }
                    for (std::int32_t tile_y = projection.tile_min_y; tile_y <= projection.tile_max_y; ++tile_y) {
                        for (std::int32_t tile_x = projection.tile_min_x; tile_x <= projection.tile_max_x; ++tile_x) {
                            std::size_t const tile = static_cast<std::size_t>(tile_y) * lists.grid.tiles_x + static_cast<std::size_t>(tile_x);
                            ++lists.counts[tile];
                            ++total_entries;
                        }
                    }
                }
                if (total_entries > entry_budget) {
                    // GLOBAL budget: one capacity, one failure mode (spec §5.4). The caller runs the complete
                    // hardware path; no partial CSR list is ever produced.
                    return std::unexpected{software_error::entry_budget_exceeded};
                }
                lists.items.push_back(split);
                filed.push_back(std::move(entry_set));
            }
            // ---- the exclusive prefix sum: offsets[t] is tile t's first entry, offsets[tiles] is entry_count ----
            lists.offsets.assign(static_cast<std::size_t>(tiles) + 1u, 0u);
            for (std::uint32_t tile = 0u; tile < tiles; ++tile) {
                lists.offsets[tile + 1u] = lists.offsets[tile] + lists.counts[tile];
            }
            lists.entry_count = lists.offsets[tiles];
            lists.entries.assign(lists.entry_count, tile_entry{});
            // ---- PASS 2: fill tile ranges in item -> run -> tile order ----
            deren::utility::vessel::vector<std::uint32_t> cursor = lists.offsets; // per-tile write head
            for (std::size_t item_position = 0u; item_position < filed.size(); ++item_position) {
                filed_item const& entry_set = filed[item_position];
                item_split const& split = entry_set.split;
                std::uint32_t suffix = 0u;
                for (std::size_t index = 0u; index < entry_set.software.size(); ++index) {
                    if (entry_set.software[index] == 0u) {
                        continue;
                    }
                    std::uint32_t const meshlet_id = split.hardware_count + suffix;
                    ++suffix;
                    projected_record const& projection = entry_set.projections[index];
                    if (!projection.on_screen) {
                        continue;
                    }
                    for (std::int32_t tile_y = projection.tile_min_y; tile_y <= projection.tile_max_y; ++tile_y) {
                        for (std::int32_t tile_x = projection.tile_min_x; tile_x <= projection.tile_max_x; ++tile_x) {
                            std::size_t const tile = static_cast<std::size_t>(tile_y) * lists.grid.tiles_x + static_cast<std::size_t>(tile_x);
                            lists.entries[cursor[tile]++] = tile_entry{.item_index = split.item_index, .meshlet_id = meshlet_id};
                        }
                    }
                }
            }
            for (std::uint32_t const count : lists.counts) {
                lists.max_tile_entries = std::max(lists.max_tile_entries, count);
            }
            return lists;
        }
    } // namespace detail

    /**
     * @brief THE PRODUCTION TILE LIST BUILDER: bin the frame's subpixel records into the CSR sparse list, or refuse
     *        the frame
     * @param items the frame's classified items (the same span the split uses)
     * @param parameters the frame's camera (view/proj/extent)
     * @param threshold_px the subpixel threshold (1.0 is the primary band)
     * @param entry_budget the global CSR entry budget; the single capacity of this design (`software_entry_budget`)
     * @return the CSR plan, or the named reason this frame must run the COMPLETE hardware path
     * @note `entry_budget_exceeded` is an ERROR rather than a truncated list: a partial list would silently drop
     *       visible subpixel clusters (spec §5.4). A zero budget is refused as invalid; every overflow decision is
     *       deterministic because the two passes walk the same item -> run -> tile order.
     */
    [[nodiscard]] inline std::expected<software_tile_lists, software_error> build_tile_lists(
        std::span<item_records const> const items, camera_parameters const& parameters, float const threshold_px,
        std::uint32_t const entry_budget = software_entry_budget, routing_metric metric = routing_metric::cluster_diameter) {
        if (!std::isfinite(threshold_px) || threshold_px <= 0.0f) {
            return std::unexpected{software_error::invalid_threshold};
        }
        if (entry_budget == 0u) {
            return std::unexpected{software_error::invalid_capacity};
        }
        auto const camera = prepare_camera(parameters);
        if (!camera.has_value()) {
            return std::unexpected{camera.error()};
        }
        return detail::bin_csr_tile_lists(items, *camera, threshold_px, entry_budget, metric);
    }

    // ================================================================================================
    // THE RESOLVE/MERGE CONTRACT (spec §3.2): one software pixel, the winner rule BEFORE the merge, and the
    // merged depth the merge pass leaves behind
    //
    // THE ORDER IS THE CORRECTNESS ARGUMENT. The resolve runs on the UNMERGED hardware depth. Its winner rule is
    //   software wins iff sw_depth < hw_depth                       (strictly nearer)
    //                  or sw_depth == hw_depth and !hw_id.valid()   (the hardware pixel is background/clear)
    // and it uses the WINNING candidate's own depth for the reconstruction/tolerance check, so a software pixel's
    // z-check compares the software id's reconstructed z against the software raster's own depth. If the merge ran
    // FIRST, software-won pixels would have their depth overwritten by sw_depth and the strict `<` could no longer
    // see the old hardware depth (the design review's blocking defect, spec §3.2/§5.1). The merge is a depth-only
    // pass with LESS_OR_EQUAL + depth write whose final value is therefore min(hw_depth, sw_depth).
    //
    // COMPARISON IS EXACT (no epsilon), per spec: the two producers can disagree by one ULP at a nominally equal
    // depth, which the strict `<` resolves deterministically; the merge writes the smaller bit pattern, and for
    // positive floats the bit order IS the float order.
    // ================================================================================================
    /// the software images' "no software geometry wrote this pixel" values: depth = a bit pattern larger than any
    /// legal positive float (so integer min selection keeps it last), id = the cleared id (0)
    inline constexpr std::uint32_t software_depth_empty = 0xFFFFFFFFu;
    inline constexpr std::uint32_t software_id_empty = 0u;

    /// one software pixel: NDC depth as an INTEGER (float bit pattern) + the packed 32-bit visibility id
    struct software_pixel {
        std::uint32_t depth_bits = software_depth_empty;
        std::uint32_t id = software_id_empty;
    };

    /// @brief whether a software pixel names geometry with a usable depth (empty id / non-finite / out-of-range fail)
    /// @note spec §5.4: a non-finite or out-of-[0,1] software depth is treated as EMPTY (skipped) - the design never
    ///       writes a NaN into the depth attachment
    [[nodiscard]] inline bool software_pixel_usable(software_pixel const& pixel) noexcept {
        if (pixel.id == software_id_empty) {
            return false;
        }
        float const depth = std::bit_cast<float>(pixel.depth_bits);
        return std::isfinite(depth) && depth >= 0.0f && depth <= 1.0f;
    }

    enum class candidate_source : std::uint8_t { hardware,
                                                 software };

    /// the resolve's choice for one pixel: which producer's id it decodes and which depth it reconstructs with
    struct resolved_candidate {
        candidate_source source = candidate_source::hardware;
        std::uint32_t id = 0u; ///< the packed id to decode (hardware's or software's)
        float depth = 0.0f;    ///< the depth the reconstruction/tolerance check must use (the WINNER's own depth)
    };

    /**
     * @brief THE WINNER RULE (spec §3.2), evaluated BEFORE the merge on the unmerged hardware depth
     * @param hardware_id the hardware visibility id (its valid bit distinguishes "background" from "geometry")
     * @param hardware_depth the unmerged G-buffer depth the hardware raster wrote for this pixel
     * @param software the software producer's pixel (id + depth bits)
     * @note software wins on a STRICTLY smaller depth, or on an exact tie when the hardware pixel is background
     *       (invalid id). Every other case - including a software pixel that is empty, non-finite or out of [0,1] -
     *       keeps hardware. The returned depth is the chosen candidate's own depth (spec §5.3).
     */
    [[nodiscard]] inline resolved_candidate resolve_candidate(visibility::id const hardware_id, float const hardware_depth,
                                                              software_pixel const software) noexcept {
        resolved_candidate chosen{.source = candidate_source::hardware, .id = hardware_id.value, .depth = hardware_depth};
        if (!software_pixel_usable(software)) {
            return chosen;
        }
        float const software_depth = std::bit_cast<float>(software.depth_bits);
        bool const strictly_nearer = software_depth < hardware_depth;
        bool const background_tie = software_depth == hardware_depth && !hardware_id.valid();
        if (strictly_nearer || background_tie) {
            chosen = resolved_candidate{.source = candidate_source::software, .id = software.id, .depth = software_depth};
        }
        return chosen;
    }

    /**
     * @brief the depth the MERGE pass leaves in the depth attachment: `min(hardware, software)`
     * @param hardware_depth_bits the depth attachment's current value (the hardware producer's, bit pattern)
     * @param software the software pixel
     * @note the merge pipeline is LESS_OR_EQUAL + depth write on `clamp(asfloat(sw_depth_bits),0,1)`, so it writes
     *       only when `sw_depth <= hw_depth`; the resulting value is `min`. Equal writes the same value (the spec's
     *       "tie writes same value, no change"); for positive floats the numeric min is the bit-pattern min, which is
     *       what makes this CPU reference exact.
     */
    [[nodiscard]] inline std::uint32_t merged_depth_bits(std::uint32_t const hardware_depth_bits, software_pixel const software) noexcept {
        if (!software_pixel_usable(software)) {
            return hardware_depth_bits;
        }
        return software.depth_bits < hardware_depth_bits ? software.depth_bits : hardware_depth_bits;
    }

    // ================================================================================================
    // THE CPU REFERENCE RASTERIZER (spec §4.1): fixtures' expected software images
    //
    // It consumes the SAME "already expanded" data the GPU pass fetches through device addresses - per item its
    // run table (hardware prefix then software suffix), index buffer, vertex positions and model - so it does NOT
    // redo the import/loader. The arithmetic is the shader's: world = model * position (the renderer's column-major
    // storage convention, the same one visibility.cppm's resolve reference uses), clip = proj * view * world, the
    // pixel footprint from `visibility::clip_to_screen`/`screen_barycentrics`, and screen-linear z
    // `sum(bary_i * clip_i.z/clip_i.w)` (the hardware raster's and resolve's depth convention). Every pixel gets
    // the total order `(depth_bits, id)` minimum, so the result does not depend on tile/entry scan order.
    // ================================================================================================
    namespace detail {
        /// column-major storage: element (row, column) lives at matrix[column * 4 + row]; matches glm::mat4's bytes
        [[nodiscard]] inline std::array<double, 4> reference_transform(std::array<float, 16> const& matrix,
                                                                       std::array<double, 4> const& vector) noexcept {
            std::array<double, 4> result{};
            for (std::uint32_t row = 0u; row < 4u; ++row) {
                double value = 0.0;
                for (std::uint32_t column = 0u; column < 4u; ++column) {
                    value += static_cast<double>(matrix[column * 4u + row]) * vector[column];
                }
                result[row] = value;
            }
            return result;
        }

        /// @brief `a * b` in the same column-vector sense the shader's `mul(vec, a)` then `* b` computes
        [[nodiscard]] inline std::array<float, 16> reference_multiply(std::array<float, 16> const& a, std::array<float, 16> const& b) noexcept {
            std::array<float, 16> result{};
            for (std::uint32_t row = 0u; row < 4u; ++row) {
                for (std::uint32_t column = 0u; column < 4u; ++column) {
                    double sum = 0.0;
                    for (std::uint32_t inner = 0u; inner < 4u; ++inner) {
                        sum += static_cast<double>(a[inner * 4u + row]) * static_cast<double>(b[column * 4u + inner]);
                    }
                    result[column * 4u + row] = static_cast<float>(sum);
                }
            }
            return result;
        }
    } // namespace detail

    /// one item's expanded geometry: exactly the facts the GPU software raster fetches through the draw-item row
    struct reference_item {
        std::uint32_t item_index = 0u;
        std::array<float, 16> model{};                     ///< draw_item.model (column-major storage, applied as M*v)
        std::int32_t base_vertex = 0;                      ///< draw_item.base_vertex, added to every fetched index
        std::span<meshlet const> records{};                ///< the item's run: hardware prefix, then software suffix
        std::span<std::array<float, 3> const> positions{}; ///< item-local positions (the 64-byte vertex's position lane)
        std::span<std::uint32_t const> indices{};          ///< item index buffer; a record's first_index is absolute into it
        bool double_sided = true;                          ///< fixtures may opt out of material backface culling
    };

    /// the CPU reference's whole input: the frame camera and every item the CSR list can name
    struct reference_inputs {
        camera_parameters camera{};
        std::span<reference_item const> items{};
    };

    /// the CPU reference's output: the two software images, in the GPU's own empty-value encoding
    struct software_image {
        std::uint32_t width = 0u;
        std::uint32_t height = 0u;
        deren::utility::vessel::vector<std::uint32_t> depth_bits{}; ///< width*height, empty = software_depth_empty
        deren::utility::vessel::vector<std::uint32_t> id{};         ///< width*height, empty = software_id_empty

        [[nodiscard]] software_pixel at(std::uint32_t const x, std::uint32_t const y) const noexcept {
            if (x >= this->width || y >= this->height) {
                return {};
            }
            std::size_t const index = static_cast<std::size_t>(y) * this->width + x;
            return software_pixel{.depth_bits = this->depth_bits[index], .id = this->id[index]};
        }
    };

    /**
     * @brief rasterize the CSR tile list into the two software images (the fixture's expected values)
     * @return the images, or the named reason the input could not be a GPU-produced list/geometry pair
     * @note triangles that are degenerate, have a non-finite projection, or cross the eye plane (w <= 0) are
     *       SKIPPED - v1's classification only moves clusters whose sphere is wholly in front of the near plane, so
     *       a triangle reaching the eye plane is a wiring bug rather than a supported case (the shader does the same)
     * @note the depth is clamped to [0,1] AFTER a finiteness check; a non-finite interpolated z skips the pixel
     *       (spec §5.4: never write NaN into the software depth image)
     */
    [[nodiscard]] inline std::expected<software_image, software_error> rasterize_tiles(software_tile_lists const& plan,
                                                                                       reference_inputs const& inputs) {
        auto const camera = prepare_camera(inputs.camera);
        if (!camera.has_value()) {
            return std::unexpected{camera.error()};
        }
        std::size_t const tiles = plan.grid.tile_count();
        if (plan.offsets.size() != tiles + 1u || plan.counts.size() != tiles || plan.entries.size() != plan.entry_count) {
            return std::unexpected{software_error::invalid_reference};
        }
        software_image image{};
        image.width = camera->width;
        image.height = camera->height;
        std::size_t const pixels = static_cast<std::size_t>(image.width) * image.height;
        image.depth_bits.assign(pixels, software_depth_empty);
        image.id.assign(pixels, software_id_empty);
        std::array<float, 16> const view_projection = detail::reference_multiply(camera->proj, camera->view);
        constexpr float clip_w_epsilon = 1e-5f; // the resolve's near-plane clip epsilon; below it a triangle is clipped
        for (std::uint32_t tile_y = 0u; tile_y < plan.grid.tiles_y; ++tile_y) {
            for (std::uint32_t tile_x = 0u; tile_x < plan.grid.tiles_x; ++tile_x) {
                for (tile_entry const& entry : plan.tile_span(tile_x, tile_y)) {
                    // locate the item: fixture-sized lists make the linear walk the honest, allocation-free choice
                    reference_item const* item = nullptr;
                    for (reference_item const& candidate : inputs.items) {
                        if (candidate.item_index == entry.item_index) {
                            item = &candidate;
                            break;
                        }
                    }
                    if (item == nullptr || entry.meshlet_id >= item->records.size()) {
                        return std::unexpected{software_error::invalid_reference};
                    }
                    meshlet const& record = item->records[entry.meshlet_id];
                    std::uint32_t const triangles = record.index_count / 3u;
                    for (std::uint32_t triangle = 0u; triangle < triangles; ++triangle) {
                        std::array<std::array<float, 2>, 3> screen{};
                        std::array<float, 3> corner_depth{};
                        bool usable = true;
                        for (std::uint32_t corner = 0u; corner < 3u && usable; ++corner) {
                            std::uint64_t const index_position = static_cast<std::uint64_t>(record.first_index) + triangle * 3u + corner;
                            if (index_position >= item->indices.size()) {
                                return std::unexpected{software_error::invalid_reference};
                            }
                            std::int64_t const vertex_index = static_cast<std::int64_t>(item->indices[index_position]) + record.base_vertex + item->base_vertex;
                            if (vertex_index < 0 || static_cast<std::size_t>(vertex_index) >= item->positions.size()) {
                                return std::unexpected{software_error::invalid_reference};
                            }
                            std::array<float, 3> const& position = item->positions[static_cast<std::size_t>(vertex_index)];
                            std::array<double, 4> const clip =
                                detail::reference_transform(view_projection,
                                                            detail::reference_transform(item->model, {position[0], position[1], position[2], 1.0}));
                            if (!std::isfinite(clip[0]) || !std::isfinite(clip[1]) || !std::isfinite(clip[2]) || !std::isfinite(clip[3]) ||
                                clip[3] <= static_cast<double>(clip_w_epsilon)) {
                                usable = false; // behind/clipping the eye plane (v1 never moves such a cluster to software)
                                break;
                            }
                            auto const mapped = visibility::clip_to_screen(
                                std::array<float, 4>{static_cast<float>(clip[0]), static_cast<float>(clip[1]), static_cast<float>(clip[2]), static_cast<float>(clip[3])},
                                static_cast<float>(image.width), static_cast<float>(image.height));
                            if (!mapped.has_value()) {
                                usable = false;
                                break;
                            }
                            screen[corner] = *mapped;
                            corner_depth[corner] = static_cast<float>(clip[2] / clip[3]);
                        }
                        if (!usable || visibility::triangle_twice_area(screen[0], screen[1], screen[2]) == 0.0f) {
                            continue; // degenerate or clipped: covers no pixel
                        }
                        if (!item->double_sided && visibility::triangle_twice_area(screen[0], screen[1], screen[2]) >= 0.0f)
                            continue;
                        float const min_x = std::min({screen[0][0], screen[1][0], screen[2][0]});
                        float const max_x = std::max({screen[0][0], screen[1][0], screen[2][0]});
                        float const min_y = std::min({screen[0][1], screen[1][1], screen[2][1]});
                        float const max_y = std::max({screen[0][1], screen[1][1], screen[2][1]});
                        std::int32_t const y_begin = std::max<std::int32_t>(0, static_cast<std::int32_t>(std::floor(min_y)));
                        std::int32_t const y_end = std::min<std::int32_t>(static_cast<std::int32_t>(image.height) - 1, static_cast<std::int32_t>(std::ceil(max_y)));
                        std::int32_t const x_begin = std::max<std::int32_t>(0, static_cast<std::int32_t>(std::floor(min_x)));
                        std::int32_t const x_end = std::min<std::int32_t>(static_cast<std::int32_t>(image.width) - 1, static_cast<std::int32_t>(std::ceil(max_x)));
                        auto const packed = visibility::pack_id(entry.item_index, entry.meshlet_id, triangle);
                        if (!packed.has_value()) {
                            return std::unexpected{software_error::invalid_reference};
                        }
                        for (std::int32_t pixel_y = y_begin; pixel_y <= y_end; ++pixel_y) {
                            for (std::int32_t pixel_x = x_begin; pixel_x <= x_end; ++pixel_x) {
                                std::array<float, 2> const centre{static_cast<float>(pixel_x) + 0.5f, static_cast<float>(pixel_y) + 0.5f};
                                visibility::barycentrics const barycentrics = visibility::screen_barycentrics(centre, screen[0], screen[1], screen[2]);
                                bool const inside = (barycentrics.a >= 0.0f && barycentrics.b >= 0.0f && barycentrics.c >= 0.0f) ||
                                                    (barycentrics.a <= 0.0f && barycentrics.b <= 0.0f && barycentrics.c <= 0.0f);
                                if (!inside) {
                                    continue;
                                }
                                float depth = barycentrics.a * corner_depth[0] + barycentrics.b * corner_depth[1] + barycentrics.c * corner_depth[2];
                                if (!std::isfinite(depth)) {
                                    continue;
                                }
                                depth = std::clamp(depth, 0.0f, 1.0f);
                                std::uint32_t const depth_bits = std::bit_cast<std::uint32_t>(depth);
                                std::size_t const pixel = static_cast<std::size_t>(pixel_y) * image.width + static_cast<std::size_t>(pixel_x);
                                if (depth_bits < image.depth_bits[pixel] ||
                                    (depth_bits == image.depth_bits[pixel] && packed->value < image.id[pixel])) {
                                    image.depth_bits[pixel] = depth_bits;
                                    image.id[pixel] = packed->value;
                                }
                            }
                        }
                    }
                }
            }
        }
        return image;
    }
} // namespace deren::engine::visibility::software
