module;
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>  // std::memcpy: the probe's same-frame camera snapshot for the metadata
#include <expected> // the HZB lane's graph adapter answers std::expected, like the other adapters
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
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
import deren.engine.render_graph.rt_shadow;
import deren.engine.render_graph.gaussian;
import deren.engine.render_graph.visibility;
import deren.engine.pass.transparent;
import deren.engine.pass.taa;
import deren.engine.pass;
import deren.engine.pass.hzb_build; // the runtime-owned HZB build pass (lane 22) and its header constants
import deren.engine.pass.hzb_cull;  // the runtime-owned HZB cull pass (lane 21, M3 phase B2)
import deren.engine.pass.software_visibility;
import deren.engine.pass.software_merge;
import deren.engine.pass.output_targets;
import deren.engine.visibility.software;
import deren.engine.visibility.hzb; // the pyramid layout the probe's lane sizes are computed with (§4.1)
import deren.promise.rhi;
namespace deren::engine {
    namespace graph = deren::engine::render_graph;
    namespace rhi = deren::promise::rhi;
    namespace {
        struct post_recording_payload {
            pass::pass_host host{};
            // THE LANE NUMBER IS A SLOT IN THESE ARRAYS, AND THE WHOLE FRAME IS ONE GRAPH: the main-view passes and
            // the post chain share this payload, so a lane index is a NAMESPACE ACROSS BOTH. The assigned slots, as
            // of visibility-buffer v0.4c (a lane collision is SILENT until the day the pass actually runs):
            //   0..3 bloom levels      4 post composite      5 post resolve (fxaa/upscale)
            //   6 deferred lighting    7 scene               8 cluster    9 shadow
            //   10 TAA resolve         11 TAA history copy   12 gbuffer debug
            //   13 transparent         14 character forward   15 toon screen rim   16 goo rim
            //   17 gaussian splats     18 ray-traced shadow   19 VISIBILITY RASTER  20 VISIBILITY RESOLVE
            //   21 reserved for the HZB CULL lane (phase B; kept free so that phase can append it without
            //      renumbering), 22 HZB BUILD (M3 phase A2)
            // THE VISIBILITY LANES ARE 19/20 BECAUSE THOSE ARE THE LAST FREE SLOTS. The first attempt shipped them at
            // 3/4 (the post chain overwrote `passes[3]` with `post_bloom_3`) and the second at 10/11 (the TAA lane's
            // two passes): both are harmless while the visibility passes are pruned, and both mis-record another pass
            // the moment one runs - measured, first as a `post_bloom_3` begin_rendering in the wrong layout, then as
            // the post composite recording the resolve pass instead of itself (a transparent-black frame).
            std::array<pass::resolved_io, 27> ios;
            std::array<pass::frame_pass*, 27> passes{};
            rhi::error record(unsigned lane) const noexcept {
                if (host.apply_behaviour) {
                    host.apply_behaviour(host.context, *passes[lane], ios[lane]);
                }
                auto error = passes[lane]->record_content(ios[lane]);
                if (error != rhi::error::ok) { // which lane refused, and with what - one line, once per failure
                    deren::utility::log("post graph: lane {} refused recording ({})", lane, static_cast<uint32_t>(error));
                }
                return error;
            }
        };
        rhi::error post_build_error(graph::execution::build_error const& error) noexcept {
            if (!error.execution) {
                return rhi::error::invalid_argument;
            }
            // WHICH PASS AND RESOURCE, when the execution layer names them: an unusable usage or stage is the one
            // failure whose code alone cannot be acted on.
            deren::utility::log("post graph: execution refused ({}) at pass {} resource {}", static_cast<uint32_t>(error.execution->code),
                                error.execution->pass, error.execution->resource);
            return error.execution->code;
        }
        rhi::error clear_post_target(pass::resolved_io const& io) noexcept {
            if (!io.list || io.targets.empty() || !io.targets[0].view_handle || !io.extent.width || !io.extent.height) {
                return rhi::error::invalid_argument;
            }
            std::array<rhi::color_attachment, 1> colors{{{.view = io.targets[0].view_handle, .load = rhi::load_op::clear, .store = rhi::store_op::store, .clear = {}}}};
            rhi::rendering_info info{.struct_size = sizeof(rhi::rendering_info),
                                     .area = {.offset_x = 0, .offset_y = 0, .width = io.extent.width, .height = io.extent.height},
                                     .layer_count = 1,
                                     .colors = colors,
                                     .depth = {},
                                     .has_depth = false,
                                     .secondary_contents = false};
            auto error = io.list->begin_rendering(info);
            if (error == rhi::error::ok) {
                io.list->end_rendering();
            }
            return error;
        }

