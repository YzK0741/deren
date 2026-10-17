module;

#include <array>
#include <expected>
#include <optional>
#include <utility>

export module deren.engine.render_graph.visibility;

export import deren.engine.render_graph;

export namespace deren::engine::render_graph::visibility {
    /**
     * @brief the GPU-driven geometry the visibility raster reads, in the frame's own tables
     *
     * ALL FOUR ARE READS, and none of them is an attachment: the mesh stage reads the camera to project, the
     * meshlet table to emit the triangles of its run, the per-frame draw-item table to find where that run's
     * geometry lives, and the indirect command table for `draw_mesh_tasks_indirect` - so the last one is
     * declared with the indirect usage rather than a storage read (the same split the geometry lane makes for
     * its own indirect records).
     */
    struct shared_inputs {
        buffer_ref camera;
        buffer_ref meshlets;
        buffer_ref items;
        buffer_ref indirect;
    };

    struct shared_parameters {
        buffer_read camera, meshlets, items, indirect;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(camera, meshlets, items, indirect);
        }
    };

    struct inputs {
        /// the R32_UINT target the packed ids are written into (one per swapchain image)
        texture_ref vis_id;
        /// the shared depth image; the rasterizer's depth test decides which triangle wins each pixel
        texture_ref depth;
        std::optional<shared_inputs> shared = {};
    };

    /**
     * @brief the lane's writes: the id target and the depth image, and nothing else
     *
     * DEPTH IS A WRITE HERE even though this lane is not the depth image's only writer over a frame: it
     * writes the MAIN VIEW's depth, which the G-buffer pass would otherwise have produced - the graph's job
     * is to order that fact, not to hide it. On the capture-only step the lane runs BESIDE the old geometry
     * pass rather than in place of it, so the host does not wire it into the frame until the resolve pass can
     * take the G-buffer's place.
     */
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

    /**
     * @brief append the visibility raster pass: mesh-task dispatches that write depth + one packed id per pixel
     * @param graph the frame being built
     * @param in the two attachments and (optionally) the four table reads
     * @return the declarations the caller binds its record callback to, or the graph error
     *
     * THE ALIAS CHECK IS NOT DECORATION: the id target is a colour attachment and the depth image is a depth
     * attachment, so one handle for both would be an import the graph cannot express - the same
     * `conflicting_import` error the geometry lane raises for its four-way G-buffer alias check.
     */
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
            params.shared = shared_parameters{
                {in.shared->camera, {}, camera},
                {in.shared->meshlets, {}, mesh},
                {in.shared->items, {}, mesh},
                {in.shared->indirect, {}, indirect},
            };
        }
        auto added = graph.add_pass("scene visibility raster", std::move(params));
        if (!added) {
            return std::unexpected(added.error());
        }
        return outputs{in, *added};
    }
} // namespace deren::engine::render_graph::visibility
