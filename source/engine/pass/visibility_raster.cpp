/**
 * @file source/engine/pass/visibility_raster.cpp
 * @brief the visibility raster pass's pipeline and recording (see the header's WHY THIS PASS EXISTS).
 */
module;

#include <array>
#include <cstdint>
#include <span>
#include <utility>

module deren.engine.pass.visibility_raster;

import deren.utility.shared;
import deren.utility; // deren::utility::log
import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pipelines;
import deren.engine.render_layout;

namespace deren::engine::pass {
    namespace {
        constexpr std::string_view mesh_shader_name = "visibility.mesh.spv";
        constexpr std::string_view fragment_shader_name = "visibility.frag.spv";
    } // namespace

    void visibility_raster_pass::create(pass_context const& context) {
        if (context.face == nullptr) {
            return;
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->raster_pipeline.reset(); // a new device generation: the old handle belonged to the old device
        }
        this->built_against = context.face;
        if (this->raster_pipeline.has_value()) {
            return; // already built for this device
        }
        std::span<std::uint8_t const> const mesh_spirv = context.shader != nullptr ? context.shader(context.owner, mesh_shader_name) : std::span<std::uint8_t const>{};
        std::span<std::uint8_t const> const fragment_spirv = context.shader != nullptr ? context.shader(context.owner, fragment_shader_name) : std::span<std::uint8_t const>{};
        if (mesh_spirv.empty() || fragment_spirv.empty()) {
            deren::utility::log("visibility raster disabled: the owner has no {} or {}", mesh_shader_name, fragment_shader_name);
            return;
        }
        // THE TARGET SHAPE IS THE DECLARATION'S: one R32_UINT colour target (the visibility buffer), the shared
        // depth image, no blending (integer targets cannot blend), depth test on and LESS_OR_EQUAL - the same
        // comparison the G-buffer pass uses, so an equal-depth tie keeps the same triangle the old path keeps.
        std::array<rhi::image_format, 1> const color_formats = {deren::engine::render_layout::visibility_format};
        auto built = pipelines::make_graphics_pipeline(*context.face,
                                                       std::span<rhi::image_format const>(color_formats),
                                                       rhi::image_format::depth,
                                                       mesh_spirv,
                                                       fragment_spirv,
                                                       1u,
                                                       /*depth_test=*/true,
                                                       0.0f,
                                                       0.0f,
                                                       0.0f,
                                                       std::span<rhi::blend_mode const>{},
                                                       rhi::shader_stage::mesh,
                                                       rhi::depth_compare::less_or_equal,
                                                       "visibility raster");
        if (!built) {
            deren::utility::log("visibility raster disabled: {}", built.error());
            return;
        }
        this->raster_pipeline = std::move(*built);
        deren::utility::log("SUCCESS: visibility raster pipeline created (mesh stage, one r32_uint target, depth-tested, no blending)");
    }

    bool visibility_raster_pass::bind_segment(void* const owner, rhi::mergeable_command_buffer_ref const& segment) noexcept {
        return owner != nullptr && segment && static_cast<visibility_raster_pass*>(owner)->bind_segment_impl(segment);
    }

    bool visibility_raster_pass::bind_segment_impl(rhi::mergeable_command_buffer_ref const& segment) noexcept {
        if (!this->raster_pipeline.has_value() || !segment) {
            return false;
        }
        rhi::image_format const color_format = deren::engine::render_layout::visibility_format;
        rhi::command_buffer_inheritance_info inheritance{};
        inheritance.color_format_count = 1;
        inheritance.color_formats = &color_format;
        inheritance.depth_format = rhi::image_format::depth;
        inheritance.samples = 1;
        rhi::command_buffer_begin_info begin{};
        begin.usage = rhi::to_bits(rhi::command_buffer_usage::render_pass_continue);
        begin.next = &inheritance.header;
        if (segment->begin_recording(begin) != rhi::error::ok) {
            return false;
        }
        this->raster_pipeline->viewport = rhi::viewport{.x = 0.0f,
                                                        .y = 0.0f,
                                                        .width = static_cast<float>(this->pass_frame.extent.width),
                                                        .height = static_cast<float>(this->pass_frame.extent.height),
                                                        .min_depth = 0.0f,
                                                        .max_depth = 1.0f};
        this->raster_pipeline->scissor = rhi::rect{.offset_x = 0, .offset_y = 0, .width = this->pass_frame.extent.width, .height = this->pass_frame.extent.height};
        this->raster_pipeline->begin_pipeline(*segment);
        segment->set_cull_mode(rhi::cull_mode::back);
        segment->set_depth_write(true);
        segment->set_viewport(this->raster_pipeline->viewport);
        segment->set_scissor(this->raster_pipeline->scissor);
        return true;
    }

