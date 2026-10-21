module;
#include <algorithm>
#include <array>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <meshoptimizer.h>
#include <mutex>
module deren.asset.subtilis.builder;

namespace deren::asset::subtilis::builder_detail {
    namespace {
        auto fail(char const* message) {
            return std::unexpected(error{error_code::invalid_geometry, message, {}, {}});
        }
        double stage_now() {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        struct compact_geometry {
            vector<uint32_t> original, indices;
            vector<array<float, 3>> positions;
        };
        result<compact_geometry> compact(source_geometry const& source, std::span<uint32_t const> indices, primitive_scale const& scale) {
            if (indices.empty() || indices.size() % 3 || indices.size() > UINT32_MAX || !std::isfinite(scale.diagonal) || scale.diagonal < 0)
                return fail("invalid candidate shape or scale");
            for (auto id : indices)
                if (id >= source.positions.size())
                    return fail("candidate index outside source");
            compact_geometry out;
            out.original.assign(indices.begin(), indices.end());
            std::sort(out.original.begin(), out.original.end());
            out.original.erase(std::unique(out.original.begin(), out.original.end()), out.original.end());
            out.positions.resize(out.original.size());
            out.indices.reserve(indices.size());
            for (size_t i = 0; i < out.original.size(); ++i)
                for (unsigned k = 0; k < 3; ++k) {
                    double value = scale.diagonal ? (double(source.positions[out.original[i]][k]) - scale.origin[k]) / scale.diagonal : 0;
                    out.positions[i][k] = static_cast<float>(value);
                    if (!std::isfinite(out.positions[i][k]))
                        return fail("nonfinite normalized position");
                }
            for (auto id : indices)
                out.indices.push_back(static_cast<uint32_t>(std::lower_bound(out.original.begin(), out.original.end(), id) - out.original.begin()));
            return out;
        }
    } // namespace
    void initialize_meshoptimizer() {
        static std::once_flag once;
        std::call_once(once, [] { meshopt_setAllocator([](size_t size) { return utility::memory::allocate(size); }, [](void* pointer) { utility::memory::deallocate(pointer); }); });
    }
    primitive_scale measure_scale(source_geometry const& source) {
        primitive_scale out;
        if (source.indices.empty())
            return out;
        array<double, 3> hi{};
        for (size_t i = 0; i < source.indices.size(); ++i) {
            auto id = source.indices[i];
            if (id >= source.positions.size())
                return {};
            for (unsigned k = 0; k < 3; ++k) {
                double p = source.positions[id][k];
                out.origin[k] = i ? std::min(out.origin[k], p) : p;
                hi[k] = i ? std::max(hi[k], p) : p;
            }
        }
        out.diagonal = std::hypot(hi[0] - out.origin[0], hi[1] - out.origin[1], hi[2] - out.origin[2]);
        return out;
    }
    result<sphere> geometry_sphere(source_geometry const& source, std::span<uint32_t const> indices) {
        if (indices.empty())
            return fail("empty sphere input");
        array<double, 3> lo{}, hi{};
        for (size_t i = 0; i < indices.size(); ++i) {
            auto id = indices[i];
            if (id >= source.positions.size())
                return fail("sphere index outside source");
            for (unsigned k = 0; k < 3; ++k) {
                double p = source.positions[id][k];
                if (!std::isfinite(p))
                    return fail("nonfinite sphere input");
                lo[k] = i ? std::min(lo[k], p) : p;
                hi[k] = i ? std::max(hi[k], p) : p;
            }
        }
        array<float, 3> center{};
        for (unsigned k = 0; k < 3; ++k)
            center[k] = static_cast<float>(lo[k] / 2 + hi[k] / 2);
        double radius = 0;
        for (auto id : indices) {
            auto p = source.positions[id];
            radius = std::max(radius, std::hypot(double(p[0]) - center[0], double(p[1]) - center[1], double(p[2]) - center[2]));
        }
        float rounded = static_cast<float>(radius);
        if (double(rounded) < radius)
            rounded = std::nextafter(rounded, std::numeric_limits<float>::infinity());
        if (!std::isfinite(rounded))
            return fail("sphere cannot be represented in f32");
        return sphere{center[0], center[1], center[2], rounded};
    }
    result<frontier> cluster_indices(source_geometry const& source, std::span<uint32_t const> indices, build_options const& options, primitive_scale const& scale) {
        auto identity = build_digest(options);
        if (!identity)
            return std::unexpected(identity.error());
        auto local = compact(source, indices, scale);
        if (!local)
            return std::unexpected(local.error());
        initialize_meshoptimizer();
        auto bound = meshopt_buildMeshletsBound(indices.size(), options.vertex_limit, options.triangle_limit);
        vector<meshopt_Meshlet> meshlets(bound);
        vector<uint32_t> vertices(indices.size());
        vector<unsigned char> triangles(indices.size());
        auto count = meshopt_buildMeshlets(meshlets.data(), vertices.data(), triangles.data(), local->indices.data(), local->indices.size(), local->positions[0].data(), local->positions.size(), sizeof(array<float, 3>), options.vertex_limit, options.triangle_limit, 0);
        if (!count || count > bound)
            return fail("invalid meshlet output count");
        frontier out;
        out.indices.reserve(indices.size());
        out.clusters.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            auto const& c = meshlets[i];
            if (!c.triangle_count || c.triangle_count > options.triangle_limit || c.vertex_count > options.vertex_limit || c.vertex_offset > vertices.size() || c.vertex_count > vertices.size() - c.vertex_offset || c.triangle_offset > triangles.size() || size_t(c.triangle_count) * 3 > triangles.size() - c.triangle_offset)
                return fail("invalid meshlet output window");
            auto first = static_cast<uint32_t>(out.indices.size());
            for (size_t k = 0; k < size_t(c.triangle_count) * 3; ++k) {
                auto v = triangles[c.triangle_offset + k];
                if (v >= c.vertex_count || vertices[c.vertex_offset + v] >= local->original.size())
                    return fail("invalid meshlet vertex");
                out.indices.push_back(local->original[vertices[c.vertex_offset + v]]);
            }
            auto bounds = geometry_sphere(source, std::span(out.indices).subspan(first));
            if (!bounds)
                return std::unexpected(bounds.error());
            out.clusters.push_back({first, c.triangle_count * 3, none, none, *bounds});
        }
        if (out.indices.size() != indices.size())
            return fail("clustering dropped source corners");
        return out;
    }
    result<frontier> cluster_lod0(source_geometry const& source, build_options const& options) {
        auto identity = build_digest(options);
        if (!identity)
            return std::unexpected(identity.error());
        auto content = source_digest(source);
        if (!content)
            return std::unexpected(content.error());
        return cluster_indices(source, source.indices, options, measure_scale(source));
    }
    result<vector<vector<uint32_t>>> partition_clusters(source_geometry const& source, std::span<cluster_input const> clusters, build_options const& options, primitive_scale const& scale) {
        auto identity = build_digest(options);
        if (!identity)
            return std::unexpected(identity.error());
        vector<vector<uint32_t>> groups;
        if (clusters.empty())
            return groups;
        vector<uint32_t> indices, counts;
        for (auto c : clusters) {
            if (c.indices.empty() || c.indices.size() % 3 || c.indices.size() > 255 || c.indices.size() > UINT32_MAX - indices.size())
                return fail("invalid partition input");
            counts.push_back(static_cast<uint32_t>(c.indices.size()));
            indices.insert(indices.end(), c.indices.begin(), c.indices.end());
        }
        auto local = compact(source, indices, scale);
        if (!local)
            return std::unexpected(local.error());
        initialize_meshoptimizer();
        vector<uint32_t> partitions(clusters.size());
        auto count = meshopt_partitionClusters(partitions.data(), local->indices.data(), local->indices.size(), counts.data(), counts.size(), local->positions[0].data(), local->positions.size(), sizeof(array<float, 3>), options.target_group_size);
        if (!count || count > clusters.size())
            return fail("invalid partition count");
        groups.resize(count);
        for (uint32_t i = 0; i < partitions.size(); ++i) {
            if (partitions[i] >= count)
                return fail("invalid partition ID");
            groups[partitions[i]].push_back(i);
        }
        if (std::any_of(groups.begin(), groups.end(), [](auto const& g) { return g.empty(); }))
            return fail("empty partition");
        std::sort(groups.begin(), groups.end(), [](auto const& a, auto const& b) { return a.front() < b.front(); });
        return groups;
    }
    result<reduction_candidate> reduce_group(source_geometry const& source, std::span<cluster_input const> inputs, build_options const& options, primitive_scale const& scale, topology const* cached, reduce_timing* timing) {
        auto identity = build_digest(options);
        if (!identity)
            return std::unexpected(identity.error());
        if (source.normals.size() != source.positions.size() || source.uv0.size() != source.positions.size())
            return fail("source attributes have different counts");
        vector<uint32_t> indices;
        for (auto c : inputs) {
            if (c.indices.size() > UINT32_MAX - indices.size())
                return fail("group index count exceeds u32");
            indices.insert(indices.end(), c.indices.begin(), c.indices.end());
        }
        auto local = compact(source, indices, scale);
        if (!local)
            return std::unexpected(local.error());
        if (!scale.diagonal)
            return reduction_candidate{{}, 0, rejection_reason::zero_scale};
        auto owned = cached ? topology{} : analyze_topology(source);
        auto const& t = cached ? *cached : owned;
        double const boundary_started = stage_now();
        auto original = boundary_of(source, indices, t);
        if (timing)
            timing->boundary_ms += stage_now() - boundary_started;
        if (!original.valid)
            return reduction_candidate{{}, 0, rejection_reason::unsafe_topology};
        vector<array<float, 5>> attributes(local->original.size());
        vector<unsigned char> locks(local->original.size(), 0);
        for (size_t i = 0; i < local->original.size(); ++i) {
            auto id = local->original[i];
            auto n = source.normals[id];
            auto uv = source.uv0[id];
            attributes[i] = {n[0], n[1], n[2], uv[0], uv[1]};
            for (float v : attributes[i])
                if (!std::isfinite(v))
                    return fail("nonfinite candidate attribute");
            auto c = t.position_class[id];
            locks[i] = (t.seams[c] || std::binary_search(original.boundary_classes.begin(), original.boundary_classes.end(), c)) ? meshopt_SimplifyVertex_Lock : 0;
        }
        vector<uint32_t> simplified(indices.size());
        array<float, 5> weights{options.normal_weight, options.normal_weight, options.normal_weight, options.uv_weight, options.uv_weight};
        auto target = std::max<size_t>(1, static_cast<size_t>(double(indices.size() / 3) * options.target_ratio)) * 3;
        float step_error = 0;
        initialize_meshoptimizer();
        double const simplify_started = stage_now();
        std::fenv_t environment;
        if (std::feholdexcept(&environment) != 0)
            return reduction_candidate{{}, 0, rejection_reason::numeric_error};
        auto count = meshopt_simplifyWithAttributes(simplified.data(), local->indices.data(), local->indices.size(), local->positions[0].data(), local->positions.size(), sizeof(array<float, 3>), attributes[0].data(), sizeof(array<float, 5>), weights.data(), weights.size(), locks.data(), target, options.relative_error, meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute, &step_error);
        if (timing)
            timing->simplify_ms += stage_now() - simplify_started;
        auto arithmetic = std::fetestexcept(FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW);
        auto restored = std::fesetenv(&environment);
        if (arithmetic || restored || !std::isfinite(step_error) || step_error < 0)
            return reduction_candidate{{}, 0, rejection_reason::numeric_error};
        if (!count || count % 3 || count >= indices.size())
            return reduction_candidate{{}, 0, rejection_reason::no_progress};
        simplified.resize(count);
        vector<uint32_t> candidate;
        candidate.reserve(count);
        for (auto id : simplified) {
            if (id >= local->original.size())
                return fail("simplifier vertex outside source");
            candidate.push_back(local->original[id]);
        }
        double const preserve_started = stage_now();
        auto preserved = preserves_region(source, t, original, candidate);
        if (timing)
            timing->boundary_ms += stage_now() - preserve_started;
        if (!preserved)
            return reduction_candidate{{}, 0, rejection_reason::boundary_changed};
        double const recluster_started = stage_now();
        auto clustered = cluster_indices(source, candidate, options, scale);
        if (timing)
            timing->recluster_ms += stage_now() - recluster_started;
        if (!clustered)
            return std::unexpected(clustered.error());
        double error = double(step_error) * scale.diagonal;
        float rounded = static_cast<float>(error);
        if (double(rounded) < error)
            rounded = std::nextafter(rounded, std::numeric_limits<float>::infinity());
        if (!std::isfinite(rounded))
            return reduction_candidate{{}, 0, rejection_reason::numeric_error};
        return reduction_candidate{std::move(*clustered), rounded, rejection_reason::none};
    }
} // namespace deren::asset::subtilis::builder_detail
