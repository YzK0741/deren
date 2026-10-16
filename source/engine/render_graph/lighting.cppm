module;
#include <array>
#include <expected>
#include <optional>
#include <utility>
export module deren.engine.render_graph.lighting;
export import deren.engine.render_graph;
export namespace deren::engine::render_graph::lighting {
    struct shared_inputs {
        buffer_ref camera, light;
    };
    struct shared_parameters {
        buffer_read camera, light;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(camera, light);
        }
    };
    struct inputs {
        texture_ref color;
        std::array<texture_ref, 3> surface;
        texture_ref depth;
        std::optional<shared_inputs> shared = {};
    };
    struct parameters {
        std::array<texture_read, 3> surface;
        texture_read depth;
        texture_preserve_write color;
        std::optional<shared_parameters> shared = {};
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(surface, depth, color, shared);
        }
    };
    struct outputs {
        texture_ref color;
        pass_ref pass;
    };
    // Mesh/material textures, IBL, cluster/shadow inputs retain their external protocols.
    [[nodiscard]] std::expected<outputs, graph_error> append(builder& graph, inputs const& in) noexcept {
        std::array refs{in.color, in.surface[0], in.surface[1], in.surface[2], in.depth};
        for (std::size_t i = 0; i < refs.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (refs[i] == refs[j])
                    return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "deferred lighting", .resource_name = "lighting alias"});
        constexpr access_scope sampled{resource_usage::sampled, pipeline_stage::fragment_shader};
        parameters params{{{{in.surface[0], {}, sampled}, {in.surface[1], {}, sampled}, {in.surface[2], {}, sampled}}},
                          {in.depth, {}, sampled},
                          {in.color, {}, {resource_usage::color_attachment, pipeline_stage::color_output}},
                          {}};
        if (in.shared) {
            constexpr access_scope uniform{resource_usage::uniform_buffer, pipeline_stage::fragment_shader};
            params.shared = shared_parameters{{in.shared->camera, {}, uniform}, {in.shared->light, {}, uniform}};
        }
        auto added = graph.add_pass("deferred lighting", std::move(params));
        if (!added)
            return std::unexpected(added.error());
        return outputs{in.color, *added};
    }
} // namespace deren::engine::render_graph::lighting
