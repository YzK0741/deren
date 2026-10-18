// Headless unit tests: the visibility RESOLVE pass's declaration ==================
// The pass records into the graph; what can be checked without a device is the DECLARATION, the compute
// behaviour and the recording guards - the parts a later edit could quietly widen (v0.4c).
#include "vk_test.h"

import deren.engine.pass;
import deren.engine.render_resource;
import deren.promise.rhi;
import deren.engine.pass.visibility_resolve;

namespace pass = deren::engine::pass;

int main() {
    pass::visibility_resolve_pass resolve;
    // ---- the declaration: a HEAP-NATIVE compute pass names no binding, no target and no push block ----
    auto const& io = resolve.io();
    CHECK(io.name == "visibility resolve");
    CHECK(io.bindings.empty());
    CHECK(io.targets.empty());
    CHECK(io.push == std::nullopt);
    // ---- the behaviour: one 8x8 group per pixel tile over the frame's full extent, no viewport resync ----
    auto const& behaviour = resolve.behaviour();
    CHECK(behaviour.kind == pass::behaviour_kind::compute);
    CHECK(behaviour.group_size_x == 8u);
    CHECK(behaviour.group_size_y == 8u);
    CHECK(behaviour.group_size_z == 1u);
    CHECK(behaviour.extent == pass::extent_rule::full);
    CHECK(!behaviour.resync_viewport);
    CHECK(behaviour.pipelines.empty());
    // ---- before create() there is no pipeline, so the frame must not be recorded ----
    CHECK(!resolve.has_pipeline());
    CHECK(!resolve.ready());
    CHECK(resolve.pipeline_handle() == nullptr);
    {
        pass::resolved_io empty{};
        CHECK(resolve.record_content(empty) == deren::promise::rhi::error::invalid_argument);
    }
    // ---- an extent but no pipeline is still refused: the guard is the pipeline, not the frame ----
    {
        pass::resolved_io io_with_extent{};
        io_with_extent.extent = {.width = 64u, .height = 64u};
        CHECK(resolve.record_content(io_with_extent) == deren::promise::rhi::error::invalid_argument);
    }
    return deren::vk_test::finish("test_visibility_resolve_pass");
}
