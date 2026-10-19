module;
#include <expected>
#include <utility>
export module deren.engine.render_graph.transparent;
export import deren.engine.render_graph;
export namespace deren::engine::render_graph::transparent {
    /// The alpha-blended geometry, composited over the shaded frame.
    ///
    /// The pass LOADs the scene target (the lit image, or the TAA working image on a TAA frame) and the opaque
    /// depth, and blends its leaves over both - so the target is a PRESERVE write (its contents are read and
    /// kept) and the depth is read through the depth test. The removed legacy path published the two hand-off transitions
    /// itself; declaring them here is what lets the graph order lighting -> transparent -> TAA by resource.
    struct inputs {
        texture_ref target;
        texture_ref depth;
    };
    struct parameters {
        texture_preserve_write target;
        texture_read depth;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(target, depth);
        }
    };
    struct outputs {
        texture_ref target;
        pass_ref pass;
    };
    [[nodiscard]] std::expected<outputs, graph_error> append(builder& graph, inputs const& in) noexcept {
        if (in.target == in.depth) {
            return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "transparent", .resource_name = "transparent alias"});
        }
        parameters params{{in.target, {}, {resource_usage::color_attachment, pipeline_stage::color_output}},
                          {in.depth, {}, {resource_usage::depth_stencil_read, pipeline_stage::depth_stencil}}};
        auto added = graph.add_pass("transparent", std::move(params));
        if (!added) {
            return std::unexpected(added.error());
        }
        return outputs{in.target, *added};
    }
} // namespace deren::engine::render_graph::transparent
