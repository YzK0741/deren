/**
 * @file source/engine/pass/software_visibility.cpp
 * @brief the software raster pass's pipeline and tile dispatch (see the header's WHY THIS PASS EXISTS).
 */
module;

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

module deren.engine.pass.software_visibility;

import deren.utility.shared;
import deren.utility; // deren::utility::log
import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pipelines;

namespace deren::engine::pass {
    void software_visibility_pass::create(pass_context const& context) {
        if (context.face == nullptr) {
            return;
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->visibility_pipeline.reset(); // a new device generation: the old handle belonged to the old device
        }
        this->built_against = context.face;
        if (this->visibility_pipeline.has_value()) {
            return; // already built for this device
        }
        std::span<std::uint8_t const> const spirv = context.shader != nullptr ? context.shader(context.owner, shader_name) : std::span<std::uint8_t const>{};
        if (spirv.empty()) {
            deren::utility::log("software visibility disabled: the owner has no {}", shader_name);
            return;
        }
        // A HEAP-NATIVE COMPUTE PIPELINE: no layout, no set - every descriptor it samples or writes is a heap slot.
        auto built = pipelines::build_software_visibility(*context.face, spirv);
        if (!built) {
            deren::utility::log("software visibility disabled: {}", built.error());
            this->visibility_pipeline.reset();
            return;
        }
        this->visibility_pipeline = std::move(built->trace);
        deren::utility::log("SUCCESS: software visibility pipeline created (compute, one 8x8 group per 16x16 tile, two R32_UINT software images)");
    }

    rhi::error software_visibility_pass::record_content(resolved_io const& io) noexcept {
        if (!this->has_pipeline() || io.list == nullptr || io.extent.width == 0u || io.extent.height == 0u) {
            return rhi::error::invalid_argument;
        }
        // ONE GROUP PER TILE, rounded up: the shader's 8x8 threads cover a 16x16 tile, and the tile grid is what the
        // CSR list was binned for. A thread past the right/bottom edge of the frame returns early (Task 3's shader),
        // so the dispatch needs no exact-divisibility assumption.
        //
        // THE FRAME'S RECORD STEP BOUND THE PIPELINE the declaration resolved from `pipeline_handle()` (the pass's own
        // contract object), so this entry binds nothing: a second bind would be a duplicated state change. The heap
        // index lanes and the tile's own indices are Task 3's push block - this scaffolding records the dispatch shape.
        std::uint32_t const groups_x = (io.extent.width + tile_extent - 1u) / tile_extent;
        std::uint32_t const groups_y = (io.extent.height + tile_extent - 1u) / tile_extent;
        io.list->dispatch(groups_x, groups_y, 1u);
        return rhi::error::ok;
    }
} // namespace deren::engine::pass
