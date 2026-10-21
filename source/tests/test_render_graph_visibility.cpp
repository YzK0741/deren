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

namespace {
    struct cull_outputs {
        rg::buffer_discard_write survivors, results;
        rg::buffer_read_write commands;
        void visit_resources(rg::parameter_visitor& visit) const noexcept {
            visit(survivors, results, commands);
        }
    };
    void test_hzb_consumers() {
        for (bool resolve : {false, true}) {
            rg::builder graph;
            std::array<rg::buffer_ref, 10> b{};
            for (unsigned i = 0; i < b.size(); ++i)
                b[i] = *graph.import_buffer({83, 100 + i}, rg::initial_contents::defined, "table", {64},
                                            rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::compute_shader, rg::memory_access::read});
            constexpr rg::access_scope compute{rg::resource_usage::storage, rg::pipeline_stage::compute_shader};
            CHECK(graph.add_pass("hzb cull", cull_outputs{{b[4], {}, compute}, {b[5], {}, compute}, {b[2], {}, compute}}).has_value());
            auto image = [&](unsigned id, bool defined, bool depth = false) {
                return *graph.import_texture({83, id}, defined ? rg::initial_contents::defined : rg::initial_contents::undefined,
                                             "attachment", {1, 1, depth ? rg::texture_aspect::depth : rg::texture_aspect::color},
                                             defined ? rg::resource_state{rg::resource_layout::shader_read_only, rg::pipeline_stage::compute_shader, rg::memory_access::read} : rg::resource_state{});
            };
            auto vis = image(1, resolve), depth = image(2, resolve, true);
            rg::pass_ref consumer{};
            if (!resolve) {
                visibility::inputs in{vis, depth, visibility::shared_inputs{b[0], b[1], b[2]}};
                in.shared->culled_meshlets = b[3];
                in.shared->hzb_survivors = b[4];
                CHECK(!visibility::append(graph, in).has_value());
                in.shared->hzb_results = b[5];
                auto appended = visibility::append(graph, in);
                CHECK(appended.has_value());
                if (!appended)
                    continue;
                consumer = appended->pass;
                CHECK(graph.export_resource(vis).has_value());
            } else {
                visibility::resolve_inputs in{};
                in.vis_id = vis;
                in.depth = depth;
                for (unsigned i = 0; i < in.surface.size(); ++i)
                    in.surface[i] = image(3 + i, false);
                in.velocity = image(6, false);
                in.color = image(7, false);
                in.shared = visibility::resolve_shared_inputs{b[0], b[6], b[7], b[1], b[8], b[9]};
                in.shared->culled_meshlets = b[3];
                in.shared->hzb_survivors = b[4];
                in.shared->hzb_results = b[5];
                auto appended = visibility::append_resolve(graph, in);
                CHECK(appended.has_value());
                if (!appended)
                    continue;
                consumer = appended->pass;
                CHECK(graph.export_resource(in.color).has_value());
            }
            auto plan = graph.compile({true});
            CHECK(plan.has_value());
            if (!plan)
                continue;
            for (auto resource : {b[4], b[5], b[2]}) {
                if (resolve && resource == b[2])
                    continue;
                bool ordered = false;
                auto stage = resource == b[2] ? rg::pipeline_stage::draw_indirect : (resolve ? rg::pipeline_stage::compute_shader : rg::pipeline_stage::mesh_shader);
                for (auto const& barrier : plan->barriers)
                    if (barrier.resource == resource.index() && barrier.before_pass == consumer.index())
                        ordered |= barrier.memory_dependency && barrier.before.stages == rg::pipeline_stage::compute_shader && barrier.after.stages == stage;
                CHECK(ordered);
            }
        }
    }
} // namespace
namespace {
    void test_full_recovery_chain() {
        rg::builder graph;
        std::array<rg::buffer_ref, 13> b{};
        for (unsigned i = 0; i < b.size(); ++i)
            b[i] = *graph.import_buffer({90, 100 + i}, rg::initial_contents::defined, "table", {256}, rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::compute_shader, rg::memory_access::read});
        auto image = [&](unsigned id, bool depth = false) { return *graph.import_texture({90, id}, rg::initial_contents::undefined, "image", {1, 1, depth ? rg::texture_aspect::depth : rg::texture_aspect::color}, rg::resource_state{}); };
        auto vis = image(1), depth = image(2, true);
        visibility::shared_inputs shared{b[0], b[1], b[2]};
        shared.culled_meshlets = b[3];
        shared.hzb_survivors = b[4];
        shared.hzb_results = b[5];
        auto cull = visibility::append_hzb_cull(graph, b[6], b[7], b[8], b[3], b[9], b[5], b[4], b[2]);
        CHECK(cull.has_value());
        auto raster = visibility::append(graph, {vis, depth, shared});
        CHECK(raster.has_value());
        auto build = visibility::append_hzb_build(graph, b[6], b[7], depth, b[0]);
        CHECK(build.has_value());
        auto recover = visibility::append_hzb_recovery(graph, b[6], b[7], b[8], b[3], b[9], b[5], b[4], b[2]);
        CHECK(recover.has_value());
        auto load = visibility::append_recovery(graph, {vis, depth, shared});
        CHECK(load.has_value());
        visibility::resolve_inputs in{};
        in.vis_id = vis;
        in.depth = depth;
        for (unsigned i = 0; i < 3; ++i)
            in.surface[i] = image(3 + i);
        in.velocity = image(6);
        in.color = image(7);
        in.shared = visibility::resolve_shared_inputs{b[0], b[10], b[9], b[1], b[11], b[12]};
        in.shared->culled_meshlets = b[3];
        in.shared->hzb_survivors = b[4];
        in.shared->hzb_results = b[5];
        auto resolve = visibility::append_resolve(graph, in);
        CHECK(resolve.has_value());
        CHECK(graph.export_resource(in.color).has_value());
        auto plan = graph.compile({true});
        CHECK(plan.has_value());
        if (!plan || !cull || !raster || !build || !recover || !load || !resolve)
            return;
        std::array<std::uint32_t, 6> expected{cull->index(), raster->pass.index(), build->index(), recover->index(), load->pass.index(), resolve->pass.index()};
        CHECK(plan->execution_order.size() == expected.size());
        for (unsigned i = 0; i < expected.size() && i < plan->execution_order.size(); ++i)
            CHECK(plan->execution_order[i] == expected[i]);
        auto const* params = plan->passes[load->pass.index()].parameters.get<visibility::raster_load_parameters>();
        CHECK(params && params->vis_id.resource == vis && params->depth.resource == depth);
        auto dependency = [&](auto res, rg::pass_ref before, rg::pipeline_stage stage) {
            for (auto const& barrier : plan->barriers)
                if (barrier.resource == res.index() && barrier.before_pass == before.index() && barrier.memory_dependency && barrier.after.stages == stage)
                    return true;
            return false;
        };
        CHECK(dependency(b[4], load->pass, rg::pipeline_stage::mesh_shader));
        CHECK(dependency(b[2], load->pass, rg::pipeline_stage::draw_indirect));
        CHECK(dependency(depth, *build, rg::pipeline_stage::compute_shader));
        CHECK(dependency(depth, load->pass, rg::pipeline_stage::depth_stencil));
        CHECK(dependency(vis, resolve->pass, rg::pipeline_stage::compute_shader));
    }
} // namespace

