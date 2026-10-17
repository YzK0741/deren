module;
#include <array>
#include <expected>
#include <optional>
#include <utility>
export module deren.engine.render_graph.lighting;
export import deren.engine.render_graph;
export namespace deren::engine::render_graph::lighting {
    /// The scene-wide image-based lighting the lighting stage samples: the prefiltered environment cube, the
    /// irradiance cube and the BRDF LUT (heap bindings 2-4). All three are uploaded once and read-only
    /// afterwards, so declaring them adds the graph's knowledge, not frame-level ordering - which is the
    /// point: the pass's sampled inputs stop being invisible to the graph.
    struct ibl_inputs {
        texture_ref environment, irradiance, lut;
    };
    struct ibl_parameters {
        texture_read environment, irradiance, lut;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(environment, irradiance, lut);
        }
    };
    /// The clustered-light sort's per-slot output, which the lighting stage reads as storage buffers (the
    /// heap bindings the sort's removed legacy path published with its own two barriers). Declared here, the graph
    /// owns the producer -> consumer dependency instead of the pass.
    struct cluster_inputs {
        buffer_ref counts, indices;
    };
    struct cluster_parameters {
        buffer_read counts, indices;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(counts, indices);
        }
    };
    struct shared_inputs {
        buffer_ref camera, light;
        std::optional<ibl_inputs> ibl = {};
        std::optional<cluster_inputs> cluster = {};
        /// The cascaded shadow map (a layered depth array, one image per frame slot), sampled by the shading
        /// path. The shadow pass renders it OUTSIDE the graph today and the renderer's hand-back barrier makes
        /// every allocated layer sampled before the graph records - so declaring it here is the consumer half:
        /// the graph knows the reads and emits no barrier, and migrating the producer is a separate step.
        std::optional<texture_ref> shadow_cascades = {};
        /// THE RAY-TRACED VISIBILITY IMAGE (RT stage 2b): the lighting stage's descriptor statically binds it too,
        /// and on a graph-recorded RT frame the graph records its PRODUCER (the ray-traced shadow lane) - so the
        /// read has to be declared here or the two passes would be independent in the plan and could be scheduled
        /// in either order. Absent on a frame whose ray-traced shadow pass did not run.
        std::optional<texture_ref> rt_visibility = {};
    };
    struct shared_parameters {
        buffer_read camera, light;
        std::optional<ibl_parameters> ibl = {};
        std::optional<cluster_parameters> cluster = {};
        std::optional<texture_read> shadow_cascades = {};
        std::optional<texture_read> rt_visibility = {};
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(camera, light, ibl, cluster, shadow_cascades, rt_visibility);
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
    // Mesh/material textures, the cluster/shadow producers and the transient feature images retain their
    // external protocols; the scene-wide IBL images are declared through `shared_inputs` because they are
    // real sampled resources the lighting stage reads every frame.
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
            params.shared = shared_parameters{{in.shared->camera, {}, uniform}, {in.shared->light, {}, uniform}, {}};
            if (in.shared->ibl)
                params.shared->ibl = ibl_parameters{{in.shared->ibl->environment, {}, sampled},
                                                    {in.shared->ibl->irradiance, {}, sampled},
                                                    {in.shared->ibl->lut, {}, sampled}};
            if (in.shared->cluster) {
                constexpr access_scope storage{resource_usage::storage, pipeline_stage::fragment_shader};
                params.shared->cluster = cluster_parameters{{in.shared->cluster->counts, {}, storage},
                                                            {in.shared->cluster->indices, {}, storage}};
            }
            if (in.shared->shadow_cascades)
                params.shared->shadow_cascades = texture_read{*in.shared->shadow_cascades, {}, sampled};
            if (in.shared->rt_visibility)
                params.shared->rt_visibility = texture_read{*in.shared->rt_visibility, {}, sampled};
        }
        auto added = graph.add_pass("deferred lighting", std::move(params));
        if (!added)
            return std::unexpected(added.error());
        return outputs{in.color, *added};
    }
} // namespace deren::engine::render_graph::lighting
