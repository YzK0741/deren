// Headless unit tests: the visibility raster pass's declaration ==================
// The pass records into the graph; what can be checked without a device is the DECLARATION and the recording
// guards, and those are the parts a later edit could quietly widen.
#include "vk_test.h"

import deren.engine.pass;
import deren.engine.render_resource;
import deren.promise.rhi;
import deren.engine.pass.visibility_raster;

namespace pass = deren::engine::pass;

int main() {
    pass::visibility_raster_pass visibility;
    // ---- the declaration: the id target is colour, the shared depth is depth ----
    auto const& io = visibility.io();
    CHECK(io.name == "visibility raster");
    CHECK(io.targets.size() == 2u);
    CHECK(io.targets[0].resource == deren::engine::render_resource::resource_id::visibility_buffer);
    CHECK(io.targets[0].kind == deren::engine::render_resource::target_kind::color);
    CHECK(io.targets[1].resource == deren::engine::render_resource::resource_id::visibility_depth);
    CHECK(io.targets[1].kind == deren::engine::render_resource::target_kind::depth);
    CHECK(io.push == std::nullopt);
    // ---- the behaviour: a graphics pass over the full extent, with the viewport rule carried by the declaration ----
    auto const& behaviour = visibility.behaviour();
    CHECK(behaviour.kind == pass::behaviour_kind::graphics);
    CHECK(behaviour.extent == pass::extent_rule::full);
    CHECK(behaviour.resync_viewport);
    CHECK(behaviour.pipelines.empty());
    // ---- before create() there is no pipeline, so the frame must not be recorded ----
    CHECK(!visibility.has_pipeline());
    {
        pass::resolved_io empty{};
        CHECK(visibility.record_content(empty) == deren::promise::rhi::error::invalid_argument);
    }
    // ---- a frame with no renderer callback is refused rather than recorded halfway (only the callback is
    //      missing here; the attachment/instance checks are exercised by the frame that wires it) ----
    {
        visibility.set_frame(pass::visibility_frame{});
        pass::resolved_io io_without_callback{};
        io_without_callback.extent = {.width = 64u, .height = 64u};
        CHECK(visibility.record_content(io_without_callback) == deren::promise::rhi::error::invalid_argument);
    }
    return deren::vk_test::finish("test_visibility_raster_pass");
}