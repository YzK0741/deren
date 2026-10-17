// Headless unit tests: the visibility buffer's CPU-side vocabulary ==============
// The packed id and the draw-item record are mirrored BY HAND in the geometry raster's mesh/fragment shader
// and in the resolve compute shader (a shader cannot include a C++ enum). These tests are the C++ half of
// that mirror: the bit arithmetic, the capacities, the exact record size and the field offsets.
#include "vk_test.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

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

namespace {
    void test_push_block_layout() {
        static_assert(push_block_bytes == 104u);
        static_assert(sizeof(push_block) == push_block_bytes);
        static_assert(offsetof(push_block, model) == 0u);
        static_assert(offsetof(push_block, item_index) == 64u);
        static_assert(offsetof(push_block, frame_slot) == 68u);
        static_assert(offsetof(push_block, geometry) == 72u);
        static_assert(sizeof(geometry_lanes) == 32u);
        static_assert(offsetof(geometry_lanes, first_index) == 16u);
        push_block const defaults{};
        CHECK(defaults.geometry.index_width == 4u);
        CHECK(defaults.item_index == 0u);
    }

    void test_draw_item_builder() {
        std::array<float, 16> const model{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 4.0f, 5.0f, 6.0f, 1.0f};
        draw_item const item = make_draw_item(0x1122334455667788ull, 0x99AABBCCDDEEFF00ull, 17u, -3, 2u, 4096u, 7u, 11u, model);
        // the 64-bit addresses are split little-endian: low half first, high half second
        CHECK(item.vertex_address_low == 0x55667788u);
        CHECK(item.vertex_address_high == 0x11223344u);
        CHECK(item.index_address_low == 0xDDEEFF00u);
        CHECK(item.index_address_high == 0x99AABBCCu);
        CHECK(item.first_index == 17u);
        CHECK(item.base_vertex == -3);
        CHECK(item.index_width == 2u);
        CHECK(item.meshlet_base == 4096u);
        CHECK(item.meshlet_table == static_cast<std::uint32_t>(meshlet_source::table));
        CHECK(item.material_index == 7u);
        CHECK(item.motion_index == 11u);
        CHECK(item.flags == 0u);
        CHECK(item.model == model);
    }

    /// the shader mirrors the C++ layout BY HAND, so the test reads the shader and pins what it says - the
    /// same two-sided rule the heap-slot grid's test follows (a drift is a wrong picture, not a build error).
    void test_shader_mirror() {
        std::ifstream file(VR_TEST_SOURCE_DIR "/source/shaders/scene/visibility.slang");
        CHECK_MSG(file.is_open(), "visibility.slang must exist");
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
        }
        bool saw_push_struct = false;
        std::vector<std::string> fields;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find("struct VisibilityPush") != std::string::npos) {
                saw_push_struct = true;
                for (std::size_t k = i + 1; k < lines.size() && lines[k].find('}') == std::string::npos; ++k) {
                    // the DECLARATION only: strip the line's comment, then look for a declaration spelling
                    std::string declaration = lines[k];
                    if (std::size_t const comment_at = declaration.find("//"); comment_at != std::string::npos) {
                        declaration.erase(comment_at);
                    }
                    for (char const* const field : {"model;", "item_index;", "frame_slot;", "geometry;"}) {
                        if (declaration.find(field) != std::string::npos) {
                            fields.emplace_back(std::string(field, std::string(field).size() - 1u));
                        }
                    }
                }
                break;
            }
        }
        CHECK_MSG(saw_push_struct, "the shader must declare struct VisibilityPush");
        CHECK(fields.size() == 4u);
        if (fields.size() == 4u) {
            CHECK(fields[0] == "model");
            CHECK(fields[1] == "item_index");
            CHECK(fields[2] == "frame_slot");
            CHECK(fields[3] == "geometry");
        }
        // and the id's packing constants, on both sides: the C++ side is static_asserted below
        static_assert(id_meshlet_shift == 7u);
        static_assert(id_meshlet_bits == 14u);
        static_assert(id_item_shift == 21u);
        static_assert(id_valid_shift == 31u);
        auto const contains = [&lines](char const* const needle) {
            for (auto const& text : lines) {
                if (text.find(needle) != std::string::npos) {
                    return true;
                }
            }
            return false;
        };
        CHECK_MSG(contains("visibility_id_meshlet_shift = 7u"), "the shader's meshlet shift must stay 7");
        CHECK_MSG(contains("visibility_id_item_shift = visibility_id_meshlet_shift + 14u"), "the shader's item shift must stay meshlet_shift + 14");
        CHECK_MSG(contains("visibility_id_valid = 1u << 31u"), "the shader's valid bit must stay bit 31");
    }
} // namespace

