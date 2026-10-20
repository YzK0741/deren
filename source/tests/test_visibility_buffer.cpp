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
import deren.engine.subtilis;

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
        case_t const cases[] = {{0u, 0u, 0u}, {1u, 0u, 0u}, {0u, 1u, 0u}, {0u, 0u, 1u}, {4095u, 4095u, 127u}, {2048u, 2048u, 85u}};
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
        static_assert(push_block_bytes == 112u);
        static_assert(sizeof(push_block) == push_block_bytes);
        static_assert(offsetof(push_block, model) == 0u);
        static_assert(offsetof(push_block, item_index) == 64u);
        static_assert(offsetof(push_block, frame_slot) == 68u);
        static_assert(offsetof(push_block, geometry) == 80u);
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
        static_assert(id_meshlet_bits == 12u);
        static_assert(id_item_shift == 19u);
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
        CHECK_MSG(contains("visibility_id_item_shift = visibility_id_meshlet_shift + 12u"), "the shader's item shift must stay meshlet_shift + 12");
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

    void test_resolve_indices() {
        draw_item const item{.meshlet_base = 100u};
        auto const packed = pack_id(3u, 5u, 7u);
        CHECK(packed.has_value());
        auto const resolved = resolve_indices(item, packed->value, 400u);
        CHECK(resolved.meshlet_table_index == 105u);
        CHECK(resolved.first_index == 400u + 21u);
        // the meshlet order's field width is the id's own; a maximum-order id stays inside meshlet_base's run
        auto const maximum = pack_id(4095u, 4095u, 85u);
        CHECK(maximum.has_value());
        auto const far = resolve_indices(item, maximum->value, 0u);
        CHECK(far.meshlet_table_index == 100u + 4095u);
        CHECK(far.first_index == 85u * 3u);
    }

    void test_clip_to_screen() {
        auto const close = [](float const a, float const b) { return std::abs(a - b) < 1e-5f; };
        // the clip-space centre lands at the framebuffer centre
        auto const centre = clip_to_screen({0.0f, 0.0f, 0.5f, 1.0f}, 100.0f, 50.0f);
        CHECK(centre.has_value());
        CHECK(close((*centre)[0], 50.0f));
        CHECK(close((*centre)[1], 25.0f));
        // the +x/+y corner at w = 2 is the 3/4 point (y grows DOWN, Vulkan's convention)
        auto const corner = clip_to_screen({1.0f, 1.0f, 0.5f, 2.0f}, 100.0f, 50.0f);
        CHECK(corner.has_value());
        CHECK(close((*corner)[0], 75.0f));
        CHECK(close((*corner)[1], 37.5f));
        // behind the eye: no pixel, no mirrored point
        CHECK(!clip_to_screen({0.0f, 0.0f, 0.5f, 0.0f}, 100.0f, 50.0f).has_value());
        CHECK(!clip_to_screen({1.0f, 2.0f, 0.5f, -1.0f}, 100.0f, 50.0f).has_value());
    }
} // namespace

