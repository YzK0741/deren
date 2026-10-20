/**
 * @file source/engine/visibility/hzb.cppm
 * @defgroup engine_visibility_hzb HZB occlusion culling (the CPU reference)
 * @brief the CPU REFERENCE for the HZB (hierarchical Z-buffer) occlusion-culling design:
 *        docs/superpowers/specs/2026-10-20-hzb-occlusion-design.md §4.1, §5, §6, §9.2 and §10.1.
 *
 * WHY A MODULE OF ITS OWN, AND A PURE ONE. The HZB pipeline is built and consumed entirely ON THE GPU (a build
 * compute pass writes the max pyramid, a cull compute pass tests candidates against it and writes the compacted
 * survivor list and the indirect command words), which is the design's whole point: the host prototype paid
 * +11.2 ms of CPU per frame and was still not conservative (§0.1). Nothing here is on a frame's critical path.
 * What the GPU cannot have is a HEADLESS TEST: a shader's ceil/offset arithmetic and its conservative math are
 * checked only by a picture, and a picture cannot say which of a dozen formulas is the wrong one (the prototype's
 * 0.314% pixel difference was exactly that). So the arithmetic lives here as plain C++, in double, with no device,
 * no RHI type and no shader compiler in the loop, and the headless test (source/tests/test_hzb.cpp) pins:
 *   - the pyramid layout this module sizes the HOST buffer with is the SAME formula the two shaders inline
 *     (§4.1: one expression, `pyramid_words` / `level_offset`, mirrored by hand on the shader side);
 *   - every "occluded" answer is CONSERVATIVE: all prev-depth the ball's footprint covers is strictly nearer than
 *     the ball's own nearest point (§6.7 property 1), and every uncertain case FAILS OPEN to "visible" (§6.6);
 *   - the phase-B scene gate's byte-exact comparison (§9.2) and the cull pass's deterministic ascending
 *     compaction (§4.3, §10.1 item 3) are pure functions pinned case by case.
 *
 * THE TWO APPROXIMATIONS THE HOST PROTOTYPE GOT WRONG, because they shape every function below (§6, §10.1 item 2):
 *   - THE NEAR-DEPTH BOUND (§6.4). The prototype tested `ndc_z - radius / clip.w`. NDC z is a monotone function of
 *     the eye-space depth, so the ball's LOWER bound is its value at the ball's nearest depth
 *     `d_near = w - r_w`: `z_ndc(d_near)`. The two differ by `A*near*r / (w*(w-r))` vs `r/w`, and the prototype's
 *     form is the smaller offset (i.e. the FARTHER bound, i.e. the non-conservative one) exactly when the ball hugs
 *     the near plane. The 1.001*near early-out is the other half of the same fix: it rejects the balls for which
 *     the monotone formula stops being a bound at all.
 *   - THE SCREEN RECT (§6.5). The prototype drew a circle of radius `radius * focal / clip.w` around the
 *     projected centre. Under perspective a large OFF-AXIS ball's footprint is not that circle - its NDC extent
 *     grows as parts of it approach the eye plane - so the circle can be too small, the sampled tiles cover less
 *     than the sphere covers, and a nearer occluder outside the circle is missed. This module projects the EIGHT
 *     CORNERS of the world AABB (the ball is inside it) and takes their NDC min/max: perspective maps convex sets
 *     to convex sets, so the corner bbox contains the whole projected ball.
 *
 * THE HEAP TABLES IT MIRRORS (their slot numbers are declared in `deren.engine.render_layout`, §5.1; phase A2 is
 * what wires their descriptors): `heap_slots::hzb_levels` (the pyramid these layout functions size and index),
 * `heap_slots::hzb_header` (the camera/extent snapshot the build pass writes), `heap_slots::hzb_items` (the per-item
 * candidate runs the cull pass walks), `heap_slots::hzb_results` (per-candidate flags, per-item counts and the stats
 * block), `heap_slots::hzb_survivors` (the compacted records) and `heap_slots::hzb_commands` (the mesh-indirect
 * command-word alias). They are ONE descriptor with an INTERNAL per-frame lane each - a shader reads
 * `heap_at<...>(heap_slots_hzb_x)` and then indexes `frame_slot * stride + index`, never `+ heap_frame_slot` (§5.3).
 */
module;

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

export module deren.engine.visibility.hzb;

export namespace deren::engine::visibility::hzb {
    // ================================================================================================
    // 1. THE PYRAMID LAYOUT (spec §4.1): ONE formula, two consumers
    //
    //    The shader(s) and the host must agree on where each level starts INSIDE one storage buffer - a drift is
    //    not a validation error, it is a build pass writing a level to the wrong words and a cull pass reading
    //    somebody else's depth from them. So the recurrence is written ONCE here (and mirrored by hand in
    //    source/shaders/hzb_build.slang / hzb_cull.slang's inline helpers, exactly as the heap slot grid is
    //    mirrored), and the host sizes `heap_slots::hzb_levels` with `pyramid_words` of the same extent.
    //
    //    level 0 is the depth image verbatim (full resolution); level L is `ceil(w / 2^L)` wide, which is the
    //    number of `2x2 max` tiles that cover a row whose odd tail is replicated (§4.2/§6, see build_max_pyramid).
    // ================================================================================================