        // ================================================================================================
        // THE HZB BUILD LANE'S GRAPH DECLARATION (M3 phase A2, spec §4.2/§8.2 step 3)
        //
        // ONE graph node with four declared accesses: the frame's DEPTH sampled read (its producer is the
        // visibility raster lane, so the graph orders build AFTER raster and emits the attachment -> sampled
        // transition), the frame camera as a uniform/storage read (the header's matrix snapshot - the shader reads
        // camera UBO lane `frame_slot`), and the TWO HZB TABLES as storage writes at the compute stage.
        //
        // THE TABLES ARE IMPORTED WITH UNDEFINED CONTENTS AND WRITTEN WHOLESALE (`discard_write`): nothing IN THIS
        // GRAPH reads them this phase - the only reader is the readback probe, a copy recorded manually after
        // `prepared->record` - so no value edge exists and the graph must not invent a "defined contents" state for
        // them. Phase B will add the cull lane that reads them, and that lane's declaration will turn the build
        // into a real producer (its import can then stay undefined; the consumer is what creates the edge).
        //
        // `side_effect = true` IS LOAD-BEARING: a pass whose outputs nothing in the graph reads would be culled by
        // the value-root liveness pass (render_graph.cpp's roots list) and the pyramid would never be built. This
        // is the same requirement the TAA history copy states with `export_resource` - here the side effect is the
        // pass's own: it writes tables the NEXT frame's phase-B cull will read, which no graph can see.
        //
        // IT IS DECLARED HERE RATHER THAN IN A `render_graph/hzb.cppm` because phase A has ONE such pass and this
        // is the file that appends it; the module-per-chain shape the other adapters use earns its file when the
        // cull lane joins in phase B.
        // ================================================================================================
        using graph::visibility::append_hzb_build;
        using graph::visibility::append_hzb_cull;
        using graph::visibility::hzb_build_parameters;
        using graph::visibility::hzb_cull_parameters;
    } // namespace
    rhi::error runtime::record_post_graph() {
        using lease = resource_lease<rhi::image>;
        // The HZB probe may only dump a frame whose build lane RECORDED it (see the probe block at the end of
        // this function): cleared here, once per frame, and set by that lane's recorder. A frame that is not a
        // main graph frame never runs the visibility lanes, so it must not inherit the previous frame's flag.
        this->hzb_recorded_this_frame = false;
        // ... and the same bookkeeping for the phase-B cull lane: `hzb_cull_active_frame` is THIS frame's frozen
        // verdict (set by evaluate_hzb_cull_lane below), `hzb_cull_recorded_this_frame` its recorder's answer.
        this->hzb_cull_active_frame = false;
        this->software_gpu_route_active_frame = false;
        if (this->software_visibility)
            this->software_visibility->disable_gpu_route();
        this->hzb_recovery_active_frame = false;
        this->hzb_cull_recorded_this_frame = false;
        auto commands = this->frame_command_buffer();
        if (!commands || !this->cluster_retention_open) {
            return rhi::error::not_ready;
        }
        // Texture residency demand is computed by the CPU from begin_recording()'s visible lists and
        // applied before the graph records; there is no GPU feedback pass/checkpoint on this path.
        bool const residency = false;
        std::optional<std::uint32_t> residency_resolve_index;
        auto index = this->current_image_index;
        if (index >= rhi::max_swapchain_images) {
            return rhi::error::invalid_argument;
        }
        auto frames = rhi::query_extension<rhi::frame_resources>(this->rhi_face());
        rhi::shared_object<rhi::image> frame = frames ? frames->retain_image() : rhi::shared_object<rhi::image>{};
        if (!frame) {
            return rhi::error::not_ready;
        }
        this->post_graph_output = lease::from_shared(this->core_owner, frame);
        auto frame_cluster = this->create_resource_cluster("post frame");
        auto view = frame_cluster->create_image_view(this->post_graph_output, {});
        if (!view) {
            return view.error();
        }
        auto& retained = this->frame_graph_retention[this->cluster_retention_slot];
        retained.push_back(frame_cluster);
        graph::post::options settings{};
        graph::resource_state input_state{graph::resource_layout::shader_read_only, graph::pipeline_stage::fragment_shader, graph::memory_access::read};
        if (this->main_graph_frame) {
            input_state = {};
        }
        graph::resource_state ldr_state{};
        std::array<graph::resource_state, 4> level_states{};
        graph::execution::builder build(this->core_owner);
        graph::post::inputs inputs{};
        auto output_ref = build.import_texture(this->post_graph_output, {}, "frame output");
        if (!output_ref) {
            return post_build_error(output_ref.error());
        }
        inputs.frame = *output_ref;
        settings.bloom_enabled = this->feature_active("bloom");
        if (this->post_upscale_active()) {
            settings.resolution = graph::post::resolve::upscale;
        } else if (this->post_fxaa_active()) {
            settings.resolution = graph::post::resolve::fxaa;
        } else {
            settings.resolution = graph::post::resolve::none;
        }
        for (auto it = this->bloom_graph_states.begin(); it != this->bloom_graph_states.end();) {
            if (it->first.expired()) {
                it = this->bloom_graph_states.erase(it);
            } else {
                ++it;
            }
        }
        auto retain = [&](unsigned family, unsigned pending, graph::resource_state& state) -> lease {
            auto generation = this->output_resource_views[family].find(pass::output_targets_pass::ids[family]);
            if (!generation || generation->device.get() != this->core_owner.get() || index >= generation->objects.size()) {
                return {};
            }
            auto image = generation->objects[index].image.get();
            if (!image) {
                return {};
            }
            if (pending < this->bloom_graph_pending_generations.size()) {
                auto known = this->bloom_graph_states.find(weak_bloom_generation{generation});
                if (known != this->bloom_graph_states.end() && known->second[index]) {
                    state = {graph::resource_layout::shader_read_only, graph::pipeline_stage::fragment_shader | graph::pipeline_stage::color_output,
                             graph::memory_access::read | graph::memory_access::write};
                }
                this->bloom_graph_pending_generations[pending] = generation;
            }
            return lease::from_shared(this->core_owner, rhi::shared_object<rhi::image>{std::move(generation), image});
        };
        auto input = retain(0, graph::invalid_index, input_state);
        if (!input || input.get() != this->hdr_images[index].get()) {
            return rhi::error::not_ready;
        }
        auto input_ref = build.import_texture(input, {this->main_graph_frame ? graph::initial_contents::undefined : graph::initial_contents::defined, input_state}, "post HDR");
        if (!input_ref) {
            return post_build_error(input_ref.error());
        }
        inputs.hdr = *input_ref;
        // THE TAA WORKING IMAGE AND HISTORY (adoption stage 2I): on a TAA frame the geometry and lighting passes
        // write the TAA working image (the "scene colour", family 5) and the resolve turns it into this HDR image.
        // The history (family 7) is the PREVIOUS frame's resolve: it is only ever transitioned, never discarded, and
        // it is always left SAMPLED outside the graph - this frame's copy's transfer-destination state is handed
        // back right after the graph records (see below), the same rule the removed legacy path's hand-back kept.
        graph::texture_ref scene_target = inputs.hdr;
        rhi::image* scene_target_image = input.get();
        std::optional<graph::texture_ref> taa_history{};
        rhi::image* taa_history_image = nullptr;
        if (this->graph_taa_frame) {
            graph::resource_state taa_scene_state{};
            auto taa_scene = retain(5, graph::invalid_index, taa_scene_state);
            auto const& ids = pass::output_targets_pass::ids;
            if (!taa_scene || !this->output_resource_views[5].find(ids[5])) {
                return rhi::error::not_ready;
            }
            auto taa_scene_ref = build.import_texture(taa_scene, {}, "TAA scene colour");
            if (!taa_scene_ref) {
                return post_build_error(taa_scene_ref.error());
            }
            scene_target = *taa_scene_ref;
            scene_target_image = taa_scene.get();
            graph::resource_state history_state{};
            auto taa_history_lease = retain(7, graph::invalid_index, history_state);
            if (!taa_history_lease || !this->output_resource_views[7].find(ids[7])) {
                return rhi::error::not_ready;
            }
            // THE HISTORY'S FIRST USE: the graph can only import "defined contents in a KNOWN layout", and a history
            // image that no path has transitioned yet is still in UNDEFINED - so the runtime performs the one
            // transition the removed legacy pass performed for itself, keeping the invariant every frame relies on: the
            // history is SAMPLED whenever the graph starts. (After this, the post-record hand-back keeps it so.)
            if (index >= this->taa_history_initialized.size() || !this->taa_history_initialized[index]) {
                auto const first_use = commands->barrier(rhi::image_barrier{.resource = taa_history_lease.get(),
                                                                            .from = rhi::image_use::undefined,
                                                                            .to = rhi::image_use::shader_read,
                                                                            .range = {}});
                if (first_use != rhi::error::ok) {
                    return first_use;
                }
                this->taa_history_initialized[index] = true;
            }
            graph::resource_state const history_prior{graph::resource_layout::shader_read_only, graph::pipeline_stage::fragment_shader, graph::memory_access::read};
            auto taa_history_ref = build.import_texture(taa_history_lease, {graph::initial_contents::defined, history_prior}, "TAA history");
            if (!taa_history_ref) {
                return post_build_error(taa_history_ref.error());
            }
            taa_history = *taa_history_ref;
            taa_history_image = taa_history_lease.get();
        }
        // THE REMOVED LEGACY TAA frame transitioned the history for itself (its first-use barrier) and left it sampled by its
        // own hand-back: record that too, so a later graph frame imports a KNOWN layout instead of re-issuing the
        // first-use transition on an image that is no longer UNDEFINED.
        if (this->taa_active() && index < this->taa_history_initialized.size()) {
            this->taa_history_initialized[index] = true;
        }
        for (unsigned i = 0; i < 4; ++i) {
            auto level = retain(9 + i, i, level_states[i]);
            if (!level || level.get() != this->bloom_images[i][index].get()) {
                return rhi::error::not_ready;
            }
            auto level_ref = build.import_texture(level, {graph::initial_contents::undefined, level_states[i]}, "bloom level");
            if (!level_ref) {
                return post_build_error(level_ref.error());
            }
            inputs.bloom[i] = *level_ref;
        }
        lease ldr;
        if (settings.resolution != graph::post::resolve::none) {
            ldr = retain(8, 4, ldr_state);
            if (!ldr || ldr.get() != this->ldr_images[index].get()) {
                return rhi::error::not_ready;
            }
            auto ldr_ref = build.import_texture(ldr, {graph::initial_contents::undefined, ldr_state}, "post LDR");
            if (!ldr_ref) {
                return post_build_error(ldr_ref.error());
            }
            inputs.ldr = *ldr_ref;
        }
        if (this->post_lut_image) {
            auto lut = lease::from_shared(this->core_owner, this->post_lut_image);
            auto lut_ref = build.import_texture(lut, {graph::initial_contents::defined, {graph::resource_layout::shader_read_only, graph::pipeline_stage::fragment_shader, graph::memory_access::read}}, "post LUT");
            if (!lut_ref) {
                return post_build_error(lut_ref.error());
            }
            inputs.lut = *lut_ref;
            auto kept = build.retain(this->post_lut_view);
            if (!kept) {
                return post_build_error(kept.error());
            }
        }
        pass::stage composite{.name = "post_composite", .passes = this->post_composite_stage};
        this->prepare_stage(composite);
        if (settings.resolution != graph::post::resolve::none) {
            pass::stage resolve{.name = settings.resolution == graph::post::resolve::fxaa ? "fxaa" : "upscale",
                                .passes = settings.resolution == graph::post::resolve::fxaa ? std::span<pass::frame_pass*>{this->fxaa_pass} : std::span<pass::frame_pass*>{this->upscale_pass}};
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
            // THE TWO STATICALLY BOUND IMAGES' LAYOUT PROTOCOL (stage 2B): the deferred lighting descriptor keeps the
            // RT visibility and MegaLights resolve images bound even when their producers are predicated out, and the
            // graph cannot declare a read of contents that are undefined by design. The runtime owns that ONE
            // initialization transition - but only once per GENERATION (the same boundary rule the TAA history's
            // first use follows): no eligible graph frame runs those producers, so after the seed the images stay
            // sampled and every later frame is barrier-free from the runtime's side.
            if (!this->inactive_descriptor_images_seeded) {
                std::array<rhi::image*, 1> inactive{this->rt_shadow_images[this->frame_ring().position()].get()};
                for (auto* image : inactive) {
                    if (image) {
                        auto error = commands->barrier(rhi::image_barrier{.resource = image, .from = rhi::image_use::undefined, .to = rhi::image_use::shader_read, .range = {}});
                        if (error != rhi::error::ok) {
                            return error;
                        }
                    }
                }
                this->inactive_descriptor_images_seeded = true;
            }
            graph::lighting::inputs lighting_inputs{};
            lighting_inputs.color = scene_target;
            for (unsigned i = 0; i < 3; ++i) {
                graph::resource_state state{};
                auto surface = retain(1 + i, graph::invalid_index, state);
                if (!surface || surface.get() != this->gbuffer_images[i][index].get()) {
                    return rhi::error::not_ready;
                }
                auto ref = build.import_texture(surface, {graph::initial_contents::undefined, state}, "GBuffer surface");
                if (!ref) {
                    return post_build_error(ref.error());
                }
                lighting_inputs.surface[i] = *ref;
            }
            graph::resource_state depth_state{};
            auto depth = retain(6, graph::invalid_index, depth_state);
            if (!depth || depth.get() != this->gbuffer_depth_images[index].get()) {
                return rhi::error::not_ready;
            }
            auto depth_ref = build.import_texture(depth, {graph::initial_contents::undefined, depth_state}, "GBuffer depth");
            if (!depth_ref) {
                return post_build_error(depth_ref.error());
            }
            lighting_inputs.depth = *depth_ref;
            graph::resource_state velocity_state{};
            auto velocity = retain(4, graph::invalid_index, velocity_state);
            if (!velocity || velocity.get() != this->velocity_images[index].get()) {
                return rhi::error::not_ready;
            }
            auto velocity_ref = build.import_texture(velocity, {}, "GBuffer velocity");
            if (!velocity_ref) {
                return post_build_error(velocity_ref.error());
            }
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
                    if (!this->ibl_images[i]) {
                        continue;
                    }
                    auto ibl_lease = lease::from_shared(this->core_owner, this->ibl_images[i]);
                    auto ref = build.import_texture(ibl_lease,
                                                    {graph::initial_contents::defined,
                                                     {graph::resource_layout::shader_read_only, graph::pipeline_stage::fragment_shader, graph::memory_access::read}},
                                                    names[i]);
                    if (!ref) {
                        return post_build_error(ref.error());
                    }
                    *refs[i] = *ref;
                }
            }
            // CPU writes were published for the completed slot before submission. These
            // imports describe GPU reads, not fictitious GPU upload writes.
            auto const slot = this->frame_ring().position();
            if (slot >= this->camera_buffers.size() || slot >= this->light_buffers.size() ||
                slot >= this->motion_buffers.size() || slot >= this->skin_buffers.size() ||
                slot >= this->skin_buffers_previous.size() || slot >= this->morph_buffers.size()) {
                return rhi::error::not_ready;
            }
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
                    slot >= indices_generation->objects.size()) {
                    return rhi::error::not_ready;
                }
                auto* counts = counts_generation->objects[slot].buffer.get();
                auto* indices = indices_generation->objects[slot].buffer.get();
                if (!counts || !indices) {
                    return rhi::error::not_ready;
                }
                auto counts_lease = resource_lease<rhi::buffer>::from_shared(this->core_owner, rhi::shared_object<rhi::buffer>{std::move(counts_generation), counts});
                auto counts_import =
                    build.import_buffer(counts_lease,
                                        {graph::initial_contents::defined,
                                         {graph::resource_layout::general, graph::pipeline_stage::compute_shader, graph::memory_access::read | graph::memory_access::write}},
                                        "cluster counts");
                if (!counts_import) {
                    return post_build_error(counts_import.error());
                }
                auto indices_lease = resource_lease<rhi::buffer>::from_shared(this->core_owner, rhi::shared_object<rhi::buffer>{std::move(indices_generation), indices});
                auto indices_import = build.import_buffer(indices_lease, {}, "cluster indices");
                if (!indices_import) {
                    return post_build_error(indices_import.error());
                }
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
                        auto shadow_lease = lease::from_shared(this->core_owner, rhi::shared_object<rhi::image>{std::move(shadow_generation), shadow_image});
                        graph::execution::import_state const shadow_import =
                            this->graph_shadow_frame
                                ? graph::execution::import_state{graph::initial_contents::undefined, {}}
                                : graph::execution::import_state{graph::initial_contents::defined,
                                                                 {graph::resource_layout::shader_read_only, graph::pipeline_stage::fragment_shader, graph::memory_access::read}};
                        auto shadow_ref = build.import_texture(shadow_lease, shadow_import, "shadow cascades");
                        if (!shadow_ref) {
                            return post_build_error(shadow_ref.error());
                        }
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
                              graph::pipeline_stage::draw_indirect, graph::memory_access::read, "mesh indirect commands"},
                // THE VISIBILITY ITEM TABLE (v0.2b): the raster lane declares it as a read so the table's
                // producer/consumer dependency is the graph's
                shared_import{&this->scene_table_resource_views[9], id::visibility_items, 0, this->visibility_items_buffer.get(), mesh,
                              graph::memory_access::read, "visibility items"}};
            std::array<graph::buffer_ref, 13> shared_refs;
            for (unsigned i = 0; i < shared_imports.size(); ++i) {
                auto const& entry = shared_imports[i];
                auto generation = entry.resources->find(entry.resource);
                if (!generation || generation->device.get() != this->core_owner.get() || entry.instance >= generation->objects.size()) {
                    return rhi::error::not_ready;
                }
                auto* buffer = generation->objects[entry.instance].buffer.get();
                if (!buffer || buffer != entry.expected) {
                    return rhi::error::invalid_argument;
                }
                auto retained_buffer = resource_lease<rhi::buffer>::from_shared(this->core_owner, rhi::shared_object<rhi::buffer>{std::move(generation), buffer});
                auto imported = build.import_buffer(retained_buffer, {graph::initial_contents::defined, {graph::resource_layout::general, entry.stages, entry.access}}, entry.name);
                if (!imported) {
                    return post_build_error(imported.error());
                }
                shared_refs[i] = *imported;
            }
            graph::geometry::shared_inputs shared_geometry{shared_refs[0], shared_refs[1], shared_refs[2], shared_refs[3], shared_refs[4], shared_refs[5], shared_refs[6]};
            shared_geometry.meshlets = graph::geometry::meshlet_inputs{shared_refs[8], shared_refs[9], shared_refs[10], shared_refs[11]};
            // THE RAY-TRACED VISIBILITY IMAGE (RT stage 2b): its producer is a lane of this graph and the lighting
            // stage declares the matching sampled read, so the graph owns the write -> hand-off pair the pass's
            // legacy record issues itself. Imported with a KNOWN sampled state: the 2B seed transitioned it once
            // this generation, and the per-frame write is the layout transition the plan emits.
            std::optional<graph::texture_ref> rt_visibility{};
            if (this->graph_rt_frame) {
                // The family's publication is per FRAME SLOT (one visibility image per structure/FIF slot), which is why
                // this is the shadow map's shape rather than `retain`'s: the lease comes from the generation's object at
                // the slot, and it is checked against the borrowed array the pass's descriptor sees.
                auto const rt_slot = this->frame_ring().position();
                auto rt_generation = this->output_resource_views[13].find(pass::output_targets_pass::ids[13]);
                if (!rt_generation || rt_generation->device.get() != this->core_owner.get() || rt_slot >= rt_generation->objects.size()) {
                    return rhi::error::not_ready;
                }
                auto* rt_image = rt_generation->objects[rt_slot].image.get();
                if (!rt_image || !this->rt_shadow_image_views[rt_slot] || rt_image != this->rt_shadow_images[rt_slot].get()) {
                    return rhi::error::not_ready;
                }
                auto rt_lease = lease::from_shared(this->core_owner, rhi::shared_object<rhi::image>{std::move(rt_generation), rt_image});
                if (!rt_lease) {
                    return rhi::error::not_ready;
                }
                graph::execution::import_state const rt_import{
                    graph::initial_contents::defined,
                    graph::resource_state{graph::resource_layout::shader_read_only, graph::pipeline_stage::fragment_shader, graph::memory_access::read}};
                auto rt_ref = build.import_texture(rt_lease, rt_import, "RT shadow visibility");
                if (!rt_ref) {
                    return post_build_error(rt_ref.error());
                }
                rt_visibility = *rt_ref;
            }
            graph::lighting::shared_inputs lighting_shared{shared_refs[0], shared_refs[7]};
            if (ibl_environment && ibl_irradiance && ibl_lut) {
                lighting_shared.ibl = graph::lighting::ibl_inputs{*ibl_environment, *ibl_irradiance, *ibl_lut};
            }
            if (cluster_counts_ref && cluster_indices_ref) {
                lighting_shared.cluster = graph::lighting::cluster_inputs{*cluster_counts_ref, *cluster_indices_ref};
            }
            if (shadow_cascades) {
                lighting_shared.shadow_cascades = *shadow_cascades;
            }
            if (rt_visibility) {
                lighting_shared.rt_visibility = *rt_visibility;
            }
            lighting_inputs.shared = lighting_shared;
            // THE CLUSTERED-LIGHT SORT AS A GRAPH PASS (adoption stage 2G): its two per-slot buffers are imported
            // above and declared by the adapter, the lighting pass declares the same buffers as reads, and the
            // graph owns the producer -> consumer dependency - the removed legacy path kept its own two barriers
            // for the frames it alone recorded, and its recorder here uses the barrier-free record_content entry.
            if (this->graph_cluster_frame) {
                auto cluster_added = build.append([&](graph::builder& description) noexcept {
                    return graph::cluster::append(description, {*cluster_counts_ref, *cluster_indices_ref});
                });
                if (!cluster_added) {
                    return post_build_error(cluster_added.error());
                }
                pass::stage cluster_stage{.name = "cluster", .passes = this->cluster_pass};
                this->prepare_stage(cluster_stage);
                passes[8] = this->cluster_pass[0];
                ios[8].push_block = host.push_block;
                if (!passes[8] || !host.resolve || !host.resolve(host.context, *passes[8], ios[8]) || ios[8].list != commands.get()) {
                    return rhi::error::not_ready;
                }
                auto cluster_bound = build.bind_pass<graph::cluster::parameters>(
                    cluster_added->pass, [payload](graph::cluster::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(8); });
                if (!cluster_bound) {
                    return post_build_error(cluster_bound.error());
                }
            }
            // THE CASCADED-SHADOW PRODUCER AS A GRAPH PASS (adoption stage 2H): only on a frame whose maps are stale
            // (the reuse test), because the graph's write declaration has to match what the pass actually writes -
            // a reused map is not written and the lighting read already has its sampled state (the import above).
            // The write range is the RESOLVED target count, which is exactly the layer run the pass records.
            if (this->graph_shadow_frame && shadow_cascades) {
                pass::stage shadow_stage{.name = "shadow", .passes = this->shadow_pass};
                this->prepare_stage(shadow_stage);
                passes[9] = this->shadow_pass[0];
                ios[9].push_block = host.push_block;
                if (!passes[9] || !host.resolve || !host.resolve(host.context, *passes[9], ios[9]) || ios[9].list != commands.get() || ios[9].targets.empty()) {
                    return rhi::error::not_ready;
                }
                auto shadow_added = build.append([&](graph::builder& description) noexcept {
                    return graph::shadow::append(description, {*shadow_cascades, static_cast<std::uint32_t>(ios[9].targets.size())});
                });
                if (!shadow_added) {
                    return post_build_error(shadow_added.error());
                }
                auto shadow_bound = build.bind_pass<graph::shadow::parameters>(
                    shadow_added->pass, [payload](graph::shadow::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(9); });
                if (!shadow_bound) {
                    return post_build_error(shadow_bound.error());
                }
            }
            // ---- THE VISIBILITY PATH'S TWO LANES (visibility-buffer v0.4c, made the opaque producer in v0.5) ----
            // WHEN THE FRAME'S FROZEN LANE IS `visibility`, THESE TWO PASSES ARE THE G-BUFFER'S PRODUCER: the raster
            // lane writes the packed id AND the frame's own shared depth (no separate capture depth any more - the
            // lighting, transparent and toon stages read the depth THIS lane wrote), and the resolve lane reads that
            // pair back and reconstructs the five colour targets. The geometry lane is then NOT recorded at all.
            //
            // THE LANE DECISION IS THE RUNTIME'S, FROZEN ONCE PER FRAME in record_main_drawcalls (`visibility_lane_frame`,
            // see deren::engine::visibility::choose_opaque_lane): the switch, the pipelines, the frame's scene and every
            // leaf's admission clause. A frame that is off OR cannot be carried records the geometry lane for every
            // leaf and the fallback line names the reason - a frame with two G-buffer authors is the failure the gate
            // exists to make impossible.
            //
            // THE ORDER VISIBILITY -> RESOLVE -> (skipped geometry) IS THE GRAPH'S, and it is the whole dependency:
            //   * the raster writes vis id + depth (both discard writes);
            //   * the resolve reads both (sampled) and writes the five colour targets as STORAGE images - the compute
            //     stage's only route, and the reason those five families carry STORAGE usage;
            //   * the lighting / debug / gaussian / transparent / toon lanes below read the same five targets and the
            //     depth exactly as they read the geometry lane's, so none of them knows which producer ran.
            bool const visibility_frame = this->visibility_lane_frame == deren::engine::visibility::opaque_lane::visibility;
            // ---- THE HZB LANES' SHARED ADMISSION, AND THE PHASE-B STATIC GATE (M3 phase A2 + B1) ----
            // The BUILD lane's predicate is phase A2's, unchanged (the gate, the two tables, the pipeline, and a
            // visibility-lane frame - §8.3: a geometry-lane frame builds nothing). The CULL lane adds its own §9.1
            // clauses AND the §9.2 byte-exact gate, evaluated ONCE, here, before any HZB resource is imported:
            // `evaluate_hzb_cull_lane` captures this frame's scene snapshot and compares it with the slot's saved
            // "what built the history HZB" record. A closed gate prunes the lane, and the frame keeps today's path.
            bool const hzb_build_lane = visibility_frame && this->hzb_build_enabled && this->hzb_resources_ready && this->hzb_build != nullptr && this->hzb_build->ready();
            std::uint32_t const hzb_item_count = static_cast<std::uint32_t>(this->frame_visible.size());
            bool const hzb_cull_lane = hzb_build_lane && this->hzb_cull_enabled && this->evaluate_hzb_cull_lane(hzb_item_count);
            std::optional<graph::buffer_ref> software_tiles_ref{}, software_route_arguments_ref{};
            std::optional<graph::visibility::software_routing_inputs> software_routing_ref{};
            auto const* gpu_mode = std::getenv("DEREN_SOFTWARE_RASTER_GPU");
            if (this->software_raster_active_frame && hzb_cull_lane && gpu_mode && std::string_view(gpu_mode) == "1" &&
                this->software_visibility && this->software_tiles_mapped && this->software_tiles_buffer) {
                namespace sw = deren::engine::visibility::software;
                sw::camera_parameters camera{};
                std::memcpy(camera.view.data(), &this->current_ubo.view, 64u);
                std::memcpy(camera.proj.data(), &this->current_ubo.proj, 64u);
                auto const extent = this->render_extent();
                camera.width = extent.width;
                camera.height = extent.height;
                auto metrics = sw::prepare_camera(camera);
                std::uint32_t budget = sw::software_entry_budget;
                bool valid_budget = true;
                if (auto const* requested = std::getenv("DEREN_SOFTWARE_GPU_BUDGET")) {
                    std::string_view text(requested);
                    auto parsed = std::from_chars(text.data(), text.data() + text.size(), budget);
                    valid_budget = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && budget <= sw::software_entry_budget;
                }
                auto const bytes = sw::software_tiles_bytes(extent.width, extent.height);
                if (metrics && valid_budget && bytes * this->frame_ring().slot_count() <= this->software_tiles_buffer->size()) {
                    pass::gpu_route_parameters parameters{.threshold_px = this->software_raster_threshold_px,
                                                          .near_plane = std::nextafter(static_cast<float>(metrics->near_plane), std::numeric_limits<float>::infinity()),
                                                          .projection_scale = std::nextafter(static_cast<float>(metrics->proj_scale), std::numeric_limits<float>::infinity()),
                                                          .perspective = metrics->kind == sw::projection_kind::perspective,
                                                          .budget = budget};
                    this->software_gpu_route_active_frame = this->software_visibility->prepare_gpu_route(this->frame_ring().position(), extent.width, extent.height, parameters);
                }
            }
            if (this->software_raster_active_frame) {
                auto generation = this->software_tiles_resource_view.find(render_resource::resource_id::software_tiles);
                if (!generation || generation->device.get() != this->core_owner.get() || generation->objects.empty() || !generation->objects[0].buffer)
                    return rhi::error::not_ready;
                auto retained = resource_lease<rhi::buffer>::from_shared(this->core_owner, rhi::shared_object<rhi::buffer>{generation, generation->objects[0].buffer.get()});
                auto imported = build.import_buffer(retained, {graph::initial_contents::defined, {graph::resource_layout::general, graph::pipeline_stage::compute_shader, graph::memory_access::read}}, "software work list");
                if (!imported)
                    return post_build_error(imported.error());
                software_tiles_ref = *imported;
            }
            if (this->software_gpu_route_active_frame) {
                auto metadata = build.import_buffer(resource_lease<rhi::buffer>::from_shared(this->core_owner, this->software_visibility->route_metadata_buffer()),
                                                    {graph::initial_contents::defined, {graph::resource_layout::general, graph::pipeline_stage::compute_shader, graph::memory_access::read}}, "software route metadata");
                auto arguments = build.import_buffer(resource_lease<rhi::buffer>::from_shared(this->core_owner, this->software_visibility->route_arguments_buffer()),
                                                     {graph::initial_contents::defined, {graph::resource_layout::general, graph::pipeline_stage::compute_shader, graph::memory_access::write}}, "software route arguments");
                if (!metadata || !arguments)
                    return post_build_error(!metadata ? metadata.error() : arguments.error());
                software_route_arguments_ref = *arguments;
                software_routing_ref = graph::visibility::software_routing_inputs{shared_refs[0], *metadata, *software_tiles_ref, *arguments};
            }
            // THE HISTORY LANES ARE IMPORTED ONCE, FOR BOTH LANES. A cull frame's history is DEFINED (it is the same
            // frame slot's previous use: the cull reads it and the build overwrites it, and the graph's WAR edge from
            // that read to the build's write is the §8.2 cull-before-build order). Without a cull lane the build
            // lane's import is phase A2's own - undefined contents, no state - so a closed-gate frame is byte-for-byte
            // a phase-A2 frame.
            std::optional<graph::buffer_ref> hzb_levels_ref{}, hzb_header_ref{};
            if (hzb_build_lane) {
                auto levels_import = hzb_cull_lane
                                         ? build.import_buffer(this->hzb_levels_buffer,
                                                               {graph::initial_contents::defined,
                                                                {graph::resource_layout::general, graph::pipeline_stage::compute_shader, graph::memory_access::read}},
                                                               "hzb levels")
                                         : build.import_buffer(this->hzb_levels_buffer, {}, "hzb levels");
                if (!levels_import) {
                    return post_build_error(levels_import.error());
                }
                auto header_import = hzb_cull_lane
                                         ? build.import_buffer(this->hzb_header_buffer,
                                                               {graph::initial_contents::defined,
                                                                {graph::resource_layout::general, graph::pipeline_stage::compute_shader, graph::memory_access::read}},
                                                               "hzb header")
                                         : build.import_buffer(this->hzb_header_buffer, {}, "hzb header");
                if (!header_import) {
                    return post_build_error(header_import.error());
                }
                hzb_levels_ref = *levels_import;
                hzb_header_ref = *header_import;
            }
            // ---- THE HZB CULL LANE (M3 phase B2, spec §4.3/§8.2 step 1): APPENDED BEFORE THE RASTER LANE ----
            // Its accesses are the history pyramid/header (reads), the phase-B item table (host-written read), the
            // frame's existing candidate/draw-item tables (reads) and its two result tables (writes). The explicit
            // order edge below pins the §8.2 order even though the WAR edge from the build lane's write would
            // already do it - the design's order is a contract, not an emergent property of the hazard analysis.
            std::optional<graph::pass_ref> hzb_cull_ref{};
            std::optional<graph::buffer_ref> hzb_survivors_ref{};
            std::optional<graph::buffer_ref> hzb_results_ref{};
            std::optional<graph::buffer_ref> hzb_items_ref{};
            if (hzb_cull_lane && hzb_levels_ref && hzb_header_ref) {
                auto items_import = build.import_buffer(this->hzb_items_buffer,
                                                        {graph::initial_contents::defined,
                                                         {graph::resource_layout::general, graph::pipeline_stage::compute_shader, graph::memory_access::read}},
                                                        "hzb items");
                auto results_import = build.import_buffer(this->hzb_results_buffer,
                                                          {graph::initial_contents::defined,
                                                           {graph::resource_layout::general, graph::pipeline_stage::compute_shader, graph::memory_access::read}},
                                                          "hzb results");
                auto survivors_import = build.import_buffer(this->hzb_survivors_buffer,
                                                            {graph::initial_contents::defined,
                                                             {graph::resource_layout::general, graph::pipeline_stage::compute_shader, graph::memory_access::write}},
                                                            "hzb survivors");
                if (!items_import || !results_import || !survivors_import) {
                    return post_build_error(!items_import ? items_import.error() : (!results_import ? results_import.error() : survivors_import.error()));
                }
                // THE CANDIDATE LANE AND THE DRAW-ITEM TABLE ARE THE FRAME'S OWN SHARED IMPORTS (shared_refs[9] and
                // [12]): the cull reads the records the host compacted in phase 2 and the model matrix the raster
                // will project with, so the two lanes cannot disagree about either.
                auto hzb_added = build.append([&](graph::builder& description) noexcept {
                    return append_hzb_cull(description, *hzb_levels_ref, *hzb_header_ref, *items_import, shared_refs[9], shared_refs[12], *results_import, *survivors_import, shared_refs[11], software_routing_ref);
                });
                if (!hzb_added) {
                    return post_build_error(hzb_added.error());
                }
                hzb_cull_ref = *hzb_added;
                hzb_survivors_ref = *survivors_import;
                hzb_results_ref = *results_import;
                hzb_items_ref = *items_import;
                // THE PHASE-B PROBE'S READBACKS ARE ALLOCATED HERE, BEFORE THE LANE RECORDS: the history-lane copy is
                // recorded INSIDE the cull lane (this frame's build overwrites the same lane later in the submission,
                // so after the graph would be too late - see runtime::begin_hzb_cull_probe).
                if (this->screenshot_requested && std::getenv("DEREN_HZB_CULL_PROBE_CAPTURE") != nullptr) {
                    (void)this->begin_hzb_cull_probe();
                }
                std::array<pass::frame_pass*, 1> hzb_cull_stage_passes{this->hzb_cull.get()};
                pass::stage hzb_cull_stage{.name = "hzb cull", .passes = hzb_cull_stage_passes};
                this->prepare_stage(hzb_cull_stage);
                passes[21] = this->hzb_cull.get();
                ios[21].push_block = host.push_block;
                if (!host.resolve || !host.resolve(host.context, *passes[21], ios[21]) || ios[21].list != commands.get()) {
                    return rhi::error::not_ready;
                }
                this->hzb_cull->set_frame(hzb_item_count, this->render_extent(), false, this->software_gpu_route_active_frame);
                auto hzb_bound = build.bind_pass<hzb_cull_parameters>(
                    *hzb_cull_ref, [this, payload](hzb_cull_parameters const&, rhi::command_buffer& command_buffer, graph::execution::record_context const&) noexcept {
                        auto const error = payload->record(21);
                        if (error != rhi::error::ok) {
                            return error;
                        }
                        // the lane's witness + its own timing boundary (the "hzb cull" interval ends here, before
                        // the raster lane's "visibility raster" interval starts - spec §10.5's split)
                        this->hzb_cull_recorded_this_frame = true;
                        this->hzb_cull_stats_valid = true;
                        this->hzb_cull_last_slot = this->frame_ring().position();
                        this->gpu_mark(gpu_mark_id::hzb_cull_end);
                        if (this->hzb_cull_probe_active) {
                            this->record_hzb_cull_probe_copy(command_buffer);
                        }
                        return rhi::error::ok;
                    });
                if (!hzb_bound) {
                    return post_build_error(hzb_bound.error());
                }
            }
            std::optional<graph::texture_ref> visibility_ref{};
            std::optional<graph::texture_ref> software_depth_ref{}, software_id_ref{};

            std::optional<graph::pass_ref> software_raster_ref{};
            std::optional<graph::visibility::software_raster_inputs> software_inputs_ref{};
            std::optional<graph::pass_ref> visibility_pass_ref{}, visibility_recovery_ref{}, hzb_build_pass_ref{};
            if (visibility_frame) {
                graph::resource_state visibility_id_state{};
                auto visibility_lease = retain(14, graph::invalid_index, visibility_id_state);
                if (!visibility_lease || visibility_lease.get() != this->visibility_images[index].get()) {
                    return rhi::error::not_ready;
                }
                auto visibility_import = build.import_texture(visibility_lease, {graph::initial_contents::undefined, {}}, "visibility buffer");
                if (!visibility_import) {
                    return post_build_error(visibility_import.error());
                }
                visibility_ref = *visibility_import;
                // THE RASTER LANE'S DEPTH IS THE FRAME'S G-BUFFER DEPTH, imported above with the surface targets -
                // the same image the lighting stage samples, and the same one the geometry lane would have written.
                graph::visibility::inputs visibility_inputs{*visibility_ref, *depth_ref};
                // THE RASTER STAGE'S THREE READS: the camera it projects through, the meshlet records it culls/emits
                // and this frame's indirect dispatch arguments (consumed at the DRAW-INDIRECT stage). The draw-item
                // table is NOT a raster read - its consumer is the resolve lane below.
                visibility_inputs.shared = graph::visibility::shared_inputs{shared_refs[0], shared_refs[8], shared_refs[11]};
                if (this->subtilis_active_frame) {
                    visibility_inputs.shared->culled_meshlets = shared_refs[9];
                }
                visibility_inputs.shared->hzb_survivors = hzb_survivors_ref;
                visibility_inputs.shared->hzb_results = hzb_results_ref;
                auto visibility_added = build.append([&](graph::builder& description) noexcept {
                    return graph::visibility::append(description, visibility_inputs);
                });
                if (!visibility_added) {
                    return post_build_error(visibility_added.error());
                }
                visibility_pass_ref = visibility_added->pass;
                if (this->software_raster_active_frame) {
                    auto import_software_image = [&](std::size_t const family, std::string_view const name) -> std::optional<graph::texture_ref> {
                        auto generation = this->output_resource_views[family].find(pass::output_targets_pass::ids[family]);
                        if (!generation || generation->device.get() != this->core_owner.get() || index >= generation->objects.size()) {
                            return {};
                        }
                        auto* image = generation->objects[index].image.get();
                        if (!image) {
                            return {};
                        }
                        auto image_lease = lease::from_shared(this->core_owner, rhi::shared_object<rhi::image>{std::move(generation), image});
                        auto imported = build.import_texture(image_lease, {graph::initial_contents::undefined, {}}, name);
                        return imported ? std::optional<graph::texture_ref>{*imported} : std::optional<graph::texture_ref>{};
                    };
                    auto depth_import = import_software_image(15u, "software visibility depth");
                    auto id_import = import_software_image(16u, "software visibility id");
                    if (!depth_import || !id_import || !software_tiles_ref)
                        return rhi::error::not_ready;
                    software_depth_ref = *depth_import;
                    software_id_ref = *id_import;

                    graph::visibility::software_raster_inputs inputs{*software_depth_ref, *software_id_ref, shared_refs[0], shared_refs[12], shared_refs[8], *software_tiles_ref, shared_refs[10], {}};
                    if (this->subtilis_active_frame) {
                        inputs.culled_meshlets = shared_refs[9];
                    }
                    if (hzb_cull_lane) {
                        inputs.hzb_results = hzb_results_ref;
                    }
                    inputs.indirect_arguments = software_route_arguments_ref;
                    software_inputs_ref = inputs;
                }
                // THE §8.2 ORDER, STATED RATHER THAN EMERGENT: cull BEFORE raster (phase B2's raster consumes the
                // survivor table and indirect commands the cull just wrote).
                if (hzb_cull_ref) {
                    if (auto ordered = build.order_after(visibility_added->pass, *hzb_cull_ref); !ordered) {
                        return post_build_error(ordered.error());
                    }
                }
                pass::stage visibility_stage{.name = "visibility raster", .passes = this->visibility_pass};
                this->prepare_stage(visibility_stage);
                passes[19] = this->visibility_pass[0];
                ios[19].push_block = host.push_block;
                if (!passes[19] || !host.resolve || !host.resolve(host.context, *passes[19], ios[19]) || ios[19].list != commands.get() ||
                    ios[19].targets.size() != 2) {
                    return rhi::error::not_ready;
                }
                this->visibility_raster->set_frame(this->make_visibility_frame());
                auto visibility_bound = build.bind_pass<graph::visibility::parameters>(
                    visibility_added->pass, [this, payload](graph::visibility::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                        // THE CULL LANE'S MARK IS POSITIONAL LIKE EVERY OTHER: a frame whose cull lane was pruned
                        // still has to emit `hzb_cull_end` AT THE CULL LANE'S POINT - i.e. before the raster
                        // records - or the interval would be measured as the raster's (the OFF path's pass report
                        // would call the raster "hzb cull"). `gpu_mark_through` is exactly that filler: a 0 ms
                        // interval when the lane did not run.
                        this->gpu_mark_through(gpu_mark_id::hzb_cull_end);
                        auto error = payload->record(19);
                        if (error != rhi::error::ok) {
                            return error;
                        }
                        // shadow_end -> hzb_cull_end is the cull lane; hzb_cull_end -> here is the MESH-TASK RASTER
                        // ALONE; here -> scene_end (written by the resolve lane below) is the resolve. BOTH paths
                        // write this mark, so the labels never lie per-path (the geometry callback writes it
                        // immediately before its pass).
                        this->gpu_mark(gpu_mark_id::visibility_raster_end);
                        return rhi::error::ok;
                    });
                if (!visibility_bound) {
                    return post_build_error(visibility_bound.error());
                }
            }
            auto append_build_lane = [&]() -> rhi::error {
                // ---- THE HZB BUILD LANE (M3 phase A2, spec §4.2/§8.2 step 3): the max pyramid of THIS frame's depth ----
                // Recorded AFTER the visibility raster (the depth it samples is final there, and the graph emits the
                // attachment -> sampled transition from that dependency) and BEFORE the resolve lane is the declared
                // order of §8.2; the graph schedules it by dependencies, which is the same thing because both lanes
                // only read the depth the raster wrote.
                //
                // ITS PREDICATE AND ITS TWO IMPORTS ARE HOISTED ABOVE (hzb_build_lane / hzb_levels_ref / hzb_header_ref):
                // the cull lane that runs BEFORE the raster needs the very same two buffers as reads, and a resource may
                // be imported once with one initial state - so the admission is one predicate and the imports are one
                // pair. With no cull lane this is exactly phase A2's `{}` import and the same discard write.
                if (hzb_build_lane && hzb_levels_ref && hzb_header_ref) {
                    auto hzb_added = build.append([&](graph::builder& description) noexcept {
                        return append_hzb_build(description, *hzb_levels_ref, *hzb_header_ref, *depth_ref, shared_refs[0]);
                    });
                    if (!hzb_added) {
                        return post_build_error(hzb_added.error());
                    }
                    hzb_build_pass_ref = *hzb_added;
                    std::array<pass::frame_pass*, 1> hzb_stage_passes{this->hzb_build.get()};
                    pass::stage hzb_stage{.name = "hzb build", .passes = hzb_stage_passes};
                    this->prepare_stage(hzb_stage);
                    passes[22] = this->hzb_build.get();
                    ios[22].push_block = host.push_block;
                    if (!host.resolve || !host.resolve(host.context, *passes[22], ios[22]) || ios[22].list != commands.get()) {
                        return rhi::error::not_ready;
                    }
                    // THE BARRIER BUFFER THE PASS RECORDS ITS INTER-LEVEL ORDERING ON: the graph cannot see inside one
                    // pass, so the pass is handed the levels table's handle (the only buffer it needs a role pair on).
                    this->hzb_build->set_hzb_levels(this->hzb_levels_buffer.get());
                    auto hzb_bound = build.bind_pass<hzb_build_parameters>(
                        *hzb_added, [this, payload](hzb_build_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                            auto const error = payload->record(22);
                            if (error == rhi::error::ok) {
                                this->hzb_recorded_this_frame = true; // the probe copies only a frame that actually built
                                // THE PHASE-B SNAPSHOT IS TAKEN HERE, FROM THE FRAME THAT BUILT THE PYRAMID (§9.2): the
                                // matrices/items the build just copied into the header and the depth it built from. This
                                // slot's next use (2 frames from now) compares against exactly this record.
                                if (this->hzb_cull_enabled) {
                                    this->save_hzb_source_scene();
                                }
                            }
                            if (error == rhi::error::ok && this->hzb_recovery_active_frame) {
                                this->gpu_mark(gpu_mark_id::hzb_build_end);
                            }
                            return error;
                        });
                    if (!hzb_bound) {
                        return post_build_error(hzb_bound.error());
                    }
                }
                return rhi::error::ok;
            };
            // Build from the completed hardware depth before optional software coverage. Without software this is
            // byte-equivalent input; with software it is conservative because omitted tiny occluders only reduce culling.
            if (!this->hzb_recovery_active_frame) {
                if (auto error = append_build_lane(); error != rhi::error::ok) {
                    return error;
                }
            }
            if (this->hzb_recovery_active_frame) {
                if (auto error = append_build_lane(); error != rhi::error::ok) {
                    return error;
                }
                if (!hzb_levels_ref || !hzb_header_ref || !hzb_items_ref || !hzb_results_ref || !hzb_survivors_ref) {
                    return rhi::error::not_ready;
                }
                auto recovery_added = build.append([&](graph::builder& description) noexcept {
                    return graph::visibility::append_hzb_recovery(description, *hzb_levels_ref, *hzb_header_ref, *hzb_items_ref, shared_refs[9], shared_refs[12], *hzb_results_ref, *hzb_survivors_ref, shared_refs[11], software_routing_ref);
                });
                if (!recovery_added) {
                    return post_build_error(recovery_added.error());
                }
                std::array<pass::frame_pass*, 1> recovery_stages{this->hzb_recovery.get()};
                pass::stage recovery_stage{.name = "hzb recovery", .passes = recovery_stages};
                this->prepare_stage(recovery_stage);
                passes[23] = this->hzb_recovery.get();
                ios[23].push_block = host.push_block;
                if (!host.resolve || !host.resolve(host.context, *passes[23], ios[23]) || ios[23].list != commands.get()) {
                    return rhi::error::not_ready;
                }
                this->hzb_recovery->set_frame(hzb_item_count, this->render_extent(), true, this->software_gpu_route_active_frame);
                auto recovery_bound = build.bind_pass<graph::visibility::hzb_recovery_parameters>(*recovery_added,
                                                                                                  [this, payload](graph::visibility::hzb_recovery_parameters const&, rhi::command_buffer& command_buffer, graph::execution::record_context const&) noexcept {
                                                                                                      auto error = payload->record(23);
                                                                                                      if (error == rhi::error::ok && this->hzb_cull_probe_active) {
                                                                                                          this->record_hzb_recovery_probe_copy(command_buffer);
                                                                                                      }
                                                                                                      if (error == rhi::error::ok) {
                                                                                                          this->gpu_mark(gpu_mark_id::hzb_recovery_end);
                                                                                                      }
                                                                                                      return error;
                                                                                                  });
                if (!recovery_bound) {
                    return post_build_error(recovery_bound.error());
                }
                graph::visibility::inputs load_inputs{*visibility_ref, *depth_ref, graph::visibility::shared_inputs{shared_refs[0], shared_refs[8], shared_refs[11]}};
                load_inputs.shared->culled_meshlets = shared_refs[9];
                load_inputs.shared->hzb_survivors = hzb_survivors_ref;
                load_inputs.shared->hzb_results = hzb_results_ref;
                auto load_added = build.append([&](graph::builder& description) noexcept { return graph::visibility::append_recovery(description, load_inputs); });
                if (!load_added) {
                    return post_build_error(load_added.error());
                }
                visibility_recovery_ref = load_added->pass;
                std::array<pass::frame_pass*, 1> load_stages{this->visibility_recovery.get()};
                pass::stage load_stage{.name = "visibility recovery", .passes = load_stages};
                this->prepare_stage(load_stage);
                passes[24] = this->visibility_recovery.get();
                ios[24].push_block = host.push_block;
                if (!host.resolve || !host.resolve(host.context, *passes[24], ios[24]) || ios[24].list != commands.get() || ios[24].targets.size() != 2) {
                    return rhi::error::not_ready;
                }
                this->visibility_recovery->set_frame(this->make_visibility_frame(true));
                auto load_bound = build.bind_pass<graph::visibility::raster_load_parameters>(load_added->pass,
                                                                                             [this, payload](graph::visibility::raster_load_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                                                                                                 auto error = payload->record(24);
                                                                                                 if (error == rhi::error::ok) {
                                                                                                     this->gpu_mark(gpu_mark_id::visibility_recovery_end);
                                                                                                 }
                                                                                                 return error;
                                                                                             });
                if (!load_bound) {
                    return post_build_error(load_bound.error());
                }
            }
            if (this->software_raster_active_frame) {
                if (!software_inputs_ref || !visibility_pass_ref) {
                    return rhi::error::not_ready;
                }
                this->software_visibility->set_hzb_results_active(hzb_cull_lane);
                this->software_visibility->set_frame_inputs(this->software_active_tile_count, this->software_depth_images[index].get(),
                                                            this->software_id_images[index].get());
                std::array<pass::frame_pass*, 1> software_stage_passes{this->software_visibility.get()};
                pass::stage software_stage{.name = "software visibility", .passes = software_stage_passes};
                this->prepare_stage(software_stage);
                passes[25] = this->software_visibility.get();
                ios[25].push_block = {this, &runtime::push_software_block};
                if (!passes[25] || !host.resolve || !host.resolve(host.context, *passes[25], ios[25]) || ios[25].list != commands.get()) {
                    return rhi::error::not_ready;
                }
                auto software_added = build.append([&](graph::builder& description) noexcept {
                    return graph::visibility::append_software_raster(description, *software_inputs_ref, *visibility_pass_ref, visibility_recovery_ref);
                });
                if (!software_added) {
                    return post_build_error(software_added.error());
                }
                software_raster_ref = software_added->pass;
                if (hzb_build_pass_ref) {
                    if (auto ordered = build.order_after(*software_raster_ref, *hzb_build_pass_ref); !ordered) {
                        return post_build_error(ordered.error());
                    }
                }
                auto software_bound = build.bind_pass<graph::visibility::software_raster_parameters>(
                    *software_raster_ref, [this, payload](graph::visibility::software_raster_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                        // The ordered mark range includes the optional HZB build/recovery steps, which precede this
                        // pass whenever enabled. Fill omitted steps before the software timing boundary.
                        this->gpu_mark_through(gpu_mark_id::visibility_recovery_end);
                        // Visibility recording has now published this frame's candidate count.
                        auto const index = this->current_image_index;
                        this->software_visibility->set_frame_inputs(this->software_active_tile_count,
                                                                    this->software_depth_images[index].get(), this->software_id_images[index].get());
                        auto const error = payload->record(25);
                        if (error == rhi::error::ok) {
                            this->gpu_mark(gpu_mark_id::software_raster_end);
                        }
                        return error;
                    });
                if (!software_bound) {
                    return post_build_error(software_bound.error());
                }
            }
            if (visibility_frame) {
                if (this->visibility_resolve == nullptr) {
                    // the admission said both pipelines exist; a null here is a wiring error, not a fallback case
                    return rhi::error::not_ready;
                }
                graph::visibility::resolve_inputs resolve_inputs{*visibility_ref, *depth_ref, lighting_inputs.surface, *velocity_ref, scene_target, {}};
                resolve_inputs.shared = graph::visibility::resolve_shared_inputs{
                    shared_refs[0],  // camera: the same per-frame block the raster projected with
                    shared_refs[1],  // material table: the shared decode reads it
                    shared_refs[12], // this frame's draw-item table
                    shared_refs[8],  // the meshlet record table the id's order indexes
                    shared_refs[3],  // previous transforms: the motion vector's second half
                    shared_refs[10]  // mesh statistics: the resolve's two failure counters are atomic adds
                };
                if (this->subtilis_active_frame) {
                    resolve_inputs.shared->culled_meshlets = shared_refs[9];
                }
                resolve_inputs.shared->hzb_survivors = hzb_survivors_ref;
                resolve_inputs.shared->hzb_results = hzb_results_ref;
                if (this->software_raster_active_frame) {
                    if (!software_depth_ref || !software_id_ref || !software_raster_ref) {
                        return rhi::error::not_ready;
                    }
                    resolve_inputs.software_depth = software_depth_ref;
                    resolve_inputs.software_id = software_id_ref;
                }
                auto resolve_added = build.append([&](graph::builder& description) noexcept {
                    return graph::visibility::append_resolve(description, resolve_inputs);
                });
                if (!resolve_added) {
                    return post_build_error(resolve_added.error());
                }
                if (residency) {
                    constexpr graph::access_scope storage{graph::resource_usage::storage, graph::pipeline_stage::compute_shader};
                    constexpr graph::access_scope sampled{graph::resource_usage::sampled, graph::pipeline_stage::compute_shader};
                    auto result_lease = resource_lease<rhi::buffer>::from_shared(core_owner, visibility_feedback_buffer);
                    auto feedback_ref = build.import_buffer(result_lease, {graph::initial_contents::defined, {graph::resource_layout::general, graph::pipeline_stage::compute_shader, graph::memory_access::read | graph::memory_access::write}}, "material feedback");
                    if (!feedback_ref)
                        return post_build_error(feedback_ref.error());
                    graph::visibility::feedback_parameters parameters{{*visibility_ref, {}, sampled}, {*depth_ref, {}, sampled}, {shared_refs[12], {}, storage}, {*feedback_ref, {}, storage}};
                    if (software_raster_active_frame) {
                        parameters.software_depth = graph::texture_read{*software_depth_ref, {}, sampled};
                        parameters.software_id = graph::texture_read{*software_id_ref, {}, sampled};
                    }
                    auto feedback = build.append([&](graph::builder& description) noexcept {
                        return description.add_pass("visibility material feedback", parameters, graph::pass_options{.side_effect = true});
                    });
                    if (!feedback)
                        return post_build_error(feedback.error());
                    auto bound = build.bind_pass<graph::visibility::feedback_parameters>(*feedback,
                                                                                         [this](graph::visibility::feedback_parameters const&, rhi::command_buffer& buffer, graph::execution::record_context const&) noexcept {
                                                                                             return record_texture_feedback(buffer);
                                                                                         });
                    if (!bound)
                        return post_build_error(bound.error());
                    if (auto order = build.order_after(resolve_added->pass, *feedback); !order)
                        return post_build_error(order.error());
                    residency_resolve_index = resolve_added->pass.index();
                }
                pass::stage resolve_stage{.name = "visibility resolve", .passes = this->visibility_resolve_stage};
                this->prepare_stage(resolve_stage);
                passes[20] = this->visibility_resolve_stage[0];
                ios[20].push_block = host.push_block;
                if (!passes[20] || !host.resolve || !host.resolve(host.context, *passes[20], ios[20]) || ios[20].list != commands.get()) {
                    return rhi::error::not_ready;
                }
                auto resolve_bound = build.bind_pass<graph::visibility::resolve_parameters>(
                    resolve_added->pass, [this, payload](graph::visibility::resolve_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                        this->gpu_mark_through(gpu_mark_id::software_raster_end);
                        auto error = payload->record(20);
                        if (error != rhi::error::ok) {
                            return error;
                        }
                        // THE GEOMETRY LANE'S GPU MARKS, EMITTED BY THE LANE THAT REPLACED IT: the per-pass
                        // timing report walks the frame's marks in gpu_mark_id order, and on a visibility frame the
                        // geometry lane (which wrote scene_end/rt_shadow_end) is not recorded - so without these
                        // two the NEXT marks are refused and the ON path's pass report is mislabeled (measured:
                        // "GPU timing mark 5 was refused (9)" repeated for the rest of the frame). The marks mean
                        // the same thing here: the opaque producer and the ray-traced shadow lane are the points
                        // the report names, whoever recorded them.
                        this->gpu_mark(gpu_mark_id::scene_end);
                        this->gpu_mark(gpu_mark_id::rt_shadow_end);
                        return rhi::error::ok;
                    });
                if (!resolve_bound) {
                    return post_build_error(resolve_bound.error());
                }
                if (this->software_raster_active_frame) {
                    auto merge_added = build.append([&](graph::builder& description) noexcept {
                        return graph::visibility::append_software_merge(
                            description, {*software_depth_ref, *software_id_ref, *depth_ref}, resolve_added->pass, *software_raster_ref);
                    });
                    if (!merge_added) {
                        return post_build_error(merge_added.error());
                    }
                    std::array<pass::frame_pass*, 1> merge_stage_passes{this->software_merge.get()};
                    pass::stage merge_stage{.name = "software merge", .passes = merge_stage_passes};
                    this->prepare_stage(merge_stage);
                    passes[26] = this->software_merge.get();
                    ios[26].push_block = host.push_block;
                    if (!passes[26] || !host.resolve || !host.resolve(host.context, *passes[26], ios[26]) ||
                        ios[26].list != commands.get() || ios[26].targets.size() != 1u) {
                        return rhi::error::not_ready;
                    }
                    auto merge_bound = build.bind_pass<graph::visibility::software_merge_parameters>(
                        merge_added->pass, [this, payload](graph::visibility::software_merge_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                            auto const error = payload->record(26);
                            if (error == rhi::error::ok) {
                                this->gpu_mark(gpu_mark_id::software_merge_end);
                            }
                            return error;
                        });
                    if (!merge_bound) {
                        return post_build_error(merge_bound.error());
                    }
                }
            }
            // THE GEOMETRY LANE (the old producer) IS RECORDED ONLY WHEN THE VISIBILITY LANE IS NOT: the two write
            // the same six attachments, so recording both would be two G-buffer authors. `update_pass_geometry`
            // still runs on EVERY frame - the transparent, character, shadow and cluster passes are not part of
            // either opaque lane and keep their cached viewports synced from here.
            this->update_pass_geometry();
            if (!visibility_frame) {
                auto geometry = build.append([&](graph::builder& description) noexcept {
                    return graph::geometry::append(description, {scene_target, lighting_inputs.surface, *velocity_ref, *depth_ref, shared_geometry});
                });
                if (!geometry) {
                    return post_build_error(geometry.error());
                }
                pass::stage scene_stage{.name = "scene", .passes = this->scene_pass};
                this->prepare_stage(scene_stage);
                passes[7] = this->scene_pass[0];
                ios[7].push_block = host.push_block;
                if (!passes[7] || !host.resolve || !host.resolve(host.context, *passes[7], ios[7]) ||
                    ios[7].list != commands.get() || ios[7].targets.size() != 6) {
                    return rhi::error::not_ready;
                }
                std::array expected{this->gbuffer_images[0][index].get(), this->gbuffer_images[1][index].get(), this->gbuffer_images[2][index].get(),
                                    velocity.get(), scene_target_image, depth.get()};
                for (unsigned i = 0; i < expected.size(); ++i) {
                    if (ios[7].targets[i].image_handle != expected[i]) {
                        return rhi::error::invalid_argument;
                    }
                }
                auto geometry_bound = build.bind_pass<graph::geometry::parameters>(geometry->pass,
                                                                                   [this, payload](graph::geometry::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                                                                                       // the cull lane did not run on this path (a geometry-lane frame builds
                                                                                       // and culls nothing), so its mark is filled here - the positional rule
                                                                                       // every optional pass follows (0 ms interval, labels never shift).
                                                                                       this->gpu_mark_through(gpu_mark_id::hzb_cull_end);
                                                                                       // THE SPLIT MARK, before the pass: the geometry lane keeps the interval it
                                                                                       // always had ("scene" = this mark -> scene_end); the visibility slot reads ~0
                                                                                       this->gpu_mark(gpu_mark_id::visibility_raster_end);
                                                                                       this->gpu_mark_through(gpu_mark_id::software_raster_end);
                                                                                       auto error = payload->record(7);
                                                                                       if (error != rhi::error::ok) {
                                                                                           return error;
                                                                                       }
                                                                                       this->gpu_mark(gpu_mark_id::scene_end);
                                                                                       this->gpu_mark(gpu_mark_id::rt_shadow_end);
                                                                                       return rhi::error::ok;
                                                                                   });
                if (!geometry_bound) {
                    return post_build_error(geometry_bound.error());
                }
            }
            // THE RAY-TRACED SHADOW LANE (RT stage 2b): recorded between the G-buffer producer and the lighting stage its
            // visibility feeds. The adapter declares the storage write from the ray-tracing stage plus the G-buffer
            // reads the tracer reconstructs its ray origins from, so the graph orders geometry -> trace -> lighting and
            // owns the image's two transitions; the pass's record_content keeps the launch alone.
            if (this->graph_rt_frame && rt_visibility) {
                pass::stage rt_stage{.name = "rt_shadow", .passes = this->rt_shadow_pass};
                this->prepare_stage(rt_stage);
                passes[18] = this->rt_shadow_pass[0];
                ios[18].push_block = host.push_block;
                if (!passes[18] || !host.resolve || !host.resolve(host.context, *passes[18], ios[18]) || ios[18].list != commands.get()) {
                    return rhi::error::not_ready;
                }
                auto rt_added = build.append([&](graph::builder& description) noexcept {
                    return graph::rt_shadow::append(description, {*rt_visibility, lighting_inputs.surface, *depth_ref});
                });
                if (!rt_added) {
                    return post_build_error(rt_added.error());
                }
                auto rt_bound = build.bind_pass<graph::rt_shadow::parameters>(
                    rt_added->pass, [payload](graph::rt_shadow::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(18); });
                if (!rt_bound) {
                    return post_build_error(rt_bound.error());
                }
            }
            if (!this->graph_debug_frame) {
                auto appended = build.append([&](graph::builder& description) noexcept { return graph::lighting::append(description, lighting_inputs); });
                if (!appended) {
                    return post_build_error(appended.error());
                }
                lit = *appended;
                pass::stage stage{.name = "deferred", .passes = this->deferred_pass};
                this->prepare_stage(stage);
                passes[6] = this->deferred_pass[0];
                ios[6].push_block = host.push_block;
                if (!passes[6] || !host.resolve || !host.resolve(host.context, *passes[6], ios[6]) ||
                    ios[6].list != commands.get() || ios[6].targets.size() != 1 || ios[6].targets[0].image_handle != scene_target_image) {
                    return rhi::error::not_ready;
                }
                auto bound = build.bind_pass<graph::lighting::parameters>(lit->pass,
                                                                          [this, payload](graph::lighting::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                                                                              this->gpu_mark_through(gpu_mark_id::software_merge_end);
                                                                              auto error = payload->record(6);
                                                                              if (error != rhi::error::ok) {
                                                                                  return error;
                                                                              }
                                                                              this->gpu_mark(gpu_mark_id::lighting_end);
                                                                              return rhi::error::ok;
                                                                          });
                if (!bound) {
                    return post_build_error(bound.error());
                }
            } else {
                // THE G-BUFFER DEBUG VIEW AS A GRAPH PASS (adoption stage 2J): it samples the stored targets and the
                // motion vectors and CLEARS the HDR target into one visible channel - the lane that runs INSTEAD of
                // the lighting stage on a debug frame. Its four input transitions and the HDR write are the graph's
                // now; the frame loop's own HDR transition for this view belonged to the removed legacy path.
                pass::stage debug_stage{.name = "gbuffer_debug", .passes = this->gbuffer_debug_stage};
                this->prepare_stage(debug_stage);
                passes[12] = this->gbuffer_debug_stage[0];
                ios[12].push_block = host.push_block;
                if (!passes[12] || !host.resolve || !host.resolve(host.context, *passes[12], ios[12]) || ios[12].list != commands.get() ||
                    ios[12].targets.size() != 1 || ios[12].targets[0].image_handle != scene_target_image) {
                    return rhi::error::not_ready;
                }
                auto debug_added = build.append([&](graph::builder& description) noexcept {
                    return graph::gbuffer_debug::append(description, {lighting_inputs.surface, *velocity_ref, scene_target});
                });
                if (!debug_added) {
                    return post_build_error(debug_added.error());
                }
                auto debug_bound = build.bind_pass<graph::gbuffer_debug::parameters>(
                    debug_added->pass, [payload](graph::gbuffer_debug::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(12); });
                if (!debug_bound) {
                    return post_build_error(debug_bound.error());
                }
            }
            // THE GAUSSIAN RASTER AS A GRAPH PASS (adoption stage 2O): the removed legacy tail ran it BEFORE transparency
            // (lighting -> gaussian -> transparent), so it is appended here. Its GPU compute chain keeps its own
            // buffer barriers inside the recorder - the snapshot's private buffers, no other pass touches them - and
            // the graph owns the pass-level boundary: the working image as a preserve write and the depth as a read.
            if (this->gaussian_active()) {
                pass::stage gaussian_stage{.name = "gaussian", .passes = this->gaussian_pass};
                this->prepare_stage(gaussian_stage);
                passes[17] = this->gaussian_pass[0];
                ios[17].push_block = host.push_block;
                if (!passes[17] || !host.resolve || !host.resolve(host.context, *passes[17], ios[17]) || ios[17].list != commands.get() ||
                    ios[17].targets.size() != 2 || ios[17].targets[0].image_handle != scene_target_image || ios[17].targets[1].image_handle != depth.get()) {
                    return rhi::error::not_ready;
                }
                auto gaussian_added = build.append([&](graph::builder& description) noexcept {
                    return graph::gaussian::append(description, {scene_target, *depth_ref});
                });
                if (!gaussian_added) {
                    return post_build_error(gaussian_added.error());
                }
                auto gaussian_bound = build.bind_pass<graph::gaussian::parameters>(
                    gaussian_added->pass, [this, payload](graph::gaussian::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                        // the pass's own GPU timing callback writes 7/8/9 INSIDE this record; the raster mark (10) is
                        // the zero-interval placeholder after it (or the whole 7..10 range when it did not record)
                        auto const recorded = payload->record(17);
                        this->gpu_mark_through(gpu_mark_id::gaussian_raster_end);
                        return recorded;
                    });
                if (!gaussian_bound) {
                    return post_build_error(gaussian_bound.error());
                }
            }
            // THE TRANSPARENT PASS AS A GRAPH PASS (adoption stage 2K): it blends the alpha geometry over the lit
            // working image and reads the opaque depth. The graph owns the two hand-off transitions the pass's
            // removed legacy record issued, and this declaration is what orders lighting -> transparent -> TAA.
            if (this->active_features().transparent) {
                pass::stage transparent_stage{.name = "transparent", .passes = this->transparent_pass};
                this->prepare_stage(transparent_stage);
                passes[13] = this->transparent_pass[0];
                ios[13].push_block = host.push_block;
                if (!passes[13] || !host.resolve || !host.resolve(host.context, *passes[13], ios[13]) || ios[13].list != commands.get() ||
                    ios[13].targets.size() < 2 || ios[13].targets[0].image_handle != scene_target_image || ios[13].targets[1].image_handle != depth.get()) {
                    return rhi::error::not_ready;
                }
                auto transparent_added = build.append([&](graph::builder& description) noexcept {
                    return graph::transparent::append(description, {scene_target, *depth_ref});
                });
                if (!transparent_added) {
                    return post_build_error(transparent_added.error());
                }
                auto transparent_bound = build.bind_pass<graph::transparent::parameters>(
                    transparent_added->pass, [this, payload](graph::transparent::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                        auto const recorded = payload->record(13);
                        this->gpu_mark_through(gpu_mark_id::transparent_end);
                        return recorded;
                    });
                if (!transparent_bound) {
                    return post_build_error(transparent_bound.error());
                }
            }
            // THE CHARACTER CHAIN (adoption stage 2L): the character-forward pass and the two screen rims compose over
            // the shaded frame in the old order (transparent -> character forward -> toon rim -> goo rim -> TAA).
            // All three keep the scene target as a PRESERVE write; the forward pass reads the depth as a depth test,
            // the rims declare the depth/albedo reads their shaders take through per-image heap slots.
            if (this->feature_active("character_forward")) {
                pass::stage character_stage{.name = "character_forward", .passes = this->character_forward_pass};
                this->prepare_stage(character_stage);
                passes[14] = this->character_forward_pass[0];
                ios[14].push_block = host.push_block;
                if (!passes[14] || !host.resolve || !host.resolve(host.context, *passes[14], ios[14]) || ios[14].list != commands.get() ||
                    ios[14].targets.size() < 2 || ios[14].targets[0].image_handle != scene_target_image || ios[14].targets[1].image_handle != depth.get()) {
                    return rhi::error::not_ready;
                }
                auto character_added = build.append([&](graph::builder& description) noexcept {
                    return graph::character_forward::append(description, {scene_target, *depth_ref});
                });
                if (!character_added) {
                    return post_build_error(character_added.error());
                }
                auto character_bound = build.bind_pass<graph::character_forward::parameters>(
                    character_added->pass, [payload](graph::character_forward::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(14); });
                if (!character_bound) {
                    return post_build_error(character_bound.error());
                }
            }
            if (this->feature_active("toon_screen_rim")) {
                pass::stage toon_stage{.name = "toon_screen_rim", .passes = this->toon_screen_rim_pass};
                this->prepare_stage(toon_stage);
                passes[15] = this->toon_screen_rim_pass[0];
                ios[15].push_block = host.push_block;
                if (!passes[15] || !host.resolve || !host.resolve(host.context, *passes[15], ios[15]) || ios[15].list != commands.get() ||
                    ios[15].targets.size() != 1 || ios[15].targets[0].image_handle != scene_target_image) {
                    return rhi::error::not_ready;
                }
                auto toon_added = build.append([&](graph::builder& description) noexcept {
                    return graph::toon_screen_rim::append(description, {scene_target, *depth_ref, lighting_inputs.surface[0]});
                });
                if (!toon_added) {
                    return post_build_error(toon_added.error());
                }
                auto toon_bound = build.bind_pass<graph::toon_screen_rim::parameters>(
                    toon_added->pass, [payload](graph::toon_screen_rim::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(15); });
                if (!toon_bound) {
                    return post_build_error(toon_bound.error());
                }
            }
            if (this->feature_active("goo_rim")) {
                pass::stage goo_stage{.name = "goo_rim", .passes = this->goo_rim_pass};
                this->prepare_stage(goo_stage);
                passes[16] = this->goo_rim_pass[0];
                ios[16].push_block = host.push_block;
                if (!passes[16] || !host.resolve || !host.resolve(host.context, *passes[16], ios[16]) || ios[16].list != commands.get() ||
                    ios[16].targets.size() != 1 || ios[16].targets[0].image_handle != scene_target_image) {
                    return rhi::error::not_ready;
                }
                auto goo_added = build.append([&](graph::builder& description) noexcept {
                    return graph::goo_rim::append(description, {scene_target, *depth_ref, lighting_inputs.surface[0]});
                });
                if (!goo_added) {
                    return post_build_error(goo_added.error());
                }
                auto goo_bound = build.bind_pass<graph::goo_rim::parameters>(
                    goo_added->pass, [payload](graph::goo_rim::parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(16); });
                if (!goo_bound) {
                    return post_build_error(goo_bound.error());
                }
            }
            // THE TAA RESOLVE AND ITS HISTORY COPY (adoption stage 2I): TWO graph passes from the one pass object.
            // The resolve turns the working image (which geometry and lighting just wrote) into this HDR image;
            // the copy moves that HDR image into the history. The graph owns the attachment transition, the
            // transfer round trip and the ordering between them - kept by the pass's removed legacy record.
            // (The legacy frames it alone recorded are gone with that path.)
            if (this->graph_taa_frame && taa_history) {
                pass::stage taa_stage{.name = "taa", .passes = this->taa_pass};
                this->prepare_stage(taa_stage);
                passes[10] = this->taa_pass[0];
                ios[10].push_block = host.push_block;
                if (!passes[10] || !host.resolve || !host.resolve(host.context, *passes[10], ios[10]) || ios[10].list != commands.get()) {
                    return rhi::error::not_ready;
                }
                passes[11] = this->taa_pass[0];
                ios[11].push_block = host.push_block;
                if (!host.resolve(host.context, *passes[11], ios[11]) || ios[11].list != commands.get()) {
                    return rhi::error::not_ready;
                }
                auto resolve_added = build.append([&](graph::builder& description) noexcept {
                    return graph::taa::append_resolve(description, {scene_target, *velocity_ref, *depth_ref, *taa_history, inputs.hdr});
                });
                if (!resolve_added) {
                    return post_build_error(resolve_added.error());
                }
                auto resolve_bound = build.bind_pass<graph::taa::resolve_parameters>(
                    resolve_added->pass, [payload](graph::taa::resolve_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(10); });
                if (!resolve_bound) {
                    return post_build_error(resolve_bound.error());
                }
                auto copy_added = build.append([&](graph::builder& description) noexcept {
                    return graph::taa::append_history_copy(description, {inputs.hdr, *taa_history});
                });
                if (!copy_added) {
                    return post_build_error(copy_added.error());
                }
                // THE COPY MUST BE LIVE, and that is not automatic: its only output is the history image, which no
                // pass in THIS frame reads - the value-root culling would drop the pass and the history would never
                // be written (measured: the graph path then behaved as if every frame were a first frame). Exporting
                // the history is what keeps the pass in the plan; the next frame's resolve is its consumer.
                if (auto exported = build.export_resource(*taa_history); !exported) {
                    return post_build_error(exported.error());
                }
                auto copy_bound = build.bind_pass<graph::taa::copy_parameters>(
                    copy_added->pass, [this, payload](graph::taa::copy_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                        auto const recorded = static_cast<pass::taa_pass*>(payload->passes[11])->record_copy(payload->ios[11]);
                        this->gpu_mark_through(gpu_mark_id::taa_end);
                        return recorded;
                    });
                if (!copy_bound) {
                    return post_build_error(copy_bound.error());
                }
            }
        }
        for (unsigned i = 0; i < 4; ++i) {
            passes[i] = this->bloom_pass[i];
        }
        passes[4] = this->post_composite_stage[0];
        passes[5] = settings.resolution == graph::post::resolve::fxaa ? this->fxaa_pass[0] : this->upscale_pass[0];
        unsigned count = settings.resolution == graph::post::resolve::none ? 5 : 6;
        for (unsigned i = 0; i < count; ++i) {
            ios[i].push_block = host.push_block;
            if (!passes[i] || !host.resolve || !host.resolve(host.context, *passes[i], ios[i]) ||
                ios[i].list != commands.get() || ios[i].targets.size() != 1) {
                return rhi::error::not_ready;
            }
            auto const expected = [&] {
                if (i < 4) {
                    return this->bloom_images[i][index].get();
                }
                if (i == 4 && ldr) {
                    return ldr.get();
                }
                return this->rhi_face().frames->frame_image();
            }();
            if (ios[i].targets[0].image_handle != expected) {
                return rhi::error::invalid_argument;
            }
            if (expected == this->rhi_face().frames->frame_image()) {
                ios[i].target_storage[0] = {.image_handle = frame.get(), .view_handle = view->get()};
                ios[i].targets = std::span<pass::resolved_binding const>{ios[i].target_storage.data(), 1};
            }
        }
        auto output = build.append([&](graph::builder& description) noexcept {
            return graph::post::append(description, inputs, {settings.resolution, settings.bloom_enabled});
        });
        if (!output) {
            return post_build_error(output.error());
        }
        for (unsigned i = 0; i < 4; ++i) {
            auto bound = settings.bloom_enabled
                             ? build.bind_pass<graph::post::bloom_parameters>(output->bloom[i], [payload, i](graph::post::bloom_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return payload->record(i); })
                             : build.bind_pass<graph::post::clear_parameters>(output->bloom[i], [payload, i](graph::post::clear_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept { return clear_post_target(payload->ios[i]); });
            if (!bound) {
                return post_build_error(bound.error());
            }
        }
        auto composite_bound = build.bind_pass<graph::post::composite_parameters>(output->composite,
                                                                                  [this, payload](graph::post::composite_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                                                                                      // the scene side is over here: fill any stage mark whose real emitter did
                                                                                      // not run (debug-view frames, no lighting, disabled optional stages)
                                                                                      this->gpu_mark_through(gpu_mark_id::main_end);
                                                                                      this->gpu_mark(gpu_mark_id::bloom_end);
                                                                                      return payload->record(4);
                                                                                  });
        if (!composite_bound) {
            return post_build_error(composite_bound.error());
        }
        if (output->resolve) {
            auto resolve_bound = build.bind_pass<graph::post::resolve_parameters>(*output->resolve,
                                                                                  [this, payload, upscale = settings.resolution == graph::post::resolve::upscale](graph::post::resolve_parameters const&, rhi::command_buffer&, graph::execution::record_context const&) noexcept {
                                                                                      this->gpu_mark(gpu_mark_id::composite_end);
                                                                                      if (upscale) {
                                                                                          this->gpu_mark(gpu_mark_id::fxaa_end);
                                                                                      }
                                                                                      return payload->record(5);
                                                                                  });
            if (!resolve_bound) {
                return post_build_error(resolve_bound.error());
            }
        }
        auto exported = build.export_resource(output->color);
        if (!exported) {
            return post_build_error(exported.error());
        }
        auto prepared = build.prepare();
        if (!prepared) {
            return post_build_error(prepared.error());
        }
        retained.push_back(prepared->retention());
        rhi::error error;
        if (residency_resolve_index) {
            graph::execution::prepared_graph prefix, suffix;
            error = prepared->partition_before(*residency_resolve_index, prefix, suffix);
            if (error == rhi::error::ok)
                error = prefix.record(*commands);
            if (error == rhi::error::ok)
                error = rhi::query_extension<rhi::frame_checkpoint>(rhi_face())->checkpoint_and_wait();
            if (error == rhi::error::ok)
                error = update_texture_residency(false);
            if (error == rhi::error::ok)
                error = suffix.record(*commands);
        } else {
            error = prepared->record(*commands);
        }
        if (error != rhi::error::ok) {
            return error;
        }
        // THE TAA HISTORY'S HAND-BACK (adoption stage 2I): the graph's copy leaves the history image in
        // transfer-destination, and the invariant every frame's import relies on is "the history is sampled outside
        // the graph" - so the frame that wrote it hands it back, as the removed legacy TAA record did for its own
        // frame. The graph cannot own a state that outlives its plan, which is why this stays the runtime's.
        if (this->graph_taa_frame && taa_history_image != nullptr) {
            auto const hand_back = commands->barrier(rhi::image_barrier{.resource = taa_history_image,
                                                                        .from = rhi::image_use::transfer_destination,
                                                                        .to = rhi::image_use::shader_read,
                                                                        .range = {}});
            if (hand_back != rhi::error::ok) {
                return hand_back;
            }
        }
        // THE TAA STAGE'S COLLECTED RESULT: the legacy tail collected it after its own TAA stage, and a written history
        // is what publishes this frame's UNJITTERED view-projection for the NEXT frame's reprojection. Measured:
        // without this the graph-recorded TAA frame differs (the resolve reprojects against a stale matrix).
        if (this->graph_taa_frame) {
            this->collect_stage("taa");
        }
        // THE GAUSSIAN HDR CAPTURE (moved here from the legacy scene tail when that tail went): a measurement
        // script asks for the pre-post HDR image of the screenshot frame (`DEREN_GAUSSIAN_HDR_CAPTURE` is the
        // path, see docs/gaussian_splatting.md). The image is copied out HERE because this is the point where the
        // frame's HDR exists and its layout is known: the graph just recorded it (the post chain reads it), and
        // this frame's recorder is the only one there is. The readback itself stays the screenshot path's
        // (runtime.readback.cppm), which is why only the copy and the buffer publication live here.
        if (this->screenshot_requested && std::getenv("DEREN_GAUSSIAN_HDR_CAPTURE")) {
            auto* hdr = this->hdr_images[this->current_image_index].get();
            auto const extent = this->render_extent();
            rhi::buffer_desc desc;
            desc.size = static_cast<std::uint64_t>(extent.width) * extent.height * 8u;
            desc.usage = rhi::buffer_usage::readback_coherent;
            this->gaussian_hdr_readback = rhi::unique_object<rhi::buffer>{this->rhi_face().resources->create_buffer(desc)};
            // THE PAIR IS THE LEGACY ONE ON PURPOSE: the contract's transition vocabulary has a
            // (color_attachment -> transfer_source) recipe and no (sampled-read -> transfer_source) one, because
            // nothing in the frame pipeline copies out of an image it just sampled. A copy after a sampled read
            // needs no ordering beyond the write flush that recipe carries, and the image is left in the state the
            // frame's next graph import discards (the HDR is re-imported with undefined contents).
            auto const from = rhi::image_use::color_attachment;
            if (!hdr || !this->gaussian_hdr_readback) {
                deren::utility::log("Gaussian HDR capture: FAIL allocation");
            } else if (auto const entry = commands->barrier(rhi::image_barrier{.resource = hdr, .from = from, .to = rhi::image_use::transfer_source});
                       entry != rhi::error::ok) {
                deren::utility::log("Gaussian HDR capture: FAIL entry barrier ({})", static_cast<std::uint32_t>(entry));
            } else {
                auto const copied = commands->copy_image_to_buffer(*this->gaussian_hdr_readback, *hdr, {.extent = extent});
                auto const restored = commands->barrier(rhi::image_barrier{.resource = hdr, .from = rhi::image_use::transfer_source, .to = from});
                rhi::buffer_barrier const host{.resource = this->gaussian_hdr_readback.get(), .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::host_read};
                auto const published = commands->barrier(rhi::barrier_group{.buffers = std::span{&host, 1}});
                if (copied != rhi::error::ok || restored != rhi::error::ok || published != rhi::error::ok) {
                    deren::utility::log("Gaussian HDR capture: FAIL copy or publication");
                } else {
                    this->gaussian_hdr_extent = extent;
                }
            }
        }
        // ---- THE DEPTH-PROBE CAPTURE (visibility-buffer root-cause measurement) ----
        // The shared G-buffer depth is the value the toon re-shading stage tests EQUAL against, so it is THE
        // datum the depth-mismatch investigation needs. Copied out on the same screenshot frame as the Gaussian
        // HDR capture, for the same reason: this is where the frame exists and its layout is known. The
        // readback itself stays the screenshot path (runtime.readback.cppm).
        if (this->screenshot_requested && std::getenv("DEREN_SOFTWARE_IMAGES_CAPTURE")) {
            this->software_probe_extent = {};
            auto const extent = this->render_extent();
            std::array<rhi::image*, 2> const images{
                this->software_depth_images[this->current_image_index].get(),
                this->software_id_images[this->current_image_index].get()};
            bool complete = this->software_raster_active_frame;
            for (std::size_t i = 0; complete && i < images.size(); ++i) {
                rhi::buffer_desc desc;
                desc.size = static_cast<std::uint64_t>(extent.width) * extent.height * 4u;
                desc.usage = rhi::buffer_usage::readback_coherent;
                this->software_probe_readbacks[i] = rhi::unique_object<rhi::buffer>{this->rhi_face().resources->create_buffer(desc)};
                auto* image = images[i];
                auto* destination = this->software_probe_readbacks[i].get();
                if (!image || !destination) {
                    complete = false;
                    break;
                }
                auto const entered = commands->barrier(rhi::image_barrier{.resource = image,
                                                                          .from = rhi::image_use::shader_write,
                                                                          .to = rhi::image_use::transfer_source});
                auto const copied = entered == rhi::error::ok
                                        ? commands->copy_image_to_buffer(*destination, *image, {.extent = extent})
                                        : entered;
                auto const restored = commands->barrier(rhi::image_barrier{.resource = image,
                                                                           .from = rhi::image_use::transfer_source,
                                                                           .to = rhi::image_use::shader_read});
                rhi::buffer_barrier const host{.resource = destination,
                                               .from = rhi::buffer_use::transfer_destination,
                                               .to = rhi::buffer_use::host_read};
                auto const published = commands->barrier(rhi::barrier_group{.buffers = std::span{&host, 1}});
                complete = copied == rhi::error::ok && restored == rhi::error::ok && published == rhi::error::ok;
            }
            this->software_route_probe_ready = false;
            this->software_route_probe_items.clear();
            if (complete && this->software_gpu_route_active_frame) {
                auto const slot = this->frame_ring().position();
                this->software_route_probe_slot = slot;
                this->software_route_probe_budget = this->software_visibility->route_metadata_words()[4];
                std::memcpy(this->software_route_probe_view.data(), &this->current_ubo.view, sizeof(float) * 16u);
                std::memcpy(this->software_route_probe_proj.data(), &this->current_ubo.proj, sizeof(float) * 16u);
                auto const* descriptions = static_cast<pass::hzb_item const*>(this->hzb_items_mapped) + slot * pass::hzb_cull_item_capacity;
                auto const* candidates = static_cast<meshlet const*>(this->meshlet_culled_mapped) + slot * meshlet_capacity;
                for (std::uint32_t i = 0; i < this->frame_visible.size(); ++i) {
                    auto const& description = descriptions[i];
                    if (description.source != pass::hzb_item_source_culled || !description.candidate_count)
                        continue;
                    if (description.meshlet_base > meshlet_capacity || description.candidate_count > meshlet_capacity - description.meshlet_base) {
                        complete = false;
                        break;
                    }
                    software_route_probe_item item;
                    item.item = i;
                    item.base = description.meshlet_base;
                    item.records.assign(candidates + item.base, candidates + item.base + description.candidate_count);
                    auto const* leaf = this->frame_visible[i];
                    std::memcpy(item.model.data(), &leaf->push.model, sizeof(float) * 16u);
                    item.double_sided = (this->material_flags_of(*leaf) & 8u) != 0u;
                    if (leaf->subtilis_geometry) {
                        auto const& source = leaf->subtilis_geometry->source();
                        item.positions.resize(source.vertex_count());
                        for (std::size_t v = 0; v < item.positions.size(); ++v)
                            std::memcpy(item.positions[v].data(), source.vertices().data() + v * source.vertex_stride(), sizeof(float) * 3u);
                        item.indices.assign(source.indices().begin(), source.indices().end());
                    }
                    this->software_route_probe_items.push_back(std::move(item));
                }
                std::array<rhi::buffer*, 3> sources{this->software_tiles_buffer.get(), this->hzb_results_buffer.get(), this->hzb_survivors_buffer.get()};
                std::array<std::uint64_t, 3> sizes{visibility::software::software_tiles_bytes(extent.width, extent.height),
                                                   pass::hzb_results_words_per_lane * 4ull, meshlet_capacity * sizeof(meshlet)};
                for (std::size_t i = 0; complete && i < sources.size(); ++i) {
                    rhi::buffer_desc desc;
                    desc.size = sizes[i];
                    desc.usage = rhi::buffer_usage::readback_coherent;
                    this->software_route_probe_readbacks[i] = rhi::unique_object<rhi::buffer>{this->rhi_face().resources->create_buffer(desc)};
                    auto* destination = this->software_route_probe_readbacks[i].get();
                    if (!sources[i] || !destination) {
                        complete = false;
                        break;
                    }
                    rhi::buffer_barrier enter{.resource = sources[i], .from = rhi::buffer_use::shader_read_write, .to = rhi::buffer_use::transfer_source};
                    auto entered = commands->barrier(rhi::barrier_group{.buffers = std::span{&enter, 1}});
                    auto copied = entered == rhi::error::ok ? commands->copy_buffer(*destination, *sources[i], sizes[i], slot * sizes[i], 0u) : entered;
                    rhi::buffer_barrier restore{.resource = sources[i], .from = rhi::buffer_use::transfer_source, .to = rhi::buffer_use::shader_read_write};
                    auto restored = commands->barrier(rhi::barrier_group{.buffers = std::span{&restore, 1}});
                    rhi::buffer_barrier host{.resource = destination, .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::host_read};
                    auto published = commands->barrier(rhi::barrier_group{.buffers = std::span{&host, 1}});
                    complete = copied == rhi::error::ok && restored == rhi::error::ok && published == rhi::error::ok;
                }
                this->software_route_probe_ready = complete;
            }
            if (complete)
                this->software_probe_extent = extent;
            else
                deren::utility::log("software image capture: FAIL inactive lane or readback recording");
        }
        if (this->screenshot_requested && std::getenv("DEREN_DEPTH_PROBE_CAPTURE")) {
            auto* const depth = this->gbuffer_depth_images[this->current_image_index].get();
            auto const extent = this->render_extent();
            rhi::buffer_desc desc;
            desc.size = static_cast<std::uint64_t>(extent.width) * extent.height * 4u;
            desc.usage = rhi::buffer_usage::readback_coherent;
            this->depth_probe_readback = rhi::unique_object<rhi::buffer>{this->rhi_face().resources->create_buffer(desc)};
            // THE PAIR IS THE LEGACY ONE, deliberately (same reasoning as the Gaussian HDR copy above): unified
            // image layouts make every state GENERAL, so this pair is a synchronization recipe, and the depth is
            // re-imported with undefined contents by the next frame.
            auto const from = rhi::image_use::color_attachment;
            if (!depth || !this->depth_probe_readback) {
                deren::utility::log("depth probe capture: FAIL allocation");
            } else if (auto const entry = commands->barrier(rhi::image_barrier{.resource = depth, .from = from, .to = rhi::image_use::transfer_source});
                       entry != rhi::error::ok) {
                deren::utility::log("depth probe capture: FAIL entry barrier ({})", static_cast<std::uint32_t>(entry));
            } else {
                auto const copied = commands->copy_image_to_buffer(*this->depth_probe_readback, *depth, {.extent = extent});
                auto const restored = commands->barrier(rhi::image_barrier{.resource = depth, .from = rhi::image_use::transfer_source, .to = from});
                rhi::buffer_barrier const host{.resource = this->depth_probe_readback.get(), .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::host_read};
                auto const published = commands->barrier(rhi::barrier_group{.buffers = std::span{&host, 1}});
                if (copied != rhi::error::ok || restored != rhi::error::ok || published != rhi::error::ok) {
                    deren::utility::log("depth probe capture: FAIL copy or publication");
                } else {
                    this->depth_probe_extent = extent;
                }
            }
        }
        // ---- THE HZB PROBE CAPTURE (M3 phase A2, spec §10.2) ----
        // The pyramid lane and the header lane of the frame that JUST built them are copied out here, for the same
        // reason the depth probe copies here: the graph has recorded, the buffers' contents are final, and this is
        // the only recorder there is. The copy is NOT a graph node (no pass reads these tables this phase), so it
        // needs its own buffer role pair: compute-write -> transfer-source, then transfer-destination -> host-read
        // on the two readback slots (the depth probe's image pair, one resource class over).
        //
        // THE METADATA IS THE POINT (spec §4.1/C21): the header's view/proj are a GPU snapshot of the camera lane the
        // build read, and the only way to prove "the header is THAT frame's camera" is to dump the host's own
        // snapshot of the same frame (current_ubo, written in pace_and_acquire before this frame recorded) beside
        // it. scripts/measure/hzb_verify.py then compares the two BIT FOR BIT, and independently rebuilds the whole
        // pyramid from the same frame's DEREN_DEPTH_PROBE_CAPTURE dump.
        if (this->screenshot_requested && std::getenv("DEREN_HZB_PROBE_CAPTURE") && this->hzb_resources_ready) {
            auto* const levels = this->hzb_levels_buffer.get();
            auto* const header = this->hzb_header_buffer.get();
            auto const extent = this->render_extent();
            std::uint32_t const slot = this->frame_ring().position();
            std::uint64_t const levels_bytes = deren::engine::visibility::hzb::pyramid_bytes(extent.width, extent.height);
            rhi::buffer_desc levels_desc;
            levels_desc.size = levels_bytes;
            levels_desc.usage = rhi::buffer_usage::readback_coherent;
            this->hzb_probe_pyramid_readback = rhi::unique_object<rhi::buffer>{this->rhi_face().resources->create_buffer(levels_desc)};
            rhi::buffer_desc header_desc;
            header_desc.size = pass::hzb_header_byte_capacity;
            header_desc.usage = rhi::buffer_usage::readback_coherent;
            this->hzb_probe_header_readback = rhi::unique_object<rhi::buffer>{this->rhi_face().resources->create_buffer(header_desc)};
            if (!levels || !header || !this->hzb_probe_pyramid_readback || !this->hzb_probe_header_readback) {
                deren::utility::log("hzb probe capture: FAIL allocation");
            } else if (!this->hzb_recorded_this_frame) {
                // The build lane is pruned on a frame that is not a visibility frame (or whose gate/resources are
                // not ready): the buffers hold a PREVIOUS frame's pyramid and its header, so a dump here would
                // pass the script's magic/extent check and silently describe the wrong frame. Refuse instead.
                deren::utility::log("hzb probe capture: skipped - the HZB build lane did not record on this screenshot frame");
            } else {
                std::array<rhi::buffer_barrier, 2> const to_source{{{.resource = levels, .from = rhi::buffer_use::shader_write, .to = rhi::buffer_use::transfer_source},
                                                                    {.resource = header, .from = rhi::buffer_use::shader_write, .to = rhi::buffer_use::transfer_source}}};
                auto const entered = commands->barrier(rhi::barrier_group{.buffers = std::span<rhi::buffer_barrier const>(to_source)});
                // THE COPY IS THE FRAME SLOT'S LANE, not the whole buffer: the tables are ONE descriptor with an
                // internal per-frame lane (§5.3), and the source offset is that lane's byte start.
                auto const levels_copied = entered == rhi::error::ok
                                               ? commands->copy_buffer(*this->hzb_probe_pyramid_readback, *levels, levels_bytes, static_cast<std::uint64_t>(slot) * levels_bytes, 0u)
                                               : entered;
                auto const header_copied = entered == rhi::error::ok
                                               ? commands->copy_buffer(*this->hzb_probe_header_readback, *header, pass::hzb_header_byte_capacity,
                                                                       static_cast<std::uint64_t>(slot) * pass::hzb_header_byte_capacity, 0u)
                                               : entered;
                std::array<rhi::buffer_barrier, 2> const restored{{{.resource = levels, .from = rhi::buffer_use::transfer_source, .to = rhi::buffer_use::shader_write},
                                                                   {.resource = header, .from = rhi::buffer_use::transfer_source, .to = rhi::buffer_use::shader_write}}};
                auto const left = commands->barrier(rhi::barrier_group{.buffers = std::span<rhi::buffer_barrier const>(restored)});
                std::array<rhi::buffer_barrier, 2> const host_reads{{{.resource = this->hzb_probe_pyramid_readback.get(), .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::host_read},
                                                                     {.resource = this->hzb_probe_header_readback.get(), .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::host_read}}};
                auto const published = commands->barrier(rhi::barrier_group{.buffers = std::span<rhi::buffer_barrier const>(host_reads)});
                if (levels_copied != rhi::error::ok || header_copied != rhi::error::ok || left != rhi::error::ok || published != rhi::error::ok) {
                    deren::utility::log("hzb probe capture: FAIL copy or publication");
                } else {
                    this->hzb_probe_extent = extent;
                    this->hzb_probe_words = deren::engine::visibility::hzb::pyramid_words(extent.width, extent.height);
                    this->hzb_probe_levels = deren::engine::visibility::hzb::level_count(extent.width, extent.height);
                    this->hzb_probe_frame_slot = slot;
                    std::memcpy(this->hzb_probe_view.data(), &this->current_ubo.view[0][0], sizeof(float) * 16u);
                    std::memcpy(this->hzb_probe_proj.data(), &this->current_ubo.proj[0][0], sizeof(float) * 16u);
                    this->hzb_probe_camera_pos = {this->current_ubo.camera_pos.x, this->current_ubo.camera_pos.y, this->current_ubo.camera_pos.z};
                    deren::utility::log("hzb probe capture: pyramid lane {} ({} word(s)) + header lane {} dumped for {}x{}, {} level(s); the frame camera is in the metadata",
                                        slot,
                                        this->hzb_probe_words,
                                        slot,
                                        extent.width,
                                        extent.height,
                                        this->hzb_probe_levels);
                }
            }
        }
        if (this->main_graph_frame && !this->main_graph_reported) {
            deren::utility::log("main graph: scene geometry -> GBuffer handoff -> deferred LOAD -> post recorded in one graph");
            this->main_graph_reported = true;
        }
        if (settings.resolution == graph::post::resolve::none) {
            this->gpu_mark(gpu_mark_id::composite_end);
        }
        if (settings.resolution != graph::post::resolve::upscale) {
            this->gpu_mark(gpu_mark_id::fxaa_end);
        }
        this->bloom_graph_pending = true;
        this->post_graph_drew_frame = true;
        return rhi::error::ok;
    }
    rhi::error runtime::finish_post_graph() {
        auto plan = graph::post::compile_finish(graph::execution::identify(this->post_graph_output), this->create_options.native_window != nullptr);
        if (!plan) {
            return rhi::error::invalid_argument;
        }
        std::array<graph::execution::resource_binding, 1> binding{this->post_graph_output};
        auto prepared = graph::execution::prepared_graph::prepare(this->core_owner, std::move(*plan), binding);
        if (!prepared) {
            return prepared.error().code;
        }
        this->frame_graph_retention[this->cluster_retention_slot].push_back(prepared->retention());
        return prepared->record(*this->frame_command_buffer(), [](graph::execution::record_context const&) noexcept { return rhi::error::ok; });
    }
} // namespace deren::engine