    rhi::error visibility_raster_pass::record_content(resolved_io const& io) noexcept {
        if (!this->raster_pipeline.has_value() || this->pass_frame.record_items == nullptr || io.targets.size() != visibility_raster_io.targets.size() ||
            io.list == nullptr || io.extent.width == 0 || io.extent.height == 0 || this->pass_frame.extent.width == 0 || this->pass_frame.extent.height == 0) {
            return rhi::error::invalid_argument;
        }
        for (auto const& target : io.targets) {
            if (target.view_handle == nullptr) {
                return rhi::error::invalid_argument;
            }
        }
        // THE SCOPE: the id target CLEARS to zero (0 = "no geometry"), the shared G-buffer depth CLEARS to the far
        // plane - the exact pair the geometry pass clears when IT owns the frame. This pass replaces the geometry lane
        // on an admitted frame (v0.5), so these clears ARE the frame's, not a private copy: the lighting and toon
        // stages read the depth written here.
        std::array<rhi::color_attachment, 1> const colors = {
            rhi::color_attachment{.view = io.targets[0].view_handle, .load = this->load_existing ? rhi::load_op::load : rhi::load_op::clear, .store = rhi::store_op::store, .clear = {0.0f, 0.0f, 0.0f, 0.0f}},
        };
        rhi::depth_attachment const depth = {.view = io.targets[1].view_handle,
                                             .load = this->load_existing ? rhi::load_op::load : rhi::load_op::clear,
                                             .store = rhi::store_op::store,
                                             .read_only = false,
                                             .has_stencil = false,
                                             .clear_depth = 1.0f,
                                             .clear_stencil = 0};
        this->pass_frame.bind_segment = &visibility_raster_pass::bind_segment;
        this->pass_frame.segment_owner = this;
        bool const parallel = !this->pass_frame.segments.empty() && this->pass_frame.bind_segment != nullptr;
        rhi::rendering_info const rendering_info{
            .struct_size = sizeof(rhi::rendering_info),
            .area = {.offset_x = 0, .offset_y = 0, .width = this->pass_frame.extent.width, .height = this->pass_frame.extent.height},
            .layer_count = 1,
            .colors = std::span<rhi::color_attachment const>(colors.data(), colors.size()),
            .depth = depth,
            .has_depth = true,
            // Parallel recording produces mergeable segments; the serial path records straight into the primary.
            .secondary_contents = parallel,
        };
        if (rhi::error const begun = io.list->begin_rendering(rendering_info); begun != rhi::error::ok) {
            return begun;
        }
        // BIND + RE-EMIT the dynamic state the handle stores. THE STORED VIEWPORT IS WRITTEN FIRST, because
        // `begin_pipeline` re-emits the handle's own state and a pipeline this pass owns starts with a {0,0,0,0}
        // viewport - recording that is a validation error (VUID-VkViewport-width-01770), which is how the first
        // run that actually executed this pass found it. After this assignment the bind and the explicit sets
        // below name the same numbers, for the same "the record step cannot know this pass's extent" reason the
        // declaration's `resync_viewport` documents. (This pipeline is the pass's own, so writing its state is
        // the pass's own business - the runtime's registry-wide resync never sees it.)
        this->raster_pipeline->viewport = rhi::viewport{.x = 0.0f,
                                                        .y = 0.0f,
                                                        .width = static_cast<float>(this->pass_frame.extent.width),
                                                        .height = static_cast<float>(this->pass_frame.extent.height),
                                                        .min_depth = 0.0f,
                                                        .max_depth = 1.0f};
        this->raster_pipeline->scissor = rhi::rect{.offset_x = 0, .offset_y = 0, .width = this->pass_frame.extent.width, .height = this->pass_frame.extent.height};
        this->raster_pipeline->begin_pipeline(*io.list);
        // THE PIPELINE'S TWO DYNAMIC RASTER STATES: `make_graphics_pipeline` creates EVERY graphics pipeline with
        // VK_DYNAMIC_STATE_CULL_MODE and VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE (the scene pass sets them per draw
        // because a single pipeline serves single- and double-sided materials), so a pass that binds its own
        // pipeline must set them itself: BACK-face culling - the cull the G-buffer pass applies to a single-sided
        // material - and depth WRITE on, because this pass is the depth producer. MEASURED: without the two calls
        // the mesh dispatches are two validation errors (07840 / 07844) even though the picture would be right.
        io.list->set_cull_mode(rhi::cull_mode::back);
        io.list->set_depth_write(true);
        io.list->set_viewport(this->raster_pipeline->viewport);
        io.list->set_scissor(this->raster_pipeline->scissor);
        std::uint32_t recorded = 0;
        bool const ok = this->pass_frame.record_items(this->pass_frame.owner, *io.list, this->pass_frame.segments,
                                                      this->pass_frame.bind_segment, this->pass_frame.segment_owner, recorded);
        io.list->end_rendering();
        // THE LANE'S OWN WITNESS (visibility-buffer v0.4b): one line, once per run, saying that the item recorder
        // actually ran and how many mesh-task dispatches it issued. Until this existed the lane's only evidence was
        // the absence of a validation error, which cannot distinguish "the resolve consumed the ids" from "the graph
        // never recorded this pass"; the depth-mismatch counter in visibility_resolve.slang is the other half (it
        // only has a value if the resolve read what this pass wrote).
        if (ok && recorded > 0u) {
            static bool recorded_logged = false;
            if (!recorded_logged) {
                recorded_logged = true;
                deren::utility::log("visibility raster: recorded {} item(s) - the lane dispatched its meshlets into the packed id and the frame's shared depth", recorded);
            }
        }
        return ok ? rhi::error::ok : rhi::error::invalid_argument;
    }
} // namespace deren::engine::pass