    /// the number of levels the build pass can dispatch in one run: 16 levels reach 1x1 from any width/height up to
    /// 32768 - i.e. every swapchain this renderer can create. Mirrored as `hzb_max_levels` in the build shader (§7.1).
    inline constexpr std::uint32_t hzb_max_levels = 16u;
    /// a pyramid word is one D32 float
    inline constexpr std::uint32_t hzb_word_bytes = 4u;

    /// the NDC-depth epsilon of the occlusion comparison (§6.6). Only the conservative side depends on it, so the
    /// rule is "as small as floating point tolerates": the bound is now an exact formula rather than a biased
    /// approximation (the prototype used 1e-4 for that reason, this design uses 1e-5).
    inline constexpr double hzb_ndc_epsilon = 1e-5;
    /// the factor of the cross-near-plane early-out (§6.3): `d_near <= near * this` -> preserve. The monotone bound is
    /// undefined for balls that cross the plane, and the slack is where the derived near bound stops being one.
    inline constexpr double hzb_near_plane_scale = 1.001;
    /// clip-space w below which a position counts as "behind the eye" (§6.3, §6.5)
    inline constexpr double hzb_w_epsilon = 1e-4;

    /// how many candidates ONE item may offer the cull pass: the packed visibility id's meshlet field (C5), and the
    /// per-item cap §9.4 relies on. The id layout lives in deren.engine.visibility; the test cross-checks equality.
    inline constexpr std::uint32_t hzb_item_candidate_capacity = 4096u;
    /// records one frame lane of a meshlet table holds: `hzb_results.flags` and `hzb_survivors` are indexed by
    /// absolute meshlet index and use this capacity (§5.1, mirrored from mesh_geometry.slang's meshlet_table_capacity)
    inline constexpr std::uint32_t hzb_candidate_capacity = 65536u;
} // namespace deren::engine::visibility::hzb

namespace deren::engine::visibility::hzb::detail {
    // ================================================================================================
    // THE MATRIX ARITHMETIC OF §6.2 AND §6.5, in double, with NO convention left to the reader.
    //
    // A matrix is the 16 floats the renderer already puts in its buffers: glm's COLUMN-MAJOR layout, which is what
    // the shaders' `VR_MAT4` (`row_major float4x4`, i.e. SPIR-V ColMajor) is documented to match, and what
    // `mul(vector, matrix)` in Slang computes `matrix * vector` on (heap_access.slang:12-13). So element (row,
    // column) is `m[column * 4 + row]`, and `transform(m, v)` is the host spelling of that shader `mul`.
    //
    // NOT constexpr, deliberately: the bounds below call <cmath> (isfinite/floor/ceil/sqrt/fmax), and this project
    // does not rely on the C++23 constexpr maths the standard library is still allowed to omit.
    // ================================================================================================
    [[nodiscard]] double matrix_element(std::array<double, 16> const& matrix, std::uint32_t const row, std::uint32_t const column) noexcept {
        return matrix[static_cast<std::size_t>(column) * 4u + row];
    }

    [[nodiscard]] std::array<double, 4> transform(std::array<double, 16> const& matrix, std::array<double, 4> const& vector) noexcept {
        std::array<double, 4> result{};
        for (std::uint32_t row = 0; row < 4u; ++row) {
            double sum = 0.0;
            for (std::uint32_t column = 0; column < 4u; ++column) {
                sum += matrix_element(matrix, row, column) * vector[column];
            }
            result[row] = sum;
        }
        return result;
    }

    /// @brief `a * b` in the same column-vector sense the shader's `mul(mul(v, a), b)` computes
    [[nodiscard]] std::array<double, 16> multiply(std::array<double, 16> const& a, std::array<double, 16> const& b) noexcept {
        std::array<double, 16> result{};
        for (std::uint32_t row = 0; row < 4u; ++row) {
            for (std::uint32_t column = 0; column < 4u; ++column) {
                double sum = 0.0;
                for (std::uint32_t inner = 0; inner < 4u; ++inner) {
                    sum += matrix_element(a, row, inner) * matrix_element(b, inner, column);
                }
                result[static_cast<std::size_t>(column) * 4u + row] = sum;
            }
        }
        return result;
    }