namespace {
    /// the lines of one shader source, or an empty vector when it cannot be read (the CHECKs report that)
    std::vector<std::string> shader_lines(char const* const relative) {
        std::ifstream file(std::string(VR_TEST_SOURCE_DIR) + "/source/shaders/" + relative);
        CHECK_MSG(file.is_open(), relative);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
        }
        return lines;
    }

    /// the source with every `//` comment stripped, so a text check cannot be satisfied by prose
    std::string strip_comments(std::vector<std::string> const& lines) {
        std::string text;
        for (std::string line : lines) {
            if (std::size_t const comment_at = line.find("//"); comment_at != std::string::npos) {
                line.erase(comment_at);
            }
            text += line;
            text += '\n';
        }
        return text;
    }

    bool text_contains(std::string const& text, char const* const needle) {
        return text.find(needle) != std::string::npos;
    }

    /// the resolve compute stage mirrors the CPU reference's arithmetic AND the draw-item record, and it is the
    /// third hand-mirror of the C++ layout (visibility.slang and the CPU reference are the other two). These checks
    /// pin what a compile cannot: the record's FIELD ORDER, the packing constants, and the compute-stage rules that
    /// would compile inside a body but not inside this leaf (a `discard` or a derivative).
    void test_resolve_shader_mirror() {
        std::vector<std::string> const lines = shader_lines("scene/visibility_resolve.slang");
        std::string const code = strip_comments(lines);
        // ---- the draw-item record, field for field: deren.engine.visibility::draw_item's 12 lanes then the matrix
        std::vector<std::string> fields;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find("struct VisibilityDrawItem") == std::string::npos) {
                continue;
            }
            for (std::size_t k = i + 1; k < lines.size() && lines[k].find('}') == std::string::npos; ++k) {
                std::string declaration = lines[k];
                if (std::size_t const comment_at = declaration.find("//"); comment_at != std::string::npos) {
                    declaration.erase(comment_at);
                }
                // the declaration's NAME is the LAST word of `uint x;` / `VR_MAT4 m;` - the struct has no arrays, and
                // a line without a terminator (the opening brace) is not a declaration
                while (!declaration.empty() && declaration.back() == ' ') {
                    declaration.pop_back();
                }
                if (declaration.empty() || declaration.back() != ';') {
                    continue;
                }
                declaration.pop_back();
                std::size_t const space = declaration.rfind(' ');
                fields.emplace_back(declaration.substr(space == std::string::npos ? 0u : space + 1u));
            }
            break;
        }
        std::vector<std::string> const expected{"vertex_address_low", "vertex_address_high", "index_address_low", "index_address_high",
                                                "first_index", "base_vertex", "index_width", "meshlet_base",
                                                "meshlet_table", "material_index", "motion_index", "flags", "model"};
        CHECK_MSG(fields.size() == expected.size(), "the resolve shader's VisibilityDrawItem must declare 12 lanes + model");
        for (std::size_t i = 0; i < fields.size() && i < expected.size(); ++i) {
            CHECK_MSG(fields[i] == expected[i], (std::string("the resolve shader's draw-item field ") + std::to_string(i) + " is '" + fields[i] + "', expected '" + expected[i] + "'").c_str());
        }
        // ---- the packed id's fields, pinned against the C++ constants
        static_assert(id_triangle_bits == 7u && id_meshlet_bits == 12u && id_item_bits == 12u && id_valid_bits == 1u);
        CHECK_MSG(text_contains(code, "visibility_id_triangle_shift = 0u"), "the resolve shader's triangle shift must stay 0");
        CHECK_MSG(text_contains(code, "visibility_id_meshlet_shift = 7u"), "the resolve shader's meshlet shift must stay 7");
        CHECK_MSG(text_contains(code, "visibility_id_item_shift = 19u"), "the resolve shader's item shift must stay 19");
        CHECK_MSG(text_contains(code, "visibility_id_valid = 1u << 31u"), "the resolve shader's valid bit must stay bit 31");
        CHECK_MSG(text_contains(code, "visibility_id_triangle_mask = 0x7Fu"), "the resolve shader's triangle mask must stay 7 bits");
        CHECK_MSG(text_contains(code, "visibility_id_meshlet_mask = 0xFFFu"), "the resolve shader's meshlet mask must stay 12 bits");
        CHECK_MSG(text_contains(code, "visibility_id_item_mask = 0xFFFu"), "the resolve shader's item mask must stay 12 bits");
        // ---- the item table's LANE ARITHMETIC: one descriptor with the per-frame lanes INSIDE it (measured: adding
        //      the frame slot to the heap slot instead read the next array's storage-image descriptor as a structured
        //      buffer, and the device was lost on the second frame slot)
        static_assert(id_item_capacity == 4096u);
        CHECK_MSG(text_contains(code, "visibility_item_capacity = 4096u"), "the resolve shader's per-frame item stride must stay 4096 rows");
        CHECK_MSG(text_contains(code, "push.frame_slot * visibility_item_capacity + id_item"), "the resolve must index the item table by frame-slot lane, not by heap slot");
        CHECK_MSG(!text_contains(code, "heap_slots_visibility_items + heap_frame_slot"), "the item table is ONE descriptor with per-frame rows, not a per-frame array of descriptors");
        // ---- the COMPUTE rules: no fragment-only statement, and the shared bodies are INCLUDED, not copied
        CHECK_MSG(!text_contains(code, "discard;"), "a compute stage cannot discard: the MASK rule must be the eligibility check's");
        CHECK_MSG(!text_contains(code, "ddx(") && !text_contains(code, "ddy("), "a compute stage has no derivatives: the tangent frame must come from the triangle's edges");
        CHECK_MSG(text_contains(code, "#include \"scene/gbuffer_body.slang\""), "the resolve must include the shared G-buffer composition");
        CHECK_MSG(text_contains(code, "#include \"geometry/mesh_geometry.slang\""), "the resolve must fetch through the shared mesh geometry helpers");
        // ---- the push block's field order (the host appends the two heap index lanes to the pass's own payload)
        std::vector<std::string> push_fields;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find("struct VisibilityResolvePush") == std::string::npos) {
                continue;
            }
            for (std::size_t k = i + 1; k < lines.size() && lines[k].find('}') == std::string::npos; ++k) {
                for (char const* const field : {"frame_slot;", "image_index;", "spare_lane;"}) {
                    if (lines[k].find(field) != std::string::npos) {
                        push_fields.emplace_back(std::string(field, std::string(field).size() - 1u));
                    }
                }
            }
            break;
        }
        CHECK(push_fields.size() == 3u);
        if (push_fields.size() == 3u) {
            CHECK(push_fields[0] == "frame_slot");
            CHECK(push_fields[1] == "image_index");
            CHECK(push_fields[2] == "spare_lane");
        }
        // ---- the two failure counters are the capture lane's witnesses: the resolve increments them
        CHECK_MSG(text_contains(code, "mesh_stat_visibility_depth_mismatch"), "the resolve must report an id/depth disagreement");
        CHECK_MSG(text_contains(code, "mesh_stat_visibility_unresolved"), "the resolve must report a pixel it could not rebuild");
    }

    /// the shared-body extraction (v0.4b): the fragment stage and the resolve must COMPOSE through one body, and the
    /// visibility mesh entry must fetch through the meshlet record (the bug the first capture-only attempt hid).
    void test_shared_gbuffer_body() {
        std::string const gbuffer = strip_comments(shader_lines("scene/gbuffer.slang"));
        CHECK_MSG(text_contains(gbuffer, "#include \"scene/gbuffer_body.slang\""), "gbuffer.slang must include the shared composition");
        CHECK_MSG(text_contains(gbuffer, "gbuffer_from_surface("), "gbuffer.slang must compose through the shared body");
        CHECK_MSG(text_contains(gbuffer, "motion_vector("), "gbuffer.slang must compute its velocity through the shared body");
        CHECK_MSG(!text_contains(gbuffer, "struct gbuffer_output"), "gbuffer_output moved to gbuffer_body.slang: a second declaration is a second contract");
        std::string const body = strip_comments(shader_lines("scene/gbuffer_body.slang"));
        CHECK_MSG(text_contains(body, "struct gbuffer_output"), "gbuffer_body.slang owns the five-target struct");
        CHECK_MSG(text_contains(body, "gbuffer_empty_output("), "gbuffer_body.slang owns the clear value the resolve writes for empty pixels");
        for (char const* const location : {"[[vk::location(0)]]", "[[vk::location(1)]]", "[[vk::location(2)]]", "[[vk::location(3)]]", "[[vk::location(4)]]"}) {
            CHECK_MSG(text_contains(body, location), location);
        }
        std::string const surface = strip_comments(shader_lines("common/surface.slang"));
        CHECK_MSG(text_contains(surface, "#include \"common/surface_body.slang\""), "surface.slang must include the shared decode");
        CHECK_MSG(text_contains(surface, "gather_surface_base("), "the fragment wrapper must call the shared MASK phase first");
        CHECK_MSG(text_contains(surface, "gather_surface_finish("), "the fragment wrapper must call the shared finish phase");
        CHECK_MSG(text_contains(surface, "discard"), "the MASK discard stays in the fragment wrapper");
        // THE ORDER OF THE WRAPPER IS LOAD-BEARING AND WAS MEASURED: the MASK verdict (and its discard) must come
        // BEFORE the derivative frame is requested, and the frame is requested ONLY for a normal-mapped material -
        // a killed invocation stops contributing to its quad's derivative helpers, so the order changed the TBN of
        // normal-mapped alpha-tested foliage and moved 10.5% of the capture gate's sponza pixels when it was wrong.
        {
            std::size_t const base_at = surface.find("gather_surface_base(");
            std::size_t const discard_at = surface.find("discard");
            std::size_t const tbn_at = surface.find("surface_tbn(");
            CHECK_MSG(base_at != std::string::npos && discard_at != std::string::npos && tbn_at != std::string::npos, "the wrapper's three steps must be visible to this test");
            CHECK_MSG(discard_at < tbn_at, "THE MASK DISCARD MUST PRECEDE THE DERIVATIVE FRAME (measured: sponza)");
            CHECK_MSG(surface.find("(base.material.flags & 1u) != 0u") != std::string::npos, "the derivative frame is requested only for a normal-mapped material");
        }
        std::string const decode = strip_comments(shader_lines("common/surface_body.slang"));
        CHECK_MSG(text_contains(decode, "surface_base gather_surface_base("), "surface_body.slang owns the material/MASK phase");
        CHECK_MSG(text_contains(decode, "surface_sample gather_surface_finish("), "surface_body.slang owns the surface decode");
        CHECK_MSG(!text_contains(decode, "discard;"), "the shared decode must not discard: a compute caller cannot");
        CHECK_MSG(!text_contains(decode, "ddx(") && !text_contains(decode, "ddy("), "the shared decode must not use derivatives: the frame is an argument");
        // ---- the visibility mesh entry fetches the MESHLET's window, not the geometry lanes' run base/count
        std::string const visibility = strip_comments(shader_lines("scene/visibility.slang"));
        CHECK_MSG(text_contains(visibility, "meshlet.first_index + t * mesh_indices_per_triangle"), "the id raster must fetch the meshlet record's index window");
        CHECK_MSG(text_contains(visibility, "meshlet.base_vertex"), "the id raster must use the meshlet record's base vertex");
        CHECK_MSG(!text_contains(visibility, "geometry.first_index + (first_triangle + t)"), "the id raster must not use the run window as a mesh window (the v0.3a bug)");
    }

    /// the resolve's front/back decision: Vulkan decides facing by the signed area in FRAMEBUFFER coordinates (y
    /// grows DOWN here) with `frontFace = COUNTER_CLOCKWISE`, so a front face is the NEGATIVE-area triangle.
    void test_front_face_convention() {
        auto const close = [](float const a, float const b) { return std::abs(a - b) < 1e-5f; };
        std::array<float, 2> const origin{0.0f, 0.0f};
        std::array<float, 2> const right{4.0f, 0.0f};
        std::array<float, 2> const down{0.0f, 4.0f};
        // (origin, right, down): turning towards +y (DOWN the screen) is a CLOCKWISE turn as seen, i.e. the back face
        CHECK(close(triangle_twice_area(origin, right, down), 16.0f));
        CHECK(!is_front_face(origin, right, down));
        // ... and the mirrored winding is the front face
        CHECK(close(triangle_twice_area(origin, down, right), -16.0f));
        CHECK(is_front_face(origin, down, right));
        // a degenerate triangle covered no pixel: "front" would be a meaningless answer
        CHECK(!is_front_face(origin, right, std::array<float, 2>{8.0f, 0.0f}));
    }

    /// THE FRAME'S ADMISSION AND LANE DECISION (visibility-buffer v0.5): the switch and the per-leaf clauses fold
    /// into ONE lane answer, and "off" and "cannot carry" MUST agree - both mean the old geometry lane for every
    /// leaf. The runtime's fallback and its one log line read exactly these two functions, so every refusal is
    /// pinned by name, and so is the truth table that would otherwise be four hand-written ifs.
    void test_frame_admission_and_lane() {
        // the clauses are ordered (first refusal wins), and each one is a way the per-item resolve record would be wrong
        CHECK(admitted(admit_leaf(leaf_admission_facts{})));
        auto const refused = [](auto&& mutate) {
            leaf_admission_facts facts{};
            mutate(facts);
            return admit_leaf(facts);
        };
        CHECK(refused([](leaf_admission_facts& f) { f.default_pipeline = false; }) == admission::custom_pipeline);
        CHECK(refused([](leaf_admission_facts& f) { f.instanced = true; }) == admission::instanced);
        CHECK(refused([](leaf_admission_facts& f) { f.deforming = true; }) == admission::deforming);
        CHECK(refused([](leaf_admission_facts& f) { f.has_vertex_buffer = false; }) == admission::missing_geometry);
        CHECK(refused([](leaf_admission_facts& f) { f.has_index_buffer = false; }) == admission::missing_geometry);
        CHECK(refused([](leaf_admission_facts& f) { f.vertex_stride = 32; }) == admission::incompatible_layout);
        CHECK(refused([](leaf_admission_facts& f) { f.has_meshlets = false; }) == admission::no_meshlet_run);
        // the MATERIAL clauses: a MASK material cannot be resolved (a written depth cannot be discarded), and an
        // index outside the host table would read a zero flag word and be carried as if it were a plain surface
        CHECK(refused([](leaf_admission_facts& f) { f.material_index_valid = false; }) == admission::missing_material);
        // a MASK material is CARRIED now: its cut-out is the raster fragment's discard before the depth write
        // (item 2), so the frame no longer falls back for it
        {
            leaf_admission_facts masked{};
            masked.material_flags = 16u;
            CHECK(admitted(admit_leaf(masked)));
        }
        CHECK(refused([](leaf_admission_facts& f) { f.meshlet_count = id_meshlet_capacity + 1u; }) == admission::meshlet_overflow);
        CHECK(refused([](leaf_admission_facts& f) { f.item_index = id_item_capacity; }) == admission::item_overflow);
        // THE TWO-SIDED FLAG IS NOT A CLAUSE (the raster lane culls per item, like the fragment path): a doubled-sided
        // material must be carried, not refused
        {
            leaf_admission_facts two_sided{};
            two_sided.material_flags = 8u; // doubleSided, no MASK
            CHECK(admitted(admit_leaf(two_sided)));
        }
        // the biggest LEGAL values still take the path: the overflow checks are off-by-one-sensitive
        leaf_admission_facts edge{};
        edge.meshlet_count = id_meshlet_capacity;
        edge.item_index = id_item_capacity - 1u;
        CHECK(admitted(admit_leaf(edge)));
        // THE LANE TRUTH TABLE: the switch alone is NOT enough (a refused frame still records the old lane) and an
        // admitted frame with the switch off records it too - the two inputs are independent, the answer is one lane.
        CHECK(choose_opaque_lane(true, admission::admitted) == opaque_lane::visibility);
        CHECK(choose_opaque_lane(false, admission::admitted) == opaque_lane::geometry);
        std::array const refusals{admission::disabled, admission::no_pipeline, admission::no_scene, admission::no_draws, admission::depth_equal_stage,
                                  admission::custom_pipeline, admission::instanced, admission::deforming, admission::missing_geometry, admission::no_meshlet_run,
                                  admission::missing_material, admission::meshlet_overflow, admission::item_overflow};
        for (admission const refusal : refusals) {
            CHECK_MSG(choose_opaque_lane(true, refusal) == opaque_lane::geometry, "a refusal must record the geometry lane");
            CHECK_MSG(!describe(refusal).empty(), "every refusal has a name for the one fallback line");
        }
        CHECK(!describe(admission::admitted).empty());
    }
} // namespace

