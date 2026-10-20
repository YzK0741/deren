#include "vk_test.h"

#include <array>
#include <cstring>
#include <limits>
#include <map>
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
    auto root_indices(sub::built_geometry const& built) {
        deren::utility::vessel::vector<std::uint32_t> result;
        for (auto root : built.hierarchy().roots()) {
            auto range = built.hierarchy().groups()[root].record.coarse;
            for (unsigned i = 0; i < range.count; ++i) {
                auto record = built.hierarchy().clusters()[range.first + i];
                auto window = built.indices().subspan(record.first_index, record.index_count);
                result.insert(result.end(), window.begin(), window.end());
            }
        }
        return result;
    }
    auto boundary(std::span<std::uint32_t const> indices) {
        deren::utility::vessel::map<std::array<std::uint32_t, 2>, int> result;
        for (std::size_t t = 0; t < indices.size(); t += 3) {
            for (unsigned edge = 0; edge < 3; ++edge) {
                auto a = indices[t + edge], b = indices[t + (edge + 1) % 3];
                result[{std::min(a, b), std::max(a, b)}] += a < b ? 1 : -1;
            }
        }
        for (auto it = result.begin(); it != result.end();) {
            if (!it->second) {
                it = result.erase(it);
            } else {
                ++it;
            }
        }
        return result;
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
    // Real reduction: the production builder must not pass by always returning leaves.
    constexpr unsigned side = 33;
    deren::utility::vessel::vector<vertex> grid;
    deren::utility::vessel::vector<std::uint32_t> triangles;
    for (unsigned y = 0; y < side; ++y) {
        for (unsigned x = 0; x < side; ++x) {
            grid.push_back({{float(x), float(y), 0}, {0, 0, 1}, {float(x) / (side - 1), float(y) / (side - 1)}});
        }
    }
    for (unsigned y = 0; y + 1 < side; ++y) {
        for (unsigned x = 0; x + 1 < side; ++x) {
            auto a = y * side + x, b = a + 1, c = a + side, d = c + 1;
            for (auto value : {a, b, c, b, d, c}) {
                triangles.push_back(value);
            }
        }
    }
    sub::build_input grid_input{{bytes(std::span(grid)), sizeof(vertex), static_cast<std::uint32_t>(grid.size()),
                                 bytes(std::span(triangles)), 4, 0, static_cast<std::uint32_t>(triangles.size()), 0}};
    auto hierarchy = sub::build(grid_input);
    CHECK(hierarchy);
    if (hierarchy) {
        CHECK(hierarchy->statistics().depth >= 1);
        CHECK(hierarchy->statistics().coarse_triangles < hierarchy->statistics().lod0_triangles);
        CHECK(hierarchy->indices().size() > triangles.size());
        CHECK(std::memcmp(hierarchy->indices().data(), triangles.data(), triangles.size() * 4) == 0);
        std::uint32_t root_records = 0;
        for (auto root : hierarchy->hierarchy().roots()) {
            root_records += hierarchy->hierarchy().groups()[root].record.coarse.count;
        }
        CHECK(root_records < hierarchy->hierarchy().lod0_count());
        CHECK(boundary(root_indices(*hierarchy)) == boundary(triangles));
        deren::vk_test::write_line("grid: {} -> {} triangles, {} -> {} records, depth {}",
                                   hierarchy->statistics().lod0_triangles, hierarchy->statistics().coarse_triangles,
                                   hierarchy->hierarchy().lod0_count(), root_records, hierarchy->statistics().depth);
    }
    // A separate closed island must survive even though its boundary is empty.
    auto island_vertices = grid;
    auto island_indices = triangles;
    auto offset = static_cast<std::uint32_t>(island_vertices.size());
    for (auto p : {std::array<float, 3>{100, 0, 0}, {101, 0, 0}, {100, 1, 0}, {100, 0, 1}}) {
        island_vertices.push_back({p, {0, 0, 1}, {0, 0}});
    }
    for (auto index : {0u, 2u, 1u, 0u, 1u, 3u, 0u, 3u, 2u, 1u, 2u, 3u}) {
        island_indices.push_back(offset + index);
    }
    auto island_input = grid_input;
    island_input.geometry.vertex_data = bytes(std::span(island_vertices));
    island_input.geometry.vertex_count = static_cast<std::uint32_t>(island_vertices.size());
    island_input.geometry.index_data = bytes(std::span(island_indices));
    island_input.geometry.index_count = static_cast<std::uint32_t>(island_indices.size());
    auto island = sub::build(island_input);
    CHECK(island);
    if (island) {
        auto coarse = root_indices(*island);
        unsigned surviving = 0;
        bool unmixed = true;
        for (std::size_t i = 0; i < coarse.size(); i += 3) {
            unsigned count = (coarse[i] >= offset) + (coarse[i + 1] >= offset) + (coarse[i + 2] >= offset);
            if (count == 3) {
                ++surviving;
            }
            unmixed = unmixed && (count == 0 || count == 3);
        }
        CHECK(surviving >= 4 && unmixed);
        CHECK(boundary(coarse) == boundary(island_indices));
    }
    // Degenerate triangles are preserved in an unchanged finest branch, not filtered away.
    auto bad_indices = triangles;
    bad_indices.insert(bad_indices.end(), {0, 0, 0});
    auto degenerate_input = grid_input;
    degenerate_input.geometry.index_data = bytes(std::span(bad_indices));
    degenerate_input.geometry.index_count = static_cast<std::uint32_t>(bad_indices.size());
    auto degenerate = sub::build(degenerate_input);
    CHECK(degenerate && degenerate->statistics().rejected_topology > 0);
    if (degenerate) {
        auto coarse = root_indices(*degenerate);
        unsigned preserved = 0;
        for (std::size_t i = 0; i < coarse.size(); i += 3) {
            preserved += coarse[i] == 0 && coarse[i + 1] == 0 && coarse[i + 2] == 0;
        }
        CHECK(preserved == 1);
    }
    // Duplicate the middle column on its right side: UV and normal discontinuities
    // must retain both copies of every seam edge, including at mixed LODs.
    auto seam_vertices = grid;
    auto seam_indices = triangles;
    constexpr unsigned middle = side / 2;
    for (unsigned y = 0; y < side; ++y) {
        auto copy = grid[y * side + middle];
        copy.normal = {0, 1, 0};
        copy.uv[0] += 1;
        seam_vertices.push_back(copy);
    }
    for (std::size_t t = 0; t < seam_indices.size(); t += 3) {
        bool right = false;
        for (unsigned k = 0; k < 3; ++k) {
            right = right || seam_indices[t + k] % side > middle;
        }
        if (right) {
            for (unsigned k = 0; k < 3; ++k) {
                auto& id = seam_indices[t + k];
                if (id % side == middle) {
                    id = side * side + id / side;
                }
            }
        }
    }
    auto seam_input = grid_input;
    seam_input.geometry.vertex_data = bytes(std::span(seam_vertices));
    seam_input.geometry.vertex_count = static_cast<std::uint32_t>(seam_vertices.size());
    seam_input.geometry.index_data = bytes(std::span(seam_indices));
    auto seam = sub::build(seam_input);
    CHECK(seam && seam->statistics().depth > 0);
    if (seam) {
        CHECK(boundary(root_indices(*seam)) == boundary(seam_indices));
    }
    // A duplicate face makes three triangles share an edge. It must remain intact.
    auto nonmanifold_indices = triangles;
    nonmanifold_indices.insert(nonmanifold_indices.end(), triangles.begin(), triangles.begin() + 3);
    auto nonmanifold_input = grid_input;
    nonmanifold_input.geometry.index_data = bytes(std::span(nonmanifold_indices));
    nonmanifold_input.geometry.index_count = static_cast<std::uint32_t>(nonmanifold_indices.size());
    auto nonmanifold = sub::build(nonmanifold_input);
    CHECK(nonmanifold && nonmanifold->statistics().rejected_topology > 0);
    if (nonmanifold) {
        auto coarse = root_indices(*nonmanifold);
        unsigned copies = 0;
        for (std::size_t i = 0; i < coarse.size(); i += 3) {
            copies += std::equal(coarse.begin() + i, coarse.begin() + i + 3, triangles.begin());
        }
        CHECK(copies == 2 && boundary(coarse) == boundary(nonmanifold_indices));
    }
    return deren::vk_test::finish("test_subtilis_builder");
}