    [[nodiscard]] bool all_finite(std::array<double, 16> const& matrix) noexcept {
        for (double const value : matrix) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool all_finite3(std::array<double, 3> const& vector) noexcept {
        for (double const value : vector) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
        return true;
    }

    /// @brief the largest row sum of the matrix's linear part: the operator-norm upper bound §6.2 asks for
    /// @note ROW sums rather than a "largest column length" or a singular value: `|M v|_inf <= max_i sum_j |M_ij|
    ///       |v|_inf`, it is cheap, it never underestimates (which is the only property the test needs - a radius
    ///       bound that is too LARGE only makes the test keep more), and it handles shear and non-uniform scale.
    ///       This is the same bound `host_matrix_max_axis_scale` in source/engine/primitive/primitive.cpp keeps
    ///       beside mesh_geometry.slang's `matrix_max_axis_scale`.
    [[nodiscard]] double matrix_max_axis_scale(std::array<double, 16> const& matrix) noexcept {
        double largest = 0.0;
        for (std::uint32_t row = 0; row < 3u; ++row) {
            double const sum = std::abs(matrix_element(matrix, row, 0u)) + std::abs(matrix_element(matrix, row, 1u)) + std::abs(matrix_element(matrix, row, 2u));
            largest = sum > largest ? sum : largest;
        }
        return largest;
    }

    /// @brief the near/far planes a projection implies, or nullopt when it is not the shape §6.3/§6.4 derive them for
    struct projection_range {
        double near_plane = 0.0;
        double far_plane = 0.0;
    };

    /// @note THE SHAPE IS VALIDATED, NOT ASSUMED. §6.3's `d_near = w - r_w` is only a bound when `clip.w` IS the
    ///       eye-space depth (row 3 = (0,0,-1,0)), §6.4's monotone formula only when clip z does not mix x/y into
    ///       z, and the near derivation `P[2][3]/P[2][2]` only for `perspectiveRH_ZO` (the matrices primitive.cpp
    ///       and shadow_fit.cpp build). A projection of any other shape returns nullopt and the whole test fails
    ///       open to "visible": a bound derived from the wrong formula is not a conservative bound.
    [[nodiscard]] std::optional<projection_range> projection_of(std::array<double, 16> const& projection) noexcept {
        if (!all_finite(projection)) {
            return std::nullopt;
        }
        if (matrix_element(projection, 3u, 0u) != 0.0 || matrix_element(projection, 3u, 1u) != 0.0 || matrix_element(projection, 3u, 2u) != -1.0 ||
            matrix_element(projection, 3u, 3u) != 0.0) {
            return std::nullopt;
        }
        if (matrix_element(projection, 0u, 2u) != 0.0 || matrix_element(projection, 1u, 2u) != 0.0) {
            return std::nullopt;
        }
        double const z_row = matrix_element(projection, 2u, 2u);      // -far / (far - near)
        double const z_constant = matrix_element(projection, 2u, 3u); // -far * near / (far - near)
        if (!(z_row < 0.0) || !(z_constant < 0.0)) {
            return std::nullopt;
        }
        double const near_plane = z_constant / z_row;
        double const denominator = 1.0 + z_row;
        if (denominator == 0.0) {
            return std::nullopt; // an infinite far plane: the far end of the bound below does not exist
        }
        double const far_plane = z_constant / denominator;
        if (!std::isfinite(near_plane) || !std::isfinite(far_plane) || !(near_plane > 0.0) || !(far_plane > near_plane)) {
            return std::nullopt;
        }
        return projection_range{near_plane, far_plane};
    }

    /// @brief the NDC z of an eye-space depth @p depth (§6.4), the one formula the GPU-side inline function mirrors
    /// @note @p depth is the positive distance down the view axis (`-z_eye` for this renderer's RH projection), and
    ///       the division by it is the perspective divide's `w`, which §6.3 established equals that distance.
    [[nodiscard]] double z_ndc_at_depth(std::array<double, 16> const& projection, double const depth) noexcept {
        return (matrix_element(projection, 2u, 2u) * (-depth) + matrix_element(projection, 2u, 3u)) / depth;
    }

    /// @brief the framebuffer rectangle of a world-space ball (§6.5), or nullopt when the test must fail open
    struct screen_rect {
        std::uint32_t x0 = 0;
        std::uint32_t y0 = 0;
        std::uint32_t x1 = 0;
        std::uint32_t y1 = 0;
    };

    /// @note THE CORNER PROJECTION IS THE CONSERVATIVE PART: the ball is inside the world AABB `c_w +/- r_w`, the
    ///       projection of a convex set with all its points in front of the eye is the convex hull of the projected
    ///       corners, and a bbox of those corners therefore CONTAINS the ball's footprint. A corner at or behind the
    ///       eye plane breaks that (its perspective image is not on the same side of the horizon), so such a ball
    ///       is preserved instead. This is the function that replaces the prototype's circle (see the file header).
    [[nodiscard]] std::optional<screen_rect> screen_rect_of(std::array<double, 16> const& view_projection, std::array<double, 3> const& world_center,
                                                            double const world_radius, std::uint32_t const width, std::uint32_t const height) noexcept {
        double min_x = std::numeric_limits<double>::infinity();
        double min_y = std::numeric_limits<double>::infinity();
        double max_x = -std::numeric_limits<double>::infinity();
        double max_y = -std::numeric_limits<double>::infinity();
        for (std::uint32_t corner = 0; corner < 8u; ++corner) {
            std::array<double, 4> const position{
                world_center[0] + ((corner & 1u) != 0u ? world_radius : -world_radius),
                world_center[1] + ((corner & 2u) != 0u ? world_radius : -world_radius),
                world_center[2] + ((corner & 4u) != 0u ? world_radius : -world_radius),
                1.0,
            };
            std::array<double, 4> const clip = transform(view_projection, position);
            if (clip[3] <= hzb_w_epsilon) {
                return std::nullopt; // a corner behind the eye: no finite rectangle bounds this projection
            }
            double const ndc_x = clip[0] / clip[3];
            double const ndc_y = clip[1] / clip[3];
            if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y)) {
                return std::nullopt;
            }
            min_x = ndc_x < min_x ? ndc_x : min_x;
            min_y = ndc_y < min_y ? ndc_y : min_y;
            max_x = ndc_x > max_x ? ndc_x : max_x;
            max_y = ndc_y > max_y ? ndc_y : max_y;
        }
        // §6.5's pixel mapping, with Vulkan's y-down convention already inside `proj` (`proj[1][1] < 0`, exactly the
        // mapping deren::engine::visibility::clip_to_screen documents). floor/ceil rather than round: the rasterizer
        // covers a pixel when its CENTER is inside a triangle, so flooring the left edge and ceiling the right one
        // covers every pixel the geometry could touch.
        double const left = std::floor((min_x * 0.5 + 0.5) * static_cast<double>(width));
        double const top = std::floor((min_y * 0.5 + 0.5) * static_cast<double>(height));
        double const right = std::ceil((max_x * 0.5 + 0.5) * static_cast<double>(width));
        double const bottom = std::ceil((max_y * 0.5 + 0.5) * static_cast<double>(height));
        if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom)) {
            return std::nullopt;
        }
        // The clamp is ONE-SIDED per edge (§6.5): x0 only grows to 0 and x1 only shrinks to width-1, so a rectangle
        // that lies entirely off an edge ends up with x1 < x0 (and is preserved) instead of collapsing onto the
        // border column and sampling it.
        double const clamped_left = left < 0.0 ? 0.0 : left;
        double const clamped_top = top < 0.0 ? 0.0 : top;
        double const clamped_right = right > static_cast<double>(width) - 1.0 ? static_cast<double>(width) - 1.0 : right;
        double const clamped_bottom = bottom > static_cast<double>(height) - 1.0 ? static_cast<double>(height) - 1.0 : bottom;
        if (clamped_right < clamped_left || clamped_bottom < clamped_top) {
            return std::nullopt; // entirely outside the OLD frustum: it could not have been in the old image
        }
        return screen_rect{
            static_cast<std::uint32_t>(clamped_left),
            static_cast<std::uint32_t>(clamped_top),
            static_cast<std::uint32_t>(clamped_right),
            static_cast<std::uint32_t>(clamped_bottom),
        };
    }
} // namespace deren::engine::visibility::hzb::detail

