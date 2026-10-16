module;
#include <cstdint>
#include <expected>
#include <variant>
export module deren.engine.render_graph.rhi;
export import deren.engine.render_graph;
export import deren.promise.rhi.synchronization;
namespace deren::engine::render_graph::rhi_bridge::detail {
    namespace sync = deren::promise::rhi::synchronization;
    std::expected<sync::scope, sync::error> scope(resource_state value) noexcept {
        auto stages = static_cast<uint32_t>(value.stages), access = static_cast<uint32_t>(value.access);
        if ((stages & ~1023U) || (access & ~3U))
            return std::unexpected(sync::error::unsupported);
        if (access && !stages)
            return std::unexpected(sync::error::invalid_argument);
        sync::scope result{};
        auto include = [&](pipeline_stage from, sync::stage to) { if ((value.stages & from) == from) result.stages = result.stages | to; };
        include(pipeline_stage::vertex_shader, sync::stage::vertex_shader);
        include(pipeline_stage::fragment_shader, sync::stage::fragment_shader);
        include(pipeline_stage::compute_shader, sync::stage::compute_shader);
        include(pipeline_stage::color_output, sync::stage::color_output);
        include(pipeline_stage::depth_stencil, sync::stage::depth_stencil);
        include(pipeline_stage::transfer, sync::stage::transfer);
        include(pipeline_stage::vertex_input, sync::stage::vertex_input);
        include(pipeline_stage::draw_indirect, sync::stage::draw_indirect);
        include(pipeline_stage::task_shader, sync::stage::task_shader);
        include(pipeline_stage::mesh_shader, sync::stage::mesh_shader);
        if (access & 1)
            result.accesses = result.accesses | sync::access::read;
        if (access & 2)
            result.accesses = result.accesses | sync::access::write;
        return result;
    }
    std::expected<sync::image_state, sync::error> image_state(resource_state value) noexcept {
        auto masks = scope(value);
        if (!masks)
            return std::unexpected(masks.error());
        sync::image_layout layout;
        switch (value.layout) {
        case resource_layout::undefined:
            layout = sync::image_layout::undefined;
            break;
        case resource_layout::general:
            layout = sync::image_layout::general;
            break;
        case resource_layout::shader_read_only:
            layout = sync::image_layout::shader_read_only;
            break;
        case resource_layout::color_attachment:
            layout = sync::image_layout::color_attachment;
            break;
        case resource_layout::depth_stencil_attachment:
            layout = sync::image_layout::depth_stencil_attachment;
            break;
        case resource_layout::depth_stencil_read_only:
            layout = sync::image_layout::depth_stencil_read_only;
            break;
        case resource_layout::transfer_source:
            layout = sync::image_layout::transfer_source;
            break;
        case resource_layout::transfer_destination:
            layout = sync::image_layout::transfer_destination;
            break;
        case resource_layout::present:
            layout = sync::image_layout::present;
            break;
        default:
            return std::unexpected(sync::error::unsupported);
        }
        return sync::image_state{layout, *masks};
    }
} // namespace deren::engine::render_graph::rhi_bridge::detail
export namespace deren::engine::render_graph::rhi_bridge {
    namespace sync = deren::promise::rhi::synchronization;
    struct scope_packet {
        uint32_t resource, before_pass;
        std::variant<sync::image_transition, sync::buffer_transition> transition;
    };
    struct scope_plan {
        deren::utility::vessel::vector<scope_packet> barriers;
    };
    struct lowering_error {
        sync::error code;
        uint32_t barrier = invalid_index, resource = invalid_index;
    };
    /// Converts scopes, not bindings. Frontend states remain abstract and unchanged.
    [[nodiscard]] std::expected<scope_plan, lowering_error> lower_scopes(compiled_graph const& graph) {
        if (!graph.synchronization_planned)
            return std::unexpected(lowering_error{sync::error::invalid_argument});
        if (graph.execution_order.size() >= invalid_index || graph.barriers.size() >= invalid_index)
            return std::unexpected(lowering_error{sync::error::unsupported});
        deren::utility::vessel::vector<uint32_t> positions(graph.passes.size(), invalid_index);
        for (uint32_t i = 0; i < graph.execution_order.size(); ++i) {
            auto p = graph.execution_order[i];
            if (p >= positions.size() || !graph.passes[p].live || positions[p] != invalid_index)
                return std::unexpected(lowering_error{sync::error::invalid_argument});
            positions[p] = i;
        }
        scope_plan result;
        result.barriers.reserve(graph.barriers.size());
        uint32_t previous = 0;
        for (std::size_t i = 0; i < graph.barriers.size(); ++i) {
            auto const& barrier = graph.barriers[i];
            auto fail = [&](sync::error code) { return std::unexpected(lowering_error{code, static_cast<uint32_t>(i), barrier.resource}); };
            if (barrier.resource >= graph.resources.size() || barrier.before_pass >= positions.size() || positions[barrier.before_pass] == invalid_index || positions[barrier.before_pass] < previous)
                return fail(sync::error::invalid_argument);
            previous = positions[barrier.before_pass];
            auto const& resource = graph.resources[barrier.resource];
            if (resource.kind == resource_kind::texture) {
                auto range = std::get_if<texture_range>(&barrier.range);
                auto shape = std::get_if<texture_shape>(&resource.shape);
                if (range && (static_cast<uint32_t>(range->aspects) & ~7U))
                    return fail(sync::error::unsupported);
                if (!range || !shape || range->base_mip >= shape->mip_levels || !range->mip_count || range->mip_count > shape->mip_levels - range->base_mip || range->base_layer >= shape->array_layers || !range->layer_count || range->layer_count > shape->array_layers - range->base_layer || !static_cast<uint32_t>(range->aspects) || (static_cast<uint32_t>(range->aspects) & ~static_cast<uint32_t>(shape->aspects)))
                    return fail(sync::error::invalid_argument);
                auto before = detail::image_state(barrier.before), after = detail::image_state(barrier.after);
                if (!before || !after)
                    return fail(!before ? before.error() : after.error());
                sync::aspect aspects = static_cast<sync::aspect>(0);
                auto include = [&](texture_aspect from, sync::aspect to) { if (static_cast<uint32_t>(range->aspects) & static_cast<uint32_t>(from)) aspects = aspects | to; };
                include(texture_aspect::color, sync::aspect::color);
                include(texture_aspect::depth, sync::aspect::depth);
                include(texture_aspect::stencil, sync::aspect::stencil);
                result.barriers.push_back({barrier.resource, barrier.before_pass, sync::image_transition{*before, *after, {range->base_mip, range->mip_count, range->base_layer, range->layer_count, aspects}}});
            } else if (resource.kind == resource_kind::buffer) {
                auto range = std::get_if<buffer_range>(&barrier.range);
                auto shape = std::get_if<buffer_shape>(&resource.shape);
                if (!range || !shape || range->offset >= shape->byte_size || !range->size || range->size > shape->byte_size - range->offset || (barrier.before.layout != resource_layout::undefined && barrier.before.layout != resource_layout::general) || barrier.after.layout != resource_layout::general)
                    return fail(sync::error::invalid_argument);
                if (barrier.before.layout == resource_layout::undefined && (barrier.before.access != memory_access::none || barrier.before.stages != pipeline_stage::none))
                    return fail(sync::error::invalid_argument);
                auto before = detail::scope(barrier.before), after = detail::scope(barrier.after);
                if (!before || !after)
                    return fail(!before ? before.error() : after.error());
                result.barriers.push_back({barrier.resource, barrier.before_pass, sync::buffer_transition{*before, *after, range->offset, range->size}});
            } else
                return fail(sync::error::unsupported);
        }
        return result;
    }
} // namespace deren::engine::render_graph::rhi_bridge
