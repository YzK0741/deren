module;

#include <array>
#include <expected>
#include <optional>
#include <utility>

export module deren.engine.render_graph.visibility;

export import deren.engine.render_graph;

export namespace deren::engine::render_graph::visibility {
    /// the reads the resolve pass makes: the two inputs (id + depth) and the per-item/meshlet materials
    struct shared_inputs {
        buffer_ref camera, materials, items, meshlets;
    };

    struct shared_parameters {
        buffer_read camera, materials, items, meshlets;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(camera, materials, items, meshlets);
        }
    };

    struct inputs {
        texture_ref vis_id;
        texture_ref depth;
        std::optional<shared_inputs> shared = {};
    };

    struct parameters {
        texture_discard_write vis_id;
        texture_discard_write depth;
        std::optional<shared_parameters> shared = {};
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(vis_id, depth, shared);
        }
    };

    struct outputs {
        inputs attachments;
        pass_ref pass;
    };

    [[nodiscard]] inline std::expected<outputs, graph_error> append(builder& graph, inputs const& in) noexcept {
        if (in.vis_id == in.depth) {
            return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "scene visibility raster", .resource_name = "visibility attachments"});
        }
        constexpr access_scope color{resource_usage::color_attachment, pipeline_stage::color_output};
        constexpr access_scope depth{resource_usage::depth_stencil_attachment, pipeline_stage::depth_stencil};
        parameters params{{in.vis_id, {}, color}, {in.depth, {}, depth}, {}};
        if (in.shared) {
            constexpr access_scope camera{resource_usage::uniform_buffer, pipeline_stage::mesh_shader};
            constexpr access_scope material{resource_usage::storage, pipeline_stage::fragment_shader};
            constexpr access_scope mesh{resource_usage::storage, pipeline_stage::mesh_shader};
            params.shared = shared_parameters{{in.shared->camera, {}, camera}, {in.shared->materials, {}, material}, {in.shared->items, {}, mesh}, {in.shared->meshlets, {}, mesh}};
        }
        auto added = graph.add_pass("scene visibility raster", std::move(params));
        if (!added)
            return std::unexpected(added.error());
        return outputs{in, *added};
    }

    /// ---- THE RESOLVE LANE'S DECLARATION (visibility-buffer v0.4) --------------------------------
    /// Inputs: the id and the capture depth, both SAMPLED reads (the resolve is a compute pass).
    /// Outputs: the five G-buffer targets the old geometry path wrote - three surfaces, velocity, emissive.
    struct resolve_inputs {
        texture_ref vis_id;
        texture_ref depth;
        std::array<texture_ref, 3> surface;
        texture_ref velocity;
        texture_ref color;
        std::optional<shared_inputs> shared = {};
    };

    struct resolve_parameters {
        texture_read vis_id;
        texture_read depth;
        std::array<texture_discard_write, 3> surface;
        texture_discard_write velocity;
        texture_discard_write color;
        std::optional<shared_parameters> shared = {};

        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(vis_id, depth, surface, velocity, color, shared);
        }
    };

    struct resolve_outputs {
        resolve_inputs attachments;
        pass_ref pass;
    };

    [[nodiscard]] inline std::expected<resolve_outputs, graph_error> append_resolve(builder& graph, resolve_inputs const& in) noexcept {
        std::array<texture_ref, 5> outputs{in.surface[0], in.surface[1], in.surface[2], in.velocity, in.color};
        for (std::size_t i = 0; i < outputs.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (outputs[i] == outputs[j])
                    return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "visibility resolve", .resource_name = "G-buffer alias"});
        if (in.vis_id == in.depth || in.vis_id == in.velocity || in.depth == in.velocity)
            return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "visibility resolve", .resource_name = "visibility input alias"});
        constexpr access_scope read{resource_usage::sampled, pipeline_stage::compute_shader};
        constexpr access_scope write{resource_usage::color_attachment, pipeline_stage::color_output};
        resolve_parameters params{{in.vis_id, {}, read}, {in.depth, {}, read}, {}, {in.velocity, {}, write}, {in.color, {}, write}};
        params.surface = {texture_discard_write{in.surface[0], {}, write}, texture_discard_write{in.surface[1], {}, write}, texture_discard_write{in.surface[2], {}, write}};
        if (in.shared) {
            constexpr access_scope buffer{resource_usage::storage, pipeline_stage::compute_shader};
            params.shared = shared_parameters{{in.shared->camera, {}, buffer}, {in.shared->materials, {}, buffer}, {in.shared->items, {}, buffer}, {in.shared->meshlets, {}, buffer}};
        }
        auto added = graph.add_pass("visibility resolve", std::move(params));
        if (!added)
            return std::unexpected(added.error());
        return resolve_outputs{in, *added};
    }
} // namespace deren::engine::render_graph::visibility
