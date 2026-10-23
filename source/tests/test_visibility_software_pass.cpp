// Headless unit tests: the SOFTWARE VISIBILITY (raster) pass's declaration ==================
// M4 Task 2 lands the pass SCAFFOLDING only: the declaration, the compute behaviour, the pipeline gating and the
// recording guards. The GPU raster math itself is Task 3, so this file checks exactly what a device-free test can
// check - and it checks the parts a later edit could quietly widen (the tile workgroup shape, the full-extent rule,
// and "no pipeline means refuse the frame, never record half of it").
#include "vk_test.h"

#include <cstdint>
#include <cstdlib>

import deren.engine.pass;
import deren.engine.render_resource;
import deren.engine.visibility.software;
import deren.promise.rhi;
import deren.engine.pass.software_visibility;

namespace pass = deren::engine::pass;
namespace software = deren::engine::visibility::software;

int main() {
    pass::software_visibility_pass raster;
    // ---- the declaration: a HEAP-NATIVE compute pass names no binding, no target and no push block ----
    auto const& io = raster.io();
    CHECK(io.name == "software visibility");
    CHECK(io.bindings.empty());
    CHECK(io.targets.empty());
    CHECK(io.push == std::nullopt);
    // ---- the behaviour: ONE 8x8 WORKGROUP PER 16x16 TILE over the frame's full extent, no viewport resync ----
    // (16 = 8x8 threads x 4 pixels each; the tile size is the CSR list's tile, so the two numbers are one contract)
    CHECK(pass::software_visibility_pass::tile_extent == software::tile_size);
    CHECK(pass::software_visibility_pass::tile_extent == 16u);
    auto const& behaviour = raster.behaviour();
    CHECK(behaviour.kind == pass::behaviour_kind::compute);
    CHECK(behaviour.group_size_x == 8u);
    CHECK(behaviour.group_size_y == 8u);
    CHECK(behaviour.group_size_z == 1u);
    CHECK(behaviour.extent == pass::extent_rule::full);
    CHECK(!behaviour.resync_viewport);
    CHECK(behaviour.pipelines.empty());
    // ---- before create() there is no pipeline, so the frame must not be recorded ----
    CHECK(!raster.has_pipeline());
    CHECK(!raster.ready());
    CHECK(raster.pipeline_handle() == nullptr);
    {
        pass::resolved_io empty{};
        CHECK(raster.record_content(empty) == deren::promise::rhi::error::invalid_argument);
    }
    // ---- an extent but no pipeline is still refused: the guard is the pipeline, not the frame ----
    {
        pass::resolved_io io_with_extent{};
        io_with_extent.extent = {.width = 64u, .height = 64u};
        CHECK(raster.record_content(io_with_extent) == deren::promise::rhi::error::invalid_argument);
    }
    // Explicit prototype request must refuse preparation without a device/pipeline;
    // the runtime calls this before publishing a shortened hardware prefix.
#ifdef _WIN32
    _putenv_s("DEREN_SOFTWARE_RASTER_TRIANGLES", "1");
#else
    setenv("DEREN_SOFTWARE_RASTER_TRIANGLES", "1", 1);
#endif
    pass::software_visibility_pass unavailable;
    unavailable.create({});
    CHECK(!unavailable.prepare_triangles(0u, 1280u, 720u));
    CHECK(!unavailable.prepare_triangles(1u, 1280u, 720u));
    CHECK(!unavailable.triangles_active());
    CHECK(!unavailable.prepare_gpu_route(0u, 1280u, 720u, {.threshold_px = 1.0f, .near_plane = 0.1f, .projection_scale = 1280.0f, .perspective = true}));
    CHECK(!unavailable.gpu_routing());

#ifdef _WIN32
    _putenv_s("DEREN_SOFTWARE_RASTER_TRIANGLES", "");
#else
    unsetenv("DEREN_SOFTWARE_RASTER_TRIANGLES");
#endif
    return deren::vk_test::finish("test_visibility_software_pass");
}
