/**
 * @file source/engine/pass/hzb_build.cpp
 * @brief the HZB build pass's pipeline and its per-level dispatches (see the header's WHY THE PASS RECORDS ITS
 *        OWN BARRIERS).
 */
module;

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

module deren.engine.pass.hzb_build;

import deren.utility.shared;
import deren.utility; // deren::utility::log
import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pipelines;
import deren.engine.visibility.hzb;

namespace deren::engine::pass {
    void hzb_build_pass::create(pass_context const& context) {
        if (context.face == nullptr) {
            return;
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->build_pipeline.reset(); // a new device generation: the old handle belonged to the old device
        }
        this->built_against = context.face;
        if (this->build_pipeline.has_value()) {
            return; // already built for this device
        }
        std::span<std::uint8_t const> const spirv = context.shader != nullptr ? context.shader(context.owner, hzb_build_shader_name) : std::span<std::uint8_t const>{};
        if (spirv.empty()) {
            deren::utility::log("hzb build disabled: the owner has no {}", hzb_build_shader_name);
            return;
        }
        auto built = pipelines::build_hzb_build(*context.face, spirv);
        if (!built) {
            deren::utility::log("hzb build disabled: {}", built.error());
            this->build_pipeline.reset();
            return;
        }
        this->build_pipeline = std::move(built->trace);
        deren::utility::log("SUCCESS: hzb build pipeline created (compute, one 8x8 group per tile of each pyramid level)");
    }

    rhi::error hzb_build_pass::record_content(resolved_io const& io) noexcept {
        // THE SAME GUARD SHAPE THE OTHER COMPUTE PASSES USE, plus the buffer the inter-level barriers need: without
        // it this pass would record a sequence of levels that each read a level nobody ordered - a wrong pyramid,
        // not an error.
        if (!this->build_pipeline.has_value() || io.list == nullptr || io.extent.width == 0u || io.extent.height == 0u || this->levels_buffer == nullptr) {
            return rhi::error::invalid_argument;
        }
        std::uint32_t const width = io.extent.width;
        std::uint32_t const height = io.extent.height;
        std::uint32_t const levels = deren::engine::visibility::hzb::level_count(width, height);
        if (levels == 0u || levels > deren::engine::visibility::hzb::hzb_max_levels) {
            return rhi::error::invalid_argument; // a degenerate extent, or more levels than the shader's cap allows
        }
        // THE FRAME'S RECORD STEP BOUND THE PIPELINE the declaration resolved from `pipeline_handle()`, so this
        // entry binds nothing (a second bind would be a duplicated state change).
        std::uint32_t dispatched = 0u;
        for (std::uint32_t level = 0u; level < levels; ++level) {
            // ---- THE INTRA-PASS DEPENDENCY (spec §8.1): level L reads what level L-1 wrote ----
            // Exactly one buffer barrier between adjacent dispatches: the same buffer, COMPUTE -> COMPUTE, which is
            // one of the RHI's role pairs (spec C12's gap is only the mesh stage, and nothing here is a mesh read).
            // The graph cannot see inside this pass, so this is the pass's own business rather than a declared edge.
            if (level > 0u) {
                rhi::buffer_barrier const ordering{.resource = this->levels_buffer, .from = rhi::buffer_use::shader_write, .to = rhi::buffer_use::shader_read};
                if (io.list->barrier(rhi::barrier_group{.buffers = std::span<rhi::buffer_barrier const>(&ordering, 1u)}) != rhi::error::ok) {
                    return rhi::error::invalid_argument;
                }
            }
            // ---- THE PUSH BLOCK: the level being dispatched ----
            // The framework appends the two heap index lanes (frame_slot, image_index) after these four bytes, which
            // is the whole of what the shader reads besides the heap (see HzbBuildPush in the shader).
            std::uint32_t const push_level = level;
            if (!io.push_block(*io.list, std::as_bytes(std::span<std::uint32_t const>(&push_level, 1u)))) {
                return rhi::error::invalid_argument;
            }
            deren::engine::visibility::hzb::level_size const extent = deren::engine::visibility::hzb::level_size_of(width, height, level);
            constexpr std::uint32_t group_size = 8u; // the shader's [numthreads(8, 8, 1)]
            io.list->dispatch((extent.width + group_size - 1u) / group_size, (extent.height + group_size - 1u) / group_size, 1u);
            ++dispatched;
        }
        // THE LANE'S OWN WITNESS, once per run (the visibility raster lane has the same shape and the same reason):
        // the absence of a validation error cannot distinguish "the graph recorded the build" from "the gate is on
        // but no frame ever had a visibility lane", and the probe dump alone cannot say which frame built it.
        static bool recorded_logged = false;
        if (!recorded_logged) {
            recorded_logged = true;
            deren::utility::log("hzb: build pass recorded {} level(s) / {}x{} extent - {} word(s) per frame lane, {} dispatch(es), one buffer barrier between adjacent levels",
                                levels,
                                width,
                                height,
                                deren::engine::visibility::hzb::pyramid_words(width, height),
                                dispatched);
        }
        return rhi::error::ok;
    }
} // namespace deren::engine::pass
