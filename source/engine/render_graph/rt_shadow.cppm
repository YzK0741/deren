module;
#include <array>
#include <expected>
#include <span>
#include <utility>
export module deren.engine.render_graph.rt_shadow;
export import deren.engine.render_graph;
export namespace deren::engine::render_graph::rt_shadow {
    /// The ray-traced shadow pass: one ray per pixel against the scene's acceleration structures, writing the
    /// visibility image the lighting stage then samples.
    ///
    /// THE GRAPH'S MODEL OF IT: the visibility image is a STORAGE write from the RAY-TRACING stage (a traceRays
    /// launch), and the G-buffer surfaces + depth are declared sampled reads because the tracer reconstructs each
    /// ray's origin from them - they reach the shader through per-image heap slots, so declaring the ACCESS is what
    /// makes the graph order geometry -> trace and transition the images (the read itself is the pass's own).
    /// The lighting stage declares the matching visibility read; that pairing is the write -> hand-off edge.
    ///
    /// THE TLAS ITSELF IS OUT OF MODEL, and stays so until the acceleration-structure resource kind lands: its build
    /// is a pre-graph border step in the same command buffer (submission order), and RT stage 1's
    /// `build_write -> shader_read` barrier publishes it before the graph records.
    struct inputs {
        texture_ref visibility;
        std::array<texture_ref, 3> surface;
        texture_ref depth;
    };
    struct parameters {
        texture_discard_write visibility;
        std::array<texture_read, 3> surface;
        texture_read depth;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(visibility, surface, depth);
        }
    };
    struct outputs {
        texture_ref visibility;
        pass_ref pass;
    };
    [[nodiscard]] std::expected<outputs, graph_error> append(builder& graph, inputs const& in) noexcept {
        std::array refs{in.visibility, in.surface[0], in.surface[1], in.surface[2], in.depth};
        for (std::size_t i = 0; i < refs.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (refs[i] == refs[j]) {
                    return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "ray-traced shadows", .resource_name = "rt shadow alias"});
                }
            }
        }
        constexpr access_scope sampled{resource_usage::sampled, pipeline_stage::ray_tracing};
        parameters params{{in.visibility, {}, {resource_usage::storage, pipeline_stage::ray_tracing}},
                          {{{in.surface[0], {}, sampled}, {in.surface[1], {}, sampled}, {in.surface[2], {}, sampled}}},
                          {in.depth, {}, sampled}};
        auto added = graph.add_pass("ray-traced shadows", std::move(params));
        if (!added) {
            return std::unexpected(added.error());
        }
        return outputs{in.visibility, *added};
    }
} // namespace deren::engine::render_graph::rt_shadow