namespace {
    void test_hzb_run_publication() {
        draw_item row{};
        push_block push{};
        push.geometry.index_width = 2;
        using deren::engine::subtilis::run_source;
        CHECK(static_cast<unsigned>(meshlet_source::hzb) == 2u);
        CHECK(apply_hzb_run(row, push, {27, 12, run_source::visible}));
        CHECK(row.meshlet_table == 2 && push.geometry.base_vertex == 2);
        CHECK(row.meshlet_base == 27 && push.geometry.first_index == 27 && push.geometry.index_count == 12);
        CHECK(push.geometry.index_width == 2);
        CHECK(!apply_hzb_run(row, push, {0, 12, run_source::shared}));
        CHECK(!apply_hzb_run(row, push, {0, 0, run_source::visible}));
        CHECK(!apply_hzb_run(row, push, {65530, 12, run_source::visible}));
        CHECK(!apply_hzb_run(row, push, {0, id_meshlet_capacity + 1, run_source::visible}));
        CHECK(row.meshlet_base == 27 && push.geometry.index_count == 12); // failed promotion changes nothing
    }
} // namespace
int main() {
    test_hzb_run_publication();
    test_id_layout();
    test_id_round_trip();
    test_id_bounds_refuse();
    test_draw_item_layout();
    test_push_block_layout();
    test_draw_item_builder();
    test_shader_mirror();
    test_resolve_shader_mirror();
    test_shared_gbuffer_body();
    test_front_face_convention();
    test_frame_admission_and_lane();
    test_resolve_barycentrics();
    test_resolve_perspective_and_interpolation();
    test_clip_to_screen();
    test_resolve_indices();
    return deren::vk_test::finish("test_visibility_buffer");
}
