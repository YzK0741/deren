module;
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <utility>
module deren.engine.runtime;
import deren.engine.render_graph.post;
import deren.engine.render_graph.compose;
import deren.engine.render_graph.lighting;
import deren.engine.render_graph.geometry;
import deren.engine.render_graph.cluster;
import deren.engine.render_graph.shadow;
import deren.engine.render_graph.taa;
import deren.engine.render_graph.gbuffer_debug;
import deren.engine.render_graph.transparent;
import deren.engine.render_graph.character;
import deren.engine.render_graph.gaussian;
import deren.engine.pass.transparent;
import deren.engine.pass.taa;
import deren.engine.pass;
import deren.engine.pass.output_targets;
import deren.promise.rhi;
namespace deren::engine {
    namespace graph = deren::engine::render_graph;
    namespace rhi = deren::promise::rhi;
    namespace {
        struct post_recording_payload {
            pass::pass_host host{};
            std::array<pass::resolved_io, 18> ios;
            std::array<pass::frame_pass*, 18> passes{};
            rhi::error record(unsigned lane) const noexcept {
                if (host.apply_behaviour)
                    host.apply_behaviour(host.context, *passes[lane], ios[lane]);
                auto error = passes[lane]->record_content(ios[lane]);
                if (error != rhi::error::ok) // which lane refused, and with what - one line, once per failure
                    deren::utility::log("post graph: lane {} refused recording ({})", lane, static_cast<uint32_t>(error));
                return error;
            }
        };
        rhi::error post_build_error(graph::execution::build_error const& error) noexcept {
            if (!error.execution)
                return rhi::error::invalid_argument;
            // WHICH PASS AND RESOURCE, when the execution layer names them: an unusable usage or stage is the one
            // failure whose code alone cannot be acted on.
            deren::utility::log("post graph: execution refused ({}) at pass {} resource {}", static_cast<uint32_t>(error.execution->code),
                                error.execution->pass, error.execution->resource);
            return error.execution->code;
        }
        rhi::error clear_post_target(pass::resolved_io const& io) noexcept {
            if (!io.list || io.targets.empty() || !io.targets[0].view_handle || !io.extent.width || !io.extent.height)
                return rhi::error::invalid_argument;
            std::array<rhi::color_attachment, 1> colors{{{.view = io.targets[0].view_handle, .load = rhi::load_op::clear, .store = rhi::store_op::store, .clear = {}}}};
            rhi::rendering_info info{.struct_size = sizeof(rhi::rendering_info),
                                     .area = {.offset_x = 0, .offset_y = 0, .width = io.extent.width, .height = io.extent.height},
                                     .layer_count = 1,
                                     .colors = colors,
                                     .depth = {},
                                     .has_depth = false,
                                     .secondary_contents = false};
            auto error = io.list->begin_rendering(info);
            if (error == rhi::error::ok)
                io.list->end_rendering();
            return error;
        }
    } // namespace
    rhi::error runtime::record_post_graph() {
        using lease = resource_lease<rhi::image>;
        auto commands = this->frame_command_buffer();
        if (!commands || !this->cluster_retention_open)
            return rhi::error::not_ready;
        auto index = this->current_image_index;
        if (index >= rhi::max_swapchain_images)
            return rhi::error::invalid_argument;
        auto frames = rhi::query_extension<rhi::frame_resources>(this->rhi_face());
        auto frame = frames ? frames->retain_image() : nullptr;
        if (!frame)
            return rhi::error::not_ready;
        this->post_graph_output = lease::from_shared(this->core_owner, frame);
        auto frame_cluster = this->create_resource_cluster("post frame");
        auto view = frame_cluster->create_image_view(this->post_graph_output, {});
        if (!view)
            return view.error();
        auto& retained = this->frame_graph_retention[this->cluster_retention_slot];
        retained.push_back(frame_cluster);
        graph::post::options settings{};
        graph::resource_state input_state{graph::resource_layout::shader_read_only, graph::pipeline_stage::fragment_shader, graph::memory_access::read};
        if (this->main_graph_frame)
            input_state = {};
        graph::resource_state ldr_state{};
        std::array<graph::resource_state, 4> level_states{};
        graph::execution::builder build(this->core_owner);
        graph::post::inputs inputs{};
        auto output_ref = build.import_texture(this->post_graph_output, {}, "frame output");
        if (!output_ref)
            return post_build_error(output_ref.error());
        inputs.frame = *output_ref;
        settings.bloom_enabled = this->feature_active("bloom");
        settings.resolution = this->post_upscale_active() ? graph::post::resolve::upscale : this->post_fxaa_active() ? graph::post::resolve::fxaa
                                                                                                                     : graph::post::resolve::none;
        for (auto it = this->bloom_graph_states.begin(); it != this->bloom_graph_states.end();) {
            if (it->first.expired())
                it = this->bloom_graph_states.erase(it);
            else
                ++it;
        }
        auto retain = [&](unsigned family, unsigned pending, graph::resource_state& state) -> lease {
            auto generation = this->output_resource_views[family].find(pass::output_targets_pass::ids[family]);
            if (!generation || generation->device.get() != this->core_owner.get() || index >= generation->objects.size())
                return {};
            auto image = generation->objects[index].image.get();
            if (!image)
                return {};
            if (pending < this->bloom_graph_pending_generations.size()) {
                auto known = this->bloom_graph_states.find(weak_bloom_generation{generation});
                if (known != this->bloom_graph_states.end() && known->second[index])
                    state = {graph::resource_layout::shader_read_only, graph::pipeline_stage::fragment_shader | graph::pipeline_stage::color_output,
                             graph::memory_access::read | graph::memory_access::write};
                this->bloom_graph_pending_generations[pending] = generation;
            }
            return lease::from_shared(this->core_owner, std::shared_ptr<rhi::image>{std::move(generation), image});
        };
        auto input = retain(0, graph::invalid_index, input_state);
        if (!input || input.get() != this->hdr_images[index].get())
            return rhi::error::not_ready;
        auto input_ref = build.import_texture(input, {this->main_graph_frame ? graph::initial_contents::undefined : graph::initial_contents::defined, input_state}, "post HDR");
        if (!input_ref)
            return post_build_error(input_ref.error());
        inputs.hdr = *input_ref;
        // THE TAA WORKING IMAGE AND HISTORY (adoption stage 2I): on a TAA frame the geometry and lighting passes
        // write the TAA working image (the "scene colour", family 5) and the resolve turns it into this HDR image.
        // The history (family 7) is the PREVIOUS frame's resolve: it is only ever transitioned, never discarded, and
        // it is always left SAMPLED outside the graph - this frame's copy's transfer-destination state is handed
        // back right after the graph records (see below), the same rule the legacy path's hand-back keeps.
        graph::texture_ref scene_target = inputs.hdr;
        rhi::image* scene_target_image = input.get();
        std::optional<graph::texture_ref> taa_history{};
        rhi::image* taa_history_image = nullptr;
        if (this->graph_taa_frame) {
            graph::resource_state taa_scene_state{};
            auto taa_scene = retain(5, graph::invalid_index, taa_scene_state);
            auto const& ids = pass::output_targets_pass::ids;
            if (!taa_scene || !this->output_resource_views[5].find(ids[5]))
                return rhi::error::not_ready;
            auto taa_scene_ref = build.import_texture(taa_scene, {}, "TAA scene colour");
            if (!taa_scene_ref)
                return post_build_error(taa_scene_ref.error());
            scene_target = *taa_scene_ref;
            scene_target_image = taa_scene.get();
            graph::resource_state history_state{};
            auto taa_history_lease = retain(7, graph::invalid_index, history_state);
            if (!taa_history_lease || !this->output_resource_views[7].find(ids[7]))
                return rhi::error::not_ready;
            // THE HISTORY'S FIRST USE: the graph can only import "defined contents in a KNOWN layout", and a history
            // image that no path has transitioned yet is still in UNDEFINED - so the runtime performs the one
            // transition the legacy pass performs for itself, keeping the invariant every frame relies on: the
            // history is SAMPLED whenever the graph starts. (After this, the post-record hand-back keeps it so.)
            if (index >= this->taa_history_initialized.size() || !this->taa_history_initialized[index]) {
                auto const first_use = commands->barrier(rhi::image_barrier{.resource = taa_history_lease.get(),
                                                                            .from = rhi::image_use::undefined,
                                                                            .to = rhi::image_use::shader_read,
                                                                            .range = {}});
                if (first_use != rhi::error::ok)
                    return first_use;
                this->taa_history_initialized[index] = true;
            }
            graph::resource_state const history_prior{graph::resource_layout::shader_read_only, graph::pipeline_stage::fragment_shader, graph::memory_access::read};
            auto taa_history_ref = build.import_texture(taa_history_lease, {graph::initial_contents::defined, history_prior}, "TAA history");
            if (!taa_history_ref)
                return post_build_error(taa_history_ref.error());
            taa_history = *taa_history_ref;
            taa_history_image = taa_history_lease.get();
        }
        // A LEGACY TAA frame transitions the history for itself (its first-use barrier) and leaves it sampled by its
        // own hand-back: record that too, so a later graph frame imports a KNOWN layout instead of re-issuing the
        // first-use transition on an image that is no longer UNDEFINED.
        if (this->taa_active() && index < this->taa_history_initialized.size())
            this->taa_history_initialized[index] = true;
        for (unsigned i = 0; i < 4; ++i) {
            auto level = retain(9 + i, i, level_states[i]);
            if (!level || level.get() != this->bloom_images[i][index].get())
                return rhi::error::not_ready;
            auto level_ref = build.import_texture(level, {graph::initial_contents::undefined, level_states[i]}, "bloom level");
            if (!level_ref)
                return post_build_error(level_ref.error());
            inputs.bloom[i] = *level_ref;
        }
        lease ldr;
        if (settings.resolution != graph::post::resolve::none) {
            ldr = retain(8, 4, ldr_state);
            if (!ldr || ldr.get() != this->ldr_images[index].get())
                return rhi::error::not_ready;
            auto ldr_ref = build.import_texture(ldr, {graph::initial_contents::undefined, ldr_state}, "post LDR");
            if (!ldr_ref)
                return post_build_error(ldr_ref.error());
            inputs.ldr = *ldr_ref;
        }
        if (this->post_lut_image) {
            auto lut = lease::from_shared(this->core_owner, this->post_lut_image);
            auto lut_ref = build.import_texture(lut, {graph::initial_contents::defined, {graph::resource_layout::shader_read_only, graph::pipeline_stage::fragment_shader, graph::memory_access::read}}, "post LUT");
            if (!lut_ref)
                return post_build_error(lut_ref.error());
            inputs.lut = *lut_ref;
            auto kept = build.retain(this->post_lut_view);
            if (!kept)
                return post_build_error(kept.error());
        }
        pass::stage composite{.name = "post_composite", .passes = this->post_composite_stage, .marks = false};
        this->prepare_stage(composite);
        if (settings.resolution != graph::post::resolve::none) {
            pass::stage resolve{.name = settings.resolution == graph::post::resolve::fxaa ? "fxaa" : "upscale",
                                .passes = settings.resolution == graph::post::resolve::fxaa ? std::span<pass::frame_pass*>{this->fxaa_pass} : std::span<pass::frame_pass*>{this->upscale_pass},
                                .marks = false};
            this->prepare_stage(resolve);
        }
        // Resolve in its final allocation: resolved_io contains spans into its own storage.
        auto payload = std::allocate_shared<post_recording_payload>(deren::utility::memory::allocator<post_recording_payload>{});
        payload->host = this->make_pass_host();
        auto& host = payload->host;
        auto& ios = payload->ios;
        auto& passes = payload->passes;
        std::optional<graph::lighting::outputs> lit;
        if (this->main_graph_frame) {
            // Inactive statically bound descriptors retain their existing external layout protocol.
            std::array<rhi::image*, 2> inactive{this->rt_shadow_images[this->frame_ring().position()].get(), this->ml_resolve_images[index].get()};
            for (auto* image : inactive)
                if (image) {
                    auto error = commands->barrier(rhi::image_barrier{.resource = image, .from = rhi::image_use::undefined, .to = rhi::image_use::shader_read, .range = {}});
                    if (error != rhi::error::ok)
                        return error;
                }
            graph::lighting::inputs lighting_inputs{};
            lighting_inputs.color = scene_target;
            for (unsigned i = 0; i < 3; ++i) {
                graph::resource_state state{};
                auto surface = retain(1 + i, graph::invalid_index, state);
                if (!surface || surface.get() != this->gbuffer_images[i][index].get())
                    return rhi::error::not_ready;
                auto ref = build.import_texture(surface, {graph::initial_contents::undefined, state}, "GBuffer surface");
                if (!ref)
                    return post_build_error(ref.error());
                lighting_inputs.surface[i] = *ref;
            }
            graph::resource_state depth_state{};
            auto depth = retain(6, graph::invalid_index, depth_state);
            if (!depth || depth.get() != this->gbuffer_depth_images[index].get())
                return rhi::error::not_ready;
            auto depth_ref = build.import_texture(depth, {graph::initial_contents::undefined, depth_state}, "GBuffer depth");
            if (!depth_ref)
                return post_build_error(depth_ref.error());
            lighting_inputs.depth = *depth_ref;
            graph::resource_state velocity_state{};
            auto velocity = retain(4, graph::invalid_index, velocity_state);
            if (!velocity || velocity.get() != this->velocity_images[index].get())
                return rhi::error::not_ready;
            auto velocity_ref = build.import_texture(velocity, {}, "GBuffer velocity");
            if (!velocity_ref)
                return post_build_error(velocity_ref.error());
            // THE SCENE-WIDE IBL (adoption stage 2E): the prefiltered environment cube, the irradiance cube and the
            // BRDF LUT are real sampled images the deferred lighting stage reads every frame - uploaded once at
            // startup, read-only afterwards. They were invisible to the graph; declaring them as sampled reads
            // makes the pass's image set complete. Their prior state is the sampled one (the upload transitioned
            // them), so this adds knowledge, not a barrier.
            std::optional<graph::texture_ref> ibl_environment{}, ibl_irradiance{}, ibl_lut{};
            {
                constexpr std::array<std::string_view, 3> names{"IBL environment", "IBL irradiance", "IBL LUT"};
                std::array<std::optional<graph::texture_ref>*, 3> refs{&ibl_environment, &ibl_irradiance, &ibl_lut};
                for (std::size_t i = 0; i < names.size() && i < this->ibl_images.size(); ++i) {
                    if (!this->ibl_images[i])
                        continue;
                    auto ibl_lease = lease::from_shared(this->core_owner, this->ibl_images[i]);
                    auto ref = build.import_texture(ibl_lease,
                                                    {graph::initial_contents::defined,
                                                     {graph::resource_layout::shader_read_only, graph::pipeline_stage::fragment_shader, graph::memory_access::read}},
                                                    names[i]);
                    if (!ref)
                        return post_build_error(ref.error());
                    *refs[i] = *ref;
                }
            }
            // CPU writes were published for the completed slot before submission. These
            // imports describe GPU reads, not fictitious GPU upload writes.
            auto const slot = this->frame_ring().position();
            if (slot >= this->camera_buffers.size() || slot >= this->light_buffers.size() ||
                slot >= this->motion_buffers.size() || slot >= this->skin_buffers.size() ||
                slot >= this->skin_buffers_previous.size() || slot >= this->morph_buffers.size())
                return rhi::error::not_ready;
            // THE CLUSTERED-LIGHT SORT'S OWN OUTPUT (adoption stage 2G): the graph records the sort as a pass on
            // an eligible frame, so its two per-slot buffers are imported and declared here. `counts` keeps the
            // contents: the frame-slot reset host-zeroes the table and the dispatch ATOMICALLY increments it, so
            // its prior state is the compute read-write the last frame left. `indices` starts undefined because
            // the sort writes the light lists wholesale.
            std::optional<graph::buffer_ref> cluster_counts_ref{}, cluster_indices_ref{};
            if (this->graph_cluster_frame) {
                auto counts_generation = this->cluster_resource_view.find(render_resource::resource_id::cluster_counts);
                auto indices_generation = this->cluster_resource_view.find(render_resource::resource_id::cluster_indices);
                if (!counts_generation || !indices_generation || counts_generation->device.get() != this->core_owner.get() ||
                    indices_generation->device.get() != this->core_owner.get() || slot >= counts_generation->objects.size() ||
                    slot >= indices_generation->objects.size())
                    return rhi::error::not_ready;
                auto* counts = counts_generation->objects[slot].buffer.get();
                auto* indices = indices_generation->objects[slot].buffer.get();
                if (!counts || !indices)
                    return rhi::error::not_ready;
                auto counts_lease = resource_lease<rhi::buffer>::from_shared(this->core_owner, std::shared_ptr<rhi::buffer>{std::move(counts_generation), counts});
                auto counts_import =
                    build.import_buffer(counts_lease,
                                        {graph::initial_contents::defined,
                                         {graph::resource_layout::general, graph::pipeline_stage::compute_shader, graph::memory_access::read | graph::memory_access::write}},
                                        "cluster counts");
                if (!counts_import)
                    return post_build_error(counts_import.error());
                auto indices_lease = resource_lease<rhi::buffer>::from_shared(this->core_owner, std::shared_ptr<rhi::buffer>{std::move(indices_generation), indices});
                auto indices_import = build.import_buffer(indices_lease, {}, "cluster indices");
                if (!indices_import)
                    return post_build_error(indices_import.error());
                cluster_counts_ref = *counts_import;
                cluster_indices_ref = *indices_import;
            }
            // THE CASCADED SHADOW MAP (adoption stage 2H). Two shapes, depending on who owns the producer this frame:
            // - a GRAPH-RECORDED shadow pass (the maps are stale) writes the array, so the import declares its prior
            //   contents UNDEFINED: the first frame's image really is in UNDEFINED layout, and the producer's write
            //   (the only access) is allowed to discard it - the graph then emits the attachment transition the
            //   pass's legacy record used to own;
            // - otherwise (a reused map, or a frame that cannot record the pass) the map was handed back as sampled
            //   by the renderer before the graph records, and only the lighting reads are declared.
            std::optional<graph::texture_ref> shadow_cascades{};
            if (this->shadow_allocated_layers != 0) {
                auto shadow_generation = this->shadow_resource_view.find(render_resource::resource_id::shadow_map);
                if (shadow_generation && shadow_generation->device.get() == this->core_owner.get() && slot < shadow_generation->objects.size()) {
                    auto* shadow_image = shadow_generation->objects[slot].image.get();
                    if (shadow_image) {
                        auto shadow_lease = lease::from_shared(this->core_owner, std::shared_ptr<rhi::image>{std::move(shadow_generation), shadow_image});
                        graph::execution::import_state const shadow_import =
                            this->graph_shadow_frame
                                ? graph::execution::import_state{graph::initial_contents::undefined, {}}
                                : graph::execution::import_state{graph::initial_contents::defined,
                                                                 {graph::resource_layout::shader_read_only, graph::pipeline_stage::fragment_shader, graph::memory_access::read}};
                        auto shadow_ref = build.import_texture(shadow_lease, shadow_import, "shadow cascades");
                        if (!shadow_ref)
                            return post_build_error(shadow_ref.error());
                        shadow_cascades = *shadow_ref;
                    }
                }
            }
            using id = render_resource::resource_id;
            using snapshot = render_resource::instances::resource_snapshot;
            constexpr auto mesh = graph::pipeline_stage::mesh_shader;
            constexpr auto fragment = graph::pipeline_stage::fragment_shader;
            struct shared_import {
                snapshot const* resources;
                id resource;
                size_t instance;
                rhi::buffer* expected;
                graph::pipeline_stage stages;
                // THE ACCESS THE IMPORT REPORTS AS ITS PRIOR STATE, and the one that has to differ: the mesh
                // stage's statistics counters are ATOMICALLY read-modify-written (InterlockedAdd), so that
                // buffer is imported as read-write - it cannot join the read-only bundle.
                graph::memory_access access;
                std::string_view name;
            };
            std::array shared_imports{
                shared_import{&this->camera_resource_view, id::camera_ubo, slot, this->camera_buffers[slot].get(), mesh | fragment, graph::memory_access::read, "scene camera"},
                shared_import{&this->scene_table_resource_views[0], id::material_table, 0, this->material_buffer.get(), fragment, graph::memory_access::read, "scene materials"},
                shared_import{&this->scene_table_resource_views[1], id::instance_table, 0, this->instance_buffer.get(), mesh, graph::memory_access::read, "scene instances"},
                shared_import{&this->deformation_resource_views[0], id::motion_vectors, slot, this->motion_buffers[slot].get(), mesh, graph::memory_access::read, "previous transforms"},
                shared_import{&this->deformation_resource_views[1], id::skin_matrices, slot, this->skin_buffers[slot].get(), mesh, graph::memory_access::read, "skin matrices"},
                shared_import{&this->deformation_resource_views[2], id::skin_matrices_previous, slot, this->skin_buffers_previous[slot].get(), mesh, graph::memory_access::read, "previous skin matrices"},
                shared_import{&this->deformation_resource_views[3], id::morph_targets, slot, this->morph_buffers[slot].get(), mesh, graph::memory_access::read, "morph data"},
                shared_import{&this->light_resource_view, id::light_ubo, slot, this->light_buffers[slot].get(), fragment, graph::memory_access::read, "scene light"},
                // The GPU-driven geometry inputs: the meshlet records, this slot's host-culled run (both plain
                // storage reads) and this slot's atomic statistics counters (read-write). The indices into
                // scene_table_resource_views follow pass::scene_tables_pass::ids.
                shared_import{&this->scene_table_resource_views[5], id::meshlet_table, 0, this->meshlet_buffer.get(), mesh, graph::memory_access::read, "meshlet table"},
                shared_import{&this->scene_table_resource_views[8], id::meshlet_culled_table, 0, this->meshlet_culled_buffer.get(), mesh, graph::memory_access::read, "host-culled meshlets"},
                shared_import{&this->scene_table_resource_views[7], id::meshlet_statistics, 0, this->meshlet_stats_buffer.get(), mesh,
                              graph::memory_access::read | graph::memory_access::write, "meshlet statistics"},
                // The mesh draws are indirect records: the command buffer is consumed at the DRAW-INDIRECT stage,
                // so its import reports the draw-indirect stage instead of the mesh stage.
                shared_import{&this->scene_table_resource_views[6], id::mesh_indirect_commands, 0, this->mesh_indirect_buffer.get(),
                              graph::pipeline_stage::draw_indirect, graph::memory_access::read, "mesh indirect commands"}};
            std::array<graph::buffer_ref, 12> shared_refs;
            for (unsigned i = 0; i < shared_imports.size(); ++i) {
                auto const& entry = shared_imports[i];
                auto generation = entry.resources->find(entry.resource);
                if (!generation || generation->device.get() != this->core_owner.get() || entry.instance >= generation->objects.size())
                    return rhi::error::not_ready;
                auto* buffer = generation->objects[entry.instance].buffer.get();
                if (!buffer || buffer != entry.expected)
                    return rhi::error::invalid_argument;
                auto retained_buffer = resource_lease<rhi::buffer>::from_shared(this->core_owner, std::shared_ptr<rhi::buffer>{std::move(generation), buffer});
                auto imported = build.import_buffer(retained_buffer, {graph::initial_contents::defined, {graph::resource_layout::general, entry.stages, entry.access}}, entry.name);
                if (!imported)
                    return post_build_error(imported.error());
                shared_refs[i] = *imported;
            }
            graph::geometry::shared_inputs shared_geometry{shared_refs[0], shared_refs[1], shared_refs[2], shared_refs[3], shared_refs[4], shared_refs[5], shared_refs[6]};
            shared_geometry.meshlets = graph::geometry::meshlet_inputs{shared_refs[8], shared_refs[9], shared_refs[10], shared_refs[11]};
            graph::lighting::shared_inputs lighting_shared{shared_refs[0], shared_refs[7]};
            if (ibl_environment && ibl_irradiance && ibl_lut)
                lighting_shared.ibl = graph::lighting::ibl_inputs{*ibl_environment, *ibl_irradiance, *ibl_lut};
            if (cluster_counts_ref && cluster_indices_ref)
                lighting_shared.cluster = graph::lighting::cluster_inputs{*cluster_counts_ref, *cluster_indices_ref};
            if (shadow_cascades)
                lighting_shared.shadow_cascades = *shadow_cascades;
            lighting_inputs.shared = lighting_shared;
            // THE CLUSTERED-LIGHT SORT AS A GRAPH PASS (adoption stage 2G): its two per-slot buffers are imported
            // above and declared by the adapter, the lighting pass declares the same buffers as reads, and the
            // graph owns the producer -> consumer dependency - the pass's legacy path keeps its own two barriers
            // for legacy frames only, and its recorder here uses the barrier-free record_content entry.
            if (this->graph_cluster_frame) {
                auto cluster_added = build.append([&](graph::builder& description) noexcept {
                    return graph::cluster::append(description, {*cluster_counts_ref, *cluster_indices_ref});
                });
                if (!cluster_added)
                    return post_build_error(cluster_added.error());
                pass::stage cluster_stage{.name = "cluster", .passes = this->cluster_pass, .marks = false};
                this->prepare_stage(cluster_stage, true);
                passes[8] = this->cluster_pass[0];
                ios[8].push_block = host.push_block;
                if (!passes[8] || !host.resolve || !host.resolve(host.context, *passes[8], ios[8]) || ios[8].list != commands.get())
                    return rhi::error::not_ready;
                auto cluster_bound = build.bind_pass<graph::cluster::parameters>(
                    cluster_added->pass, [payload](graph::cluster::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(8); });
                if (!cluster_bound)
                    return post_build_error(cluster_bound.error());
            }
            // THE CASCADED-SHADOW PRODUCER AS A GRAPH PASS (adoption stage 2H): only on a frame whose maps are stale
            // (the reuse test), because the graph's write declaration has to match what the pass actually writes -
            // a reused map is not written and the lighting read already has its sampled state (the import above).
            // The write range is the RESOLVED target count, which is exactly the layer run the pass records.
            if (this->graph_shadow_frame && shadow_cascades) {
                pass::stage shadow_stage{.name = "shadow", .passes = this->shadow_pass, .marks = false};
                this->prepare_stage(shadow_stage, true);
                passes[9] = this->shadow_pass[0];
                ios[9].push_block = host.push_block;
                if (!passes[9] || !host.resolve || !host.resolve(host.context, *passes[9], ios[9]) || ios[9].list != commands.get() || ios[9].targets.empty())
                    return rhi::error::not_ready;
                auto shadow_added = build.append([&](graph::builder& description) noexcept {
                    return graph::shadow::append(description, {*shadow_cascades, static_cast<std::uint32_t>(ios[9].targets.size())});
                });
                if (!shadow_added)
                    return post_build_error(shadow_added.error());
                auto shadow_bound = build.bind_pass<graph::shadow::parameters>(
                    shadow_added->pass, [payload](graph::shadow::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(9); });
                if (!shadow_bound)
                    return post_build_error(shadow_bound.error());
            }
            auto geometry = build.append([&](graph::builder& description) noexcept {
                return graph::geometry::append(description, {scene_target, lighting_inputs.surface, *velocity_ref, *depth_ref, shared_geometry});
            });
            if (!geometry)
                return post_build_error(geometry.error());
            this->update_pass_geometry();
            pass::stage scene_stage{.name = "scene", .passes = this->scene_pass, .marks = false};
            this->prepare_stage(scene_stage, true);
            passes[7] = this->scene_pass[0];
            ios[7].push_block = host.push_block;
            if (!passes[7] || !host.resolve || !host.resolve(host.context, *passes[7], ios[7]) ||
                ios[7].list != commands.get() || ios[7].targets.size() != 6)
                return rhi::error::not_ready;
            std::array expected{this->gbuffer_images[0][index].get(), this->gbuffer_images[1][index].get(), this->gbuffer_images[2][index].get(),
                                velocity.get(), scene_target_image, depth.get()};
            for (unsigned i = 0; i < expected.size(); ++i)
                if (ios[7].targets[i].image_handle != expected[i])
                    return rhi::error::invalid_argument;
            auto geometry_bound = build.bind_pass<graph::geometry::parameters>(geometry->pass,
                                                                               [this, payload, index](graph::geometry::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                                                                                   auto error = payload->record(7);
                                                                                   if (error != rhi::error::ok)
                                                                                       return error;
                                                                                   this->gbuffer_depth_written[index] = true;
                                                                                   this->gbuffer_targets_written[index] = true;
                                                                                   this->velocity_written[index] = true;
                                                                                   this->gpu_mark(gpu_mark_id::scene_end);
                                                                                   this->gpu_mark(gpu_mark_id::rt_shadow_end);
                                                                                   return rhi::error::ok;
                                                                               });
            if (!geometry_bound)
                return post_build_error(geometry_bound.error());
            if (!this->graph_debug_frame) {
                auto appended = build.append([&](graph::builder& description) noexcept { return graph::lighting::append(description, lighting_inputs); });
                if (!appended)
                    return post_build_error(appended.error());
                lit = *appended;
                pass::stage stage{.name = "deferred", .passes = this->deferred_pass, .marks = false};
                this->prepare_stage(stage, true);
                passes[6] = this->deferred_pass[0];
                ios[6].push_block = host.push_block;
                if (!passes[6] || !host.resolve || !host.resolve(host.context, *passes[6], ios[6]) ||
                    ios[6].list != commands.get() || ios[6].targets.size() != 1 || ios[6].targets[0].image_handle != scene_target_image)
                    return rhi::error::not_ready;
                auto bound = build.bind_pass<graph::lighting::parameters>(lit->pass,
                                                                          [this, payload, index](graph::lighting::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                                                                              auto error = payload->record(6);
                                                                              if (error != rhi::error::ok)
                                                                                  return error;
                                                                              this->gbuffer_depth_written[index] = false;
                                                                              this->gbuffer_targets_written[index] = false;
                                                                              this->gpu_mark(gpu_mark_id::lighting_end);
                                                                              this->gpu_mark(gpu_mark_id::gaussian_prepare_end);
                                                                              this->gpu_mark(gpu_mark_id::gaussian_sort_end);
                                                                              this->gpu_mark(gpu_mark_id::gaussian_finalize_end);
                                                                              this->gpu_mark(gpu_mark_id::gaussian_raster_end);
                                                                              this->gpu_mark(gpu_mark_id::transparent_end);
                                                                              this->gpu_mark(gpu_mark_id::taa_end);
                                                                              this->gpu_mark(gpu_mark_id::main_end);
                                                                              return rhi::error::ok;
                                                                          });
                if (!bound)
                    return post_build_error(bound.error());
            } else {
                // THE G-BUFFER DEBUG VIEW AS A GRAPH PASS (adoption stage 2J): it samples the stored targets and the
                // motion vectors and CLEARS the HDR target into one visible channel - the lane that runs INSTEAD of
                // the lighting stage on a debug frame. Its four input transitions and the HDR write are the graph's
                // now; the frame loop's own HDR transition for this view belongs to legacy frames only.
                pass::stage debug_stage{.name = "gbuffer_debug", .passes = this->gbuffer_debug_stage, .marks = false};
                this->prepare_stage(debug_stage, true);
                passes[12] = this->gbuffer_debug_stage[0];
                ios[12].push_block = host.push_block;
                if (!passes[12] || !host.resolve || !host.resolve(host.context, *passes[12], ios[12]) || ios[12].list != commands.get() ||
                    ios[12].targets.size() != 1 || ios[12].targets[0].image_handle != scene_target_image)
                    return rhi::error::not_ready;
                auto debug_added = build.append([&](graph::builder& description) noexcept {
                    return graph::gbuffer_debug::append(description, {lighting_inputs.surface, *velocity_ref, scene_target});
                });
                if (!debug_added)
                    return post_build_error(debug_added.error());
                auto debug_bound = build.bind_pass<graph::gbuffer_debug::parameters>(
                    debug_added->pass, [payload](graph::gbuffer_debug::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(12); });
                if (!debug_bound)
                    return post_build_error(debug_bound.error());
            }
            // THE GAUSSIAN RASTER AS A GRAPH PASS (adoption stage 2O): the legacy tail runs it BEFORE transparency
            // (lighting -> gaussian -> transparent), so it is appended here. Its GPU compute chain keeps its own
            // buffer barriers inside the recorder - the snapshot's private buffers, no other pass touches them - and
            // the graph owns the pass-level boundary: the working image as a preserve write and the depth as a read.
            if (this->gaussian_active()) {
                pass::stage gaussian_stage{.name = "gaussian", .passes = this->gaussian_pass, .marks = false};
                this->prepare_stage(gaussian_stage, true);
                passes[17] = this->gaussian_pass[0];
                ios[17].push_block = host.push_block;
                if (!passes[17] || !host.resolve || !host.resolve(host.context, *passes[17], ios[17]) || ios[17].list != commands.get() ||
                    ios[17].targets.size() != 2 || ios[17].targets[0].image_handle != scene_target_image || ios[17].targets[1].image_handle != depth.get())
                    return rhi::error::not_ready;
                auto gaussian_added = build.append([&](graph::builder& description) noexcept {
                    return graph::gaussian::append(description, {scene_target, *depth_ref});
                });
                if (!gaussian_added)
                    return post_build_error(gaussian_added.error());
                auto gaussian_bound = build.bind_pass<graph::gaussian::parameters>(
                    gaussian_added->pass, [payload](graph::gaussian::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(17); });
                if (!gaussian_bound)
                    return post_build_error(gaussian_bound.error());
            }
            // THE TRANSPARENT PASS AS A GRAPH PASS (adoption stage 2K): it blends the alpha geometry over the lit
            // working image and reads the opaque depth. The graph owns the two hand-off transitions the pass's
            // legacy record issues itself, and this declaration is what orders lighting -> transparent -> TAA.
            if (this->active_features().transparent) {
                pass::stage transparent_stage{.name = "transparent", .passes = this->transparent_pass, .marks = false};
                this->prepare_stage(transparent_stage, true);
                passes[13] = this->transparent_pass[0];
                ios[13].push_block = host.push_block;
                if (!passes[13] || !host.resolve || !host.resolve(host.context, *passes[13], ios[13]) || ios[13].list != commands.get() ||
                    ios[13].targets.size() < 2 || ios[13].targets[0].image_handle != scene_target_image || ios[13].targets[1].image_handle != depth.get())
                    return rhi::error::not_ready;
                auto transparent_added = build.append([&](graph::builder& description) noexcept {
                    return graph::transparent::append(description, {scene_target, *depth_ref});
                });
                if (!transparent_added)
                    return post_build_error(transparent_added.error());
                auto transparent_bound = build.bind_pass<graph::transparent::parameters>(
                    transparent_added->pass, [payload](graph::transparent::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(13); });
                if (!transparent_bound)
                    return post_build_error(transparent_bound.error());
            }
            // THE CHARACTER CHAIN (adoption stage 2L): the character-forward pass and the two screen rims compose over
            // the shaded frame in the legacy order (transparent -> character forward -> toon rim -> goo rim -> TAA).
            // All three keep the scene target as a PRESERVE write; the forward pass reads the depth as a depth test,
            // the rims declare the depth/albedo reads their shaders take through per-image heap slots.
            if (this->feature_active("character_forward")) {
                pass::stage character_stage{.name = "character_forward", .passes = this->character_forward_pass, .marks = false};
                this->prepare_stage(character_stage, true);
                passes[14] = this->character_forward_pass[0];
                ios[14].push_block = host.push_block;
                if (!passes[14] || !host.resolve || !host.resolve(host.context, *passes[14], ios[14]) || ios[14].list != commands.get() ||
                    ios[14].targets.size() < 2 || ios[14].targets[0].image_handle != scene_target_image || ios[14].targets[1].image_handle != depth.get())
                    return rhi::error::not_ready;
                auto character_added = build.append([&](graph::builder& description) noexcept {
                    return graph::character_forward::append(description, {scene_target, *depth_ref});
                });
                if (!character_added)
                    return post_build_error(character_added.error());
                auto character_bound = build.bind_pass<graph::character_forward::parameters>(
                    character_added->pass, [payload](graph::character_forward::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(14); });
                if (!character_bound)
                    return post_build_error(character_bound.error());
            }
            if (this->feature_active("toon_screen_rim")) {
                pass::stage toon_stage{.name = "toon_screen_rim", .passes = this->toon_screen_rim_pass, .marks = false};
                this->prepare_stage(toon_stage, true);
                passes[15] = this->toon_screen_rim_pass[0];
                ios[15].push_block = host.push_block;
                if (!passes[15] || !host.resolve || !host.resolve(host.context, *passes[15], ios[15]) || ios[15].list != commands.get() ||
                    ios[15].targets.size() != 1 || ios[15].targets[0].image_handle != scene_target_image)
                    return rhi::error::not_ready;
                auto toon_added = build.append([&](graph::builder& description) noexcept {
                    return graph::toon_screen_rim::append(description, {scene_target, *depth_ref, lighting_inputs.surface[0]});
                });
                if (!toon_added)
                    return post_build_error(toon_added.error());
                auto toon_bound = build.bind_pass<graph::toon_screen_rim::parameters>(
                    toon_added->pass, [payload](graph::toon_screen_rim::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(15); });
                if (!toon_bound)
                    return post_build_error(toon_bound.error());
            }
            if (this->feature_active("goo_rim")) {
                pass::stage goo_stage{.name = "goo_rim", .passes = this->goo_rim_pass, .marks = false};
                this->prepare_stage(goo_stage, true);
                passes[16] = this->goo_rim_pass[0];
                ios[16].push_block = host.push_block;
                if (!passes[16] || !host.resolve || !host.resolve(host.context, *passes[16], ios[16]) || ios[16].list != commands.get() ||
                    ios[16].targets.size() != 1 || ios[16].targets[0].image_handle != scene_target_image)
                    return rhi::error::not_ready;
                auto goo_added = build.append([&](graph::builder& description) noexcept {
                    return graph::goo_rim::append(description, {scene_target, *depth_ref, lighting_inputs.surface[0]});
                });
                if (!goo_added)
                    return post_build_error(goo_added.error());
                auto goo_bound = build.bind_pass<graph::goo_rim::parameters>(
                    goo_added->pass, [payload](graph::goo_rim::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(16); });
                if (!goo_bound)
                    return post_build_error(goo_bound.error());
            }
            // THE TAA RESOLVE AND ITS HISTORY COPY (adoption stage 2I): TWO graph passes from the one pass object.
            // The resolve turns the working image (which geometry and lighting just wrote) into this HDR image;
            // the copy moves that HDR image into the history. The graph owns the attachment transition, the
            // transfer round trip and the ordering between them - the pass's legacy record keeps all of that for
            // legacy frames only.
            if (this->graph_taa_frame && taa_history) {
                pass::stage taa_stage{.name = "taa", .passes = this->taa_pass, .marks = false};
                this->prepare_stage(taa_stage, true);
                passes[10] = this->taa_pass[0];
                ios[10].push_block = host.push_block;
                if (!passes[10] || !host.resolve || !host.resolve(host.context, *passes[10], ios[10]) || ios[10].list != commands.get())
                    return rhi::error::not_ready;
                passes[11] = this->taa_pass[0];
                ios[11].push_block = host.push_block;
                if (!host.resolve(host.context, *passes[11], ios[11]) || ios[11].list != commands.get())
                    return rhi::error::not_ready;
                auto resolve_added = build.append([&](graph::builder& description) noexcept {
                    return graph::taa::append_resolve(description, {scene_target, *velocity_ref, *depth_ref, *taa_history, inputs.hdr});
                });
                if (!resolve_added)
                    return post_build_error(resolve_added.error());
                auto resolve_bound = build.bind_pass<graph::taa::resolve_parameters>(
                    resolve_added->pass, [payload](graph::taa::resolve_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(10); });
                if (!resolve_bound)
                    return post_build_error(resolve_bound.error());
                auto copy_added = build.append([&](graph::builder& description) noexcept {
                    return graph::taa::append_history_copy(description, {inputs.hdr, *taa_history});
                });
                if (!copy_added)
                    return post_build_error(copy_added.error());
                // THE COPY MUST BE LIVE, and that is not automatic: its only output is the history image, which no
                // pass in THIS frame reads - the value-root culling would drop the pass and the history would never
                // be written (measured: the graph path then behaved as if every frame were a first frame). Exporting
                // the history is what keeps the pass in the plan; the next frame's resolve is its consumer.
                if (auto exported = build.export_resource(*taa_history); !exported)
                    return post_build_error(exported.error());
                auto copy_bound = build.bind_pass<graph::taa::copy_parameters>(
                    copy_added->pass, [payload](graph::taa::copy_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                        return static_cast<pass::taa_pass*>(payload->passes[11])->record_copy(payload->ios[11]);
                    });
                if (!copy_bound)
                    return post_build_error(copy_bound.error());
            }
        }
        for (unsigned i = 0; i < 4; ++i)
            passes[i] = this->bloom_pass[i];
        passes[4] = this->post_composite_stage[0];
        passes[5] = settings.resolution == graph::post::resolve::fxaa ? this->fxaa_pass[0] : this->upscale_pass[0];
        unsigned count = settings.resolution == graph::post::resolve::none ? 5 : 6;
        for (unsigned i = 0; i < count; ++i) {
            ios[i].push_block = host.push_block;
            if (!passes[i] || !host.resolve || !host.resolve(host.context, *passes[i], ios[i]) ||
                ios[i].list != commands.get() || ios[i].targets.size() != 1)
                return rhi::error::not_ready;
            auto expected = i < 4 ? this->bloom_images[i][index].get() : i == 4 && ldr ? ldr.get()
                                                                                       : this->rhi_face().frame_image();
            if (ios[i].targets[0].image_handle != expected)
                return rhi::error::invalid_argument;
            if (expected == this->rhi_face().frame_image()) {
                ios[i].target_storage[0] = {.image_handle = frame.get(), .view_handle = view->get()};
                ios[i].targets = std::span<pass::resolved_binding const>{ios[i].target_storage.data(), 1};
            }
        }
        auto output = build.append([&](graph::builder& description) noexcept {
            return graph::post::append(description, inputs, {settings.resolution, settings.bloom_enabled});
        });
        if (!output)
            return post_build_error(output.error());
        for (unsigned i = 0; i < 4; ++i) {
            auto bound = settings.bloom_enabled
                             ? build.bind_pass<graph::post::bloom_parameters>(output->bloom[i], [payload, i](graph::post::bloom_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(i); })
                             : build.bind_pass<graph::post::clear_parameters>(output->bloom[i], [payload, i](graph::post::clear_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return clear_post_target(payload->ios[i]); });
            if (!bound)
                return post_build_error(bound.error());
        }
        auto composite_bound = build.bind_pass<graph::post::composite_parameters>(output->composite,
                                                                                  [this, payload](graph::post::composite_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                                                                                      this->gpu_mark(gpu_mark_id::bloom_end);
                                                                                      return payload->record(4);
                                                                                  });
        if (!composite_bound)
            return post_build_error(composite_bound.error());
        if (output->resolve) {
            auto resolve_bound = build.bind_pass<graph::post::resolve_parameters>(*output->resolve,
                                                                                  [this, payload, upscale = settings.resolution == graph::post::resolve::upscale](graph::post::resolve_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                                                                                      this->gpu_mark(gpu_mark_id::composite_end);
                                                                                      if (upscale)
                                                                                          this->gpu_mark(gpu_mark_id::fxaa_end);
                                                                                      return payload->record(5);
                                                                                  });
            if (!resolve_bound)
                return post_build_error(resolve_bound.error());
        }
        auto exported = build.export_resource(output->color);
        if (!exported)
            return post_build_error(exported.error());
        auto prepared = build.prepare();
        if (!prepared)
            return post_build_error(prepared.error());
        retained.push_back(prepared->retention());
        auto error = prepared->record(*commands);
        if (error != rhi::error::ok)
            return error;
        // THE TAA HISTORY'S HAND-BACK (adoption stage 2I): the graph's copy leaves the history image in
        // transfer-destination, and the invariant every frame's import relies on is "the history is sampled outside
        // the graph" - so the frame that wrote it hands it back, exactly as the legacy TAA record does for its own
        // frame. The graph cannot own a state that outlives its plan, which is why this stays the runtime's.
        if (this->graph_taa_frame && taa_history_image != nullptr) {
            auto const hand_back = commands->barrier(rhi::image_barrier{.resource = taa_history_image,
                                                                        .from = rhi::image_use::transfer_destination,
                                                                        .to = rhi::image_use::shader_read,
                                                                        .range = {}});
            if (hand_back != rhi::error::ok)
                return hand_back;
        }
        // THE TAA STAGE'S COLLECTED RESULT: the legacy tail collects it after its own TAA stage, and a written history
        // is what publishes this frame's UNJITTERED view-projection for the NEXT frame's reprojection. Measured:
        // without this the graph-recorded TAA frame differs (the resolve reprojects against a stale matrix).
        if (this->graph_taa_frame)
            this->collect_stage("taa");
        if (this->main_graph_frame && !this->main_graph_reported) {
            deren::utility::log("main graph: scene geometry -> GBuffer handoff -> deferred LOAD -> post recorded in one graph");
            this->main_graph_reported = true;
        }
        if (settings.resolution == graph::post::resolve::none)
            this->gpu_mark(gpu_mark_id::composite_end);
        if (settings.resolution != graph::post::resolve::upscale)
            this->gpu_mark(gpu_mark_id::fxaa_end);
        this->bloom_graph_pending = true;
        this->post_graph_drew_frame = true;
        return rhi::error::ok;
    }
    rhi::error runtime::finish_post_graph() {
        auto plan = graph::post::compile_finish(graph::execution::identify(this->post_graph_output), this->create_options.native_window != nullptr);
        if (!plan)
            return rhi::error::invalid_argument;
        std::array<graph::execution::resource_binding, 1> binding{this->post_graph_output};
        auto prepared = graph::execution::prepared_graph::prepare(this->core_owner, std::move(*plan), binding);
        if (!prepared)
            return prepared.error().code;
        this->frame_graph_retention[this->cluster_retention_slot].push_back(prepared->retention());
        return prepared->record(*this->frame_command_buffer(), [](graph::execution::record_context const&) noexcept { return rhi::error::ok; });
    }
} // namespace deren::engine