export namespace deren::engine::visibility::hzb {
    /// one level's dimensions
    struct level_size {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    /**
     * @brief the width of level @p level: the `ceil((w + 1) / 2)` recurrence of §4.1, applied @p level times
     * @note the recurrence is integer `(w + 1) / 2` = `ceil(w / 2)`; for any positive width it reaches 1 and then
     *       stays 1, so no `max(1, ...)` clamp is needed (and would only mask a degenerate 0-wide extent - which is
     *       refused as a whole pyramid, see level_count).
     */
    [[nodiscard]] constexpr std::uint32_t level_width(std::uint32_t const width, std::uint32_t const level) noexcept {
        if (width == 0u) {
            return 0u;
        }
        std::uint32_t result = width;
        for (std::uint32_t step = 0; step < level; ++step) {
            result = (result + 1u) / 2u;
        }
        return result;
    }

    /// @brief the height of level @p level (same recurrence as @ref level_width)
    [[nodiscard]] constexpr std::uint32_t level_height(std::uint32_t const height, std::uint32_t const level) noexcept {
        if (height == 0u) {
            return 0u;
        }
        std::uint32_t result = height;
        for (std::uint32_t step = 0; step < level; ++step) {
            result = (result + 1u) / 2u;
        }
        return result;
    }

    /// @brief both dimensions of level @p level
    [[nodiscard]] constexpr level_size level_size_of(std::uint32_t const width, std::uint32_t const height, std::uint32_t const level) noexcept {
        return level_size{level_width(width, level), level_height(height, level)};
    }

    /// @brief how many levels a pyramid over @p width x @p height has: level 0 plus one per halving until 1x1
    /// @return 0 for a degenerate extent (either dimension 0), which no render target has and which the pyramid
    ///         builder and the occlusion test both refuse
    [[nodiscard]] constexpr std::uint32_t level_count(std::uint32_t const width, std::uint32_t const height) noexcept {
        if (width == 0u || height == 0u) {
            return 0u;
        }
        std::uint32_t remaining_width = width;
        std::uint32_t remaining_height = height;
        std::uint32_t levels = 1u;
        while (remaining_width > 1u || remaining_height > 1u) {
            remaining_width = (remaining_width + 1u) / 2u;
            remaining_height = (remaining_height + 1u) / 2u;
            ++levels;
        }
        return levels;
    }

    /**
     * @brief the word offset of level @p level inside one frame's lane
     * @param level clamped to the pyramid's own level count: `pyramid_words` returns the total word count, which is
     *        the only value a caller can sensibly ask for past the last 1x1 level
     * @note THE SINGLE FORMULA §4.1 ASKS FOR: the host sizes the buffer with `pyramid_words`, the build pass writes
     *       level L at this offset and the cull pass reads tile (tx, ty) of level L at this offset + ty * w_L + tx.
     */
    [[nodiscard]] constexpr std::uint32_t level_offset(std::uint32_t const width, std::uint32_t const height, std::uint32_t const level) noexcept {
        std::uint32_t const levels = level_count(width, height);
        std::uint32_t const last = level < levels ? level : levels;
        std::uint32_t offset = 0u;
        for (std::uint32_t each = 0; each < last; ++each) {
            offset += level_width(width, each) * level_height(height, each);
        }
        return offset;
    }

