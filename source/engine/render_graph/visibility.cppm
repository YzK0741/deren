module;

#include <array>
#include <expected>
#include <optional>
#include <utility>

export module deren.engine.render_graph.visibility;

export import deren.engine.render_graph;

export namespace deren::engine::render_graph::visibility {
    /// the reads the VISIBILITY RASTER makes: the camera the mesh stage projects through, the meshlet records it
    /// culls and emits, and this frame slot's draw arguments - which the dispatches consume at the DRAW-INDIRECT
    /// stage rather than reading as a storage buffer (the mesh draws are `draw_mesh_tasks_indirect` records).
    ///
    /// Subtilis additionally reads the optional compacted table. The raster does NOT read the draw-item table -
    /// the per-item block arrives in its push block, and the item table's consumer is the resolve stage (below).
    struct shared_inputs {
        buffer_ref camera, meshlets, indirect;
        std::optional<buffer_ref> culled_meshlets = {};
    };

    struct shared_parameters {
        buffer_read camera, meshlets, indirect;
        std::optional<buffer_read> culled_meshlets = {};
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(camera, meshlets, indirect, culled_meshlets);
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
            constexpr access_scope mesh{resource_usage::storage, pipeline_stage::mesh_shader};
            constexpr access_scope indirect{resource_usage::indirect_buffer, pipeline_stage::draw_indirect};
            params.shared = shared_parameters{{in.shared->camera, {}, camera}, {in.shared->meshlets, {}, mesh}, {in.shared->indirect, {}, indirect}};
            if (in.shared->culled_meshlets) {
                params.shared->culled_meshlets = buffer_read{*in.shared->culled_meshlets, {}, mesh};
            }
        }
        auto added = graph.add_pass("scene visibility raster", std::move(params));
        if (!added) {
            return std::unexpected(added.error());
        }
        return outputs{in, *added};
    }

    // ---- THE RESOLVE LANE (visibility-buffer v0.4) -------------------------------------------------
    /// the reads the VISIBILITY RESOLVE makes: the two textures it samples (the id and the FRAME'S G-BUFFER
    /// DEPTH, which the raster lane wrote - v0.5 made that lane the opaque producer, so its depth output is the
    /// depth the lighting stage also samples), the five G-buffer targets it writes, and the six scene/visibility
    /// tables a pixel's id is turned back into a surface through.
    struct resolve_shared_inputs {
        buffer_ref camera;              ///< the same per-frame camera block the raster projected with
        buffer_ref materials;           ///< the material table the shared decode reads
        buffer_ref items;               ///< this frame's draw-item table (one row per visible item)
        buffer_ref meshlets;            ///< the import-time meshlet record table
        buffer_ref previous_transforms; ///< last frame's world matrices, for the motion vector
        buffer_ref statistics;          ///< the mesh-statistics buffer: the resolve's failure counters are atomic adds
        std::optional<buffer_ref> culled_meshlets = {};
    };

    struct resolve_shared_parameters {
        buffer_read camera, materials, items, meshlets, previous_transforms;
        buffer_read_write statistics;
        std::optional<buffer_read> culled_meshlets = {};
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(camera, materials, items, meshlets, previous_transforms, statistics, culled_meshlets);
        }
    };

    struct resolve_inputs {
        texture_ref vis_id;
        texture_ref depth;
        std::array<texture_ref, 3> surface;
        texture_ref velocity;
        texture_ref color;
        std::optional<resolve_shared_inputs> shared = {};
    };

    /// THE RESOLVE IS A COMPUTE PASS, and its write scope says so: the five G-buffer targets are written as STORAGE
    /// images (the compute stage's only route), not as colour attachments - the graphics scope belongs to the
    /// geometry lane that owns the same images. The graph's WAW edge is what orders the two writers.
    struct resolve_parameters {
        texture_read vis_id;
        texture_read depth;
        std::array<texture_discard_write, 3> surface;
        texture_discard_write velocity;
        texture_discard_write color;
        std::optional<resolve_shared_parameters> shared = {};

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
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (outputs[i] == outputs[j]) {
                    return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "visibility resolve", .resource_name = "G-buffer alias"});
                }
            }
        }
        // the inputs and the outputs are different images by construction (the id is not a G-buffer family, and the
        // depth is READ but never written by this pass), so a shared handle would be a wiring mistake rather than a
        // legal read-write pair.
        for (texture_ref const input : {in.vis_id, in.depth}) {
            for (texture_ref const output : outputs) {
                if (input == output) {
                    return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "visibility resolve", .resource_name = "visibility input alias"});
                }
            }
        }
        constexpr access_scope read{resource_usage::sampled, pipeline_stage::compute_shader};
        constexpr access_scope write{resource_usage::storage, pipeline_stage::compute_shader};
        resolve_parameters params{{in.vis_id, {}, read}, {in.depth, {}, read}, {}, {in.velocity, {}, write}, {in.color, {}, write}};
        params.surface = {texture_discard_write{in.surface[0], {}, write}, texture_discard_write{in.surface[1], {}, write}, texture_discard_write{in.surface[2], {}, write}};
        if (in.shared) {
            constexpr access_scope buffer{resource_usage::storage, pipeline_stage::compute_shader};
            constexpr access_scope camera{resource_usage::uniform_buffer, pipeline_stage::compute_shader};
            params.shared = resolve_shared_parameters{{in.shared->camera, {}, camera},
                                                      {in.shared->materials, {}, buffer},
                                                      {in.shared->items, {}, buffer},
                                                      {in.shared->meshlets, {}, buffer},
                                                      {in.shared->previous_transforms, {}, buffer},
                                                      {in.shared->statistics, {}, buffer}};
            if (in.shared->culled_meshlets) {
                params.shared->culled_meshlets = buffer_read{*in.shared->culled_meshlets, {}, buffer};
            }
        }
        auto added = graph.add_pass("visibility resolve", std::move(params));
        if (!added) {
            return std::unexpected(added.error());
        }
        return resolve_outputs{in, *added};
    }
} // namespace deren::engine::render_graph::visibility
