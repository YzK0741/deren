// Headless unit tests: the visibility lanes' graph declarations ==================
// Lane R (the raster) is ONE pass with two attachments and three table reads; lane V (the resolve, v0.4c) is ONE
// compute pass with two sampled reads, five storage writes and six buffer reads. These checks pin both declarations
// so the runtime wiring cannot silently widen or mis-scope them. The full-frame acceptance is the capture gate (a
// GPU artifact); what can run in CI is the graph's own arithmetic.
#include "vk_test.h"

#include <array>
#include <optional>

import deren.engine.render_graph.visibility;

namespace rg = deren::engine::render_graph;
namespace visibility = rg::visibility;

int main() {
    // ---- the RASTER lane as it will be recorded: both attachments and the three stage reads ----
    {
        rg::builder graph;
        visibility::inputs in{};
        in.vis_id = *graph.import_texture({31, 1}, rg::initial_contents::undefined, "visibility", {}, rg::resource_state{});
        in.depth = *graph.import_texture({31, 2}, rg::initial_contents::undefined, "depth", {1, 1, rg::texture_aspect::depth}, rg::resource_state{});
        std::array<rg::buffer_ref, 3> buffers{};
        for (unsigned i = 0; i < buffers.size(); ++i) {
            buffers[i] = *graph.import_buffer({31, 10 + i}, rg::initial_contents::defined, "visibility table", {64},
                                              rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::mesh_shader, rg::memory_access::read});
        }
        in.shared = visibility::shared_inputs{buffers[0], buffers[1], buffers[2]};
        auto added = visibility::append(graph, in);
        CHECK(added.has_value());
        CHECK(graph.export_resource(in.vis_id).has_value());
        auto plan = graph.compile({true});
        CHECK(plan.has_value());
        if (plan && added) {
            // two attachment writes + three table reads; the indirect read lands on draw_indirect
            CHECK(plan->passes[added->pass.index()].accesses.size() == 5u);
            auto const* params = plan->passes[added->pass.index()].parameters.get<visibility::parameters>();
            CHECK(params != nullptr);
            CHECK(params && params->shared.has_value());
            CHECK(params && params->shared->camera.resource == buffers[0]);
            CHECK(params && params->shared->meshlets.resource == buffers[1]);
            CHECK(params && params->shared->indirect.resource == buffers[2]);
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

    // ---- the RESOLVE lane (v0.4c): two sampled reads, five storage writes, six table reads ----
    {
        rg::builder graph;
        visibility::resolve_inputs in{};
        auto const surface_state = rg::resource_state{};
        // THE TWO INPUTS ARE DEFINED AT THE IMPORT: the resolve reads them, and a graph whose only write to a
        // sampled image is the raster lane's (this test has no raster pass) would otherwise be the
        // `undefined_read` the graph refuses - the runtime path always has the raster lane ahead of this one.
        rg::resource_state const sampled_state{rg::resource_layout::shader_read_only, rg::pipeline_stage::compute_shader, rg::memory_access::read};
        in.vis_id = *graph.import_texture({31, 51}, rg::initial_contents::defined, "visibility", {}, sampled_state);
        in.depth = *graph.import_texture({31, 52}, rg::initial_contents::defined, "capture depth", {1, 1, rg::texture_aspect::depth}, sampled_state);
        for (unsigned i = 0; i < in.surface.size(); ++i) {
            in.surface[i] = *graph.import_texture({31, 53 + i}, rg::initial_contents::undefined, "GBuffer surface", {}, surface_state);
        }
        in.velocity = *graph.import_texture({31, 56}, rg::initial_contents::undefined, "velocity", {}, surface_state);
        in.color = *graph.import_texture({31, 57}, rg::initial_contents::undefined, "scene colour", {}, surface_state);
        std::array<rg::buffer_ref, 6> buffers{};
        for (unsigned i = 0; i < buffers.size(); ++i) {
            buffers[i] = *graph.import_buffer({31, 60 + i}, rg::initial_contents::defined, "resolve table", {64},
                                              rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::compute_shader, rg::memory_access::read});
        }
        in.shared = visibility::resolve_shared_inputs{buffers[0], buffers[1], buffers[2], buffers[3], buffers[4], buffers[5]};
        auto added = visibility::append_resolve(graph, in);
        CHECK(added.has_value());
        CHECK(graph.export_resource(in.color).has_value());
        auto plan = graph.compile({true});
        CHECK(plan.has_value());
        if (plan && added) {
            CHECK(plan->passes[added->pass.index()].accesses.size() == 13u); // 2 reads + 5 writes + 6 buffers
            auto const* params = plan->passes[added->pass.index()].parameters.get<visibility::resolve_parameters>();
            CHECK(params != nullptr);
            CHECK(params && params->vis_id.resource == in.vis_id);
            CHECK(params && params->depth.resource == in.depth);
            CHECK(params && params->surface[0].resource == in.surface[0]);
            CHECK(params && params->surface[1].resource == in.surface[1]);
            CHECK(params && params->surface[2].resource == in.surface[2]);
            CHECK(params && params->velocity.resource == in.velocity);
            CHECK(params && params->color.resource == in.color);
            CHECK(params && params->shared.has_value());
            CHECK(params && params->shared->camera.resource == buffers[0]);
            CHECK(params && params->shared->materials.resource == buffers[1]);
            CHECK(params && params->shared->items.resource == buffers[2]);
            CHECK(params && params->shared->meshlets.resource == buffers[3]);
            CHECK(params && params->shared->previous_transforms.resource == buffers[4]);
            CHECK(params && params->shared->statistics.resource == buffers[5]);
        }
    }
    // ---- one image cannot be two of the resolve's outputs ----
    {
        rg::builder graph;
        visibility::resolve_inputs in{};
        in.vis_id = *graph.import_texture({31, 71}, rg::initial_contents::undefined, "visibility", {}, rg::resource_state{});
        in.depth = *graph.import_texture({31, 72}, rg::initial_contents::undefined, "capture depth", {1, 1, rg::texture_aspect::depth}, rg::resource_state{});
        for (unsigned i = 0; i < in.surface.size(); ++i) {
            in.surface[i] = *graph.import_texture({31, 73 + i}, rg::initial_contents::undefined, "GBuffer surface", {}, rg::resource_state{});
        }
        in.velocity = *graph.import_texture({31, 76}, rg::initial_contents::undefined, "velocity", {}, rg::resource_state{});
        in.color = in.velocity; // the aliasing this lane must refuse
        CHECK(!visibility::append_resolve(graph, in).has_value());
    }
    // ---- ... and an output cannot be one of the inputs either ----
    {
        rg::builder graph;
        visibility::resolve_inputs in{};
        in.vis_id = *graph.import_texture({31, 81}, rg::initial_contents::undefined, "visibility", {}, rg::resource_state{});
        in.depth = *graph.import_texture({31, 82}, rg::initial_contents::undefined, "capture depth", {1, 1, rg::texture_aspect::depth}, rg::resource_state{});
        for (unsigned i = 0; i < in.surface.size(); ++i) {
            in.surface[i] = *graph.import_texture({31, 83 + i}, rg::initial_contents::undefined, "GBuffer surface", {}, rg::resource_state{});
        }
        in.velocity = *graph.import_texture({31, 86}, rg::initial_contents::undefined, "velocity", {}, rg::resource_state{});
        in.color = in.vis_id; // a read that is also a write
        CHECK(!visibility::append_resolve(graph, in).has_value());
    }
    return deren::vk_test::finish("test_render_graph_visibility");
}
