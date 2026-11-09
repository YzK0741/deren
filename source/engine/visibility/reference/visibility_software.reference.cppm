// ============================================================================
// module: deren.engine.visibility.software:reference  (the CPU reference
//         rasterizer used by fixtures' expected software images)
//
// This is the only domain that stands alone: it consumes the tiling plan and
// the merge contract, but no production path consumes it.
// ============================================================================

module;
#include <cstdint>
export module deren.engine.visibility.software:reference;
export import deren.vstd;

import deren.engine.visibility;
import deren.engine.meshlet;
import deren.utility.shared;

import :classification;
import :tiling;
import :merge;

export namespace deren::engine::visibility::software {
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

    /// the CPU reference's output: the two software images, in the GPU's empty-value encoding
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
                        if (!item->double_sided && visibility::triangle_twice_area(screen[0], screen[1], screen[2]) >= 0.0f) {
                            continue;
                        }
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