int main() {
    test_full_recovery_chain();
    test_hzb_consumers();
    // ---- the RASTER lane as it will be recorded: both attachments and the three stage reads ----
    for (bool with_culled : {false, true}) {
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
        auto culled = *graph.import_buffer({31, 100}, rg::initial_contents::defined, "selected run", {64},
                                           rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::compute_shader, rg::memory_access::write});
        if (with_culled)
            in.shared->culled_meshlets = culled;
        auto added = visibility::append(graph, in);
        CHECK(added.has_value());
        CHECK(graph.export_resource(in.vis_id).has_value());
        auto plan = graph.compile({true});
        CHECK(plan.has_value());
        if (plan && added) {
            // two attachment writes + three table reads; the indirect read lands on draw_indirect
            CHECK(plan->passes[added->pass.index()].accesses.size() == (with_culled ? 6u : 5u));
            auto const* params = plan->passes[added->pass.index()].parameters.get<visibility::parameters>();
            CHECK(params != nullptr);
            CHECK(params && params->shared.has_value());
            CHECK(params && params->shared->camera.resource == buffers[0]);
            CHECK(params && params->shared->meshlets.resource == buffers[1]);
            CHECK(params && params->shared->indirect.resource == buffers[2]);
            CHECK(params && params->shared->culled_meshlets.has_value() == with_culled);
            if (with_culled) {
                bool dependency = false;
                for (auto const& barrier : plan->barriers)
                    if (barrier.resource == culled.index() && barrier.before_pass == added->pass.index())
                        dependency = barrier.memory_dependency && barrier.after.stages == rg::pipeline_stage::mesh_shader;
                CHECK(dependency);
            }
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

    // ---- the RESOLVE lane (v0.4c; v0.5 made it the opaque producer so its depth input is the frame's own
    //      G-buffer depth): two sampled reads, five storage writes, six table reads ----
    for (bool with_culled : {false, true}) {
        rg::builder graph;
        visibility::resolve_inputs in{};
        auto const surface_state = rg::resource_state{};
        // THE TWO INPUTS ARE DEFINED AT THE IMPORT: the resolve reads them, and a graph whose only write to a
        // sampled image is the raster lane's (this test has no raster pass) would otherwise be the
        // `undefined_read` the graph refuses - the runtime path always has the raster lane ahead of this one.
        rg::resource_state const sampled_state{rg::resource_layout::shader_read_only, rg::pipeline_stage::compute_shader, rg::memory_access::read};
        in.vis_id = *graph.import_texture({31, 51}, rg::initial_contents::defined, "visibility", {}, sampled_state);
        in.depth = *graph.import_texture({31, 52}, rg::initial_contents::defined, "GBuffer depth", {1, 1, rg::texture_aspect::depth}, sampled_state);
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
        auto culled = *graph.import_buffer({31, 100}, rg::initial_contents::defined, "selected run", {64},
                                           rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::compute_shader, rg::memory_access::write});
        if (with_culled)
            in.shared->culled_meshlets = culled;
        auto added = visibility::append_resolve(graph, in);
        CHECK(added.has_value());
        CHECK(graph.export_resource(in.color).has_value());
        auto plan = graph.compile({true});
        CHECK(plan.has_value());
        if (plan && added) {
            CHECK(plan->passes[added->pass.index()].accesses.size() == (with_culled ? 14u : 13u)); // 2 reads + 5 writes + 6 buffers
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
            CHECK(params && params->shared->culled_meshlets.has_value() == with_culled);
            if (with_culled) {
                bool dependency = false;
                for (auto const& barrier : plan->barriers)
                    if (barrier.resource == culled.index() && barrier.before_pass == added->pass.index())
                        dependency = barrier.memory_dependency && barrier.after.stages == rg::pipeline_stage::compute_shader;
                CHECK(dependency);
            }
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