    /// @brief how many floats one frame's lane of the pyramid holds: the size `heap_slots::hzb_levels` is written with
    /// @note 3840x2160 needs ~11.06M words (44.2 MB) per lane; that fits 32 bits with room, which is what lets the
    ///       host and the shader both do this arithmetic in `uint` (spec §13's measured memory report).
    [[nodiscard]] constexpr std::uint32_t pyramid_words(std::uint32_t const width, std::uint32_t const height) noexcept {
        return level_offset(width, height, level_count(width, height));
    }

    /// @brief how many BYTES one frame's lane of the pyramid holds (the value a heap descriptor write takes)
    [[nodiscard]] constexpr std::uint32_t pyramid_bytes(std::uint32_t const width, std::uint32_t const height) noexcept {
        return pyramid_words(width, height) * hzb_word_bytes;
    }

    // ================================================================================================
    // 2. THE MAX PYRAMID (spec §4.1/§4.2)
    //
    //    A VIEW over the words, not an owner: the GPU's buffer and this reference's storage have the same layout,
    //    so the test can build one from synthetic depth and hand it to the occlusion test exactly as the cull pass
    //    would hand it the built buffer. The accessor is deliberately unchecked - it is the hot loop's inner read
    //    on the GPU side too - and `valid()` is the one gate a caller must pass first.
    // ================================================================================================
    struct max_pyramid_view {
        std::span<float const> words{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t levels = 0;

        /// @brief whether the span is long enough to hold every level this extent needs
        [[nodiscard]] constexpr bool valid() const noexcept {
            return this->width != 0u && this->height != 0u && this->levels != 0u && this->levels == level_count(this->width, this->height) &&
                   this->words.size() >= pyramid_words(this->width, this->height);
        }

        /// @brief one texel (float depth) of @p level
        /// @pre valid(), and (level, x, y) inside that level's extent
        [[nodiscard]] constexpr float at(std::uint32_t const level, std::uint32_t const x, std::uint32_t const y) const noexcept {
            std::size_t const index = std::size_t(level_offset(this->width, this->height, level)) + std::size_t(y) * level_width(this->width, level) + x;
            return this->words[index];
        }

        /// @brief @p level's first word, for a caller that has already computed the sizes
        /// @pre valid() and level < levels
        [[nodiscard]] constexpr std::span<float const> level_words(std::uint32_t const level) const noexcept {
            std::size_t const begin = level_offset(this->width, this->height, level);
            level_size const size = level_size_of(this->width, this->height, level);
            return this->words.subspan(begin, std::size_t(size.width) * size.height);
        }
    };

    /**
     * @brief build the max pyramid of a depth image into @p storage: level 0 is the depth image verbatim, and level
     *        L is the 2x2 max of level L-1 with the odd boundary's neighbourhood REPLICATED
     * @return a view of @p storage's used prefix, or an invalid view (empty span) when the inputs are too small
     * @note REPLICATION IS CONSERVATIVE, and that is the whole reason for the clamp rather than a sentinel: the last
     *       column/row of an odd-width level has no right/bottom neighbour, and pulling the edge value in can only
     *       raise a max that is already taken over a real texel. A larger max can only make the occlusion comparison
     *       "prev depth >= ball near depth" MORE likely, i.e. keep the meshlet - the direction every failure in this
     *       file has to take (§4.2). This is the same argument the GPU build pass repeats in source/shaders/.
     * @note level 0 is a COPY rather than a 2x2 max of the depth (no half-resolution HZB in this design, §12 Q2):
     *       the offline probe and this reference then describe the same thing, which is what makes the GPU build
     *       pass checkable against a numpy rebuild word for word (§10.2).
     */
    [[nodiscard]] max_pyramid_view build_max_pyramid(std::span<float const> const depth, std::uint32_t const width, std::uint32_t const height, std::span<float> const storage) noexcept {
        std::uint32_t const levels = level_count(width, height);
        std::size_t const depth_texels = std::size_t(width) * height;
        if (levels == 0u || depth.size() < depth_texels || storage.size() < pyramid_words(width, height)) {
            return {};
        }
        for (std::size_t texel = 0; texel < depth_texels; ++texel) {
            storage[texel] = depth[texel];
        }
        for (std::uint32_t level = 1u; level < levels; ++level) {
            level_size const previous = level_size_of(width, height, level - 1u);
            level_size const current = level_size_of(width, height, level);
            std::size_t const previous_begin = level_offset(width, height, level - 1u);
            std::size_t const current_begin = level_offset(width, height, level);
            for (std::uint32_t y = 0; y < current.height; ++y) {
                std::uint32_t const y0 = 2u * y < previous.height ? 2u * y : previous.height - 1u;
                std::uint32_t const y1 = 2u * y + 1u < previous.height ? 2u * y + 1u : previous.height - 1u;
                for (std::uint32_t x = 0; x < current.width; ++x) {
                    std::uint32_t const x0 = 2u * x < previous.width ? 2u * x : previous.width - 1u;
                    std::uint32_t const x1 = 2u * x + 1u < previous.width ? 2u * x + 1u : previous.width - 1u;
                    float const top_left = storage[previous_begin + std::size_t(y0) * previous.width + x0];
                    float const top_right = storage[previous_begin + std::size_t(y0) * previous.width + x1];
                    float const bottom_left = storage[previous_begin + std::size_t(y1) * previous.width + x0];
                    float const bottom_right = storage[previous_begin + std::size_t(y1) * previous.width + x1];
                    // fmax rather than `a > b ? a : b`: a NaN depth from garbage input must not swallow the max of the
                    // real neighbours (the GPU's max over a cleared 1.0 and a NaN is not defined either, but here the
                    // conservative answer is "keep the larger real depth")
                    storage[current_begin + std::size_t(y) * current.width + x] = std::fmax(std::fmax(top_left, top_right), std::fmax(bottom_left, bottom_right));
                }
            }
        }
        return max_pyramid_view{storage.first(pyramid_words(width, height)), width, height, levels};
    }

