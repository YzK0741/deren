module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <span>
module deren.asset.subtilis.builder;
namespace deren::asset::subtilis::builder_detail {
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
        triangle_graph graph(source_geometry const& data, topology const& t, std::span<std::uint32_t const> indices, bool attributes = false) {
            auto const& classes = attributes ? t.attribute_class : t.position_class;
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
                std::array<std::uint32_t, 3> corners{classes[indices[first]], classes[indices[first + 1]], classes[indices[first + 2]]};
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

    topology analyze_topology(source_geometry const& data) {
        topology t;
        if (!source_digest(data))
            return t;
        vector<uint32_t> order(data.positions.size());
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&](auto a, auto b) { return data.positions[a] != data.positions[b] ? data.positions[a] < data.positions[b] : a < b; });
        t.position_class.resize(order.size());
        t.attribute_class.resize(order.size());
        t.seams.resize(order.size(), 0);
        t.unsafe_vertices.resize(order.size(), 0);
        vector<bool> used(order.size(), false);
        for (auto id : data.indices)
            used[id] = true;
        auto attrs = [&](uint32_t id) {auto n=data.normals[id];auto uv=data.uv0[id];return array<float,5>{n[0],n[1],n[2],uv[0],uv[1]}; };
        uint32_t representative = 0, first_used = none;
        for (size_t i = 0; i < order.size(); ++i) {
            auto id = order[i];
            if (!i || data.positions[id] != data.positions[order[i - 1]]) {
                representative = id;
                first_used = none;
            }
            t.position_class[id] = representative;
            if (used[id]) {
                if (first_used == none)
                    first_used = id;
                else if (attrs(id) != attrs(first_used))
                    t.seams[representative] = 1;
            }
        }
        std::sort(order.begin(), order.end(), [&](auto a, auto b) {if(data.positions[a]!=data.positions[b])return data.positions[a]<data.positions[b];auto aa=attrs(a),bb=attrs(b);return aa!=bb?aa<bb:a<b; });
        for (size_t i = 0; i < order.size(); ++i) {
            auto id = order[i];
            if (!i || data.positions[id] != data.positions[order[i - 1]] || attrs(id) != attrs(order[i - 1]))
                representative = id;
            t.attribute_class[id] = representative;
        }
        auto original = graph(data, t, data.indices);
        for (size_t triangle = 0; triangle < original.unsafe.size(); ++triangle)
            if (original.unsafe[triangle])
                for (unsigned k = 0; k < 3; ++k)
                    t.unsafe_vertices[t.position_class[data.indices[triangle * 3 + k]]] = 1;
        vector<pair> owners;
        owners.reserve(data.indices.size());
        for (size_t i = 0; i < data.indices.size(); ++i)
            owners.push_back({t.position_class[data.indices[i]], original.parent[i / 3]});
        std::sort(owners.begin(), owners.end());
        for (size_t i = 1; i < owners.size(); ++i)
            if (owners[i][0] == owners[i - 1][0] && owners[i][1] != owners[i - 1][1])
                t.unsafe_vertices[owners[i][0]] = 1;
        return t;
    }
    boundary_signature boundary_of(source_geometry const& data, std::span<uint32_t const> indices, topology const& t) {
        boundary_signature out;
        if (indices.empty() || indices.size() % 3 || t.position_class.size() != data.positions.size())
            return out;
        for (auto id : indices)
            if (id >= t.position_class.size() || t.unsafe_vertices[t.position_class[id]])
                return out;
        auto g = graph(data, t, indices), a = graph(data, t, indices, true);
        if (std::find(g.unsafe.begin(), g.unsafe.end(), 1) != g.unsafe.end() || std::find(a.unsafe.begin(), a.unsafe.end(), 1) != a.unsafe.end())
            return out;
        out.geometric = boundary(g);
        out.attribute = boundary(a);
        for (auto e : out.geometric) {
            out.boundary_classes.push_back(e[0]);
            out.boundary_classes.push_back(e[1]);
        }
        for (auto e : out.attribute) {
            out.boundary_classes.push_back(t.position_class[e[0]]);
            out.boundary_classes.push_back(t.position_class[e[1]]);
        }
        std::sort(out.boundary_classes.begin(), out.boundary_classes.end());
        out.boundary_classes.erase(std::unique(out.boundary_classes.begin(), out.boundary_classes.end()), out.boundary_classes.end());
        for (size_t i = 0; i < indices.size(); ++i)
            out.class_components.push_back({t.position_class[indices[i]], g.parent[i / 3]});
        std::sort(out.class_components.begin(), out.class_components.end());
        out.class_components.erase(std::unique(out.class_components.begin(), out.class_components.end()), out.class_components.end());
        for (size_t i = 1; i < out.class_components.size(); ++i)
            if (out.class_components[i][0] == out.class_components[i - 1][0])
                return {};
        out.components = g.parent;
        std::sort(out.components.begin(), out.components.end());
        out.components.erase(std::unique(out.components.begin(), out.components.end()), out.components.end());
        out.valid = true;
        return out;
    }
    boundary_signature boundary_of(source_geometry const& data, std::span<uint32_t const> indices) {
        return boundary_of(data, indices, analyze_topology(data));
    }
    bool preserves_region(source_geometry const& data, topology const& t, boundary_signature const& original, std::span<uint32_t const> indices) {
        auto candidate = boundary_of(data, indices, t);
        if (!original.valid || !candidate.valid || !(candidate == original))
            return false;
        vector<uint32_t> components;
        for (size_t i = 0; i < indices.size(); i += 3) {
            array<uint32_t, 3> owners{};
            for (unsigned k = 0; k < 3; ++k) {
                auto id = t.position_class[indices[i + k]];
                auto found = std::lower_bound(original.class_components.begin(), original.class_components.end(), id, [](auto const& entry, auto key) { return entry[0] < key; });
                if (found == original.class_components.end() || (*found)[0] != id)
                    return false;
                owners[k] = (*found)[1];
            }
            if (owners[0] != owners[1] || owners[0] != owners[2])
                return false;
            components.push_back(owners[0]);
        }
        std::sort(components.begin(), components.end());
        components.erase(std::unique(components.begin(), components.end()), components.end());
        return components == original.components;
    }
} // namespace deren::asset::subtilis::builder_detail
