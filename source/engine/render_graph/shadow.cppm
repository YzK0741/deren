module;
#include <cstdint>
#include <expected>
#include <utility>
export module deren.engine.render_graph.shadow;
export import deren.engine.render_graph;
export namespace deren::engine::render_graph::shadow {
    /// The cascaded-shadow producer's declaration.
    ///
    /// The map is a layered depth array owned outside the graph (one image per frame slot, one layer per ACTIVE
    /// cascade). Only the active layers are written - the pass records `min(cascades, targets)` of them - so the
    /// write range is explicit: a wider declaration would promise layers the pass never touches, and a narrower
    /// one would leave the lighting read of the remaining layers unsynchronized. `layers` is the frame's active
    /// cascade count.
    struct inputs {
        texture_ref cascades;
        std::uint32_t layers = 0;
    };
    struct parameters {
        texture_discard_write cascades;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(cascades);
        }
    };
    struct outputs {
        pass_ref pass;
    };
    [[nodiscard]] std::expected<outputs, graph_error> append(builder& graph, inputs const& in) noexcept {
        if (in.layers == 0)
            return std::unexpected(graph_error{.code = error_code::invalid_range, .pass_name = "shadow cascades", .resource_name = "empty cascade run"});
        // THE WHOLE IMAGE, NOT THE ACTIVE RUN, and that is measured rather than preferred: a layer-ranged write
        // declaration is refused as `unsupported` by this tree's execution path (the transition never reaches the
        // backend), while the whole-image declaration is accepted. It is SAFE for the spare layers because the
        // declaration is a promise about ordering, not an operation - no discard is recorded, the spare layers keep
        // their previous contents, and nothing samples them (the lighting read uses the same cascade count the pass
        // renders). Making the range precise is a follow-up for the execution/backends path, not for this adapter.
        texture_range const written{};
        parameters params{{in.cascades, written, {resource_usage::depth_stencil_attachment, pipeline_stage::depth_stencil}}};
        auto added = graph.add_pass("shadow cascades", std::move(params));
        if (!added)
            return std::unexpected(added.error());
        return outputs{*added};
    }
} // namespace deren::engine::render_graph::shadow