namespace {
    void test_resolve_barycentrics() {
        std::array<float, 2> const pa{0.0f, 0.0f};
        std::array<float, 2> const pb{4.0f, 0.0f};
        std::array<float, 2> const pc{0.0f, 4.0f};
        auto const close = [](float const a, float const b) { return std::abs(a - b) < 1e-5f; };
        // the centroid is (1/3, 1/3, 1/3)
        auto const centre = screen_barycentrics({4.0f / 3.0f, 4.0f / 3.0f}, pa, pb, pc);
        CHECK(close(centre.a, 1.0f / 3.0f));
        CHECK(close(centre.b, 1.0f / 3.0f));
        CHECK(close(centre.c, 1.0f / 3.0f));
        // a vertex is itself: weight one, the other two zero
        auto const at_b = screen_barycentrics(pb, pa, pb, pc);
        CHECK(close(at_b.a, 0.0f));
        CHECK(close(at_b.b, 1.0f));
        CHECK(close(at_b.c, 0.0f));
        // the weights sum to one everywhere (signed, outside included)
        auto const outside = screen_barycentrics({8.0f, 8.0f}, pa, pb, pc);
        CHECK(close(outside.a + outside.b + outside.c, 1.0f));
        // a degenerate triangle is defined as "no triangle", not a division by zero
        auto const degenerate = screen_barycentrics({1.0f, 1.0f}, pa, pb, {8.0f, 0.0f});
        CHECK(degenerate.a == 0.0f);
        CHECK(degenerate.b == 0.0f);
        CHECK(degenerate.c == 0.0f);
    }

    void test_resolve_perspective_and_interpolation() {
        auto const close = [](float const a, float const b) { return std::abs(a - b) < 1e-5f; };
        barycentrics const centre{1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f};
        // equal w: the screen weights ARE the object weights
        auto const uniform = perspective_correct(centre, 2.0f, 2.0f, 2.0f);
        CHECK(close(uniform.a, 1.0f / 3.0f));
        // w = (1, 2, 4): raw ratios (1, 1/2, 1/4) normalized
        auto const corrected = perspective_correct(centre, 1.0f, 2.0f, 4.0f);
        CHECK(close(corrected.a, (1.0f / 1.0f) / (1.0f + 0.5f + 0.25f)));
        CHECK(close(corrected.a + corrected.b + corrected.c, 1.0f));
        // a zero w collapses to "no weights" rather than inf/NaN
        auto const collapsed = perspective_correct(centre, 0.0f, 1.0f, 1.0f);
        CHECK(collapsed.a == 0.0f && collapsed.b == 0.0f && collapsed.c == 0.0f);
        // interpolation hits the vertex values at the vertices
        std::array<float, 3> const va{1.0f, 2.0f, 3.0f};
        std::array<float, 3> const vb{5.0f, 7.0f, 11.0f};
        std::array<float, 3> const vc{-1.0f, 0.0f, 2.0f};
        auto const value = interpolate3(barycentrics{0.0f, 1.0f, 0.0f}, va, vb, vc);
        CHECK(close(value[0], 5.0f));
        CHECK(close(value[1], 7.0f));
        CHECK(close(value[2], 11.0f));
        auto const mid = interpolate2(barycentrics{0.5f, 0.5f, 0.0f}, {2.0f, 4.0f}, {6.0f, 8.0f}, {0.0f, 0.0f});
        CHECK(close(mid[0], 4.0f));
        CHECK(close(mid[1], 6.0f));
    }
} // namespace

int main() {
    test_id_layout();
    test_id_round_trip();
    test_id_bounds_refuse();
    test_draw_item_layout();
    test_push_block_layout();
    test_draw_item_builder();
    test_shader_mirror();
    test_resolve_barycentrics();
    test_resolve_perspective_and_interpolation();
    return deren::vk_test::finish("test_visibility_buffer");
}