    // ================================================================================================
    // 3. THE CONSERVATIVE SPHERE-vs-HZB TEST (spec §6)
    //
    //    Inputs are exactly what the cull pass has in front of it (§6.1): the meshlet record's OBJECT-space sphere,
    //    the item's model matrix, and the HZB HEADER's OLD view/proj/camera position (the matrices that BUILT the
    //    pyramid - not the current frame's; that correspondence is what makes "the old depth" the right depth to
    //    test against, and phase B's byte-exact gate makes the current frame identical to the old one, §9.2).
    // ================================================================================================
    using hzb_matrix = std::array<double, 16>;

    /// a meshlet's bounding sphere in OBJECT space (meshlet.cppm's record, C1)
    struct hzb_sphere {
        std::array<double, 3> center{};
        double radius = 0.0;
    };

    /// what the HZB header carries about the frame that built it (§4.1), widened to double for the reference (§6.7)
    struct hzb_camera_snapshot {
        hzb_matrix view{};
        hzb_matrix proj{};
        std::array<double, 3> position{};
    };

    /// the answer. THE VALUES ARE THE CULL PASS'S FLAGS WORD (§4.3): 0 = survivor, 1 = culled, so a probe comparing
    /// the CPU reference against a captured `hzb_results.flags` entry compares numbers and not enum encodings.
    enum class hzb_decision : std::uint32_t {
        visible = 0u,
        occluded = 1u,
    };

