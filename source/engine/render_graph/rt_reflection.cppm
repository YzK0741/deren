module;
export module deren.engine.render_graph.rt_reflection;
export import deren.vstd;
export import deren.engine.render_graph;
export namespace deren::engine::render_graph::rt_reflection {
    /// The ray-traced reflection pass: one reflection ray per pixel, writing an HDR radiance image whose alpha
    /// carries the hit distance. The deferred lighting stage samples it as the specular reflection term.
    ///
    /// THE GRAPH'S MODEL OF IT: the reflection image is a STORAGE write from the RAY-TRACING stage (a traceRays
    /// launch), and the G-buffer surfaces + depth are sampled reads because the tracer reconstructs each ray's
    /// origin and direction from them. The lighting stage declares the matching reflection read, which is the
    /// write -> hand-off edge that orders trace -> lighting and holds the image's transition to sampled.
    struct inputs {
        texture_ref reflection;
        std::array<texture_ref, 3> surface;
        texture_ref depth;
    };
    struct parameters {
        texture_discard_write reflection;
        std::array<texture_read, 3> surface;
        texture_read depth;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(reflection, surface, depth);
        }
    };
    struct outputs {
        texture_ref reflection;
        pass_ref pass;
    };
    [[nodiscard]] std::expected<outputs, graph_error> append(builder& graph, inputs const& in) noexcept {
        std::array refs{in.reflection, in.surface[0], in.surface[1], in.surface[2], in.depth};
        for (std::size_t i = 0; i < refs.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (refs[i] == refs[j]) {
                    return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "ray-traced reflections", .resource_name = "rt reflection alias"});
                }
            }
        }
        constexpr access_scope sampled{resource_usage::sampled, pipeline_stage::ray_tracing};
        parameters params{{in.reflection, {}, {resource_usage::storage, pipeline_stage::ray_tracing}},
                          {{{in.surface[0], {}, sampled}, {in.surface[1], {}, sampled}, {in.surface[2], {}, sampled}}},
                          {in.depth, {}, sampled}};
        auto added = graph.add_pass("ray-traced reflections", std::move(params));
        if (!added) {
            return std::unexpected(added.error());
        }
        return outputs{in.reflection, *added};
    }
} // namespace deren::engine::render_graph::rt_reflection
