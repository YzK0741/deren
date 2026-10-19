module;
#include <array>
#include <cstddef>
#include <expected>
#include <utility>
export module deren.engine.render_graph.gbuffer_debug;
export import deren.engine.render_graph;
export namespace deren::engine::render_graph::gbuffer_debug {
    /// The G-buffer debug view: it samples the three stored targets and the motion-vector target and CLEARS the
    /// HDR target into one visible channel. Its input transitions belonged to the pass in the removed legacy path; declaring
    /// them here is what lets the graph emit them (and the HDR write) instead.
    struct inputs {
        std::array<texture_ref, 3> surface;
        texture_ref velocity;
        texture_ref target;
    };
    struct parameters {
        std::array<texture_read, 3> surface;
        texture_read velocity;
        texture_discard_write target;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(surface, velocity, target);
        }
    };
    struct outputs {
        texture_ref target;
        pass_ref pass;
    };
    [[nodiscard]] std::expected<outputs, graph_error> append(builder& graph, inputs const& in) noexcept {
        std::array refs{in.surface[0], in.surface[1], in.surface[2], in.velocity, in.target};
        for (std::size_t i = 0; i < refs.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (refs[i] == refs[j]) {
                    return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "GBuffer debug view", .resource_name = "debug alias"});
                }
            }
        }
        constexpr access_scope sampled{resource_usage::sampled, pipeline_stage::fragment_shader};
        parameters params{{{{in.surface[0], {}, sampled}, {in.surface[1], {}, sampled}, {in.surface[2], {}, sampled}}},
                          {in.velocity, {}, sampled},
                          {in.target, {}, {resource_usage::color_attachment, pipeline_stage::color_output}}};
        auto added = graph.add_pass("GBuffer debug view", std::move(params));
        if (!added) {
            return std::unexpected(added.error());
        }
        return outputs{in.target, *added};
    }
} // namespace deren::engine::render_graph::gbuffer_debug
