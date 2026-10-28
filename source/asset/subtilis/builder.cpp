module;
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <yyjson.h>
module deren.asset.subtilis.builder;

namespace deren::asset::subtilis {
    namespace {
        namespace bd = builder_detail;
        double stage_now() {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
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
                utility::fast_panic("subtilis metadata serialization failed");
            string out(bytes, length);
            utility::memory::deallocate(bytes);
            yyjson_mut_doc_free(d);
            return out;
        }
        // ---------------------------------------------------------------- structural stage profiling
        enum class stage_kind : size_t { source_fingerprint,
                                         leaf_cluster,
                                         topology,
                                         partition,
                                         boundary,
                                         simplify,
                                         recluster,
                                         candidate,
                                         commit,
                                         validate,
                                         encode,
                                         count };
        constexpr size_t stage_kinds = size_t(stage_kind::count);
        struct stage_definition {
            char const* name;
            bool inclusive;
        };
        // Fixed order and fixed names: rows are complete on every successful build, including
        // LOD0-only assets, so the report cannot silently miss a stage. `candidate` is the only
        // inclusive row; its children boundary/simplify/recluster overlap it and are excluded from
        // the accumulated total, so nothing is double counted.
        constexpr array<stage_definition, stage_kinds> stage_definitions{{
            {"sourceFingerprint", false},
            {"leafCluster", false},
            {"topology", false},
            {"partition", false},
            {"boundary", false},
            {"simplify", false},
            {"recluster", false},
            {"candidate", true},
            {"commit", false},
            {"validate", false},
            {"encode", false},
        }};
        struct stage_accumulator {
            array<double, stage_kinds> cpu{};
            array<uint64_t, stage_kinds> count{};
            void add(stage_kind kind, double milliseconds, uint64_t tasks = 1) noexcept {
                auto at = size_t(kind);
                cpu[at] += milliseconds;
                count[at] += tasks;
            }
        };
        vector<stage_profile> profile_rows(stage_accumulator const& stages) {
            vector<stage_profile> rows;
            rows.reserve(stage_kinds);
            for (size_t at = 0; at < stage_kinds; ++at)
                rows.push_back({string(stage_definitions[at].name), stages.cpu[at], stage_definitions[at].inclusive, stages.count[at]});
            return rows;
        }
        double accumulated_milliseconds(stage_accumulator const& stages) {
            double total = 0;
            for (size_t at = 0; at < stage_kinds; ++at)
                if (!stage_definitions[at].inclusive)
                    total += stages.cpu[at];
            return total;
        }
        // Conservative per-task known-work estimate: input windows copied by the candidate plus the
        // simplified/recluster scratch and the bounded output refs. Used only for admission and
        // concurrency, never for identity.
        size_t reduction_task_estimate(std::span<bd::cluster_input const> inputs) {
            size_t indices = 0;
            for (auto input : inputs)
                indices += input.indices.size();
            return 65536 + indices * 12;
        }
        struct prepared_group {
            bd::reduction_candidate candidate;
            bd::reduce_timing timing;
            double candidate_ms = 0;
            bd::rejection_reason reason = bd::rejection_reason::no_progress;
            sphere lod{};
            float error = 0;
            uint64_t additional = 0;
        };
        struct round_workspace {
            vector<vector<uint32_t>> fine;
            vector<vector<bd::cluster_input>> inputs;
            vector<prepared_group> prepared;
            vector<uint32_t> next;
            bool progressed = false;
        };
        // Phase 1 of a reduction round: only immutable inputs and the caller's own slot are touched,
        // so this runs concurrently and its completion order cannot matter.
        result<void> prepare_group(round_workspace& workspace, size_t group, source_geometry const& source, payload const& data, bd::topology const& topology, bd::primitive_scale const& scale, build_options const& options) {
            double const started = stage_now();
            auto& slot = workspace.prepared[group];
            auto candidate = bd::reduce_group(source, workspace.inputs[group], options, scale, &topology, &slot.timing);
            if (!candidate) {
                slot.candidate_ms = stage_now() - started;
                return std::unexpected(candidate.error());
            }
            slot.candidate = std::move(*candidate);
            slot.reason = slot.candidate.accepted() ? bd::rejection_reason::none : slot.candidate.reason;
            if (slot.reason == bd::rejection_reason::none) {
                auto lod = lod_sphere(source, data, workspace.fine[group], slot.candidate.coarse);
                float previous = 0;
                for (auto id : workspace.fine[group])
                    if (data.clusters[id].producer != none)
                        previous = std::max(previous, data.groups[data.clusters[id].producer].error);
                double cumulative = double(previous) + slot.candidate.local_error;
                float error = static_cast<float>(cumulative);
                if (double(error) < cumulative)
                    error = std::nextafter(error, std::numeric_limits<float>::infinity());
                if (!lod || !std::isfinite(error)) {
                    slot.reason = bd::rejection_reason::numeric_error;
                } else {
                    slot.lod = *lod;
                    slot.error = error;
                    auto const& coarse = slot.candidate.coarse;
                    slot.additional = uint64_t(coarse.indices.size()) * 4 + uint64_t(coarse.clusters.size()) * 36 + 48 + uint64_t(workspace.fine[group].size() + coarse.clusters.size()) * 4;
                }
            }
            slot.candidate_ms = stage_now() - started;
            return {};
        }
        // Phase 2 of a reduction round: only the scheduling thread appends to the payload, in the
        // fixed partition order, so global IDs do not depend on task completion order.
        result<void> commit_group(round_workspace& workspace, size_t group, payload& data, build_limits const& budget, build_report& report, stage_accumulator& stages, uint64_t base_work) {
            double const started = stage_now();
            auto& slot = workspace.prepared[group];
            stages.add(stage_kind::boundary, slot.timing.boundary_ms);
            stages.add(stage_kind::simplify, slot.timing.simplify_ms);
            stages.add(stage_kind::recluster, slot.timing.recluster_ms);
            stages.add(stage_kind::candidate, slot.candidate_ms);
            if (slot.reason != bd::rejection_reason::none) {
                ++report.rejected[static_cast<size_t>(slot.reason)];
                workspace.next.insert(workspace.next.end(), workspace.fine[group].begin(), workspace.fine[group].end());
                stages.add(stage_kind::commit, stage_now() - started);
                return {};
            }
            auto const& coarse = slot.candidate.coarse;
            auto used = storage(data);
            if (used > budget.max_output_bytes || slot.additional > budget.max_output_bytes - used || used > budget.max_known_work_bytes - base_work || slot.additional > budget.max_known_work_bytes - base_work - used || uint64_t(data.indices.size()) + coarse.indices.size() > UINT32_MAX || uint64_t(data.clusters.size()) + coarse.clusters.size() >= none || uint64_t(data.references.size()) + workspace.fine[group].size() + coarse.clusters.size() > UINT32_MAX || data.groups.size() + 1 >= none) {
                stages.add(stage_kind::commit, stage_now() - started);
                return fail(error_code::capacity, "DAG append exceeds build budget");
            }
            auto group_id = static_cast<uint32_t>(data.groups.size()), first_cluster = static_cast<uint32_t>(data.clusters.size()), first_index = static_cast<uint32_t>(data.indices.size()), first_ref = static_cast<uint32_t>(data.references.size());
            data.indices.insert(data.indices.end(), coarse.indices.begin(), coarse.indices.end());
            for (auto c : coarse.clusters) {
                c.first_index += first_index;
                c.producer = group_id;
                data.clusters.push_back(c);
            }
            data.references.insert(data.references.end(), workspace.fine[group].begin(), workspace.fine[group].end());
            for (auto id : workspace.fine[group])
                data.clusters[id].consumer = group_id;
            for (uint32_t i = 0; i < coarse.clusters.size(); ++i) {
                data.references.push_back(first_cluster + i);
                workspace.next.push_back(first_cluster + i);
            }
            data.groups.push_back({first_ref, static_cast<uint32_t>(workspace.fine[group].size()), first_ref + static_cast<uint32_t>(workspace.fine[group].size()), static_cast<uint32_t>(coarse.clusters.size()), slot.lod, slot.error});
            workspace.progressed = true;
            report.known_work_peak_bytes = std::max(report.known_work_peak_bytes, base_work + storage(data));
            stages.add(stage_kind::commit, stage_now() - started);
            return {};
        }
        struct round_context {
            source_geometry const& source;
            payload& data;
            build_options const& options;
            build_limits const& budget;
            bd::topology const& topology;
            bd::primitive_scale const& scale;
            uint64_t base_work = 0;
            uint32_t threads = 1;
            build_report& report;
            stage_accumulator& stages;
            uint32_t& planned_concurrency;
            vector<uint32_t>& active;
        };
        // One frontier partition -> candidates -> stable commit round. threads<=1 (or a single
        // group) is the serial reference interleaving; threads>1 uses the bounded scheduler where
        // candidates are prepared concurrently and committed afterwards in partition order.
        result<bool> run_round(round_context& context) {
            double const partition_started = stage_now();
            vector<bd::cluster_input> views;
            views.reserve(context.active.size());
            for (auto id : context.active) {
                auto const& c = context.data.clusters[id];
                views.push_back({std::span(context.data.indices).subspan(c.first_index, c.index_count)});
            }
            auto partitions = bd::partition_clusters(context.source, views, context.options, context.scale);
            context.stages.add(stage_kind::partition, stage_now() - partition_started);
            if (!partitions)
                return std::unexpected(partitions.error());
            views.clear();
            round_workspace workspace;
            auto const groups = partitions->size();
            workspace.fine.resize(groups);
            workspace.inputs.resize(groups);
            workspace.prepared.resize(groups);
            for (size_t group = 0; group < groups; ++group) {
                auto const& partition = (*partitions)[group];
                workspace.fine[group].reserve(partition.size());
                for (auto index : partition)
                    workspace.fine[group].push_back(context.active[index]);
            }
            // Input spans point into the payload's index vector, which commits grow (and therefore
            // may relocate). Serial commits are interleaved with prepares, so they rebuild their
            // spans just-in-time; the parallel path prepares everything before any commit and can
            // share one stable set of spans.
            auto fill_inputs = [&](size_t group) {
                auto& inputs = workspace.inputs[group];
                inputs.clear();
                inputs.reserve(workspace.fine[group].size());
                for (auto id : workspace.fine[group]) {
                    auto const& c = context.data.clusters[id];
                    inputs.push_back({std::span(context.data.indices).subspan(c.first_index, c.index_count)});
                }
            };
            auto commit_task = [&](size_t group) -> result<void> {
                return commit_group(workspace, group, context.data, context.budget, context.report, context.stages, context.base_work);
            };
            if (context.threads <= 1 || groups <= 1) {
                for (size_t group = 0; group < groups; ++group) {
                    fill_inputs(group);
                    auto prepared = prepare_group(workspace, group, context.source, context.data, context.topology, context.scale, context.options);
                    if (!prepared)
                        return std::unexpected(prepared.error());
                    auto committed = commit_task(group);
                    if (!committed)
                        return std::unexpected(committed.error());
                }
            } else {
                vector<size_t> estimates(groups);
                size_t max_estimate = 1;
                for (size_t group = 0; group < groups; ++group) {
                    fill_inputs(group);
                    estimates[group] = reduction_task_estimate(workspace.inputs[group]);
                    max_estimate = std::max(max_estimate, estimates[group]);
                }
                size_t const available = context.budget.max_known_work_bytes - context.base_work;
                size_t const admitted = std::min({size_t(context.threads), groups, available / max_estimate});
                auto prepare_task = [&](size_t group) -> result<void> {
                    return prepare_group(workspace, group, context.source, context.data, context.topology, context.scale, context.options);
                };
                auto ordered = bd::run_ordered(groups, context.threads, estimates, available, prepare_task, commit_task);
                if (!ordered)
                    return std::unexpected(ordered.error());
                // Deterministic planned concurrency: a pure function of the estimates and budget.
                context.planned_concurrency = std::max(context.planned_concurrency, static_cast<uint32_t>(std::max<size_t>(admitted, 1)));
            }
            std::sort(workspace.next.begin(), workspace.next.end());
            context.active = std::move(workspace.next);
            if (!workspace.progressed)
                return false;
            uint64_t triangles = 0;
            for (auto id : context.active)
                triangles += context.data.clusters[id].index_count / 3;
            context.report.levels.push_back({triangles, context.active.size(), context.data.groups.size(), context.active.size()});
            return true;
        }
        result<build_result> build_impl(source_geometry source, build_options const& options, build_limits const& budget, uint32_t threads) {
            double const started = stage_now();
            stage_accumulator stages;
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
            double const fingerprint_started = stage_now();
            auto content = source_digest(source);
            stages.add(stage_kind::source_fingerprint, stage_now() - fingerprint_started);
            if (!content)
                return std::unexpected(content.error());
            // One process-wide allocator initialization before any concurrent task can touch
            // meshoptimizer; tasks never swap the global allocator callbacks.
            bd::initialize_meshoptimizer();
            double const leaf_started = stage_now();
            auto leaf = bd::cluster_lod0(source, options);
            stages.add(stage_kind::leaf_cluster, stage_now() - leaf_started);
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
            double const topology_started = stage_now();
            auto topology = bd::analyze_topology(source);
            stages.add(stage_kind::topology, stage_now() - topology_started);
            build_report report;
            report.threads = threads;
            report.known_work_limit_bytes = budget.max_known_work_bytes;
            report.known_work_peak_bytes = base_work;
            report.levels.push_back({source.indices.size() / 3, active.size(), 0, active.size()});
            if (storage(data) > budget.max_output_bytes || storage(data) > budget.max_known_work_bytes - base_work)
                return fail(error_code::capacity, "initial payload exceeds build budget");
            uint32_t planned_concurrency = 1;
            for (uint32_t round = 0; round < options.max_rounds; ++round) {
                report.rounds = round + 1;
                round_context context{source, data, options, budget, topology, scale, base_work, threads, report, stages, planned_concurrency, active};
                auto progressed = run_round(context);
                if (!progressed)
                    return std::unexpected(progressed.error());
                if (!*progressed)
                    break;
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
            double const validate_started = stage_now();
            auto validated = make_asset(std::move(source), std::move(data), validation_limits);
            stages.add(stage_kind::validate, stage_now() - validate_started);
            if (!validated)
                return std::unexpected(validated.error());
            double const encode_started = stage_now();
            auto wire = encode(validated->data(), validation_limits);
            stages.add(stage_kind::encode, stage_now() - encode_started);
            if (!wire)
                return std::unexpected(wire.error());
            report.planned_concurrency = planned_concurrency;
            report.accumulated_ms = accumulated_milliseconds(stages);
            report.stages = profile_rows(stages);
            report.wall_ms = stage_now() - started;
            return build_result{std::move(*validated), std::move(report)};
        }
    } // namespace
    result<build_result> build_serial(source_geometry source, build_options const& options, build_limits const& budget) {
        return build_impl(std::move(source), options, budget, 1);
    }
    result<build_result> build(source_geometry source, build_options const& options, execution_options const& execution) {
        if (execution.threads == 0)
            return fail(error_code::invalid_options, "thread count must be at least 1");
        return build_impl(std::move(source), options, execution.limits, execution.threads);
    }
} // namespace deren::asset::subtilis
