// Headless unit tests: the visibility buffer's CPU-side vocabulary ==============
// The packed id and the draw-item record are mirrored BY HAND in the geometry raster's mesh/fragment shader
// and in the resolve compute shader (a shader cannot include a C++ enum). These tests are the C++ half of
// that mirror: the bit arithmetic, the capacities, the exact record size and the field offsets.
#include "vk_test.h"

#include <cstddef>
#include <cstdint>

import deren.engine.visibility;

namespace {
    using namespace deren::engine::visibility;

    void test_id_layout() {
        // the budget is exact: one valid bit plus the three fields fills one 32-bit word
        static_assert(id_valid_bits + id_item_bits + id_meshlet_bits + id_triangle_bits == 32u);
        static_assert(id_valid_shift + id_valid_bits == 32u);
        static_assert((1u << id_item_bits) * (1u << id_meshlet_bits) * (1u << id_triangle_bits) == 0x80000000u);
        // a packed item 0 / meshlet 0 / triangle 0 is NOT the empty pixel: the valid bit is the difference
        static_assert(pack_id(0u, 0u, 0u)->value == 0x80000000u);
        static_assert(id{}.value == 0u);
        static_assert(pack_id(id_item_capacity - 1u, id_meshlet_capacity - 1u, id_triangle_capacity - 1u)->value == 0xFFFFFFFFu);
    }

    void test_id_round_trip() {
        struct case_t {
            std::uint32_t item, meshlet, triangle;
        };
        case_t const cases[] = {{0u, 0u, 0u}, {1u, 0u, 0u}, {0u, 1u, 0u}, {0u, 0u, 1u}, {1023u, 16383u, 127u}, {512u, 4096u, 85u}};
        for (auto const& c : cases) {
            auto const packed = pack_id(c.item, c.meshlet, c.triangle);
            CHECK(packed.has_value());
            CHECK(packed->valid());
            CHECK(packed->item() == c.item);
            CHECK(packed->meshlet_in_item() == c.meshlet);
            CHECK(packed->triangle() == c.triangle);
        }
        // the empty pixel is the one value that decodes to "none"
        CHECK(!id{}.valid());
        CHECK(id::none().value == 0u);
    }

    void test_id_bounds_refuse() {
        // one past each capacity: refused rather than truncated (a truncated id decodes to another triangle)
        CHECK(!pack_id(id_item_capacity, 0u, 0u).has_value());
        CHECK(!pack_id(0u, id_meshlet_capacity, 0u).has_value());
        CHECK(!pack_id(0u, 0u, id_triangle_capacity).has_value());
        CHECK(!pack_id(0xFFFFFFFFu, 0u, 0u).has_value());
        // the mesh-workgroup limit is inside the triangle field (85 emitted triangles per meshlet)
        CHECK(pack_id(0u, 0u, 85u).has_value());
    }

    void test_draw_item_layout() {
        static_assert(draw_item_bytes == 112u);
        static_assert(draw_item_model_offset == 48u);
        static_assert(sizeof(draw_item) == 112u);
        static_assert(offsetof(draw_item, vertex_address_low) == 0u);
        static_assert(offsetof(draw_item, vertex_address_high) == 4u);
        static_assert(offsetof(draw_item, index_address_low) == 8u);
        static_assert(offsetof(draw_item, index_address_high) == 12u);
        static_assert(offsetof(draw_item, first_index) == 16u);
        static_assert(offsetof(draw_item, base_vertex) == 20u);
        static_assert(offsetof(draw_item, index_width) == 24u);
        static_assert(offsetof(draw_item, meshlet_base) == 28u);
        static_assert(offsetof(draw_item, meshlet_table) == 32u);
        static_assert(offsetof(draw_item, material_index) == 36u);
        static_assert(offsetof(draw_item, motion_index) == 40u);
        static_assert(offsetof(draw_item, flags) == 44u);
        static_assert(offsetof(draw_item, model) == 48u);
        // the enum's values are the lane's contract
        static_assert(static_cast<std::uint32_t>(meshlet_source::table) == 0u);
        static_assert(static_cast<std::uint32_t>(meshlet_source::culled) == 1u);
        draw_item const defaults{};
        CHECK(defaults.index_width == 4u);
        CHECK(defaults.flags == 0u);
    }
} // namespace

int main() {
    test_id_layout();
    test_id_round_trip();
    test_id_bounds_refuse();
    test_draw_item_layout();
    return deren::vk_test::finish("test_visibility_buffer");
}
