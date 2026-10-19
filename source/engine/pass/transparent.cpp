// The transparent pass's implementation: the two hand-off barriers, the LOAD instance, one secondary and the
// depth hand-back. Moved unchanged from `runtime::record_transparent_pass` - same barrier constants, same
// attachment helpers, same order - so the gate decides it on `transparent_blend`, the scenario with an
// alphaMode BLEND material, and on every other scenario that has none (where the pass does not run at all).

module;

#include <array>
#include <cstdint>
#include <memory> // std::shared_ptr: the frame's secondary is the contract handle
#include <span>

module deren.engine.pass.transparent;
import deren.utility.shared;

import deren.promise.rhi; // the record series (abi 20): the barriers, the rendering scope, the secondary's lifecycle
import deren.engine.render_resource;
import deren.utility;

namespace rhi = deren::promise::rhi;

namespace deren::engine::pass {

    render_resource::pass_io const& transparent_pass::io() const noexcept {
        return render_resource::transparent_io;
    }

    deren::engine::pass::behaviour const& transparent_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view transparent_pass::feature() const noexcept {
        // THE RENDERER'S GATE, as a feature name (see the scene pass): a frame whose culling left nothing blended
        // is a frame this pass is INACTIVE on - which is what the record step checks before it resolves anything, so
        // the pass neither records nor resolves and costs nothing on those frames.
        return "transparent";
    }

    void transparent_pass::create(pass_context const&) {
        // Nothing to build: no own set (everything is in the shared scene block) and no pipeline (the leaves name
        // theirs). See the scene pass.
    }

    void transparent_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing is per-image here; the secondary is the frame loop's and the leaves own their buffers.
    }

    void transparent_pass::set_frame(transparent_frame const& frame) noexcept {
        this->pass_frame = frame;
    }

    rhi::error transparent_pass::record_content(resolved_io const& io) noexcept {
        if (this->pass_frame.make_environment == nullptr || !this->pass_frame.secondary || this->pass_frame.leaves.empty() ||
            io.targets.size() < 2 || io.list == nullptr)
            return rhi::error::invalid_argument; // the record step resolves this or does not record the pass at all
        // The leaves go into the frame slot's transparent secondary, with inheritance matching the instance
        // below: ONE colour attachment at 1x. A secondary does not inherit state from its primary, so it binds
        // the shared scene block for itself - the same bind the scene pass's segments make.
        // THE LIFECYCLE RIDES THE CONTRACT NOW (abi 20): the inheritance is the contract's own tagged structure
        // (native colour formats, the depth format, the sample count and the view mask the raw
        // VkCommandBufferInheritanceRenderingInfo carried), and the BACKEND derives the descriptor-heap half
        // itself - chaining it here is not possible and not needed.
        deren::utility::vessel::array<rhi::image_format, 1> const color_formats = {this->pass_frame.color_format};
        rhi::command_buffer_inheritance_info const inheritance = {
            .color_format_count = static_cast<std::uint32_t>(color_formats.size()),
            .color_formats = color_formats.data(),
            .depth_format = this->pass_frame.depth_format,
            .samples = 1u,  // the raw begin's 1x
            .view_mask = 0, // the raw inheritance built `viewMask = 0`
        };
        rhi::command_buffer_begin_info const secondary_begin = {
            .struct_size = sizeof(rhi::command_buffer_begin_info),
            .usage = rhi::to_bits(rhi::command_buffer_usage::render_pass_continue),
            .next = &inheritance.header,
        };
        bool recorded = false;
        if (this->pass_frame.secondary->begin_recording(secondary_begin) == rhi::error::ok) {
            render_environment env = this->pass_frame.make_environment(this->pass_frame.owner, recording_target(this->pass_frame.secondary), /*gbuffer=*/false);
            // No set to bind (see scene_pass::record_segment): every slot these leaves read comes from the heaps,
            // which the runtime binds on this same secondary before executing it.
            for (primitive const* const leaf : this->pass_frame.leaves) {
                leaf->draw(env);
            }
            // A SECONDARY IS ONLY EXECUTABLE ONCE IT HAS ENDED: a failed end is the same hazard as a failed
            // begin, so it leaves `recorded` false and nothing is executed.
            if (this->pass_frame.secondary->end_recording() == rhi::error::ok) {
                recorded = true;
            } else {
                deren::utility::log("transparent pass: secondary end failed - transparent leaves skipped this frame");
            }
        } else {
            deren::utility::log("transparent pass: secondary begin failed - transparent leaves skipped this frame");
        }

        // loadOp LOAD on both attachments: the scene target holds the shaded frame and the depth holds the
        // opaque surface, and neither may be cleared.
        // THE SCOPE RIDES THE CONTRACT TOO (abi 20): one colour attachment and the depth, both LOAD + STORE, and
        // the raw call's flags word is the `secondary_contents` bit.
        deren::utility::vessel::array<rhi::color_attachment, 1> const colors = {
            rhi::color_attachment{.view = io.targets[0].view_handle, .load = rhi::load_op::load, .store = rhi::store_op::store, .clear = {}},
        };
        rhi::depth_attachment const depth = {.view = io.targets[1].view_handle,
                                             .load = rhi::load_op::load, // make_load_depth_attachment_info's loadOp
                                             .store = rhi::store_op::store,
                                             .read_only = false,
                                             .has_stencil = false,
                                             .clear_depth = 1.0f,
                                             .clear_stencil = 0};
        rhi::rendering_info const rendering_info{
            .struct_size = sizeof(rhi::rendering_info),
            .area = {.offset_x = 0, .offset_y = 0, .width = this->pass_frame.extent.width, .height = this->pass_frame.extent.height},
            .layer_count = 1, // the raw make_rendering_info's layerCount, which is 1 at every site in this engine
            .colors = colors,
            .depth = depth,
            .has_depth = true,
            .secondary_contents = true, // the raw call passed VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT
        };
        if (io.list->begin_rendering(rendering_info) != rhi::error::ok) {
            return rhi::error::invalid_argument; // a refused scope would leave the attachments in a state nobody declared
        }
        if (recorded) {
            rhi::mergeable_command_buffer_ref const batch[] = {this->pass_frame.secondary};
            if (io.list->execute(std::span<rhi::mergeable_command_buffer_ref const>(batch)) != rhi::error::ok) {
                deren::utility::log("transparent pass: merging the segment was refused - the blended leaves are missing this frame");
            }
        }
        io.list->end_rendering();
        return rhi::error::ok;
    }

} // namespace deren::engine::pass
