module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <tuple>
#include <utility>
export module deren.engine.subtilis.dag_asset;
export import deren.engine.meshlet;

export namespace deren::engine::subtilis {
    inline constexpr uint32_t dag_none = UINT32_MAX;
    struct dag_range {
        uint32_t first = 0, count = 0;
        bool operator==(dag_range const&) const = default;
    };
    struct dag_sphere {
        float x = 0, y = 0, z = 0, radius = 0;
        bool operator==(dag_sphere const&) const = default;
    };
    struct dag_cluster {
        meshlet record;
        uint32_t producer = dag_none, consumer = dag_none;
        bool operator==(dag_cluster const& b) const {
            auto key = [](dag_cluster const& c) {auto const& r=c.record;return std::tuple(r.first_index,r.index_count,r.base_vertex,r._pad,r.center_x,r.center_y,r.center_z,r.radius,r.axis_x,r.axis_y,r.axis_z,r.cone_cos,c.producer,c.consumer); };
            return key(*this) == key(b);
        }
    };
    struct dag_group {
        dag_range fine, coarse;
        dag_sphere lod;
        float error = 0;
        bool operator==(dag_group const&) const = default;
    };
    enum class dag_error { invalid_cluster,
                           invalid_range,
                           invalid_hierarchy,
                           invalid_error,
                           invalid_lod0_coverage,
                           capacity };
    class dag_asset {
        utility::vessel::vector<dag_cluster> clusters_;
        utility::vessel::vector<dag_group> groups_;
        utility::vessel::vector<uint32_t> references_, leaves_, roots_;
        utility::vessel::vector<meshlet> lod0_;
        uint32_t lod0_indices_ = 0, selected_bound_ = 0;
        dag_asset() = default;

