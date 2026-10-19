module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <utility>
export module deren.engine.subtilis.asset;
import deren.engine.meshlet;
import deren.utility.shared;

export namespace deren::engine::subtilis {
    struct cluster_range {
        std::uint32_t first = 0, count = 0;
    };
    /// CPU order is bottom-up: every child precedes its parent. A node replaces ALL children.
    struct group_record {
        cluster_range coarse, children;
        float error = 0;
    };
    struct asset_description {
        std::span<meshlet const> clusters;
        std::span<group_record const> groups;
        std::span<std::uint32_t const> roots;
        std::uint32_t lod0_index_count = 0, total_index_count = 0;
    };
    enum class asset_error { invalid_cluster,
                             invalid_cluster_range,
                             invalid_hierarchy,
                             invalid_error,
                             invalid_lod0_coverage,
                             capacity,
    };
    struct bounding_sphere {
        std::array<double, 3> center{};
        double radius = 0;
    };
    struct validated_group {
        group_record record;
        bounding_sphere bounds;
        std::uint32_t worst_cut = 0;
    };

    /// Owns validated CPU data. This layout is neither a disk format nor a shader ABI.
    class asset {
    public:
        asset(asset&& other) noexcept
            : clusters_(std::move(other.clusters_))
            , groups_(std::move(other.groups_))
            , roots_(std::move(other.roots_))
            , lod0_count_(std::exchange(other.lod0_count_, 0))
            , maximum_cut_(std::exchange(other.maximum_cut_, 0)) {
        }
        asset& operator=(asset&& other) noexcept {
            if (this != &other) {
                clusters_ = std::move(other.clusters_);
                groups_ = std::move(other.groups_);
                roots_ = std::move(other.roots_);
                lod0_count_ = std::exchange(other.lod0_count_, 0);
                maximum_cut_ = std::exchange(other.maximum_cut_, 0);
            }
            return *this;
        }
        asset(asset const&) = delete;
        asset& operator=(asset const&) = delete;
        [[nodiscard]] std::span<meshlet const> clusters() const noexcept {
            return clusters_;
        }
        [[nodiscard]] std::span<validated_group const> groups() const noexcept {
            return groups_;
        }
        [[nodiscard]] std::span<std::uint32_t const> roots() const noexcept {
            return roots_;
        }
        [[nodiscard]] std::uint32_t lod0_count() const noexcept {
            return lod0_count_;
        }
        [[nodiscard]] std::uint32_t maximum_cut() const noexcept {
            return maximum_cut_;
        }
        [[nodiscard]] static std::expected<asset, asset_error> make(asset_description const& input) {
            if (input.clusters.size() > UINT32_MAX || input.groups.size() > UINT32_MAX) {
                return std::unexpected(asset_error::capacity);
            }
            if (input.groups.empty()) {
                if (!input.clusters.empty() || !input.roots.empty() || input.lod0_index_count || input.total_index_count) {
                    return std::unexpected(asset_error::invalid_hierarchy);
                }
                return asset{};
            }
            if (!input.lod0_index_count || input.lod0_index_count % 3 || input.lod0_index_count > input.total_index_count) {
                return std::unexpected(asset_error::invalid_lod0_coverage);
            }
            for (auto const& c : input.clusters) {
                if (!c.index_count || c.index_count % 3 || c.index_count > meshlet_max_indices ||
                    c.first_index > input.total_index_count || c.index_count > input.total_index_count - c.first_index || c.base_vertex != 0 ||
                    !std::isfinite(c.center_x) || !std::isfinite(c.center_y) || !std::isfinite(c.center_z) ||
                    !std::isfinite(c.radius) || c.radius < 0 || !std::isfinite(c.axis_x) || !std::isfinite(c.axis_y) ||
                    !std::isfinite(c.axis_z) || !std::isfinite(c.cone_cos) || c.cone_cos < -1 || c.cone_cos > 1) {
                    return std::unexpected(asset_error::invalid_cluster);
                }
            }
            asset result;
            result.clusters_.assign(input.clusters.begin(), input.clusters.end());
            result.groups_.reserve(input.groups.size());
            deren::utility::vessel::vector<unsigned char> cluster_owner(input.clusters.size(), 0);
            deren::utility::vessel::vector<unsigned char> parent(input.groups.size(), 0);
            deren::utility::vessel::vector<unsigned char> root_seen(input.groups.size(), 0);
            deren::utility::vessel::vector<cluster_range> finest;
            for (std::size_t i = 0; i < input.groups.size(); ++i) {
                auto const& g = input.groups[i];
                if (!g.coarse.count || g.coarse.first > input.clusters.size() || g.coarse.count > input.clusters.size() - g.coarse.first) {
                    return std::unexpected(asset_error::invalid_cluster_range);
                }
                if (g.children.first > i || g.children.count > i - g.children.first) {
                    return std::unexpected(asset_error::invalid_hierarchy);
                }
                if (!std::isfinite(g.error) || g.error < 0 || (!g.children.count && g.error != 0)) {
                    return std::unexpected(asset_error::invalid_error);
                }
                std::array<double, 3> lo{};
                std::array<double, 3> hi{};
                bool first = true;
                auto extend = [&](bounding_sphere const& b) {
                    for (unsigned axis = 0; axis < 3; ++axis) {
                        double min = b.center[axis] - b.radius;
                        double max = b.center[axis] + b.radius;
                        if (first) {
                            lo[axis] = min;
                            hi[axis] = max;
                        } else {
                            lo[axis] = std::min(lo[axis], min);
                            hi[axis] = std::max(hi[axis], max);
                        }
                    }
                    first = false;
                };
                for (std::uint32_t j = 0; j < g.coarse.count; ++j) {
                    auto index = g.coarse.first + j;
                    if (cluster_owner[index]++) {
                        return std::unexpected(asset_error::invalid_cluster_range);
                    }
                    auto const& c = input.clusters[index];
                    extend({{c.center_x, c.center_y, c.center_z}, c.radius});
                    if (!g.children.count) {
                        finest.push_back({c.first_index, c.index_count});
                    }
                }
                std::uint64_t children_budget = 0;
                for (std::uint32_t j = 0; j < g.children.count; ++j) {
                    auto child = g.children.first + j;
                    if (parent[child]++) {
                        return std::unexpected(asset_error::invalid_hierarchy);
                    }
                    auto const& c = result.groups_[child];
                    if (g.error < c.record.error) {
                        return std::unexpected(asset_error::invalid_error);
                    }
                    children_budget += c.worst_cut;
                    extend(c.bounds);
                }
                if (children_budget > UINT32_MAX) {
                    return std::unexpected(asset_error::capacity);
                }
                validated_group node{};
                node.record = g;
                node.worst_cut = std::max(g.coarse.count, static_cast<std::uint32_t>(children_budget));
                for (unsigned axis = 0; axis < 3; ++axis) {
                    node.bounds.center[axis] = lo[axis] / 2 + hi[axis] / 2;
                }
                auto enclose = [&](bounding_sphere const& b) {
                    double distance = std::hypot(b.center[0] - node.bounds.center[0], b.center[1] - node.bounds.center[1], b.center[2] - node.bounds.center[2]);
                    node.bounds.radius = std::max(node.bounds.radius, distance + b.radius);
                };
                for (std::uint32_t j = 0; j < g.coarse.count; ++j) {
                    auto const& c = input.clusters[g.coarse.first + j];
                    enclose({{c.center_x, c.center_y, c.center_z}, c.radius});
                }
                for (std::uint32_t j = 0; j < g.children.count; ++j) {
                    enclose(result.groups_[g.children.first + j].bounds);
                }
                node.bounds.radius = std::nextafter(node.bounds.radius, std::numeric_limits<double>::infinity());
                if (!std::isfinite(node.bounds.radius)) {
                    return std::unexpected(asset_error::invalid_cluster);
                }
                result.groups_.push_back(node);
            }
            if (std::find(cluster_owner.begin(), cluster_owner.end(), 0) != cluster_owner.end()) {
                return std::unexpected(asset_error::invalid_cluster_range);
            }
            for (auto root : input.roots) {
                if (root >= input.groups.size() || parent[root] || root_seen[root]++) {
                    return std::unexpected(asset_error::invalid_hierarchy);
                }
                result.roots_.push_back(root);
            }
            for (std::size_t i = 0; i < parent.size(); ++i) {
                if (!parent[i] && !root_seen[i]) {
                    return std::unexpected(asset_error::invalid_hierarchy);
                }
            }
            std::sort(finest.begin(), finest.end(), [](auto a, auto b) { return a.first < b.first; });
            std::uint64_t cursor = 0;
            for (auto r : finest) {
                if (r.first != cursor) {
                    return std::unexpected(asset_error::invalid_lod0_coverage);
                }
                cursor += r.count;
            }
            if (cursor != input.lod0_index_count) {
                return std::unexpected(asset_error::invalid_lod0_coverage);
            }
            result.lod0_count_ = static_cast<std::uint32_t>(finest.size());
            std::uint64_t budget = 0;
            for (auto root : result.roots_) {
                budget += result.groups_[root].worst_cut;
            }
            if (budget > result.lod0_count_ || budget > (1u << 14)) {
                return std::unexpected(asset_error::capacity);
            }
            result.maximum_cut_ = static_cast<std::uint32_t>(budget);
            return result;
        }

    private:
        asset() = default;
        deren::utility::vessel::vector<meshlet> clusters_;
        deren::utility::vessel::vector<validated_group> groups_;
        deren::utility::vessel::vector<std::uint32_t> roots_;
        std::uint32_t lod0_count_ = 0, maximum_cut_ = 0;
    };
} // namespace deren::engine::subtilis
