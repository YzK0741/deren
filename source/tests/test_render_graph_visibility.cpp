// Headless unit tests: the visibility raster lane's graph declaration =================
// The lane is ONE pass with two attachments and four table reads; these checks pin the declaration's shape so
// the runtime wiring that lands next cannot silently widen or mis-scope it. The full frame acceptance is the
// capture gate (a GPU artifact); what can run in CI is the graph's own arithmetic.
#include "vk_test.h"

#include <array>
#include <optional>

import deren.engine.render_graph.visibility;

namespace rg = deren::engine::render_graph;
namespace visibility = rg::visibility;

int main() {
    // ---- the lane as it will be recorded: both attachments and all four reads ----
    {
        rg::builder graph;
        visibility::inputs in{};
        in.vis_id = *graph.import_texture({31, 1}, rg::initial_contents::undefined, "visibility", {}, rg::resource_state{});
        in.depth = *graph.import_texture({31, 2}, rg::initial_contents::undefined, "depth", {1, 1, rg::texture_aspect::depth}, rg::resource_state{});
        std::array<rg::buffer_ref, 4> buffers{};
        for (unsigned i = 0; i < buffers.size(); ++i) {
            buffers[i] = *graph.import_buffer({31, 10 + i}, rg::initial_contents::defined, "visibility table", {64},
                                              rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::mesh_shader, rg::memory_access::read});
        }
        in.shared = visibility::shared_inputs{buffers[0], buffers[1], buffers[2], buffers[3]};
        auto added = visibility::append(graph, in);
        CHECK(added.has_value());
        CHECK(graph.export_resource(in.vis_id).has_value());
        auto plan = graph.compile({true});
        CHECK(plan.has_value());
        if (plan && added) {
            // two attachment writes + four table reads; the indirect read lands on draw_indirect
            CHECK(plan->passes[added->pass.index()].accesses.size() == 6u);
            auto const* params = plan->passes[added->pass.index()].parameters.get<visibility::parameters>();
            CHECK(params != nullptr);
            CHECK(params && params->shared.has_value());
            CHECK(params && params->shared->camera.resource == buffers[0]);
            CHECK(params && params->shared->materials.resource == buffers[1]);
            CHECK(params && params->shared->items.resource == buffers[2]);
            CHECK(params && params->shared->meshlets.resource == buffers[3]);
            // the pass is the only one, so it is first in the execution order
            CHECK(plan->execution_order.size() == 1u);
            CHECK(plan->execution_order[0] == added->pass.index());
        }
    }
    // ---- attachments only: the shared bundle is optional, and the pass still compiles ----
    {
        rg::builder graph;
        visibility::inputs in{};
        in.vis_id = *graph.import_texture({31, 21}, rg::initial_contents::undefined, "visibility", {}, rg::resource_state{});
        in.depth = *graph.import_texture({31, 22}, rg::initial_contents::undefined, "depth", {1, 1, rg::texture_aspect::depth}, rg::resource_state{});
        auto added = visibility::append(graph, in);
        CHECK(added.has_value());
        CHECK(graph.export_resource(in.vis_id).has_value());
        auto plan = graph.compile({true});
        CHECK(plan.has_value());
        if (plan && added) {
            CHECK(plan->passes[added->pass.index()].accesses.size() == 2u);
            auto params = plan->passes[added->pass.index()].parameters.get<visibility::parameters>();
            CHECK(params && !params->shared.has_value());
        }
    }
    // ---- the id target and the depth image cannot be the same handle ----
    {
        rg::builder graph;
        visibility::inputs in{};
        auto shared_target = *graph.import_texture({31, 41}, rg::initial_contents::undefined, "aliased", {}, rg::resource_state{});
        in.vis_id = shared_target;
        in.depth = shared_target;
        CHECK(!visibility::append(graph, in).has_value());
    }
    return deren::vk_test::finish("test_render_graph_visibility");
}
