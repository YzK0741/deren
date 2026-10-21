module;
#include <cstddef>
#include <cstdint>
module deren.asset.subtilis.dag;
namespace deren::asset::subtilis {
    namespace {
        auto fail(char const* message, std::optional<uint32_t> group = {}) {
            return std::unexpected(error{error_code::invalid_dag, message, {}, group});
        }
        bool range(uint32_t first, uint32_t count, size_t size) {
            return first <= size && count <= size - first;
        }
        bool sphere_valid(sphere s) {
            return std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.z) && std::isfinite(s.radius) && s.radius >= 0;
        }
        double distance(sphere a, sphere b) {
            return std::hypot(double(a.x) - b.x, double(a.y) - b.y, double(a.z) - b.z);
        }
        bool inside(sphere s, array<float, 3> const& p) {
            return std::hypot(double(p[0]) - s.x, double(p[1]) - s.y, double(p[2]) - s.z) <= double(s.radius);
        }
        std::span<uint32_t const> fine(payload const& p, group_record const& g) {
            return std::span(p.references).subspan(g.fine_first, g.fine_count);
        }
        std::span<uint32_t const> coarse(payload const& p, group_record const& g) {
            return std::span(p.references).subspan(g.coarse_first, g.coarse_count);
        }
        result<vector<uint32_t>> gather(payload const& p, std::span<uint32_t const> clusters, uint64_t max_records) {
            vector<uint32_t> out;
            uint64_t count = 0;
            for (auto id : clusters) {
                auto n = p.clusters[id].index_count;
                if (count > max_records || n > max_records - count)
                    return fail("expanded cluster index workspace exceeds limit");
                count += n;
            }
            out.reserve(static_cast<size_t>(count));
            for (auto id : clusters) {
                auto const& c = p.clusters[id];
                auto span = std::span(p.indices).subspan(c.first_index, c.index_count);
                out.insert(out.end(), span.begin(), span.end());
            }
            return out;
        }
        vector<array<uint32_t, 3>> triangles(std::span<uint32_t const> indices) {
            vector<array<uint32_t, 3>> out;
            out.reserve(indices.size() / 3);
            for (size_t i = 0; i < indices.size(); i += 3) {
                array<uint32_t, 3> a{indices[i], indices[i + 1], indices[i + 2]}, b{a[1], a[2], a[0]}, c{a[2], a[0], a[1]};
                out.push_back(std::min({a, b, c}));
            }
            std::sort(out.begin(), out.end());
            return out;
        }
        // Per-reduction boundary/component contract (design spec 3.4): position-equivalent vertices
        // define GEOMETRIC adjacency, while attribute equality (position+normal+uv) only defines the
        // seam identity of the attribute boundary. The builder checks both graphs independently, so
        // the full validator must too: judging components/pinch with a single attribute-class graph
        // rejects legitimate seam-sharing regions (a shared seam vertex is one class in two
        // attribute-connected components) on real assets.
        struct vertex_classes {
            vector<uint32_t> position, attribute;
        };
        vertex_classes classify_vertices(source_geometry const& s) {
            vertex_classes out;
            vector<uint32_t> order(s.positions.size());
            out.position.resize(order.size());
            out.attribute.resize(order.size());
            std::iota(order.begin(), order.end(), 0u);
            std::sort(order.begin(), order.end(), [&](auto a, auto b) { return s.positions[a] != s.positions[b] ? s.positions[a] < s.positions[b] : a < b; });
            uint32_t representative = 0;
            for (size_t i = 0; i < order.size(); ++i) {
                if (!i || s.positions[order[i]] != s.positions[order[i - 1]])
                    representative = order[i];
                out.position[order[i]] = representative;
            }
            auto key = [&](uint32_t id) {
                auto p = s.positions[id];
                auto n = s.normals[id];
                auto uv = s.uv0[id];
                return array<float, 8>{p[0], p[1], p[2], n[0], n[1], n[2], uv[0], uv[1]};
            };
            std::sort(order.begin(), order.end(), [&](auto a, auto b) {auto ka=key(a),kb=key(b);return ka!=kb?ka<kb:a<b; });
            representative = 0;
            for (size_t i = 0; i < order.size(); ++i) {
                if (!i || key(order[i]) != key(order[i - 1]))
                    representative = order[i];
                out.attribute[order[i]] = representative;
            }
            return out;
        }
        struct boundary_graph {
            vector<array<uint32_t, 2>> boundary;
            vector<uint32_t> components; // component root per triangle
            uint32_t component_count = 0;
        };
        result<boundary_graph> analyze_class_graph(std::span<uint32_t const> ids, vector<uint32_t> const& classes, bool track_components) {
            boundary_graph result;
            vector<array<uint32_t, 4>> edges;
            edges.reserve(ids.size());
            vector<array<uint32_t, 3>> faces;
            faces.reserve(ids.size() / 3);
            vector<uint32_t> parent(ids.size() / 3);
            std::iota(parent.begin(), parent.end(), 0u);
            auto root = [&](uint32_t id) {while(parent[id]!=id){parent[id]=parent[parent[id]];id=parent[id];}return id; };
            for (size_t i = 0; i < ids.size(); i += 3) {
                array<uint32_t, 3> tri{classes[ids[i]], classes[ids[i + 1]], classes[ids[i + 2]]};
                for (unsigned k = 0; k < 3; ++k) {
                    auto u = tri[k], v = tri[(k + 1) % 3];
                    if (u == v)
                        return fail("degenerate reduction edge");
                    edges.push_back({std::min(u, v), std::max(u, v), static_cast<uint32_t>(i / 3), u < v ? 1u : 0u});
                }
                std::sort(tri.begin(), tri.end());
                faces.push_back(tri);
            }
            std::sort(faces.begin(), faces.end());
            if (std::adjacent_find(faces.begin(), faces.end()) != faces.end())
                return fail("duplicate reduction face");
            std::sort(edges.begin(), edges.end());
            for (size_t first = 0; first < edges.size();) {
                size_t end = first + 1;
                while (end < edges.size() && edges[end][0] == edges[first][0] && edges[end][1] == edges[first][1])
                    ++end;
                if (end - first > 2 || (end - first == 2 && edges[first][3] == edges[first + 1][3]))
                    return fail("nonmanifold reduction edge");
                if (end - first == 1) {
                    auto e = edges[first];
                    result.boundary.push_back(e[3] ? array<uint32_t, 2>{e[0], e[1]} : array<uint32_t, 2>{e[1], e[0]});
                } else {
                    auto a = root(edges[first][2]), b = root(edges[first + 1][2]);
                    parent[std::max(a, b)] = std::min(a, b);
                }
                first = end;
            }
            if (track_components) {
                vector<array<uint32_t, 2>> vertices;
                vertices.reserve(ids.size());
                for (size_t i = 0; i < parent.size(); ++i) {
                    auto r = root(static_cast<uint32_t>(i));
                    if (r == i)
                        ++result.component_count;
                    for (unsigned k = 0; k < 3; ++k)
                        vertices.push_back({classes[ids[i * 3 + k]], r});
                }
                std::sort(vertices.begin(), vertices.end());
                for (size_t i = 1; i < vertices.size(); ++i)
                    if (vertices[i][0] == vertices[i - 1][0] && vertices[i][1] != vertices[i - 1][1])
                        return fail("pinched reduction components");
            }
            std::sort(result.boundary.begin(), result.boundary.end());
            return result;
        }
        struct region {
            vector<array<uint32_t, 2>> geometric, attribute;
            uint32_t components = 0;
            bool operator==(region const&) const = default;
        };
        result<region> analyze_region(source_geometry const& s, std::span<uint32_t const> ids, vertex_classes const& classes) {
            if (!ids.size() || ids.size() % 3)
                return fail("invalid reduction region");
            for (size_t i = 0; i < ids.size(); i += 3) {
                auto a = s.positions[ids[i]], b = s.positions[ids[i + 1]], c = s.positions[ids[i + 2]];
                array<double, 3> ab{}, ac{};
                for (unsigned k = 0; k < 3; ++k) {
                    ab[k] = double(b[k]) - a[k];
                    ac[k] = double(c[k]) - a[k];
                }
                if (std::hypot(ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]) == 0)
                    return fail("degenerate reduction triangle");
            }
            auto geometric = analyze_class_graph(ids, classes.position, true);
            if (!geometric)
                return std::unexpected(geometric.error());
            auto attribute = analyze_class_graph(ids, classes.attribute, false);
            if (!attribute)
                return std::unexpected(attribute.error());
            region out;
            out.geometric = std::move(geometric->boundary);
            out.attribute = std::move(attribute->boundary);
            out.components = geometric->component_count;
            return out;
        }
    } // namespace
    result<void> validate_structure(payload const& p, limits const& lim) {
        uint64_t counts[]{p.indices.size(), p.clusters.size(), p.groups.size(), p.references.size(), p.leaves.size(), p.roots.size(), p.build_meta.size()};
        uint64_t strides[]{4, 32, 48, 4, 4, 4, 1};
        uint64_t bytes = 352;
        for (size_t i = 0; i < 7; ++i) {
            if (counts[i] > lim.max_records || counts[i] > UINT32_MAX || ((i == 1 || i == 2) && counts[i] == none))
                return std::unexpected(error{error_code::capacity, "record limit", {}, {}});
            bytes += (counts[i] * strides[i] + 7) & ~uint64_t(7);
        }
        if (bytes > lim.max_payload_bytes)
            return std::unexpected(error{error_code::capacity, "payload byte limit", {}, {}});
        if (p.clusters.empty() || p.indices.empty() || p.leaves.empty() || p.roots.empty())
            return fail("empty asset");
        for (auto id : p.references)
            if (id >= p.clusters.size())
                return fail("reference outside cluster table");
        for (auto const& c : p.clusters) {
            if (!c.index_count || c.index_count % 3 || c.index_count > 255 || !range(c.first_index, c.index_count, p.indices.size()) || !sphere_valid(c.geometry))
                return fail("invalid cluster record");
            if ((c.producer != none && c.producer >= p.groups.size()) || (c.consumer != none && c.consumer >= p.groups.size()) || (c.producer != none && c.consumer != none && c.producer >= c.consumer))
                return fail("invalid cluster dependency");
        }
        vector<uint32_t> producer(p.clusters.size(), none), consumer(p.clusters.size(), none);
        for (uint32_t id = 0; id < p.groups.size(); ++id) {
            auto const& g = p.groups[id];
            if (!g.fine_count || !g.coarse_count || !range(g.fine_first, g.fine_count, p.references.size()) || !range(g.coarse_first, g.coarse_count, p.references.size()) || !sphere_valid(g.lod) || !std::isfinite(g.error) || g.error < 0)
                return fail("invalid reduction group", id);
            uint64_t fine_indices = 0, coarse_indices = 0;
            for (auto c : fine(p, g)) {
                if (c >= p.clusters.size() || consumer[c] != none || p.clusters[c].consumer != id)
                    return fail("invalid or repeated fine reference", id);
                auto dependency = p.clusters[c].producer;
                if (dependency != none && (dependency >= id || p.groups[dependency].error > g.error || distance(g.lod, p.groups[dependency].lod) + p.groups[dependency].lod.radius > double(g.lod.radius)))
                    return fail("nonmonotone dependency metric", id);
                consumer[c] = id;
                fine_indices += p.clusters[c].index_count;
            }
            for (auto c : coarse(p, g)) {
                if (c >= p.clusters.size() || producer[c] != none || p.clusters[c].producer != id || consumer[c] == id)
                    return fail("invalid or repeated coarse reference", id);
                producer[c] = id;
                coarse_indices += p.clusters[c].index_count;
            }
            if (coarse_indices >= fine_indices)
                return fail("reduction has no triangle progress", id);
        }
        for (size_t i = 0; i < p.clusters.size(); ++i)
            if (producer[i] != p.clusters[i].producer || consumer[i] != p.clusters[i].consumer)
                return fail("missing reciprocal group reference");
        auto exact_set = [&](vector<uint32_t> const& list, bool leaf) {
            vector<bool> found(p.clusters.size(), false);
            for (auto id : list) {
                if (id >= found.size() || found[id] || (leaf ? producer[id] : consumer[id]) != none)
                    return false;
                found[id] = true;
            }
            for (size_t i = 0; i < found.size(); ++i)
                if (found[i] != ((leaf ? producer[i] : consumer[i]) == none))
                    return false;
            return true;
        };
        if (!exact_set(p.leaves, true) || !exact_set(p.roots, false))
            return fail("leaf or root coverage mismatch");
        return {};
    }
    result<validated_asset> make_asset(source_geometry source, payload data, limits const& lim) {
        auto structure = validate_structure(data, lim);
        if (!structure)
            return std::unexpected(structure.error());
        if (source.positions.size() > lim.max_records || source.indices.size() > lim.max_records || uint64_t(source.positions.size()) * 32 + uint64_t(source.indices.size()) * 4 > lim.max_input_bytes)
            return std::unexpected(error{error_code::capacity, "source geometry exceeds limits", {}, {}});
        auto hash = source_digest(source);
        if (!hash)
            return std::unexpected(hash.error());
        if (*hash != data.source_hash)
            return fail("source fingerprint mismatch");
        for (auto id : data.indices)
            if (id >= source.positions.size())
                return fail("cluster vertex outside source");
        for (auto const& c : data.clusters)
            for (auto vertex : std::span(data.indices).subspan(c.first_index, c.index_count))
                if (!inside(c.geometry, source.positions[vertex]))
                    return fail("geometry sphere excludes a vertex");
        auto leaves = gather(data, data.leaves, std::min(lim.max_records, uint64_t(source.indices.size())));
        if (!leaves)
            return std::unexpected(leaves.error());
        if (triangles(*leaves) != triangles(source.indices))
            return fail("LOD0 triangle multiset or winding mismatch");
        auto classes = classify_vertices(source);
        for (uint32_t id = 0; id < data.groups.size(); ++id) {
            auto const& g = data.groups[id];
            auto f = gather(data, fine(data, g), lim.max_records), c = gather(data, coarse(data, g), lim.max_records);
            if (!f)
                return std::unexpected(f.error());
            if (!c)
                return std::unexpected(c.error());
            for (auto vertex : *f)
                if (!inside(g.lod, source.positions[vertex]))
                    return fail("LOD sphere excludes fine geometry", id);
            for (auto vertex : *c)
                if (!inside(g.lod, source.positions[vertex]))
                    return fail("LOD sphere excludes coarse geometry", id);
            auto fine_region = analyze_region(source, *f, classes), coarse_region = analyze_region(source, *c, classes);
            if (!fine_region || !coarse_region || *fine_region != *coarse_region)
                return fail("reduction boundary or components differ", id);
        }
        return validated_asset(std::move(source), std::move(data));
    }
} // namespace deren::asset::subtilis