    /**
     * @brief decide whether a meshlet's sphere is CONSERVATIVELY occluded by the previous frame's depth
     * @return `occluded` only when every depth sample under the ball's footprint is strictly nearer than the ball's
     *         nearest possible point; `visible` in every other case, INCLUDING each case where a bound is invalid
     * @note EVERY EARLY RETURN BELOW IS A FAIL-OPEN, and the list is the exhaustive set of them (§6.3-§6.6):
     *       pyramid unusable; non-finite sphere/matrix/camera; projection not the shape §6.4 derives; centre behind
     *       the eye (w <= 1e-4); eye inside the sphere; ball within `1.001 * near` of the near plane; ball entirely
     *       past the far plane; a projected AABB corner behind the eye; the footprint entirely outside the old
     *       frustum; and the sampled tiles containing the clear value (no geometry). The only way this function can
     *       be wrong in the picture is by drawing a meshlet that would have been hidden, never by hiding one.
     */
    [[nodiscard]] hzb_decision hzb_test_sphere(hzb_camera_snapshot const& camera, hzb_matrix const& model, hzb_sphere const& sphere, max_pyramid_view const& pyramid) noexcept {
        // ---- 0. the inputs a bound could be derived from ----
        if (!pyramid.valid()) {
            return hzb_decision::visible;
        }
        if (!(sphere.radius >= 0.0) || !std::isfinite(sphere.radius) || !detail::all_finite3(sphere.center) || !detail::all_finite3(camera.position) || !detail::all_finite(camera.view) ||
            !detail::all_finite(camera.proj) || !detail::all_finite(model)) {
            // a bound that is not a number is not a bound: the same rule subtilis::sphere_visible documents for its
            // own frustum test ("draw it rather than reject it")
            return hzb_decision::visible;
        }
        std::optional<detail::projection_range> const projection = detail::projection_of(camera.proj);
        if (!projection.has_value()) {
            return hzb_decision::visible;
        }

        // ---- §6.2: the world-space ball. The radius uses the row-sum bound of the MODEL matrix only: the bounds
        // that need the view/projection's scale are derived analytically below instead of scaling a radius by them ----
        std::array<double, 3> const world_center = [&] {
            std::array<double, 4> const center = detail::transform(model, std::array<double, 4>{sphere.center[0], sphere.center[1], sphere.center[2], 1.0});
            return std::array<double, 3>{center[0], center[1], center[2]};
        }();
        double const world_radius = sphere.radius * detail::matrix_max_axis_scale(model);
        if (!detail::all_finite3(world_center) || !std::isfinite(world_radius)) {
            return hzb_decision::visible;
        }

        // ---- §6.3: the cheapest early-outs. `w` is the clip-space w of the centre, which for the validated
        // projection is the centre's distance down the view axis, so the ball's nearest point is `w - r_w` ----
        std::array<double, 16> const view_projection = detail::multiply(camera.proj, camera.view);
        std::array<double, 4> const center_clip = detail::transform(view_projection, std::array<double, 4>{world_center[0], world_center[1], world_center[2], 1.0});
        double const center_w = center_clip[3];
        if (!std::isfinite(center_w) || center_w <= hzb_w_epsilon) {
            return hzb_decision::visible; // centre behind the eye plane
        }
        double const offset_x = world_center[0] - camera.position[0];
        double const offset_y = world_center[1] - camera.position[1];
        double const offset_z = world_center[2] - camera.position[2];
        double const eye_distance = std::sqrt(offset_x * offset_x + offset_y * offset_y + offset_z * offset_z);
        if (eye_distance <= world_radius) {
            return hzb_decision::visible; // the eye is inside the ball: no projection bound means anything
        }
        double const nearest_depth = center_w - world_radius; // = d_near
        if (nearest_depth <= projection->near_plane * hzb_near_plane_scale) {
            return hzb_decision::visible; // crosses (or hugs) the near plane: §6.4's bound is not defined here
        }
        if (nearest_depth >= projection->far_plane) {
            return hzb_decision::visible; // entirely past the far plane: it was not in the old image
        }

        // ---- §6.4: the near-depth bound, the monotone formula (NOT the prototype's radius/w approximation) ----
        double const near_ndc = detail::z_ndc_at_depth(camera.proj, nearest_depth);
        if (!std::isfinite(near_ndc)) {
            return hzb_decision::visible;
        }

        // ---- §6.5: the screen rectangle, from the world AABB's projected corners (NOT the prototype's circle) ----
        std::optional<detail::screen_rect> const rect = detail::screen_rect_of(view_projection, world_center, world_radius, pyramid.width, pyramid.height);
        if (!rect.has_value()) {
            return hzb_decision::visible;
        }

        // ---- §6.6: pick a level where the rectangle covers at most ~4x4 tiles, then take the max of exactly the
        // tiles covering [x0..x1] - a CLOSED range, because `ceil` pyramids put pixel x in tile x >> L ----
        std::uint32_t const horizontal = rect->x1 - rect->x0;
        std::uint32_t const vertical = rect->y1 - rect->y0;
        std::uint32_t const widest = horizontal > vertical ? horizontal : vertical;
        std::uint32_t const span = widest > 1u ? widest : 1u;
        std::uint32_t level = 0u;
        if (span > 1u) {
            std::uint32_t const floor_log2 = std::bit_width(span) - 1u;
            level = floor_log2 > 0u ? floor_log2 - 1u : 0u;
        }
        level = level < pyramid.levels ? level : pyramid.levels - 1u;
        level_size const level_extent = level_size_of(pyramid.width, pyramid.height, level);
        std::uint32_t const tile_x0 = rect->x0 >> level;
        std::uint32_t const tile_y0 = rect->y0 >> level;
        std::uint32_t tile_x1 = rect->x1 >> level;
        std::uint32_t tile_y1 = rect->y1 >> level;
        tile_x1 = tile_x1 < level_extent.width ? tile_x1 : level_extent.width - 1u;
        tile_y1 = tile_y1 < level_extent.height ? tile_y1 : level_extent.height - 1u;
        float farthest = 0.0f;
        for (std::uint32_t tile_y = tile_y0; tile_y <= tile_y1; ++tile_y) {
            for (std::uint32_t tile_x = tile_x0; tile_x <= tile_x1; ++tile_x) {
                float const depth = pyramid.at(level, tile_x, tile_y);
                farthest = depth > farthest ? depth : farthest;
            }
        }
        // the clear value is 1.0 (the far plane), so a tile that still holds it means "no geometry here": preserving
        // that ball is what makes an empty sky the exact boundary case it should be, with no sentinel in the pyramid
        if (!(farthest < 1.0f)) {
            return hzb_decision::visible;
        }
        if (near_ndc > static_cast<double>(farthest) + hzb_ndc_epsilon) {
            return hzb_decision::occluded; // every covered tile is strictly nearer than the ball's nearest point
        }
        return hzb_decision::visible;
    }

    // ================================================================================================
    // 4. THE PHASE-B SCENE GATE (spec §9.2): byte-exact, not epsilon
    //
    //    The stage-B claim is "the previous frame's depth IS this frame's depth", and it is only ever made when the
    //    camera matrices and every visible item's model matrix are BYTE-IDENTICAL to the ones the HZB was built
    //    with. A value comparison would be a different contract (it would accept a 1-ulp move, which is exactly the
    //    motion the gate exists to reject), so this snapshots the bit patterns and compares those. The gate does
    //    NOT know about the depth extent or the render pass: those are the caller's `hzb_valid` bookkeeping (§9.1).
    // ================================================================================================

    /// the bit patterns of one 16-float matrix lane. NOT values: the comparison above must distinguish -0.0 from
    /// +0.0 and compare NaN payloads, i.e. it has to be `memcmp` semantics.
    using hzb_matrix_bits = std::array<std::uint32_t, 16>;

