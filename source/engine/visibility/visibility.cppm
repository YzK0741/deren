/**
 * @file source/engine/visibility/visibility.cppm
 * @defgroup engine_visibility Visibility buffer
 * @brief the CPU-side vocabulary of the VISIBILITY BUFFER: how a rasterized pixel names the triangle
 *        that won it, and the per-frame draw-item record a resolve pass reads to get that triangle back.
 *
 * WHY THIS IS A MODULE OF ITS OWN, and a PURE one: the packing and the record layout are ARITHMETIC, they
 * need no device, no allocator and no Vulkan type, and getting them wrong is invisible until a resolve pass
 * samples an id that decodes to the wrong triangle - a picture, not a compile error. Keeping the vocabulary
 * free of dependencies is what lets the headless tests pin the exact bit layout and the exact record size,
 * both of which the GLSL side will mirror by hand (the shader cannot include a C++ enum, exactly as the heap
 * slot grid and the material lanes are mirrored).
 *
 * IT DOES NOT OWN THE BUFFER, AND IT DOES NOT DECIDE WHICH FRAME IS ELIGIBLE. What it states is the FORMAT:
 * the id a geometry raster writes, the record the host fills while it culls and builds the draws, and the
 * bounds each of them has. The render graph owns the image, the runtime owns the per-frame item table and
 * decides whether the frame can use the path at all.
 *
 * THE PACKING IS v0's, NOT THE FINAL ONE. The lightweight-Nanite roadmap replaces the item/meshlet half with
 * cluster groups and a hierarchy; that change belongs WITH the hierarchy, not before it, so this layout stays
 * as small as the current meshlet path needs: one valid bit (a triangle at id 0 must not read as "empty"),
 * an item index, the item's meshlet order, and the meshlet-local triangle. All bounds are capacities of the
 * table the host writes; an out-of-range pack FAILS rather than truncating, and the caller falls back to the
 * old geometry path for that frame instead of drawing a picture assembled from the wrong triangles.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

export module deren.engine.visibility;

export namespace deren::engine::visibility {
    // =============================================================================================
    // 1. THE PACKED ID (one R32_UINT texel of the visibility buffer)
    //
    //    bit 31    : valid - always set for a written id; 0 means "no geometry won this pixel"
    //    bits 30:21: item            - which (primitive, instance) draw item
    //    bits 20:7 : meshlet_in_item - the item's meshlet ORDER (culled-run survivor index or table index)
    //    bits 6:0  : triangle        - the triangle inside that meshlet
    //
    // The layout is spelled as shifts and masks rather than as a struct with bitfields: the shader side
    // mirrors the same arithmetic, and a bitfield's layout would be the compiler's decision, not the
    // renderer's. The 85-triangle mesh-workgroup limit is well inside the 128 a triangle field holds.
    // =============================================================================================
    inline constexpr std::uint32_t id_valid_bits = 1u;
    inline constexpr std::uint32_t id_item_bits = 10u;
    inline constexpr std::uint32_t id_meshlet_bits = 14u;
    inline constexpr std::uint32_t id_triangle_bits = 7u;

    inline constexpr std::uint32_t id_triangle_shift = 0u;
    inline constexpr std::uint32_t id_meshlet_shift = id_triangle_shift + id_triangle_bits;
    inline constexpr std::uint32_t id_item_shift = id_meshlet_shift + id_meshlet_bits;
    inline constexpr std::uint32_t id_valid_shift = id_item_shift + id_item_bits;

    static_assert(id_valid_shift + id_valid_bits == 32u, "the packed id must fill exactly one 32-bit word, no spare bits and no overflow");

    /// how many values each field can hold (the LAST value is capacity - 1)
    inline constexpr std::uint32_t id_item_capacity = 1u << id_item_bits;
    inline constexpr std::uint32_t id_meshlet_capacity = 1u << id_meshlet_bits;
    inline constexpr std::uint32_t id_triangle_capacity = 1u << id_triangle_bits;

    /// the mask of one field, already shifted into place
    [[nodiscard]] constexpr std::uint32_t id_field_mask(std::uint32_t const bits, std::uint32_t const shift) noexcept {
        return ((1u << bits) - 1u) << shift;
    }

    /**
     * @brief one pixel's visibility record: the triangle that won the depth test, or "none"
     * @note the BUFFER CLEAR VALUE is `id{}.value` (zero), which is why a valid zero triangle cannot be
     *       mistaken for an empty pixel: a written id always carries the valid bit.
     */
    struct id {
        std::uint32_t value = 0;

        /// "no geometry won this pixel": the value the buffer is cleared to
        [[nodiscard]] static constexpr id none() noexcept {
            return {};
        }

        [[nodiscard]] constexpr bool valid() const noexcept {
            return (this->value & id_field_mask(id_valid_bits, id_valid_shift)) != 0u;
        }
        [[nodiscard]] constexpr std::uint32_t item() const noexcept {
            return (this->value >> id_item_shift) & ((1u << id_item_bits) - 1u);
        }
        [[nodiscard]] constexpr std::uint32_t meshlet_in_item() const noexcept {
            return (this->value >> id_meshlet_shift) & ((1u << id_meshlet_bits) - 1u);
        }
        [[nodiscard]] constexpr std::uint32_t triangle() const noexcept {
            return (this->value >> id_triangle_shift) & ((1u << id_triangle_bits) - 1u);
        }
    };

    /**
     * @brief pack one pixel's id, or report that it does not fit this layout
     * @return the id, or `std::nullopt` when any field is past its capacity - the caller falls back for the
     *         WHOLE frame rather than packing a truncated id (an id that decodes to a different triangle is
     *         a wrong picture, not a clamp)
     */
    [[nodiscard]] constexpr std::optional<id> pack_id(std::uint32_t const item, std::uint32_t const meshlet_in_item, std::uint32_t const triangle) noexcept {
        if (item >= id_item_capacity || meshlet_in_item >= id_meshlet_capacity || triangle >= id_triangle_capacity)
            return std::nullopt;
        return id{.value = (1u << id_valid_shift) | (item << id_item_shift) | (meshlet_in_item << id_meshlet_shift) | (triangle << id_triangle_shift)};
    }

    // =============================================================================================
    // 2. THE PER-FRAME DRAW-ITEM RECORD (one element per visible item, host-visible)
    //
    //    The raster pass pushes the draw's geometry lanes and meshlet-run base per item; the resolve pass
    //    has no push block, so the same facts live in this table: where the item's vertex/index data is,
    //    which window of it to read, which meshlet run and table the id's meshlet order indexes, and the
    //    transform/material the fragment would have seen. `model` stays a plain float array so the
    //    record has no glm dependency; the host fills it at the same place it records the draw.
    //
    //    SIZE: twelve 32-bit lanes then the matrix (16 floats). 48 + 64 = 112 bytes, and 112 is a
    //    multiple of 16 - the std430 struct's alignment in the resolve shader - so the record needs no
    //    trailing padding and the same number is asserted on both sides.
    // =============================================================================================
    /// where the item's meshlet records come from: the import-time table or this frame's host-culled lane
    enum class meshlet_source : std::uint32_t { table = 0u,
                                                culled = 1u };

    /// the number of 32-bit lanes before the model matrix
    inline constexpr std::uint32_t draw_item_dwords = 12u;
    inline constexpr std::uint32_t draw_item_model_offset = draw_item_dwords * sizeof(std::uint32_t);
    inline constexpr std::uint32_t draw_item_bytes = draw_item_model_offset + 16u * sizeof(float);

    static_assert(draw_item_model_offset % 16u == 0u, "the matrix must land on the std430 16-byte alignment");
    static_assert(draw_item_bytes % 16u == 0u, "a std430 structured-buffer element cannot straddle its alignment");
    static_assert(draw_item_bytes == 112u, "the shader mirrors this number");

    struct draw_item {
        std::uint32_t vertex_address_low = 0; ///< device address of the item's vertex buffer base
        std::uint32_t vertex_address_high = 0;
        std::uint32_t index_address_low = 0; ///< device address of the item's index buffer base
        std::uint32_t index_address_high = 0;
        std::uint32_t first_index = 0;    ///< the draw's first index window (the meshlet adds its own)
        std::int32_t base_vertex = 0;     ///< added to every fetched index (static-draw chunks)
        std::uint32_t index_width = 4u;   ///< bytes per index: 2 or 4
        std::uint32_t meshlet_base = 0;   ///< where the item's meshlet run starts in its source table
        std::uint32_t meshlet_table = 0;  ///< `meshlet_source` as a lane: 0 table, 1 culled
        std::uint32_t material_index = 0; ///< the material the fragment would have shaded with
        std::uint32_t motion_index = 0;   ///< previous_transform index (the velocity half)
        std::uint32_t flags = 0;          ///< reserved: 0 in v0
        std::array<float, 16> model = {}; ///< the draw's model matrix, row-major like the push block
    };

    static_assert(sizeof(draw_item) == draw_item_bytes, "the record must be exactly the documented size");
    static_assert(std::is_trivially_copyable_v<draw_item>, "the host writes it as bytes");
    static_assert(offsetof(draw_item, vertex_address_low) == 0u);
    static_assert(offsetof(draw_item, meshlet_table) == 32u);
    static_assert(offsetof(draw_item, model) == draw_item_model_offset);
} // namespace deren::engine::visibility