    public:
        dag_asset(dag_asset const&) = delete;
        dag_asset& operator=(dag_asset const&) = delete;
        dag_asset(dag_asset&&) = default;
        dag_asset& operator=(dag_asset&&) = default;
        std::span<dag_cluster const> clusters() const noexcept {
            return clusters_;
        }
        std::span<dag_group const> groups() const noexcept {
            return groups_;
        }
        std::span<uint32_t const> references() const noexcept {
            return references_;
        }
        std::span<uint32_t const> leaves() const noexcept {
            return leaves_;
        }
        std::span<uint32_t const> roots() const noexcept {
            return roots_;
        }
        std::span<meshlet const> lod0_clusters() const noexcept {
            return lod0_;
        }
        uint32_t lod0_index_count() const noexcept {
            return lod0_indices_;
        }
        uint32_t cluster_count() const noexcept {
            return static_cast<uint32_t>(clusters_.size());
        }
        uint32_t maximum_selected_bound() const noexcept {
            return selected_bound_;
        }
        static std::expected<dag_asset, dag_error> make(std::span<dag_cluster const> clusters, std::span<dag_group const> groups, std::span<uint32_t const> references, std::span<uint32_t const> leaves, std::span<uint32_t const> roots, uint32_t lod0_index_count, uint32_t total_index_count = dag_none) {
            if (clusters.size() >= dag_none || groups.size() >= dag_none || references.size() > UINT32_MAX || leaves.size() > UINT32_MAX || roots.size() > UINT32_MAX)
                return std::unexpected(dag_error::capacity);
            if (clusters.empty() || leaves.empty() || roots.empty())
                return std::unexpected(dag_error::invalid_hierarchy);
            if (!lod0_index_count || lod0_index_count % 3)
                return std::unexpected(dag_error::invalid_lod0_coverage);
            auto valid_sphere = [](dag_sphere s) { return std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.z) && std::isfinite(s.radius) && s.radius >= 0; };
            auto distance = [](dag_sphere a, dag_sphere b) { return std::hypot(double(a.x) - b.x, double(a.y) - b.y, double(a.z) - b.z); };
            auto geometry = [](dag_cluster const& c) {auto const& r=c.record;return dag_sphere{r.center_x,r.center_y,r.center_z,r.radius}; };
            uint64_t end = 0;
            for (auto const& c : clusters) {
                auto const& r = c.record;
                auto finish = uint64_t(r.first_index) + r.index_count;
                if (finish > UINT32_MAX)
                    return std::unexpected(dag_error::capacity);
                if (!r.index_count || r.index_count % 3 || r.index_count > meshlet_max_indices || r.base_vertex || r._pad || !valid_sphere(geometry(c)) || !std::isfinite(r.axis_x) || !std::isfinite(r.axis_y) || !std::isfinite(r.axis_z) || !std::isfinite(r.cone_cos) || r.cone_cos < -1 || r.cone_cos > 1 || (total_index_count != dag_none && finish > total_index_count))
                    return std::unexpected(dag_error::invalid_cluster);
                if ((c.producer != dag_none && c.producer >= groups.size()) || (c.consumer != dag_none && c.consumer >= groups.size()) || (c.producer != dag_none && c.consumer != dag_none && c.producer >= c.consumer))
                    return std::unexpected(dag_error::invalid_hierarchy);
                if (c.producer != dag_none && r.first_index < lod0_index_count)
                    return std::unexpected(dag_error::invalid_lod0_coverage);
                end = std::max(end, finish);
            }
            if (lod0_index_count > end || (total_index_count != dag_none && lod0_index_count > total_index_count))
                return std::unexpected(dag_error::invalid_lod0_coverage);
            for (auto id : references)
                if (id >= clusters.size())
                    return std::unexpected(dag_error::invalid_range);
            utility::vessel::vector<uint32_t> producer(clusters.size(), dag_none), consumer(clusters.size(), dag_none);
            auto in_range = [&](dag_range r) { return r.first <= references.size() && r.count <= references.size() - r.first; };
            for (uint32_t id = 0; id < groups.size(); ++id) {
                auto const& g = groups[id];
                if (!g.fine.count || !g.coarse.count || !in_range(g.fine) || !in_range(g.coarse))
                    return std::unexpected(dag_error::invalid_range);
                if (!valid_sphere(g.lod))
                    return std::unexpected(dag_error::invalid_cluster);
                if (!std::isfinite(g.error) || g.error < 0)
                    return std::unexpected(dag_error::invalid_error);
                uint64_t fine_indices = 0, coarse_indices = 0;
                for (auto c : references.subspan(g.fine.first, g.fine.count)) {
                    if (consumer[c] != dag_none || clusters[c].consumer != id)
                        return std::unexpected(dag_error::invalid_hierarchy);
                    auto dependency = clusters[c].producer;
                    if (dependency != dag_none) {
                        if (dependency >= id)
                            return std::unexpected(dag_error::invalid_hierarchy);
                        auto const& previous = groups[dependency];
                        if (previous.error > g.error)
                            return std::unexpected(dag_error::invalid_error);
                        if (distance(g.lod, previous.lod) + double(previous.lod.radius) > double(g.lod.radius))
                            return std::unexpected(dag_error::invalid_cluster);
                    }
                    // Cluster and LOD bounds independently enclose actual vertices.
                    // Neither containment of the cluster sphere nor its center is
                    // implied by that contract; full geometry checks stay offline.
                    consumer[c] = id;
                    fine_indices += clusters[c].record.index_count;
                }
                for (auto c : references.subspan(g.coarse.first, g.coarse.count)) {
                    if (producer[c] != dag_none || clusters[c].producer != id || consumer[c] == id)
                        return std::unexpected(dag_error::invalid_hierarchy);
                    producer[c] = id;
                    coarse_indices += clusters[c].record.index_count;
                }
                if (coarse_indices >= fine_indices)
                    return std::unexpected(dag_error::invalid_hierarchy);
            }
            for (size_t i = 0; i < clusters.size(); ++i)
                if (producer[i] != clusters[i].producer || consumer[i] != clusters[i].consumer)
                    return std::unexpected(dag_error::invalid_hierarchy);
            auto exact_set = [&](std::span<uint32_t const> ids, bool leaf) {
                utility::vessel::vector<bool> seen(clusters.size(), false);
                for (auto id : ids) {
                    if (id >= clusters.size() || seen[id] || (leaf ? producer[id] : consumer[id]) != dag_none)
                        return false;
                    seen[id] = true;
                }
                for (size_t i = 0; i < seen.size(); ++i)
                    if (seen[i] != ((leaf ? producer[i] : consumer[i]) == dag_none))
                        return false;
                return true;
            };
            if (!exact_set(leaves, true) || !exact_set(roots, false))
                return std::unexpected(dag_error::invalid_hierarchy);
            utility::vessel::vector<meshlet> lod0;
            lod0.reserve(leaves.size());
            for (auto id : leaves)
                lod0.push_back(clusters[id].record);
            std::sort(lod0.begin(), lod0.end(), [](auto const& a, auto const& b) { return a.first_index < b.first_index; });
            uint64_t cursor = 0;
            for (auto const& c : lod0) {
                if (c.first_index != cursor)
                    return std::unexpected(dag_error::invalid_lod0_coverage);
                cursor += c.index_count;
            }
            if (cursor != lod0_index_count)
                return std::unexpected(dag_error::invalid_lod0_coverage);
            dag_asset result;
            result.clusters_.assign(clusters.begin(), clusters.end());
            result.groups_.assign(groups.begin(), groups.end());
            result.references_.assign(references.begin(), references.end());
            result.leaves_.assign(leaves.begin(), leaves.end());
            result.roots_.assign(roots.begin(), roots.end());
            result.lod0_ = std::move(lod0);
            result.lod0_indices_ = lod0_index_count;
            // Every accepted group strictly reduces triangles. A complete cut has
            // at most the LOD0 triangle count, with at least one triangle per cluster.
            result.selected_bound_ = std::min(static_cast<uint32_t>(clusters.size()), lod0_index_count / 3);
            return result;
        }
    };
} // namespace deren::engine::subtilis
