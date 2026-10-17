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
import deren.engine.pass;
import deren.engine.pass.output_targets;
import deren.promise.rhi;
namespace deren::engine {
    namespace graph = deren::engine::render_graph;
    namespace rhi = deren::promise::rhi;
    namespace {
        struct post_recording_payload {
            pass::pass_host host{};
            std::array<pass::resolved_io, 8> ios;
            std::array<pass::frame_pass*, 8> passes{};
            rhi::error record(unsigned lane) const noexcept {
                if (host.apply_behaviour)
                    host.apply_behaviour(host.context, *passes[lane], ios[lane]);
                return passes[lane]->record_content(ios[lane]);
            }
        };
        rhi::error post_build_error(graph::execution::build_error const& error) noexcept {
            return error.execution ? error.execution->code : rhi::error::invalid_argument;
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
            lighting_inputs.color = inputs.hdr;
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
            // CPU writes were published for the completed slot before submission. These
            // imports describe GPU reads, not fictitious GPU upload writes.
            auto const slot = this->frame_ring().position();
            if (slot >= this->camera_buffers.size() || slot >= this->light_buffers.size() ||
                slot >= this->motion_buffers.size() || slot >= this->skin_buffers.size() ||
                slot >= this->skin_buffers_previous.size() || slot >= this->morph_buffers.size())
                return rhi::error::not_ready;
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
            lighting_inputs.shared = graph::lighting::shared_inputs{shared_refs[0], shared_refs[7]};
            auto geometry = build.append([&](graph::builder& description) noexcept {
                return graph::geometry::append(description, {inputs.hdr, lighting_inputs.surface, *velocity_ref, *depth_ref, shared_geometry});
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
                                velocity.get(), input.get(), depth.get()};
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
            auto appended = build.append([&](graph::builder& description) noexcept { return graph::lighting::append(description, lighting_inputs); });
            if (!appended)
                return post_build_error(appended.error());
            lit = *appended;
            pass::stage stage{.name = "deferred", .passes = this->deferred_pass, .marks = false};
            this->prepare_stage(stage, true);
            passes[6] = this->deferred_pass[0];
            ios[6].push_block = host.push_block;
            if (!passes[6] || !host.resolve || !host.resolve(host.context, *passes[6], ios[6]) ||
                ios[6].list != commands.get() || ios[6].targets.size() != 1 || ios[6].targets[0].image_handle != input.get())
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
