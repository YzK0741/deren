module;
#include "graph_backend_headers.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <optional>
#include <variant>
module deren.engine.render_graph;
namespace mem = deren::utility::memory;
namespace vessel = deren::utility::vessel;
#include "graph_backend.hpp"
#include "range_operations.hpp"
#include "synchronization.hpp"
namespace deren::engine::render_graph {
    namespace {
        std::atomic_uint64_t next_incarnation{1};
        uint64_t allocate_incarnation() {
            auto current = next_incarnation.load(std::memory_order_relaxed);
            for (;;) {
                if (current == std::numeric_limits<uint64_t>::max()) {
                    deren::utility::fast_panic("render graph incarnation exhausted");
                }
                if (next_incarnation.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
                    return current;
                }
            }
        }
        bool consumes(content_access mode) noexcept {
            return mode == content_access::read || mode == content_access::preserve_write || mode == content_access::read_write;
        }
    } // namespace
    builder::builder()
        : incarnation(allocate_incarnation()) {
    }
    graph_error builder::error(error_code code, uint32_t pass, uint32_t resource) const {
        return {code, pass, resource, pass < passes.size() ? passes[pass].name : vessel::string{}, resource < resources.size() ? resources[resource].name : vessel::string{}, std::nullopt};
    }
    std::expected<uint32_t, graph_error> builder::import(resource_identity id, initial_contents contents, std::string_view name, resource_kind kind, resource_shape shape, std::optional<resource_state> state) {
        if (frozen) {
            return std::unexpected(error(error_code::frozen_builder));
        }
        auto invalid = [&](error_code code) {auto diagnostic=error(code);diagnostic.resource_name=name;return std::unexpected(std::move(diagnostic)); };
        if (!id.device || !id.object) {
            return invalid(error_code::invalid_identity);
        }
        if (!detail::valid_shape(shape)) {
            return invalid(error_code::invalid_shape);
        }
        if (contents != initial_contents::defined && contents != initial_contents::undefined) {
            return invalid(error_code::conflicting_import);
        }
        if (state && !detail::valid_initial_state(kind, shape, contents, *state)) {
            return invalid(error_code::invalid_scope);
        }
        if (auto found = identities.find(id); found != identities.end()) {
            auto index = found->second;
            if (resources[index].kind != kind || resources[index].initial != contents || resources[index].shape != shape || resources[index].initial_state != state) {
                return std::unexpected(error(error_code::conflicting_import, invalid_index, index));
            }
            return index;
        }
        if (resources.size() >= invalid_index) {
            return invalid(error_code::capacity_exceeded);
        }
        auto index = static_cast<uint32_t>(resources.size());
        resource_record record{};
        record.identity = id;
        record.kind = kind;
        record.initial = contents;
        record.name = name;
        record.defined = contents == initial_contents::defined;
        record.shape = std::move(shape);
        record.initial_state = state;
        resources.push_back(std::move(record));
        identities.emplace(id, index);
        return index;
    }
    std::expected<texture_ref, graph_error> builder::import_texture(resource_identity id, initial_contents contents, std::string_view name, texture_shape shape, std::optional<resource_state> state) {
        auto result = import(id, contents, name, resource_kind::texture, shape, state);
        if (!result) {
            return std::unexpected(result.error());
        }
        return texture_ref{incarnation, *result};
    }
    std::expected<buffer_ref, graph_error> builder::import_buffer(resource_identity id, initial_contents contents, std::string_view name, buffer_shape shape, std::optional<resource_state> state) {
        auto result = import(id, contents, name, resource_kind::buffer, shape, state);
        if (!result) {
            return std::unexpected(result.error());
        }
        return buffer_ref{incarnation, *result};
    }
    std::expected<pass_ref, graph_error> builder::add_pass(std::string_view name, std::span<resource_access const> accesses, pass_options options) {
        if (frozen) {
            return std::unexpected(error(error_code::frozen_builder));
        }
        if (passes.size() >= invalid_index) {
            return std::unexpected(error(error_code::capacity_exceeded));
        }
        pass_record candidate{vessel::string(name), {}, options.side_effect, false};
        for (auto const& access : accesses) {
            auto fail = [&](error_code code, resource_range const& range) {
                auto diagnostic = error(code, invalid_index, access.incarnation_ == incarnation ? access.resource_ : invalid_index);
                diagnostic.pass_name = candidate.name;
                diagnostic.range = range;
                return std::unexpected(std::move(diagnostic));
            };
            if (!access.incarnation_) {
                return fail(error_code::invalid_ref, access.range_);
            }
            if (access.incarnation_ != incarnation) {
                return fail(error_code::foreign_ref, access.range_);
            }
            if (access.resource_ >= resources.size() || resources[access.resource_].kind != access.kind_) {
                return fail(error_code::invalid_ref, access.range_);
            }
            if (access.mode_ < content_access::read || access.mode_ > content_access::invalidate) {
                return fail(error_code::conflicting_access, access.range_);
            }
            auto normalized = detail::normalize(resources[access.resource_].shape, access.range_);
            if (!normalized) {
                return fail(normalized.error(), access.range_);
            }
            if (!detail::valid_scope(access.kind_, *normalized, access.mode_, access.scope_)) {
                return fail(error_code::invalid_scope, *normalized);
            }
            // Normalize the union into nonoverlapping operations, including read-write.
            vessel::vector<resource_range> pending{*normalized};
            for (auto const& previous : candidate.accesses) {
                if (previous.resource != access.resource_) {
                    continue;
                }
                auto overlap = detail::intersection(previous.range, *normalized);
                if (!overlap) {
                    continue;
                }
                if (previous.mode != access.mode_ || previous.scope != access.scope_) {
                    return fail(error_code::conflicting_access, *overlap);
                }
                vessel::vector<resource_range> remainder;
                for (auto const& range : pending) {
                    auto parts = detail::partition(range, previous.range);
                    for (std::size_t i = 0; i < parts.count; ++i) {
                        remainder.push_back(parts.outside[i]);
                    }
                }
                pending = std::move(remainder);
                if (pending.empty()) {
                    break;
                }
            }
            for (auto& range : pending) {
                candidate.accesses.push_back({access.resource_, access.mode_, std::move(range), access.scope_});
            }
        }
        auto index = static_cast<uint32_t>(passes.size());
        passes.push_back(std::move(candidate));
        return pass_ref{incarnation, index};
    }
    std::expected<void, graph_error> builder::export_index(uint64_t owner, uint32_t index, resource_kind kind, resource_range range) {
        if (frozen) {
            return std::unexpected(error(error_code::frozen_builder));
        }
        if (!owner) {
            return std::unexpected(error(error_code::invalid_ref));
        }
        if (owner != incarnation) {
            return std::unexpected(error(error_code::foreign_ref));
        }
        if (index >= resources.size() || resources[index].kind != kind) {
            return std::unexpected(error(error_code::invalid_ref));
        }
        auto normalized = detail::normalize(resources[index].shape, range);
        if (!normalized) {
            auto diagnostic = error(normalized.error(), invalid_index, index);
            diagnostic.range = range;
            return std::unexpected(std::move(diagnostic));
        }
        resource_export output{index, *normalized};
        if (std::find(exports.begin(), exports.end(), output) == exports.end()) {
            exports.push_back(std::move(output));
        }
        return {};
    }
    std::expected<void, graph_error> builder::export_resource(texture_ref ref, texture_range range) {
        return export_index(ref.incarnation_, ref.index_, resource_kind::texture, range);
    }
    std::expected<void, graph_error> builder::export_resource(buffer_ref ref, buffer_range range) {
        return export_index(ref.incarnation_, ref.index_, resource_kind::buffer, range);
    }
    std::expected<void, graph_error> builder::order_after(pass_ref pass, pass_ref predecessor) {
        if (frozen) {
            return std::unexpected(error(error_code::frozen_builder));
        }
        if (!pass.incarnation_ || !predecessor.incarnation_) {
            return std::unexpected(error(error_code::invalid_ref));
        }
        if (pass.incarnation_ != incarnation || predecessor.incarnation_ != incarnation) {
            return std::unexpected(error(error_code::foreign_ref));
        }
        if (pass.index_ >= passes.size() || predecessor.index_ >= passes.size()) {
            return std::unexpected(error(error_code::invalid_ref));
        }
        auto edge = std::pair{predecessor.index_, pass.index_};
        if (std::find(orders.begin(), orders.end(), edge) == orders.end()) {
            orders.push_back(edge);
        }
        return {};
    }
    std::expected<compiled_graph, graph_error> builder::compile(compile_options options) {
        if (frozen) {
            return std::unexpected(error(error_code::frozen_builder));
        }
        frozen = true;
        compiled_graph result{resources, passes, {}, {}, {}, exports};
        ::graph explicit_graph(passes.size());
        for (auto [from, to] : orders) {
            boost::add_edge(from, to, explicit_graph);
        }
        vessel::vector<boost::default_color_type> all_live(passes.size(), boost::black_color);
        ::vertex blocked = invalid_index;
        if (!::stable_order(explicit_graph, {all_live.data()}, &blocked)) {
            return std::unexpected(error(error_code::cycle, static_cast<uint32_t>(blocked)));
        }
        ::graph values(passes.size());
        for (auto& resource : result.resources) {
            resource.final_ranges.push_back({detail::full_range(resource.shape), invalid_index, 0, resource.initial == initial_contents::defined});
        }
        vessel::vector<uint32_t> last_write_pass(resources.size(), invalid_index);
        for (uint32_t p = 0; p < passes.size(); ++p) {
            for (auto const& access : passes[p].accesses) {
                auto& resource = result.resources[access.resource];
                if (consumes(access.mode)) {
                    for (auto const& contents : resource.final_ranges) {
                        auto range = detail::intersection(contents.range, access.range);
                        if (!range) {
                            continue;
                        }
                        result.reads.push_back({p, access.resource, contents.producer, contents.version, contents.defined, *range});
                        if (contents.producer != invalid_index) {
                            boost::add_edge(contents.producer, p, values);
                            result.dependencies.push_back({contents.producer, p, access.resource, dependency_reason::value, *range});
                        }
                    }
                }
                if (access.mode != content_access::read) {
                    if (last_write_pass[access.resource] != p) {
                        ++resource.last_version;
                        last_write_pass[access.resource] = p;
                    }
                    auto touched = detail::isolate(resource.final_ranges, access.range);
                    for (auto index : touched) {
                        auto& contents = resource.final_ranges[index];
                        contents.producer = p;
                        contents.version = resource.last_version;
                        contents.defined = access.mode != content_access::invalidate;
                    }
                }
            }
        }
        for (auto& resource : result.resources) {
            resource.defined = std::all_of(resource.final_ranges.begin(), resource.final_ranges.end(), [](auto const& state) { return state.defined; });
            resource.last_producer = resource.final_ranges.front().producer;
            for (auto const& state : resource.final_ranges) {
                if (state.producer != resource.last_producer) {
                    resource.last_producer = invalid_index;
                    break;
                }
            }
        }
        vessel::vector<::vertex> roots;
        for (uint32_t p = 0; p < passes.size(); ++p) {
            if (passes[p].side_effect) {
                roots.push_back(p);
            }
        }
        for (auto const& output : exports) {
            for (auto const& contents : result.resources[output.resource].final_ranges) {
                auto range = detail::intersection(contents.range, output.range);
                if (!range) {
                    continue;
                }
                if (!contents.defined) {
                    auto diagnostic = error(error_code::undefined_export, contents.producer, output.resource);
                    diagnostic.range = *range;
                    return std::unexpected(std::move(diagnostic));
                }
                if (contents.producer != invalid_index) {
                    roots.push_back(contents.producer);
                }
            }
        }
        auto live = ::reachable(values, roots);
        for (uint32_t p = 0; p < passes.size(); ++p) {
            result.passes[p].live = live[p] != boost::white_color;
        }
        for (auto const& read : result.reads) {
            if (result.passes[read.pass].live && !read.defined) {
                auto diagnostic = error(error_code::undefined_read, read.pass, read.resource);
                diagnostic.range = read.range;
                return std::unexpected(std::move(diagnostic));
            }
        }
        std::erase_if(result.dependencies, [&](auto const& edge) { return !result.passes[edge.from].live || !result.passes[edge.to].live; });
        ::graph scheduled(passes.size());
        for (auto const& edge : result.dependencies) {
            boost::add_edge(edge.from, edge.to, scheduled);
        }
        auto dependency = [&](uint32_t from, uint32_t to, uint32_t resource, dependency_reason reason, resource_range const& range) {
            if (from == invalid_index || from == to) {
                return;
            }
            boost::add_edge(from, to, scheduled);
            result.dependencies.push_back({from, to, resource, reason, range});
        };
        struct hazard_state {
            resource_range range;
            uint32_t writer = invalid_index;
            vessel::vector<uint32_t> readers;
            std::optional<resource_layout> layout = std::nullopt;
        };
        vessel::vector<vessel::vector<hazard_state>> hazards(resources.size());
        for (std::size_t r = 0; r < resources.size(); ++r) {
            hazards[r].push_back({detail::full_range(resources[r].shape), invalid_index, {}});
        }
        for (uint32_t p = 0; p < passes.size(); ++p) {
            if (!result.passes[p].live) {
                continue;
            }
            for (auto const& access : passes[p].accesses) {
                auto& states = hazards[access.resource];
                auto touched = detail::isolate(states, access.range);
                for (auto index : touched) {
                    auto& state = states[index];
                    if (resources[access.resource].kind == resource_kind::texture && access.scope.usage != resource_usage::unspecified) {
                        auto layout = detail::layout_for(resource_kind::texture, access.scope.usage);
                        if (state.layout && *state.layout != layout) {
                            dependency(state.writer, p, access.resource, dependency_reason::state_transition, state.range);
                            for (auto reader : state.readers) {
                                dependency(reader, p, access.resource, dependency_reason::state_transition, state.range);
                            }
                        }
                        state.layout = layout;
                    } else {
                        state.layout.reset();
                    }
                    if (consumes(access.mode)) {
                        dependency(state.writer, p, access.resource, dependency_reason::raw, state.range);
                    }
                    if (access.mode != content_access::read) {
                        dependency(state.writer, p, access.resource, dependency_reason::waw, state.range);
                        for (auto reader : state.readers) {
                            dependency(reader, p, access.resource, dependency_reason::war, state.range);
                        }
                        state.readers.clear();
                        state.writer = p;
                    } else {
                        state.readers.push_back(p);
                    }
                }
            }
        }
        for (auto [from, to] : orders) {
            if (result.passes[from].live && result.passes[to].live) {
                boost::add_edge(from, to, scheduled);
                result.dependencies.push_back({from, to, invalid_index, dependency_reason::explicit_order, std::nullopt});
            }
        }
        auto sorted = ::stable_order(scheduled, {live.data()}, &blocked);
        if (!sorted) {
            return std::unexpected(error(error_code::cycle, static_cast<uint32_t>(blocked)));
        }
        for (auto pass : *sorted) {
            result.execution_order.push_back(static_cast<uint32_t>(pass));
        }
        if (options.synchronization) {
            auto planned = detail::plan_synchronization(result);
            if (!planned) {
                return std::unexpected(std::move(planned.error()));
            }
        }
        return result;
    }
} // namespace deren::engine::render_graph