    /// @brief snapshot one 16-float matrix lane as the bits the gate compares
    /// @param values a CONTIGUOUS matrix lane - a raised-to-double `glm::mat4`'s bytes, a `std::array<float, 16>`, or
    ///        a camera UBO member's storage; the first 16 floats are snapshotted. A shorter span snapshots zeroes
    ///        (a caller bug, and the resulting snapshot fails the gate rather than passing it).
    [[nodiscard]] constexpr hzb_matrix_bits hzb_matrix_bits_of(std::span<float const> const values) noexcept {
        hzb_matrix_bits bits{};
        for (std::size_t element = 0; element < bits.size(); ++element) {
            bits[element] = element < values.size() ? std::bit_cast<std::uint32_t>(values[element]) : 0u;
        }
        return bits;
    }

    /// one visible item the gate remembers: the primitive's identity AND its model matrix (both, because admission
    /// only looks at skin/morph/instance flags - a node whose transform animates is admitted as static, §9.2)
    struct hzb_scene_item {
        std::uintptr_t leaf = 0;
        hzb_matrix_bits model{};
    };

    /// the phase-B snapshots: the camera's two matrix lanes plus the ordered per-item list it was recorded with
    struct hzb_scene_snapshot {
        hzb_matrix_bits view{};
        hzb_matrix_bits proj{};
        std::span<hzb_scene_item const> items{};
    };

    /// @brief snapshot one visible item for the gate
    [[nodiscard]] constexpr hzb_scene_item hzb_capture_item(std::uintptr_t const leaf, std::span<float const> const model) noexcept {
        return hzb_scene_item{leaf, hzb_matrix_bits_of(model)};
    }

    /// @brief snapshot the camera lanes and the item list for the gate
    [[nodiscard]] constexpr hzb_scene_snapshot hzb_capture_scene(std::span<float const> const view, std::span<float const> const proj,
                                                                 std::span<hzb_scene_item const> const items) noexcept {
        return hzb_scene_snapshot{hzb_matrix_bits_of(view), hzb_matrix_bits_of(proj), items};
    }

    /**
     * @brief whether this frame is byte-identical to the frame the HZB's depth was built from (§9.2)
     * @return true only when BOTH camera matrices are byte-equal AND the item lists have the same length and every
     *         item is byte-equal in identity and model matrix - i.e. the previous frame's depth is this frame's depth
     *         for every pixel the cull will reason about
     * @note TAA's jitter lives in `proj`, so a TAA-enabled build never passes this gate: that is stated in §9.2 as
     *       the intended behaviour (phase C is what re-opens moving cameras, and its correctness comes from the
     *       second pass rather than this gate).
     */
    [[nodiscard]] constexpr bool hzb_scene_gate(hzb_scene_snapshot const& previous, hzb_scene_snapshot const& current) noexcept {
        if (previous.view != current.view || previous.proj != current.proj) {
            return false;
        }
        if (previous.items.size() != current.items.size()) {
            return false;
        }
        for (std::size_t index = 0; index < previous.items.size(); ++index) {
            if (previous.items[index].leaf != current.items[index].leaf || previous.items[index].model != current.items[index].model) {
                return false;
            }
        }
        return true;
    }

    // ================================================================================================
    // 5. THE DETERMINISTIC COMPACTION REFERENCE (spec §4.3, §10.1 item 3)
    //
    //    The cull pass writes the survivors of an item's candidate run into `heap_slots::hzb_survivors` in ASCENDING
    //    candidate order (one write per kept record, wave ballot + prefix within a chunk, chunk order across them,
    //    §7.2), and the mesh stage then treats the survivor's position as the workgroup id. So the ORDER is a
    //    contract: it is what makes a capture reproducible and what lets this reference be compared against a
    //    captured table entry by entry. Anything the GPU could not have produced (a mask that is not the candidate
    //    count, more candidates than the id can name, an output that cannot hold the survivors) is reported as
    //    nullopt so the caller keeps the old path instead of writing a table with a wrong count (§9.4).
    // ================================================================================================
    struct hzb_compaction {
        std::uint32_t candidates = 0;
        std::uint32_t survivors = 0;
    };

    /**
     * @brief compact @p keep over @p candidate_ids: `survivors[k] = candidate_ids[i]` for the k-th kept i, i ascending
     * @param survivors the output prefix to write; must hold at least the number of kept candidates
     * @return the two counts, or nullopt when the input could not be a valid cull result
     */
    [[nodiscard]] constexpr std::optional<hzb_compaction> hzb_compact(std::span<std::uint32_t const> const candidate_ids, std::span<bool const> const keep,
                                                                      std::span<std::uint32_t> const survivors) noexcept {
        if (keep.size() != candidate_ids.size() || candidate_ids.size() > hzb_item_candidate_capacity) {
            return std::nullopt;
        }
        std::size_t kept = 0;
        for (std::size_t index = 0; index < keep.size(); ++index) {
            if (keep[index]) {
                ++kept;
            }
        }
        if (survivors.size() < kept) {
            return std::nullopt; // an output bigger than the run cannot be written without reusing another item's slots
        }
        std::uint32_t written = 0;
        for (std::size_t index = 0; index < candidate_ids.size(); ++index) {
            if (keep[index]) {
                survivors[written] = candidate_ids[index];
                ++written;
            }
        }
        return hzb_compaction{static_cast<std::uint32_t>(candidate_ids.size()), written};
    }
} // namespace deren::engine::visibility::hzb
