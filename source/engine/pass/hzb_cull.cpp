/**
 * @file source/engine/pass/hzb_cull.cpp
 * @brief the HZB cull pass's pipeline and its single dispatch (see the header's WHY THE PASS RECORDS NO BARRIERS
 *        OF ITS OWN).
 */
module;

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

module deren.engine.pass.hzb_cull;

import deren.utility.shared;
import deren.utility; // deren::utility::log
import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pipelines;

namespace deren::engine::pass {
    void hzb_cull_pass::create(pass_context const& context) {
        if (context.face == nullptr) {
            return;
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->cull_pipeline.reset(); // a new device generation: the old handle belonged to the old device
        }
        this->built_against = context.face;
        if (this->cull_pipeline.has_value()) {
            return; // already built for this device
        }
        std::span<std::uint8_t const> const spirv = context.shader != nullptr ? context.shader(context.owner, hzb_cull_shader_name) : std::span<std::uint8_t const>{};
        if (spirv.empty()) {
            deren::utility::log("hzb cull disabled: the owner has no {}", hzb_cull_shader_name);
            return;
        }
        auto built = pipelines::build_hzb_cull(*context.face, spirv);
        if (!built) {
            deren::utility::log("hzb cull disabled: {}", built.error());
            this->cull_pipeline.reset();
            return;
        }
        this->cull_pipeline = std::move(built->trace);
        deren::utility::log("SUCCESS: hzb cull pipeline created (compute, one 64-thread group per item)");
    }

    rhi::error hzb_cull_pass::record_content(resolved_io const& io) noexcept {
        // THE SAME GUARD SHAPE THE OTHER COMPUTE PASSES USE. `item_count == 0` is NOT a failure: an empty frame has
        // no dispatch to record (§8.3), and the lane's own witness line below is skipped for it.
        if (!this->cull_pipeline.has_value() || io.list == nullptr || this->frame_extent.width == 0u || this->frame_extent.height == 0u) {
            return rhi::error::invalid_argument;
        }
        if (this->frame_item_count == 0u) {
            return rhi::error::ok;
        }
        // ---- THE PUSH BLOCK: the values the HOST knows (spec §7.2's expected extent and the item count) ----
        // The framework appends the two heap index lanes (frame_slot, image_index) after these three words, which is
        // the whole of what the shader reads besides the heap (see HzbCullPush in the shader).
        std::array<std::uint32_t, 3> const push_block{this->frame_item_count, this->frame_extent.width, this->frame_extent.height};
        if (!io.push_block(*io.list, std::as_bytes(std::span<std::uint32_t const>(push_block)))) {
            return rhi::error::invalid_argument;
        }
        // ---- THE DISPATCH: exactly one workgroup per item (spec §2.1 - the count is the host's answer, so the cull
        // does not need the dispatch-indirect verb the RHI deliberately does not have). ----
        io.list->dispatch(this->frame_item_count, 1u, 1u);
        // THE LANE'S OWN WITNESS, once per session (the build lane has the same shape and the same reason: the
        // absence of a validation error cannot distinguish "the graph recorded the cull" from "the gate was on but no
        // frame had a history HZB"). The GPU-side counts are the stats block's - read at shutdown and by the probe.
        static bool recorded_logged = false;
        if (!recorded_logged) {
            recorded_logged = true;
            deren::utility::log("hzb: cull pass recorded {} item(s) at {}x{} - one 64-thread group per item, testing against the "
                                "HISTORY HZB (this frame slot's previous use, 2 frames old)",
                                this->frame_item_count,
                                this->frame_extent.width,
                                this->frame_extent.height);
        }
        return rhi::error::ok;
    }
} // namespace deren::engine::pass
