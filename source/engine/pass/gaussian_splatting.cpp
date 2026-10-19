module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
module deren.engine.pass.gaussian_splatting;
import deren.utility.shared;
namespace deren::engine::pass {
    namespace rhi = deren::promise::rhi;
    namespace {
        deren::utility::vessel::array<std::string_view, 1> const pipeline_names = {"gaussian"};
        deren::engine::pass::behaviour const raster_behaviour{.kind = behaviour_kind::instanced, .pipelines = pipeline_names};
        render_resource::pass_io const raster_io{.name = "gaussian", .targets = render_resource::transparent_targets, .push = std::nullopt};
    } // namespace
    render_resource::pass_io const& gaussian_splatting_pass::io() const noexcept {
        return raster_io;
    }
    deren::engine::pass::behaviour const& gaussian_splatting_pass::behaviour() const noexcept {
        return raster_behaviour;
    }
    void gaussian_splatting_pass::create(pass_context const& context) {
        pipeline.reset();
        for (auto& item : compute) {
            item.reset();
        }
        if (context.face == nullptr || context.shader == nullptr || color_format == rhi::image_format::unknown || context.depth_format == rhi::image_format::unknown) {
            return;
        }
        auto const vertex = context.shader(context.owner, "gaussian_raster.vert.spv");
        auto const fragment = context.shader(context.owner, "gaussian_raster.frag.spv");
        if (vertex.empty() || fragment.empty() || vertex.size() % 4 != 0 || fragment.size() % 4 != 0) {
            return;
        }
        deren::utility::vessel::array<rhi::image_format, 1> const colors = {color_format};
        deren::utility::vessel::array<rhi::blend_mode, 1> const blend = {rhi::blend_mode::alpha};
        rhi::pipeline_desc desc;
        desc.color_formats = colors;
        desc.depth_format = context.depth_format;
        desc.vertex_code = std::as_bytes(vertex);
        desc.fragment_code = std::as_bytes(fragment);
        desc.first_stage = rhi::shader_stage::vertex;
        desc.sample_count = 1;
        desc.depth_test = true;
        desc.compare = rhi::depth_compare::less_or_equal;
        desc.blend_modes = blend;
        desc.debug_name = "gaussian raster";
        pipeline = rhi::object_ptr<rhi::pipeline>{context.face->create_pipeline(desc)};
        if (rhi::query_extension<rhi::gpu_sorting>(*context.face)) {
            deren::utility::vessel::array<std::string_view, 3> const names = {"gaussian_reset.comp.spv", "gaussian_prepare.comp.spv", "gaussian_finalize.comp.spv"};
            for (std::size_t i = 0; i < names.size(); ++i) {
                auto const code = context.shader(context.owner, names[i]);
                if (code.empty() || code.size() % 4u != 0u) {
                    return;
                }
                rhi::pipeline_desc compute_desc;
                compute_desc.first_stage = rhi::shader_stage::compute;
                compute_desc.compute_code = std::as_bytes(code);
                compute_desc.debug_name = names[i].data();
                compute[i] = rhi::object_ptr<rhi::pipeline>{context.face->create_pipeline(compute_desc)};
                if (!compute[i]) {
                    return;
                }
            }
        }
    }

    void gaussian_splatting_pass::record_owned(resolved_io const& io) {
        static_cast<void>(this->record_impl(io, true));
    }

    rhi::error gaussian_splatting_pass::record_content(resolved_io const& io) noexcept {
        return this->record_impl(io, false);
    }

