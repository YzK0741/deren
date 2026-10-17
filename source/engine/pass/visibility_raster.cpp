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
                                                       /*depth_test_enabled=*/true,
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
        // THE SCOPE: the id target CLEARS to zero (0 = "no geometry"), the depth CLEARS to the far plane - this
        // pass owns both while it runs, and the geometry pass that follows in the capture-only step clears the
        // shared depth again, which is exactly why the frame cannot move yet.
        std::array<rhi::color_attachment, 1> const colors = {
            rhi::color_attachment{.view = io.targets[0].view_handle, .load = rhi::load_op::clear, .store = rhi::store_op::store, .clear = {0.0f, 0.0f, 0.0f, 0.0f}},
        };
        rhi::depth_attachment const depth = {.view = io.targets[1].view_handle,
                                             .load = rhi::load_op::clear,
                                             .store = rhi::store_op::store,
                                             .read_only = false,
                                             .has_stencil = false,
                                             .clear_depth = 1.0f,
                                             .clear_stencil = 0};
        rhi::rendering_info const rendering_info{
            .struct_size = sizeof(rhi::rendering_info),
            .area = {.offset_x = 0, .offset_y = 0, .width = this->pass_frame.extent.width, .height = this->pass_frame.extent.height},
            .layer_count = 1,
            .colors = std::span<rhi::color_attachment const>(colors.data(), colors.size()),
            .depth = depth,
            .has_depth = true,
            // straight into the primary: the per-item dispatches are ordinary commands, not secondaries
            .secondary_contents = false,
        };
        if (rhi::error const begun = io.list->begin_rendering(rendering_info); begun != rhi::error::ok) {
            return begun;
        }
        // BIND + RE-EMIT the dynamic state the handle stores, then THE VIEWPORT IS THE PASS'S: this pipeline is
        // the pass's own and therefore outside the runtime's registry-wide `update_pass_geometry` resync, so the
        // declaration asks the record step for the viewport and this pass sets it itself.
        this->raster_pipeline->begin_pipeline(*io.list);
        io.list->set_viewport(rhi::viewport{.x = 0.0f,
                                            .y = 0.0f,
                                            .width = static_cast<float>(this->pass_frame.extent.width),
                                            .height = static_cast<float>(this->pass_frame.extent.height),
                                            .min_depth = 0.0f,
                                            .max_depth = 1.0f});
        io.list->set_scissor(rhi::rect{.offset_x = 0, .offset_y = 0, .width = this->pass_frame.extent.width, .height = this->pass_frame.extent.height});
        std::uint32_t recorded = 0;
        bool const ok = this->pass_frame.record_items(this->pass_frame.owner, *io.list, recorded);
        io.list->end_rendering();
        return ok ? rhi::error::ok : rhi::error::invalid_argument;
    }
} // namespace deren::engine::pass
