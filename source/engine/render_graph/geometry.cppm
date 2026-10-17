module;
#include <array>
#include <expected>
#include <optional>
#include <utility>
export module deren.engine.render_graph.geometry;
export import deren.engine.render_graph;
export namespace deren::engine::render_graph::geometry {
    /// The GPU-driven geometry inputs (adoption stage 2C): the mesh stage reads the meshlet records and this
    /// frame slot's host-culled run as storage buffers, and it ATOMICALLY updates the per-slot statistics -
    /// `InterlockedAdd` on the stats stride - which is why the statistics cannot join the read-only bundle.
    struct meshlet_inputs {
        buffer_ref table;
        buffer_ref culled;
        buffer_ref statistics;
        /// This frame slot's draw arguments, consumed at the DRAW-INDIRECT stage rather than read by the mesh
        /// stage - the mesh draws are `draw_mesh_tasks_indirect` records, so the graph declares the indirect
        /// usage instead of a storage read.
        buffer_ref indirect;
    };
    struct shared_inputs {
        buffer_ref camera, materials, instances, previous_transforms, skin, previous_skin, morph;
        std::optional<meshlet_inputs> meshlets = {};
    };
    struct meshlet_parameters {
        buffer_read table, culled;
        buffer_read_write statistics;
        buffer_read indirect;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(table, culled, statistics, indirect);
        }
    };
    struct shared_parameters {
        buffer_read camera, materials, instances, previous_transforms, skin, previous_skin, morph;
        std::optional<meshlet_parameters> meshlets = {};
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(camera, materials, instances, previous_transforms, skin, previous_skin, morph, meshlets);
        }
    };
    struct inputs {
        texture_ref color;
        std::array<texture_ref, 3> surface;
        texture_ref velocity;
        texture_ref depth;
        std::optional<shared_inputs> shared = {};
    };
    struct parameters {
        std::array<texture_discard_write, 3> surface;
        texture_discard_write velocity;
        texture_discard_write color;
        texture_discard_write depth;
        std::optional<shared_parameters> shared = {};
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(surface, velocity, color, depth, shared);
        }
    };
    struct outputs {
        inputs attachments;
        pass_ref pass;
    };
    [[nodiscard]] std::expected<outputs, graph_error> append(builder& graph, inputs const& in) noexcept {
        std::array refs{in.color, in.surface[0], in.surface[1], in.surface[2], in.velocity, in.depth};
        for (std::size_t i = 0; i < refs.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (refs[i] == refs[j])
                    return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "scene geometry", .resource_name = "geometry alias"});
        constexpr access_scope color{resource_usage::color_attachment, pipeline_stage::color_output};
        parameters params{{{{in.surface[0], {}, color}, {in.surface[1], {}, color}, {in.surface[2], {}, color}}},
                          {in.velocity, {}, color},
                          {in.color, {}, color},
                          {in.depth, {}, {resource_usage::depth_stencil_attachment, pipeline_stage::depth_stencil}},
                          {}};
        if (in.shared) {
            constexpr access_scope mesh{resource_usage::storage, pipeline_stage::mesh_shader};
            params.shared = shared_parameters{
                {in.shared->camera, {}, {resource_usage::uniform_buffer, pipeline_stage::mesh_shader | pipeline_stage::fragment_shader}},
                {in.shared->materials, {}, {resource_usage::storage, pipeline_stage::fragment_shader}},
                {in.shared->instances, {}, mesh},
                {in.shared->previous_transforms, {}, mesh},
                {in.shared->skin, {}, mesh},
                {in.shared->previous_skin, {}, mesh},
                {in.shared->morph, {}, mesh}};
            if (in.shared->meshlets) {
                constexpr access_scope indirect{resource_usage::indirect_buffer, pipeline_stage::draw_indirect};
                params.shared->meshlets = meshlet_parameters{
                    {in.shared->meshlets->table, {}, mesh},
                    {in.shared->meshlets->culled, {}, mesh},
                    {in.shared->meshlets->statistics, {}, mesh},
                    {in.shared->meshlets->indirect, {}, indirect}};
            }
        }
        auto added = graph.add_pass("scene geometry", std::move(params));
        if (!added)
            return std::unexpected(added.error());
        return outputs{in, *added};
    }
} // namespace deren::engine::render_graph::geometry