    rhi::error gaussian_splatting_pass::record_impl(resolved_io const& io, bool const own_barriers) noexcept {
        struct timing_scope {
            gaussian_splatting::gpu_frame_timing points;
            bool delegated = false;
            ~timing_scope() {
                if (!delegated && points.mark)
                    for (std::uint32_t phase = 0; phase < 3u; ++phase)
                        points.mark(points.owner, phase);
            }
        } marks{timing};
        record_error = rhi::error::ok;
        if ((!frame && !gpu_snapshot) || (gpu_snapshot ? gpu_snapshot->candidate_count() : frame->params().visible_count) == 0) {
            return rhi::error::ok; // nothing to draw is not a failure: the stage records nothing without a cloud
        }
        auto const& params = gpu_snapshot ? gpu_snapshot->params() : frame->params();
        if (!pipeline || io.list == nullptr || io.push_block.push == nullptr || io.targets.size() != 2 ||
            !io.targets[0].image_handle || !io.targets[0].view_handle || !io.targets[1].image_handle || !io.targets[1].view_handle ||
            static_cast<float>(io.extent.width) != params.viewport_width || static_cast<float>(io.extent.height) != params.viewport_height ||
            params.instances_address == 0 || params.references_address == 0) {
            record_error = rhi::error::invalid_argument;
            return record_error;
        }
        // THE GPU COMPUTE CHAIN STAYS INSIDE THIS RECORDER, and that is the model's boundary rather than a gap: its
        // three dispatches (prepare/sort/finalize) write and read the frame snapshot's OWN buffers (no other pass
        // touches them) and the snapshot emits their barriers itself, so the pass-level boundary the graph needs is
        // just the raster's target write and depth read.
        if (gpu_snapshot) {
            marks.delegated = true;
            record_error = gpu_snapshot->record_preparation(*io.list, {compute[0].get(), compute[1].get(), compute[2].get()}, io.push_block, timing);
            if (record_error != rhi::error::ok) {
                return record_error;
            }
        }
        record_error = io.list->bind_pipeline(*pipeline);
        if (record_error != rhi::error::ok) {
            return record_error;
        }
        if (!io.push_block(*io.list, push_bytes(params))) {
            record_error = rhi::error::unsupported;
            return record_error;
        }
        // THE IMAGE HAND-OFFS THE REMOVED LEGACY FRAME OWNED: the depth comes back from a sampler for the raster's depth
        // test, the target's last store is published for the LOAD, and the depth is handed back for the next
        // sampler. A graph-recorded frame declares the two accesses instead and the graph emits all three - so its
        // recorder calls record_content() and the two paths never run together.
        deren::utility::vessel::array<rhi::image_barrier, 2> const barriers = {
            rhi::image_barrier{.resource = io.targets[1].image_handle, .from = rhi::image_use::shader_read, .to = rhi::image_use::depth_attachment},
            rhi::image_barrier{.resource = io.targets[0].image_handle, .from = rhi::image_use::color_attachment, .to = rhi::image_use::color_attachment},
        };
        if (own_barriers) {
            record_error = io.list->barrier(rhi::barrier_group{.images = barriers});
            if (record_error != rhi::error::ok) {
                return record_error;
            }
        }
        rhi::image_barrier const restore{.resource = io.targets[1].image_handle, .from = rhi::image_use::depth_attachment, .to = rhi::image_use::shader_read};
        deren::utility::vessel::array<rhi::color_attachment, 1> const colors = {rhi::color_attachment{.view = io.targets[0].view_handle, .load = rhi::load_op::load, .store = rhi::store_op::store}};
        rhi::rendering_info scope;
        scope.area = {.width = io.extent.width, .height = io.extent.height};
        scope.colors = colors;
        scope.layer_count = 1;
        scope.depth = {.view = io.targets[1].view_handle, .load = rhi::load_op::load, .store = rhi::store_op::store};
        scope.has_depth = true;
        record_error = io.list->begin_rendering(scope);
        if (record_error != rhi::error::ok) {
            if (own_barriers) {
                auto const recovered = io.list->barrier(restore);
                if (recovered != rhi::error::ok) {
                    record_error = recovered;
                }
            }
            return record_error;
        }
        io.list->set_viewport({.width = params.viewport_width, .height = params.viewport_height, .min_depth = 0, .max_depth = 1});
        io.list->set_scissor(scope.area);
        io.list->set_cull_mode(rhi::cull_mode::none);
        io.list->set_depth_write(false);
        if (gpu_snapshot) {
            record_error = io.list->draw_indirect(*gpu_snapshot->draw_buffer(), 0u, 1u, sizeof(rhi::draw_indirect_command));
        } else {
            io.list->draw(6, params.visible_count, 0, 0);
        }
        io.list->end_rendering();
        if (own_barriers) {
            auto const restored = io.list->barrier(restore);
            if (restored != rhi::error::ok) {
                record_error = restored;
            }
        }
        return record_error;
    }
} // namespace deren::engine::pass
