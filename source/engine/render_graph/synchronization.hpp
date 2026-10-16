#pragma once
// Internal single-queue CPU synchronization model; no native/RHI masks.
namespace deren::engine::render_graph::detail {
    inline uint32_t bits(pipeline_stage value) noexcept {
        return static_cast<uint32_t>(value);
    }
    inline uint32_t bits(memory_access value) noexcept {
        return static_cast<uint32_t>(value);
    }
    inline bool writes(memory_access access) noexcept {
        return (bits(access) & bits(memory_access::write)) != 0;
    }
    inline bool shader_stages(pipeline_stage stages) noexcept {
        return bits(stages) && !(bits(stages) & ~(7U | 256U | 512U));
    }
    inline memory_access access_for(content_access mode) noexcept {
        if (mode == content_access::read)
            return memory_access::read;
        if (mode == content_access::discard_write)
            return memory_access::write;
        if (mode == content_access::preserve_write || mode == content_access::read_write)
            return memory_access::read | memory_access::write;
        return memory_access::none;
    }
    inline resource_layout layout_for(resource_kind kind, resource_usage usage) noexcept {
        if (kind == resource_kind::buffer)
            return resource_layout::general;
        switch (usage) {
        case resource_usage::sampled:
            return resource_layout::shader_read_only;
        case resource_usage::color_attachment:
            return resource_layout::color_attachment;
        case resource_usage::depth_stencil_attachment:
            return resource_layout::depth_stencil_attachment;
        case resource_usage::depth_stencil_read:
            return resource_layout::depth_stencil_read_only;
        case resource_usage::transfer_source:
            return resource_layout::transfer_source;
        case resource_usage::transfer_destination:
            return resource_layout::transfer_destination;
        case resource_usage::present:
            return resource_layout::present;
        default:
            return resource_layout::general;
        }
    }
    inline bool valid_scope(resource_kind kind, resource_range const& range, content_access mode, access_scope scope) noexcept {
        if (scope.usage == resource_usage::unspecified)
            return scope.stages == pipeline_stage::none;
        if (mode == content_access::invalidate)
            return false;
        auto readonly = mode == content_access::read;
        auto aspect = kind == resource_kind::texture ? bits(std::get<texture_range>(range).aspects) : 0U;
        switch (scope.usage) {
        case resource_usage::sampled:
            return kind == resource_kind::texture && readonly && shader_stages(scope.stages);
        case resource_usage::storage:
            return (kind == resource_kind::buffer || aspect == 1) && shader_stages(scope.stages);
        case resource_usage::uniform_buffer:
            return kind == resource_kind::buffer && readonly && shader_stages(scope.stages);
        case resource_usage::color_attachment:
            return kind == resource_kind::texture && aspect == 1 && !readonly && scope.stages == pipeline_stage::color_output;
        case resource_usage::depth_stencil_attachment:
            return kind == resource_kind::texture && !(aspect & 1) && !readonly && scope.stages == pipeline_stage::depth_stencil;
        case resource_usage::depth_stencil_read:
            return kind == resource_kind::texture && !(aspect & 1) && readonly && scope.stages == pipeline_stage::depth_stencil;
        case resource_usage::transfer_source:
            return readonly && scope.stages == pipeline_stage::transfer;
        case resource_usage::transfer_destination:
            return mode == content_access::discard_write && scope.stages == pipeline_stage::transfer;
        case resource_usage::vertex_buffer:
        case resource_usage::index_buffer:
            return kind == resource_kind::buffer && readonly && scope.stages == pipeline_stage::vertex_input;
        case resource_usage::indirect_buffer:
            return kind == resource_kind::buffer && readonly && scope.stages == pipeline_stage::draw_indirect;
        case resource_usage::present:
            return kind == resource_kind::texture && aspect == 1 && readonly && scope.stages == pipeline_stage::none;
        default:
            return false;
        }
    }
    inline bool valid_initial_state(resource_kind kind, resource_shape const& shape, initial_contents contents, resource_state state) noexcept {
        if (bits(state.access) > 3 || (bits(state.stages) & ~1023U))
            return false;
        if (state.layout == resource_layout::undefined)
            return contents == initial_contents::undefined && state.access == memory_access::none && state.stages == pipeline_stage::none;
        if (state.layout == resource_layout::present && state.access == memory_access::none && state.stages == pipeline_stage::none)
            return kind == resource_kind::texture && bits(std::get<texture_shape>(shape).aspects) == 1;
        if (state.access == memory_access::none || state.stages == pipeline_stage::none)
            return false;
        if (kind == resource_kind::buffer)
            return state.layout == resource_layout::general && !(bits(state.stages) & ~(7U | 32U | 64U | 128U | 256U | 512U));
        if (bits(state.stages) & ~(63U | 256U | 512U))
            return false;
        auto aspects = bits(std::get<texture_shape>(shape).aspects);
        switch (state.layout) {
        // Layout is current; masks conservatively include earlier outstanding accesses.
        case resource_layout::general:
        case resource_layout::shader_read_only:
        case resource_layout::transfer_source:
        case resource_layout::transfer_destination:
            return true;
        case resource_layout::color_attachment:
        case resource_layout::present:
            return aspects == 1;
        case resource_layout::depth_stencil_attachment:
        case resource_layout::depth_stencil_read_only:
            return !(aspects & 1);
        default:
            return false;
        }
    }
    inline resource_state state_for(resource_kind kind, resolved_access const& access) noexcept {
        return {layout_for(kind, access.scope.usage), access.scope.stages,
                access.scope.usage == resource_usage::present ? memory_access::none : access_for(access.mode)};
    }
    inline resource_state join_writer(resource_state current, std::optional<resource_state> const& writer) noexcept {
        if (writer) {
            current.stages = current.stages | writer->stages;
            current.access = current.access | writer->access;
        }
        return current;
    }
    inline std::expected<void, graph_error> plan_synchronization(compiled_graph& graph) {
        auto fail = [&](error_code code, uint32_t resource, uint32_t pass = invalid_index, std::optional<resource_range> range = std::nullopt) {
            return std::unexpected(graph_error{code, pass, resource,
                                               pass == invalid_index ? deren::utility::vessel::string{} : graph.passes[pass].name,
                                               graph.resources[resource].name, std::move(range)});
        };
        deren::utility::vessel::vector<unsigned char> needed(graph.resources.size(), 0);
        deren::utility::vessel::vector<uint32_t> extra_image_stages(graph.resources.size(), 0);
        for (auto p : graph.execution_order)
            for (auto const& access : graph.passes[p].accesses) {
                if (access.mode == content_access::invalidate)
                    return fail(error_code::unsupported_access, access.resource, p, access.range);
                if (access.scope.usage == resource_usage::unspecified)
                    return fail(error_code::missing_scope, access.resource, p, access.range);
                needed[access.resource] = 1;
                extra_image_stages[access.resource] |= bits(access.scope.stages) & (256U | 512U);
            }
        for (auto const& output : graph.outputs)
            needed[output.resource] = 1;
        struct state_cell {
            resource_range range;
            resource_state current;
            std::optional<resource_state> writer;
            pipeline_stage visible = pipeline_stage::none;
        };
        deren::utility::vessel::vector<deren::utility::vessel::vector<state_cell>> states(graph.resources.size());
        for (uint32_t r = 0; r < graph.resources.size(); ++r) {
            if (!needed[r])
                continue;
            auto const& resource = graph.resources[r];
            if (!resource.initial_state)
                return fail(error_code::missing_initial_state, r);
            auto state = *resource.initial_state;
            states[r].push_back({full_range(resource.shape), state, writes(state.access) ? std::optional(state) : std::nullopt, pipeline_stage::none});
        }
        for (auto p : graph.execution_order)
            for (auto const& access : graph.passes[p].accesses) {
                auto const kind = graph.resources[access.resource].kind;
                auto target = state_for(kind, access);
                auto& cells = states[access.resource];
                for (auto index : isolate(cells, access.range)) {
                    auto& cell = cells[index];
                    auto transition = kind == resource_kind::texture && cell.current.layout != target.layout;
                    auto write = writes(target.access);
                    auto visibility = cell.writer && (cell.visible & target.stages) != target.stages;
                    auto memory = write ? cell.current.access != memory_access::none || cell.writer.has_value() : visibility || (transition && cell.writer.has_value());
                    auto destination = target;
                    // A semantic transition may become a native transition that writes memory.
                    // Without a logical writer to track, expose it to the baseline image
                    // stages and this graph's optional task/mesh consumers. Cross-graph
                    // task/mesh consumers need explicit boundary coverage.
                    if (transition && !write && !cell.writer && target.layout != resource_layout::present)
                        destination.stages = pipeline_stage::vertex_shader | pipeline_stage::fragment_shader | pipeline_stage::compute_shader | pipeline_stage::color_output | pipeline_stage::depth_stencil | pipeline_stage::transfer | static_cast<pipeline_stage>(extra_image_stages[access.resource]);
                    if (transition || memory)
                        graph.barriers.push_back({access.resource, p, cell.range, join_writer(cell.current, cell.writer), destination, transition, memory || transition});
                    if (write) {
                        cell.current = target;
                        cell.writer = target;
                        cell.visible = pipeline_stage::none;
                    } else {
                        if (transition)
                            cell.current = destination;
                        else {
                            cell.current.layout = target.layout;
                            cell.current.stages = cell.current.stages | target.stages;
                            cell.current.access = target.access;
                        }
                        if (cell.writer)
                            cell.visible = transition ? target.stages : cell.visible | target.stages;
                    }
                }
            }
        // Retain the outstanding writer scope for a subsequent graph reading in a new stage.
        for (uint32_t r = 0; r < states.size(); ++r)
            for (auto const& cell : states[r])
                graph.final_states.push_back({r, cell.range, join_writer(cell.current, cell.writer)});
        graph.synchronization_planned = true;
        return {};
    }
} // namespace deren::engine::render_graph::detail
