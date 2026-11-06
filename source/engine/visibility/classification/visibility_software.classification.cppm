// ============================================================================
// module: deren.engine.visibility.software:classification  (the conservative
//         subpixel criterion and the stable hardware/software record split)
//
// One of the domains of deren.engine.visibility.software; the top-level
// facade re-exports it, so consumers keep importing the same module.
// ============================================================================

module;
#include <cstdint>
export module deren.engine.visibility.software:classification;
export import deren.vstd;

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

} // namespace deren::engine::visibility::software
