module;
#include <expected>
#include <utility>
export module deren.engine.render_graph.gaussian;
export import deren.engine.render_graph;
export namespace deren::engine::render_graph::gaussian {
    /// The Gaussian splat raster: it alpha-blends the cloud over the shaded frame and depth-tests against the
    /// opaque depth. Its GPU compute chain runs inside the same record entry and keeps its own buffer barriers
    /// (the snapshot's private buffers - no other pass touches them), so the pass-level boundary the graph owns is
    /// exactly this: a preserve-write colour attachment and a depth read. In the legacy tail it records BEFORE the
    /// transparent pass, and the graph keeps that order by appending it there too.
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
    [[nodiscard]] inline std::expected<outputs, graph_error> append(builder& graph, inputs const& in) noexcept {
        if (in.target == in.depth)
            return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "gaussian", .resource_name = "gaussian alias"});
        parameters params{{in.target, {}, {resource_usage::color_attachment, pipeline_stage::color_output}},
                          {in.depth, {}, {resource_usage::depth_stencil_read, pipeline_stage::depth_stencil}}};
        auto added = graph.add_pass("gaussian", std::move(params));
        if (!added)
            return std::unexpected(added.error());
        return outputs{in.target, *added};
    }
} // namespace deren::engine::render_graph::gaussian
