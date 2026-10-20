#include "vk_test.h"

#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
import deren.engine.subtilis.builder;
import deren.engine.subtilis.asset;
import deren.engine.meshlet;
import deren.utility.shared;
namespace sub = deren::engine::subtilis;
namespace {
    struct vertex {
        std::array<float, 3> position, normal;
        std::array<float, 2> uv;
    };
    template <class T, std::size_t N>
    auto bytes(std::span<T, N> data) {
        return std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const*>(data.data()), data.size_bytes()};
    }
} // namespace
int main() {
    auto empty = sub::build({});
    CHECK(empty && empty->indices().empty() && empty->hierarchy().groups().empty());
    std::array<vertex, 4> vertices{{{{0, 0, 0}, {0, 0, 1}, {0, 0}}, {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{0, 1, 0}, {0, 0, 1}, {0, 1}}, {{1, 1, 0}, {0, 0, 1}, {1, 1}}}};
    std::array<std::uint32_t, 3> indices{0, 1, 2};
    sub::build_input input{{bytes(std::span(vertices)), sizeof(vertex), 4, bytes(std::span(indices)), 4, 0, 3, 0}};
    auto built = sub::build(input);
    CHECK(built);
    if (built) {
        CHECK(built->indices().size() == 3 && built->indices()[2] == 2);
        CHECK(built->vertices().size() == sizeof(vertices));
        CHECK(built->vertex_count() == 4 && built->vertex_stride() == sizeof(vertex) && built->lod0_index_count() == 3);
        CHECK(built->hierarchy().lod0_count() == 1 && built->hierarchy().maximum_cut() == 1);
        CHECK(built->hierarchy().clusters()[0].base_vertex == 0);
        vertices[0].position[0] = 99;
        CHECK(std::memcmp(built->vertices().data(), vertices.data(), sizeof(vertices)) != 0);
        vertices[0].position[0] = 0;
        auto moved = std::move(*built);
        CHECK(moved.indices().size() == 3 && built->indices().empty() && built->vertex_count() == 0);
    }
    std::array<std::uint16_t, 6> short_indices{99, 99, 99, 0, 1, 2};
    input.geometry.index_data = bytes(std::span(short_indices));
    input.geometry.index_width = 2;
    input.geometry.first_index = 3;
    input.geometry.base_vertex = 1;
    built = sub::build(input);
    CHECK(built && built->indices()[0] == 1 && built->indices()[2] == 3);
    // The input may be byte-aligned only; decoding must use memcpy.
    deren::utility::vessel::vector<std::uint8_t> unaligned(sizeof(vertices) + 1);
    std::memcpy(unaligned.data() + 1, vertices.data(), sizeof(vertices));
    input.geometry.vertex_data = std::span(unaligned).subspan(1);
    built = sub::build(input);
    CHECK(built && built->indices()[0] == 1);
    input.geometry.vertex_data = bytes(std::span(vertices));
    auto rejects = [&](sub::build_error expected) {
        auto bad = sub::build(input);
        CHECK(!bad && bad.error() == expected);
    };
    input.geometry.base_vertex = -1;
    rejects(sub::build_error::invalid_input);
    input.geometry.base_vertex = 2;
    rejects(sub::build_error::invalid_input);
    input.geometry.base_vertex = 0;
    input.geometry.first_index = std::numeric_limits<std::uint32_t>::max();
    rejects(sub::build_error::invalid_input);
    input.geometry.first_index = 3;
    input.geometry.index_width = 1;
    rejects(sub::build_error::invalid_input);
    input.geometry.index_width = 2;
    input.geometry.index_count = 2;
    rejects(sub::build_error::invalid_input);
    input.geometry.index_count = 3;
    input.geometry.vertex_count = std::numeric_limits<std::uint32_t>::max();
    rejects(sub::build_error::invalid_input);
    input.geometry.vertex_count = 4;
    input.normal_offset = sizeof(vertex) - 4;
    rejects(sub::build_error::invalid_input);
    input.normal_offset = 12;
    vertices[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    rejects(sub::build_error::invalid_geometry);
    vertices[0].position[0] = 0;
    vertices[1].uv[0] = std::numeric_limits<float>::infinity();
    rejects(sub::build_error::invalid_geometry);
    vertices[1].uv[0] = 1;
    sub::build_options options;
    options.target_ratio = 1;
    auto bad = sub::build(input, options);
    CHECK(!bad && bad.error() == sub::build_error::invalid_options);
    options = {};
    options.group_size = 1;
    bad = sub::build(input, options);
    CHECK(!bad && bad.error() == sub::build_error::invalid_options);
    options = {};
    options.max_output_bytes = 1;
    bad = sub::build(input, options);
    CHECK(!bad && bad.error() == sub::build_error::capacity);
    bad = sub::build({}, options);
    CHECK(!bad && bad.error() == sub::build_error::capacity);
    return deren::vk_test::finish("test_subtilis_builder");
}
