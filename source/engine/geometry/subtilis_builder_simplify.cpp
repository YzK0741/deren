module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <meshoptimizer.h>
#include <mutex>
#include <span>
#include <utility>
module deren.engine.subtilis.builder;
namespace deren::engine::subtilis::builder_detail {
    void initialize_meshoptimizer() {
        static std::once_flag once;
        std::call_once(once, [] {
            meshopt_setAllocator([](std::size_t size) { return deren::utility::memory::allocate(size, alignof(std::max_align_t)); },
                                 [](void* pointer) { deren::utility::memory::deallocate(pointer); });
        });
    }
    namespace {
        enum class rejection { topology,
                               reduction,
                               budget,
        };
        std::expected<logical_node, rejection> parent_node(source const& data, topology const& topology,
                                                           vector<logical_node> const& nodes, vector<std::uint32_t> const& children,
                                                           build_options const& options) {
            logical_node parent;
            parent.children = children;
            std::size_t previous_records = 0;
            float previous_error = 0;
            for (auto child : children) {
                auto const& node = nodes[child];
                parent.indices.insert(parent.indices.end(), node.indices.begin(), node.indices.end());
                parent.source_triangles.insert(parent.source_triangles.end(), node.source_triangles.begin(), node.source_triangles.end());
                previous_records += node.records.size();
                previous_error = std::max(previous_error, node.error);
                parent.depth = std::max(parent.depth, node.depth + 1);
            }
            if (parent.depth > options.max_depth) {
                return std::unexpected(rejection::budget);
            }
            std::sort(parent.source_triangles.begin(), parent.source_triangles.end());
            auto region = source_region(data, topology, parent.source_triangles);
            if (!region.valid) {
                return std::unexpected(rejection::topology);
            }
            // Candidate-local data avoids whole-asset allocation/clearing for each simplification.
            vector<std::uint32_t> original_ids = parent.indices;
            std::sort(original_ids.begin(), original_ids.end());
            original_ids.erase(std::unique(original_ids.begin(), original_ids.end()), original_ids.end());
            vector<std::array<float, 3>> positions(original_ids.size());
            vector<std::array<float, 5>> attributes(original_ids.size());
            vector<unsigned char> locks(original_ids.size(), 0);
            for (std::size_t i = 0; i < original_ids.size(); ++i) {
                auto id = original_ids[i];
                for (unsigned axis = 0; axis < 3; ++axis) {
                    positions[i][axis] = static_cast<float>((double(data.positions[id][axis]) - data.origin[axis]) / data.scale);
                }
                attributes[i] = data.attributes[id];
                auto c = topology.class_of[id];
                locks[i] = (topology.seams[c] || std::binary_search(region.boundary_classes.begin(), region.boundary_classes.end(), c)) ? meshopt_SimplifyVertex_Lock : 0;
            }
            vector<std::uint32_t> local_indices;
            local_indices.reserve(parent.indices.size());
            for (auto id : parent.indices) {
                local_indices.push_back(static_cast<std::uint32_t>(std::lower_bound(original_ids.begin(), original_ids.end(), id) - original_ids.begin()));
            }
            vector<std::uint32_t> simplified(local_indices.size());
            std::array<float, 5> weights{options.normal_weight, options.normal_weight, options.normal_weight, options.uv_weight, options.uv_weight};
            auto input_triangles = local_indices.size() / 3;
            std::size_t target_triangles = std::max<std::size_t>(1, static_cast<std::size_t>(double(input_triangles) * options.target_ratio));
            float step_error = 0;
            auto count = meshopt_simplifyWithAttributes(simplified.data(), local_indices.data(), local_indices.size(), positions[0].data(), positions.size(), 3 * sizeof(float),
                                                        attributes[0].data(), 5 * sizeof(float), weights.data(), weights.size(), locks.data(),
                                                        target_triangles * 3, options.max_error, meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute, &step_error);
            if (!count || count % 3 || count >= local_indices.size()) {
                return std::unexpected(rejection::reduction);
            }
            if (!std::isfinite(step_error) || step_error < 0) {
                return std::unexpected(rejection::topology);
            }
            simplified.resize(count);
            vector<std::uint32_t> candidate;
            candidate.reserve(count);
            for (auto id : simplified) {
                if (id >= original_ids.size()) {
                    return std::unexpected(rejection::topology);
                }
                candidate.push_back(original_ids[id]);
            }
            if (!preserves_region(data, topology, region, candidate)) {
                return std::unexpected(rejection::topology);
            }
            std::size_t limit = meshopt_buildMeshletsBound(count, 255, meshlet_max_triangles);
            vector<meshopt_Meshlet> meshlets(limit);
            vector<std::uint32_t> vertices(count);
            vector<unsigned char> triangles(count);
            auto cluster_count = meshopt_buildMeshlets(meshlets.data(), vertices.data(), triangles.data(), simplified.data(), count,
                                                       positions[0].data(), positions.size(), 3 * sizeof(float), 255, meshlet_max_triangles, 0);
            if (!cluster_count || cluster_count >= previous_records) {
                return std::unexpected(rejection::reduction);
            }
            parent.indices.clear();
            parent.indices.reserve(count);
            for (std::size_t i = 0; i < cluster_count; ++i) {
                auto const& cluster = meshlets[i];
                auto first = static_cast<std::uint32_t>(parent.indices.size());
                if (!cluster.triangle_count || cluster.triangle_count > meshlet_max_triangles || cluster.vertex_count > 255 ||
                    cluster.vertex_offset > vertices.size() || cluster.vertex_count > vertices.size() - cluster.vertex_offset ||
                    cluster.triangle_offset > triangles.size() || std::uint64_t(cluster.triangle_count) * 3 > triangles.size() - cluster.triangle_offset) {
                    return std::unexpected(rejection::topology);
                }
                for (std::size_t corner = 0; corner < std::size_t(cluster.triangle_count) * 3; ++corner) {
                    auto local = triangles[cluster.triangle_offset + corner];
                    if (local >= cluster.vertex_count || vertices[cluster.vertex_offset + local] >= original_ids.size()) {
                        return std::unexpected(rejection::topology);
                    }
                    parent.indices.push_back(original_ids[vertices[cluster.vertex_offset + local]]);
                }
                auto record = make_record(data, std::span(parent.indices).subspan(first), first);
                if (!record) {
                    return std::unexpected(rejection::topology);
                }
                parent.records.push_back(*record);
            }
            if (parent.indices.size() != count) {
                return std::unexpected(rejection::topology);
            }
            double error = double(previous_error) + double(step_error) * data.scale;
            parent.error = static_cast<float>(error);
            if (double(parent.error) < error) {
                parent.error = std::nextafter(parent.error, std::numeric_limits<float>::infinity());
            }
            if (!std::isfinite(parent.error)) {
                return std::unexpected(rejection::topology);
            }
            return parent;
        }
    } // namespace
    std::expected<void, build_error> simplify(source const& data, vector<logical_node>& nodes, vector<std::uint32_t>& roots,
                                              build_statistics& stats, build_options const& options) {
        if (roots.size() < 2 || !options.max_depth) {
            return {};
        }
        if (data.scale == 0) {
            stats.rejected_topology = static_cast<std::uint32_t>(roots.size());
            return {};
        }
        auto topology = analyze_topology(data);
        vector<std::array<float, 3>> positions(data.positions.size());
        for (auto id : data.indices) {
            for (unsigned axis = 0; axis < 3; ++axis) {
                positions[id][axis] = static_cast<float>((double(data.positions[id][axis]) - data.origin[axis]) / data.scale);
            }
        }
        auto frontier = roots;
        vector<std::uint32_t> finished;
        std::uint64_t finished_triangles = 0;
        auto stable_order = [&](auto a, auto b) { return nodes[a].source_triangles.front() < nodes[b].source_triangles.front(); };
        for (std::uint32_t depth = 0; depth < options.max_depth && frontier.size() > 1; ++depth) {
            vector<std::uint32_t> indices;
            vector<std::uint32_t> counts;
            vector<std::uint32_t> partitions(frontier.size());
            for (auto id : frontier) {
                indices.insert(indices.end(), nodes[id].indices.begin(), nodes[id].indices.end());
                counts.push_back(static_cast<std::uint32_t>(nodes[id].indices.size()));
            }
            auto partition_count = meshopt_partitionClusters(partitions.data(), indices.data(), indices.size(), counts.data(), counts.size(),
                                                             positions[0].data(), positions.size(), 3 * sizeof(float), options.group_size);
            vector<vector<std::uint32_t>> groups(partition_count);
            for (std::size_t i = 0; i < frontier.size(); ++i) {
                groups[partitions[i]].push_back(frontier[i]);
            }
            for (auto& group : groups) {
                std::sort(group.begin(), group.end(), stable_order);
            }
            std::sort(groups.begin(), groups.end(), [&](auto const& a, auto const& b) { return stable_order(a.front(), b.front()); });
            vector<std::uint32_t> next;
            for (auto const& group : groups) {
                std::expected<logical_node, rejection> parent = std::unexpected(rejection::reduction);
                if (group.size() > 1) {
                    parent = parent_node(data, topology, nodes, group, options);
                }
                if (parent) {
                    next.push_back(static_cast<std::uint32_t>(nodes.size()));
                    nodes.push_back(std::move(*parent));
                } else {
                    if (parent.error() == rejection::topology) {
                        ++stats.rejected_topology;
                    } else if (parent.error() == rejection::budget) {
                        ++stats.rejected_budget;
                    } else {
                        ++stats.rejected_reduction;
                    }
                    for (auto id : group) {
                        finished.push_back(id);
                        finished_triangles += nodes[id].indices.size() / 3;
                    }
                }
            }
            if (next.empty()) {
                frontier.clear();
                break;
            }
            std::sort(next.begin(), next.end(), stable_order);
            std::uint64_t total = finished_triangles;
            for (auto id : next) {
                total += nodes[id].indices.size() / 3;
            }
            stats.level_triangles.push_back(total);
            frontier = std::move(next);
        }
        finished.insert(finished.end(), frontier.begin(), frontier.end());
        std::sort(finished.begin(), finished.end(), stable_order);
        roots = std::move(finished);
        return {};
    }
} // namespace deren::engine::subtilis::builder_detail
