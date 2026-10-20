module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <span>
module deren.engine.subtilis.builder;

namespace deren::engine::subtilis::builder_detail {
    namespace {
        using pair = std::array<std::uint32_t, 2>;
        using edge = std::array<std::uint32_t, 4>; // canonical ends, triangle, direction
        struct triangle_graph {
            vector<edge> edges;
            vector<std::uint32_t> parent;
            vector<std::uint8_t> unsafe;
            std::uint32_t root(std::uint32_t id) {
                while (parent[id] != id) {
                    parent[id] = parent[parent[id]];
                    id = parent[id];
                }
                return id;
            }
        };
        triangle_graph graph(source const& data, topology const& t, std::span<std::uint32_t const> indices) {
            triangle_graph out;
            auto count = static_cast<std::uint32_t>(indices.size() / 3);
            out.parent.resize(count);
            std::iota(out.parent.begin(), out.parent.end(), 0u);
            out.unsafe.resize(count, 0);
            out.edges.reserve(indices.size());
            vector<std::array<std::uint32_t, 4>> faces;
            faces.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                auto first = std::size_t(i) * 3;
                std::array<std::uint32_t, 3> corners{t.class_of[indices[first]], t.class_of[indices[first + 1]], t.class_of[indices[first + 2]]};
                for (unsigned k = 0; k < 3; ++k) {
                    auto a = corners[k];
                    auto b = corners[(k + 1) % 3];
                    if (a == b) {
                        out.unsafe[i] = 1;
                    }
                    out.edges.push_back({std::min(a, b), std::max(a, b), i, a < b ? 1u : 0u});
                }
                auto const& a = data.positions[indices[first]];
                auto const& b = data.positions[indices[first + 1]];
                auto const& c = data.positions[indices[first + 2]];
                std::array<double, 3> ab{};
                std::array<double, 3> ac{};
                for (unsigned k = 0; k < 3; ++k) {
                    ab[k] = double(b[k]) - a[k];
                    ac[k] = double(c[k]) - a[k];
                }
                if (std::hypot(ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]) == 0) {
                    out.unsafe[i] = 1;
                }
                std::sort(corners.begin(), corners.end());
                faces.push_back({corners[0], corners[1], corners[2], i});
            }
            std::sort(faces.begin(), faces.end());
            for (std::size_t i = 1; i < faces.size(); ++i) {
                if (std::equal(faces[i].begin(), faces[i].begin() + 3, faces[i - 1].begin())) {
                    out.unsafe[faces[i][3]] = out.unsafe[faces[i - 1][3]] = 1;
                }
            }
            std::sort(out.edges.begin(), out.edges.end());
            for (std::size_t first = 0; first < out.edges.size();) {
                std::size_t end = first + 1;
                while (end < out.edges.size() && out.edges[first][0] == out.edges[end][0] && out.edges[first][1] == out.edges[end][1]) {
                    ++end;
                }
                if (end - first > 2 || (end - first == 2 && out.edges[first][3] == out.edges[first + 1][3])) {
                    for (std::size_t i = first; i < end; ++i) {
                        out.unsafe[out.edges[i][2]] = 1;
                    }
                }
                for (std::size_t i = first + 1; i < end; ++i) {
                    auto a = out.root(out.edges[first][2]);
                    auto b = out.root(out.edges[i][2]);
                    out.parent[std::max(a, b)] = std::min(a, b);
                }
                first = end;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                out.parent[i] = out.root(i);
            }
            return out;
        }
        vector<pair> boundary(triangle_graph const& g) {
            vector<pair> result;
            for (std::size_t first = 0; first < g.edges.size();) {
                std::size_t end = first + 1;
                while (end < g.edges.size() && g.edges[first][0] == g.edges[end][0] && g.edges[first][1] == g.edges[end][1]) {
                    ++end;
                }
                if (end - first == 1) {
                    auto const& e = g.edges[first];
                    result.push_back(e[3] ? pair{e[0], e[1]} : pair{e[1], e[0]});
                }
                first = end;
            }
            std::sort(result.begin(), result.end());
            return result;
        }
    } // namespace
    topology analyze_topology(source const& data) {
        topology result;
        vector<std::uint32_t> order(data.positions.size());
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&](auto a, auto b) { return data.positions[a] != data.positions[b] ? data.positions[a] < data.positions[b] : a < b; });
        result.class_of.resize(order.size());
        result.seams.resize(order.size(), 0);
        std::uint32_t representative = 0;
        for (std::size_t i = 0; i < order.size(); ++i) {
            auto id = order[i];
            if (!i || data.positions[id] != data.positions[order[i - 1]]) {
                representative = id;
            }
            result.class_of[id] = representative;
            if (data.attributes[id] != data.attributes[representative]) {
                result.seams[representative] = 1;
            }
        }
        result.unsafe_triangles = graph(data, result, data.indices).unsafe;
        return result;
    }
    region source_region(source const& data, topology const& t, std::span<std::uint32_t const> triangle_ids) {
        region result;
        vector<std::uint32_t> indices;
        indices.reserve(triangle_ids.size() * 3);
        for (auto id : triangle_ids) {
            if (t.unsafe_triangles[id]) {
                return result;
            }
            auto triangle = std::span(data.indices).subspan(std::size_t(id) * 3, 3);
            indices.insert(indices.end(), triangle.begin(), triangle.end());
        }
        auto g = graph(data, t, indices);
        if (std::find(g.unsafe.begin(), g.unsafe.end(), 1) != g.unsafe.end()) {
            return result;
        }
        result.boundary = boundary(g);
        for (auto const& edge : result.boundary) {
            result.boundary_classes.push_back(edge[0]);
            result.boundary_classes.push_back(edge[1]);
        }
        std::sort(result.boundary_classes.begin(), result.boundary_classes.end());
        result.boundary_classes.erase(std::unique(result.boundary_classes.begin(), result.boundary_classes.end()), result.boundary_classes.end());
        for (std::size_t i = 0; i < indices.size(); ++i) {
            result.class_components.push_back({t.class_of[indices[i]], g.parent[i / 3]});
        }
        std::sort(result.class_components.begin(), result.class_components.end());
        result.class_components.erase(std::unique(result.class_components.begin(), result.class_components.end()), result.class_components.end());
        for (std::size_t i = 1; i < result.class_components.size(); ++i) {
            if (result.class_components[i][0] == result.class_components[i - 1][0]) {
                return result;
            } // pinched components
        }
        result.components = g.parent;
        std::sort(result.components.begin(), result.components.end());
        result.components.erase(std::unique(result.components.begin(), result.components.end()), result.components.end());
        result.valid = true;
        return result;
    }
    bool preserves_region(source const& data, topology const& t, region const& original, std::span<std::uint32_t const> indices) {
        if (!original.valid || indices.empty() || indices.size() % 3) {
            return false;
        }
        auto candidate = graph(data, t, indices);
        if (std::find(candidate.unsafe.begin(), candidate.unsafe.end(), 1) != candidate.unsafe.end() || boundary(candidate) != original.boundary) {
            return false;
        }
        vector<std::uint32_t> components;
        for (std::size_t triangle = 0; triangle < indices.size(); triangle += 3) {
            std::array<std::uint32_t, 3> owners{};
            for (unsigned k = 0; k < 3; ++k) {
                auto id = t.class_of[indices[triangle + k]];
                auto found = std::lower_bound(original.class_components.begin(), original.class_components.end(), id, [](pair const& entry, auto key) { return entry[0] < key; });
                if (found == original.class_components.end() || (*found)[0] != id) {
                    return false;
                }
                owners[k] = (*found)[1];
            }
            if (owners[0] != owners[1] || owners[0] != owners[2]) {
                return false;
            }
            components.push_back(owners[0]);
        }
        std::sort(components.begin(), components.end());
        components.erase(std::unique(components.begin(), components.end()), components.end());
        return components == original.components;
    }
} // namespace deren::engine::subtilis::builder_detail
