/**
 * @file source/engine/pass/software_merge.cpp
 * @brief the software merge pass's depth-only pipeline and fullscreen depth write (see the header's WHY THIS PASS).
 */
module;

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

module deren.engine.pass.software_merge;

import deren.utility.shared;
import deren.utility; // deren::utility::log
import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pipelines;

namespace deren::engine::pass {
    void software_merge_pass::create(pass_context const& context) {
        if (context.face == nullptr) {
            return;
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->merge_pipeline.reset(); // a new device generation: the old handle belonged to the old device
        }
        this->built_against = context.face;
        if (this->merge_pipeline.has_value()) {
            return; // already built for this device
        }
        std::span<std::uint8_t const> const vertex_spirv = context.shader != nullptr ? context.shader(context.owner, vertex_shader_name) : std::span<std::uint8_t const>{};
        std::span<std::uint8_t const> const fragment_spirv = context.shader != nullptr ? context.shader(context.owner, fragment_shader_name) : std::span<std::uint8_t const>{};
        if (vertex_spirv.empty() || fragment_spirv.empty()) {
            deren::utility::log("software merge disabled: the owner has no {} or {}", vertex_shader_name, fragment_shader_name);
            return;
        }
        // THE DEPTH FORMAT IS THE CONTRACT'S `depth` ROLE: the backend picks the device's concrete format, exactly as
        // the shadow pass's pipeline does. A fallback keeps a context that did not fill the field from building a
        // pipeline with `unknown`.
        rhi::image_format const depth_format = context.depth_format != rhi::image_format::unknown ? context.depth_format : rhi::image_format::depth;
        auto built = pipelines::build_software_merge(*context.face, depth_format, vertex_spirv, fragment_spirv);
        if (!built) {
            deren::utility::log("software merge disabled: {}", built.error());
            this->merge_pipeline.reset();
            return;
        }
        this->merge_pipeline = std::move(*built);
        deren::utility::log("SUCCESS: software merge pipeline created (depth-only fullscreen, LESS_OR_EQUAL + depth write)");
    }

    rhi::error software_merge_pass::record_content(resolved_io const& io) noexcept {
        if (!this->has_pipeline() || io.list == nullptr || io.targets.empty() || io.pipelines.empty() || io.pipelines[0] == nullptr ||
            io.targets[0].view_handle == nullptr || io.extent.width == 0u || io.extent.height == 0u) {
            return rhi::error::invalid_argument; // the record step resolves this or does not record the pass at all
        }
        // THE FRAME'S RECORD STEP BOUND THE PIPELINE (from `pipeline_handle()`) and, because the behaviour says
        // `resync_viewport`, set the viewport and scissor from the resolved extent. So this entry opens the rendering
        // instance, draws the fullscreen triangle and closes it - the pipeline's dynamic state is already emitted.
        //
        // LOAD + STORE, not CLEAR: the existing depth is the comparison's other half. The depth test is the merge
        // (LESS_OR_EQUAL writes only when the fragment's SV_Depth is <= the stored value), so the pass itself carries
        // no arithmetic; Task 3's fragment stage produces the software depth it compares.
        rhi::depth_attachment const depth{.view = io.targets[0].view_handle,
                                          .load = rhi::load_op::load,
                                          .store = rhi::store_op::store,
                                          .read_only = false,
                                          .has_stencil = false,
                                          .clear_depth = 1.0f,
                                          .clear_stencil = 0u};
        rhi::rendering_info const rendering_info{.struct_size = sizeof(rhi::rendering_info),
                                                 .area = {.offset_x = 0, .offset_y = 0, .width = io.extent.width, .height = io.extent.height},
                                                 .layer_count = 1,
                                                 .colors = {},
                                                 .depth = depth,
                                                 .has_depth = true,
                                                 .secondary_contents = false};
        auto const begun = io.list->begin_rendering(rendering_info);
        if (begun != rhi::error::ok) {
            return begun;
        }
        io.list->set_cull_mode(rhi::cull_mode::none); // the fullscreen triangle has no facing to cull
        io.list->draw(3, 1, 0, 0);
        io.list->end_rendering();
        return rhi::error::ok;
    }
} // namespace deren::engine::pass
