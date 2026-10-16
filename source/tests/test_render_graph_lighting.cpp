#include "vk_test.h"

#include <array>
import deren.engine.render_graph.lighting;
import deren.engine.render_graph.post;
import deren.engine.render_graph.geometry;
import deren.engine.pass.deferred;
namespace rg = deren::engine::render_graph;
namespace lighting = rg::lighting;
namespace post = rg::post;
namespace rhi = deren::promise::rhi;
int main() {
    for (bool defined : {false, true}) {
        rg::builder graph;
        rg::geometry::inputs in{};
        in.color = *graph.import_texture({29, 1}, rg::initial_contents::undefined, "HDR", {}, rg::resource_state{});
        in.velocity = *graph.import_texture({29, 2}, rg::initial_contents::undefined, "velocity", {}, rg::resource_state{});
        in.depth = *graph.import_texture({29, 3}, rg::initial_contents::undefined, "depth", {1, 1, rg::texture_aspect::depth}, rg::resource_state{});
        for (unsigned i = 0; i < 3; ++i)
            in.surface[i] = *graph.import_texture({29, 4 + i}, rg::initial_contents::undefined, "surface", {}, rg::resource_state{});
        std::array<rg::buffer_ref, 8> buffers;
        for (unsigned i = 0; i < buffers.size(); ++i)
            buffers[i] = *graph.import_buffer({29, 10 + i}, (i == 0 && !defined) ? rg::initial_contents::undefined : rg::initial_contents::defined, "shared", {64},
                                              rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::mesh_shader | rg::pipeline_stage::fragment_shader, rg::memory_access::read});
        in.shared = rg::geometry::shared_inputs{buffers[0], buffers[1], buffers[2], buffers[3], buffers[4], buffers[5], buffers[6]};
        auto drawn = rg::geometry::append(graph, in);
        auto lit = lighting::append(graph, {in.color, in.surface, in.depth, lighting::shared_inputs{buffers[0], buffers[7]}});
        CHECK(drawn && lit && graph.export_resource(in.color));
        auto plan = graph.compile({true});
        CHECK(static_cast<bool>(plan) == defined);
        if (plan && drawn && lit) {
            CHECK(plan->passes[drawn->pass.index()].accesses.size() == 13);
            CHECK(plan->passes[lit->pass.index()].accesses.size() == 7);
            auto params = plan->passes[drawn->pass.index()].parameters.get<rg::geometry::parameters>();
            CHECK(params && params->shared && params->shared->camera.resource == buffers[0]);
            CHECK(plan->barriers.size() == 11); // Six image initialization + five lighting handoffs, no host pseudo-write.
        }
    }
    {
        rg::builder graph;
        rg::geometry::inputs in{};
        in.color = *graph.import_texture({3, 1}, rg::initial_contents::undefined, "HDR", {}, rg::resource_state{});
        in.velocity = *graph.import_texture({3, 2}, rg::initial_contents::undefined, "velocity", {}, rg::resource_state{});
        in.depth = *graph.import_texture({3, 3}, rg::initial_contents::undefined, "depth", {1, 1, rg::texture_aspect::depth}, rg::resource_state{});
        for (unsigned i = 0; i < 3; ++i)
            in.surface[i] = *graph.import_texture({3, 4 + i}, rg::initial_contents::undefined, "surface", {}, rg::resource_state{});
        auto drawn = rg::geometry::append(graph, in);
        auto lit = lighting::append(graph, {in.color, in.surface, in.depth});
        post::inputs tail{};
        tail.hdr = in.color;
        tail.frame = *graph.import_texture({3, 7}, rg::initial_contents::undefined, "frame", {}, rg::resource_state{});
        for (unsigned i = 0; i < 4; ++i)
            tail.bloom[i] = *graph.import_texture({3, 8 + i}, rg::initial_contents::undefined, "bloom", {}, rg::resource_state{});
        auto output = post::append(graph, tail, {});
        CHECK(drawn && lit && output && graph.export_resource(output->color));
        auto plan = graph.compile({true});
        CHECK(plan && plan->execution_order.size() == 7);
        if (plan && drawn && lit) {
            CHECK(plan->execution_order[0] == drawn->pass.index());
            CHECK(plan->passes[drawn->pass.index()].accesses.size() == 6);
            unsigned handoffs = 0;
            for (auto const& barrier : plan->barriers)
                if (barrier.before_pass == lit->pass.index() && barrier.memory_dependency)
                    ++handoffs;
            CHECK(handoffs == 5);
        }
        in.velocity = in.color;
        CHECK(!rg::geometry::append(graph, in));
    }
    for (bool defined : {false, true}) {
        rg::builder graph;
        lighting::inputs inputs{};
        inputs.color = *graph.import_texture({1, 1}, defined ? rg::initial_contents::defined : rg::initial_contents::undefined, "emissive", {},
                                             rg::resource_state{rg::resource_layout::color_attachment, rg::pipeline_stage::color_output, rg::memory_access::write});
        for (unsigned i = 0; i < 3; ++i)
            inputs.surface[i] = *graph.import_texture({1, 2 + i}, rg::initial_contents::defined, "surface", {},
                                                      rg::resource_state{rg::resource_layout::color_attachment, rg::pipeline_stage::color_output, rg::memory_access::write});
        inputs.depth = *graph.import_texture({1, 5}, rg::initial_contents::defined, "depth", {1, 1, rg::texture_aspect::depth},
                                             rg::resource_state{rg::resource_layout::depth_stencil_attachment, rg::pipeline_stage::depth_stencil, rg::memory_access::write});
        auto lit = lighting::append(graph, inputs);
        CHECK(lit);
        if (!lit)
            continue;
        post::inputs post_inputs{};
        post_inputs.hdr = lit->color;
        post_inputs.frame = *graph.import_texture({1, 6}, rg::initial_contents::undefined, "frame", {}, rg::resource_state{});
        for (unsigned i = 0; i < 4; ++i)
            post_inputs.bloom[i] = *graph.import_texture({1, 7 + i}, rg::initial_contents::undefined, "bloom", {}, rg::resource_state{});
        auto output = post::append(graph, post_inputs, {});
        CHECK(output && graph.export_resource(output->color));
        auto plan = graph.compile({true});
        CHECK(static_cast<bool>(plan) == defined);
        if (plan) {
            CHECK(plan->execution_order.size() == 6 && plan->outputs.size() == 1);
            auto params = plan->passes[lit->pass.index()].parameters.get<lighting::parameters>();
            CHECK(params && plan->passes[lit->pass.index()].accesses.size() == 5);
            bool load = false, sampled = false;
            for (auto const& barrier : plan->barriers) {
                if (barrier.resource == inputs.color.index() && barrier.before_pass == lit->pass.index())
                    load = barrier.memory_dependency && barrier.after.layout == rg::resource_layout::color_attachment && barrier.after.access == (rg::memory_access::read | rg::memory_access::write);
                if (barrier.resource == inputs.color.index() && barrier.before_pass == output->bloom[0].index())
                    sampled = barrier.after.layout == rg::resource_layout::shader_read_only;
            }
            CHECK(load && sampled);
        }
    }
    rg::builder aliases;
    auto ref = *aliases.import_texture({2, 1}, rg::initial_contents::defined, "alias", {}, rg::resource_state{rg::resource_layout::shader_read_only, rg::pipeline_stage::fragment_shader, rg::memory_access::read});
    CHECK(!lighting::append(aliases, {ref, {ref, ref, ref}, ref}));
    deren::engine::pass::deferred_pass deferred;
    CHECK(deferred.record_content({}) == rhi::error::invalid_argument);
    return deren::vk_test::finish("test_render_graph_lighting");
}
