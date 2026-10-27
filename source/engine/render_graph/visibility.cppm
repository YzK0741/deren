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
        std::optional<buffer_ref> hzb_survivors = {}, hzb_results = {};
    };

    struct shared_parameters {
        buffer_read camera, meshlets, indirect;
        std::optional<buffer_read> culled_meshlets = {};
        std::optional<buffer_read> hzb_survivors = {}, hzb_results = {};
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(camera, meshlets, indirect, culled_meshlets, hzb_survivors, hzb_results);
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
        if (in.shared && in.shared->hzb_survivors.has_value() != in.shared->hzb_results.has_value()) {
            return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "visibility HZB", .resource_name = "incomplete HZB read tables"});
        }
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
            if (in.shared->hzb_survivors) {
                params.shared->hzb_survivors = buffer_read{*in.shared->hzb_survivors, {}, mesh};
                params.shared->hzb_results = buffer_read{*in.shared->hzb_results, {}, mesh};
            }
        }
        auto added = graph.add_pass("scene visibility raster", std::move(params));
        if (!added) {
            return std::unexpected(added.error());
        }
        return outputs{in, *added};
    }

    /// The second raster preserves the first draw's IDs/depth; reads are load-bearing dependencies.
    struct raster_load_parameters {
        texture_read_write vis_id, depth;
        std::optional<shared_parameters> shared = {};
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(vis_id, depth, shared);
        }
    };
    [[nodiscard]] inline std::expected<outputs, graph_error> append_recovery(builder& graph, inputs const& in) noexcept {
        if (in.shared && in.shared->hzb_survivors.has_value() != in.shared->hzb_results.has_value()) {
            return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "visibility HZB", .resource_name = "incomplete HZB read tables"});
        }
        if (in.vis_id == in.depth) {
            return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "visibility recovery raster", .resource_name = "visibility attachments"});
        }
        constexpr access_scope color{resource_usage::color_attachment, pipeline_stage::color_output};
        constexpr access_scope depth{resource_usage::depth_stencil_attachment, pipeline_stage::depth_stencil};
        raster_load_parameters params{{in.vis_id, {}, color}, {in.depth, {}, depth}, {}};
        if (in.shared) {
            constexpr access_scope camera{resource_usage::uniform_buffer, pipeline_stage::mesh_shader};
            constexpr access_scope mesh{resource_usage::storage, pipeline_stage::mesh_shader};
            constexpr access_scope indirect{resource_usage::indirect_buffer, pipeline_stage::draw_indirect};
            params.shared = shared_parameters{{in.shared->camera, {}, camera}, {in.shared->meshlets, {}, mesh}, {in.shared->indirect, {}, indirect}};
            if (in.shared->culled_meshlets) {
                params.shared->culled_meshlets = buffer_read{*in.shared->culled_meshlets, {}, mesh};
            }
            if (in.shared->hzb_survivors) {
                params.shared->hzb_survivors = buffer_read{*in.shared->hzb_survivors, {}, mesh};
                params.shared->hzb_results = buffer_read{*in.shared->hzb_results, {}, mesh};
            }
        }
        auto added = graph.add_pass("visibility recovery raster", std::move(params));
        if (!added) {
            return std::unexpected(added.error());
        }
        return outputs{in, *added};
    }

    struct hzb_build_parameters {
        buffer_discard_write levels;
        buffer_discard_write header;
        texture_read depth;
        buffer_read camera;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(levels, header, depth, camera);
        }
    };

    struct feedback_parameters {
        texture_read vis_id, depth;
        buffer_read items;
        buffer_read_write result;
        std::optional<texture_read> software_depth = {}, software_id = {};
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(vis_id, depth, items, result, software_depth, software_id);
        }
    };

    std::expected<pass_ref, graph_error> append_hzb_build(builder& description, buffer_ref levels, buffer_ref header,
                                                          texture_ref depth, buffer_ref camera) noexcept {
        constexpr access_scope storage{resource_usage::storage, pipeline_stage::compute_shader};
        constexpr access_scope sampled{resource_usage::sampled, pipeline_stage::compute_shader};
        constexpr access_scope uniform{resource_usage::uniform_buffer, pipeline_stage::compute_shader};
        hzb_build_parameters params{{levels, {}, storage}, {header, {}, storage}, {depth, {}, sampled}, {camera, {}, uniform}};
        return description.add_pass("hzb build", std::move(params), pass_options{.side_effect = true});
    }

    struct software_routing_inputs {
        buffer_ref camera, metadata, work, arguments;
    };
    struct hzb_cull_parameters {
        buffer_read levels;
        buffer_read header;
        buffer_read items;
        buffer_read candidates;
        buffer_read draw_items;
        buffer_discard_write results;
        buffer_discard_write survivors;
        buffer_read_write commands;
        std::optional<buffer_read> route_camera = {}, route_metadata = {};
        std::optional<buffer_read_write> route_work = {}, route_arguments = {};
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(levels, header, items, candidates, draw_items, results, survivors, commands, route_camera, route_metadata, route_work, route_arguments);
        }
    };

    std::expected<pass_ref, graph_error> append_hzb_cull(builder& description, buffer_ref levels, buffer_ref header,
                                                         buffer_ref items, buffer_ref candidates, buffer_ref draw_items,
                                                         buffer_ref results, buffer_ref survivors, buffer_ref commands,
                                                         std::optional<software_routing_inputs> routing = {}) noexcept {
        constexpr access_scope storage{resource_usage::storage, pipeline_stage::compute_shader};
        hzb_cull_parameters params{{levels, {}, storage}, {header, {}, storage}, {items, {}, storage}, {candidates, {}, storage}, {draw_items, {}, storage}, {results, {}, storage}, {survivors, {}, storage}, {commands, {}, storage}};
        if (routing) {
            params.route_camera = buffer_read{routing->camera, {}, access_scope{resource_usage::uniform_buffer, pipeline_stage::compute_shader}};
            params.route_metadata = buffer_read{routing->metadata, {}, storage};
            params.route_work = buffer_read_write{routing->work, {}, storage};
            params.route_arguments = buffer_read_write{routing->arguments, {}, storage};
        }
        return description.add_pass("hzb cull", std::move(params), pass_options{.side_effect = true});
    }
    struct hzb_recovery_parameters {
        buffer_read levels;
        buffer_read header;
        buffer_read items;
        buffer_read candidates;
        buffer_read draw_items;
        buffer_read_write results;
        buffer_read_write survivors;
        buffer_read_write commands;
        std::optional<buffer_read> route_camera = {}, route_metadata = {};
        std::optional<buffer_read_write> route_work = {}, route_arguments = {};
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(levels, header, items, candidates, draw_items, results, survivors, commands, route_camera, route_metadata, route_work, route_arguments);
        }
    };

    std::expected<pass_ref, graph_error> append_hzb_recovery(builder& description, buffer_ref levels, buffer_ref header,
                                                             buffer_ref items, buffer_ref candidates, buffer_ref draw_items,
                                                             buffer_ref results, buffer_ref survivors, buffer_ref commands,
                                                             std::optional<software_routing_inputs> routing = {}) noexcept {
        constexpr access_scope storage{resource_usage::storage, pipeline_stage::compute_shader};
        hzb_recovery_parameters params{{levels, {}, storage}, {header, {}, storage}, {items, {}, storage}, {candidates, {}, storage}, {draw_items, {}, storage}, {results, {}, storage}, {survivors, {}, storage}, {commands, {}, storage}};
        if (routing) {
            params.route_camera = buffer_read{routing->camera, {}, access_scope{resource_usage::uniform_buffer, pipeline_stage::compute_shader}};
            params.route_metadata = buffer_read{routing->metadata, {}, storage};
            params.route_work = buffer_read_write{routing->work, {}, storage};
            params.route_arguments = buffer_read_write{routing->arguments, {}, storage};
        }
        return description.add_pass("hzb recovery", std::move(params), pass_options{.side_effect = true});
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
        std::optional<buffer_ref> hzb_survivors = {}, hzb_results = {};
    };

    struct resolve_shared_parameters {
        buffer_read camera, materials, items, meshlets, previous_transforms;
        buffer_read_write statistics;
        std::optional<buffer_read> culled_meshlets = {};
        std::optional<buffer_read> hzb_survivors = {}, hzb_results = {};
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(camera, materials, items, meshlets, previous_transforms, statistics, culled_meshlets, hzb_survivors, hzb_results);
        }
    };

    struct resolve_inputs {
        texture_ref vis_id;
        texture_ref depth;
        std::array<texture_ref, 3> surface;
        texture_ref velocity;
        texture_ref color;
        std::optional<resolve_shared_inputs> shared = {};
        /// THE SOFTWARE CANDIDATES (M4 Task 2): when BOTH are present the resolve also reads them and picks between
        /// the hardware id/depth and the software pixel (spec §3.2's winner rule: strictly nearer software wins, a
        /// depth tie keeps hardware, an empty id keeps hardware). One without the other is a wiring mistake the
        /// append refuses, and with NEITHER the declaration is exactly the v0.5 one.
        std::optional<texture_ref> software_depth = {}, software_id = {};
        std::optional<buffer_ref> mip_feedback = {};
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
        std::optional<texture_read> software_depth = {}, software_id = {};
        std::optional<buffer_read_write> mip_feedback = {};

        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(vis_id, depth, surface, velocity, color, shared, software_depth, software_id, mip_feedback);
        }
    };

    struct resolve_outputs {
        resolve_inputs attachments;
        pass_ref pass;
    };

    [[nodiscard]] inline std::expected<resolve_outputs, graph_error> append_resolve(builder& graph, resolve_inputs const& in) noexcept {
        if (in.shared && in.shared->hzb_survivors.has_value() != in.shared->hzb_results.has_value()) {
            return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "visibility HZB", .resource_name = "incomplete HZB read tables"});
        }
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
        if (in.software_depth.has_value() != in.software_id.has_value()) {
            return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "visibility resolve", .resource_name = "incomplete software candidate images"});
        }
        if (in.software_depth) {
            for (texture_ref const software : {*in.software_depth, *in.software_id}) {
                if (software == in.vis_id || software == in.depth || software == in.surface[0] || software == in.surface[1] || software == in.surface[2] ||
                    software == in.velocity || software == in.color) {
                    return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "visibility resolve", .resource_name = "software candidate alias"});
                }
            }
            if (*in.software_depth == *in.software_id) {
                return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "visibility resolve", .resource_name = "software candidate alias"});
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
            if (in.shared->hzb_survivors) {
                params.shared->hzb_survivors = buffer_read{*in.shared->hzb_survivors, {}, buffer};
                params.shared->hzb_results = buffer_read{*in.shared->hzb_results, {}, buffer};
            }
        }
        if (in.software_depth) {
            params.software_depth = texture_read{*in.software_depth, {}, read};
            params.software_id = texture_read{*in.software_id, {}, read};
        }
        if (in.mip_feedback)
            params.mip_feedback = buffer_read_write{*in.mip_feedback, {}, {resource_usage::storage, pipeline_stage::compute_shader}};
        auto added = graph.add_pass("visibility resolve", std::move(params));
        if (!added) {
            return std::unexpected(added.error());
        }
        return resolve_outputs{in, *added};
    }

    // ============================================================================================
    // THE SOFTWARE RASTER'S LANE (M4 Task 2; docs/superpowers/specs/2026-10-22-software-raster-design.md
    // §3.2/§4.2): the tiled compute pass that rasterizes the subpixel records into its own private depth/id
    // images, the resolve's two OPTIONAL reads of them (the candidate selection), and the depth-only merge that
    // writes min(hardware, software) back into the G-buffer depth.
    //
    // THE DECLARED ORDER IS THE CORRECTNESS ARGUMENT, and it is why the appends below exist before Task 4 records
    // anything: hardware raster -> software raster -> RESOLVE -> MERGE. The resolve reads the UNMERGED hardware
    // depth (so it must run BEFORE the merge), and the merge's LESS_EQUAL depth write is only min(hw, sw) when the
    // resolve already picked among the unmerged candidates. `append_software_raster` orders itself after the
    // hardware raster; `append_software_merge` orders the resolve after the software raster AND itself after the
    // resolve - so the one call that knows both ends records both edges.
    // ============================================================================================
    /// the reads the SOFTWARE RASTER makes: the two images it writes through, the camera block, this frame's
    /// draw-item and meshlet tables, the host-written CSR tile list, and the mesh statistics counters whose
    /// software-fallback/merge failure lanes it adds to.
    struct software_raster_inputs {
        texture_ref software_depth;                     ///< R32_UINT: the nearest software candidate's depth bit pattern
        texture_ref software_id;                        ///< R32_UINT: the same packed 32-bit id the hardware raster writes
        buffer_ref camera;                              ///< the per-frame camera block (same one the hardware raster projected with)
        buffer_ref items;                               ///< this frame's draw-item table
        buffer_ref meshlets;                            ///< the import-time meshlet record table
        buffer_ref tiles;                               ///< the CSR tile list (host-written per frame lane, see visibility.software)
        buffer_ref statistics;                          ///< the mesh-statistics buffer; the raster's counters are atomic adds
        std::optional<buffer_ref> culled_meshlets = {}; ///< the compacted run table when the HZB cull produced one
        std::optional<buffer_ref> hzb_results = {};     ///< final HZB/recovery flags, when that lane ran this frame
        std::optional<buffer_ref> indirect_arguments = {};
    };

    struct software_raster_parameters {
        texture_discard_write software_depth, software_id;
        buffer_read camera, items, meshlets, tiles;
        buffer_read_write statistics;
        std::optional<buffer_read> culled_meshlets = {};
        std::optional<buffer_read> hzb_results = {};
        std::optional<buffer_read> indirect_arguments = {};
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(software_depth, software_id, camera, items, meshlets, tiles, statistics, culled_meshlets, hzb_results, indirect_arguments);
        }
    };

    struct software_raster_outputs {
        pass_ref pass;
    };

    /**
     * @brief append the software raster after the hardware and optional visibility-recovery raster
     *
     * The explicit order edge is not redundant with the data edges (the hardware raster and this pass write
     * disjoint images): it is the ordering spec §3.2 requires - candidates are only meaningful once the hardware
     * depth they are compared against exists - and it keeps the two raster lanes next to each other in the
     * execution order even when nothing reads a software image.
     */
    [[nodiscard]] inline std::expected<software_raster_outputs, graph_error> append_software_raster(builder& graph, software_raster_inputs const& in,
                                                                                                    pass_ref hardware_raster,
                                                                                                    std::optional<pass_ref> visibility_recovery = {}) noexcept {
        if (!graph.contains(hardware_raster)) {
            return std::unexpected(graph_error{.code = error_code::invalid_ref, .pass_name = "software visibility", .resource_name = "hardware raster predecessor"});
        }
        if (in.software_depth == in.software_id) {
            return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "software visibility", .resource_name = "software attachments"});
        }
        constexpr access_scope storage{resource_usage::storage, pipeline_stage::compute_shader};
        constexpr access_scope camera{resource_usage::uniform_buffer, pipeline_stage::compute_shader};
        software_raster_parameters params{{in.software_depth, {}, storage},
                                          {in.software_id, {}, storage},
                                          {in.camera, {}, camera},
                                          {in.items, {}, storage},
                                          {in.meshlets, {}, storage},
                                          {in.tiles, {}, storage},
                                          {in.statistics, {}, storage}};
        if (in.culled_meshlets) {
            params.culled_meshlets = buffer_read{*in.culled_meshlets, {}, storage};
        }
        if (in.hzb_results) {
            params.hzb_results = buffer_read{*in.hzb_results, {}, storage};
        }
        if (in.indirect_arguments)
            params.indirect_arguments = buffer_read{*in.indirect_arguments, {}, access_scope{resource_usage::indirect_buffer, pipeline_stage::draw_indirect}};
        auto added = graph.add_pass("software visibility", std::move(params));
        if (!added) {
            return std::unexpected(added.error());
        }
        auto ordered = graph.order_after(*added, hardware_raster);
        if (!ordered) {
            return std::unexpected(ordered.error());
        }
        if (visibility_recovery) {
            if (!graph.contains(*visibility_recovery)) {
                return std::unexpected(graph_error{.code = error_code::invalid_ref, .pass_name = "software visibility", .resource_name = "visibility recovery predecessor"});
            }
            auto recovery_ordered = graph.order_after(*added, *visibility_recovery);
            if (!recovery_ordered) {
                return std::unexpected(recovery_ordered.error());
            }
        }
        return software_raster_outputs{*added};
    }

    /// the reads the SOFTWARE MERGE makes: the two software images it samples, and the G-buffer depth it tests
    /// against and writes (the frame's own depth, i.e. the SAME image the hardware raster produced)
    struct software_merge_inputs {
        texture_ref software_depth, software_id, gbuffer_depth;
    };

    struct software_merge_parameters {
        texture_read software_depth, software_id;
        texture_read_write gbuffer_depth;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(software_depth, software_id, gbuffer_depth);
        }
    };

    struct software_merge_outputs {
        pass_ref pass;
    };

    /**
     * @brief append the depth-only merge, declaring the order the winner rule depends on
     *
     * Two explicit edges, and BOTH are this call's to record because it is the first point that holds the resolve
     * and the software raster refs together: resolve AFTER software raster (it must read the candidates), merge
     * AFTER resolve (it must not overwrite the unmerged depth the resolve compares against). It also reads both
     * software images sampled and the G-buffer depth as a READ-WRITE depth attachment, which is what makes the
     * graph emit the merge's WAR/RAW edges and its depth transition.
     */
    [[nodiscard]] inline std::expected<software_merge_outputs, graph_error> append_software_merge(builder& graph, software_merge_inputs const& in,
                                                                                                  pass_ref resolve, pass_ref software_raster) noexcept {
        if (!graph.contains(resolve) || !graph.contains(software_raster)) {
            return std::unexpected(graph_error{.code = error_code::invalid_ref, .pass_name = "software merge", .resource_name = "resolve or software raster predecessor"});
        }
        if (in.software_depth == in.software_id || in.software_depth == in.gbuffer_depth || in.software_id == in.gbuffer_depth) {
            return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = "software merge", .resource_name = "software merge attachments"});
        }
        constexpr access_scope sampled{resource_usage::sampled, pipeline_stage::fragment_shader};
        constexpr access_scope depth{resource_usage::depth_stencil_attachment, pipeline_stage::depth_stencil};
        software_merge_parameters params{{in.software_depth, {}, sampled}, {in.software_id, {}, sampled}, {in.gbuffer_depth, {}, depth}};
        auto added = graph.add_pass("software merge", std::move(params));
        if (!added) {
            return std::unexpected(added.error());
        }
        auto resolve_order = graph.order_after(resolve, software_raster);
        if (!resolve_order) {
            return std::unexpected(resolve_order.error());
        }
        auto merge_order = graph.order_after(*added, resolve);
        if (!merge_order) {
            return std::unexpected(merge_order.error());
        }
        return software_merge_outputs{*added};
    }
} // namespace deren::engine::render_graph::visibility
