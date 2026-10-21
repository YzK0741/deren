module;
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <yyjson.h>
module deren.asset.subtilis.builder;

namespace deren::asset::subtilis {
    namespace {
        namespace bd = builder_detail;
        auto fail(error_code code, char const* message) {
            return std::unexpected(error{code, message, {}, {}});
        }
        uint64_t storage(payload const& p) {
            return uint64_t(p.indices.size()) * 4 + uint64_t(p.clusters.size()) * 32 + uint64_t(p.groups.size()) * 48 + uint64_t(p.references.size() + p.leaves.size() + p.roots.size()) * 4 + 8192;
        }
        result<sphere> lod_sphere(source_geometry const& source, payload const& p, std::span<uint32_t const> fine, bd::frontier const& coarse) {
            array<double, 3> lo{}, hi{};
            bool first = true;
            auto extent = [&](array<double, 3> center, double radius) {for(unsigned k=0;k<3;++k){double a=center[k]-radius,b=center[k]+radius;lo[k]=first?a:std::min(lo[k],a);hi[k]=first?b:std::max(hi[k],b);}first=false; };
            auto point = [&](uint32_t id) {auto v=source.positions[id];extent({v[0],v[1],v[2]},0); };
            for (auto id : fine) {
                auto const& c = p.clusters[id];
                for (auto vertex : std::span(p.indices).subspan(c.first_index, c.index_count))
                    point(vertex);
                if (c.producer != none) {
                    auto s = p.groups[c.producer].lod;
                    extent({s.x, s.y, s.z}, s.radius);
                }
            }
            for (auto id : coarse.indices)
                point(id);
            array<float, 3> center{};
            for (unsigned k = 0; k < 3; ++k)
                center[k] = static_cast<float>(lo[k] / 2 + hi[k] / 2);
            double radius = 0;
            auto include = [&](array<double, 3> v, double r) { radius = std::max(radius, std::hypot(v[0] - center[0], v[1] - center[1], v[2] - center[2]) + r); };
            auto vertex = [&](uint32_t id) {auto v=source.positions[id];include({v[0],v[1],v[2]},0); };
            for (auto id : fine) {
                auto const& c = p.clusters[id];
                for (auto v : std::span(p.indices).subspan(c.first_index, c.index_count))
                    vertex(v);
                if (c.producer != none) {
                    auto s = p.groups[c.producer].lod;
                    include({s.x, s.y, s.z}, s.radius);
                }
            }
            for (auto v : coarse.indices)
                vertex(v);
            float rounded = static_cast<float>(radius);
            if (double(rounded) < radius)
                rounded = std::nextafter(rounded, std::numeric_limits<float>::infinity());
            if (!std::isfinite(rounded))
                return fail(error_code::invalid_geometry, "LOD sphere cannot fit f32");
            return sphere{center[0], center[1], center[2], rounded};
        }
        string metadata(build_options const& o, build_report const& report) {
            yyjson_alc allocator{
                [](void*, size_t n) -> void* { return utility::memory::allocate(n); },
                [](void*, void* p, size_t old, size_t n) -> void* {auto* next=utility::memory::allocate(n);if(p)std::memcpy(next,p,std::min(old,n));utility::memory::deallocate(p);return next; },
                [](void*, void* p) { utility::memory::deallocate(p); }, nullptr};
            auto* d = yyjson_mut_doc_new(&allocator);
            auto* root = yyjson_mut_obj(d);
            yyjson_mut_doc_set_root(d, root);
            yyjson_mut_obj_add_str(d, root, "algorithm", "subtilis-dag-v1");
            yyjson_mut_obj_add_str(d, root, "profile", "shared-vertices");
            auto* parameters = yyjson_mut_obj(d);
            yyjson_mut_obj_add_val(d, root, "parameters", parameters);
            yyjson_mut_obj_add_uint(d, parameters, "triangleLimit", o.triangle_limit);
            yyjson_mut_obj_add_uint(d, parameters, "vertexLimit", o.vertex_limit);
            yyjson_mut_obj_add_uint(d, parameters, "targetGroupSize", o.target_group_size);
            yyjson_mut_obj_add_uint(d, parameters, "maxRounds", o.max_rounds);
            yyjson_mut_obj_add_real(d, parameters, "targetRatio", o.target_ratio);
            yyjson_mut_obj_add_real(d, parameters, "relativeError", o.relative_error);
            yyjson_mut_obj_add_real(d, parameters, "normalWeight", o.normal_weight);
            yyjson_mut_obj_add_real(d, parameters, "uvWeight", o.uv_weight);
            auto* dependencies = yyjson_mut_obj(d);
            yyjson_mut_obj_add_val(d, root, "dependencies", dependencies);
            yyjson_mut_obj_add_str(d, dependencies, "meshoptimizer", "9e1f07b159d3cb777f1c67ed31fc11fd117986f4");
            yyjson_mut_obj_add_str(d, dependencies, "fastgltf", "0.9.0");
            yyjson_mut_obj_add_str(d, dependencies, "simdjson", "4.6.9");
            yyjson_mut_obj_add_str(d, dependencies, "yyjson", "0.12.0");
            yyjson_mut_obj_add_uint(d, root, "rounds", report.rounds);
            yyjson_mut_obj_add_uint(d, root, "crossGroupRepartitions", report.cross_group_repartitions);
            auto* levels = yyjson_mut_arr(d);
            yyjson_mut_obj_add_val(d, root, "levels", levels);
            for (auto const& row : report.levels) {
                auto* l = yyjson_mut_obj(d);
                yyjson_mut_obj_add_uint(d, l, "triangles", row.triangles);
                yyjson_mut_obj_add_uint(d, l, "clusters", row.clusters);
                yyjson_mut_obj_add_uint(d, l, "groups", row.groups);
                yyjson_mut_obj_add_uint(d, l, "roots", row.roots);
                yyjson_mut_arr_append(levels, l);
            }
            auto* rejected = yyjson_mut_obj(d);
            yyjson_mut_obj_add_val(d, root, "rejected", rejected);
            char const* names[]{"none", "noProgress", "unsafeTopology", "zeroScale", "numericError", "boundaryChanged"};
            for (size_t i = 1; i < report.rejected.size(); ++i)
                yyjson_mut_obj_add_uint(d, rejected, names[i], report.rejected[i]);
            size_t length = 0;
            auto* bytes = yyjson_mut_write_opts(d, 0, &allocator, &length, nullptr);
            if (!bytes)
                utility::fast_panic("Subtilis metadata serialization failed");
            string out(bytes, length);
            utility::memory::deallocate(bytes);
            yyjson_mut_doc_free(d);
            return out;
        }
    } // namespace
    result<build_result> build_serial(source_geometry source, build_options const& options, build_limits const& budget) {
        auto start = std::chrono::steady_clock::now();
        auto identity = build_digest(options);
        if (!identity)
            return std::unexpected(identity.error());
        limits validation_limits;
        validation_limits.max_payload_bytes = budget.max_output_bytes;
        if (source.positions.size() > validation_limits.max_records || source.indices.size() > validation_limits.max_records)
            return fail(error_code::capacity, "source record limit");
        uint64_t base_work = uint64_t(source.positions.size()) * 64 + uint64_t(source.indices.size()) * 96 + 65536;
        if (base_work > budget.max_known_work_bytes)
            return fail(error_code::capacity, "known work budget too small");
        auto content = source_digest(source);
        if (!content)
            return std::unexpected(content.error());
        auto leaf = bd::cluster_lod0(source, options);
        if (!leaf)
            return std::unexpected(leaf.error());
        payload data;
        data.indices = std::move(leaf->indices);
        data.clusters = std::move(leaf->clusters);
        data.source_hash = *content;
        data.build_hash = *identity;
        data.leaves.resize(data.clusters.size());
        std::iota(data.leaves.begin(), data.leaves.end(), 0u);
        auto active = data.leaves;
        auto scale = bd::measure_scale(source);
        auto topology = bd::analyze_topology(source);
        build_report report;
        report.known_work_peak_bytes = base_work;
        report.levels.push_back({source.indices.size() / 3, active.size(), 0, active.size()});
        if (storage(data) > budget.max_output_bytes || storage(data) > budget.max_known_work_bytes - base_work)
            return fail(error_code::capacity, "initial payload exceeds build budget");
        for (uint32_t round = 0; round < options.max_rounds; ++round) {
            vector<bd::cluster_input> views;
            views.reserve(active.size());
            for (auto id : active) {
                auto const& c = data.clusters[id];
                views.push_back({std::span(data.indices).subspan(c.first_index, c.index_count)});
            }
            auto partitions = bd::partition_clusters(source, views, options, scale);
            if (!partitions)
                return std::unexpected(partitions.error());
            views.clear();
            vector<uint32_t> next;
            bool progressed = false;
            report.rounds = round + 1;
            for (auto const& partition : *partitions) {
                vector<uint32_t> fine;
                vector<bd::cluster_input> inputs;
                fine.reserve(partition.size());
                inputs.reserve(partition.size());
                for (auto index : partition) {
                    auto id = active[index];
                    fine.push_back(id);
                    auto const& c = data.clusters[id];
                    inputs.push_back({std::span(data.indices).subspan(c.first_index, c.index_count)});
                }
                auto candidate = bd::reduce_group(source, inputs, options, scale, &topology);
                if (!candidate)
                    return std::unexpected(candidate.error());
                if (!candidate->accepted()) {
                    ++report.rejected[static_cast<size_t>(candidate->reason)];
                    next.insert(next.end(), fine.begin(), fine.end());
                    continue;
                }
                auto lod = lod_sphere(source, data, fine, candidate->coarse);
                float previous = 0;
                for (auto id : fine)
                    if (data.clusters[id].producer != none)
                        previous = std::max(previous, data.groups[data.clusters[id].producer].error);
                double cumulative = double(previous) + candidate->local_error;
                float error = static_cast<float>(cumulative);
                if (double(error) < cumulative)
                    error = std::nextafter(error, std::numeric_limits<float>::infinity());
                if (!lod || !std::isfinite(error)) {
                    ++report.rejected[static_cast<size_t>(bd::rejection_reason::numeric_error)];
                    next.insert(next.end(), fine.begin(), fine.end());
                    continue;
                }
                auto const& coarse = candidate->coarse;
                uint64_t additional = uint64_t(coarse.indices.size()) * 4 + uint64_t(coarse.clusters.size()) * 36 + 48 + uint64_t(fine.size() + coarse.clusters.size()) * 4;
                auto used = storage(data);
                if (used > budget.max_output_bytes || additional > budget.max_output_bytes - used || used > budget.max_known_work_bytes - base_work || additional > budget.max_known_work_bytes - base_work - used || uint64_t(data.indices.size()) + coarse.indices.size() > UINT32_MAX || uint64_t(data.clusters.size()) + coarse.clusters.size() >= none || uint64_t(data.references.size()) + fine.size() + coarse.clusters.size() > UINT32_MAX || data.groups.size() + 1 >= none)
                    return fail(error_code::capacity, "DAG append exceeds build budget");
                auto group_id = static_cast<uint32_t>(data.groups.size()), first_cluster = static_cast<uint32_t>(data.clusters.size()), first_index = static_cast<uint32_t>(data.indices.size()), first_ref = static_cast<uint32_t>(data.references.size());
                data.indices.insert(data.indices.end(), coarse.indices.begin(), coarse.indices.end());
                for (auto c : coarse.clusters) {
                    c.first_index += first_index;
                    c.producer = group_id;
                    data.clusters.push_back(c);
                }
                data.references.insert(data.references.end(), fine.begin(), fine.end());
                for (auto id : fine)
                    data.clusters[id].consumer = group_id;
                for (uint32_t i = 0; i < coarse.clusters.size(); ++i) {
                    data.references.push_back(first_cluster + i);
                    next.push_back(first_cluster + i);
                }
                data.groups.push_back({first_ref, static_cast<uint32_t>(fine.size()), first_ref + static_cast<uint32_t>(fine.size()), static_cast<uint32_t>(coarse.clusters.size()), *lod, error});
                progressed = true;
                report.known_work_peak_bytes = std::max(report.known_work_peak_bytes, base_work + storage(data));
            }
            std::sort(next.begin(), next.end());
            active = std::move(next);
            if (!progressed)
                break;
            uint64_t triangles = 0;
            for (auto id : active)
                triangles += data.clusters[id].index_count / 3;
            report.levels.push_back({triangles, active.size(), data.groups.size(), active.size()});
        }
        data.roots = active;
        report.lod0_only = data.groups.empty();
        for (auto const& g : data.groups) {
            uint32_t first = none;
            bool cross = false;
            for (auto id : std::span(data.references).subspan(g.coarse_first, g.coarse_count)) {
                auto consumer = data.clusters[id].consumer;
                if (consumer == none)
                    continue;
                if (first == none)
                    first = consumer;
                else if (first != consumer)
                    cross = true;
            }
            if (cross)
                ++report.cross_group_repartitions;
        }
        data.build_meta = metadata(options, report);
        auto wire = encode(data, validation_limits);
        if (!wire)
            return std::unexpected(wire.error());
        auto validated = make_asset(std::move(source), std::move(data), validation_limits);
        if (!validated)
            return std::unexpected(validated.error());
        report.wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return build_result{std::move(*validated), std::move(report)};
    }
    result<build_result> build(source_geometry source, build_options const& options, execution_options const& execution) {
        if (execution.threads != 1)
            return fail(error_code::invalid_options, "only --threads 1 is available before parallel scheduling");
        return build_serial(std::move(source), options, execution.limits);
    }
} // namespace deren::asset::subtilis
