module;
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>
module deren.engine.subtilis.builder;

namespace deren::engine::subtilis {
    namespace bd = builder_detail;
    namespace {
        bool valid_options(build_options const& o) {
            return o.group_size >= 2 && o.group_size <= 32 && o.max_depth <= 32 &&
                   std::isfinite(o.target_ratio) && o.target_ratio > 0 && o.target_ratio < 1 &&
                   std::isfinite(o.max_error) && o.max_error >= 0 && std::isfinite(o.normal_weight) && o.normal_weight >= 0 &&
                   std::isfinite(o.uv_weight) && o.uv_weight >= 0 && o.max_output_bytes > 0;
        }
        std::uint64_t fingerprint(build_input const& in, bd::source const& data, build_options const& o) {
            XXH3_state_t state{};
            XXH3_64bits_reset(&state);
            auto word = [&](std::uint64_t value) {
                std::array<std::uint8_t, 8> bytes{};
                for (unsigned i = 0; i < 8; ++i) {
                    bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
                }
                XXH3_64bits_update(&state, bytes.data(), bytes.size());
            };
            word(1); // builder schema
            XXH3_64bits_update(&state, builder_dependency_revision.data(), builder_dependency_revision.size());
            word(in.geometry.vertex_stride);
            word(in.geometry.vertex_count);
            word(in.geometry.index_count);
            word(in.geometry.first_index);
            word(static_cast<std::uint32_t>(in.geometry.base_vertex));
            word(in.geometry.index_width);
            word(in.normal_offset);
            word(in.uv_offset);
            word(o.group_size);
            word(o.max_depth);
            word(o.max_output_bytes);
            word(std::bit_cast<std::uint32_t>(o.target_ratio));
            word(std::bit_cast<std::uint32_t>(o.max_error));
            word(std::bit_cast<std::uint32_t>(o.normal_weight));
            word(std::bit_cast<std::uint32_t>(o.uv_weight));
            XXH3_64bits_update(&state, data.vertices.data(), data.vertices.size());
            for (auto index : data.indices) {
                word(index);
            }
            return XXH3_64bits_digest(&state);
        }
        std::expected<bd::source, build_error> decode(build_input const& in, build_options const& options) {
            auto const& g = in.geometry;
            bd::source result;
            if (!g.index_count) {
                return result;
            }
            if (g.index_count % 3 || (g.index_width != 2 && g.index_width != 4) || !g.vertex_count || g.vertex_stride < 12 ||
                in.normal_offset > g.vertex_stride || 12 > g.vertex_stride - in.normal_offset ||
                in.uv_offset > g.vertex_stride || 8 > g.vertex_stride - in.uv_offset ||
                std::uint64_t(g.vertex_count) * g.vertex_stride > g.vertex_data.size() ||
                std::uint64_t(g.first_index) * g.index_width > g.index_data.size() ||
                std::uint64_t(g.index_count) * g.index_width > g.index_data.size() - std::uint64_t(g.first_index) * g.index_width) {
                return std::unexpected(build_error::invalid_input);
            }
            std::uint64_t vertex_bytes = std::uint64_t(g.vertex_count) * g.vertex_stride;
            std::uint64_t index_bytes = std::uint64_t(g.index_count) * 4;
            if (vertex_bytes > options.max_output_bytes || index_bytes > options.max_output_bytes - vertex_bytes ||
                (std::uint64_t(g.index_count) + meshlet_max_indices - 1) / meshlet_max_indices > (1u << 14)) {
                return std::unexpected(build_error::capacity);
            }
            result.vertices.assign(g.vertex_data.begin(), g.vertex_data.begin() + static_cast<std::size_t>(vertex_bytes));
            result.positions.resize(g.vertex_count);
            result.attributes.resize(g.vertex_count);
            std::array<double, 3> low{}, high{};
            for (std::uint32_t i = 0; i < g.vertex_count; ++i) {
                auto* bytes = result.vertices.data() + std::size_t(i) * g.vertex_stride;
                std::memcpy(result.positions[i].data(), bytes, 12);
                std::memcpy(result.attributes[i].data(), bytes + in.normal_offset, 12);
                std::memcpy(result.attributes[i].data() + 3, bytes + in.uv_offset, 8);
                for (float value : result.positions[i]) {
                    if (!std::isfinite(value)) {
                        return std::unexpected(build_error::invalid_geometry);
                    }
                }
                for (float value : result.attributes[i]) {
                    if (!std::isfinite(value)) {
                        return std::unexpected(build_error::invalid_geometry);
                    }
                }
                for (unsigned axis = 0; axis < 3; ++axis) {
                    double v = result.positions[i][axis];
                    low[axis] = i ? std::min(low[axis], v) : v;
                    high[axis] = i ? std::max(high[axis], v) : v;
                }
            }
            result.scale = std::max({high[0] - low[0], high[1] - low[1], high[2] - low[2]});
            result.indices.reserve(g.index_count);
            for (std::uint32_t i = 0; i < g.index_count; ++i) {
                std::uint32_t value = 0;
                auto* bytes = g.index_data.data() + (std::uint64_t(g.first_index) + i) * g.index_width;
                if (g.index_width == 2) {
                    std::uint16_t short_value = 0;
                    std::memcpy(&short_value, bytes, 2);
                    value = short_value;
                } else {
                    std::memcpy(&value, bytes, 4);
                }
                std::int64_t absolute = std::int64_t(value) + g.base_vertex;
                if (absolute < 0 || static_cast<std::uint64_t>(absolute) >= g.vertex_count) {
                    return std::unexpected(build_error::invalid_input);
                }
                result.indices.push_back(static_cast<std::uint32_t>(absolute));
            }
            return result;
        }
    } // namespace
    std::expected<meshlet, build_error> bd::make_record(source const& data, std::span<std::uint32_t const> indices, std::uint32_t first) {
        if (indices.empty() || indices.size() > meshlet_max_indices || indices.size() % 3) {
            return std::unexpected(build_error::invalid_geometry);
        }
        std::array<double, 3> lo{}, hi{};
        for (std::size_t i = 0; i < indices.size(); ++i) {
            auto const& position = data.positions[indices[i]];
            for (unsigned axis = 0; axis < 3; ++axis) {
                double v = position[axis];
                lo[axis] = i ? std::min(lo[axis], v) : v;
                hi[axis] = i ? std::max(hi[axis], v) : v;
            }
        }
        std::array<float, 3> center{};
        for (unsigned axis = 0; axis < 3; ++axis) {
            center[axis] = static_cast<float>(lo[axis] / 2 + hi[axis] / 2);
        }
        double radius = 0;
        for (auto index : indices) {
            auto const& p = data.positions[index];
            radius = std::max(radius, std::hypot(double(p[0]) - center[0], double(p[1]) - center[1], double(p[2]) - center[2]));
        }
        float rounded = static_cast<float>(radius);
        if (double(rounded) < radius) {
            rounded = std::nextafter(rounded, std::numeric_limits<float>::infinity());
        }
        if (!std::isfinite(rounded)) {
            return std::unexpected(build_error::invalid_geometry);
        }
        meshlet record{};
        record.first_index = first;
        record.index_count = static_cast<std::uint32_t>(indices.size());
        record.center_x = center[0];
        record.center_y = center[1];
        record.center_z = center[2];
        record.radius = rounded;
        return record;
    }
    std::expected<built_geometry, build_error> build(build_input const& input, build_options const& options) {
        if (!valid_options(options)) {
            return std::unexpected(build_error::invalid_options);
        }
        auto decoded = decode(input, options);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        auto& source = *decoded;
        build_statistics stats{};
        stats.source_hash = fingerprint(input, source, options);
        stats.lod0_triangles = source.indices.size() / 3;
        bd::vector<bd::logical_node> nodes;
        bd::vector<std::uint32_t> roots;
        for (std::size_t first = 0; first < source.indices.size(); first += meshlet_max_indices) {
            std::size_t count = std::min<std::size_t>(meshlet_max_indices, source.indices.size() - first);
            auto window = std::span(source.indices).subspan(first, count);
            auto record = bd::make_record(source, window, 0);
            if (!record) {
                return std::unexpected(record.error());
            }
            bd::logical_node node;
            node.indices.assign(window.begin(), window.end());
            node.records.push_back(*record);
            node.lod0_first = static_cast<std::uint32_t>(first);
            for (std::size_t t = first / 3; t < (first + count) / 3; ++t) {
                node.source_triangles.push_back(static_cast<std::uint32_t>(t));
            }
            roots.push_back(static_cast<std::uint32_t>(nodes.size()));
            nodes.push_back(std::move(node));
        }
        if (!roots.empty()) {
            stats.level_triangles.push_back(stats.lod0_triangles);
        }
        bd::initialize_meshoptimizer();
        if (auto status = bd::simplify(source, nodes, roots, stats, options); !status) {
            return std::unexpected(status.error());
        }
        // Reverse breadth levels preserve contiguous siblings, even in an unequal-depth forest.
        bd::vector<bd::vector<std::uint32_t>> levels;
        if (!roots.empty()) {
            levels.push_back(roots);
        }
        for (std::size_t level = 0; level < levels.size(); ++level) {
            bd::vector<std::uint32_t> next;
            for (auto id : levels[level]) {
                next.insert(next.end(), nodes[id].children.begin(), nodes[id].children.end());
            }
            if (!next.empty()) {
                levels.push_back(std::move(next));
            }
        }
        bd::vector<std::uint32_t> mapping(nodes.size());
        bd::vector<group_record> groups;
        bd::vector<meshlet> records;
        bd::vector<std::uint32_t> payload = source.indices;
        std::uint64_t bytes = source.vertices.size() + std::uint64_t(payload.size()) * 4 + std::uint64_t(roots.size()) * 4 + sizeof(build_statistics) + sizeof(build_options) + std::uint64_t(stats.level_triangles.size()) * 8;
        if (bytes > options.max_output_bytes) {
            return std::unexpected(build_error::capacity);
        }
        for (std::size_t level = levels.size(); level > 0; --level) {
            for (auto id : levels[level - 1]) {
                auto const& node = nodes[id];
                std::uint64_t addition = sizeof(validated_group) + std::uint64_t(node.records.size()) * sizeof(meshlet) + (node.children.empty() ? 0 : std::uint64_t(node.indices.size()) * 4);
                if (bytes > options.max_output_bytes || addition > options.max_output_bytes - bytes ||
                    payload.size() > UINT32_MAX || node.indices.size() > UINT32_MAX - payload.size()) {
                    return std::unexpected(build_error::capacity);
                }
                bytes += addition;
                std::uint32_t index_base = node.lod0_first;
                if (!node.children.empty()) {
                    index_base = static_cast<std::uint32_t>(payload.size());
                    payload.insert(payload.end(), node.indices.begin(), node.indices.end());
                }
                group_record group{};
                group.coarse = {static_cast<std::uint32_t>(records.size()), static_cast<std::uint32_t>(node.records.size())};
                group.error = node.error;
                if (!node.children.empty()) {
                    group.children = {mapping[node.children.front()], static_cast<std::uint32_t>(node.children.size())};
                }
                for (auto record : node.records) {
                    record.first_index += index_base;
                    records.push_back(record);
                }
                mapping[id] = static_cast<std::uint32_t>(groups.size());
                groups.push_back(group);
            }
        }
        bd::vector<std::uint32_t> packed_roots;
        for (auto root : roots) {
            packed_roots.push_back(mapping[root]);
            stats.coarse_triangles += nodes[root].indices.size() / 3;
        }
        auto validated = asset::make({records, groups, packed_roots, static_cast<std::uint32_t>(source.indices.size()), static_cast<std::uint32_t>(payload.size())});
        if (!validated) {
            return std::unexpected(build_error::invalid_geometry);
        }
        stats.nodes = static_cast<std::uint32_t>(nodes.size());
        stats.roots = static_cast<std::uint32_t>(roots.size());
        stats.clusters = static_cast<std::uint32_t>(records.size());
        stats.depth = levels.empty() ? 0 : static_cast<std::uint32_t>(levels.size() - 1);
        stats.payload_bytes = bytes;
        return built_geometry(std::move(source.vertices), std::move(payload), std::move(*validated), options, std::move(stats),
                              source.positions.empty() ? 0 : input.geometry.vertex_stride, static_cast<std::uint32_t>(source.positions.size()), static_cast<std::uint32_t>(source.indices.size()));
    }
} // namespace deren::engine::subtilis
