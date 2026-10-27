/**
 * @file source/engine/pass/visibility_resolve.cpp
 * @brief the visibility resolve pass's pipeline and dispatch (see the header's WHY THIS PASS EXISTS).
 */
module;

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

module deren.engine.pass.visibility_resolve;

import deren.utility.shared;
import deren.utility; // deren::utility::log
import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pipelines;

namespace deren::engine::pass {
    namespace {
        /// the shader's [numthreads(8, 8, 1)] - the dispatch divides the extent by it, and the two numbers being one
        /// is the contract `behaviour::group_size_*` carries
        constexpr std::uint32_t resolve_group_size = 8u;
    } // namespace

    void visibility_resolve_pass::create(pass_context const& context) {
        if (context.face == nullptr) {
            return;
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->resolve_pipeline.reset(); // a new device generation: the old handle belonged to the old device
        }
        this->built_against = context.face;
        if (this->resolve_pipeline.has_value()) {
            return; // already built for this device
        }
        std::span<std::uint8_t const> const spirv = context.shader != nullptr ? context.shader(context.owner, shader_name) : std::span<std::uint8_t const>{};
        if (spirv.empty()) {
            deren::utility::log("visibility resolve disabled: the owner has no {}", shader_name);
            return;
        }
        // A HEAP-NATIVE COMPUTE PIPELINE: no layout, no set - every descriptor it samples or writes is a heap slot.
        auto built = pipelines::build_visibility_resolve(*context.face, spirv);
        if (!built) {
            deren::utility::log("visibility resolve disabled: {}", built.error());
            this->resolve_pipeline.reset();
            return;
        }
        this->resolve_pipeline = std::move(built->trace);
        deren::utility::log("SUCCESS: visibility resolve pipeline created (compute, one 8x8 group per pixel tile, five G-buffer storage targets)");
    }

    rhi::error visibility_resolve_pass::record_content(resolved_io const& io) noexcept {
        if (!this->resolve_pipeline.has_value() || io.list == nullptr || io.extent.width == 0u || io.extent.height == 0u) {
            return rhi::error::invalid_argument;
        }
        // THE FRAME'S RECORD STEP BOUND THE PIPELINE the declaration resolved from `pipeline_handle()` (the pass's
        // own contract object), so this entry binds nothing: a second bind would be a duplicated state change.
        //
        // THE PUSH BLOCK IS THE TWO HEAP INDICES (and the endpoint's third lane): the shader reads its per-frame and
        // per-image slots through them, and the block's declared shape is `VisibilityResolvePush` in the shader -
        // The endpoint writes the first three words; the optional feedback word is appended below.
        if (!io.push_block(*io.list, {})) {
            return rhi::error::invalid_argument;
        }
        // Preserve the endpoint's third word (software/hybrid visibility). Feedback has its own word.
        auto status = io.list->push_constants(12, std::as_bytes(std::span(&mip_feedback_slot, 1)));
        if (status != rhi::error::ok)
            return status;
        // ONE THREAD PER PIXEL, rounded up: the shader returns early for the threads past the right/bottom edge,
        // and the extent it derives comes from the storage image, which the declaration's `full` rule makes the
        // render extent - so a resize needs nothing newer than the frame.
        std::uint32_t const groups_x = (io.extent.width + resolve_group_size - 1u) / resolve_group_size;
        std::uint32_t const groups_y = (io.extent.height + resolve_group_size - 1u) / resolve_group_size;
        io.list->dispatch(groups_x, groups_y, 1u);
        return rhi::error::ok;
    }
} // namespace deren::engine::pass
