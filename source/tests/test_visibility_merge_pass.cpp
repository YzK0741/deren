// Headless unit tests: the SOFTWARE MERGE pass's declaration ==================
// M4 Task 2 lands the pass SCAFFOLDING only: the depth-only fullscreen declaration, the graphics behaviour, the
// pipeline gating and the recording guards. The merge's shader math is Task 3; what must be pinned HERE is the shape
// the design's correctness argument rests on - a DEPTH-ONLY fullscreen pass that writes the frame's G-buffer depth,
// so a frame with no pipeline (or no resolved depth attachment) is refused rather than recorded halfway.
#include "vk_test.h"

import deren.engine.pass;
import deren.engine.render_resource;
import deren.promise.rhi;
import deren.engine.pass.software_merge;

namespace pass = deren::engine::pass;

int main() {
    pass::software_merge_pass merge;
    // ---- the declaration: ONE depth attachment, which is the FRAME'S OWN G-buffer depth (the merge writes min(hw,sw)
    //      back into it; it has no colour target and no own binding) ----
    auto const& io = merge.io();
    CHECK(io.name == "software merge");
    CHECK(io.bindings.empty());
    CHECK(io.targets.size() == 1u);
    CHECK(io.targets[0].resource == deren::engine::render_resource::resource_id::gbuffer_depth);
    CHECK(io.targets[0].kind == deren::engine::render_resource::target_kind::depth);
    CHECK(io.push == std::nullopt);
    // ---- the behaviour: a GRAPHICS pass over the full extent; the declaration carries the viewport rule because
    //      the pipeline is the pass's own (it is not in the renderer's named registry) ----
    auto const& behaviour = merge.behaviour();
    CHECK(behaviour.kind == pass::behaviour_kind::graphics);
    CHECK(behaviour.extent == pass::extent_rule::full);
    CHECK(behaviour.resync_viewport);
    CHECK(behaviour.pipelines.empty());
    // ---- the pipeline the pass owns is the DEPTH-ONLY one built at create time (LESS_OR_EQUAL + depth write), which
    //      a device-free test can only assert through the gating: no pipeline -> not ready, refuse the frame ----
    CHECK(!merge.has_pipeline());
    CHECK(!merge.ready());
    CHECK(merge.pipeline_handle() == nullptr);
    {
        pass::resolved_io empty{};
        CHECK(merge.record_content(empty) == deren::promise::rhi::error::invalid_argument);
    }
    // ---- a frame with an extent but no resolved depth target is refused (recording it would draw into nothing) ----
    {
        pass::resolved_io io_without_target{};
        io_without_target.extent = {.width = 64u, .height = 64u};
        CHECK(merge.record_content(io_without_target) == deren::promise::rhi::error::invalid_argument);
    }
    return deren::vk_test::finish("test_visibility_merge_pass");
}
