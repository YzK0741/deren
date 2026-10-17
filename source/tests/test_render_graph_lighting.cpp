#include "vk_test.h"

#include <array>
import deren.engine.render_graph.lighting;
import deren.engine.render_graph.cluster;
import deren.engine.render_graph.post;
import deren.engine.render_graph.geometry;
import deren.engine.pass.deferred;
namespace rg = deren::engine::render_graph;
namespace lighting = rg::lighting;
namespace cluster = rg::cluster;
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
        // THE SCENE-WIDE IBL (adoption stage 2E): three sampled images the lighting stage reads every frame, plus
        // the two shared uniforms - the declared pass now covers the whole image set it samples.
        auto camera = *graph.import_buffer({1, 20}, rg::initial_contents::defined, "camera", {64},
                                           rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::fragment_shader, rg::memory_access::read});
        auto light = *graph.import_buffer({1, 21}, rg::initial_contents::defined, "light", {64},
                                          rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::fragment_shader, rg::memory_access::read});
        rg::resource_state const ibl_state{rg::resource_layout::shader_read_only, rg::pipeline_stage::fragment_shader, rg::memory_access::read};
        auto env = *graph.import_texture({1, 22}, rg::initial_contents::defined, "IBL environment", {}, ibl_state);
        auto irr = *graph.import_texture({1, 23}, rg::initial_contents::defined, "IBL irradiance", {}, ibl_state);
        auto lut = *graph.import_texture({1, 24}, rg::initial_contents::defined, "IBL LUT", {}, ibl_state);
        lighting::shared_inputs shared{camera, light};
        shared.ibl = lighting::ibl_inputs{env, irr, lut};
        // THE CASCADED SHADOW MAP (adoption stage 2H, consumer half): a layered depth array sampled by the shading
        // path. Its producer still records outside the graph, so the import's prior state is the sampled one.
        auto shadow_cascades = *graph.import_texture({1, 25}, rg::initial_contents::defined, "shadow cascades", {1, 4, rg::texture_aspect::depth},
                                                     rg::resource_state{rg::resource_layout::shader_read_only, rg::pipeline_stage::fragment_shader,
                                                                        rg::memory_access::read});
        shared.shadow_cascades = shadow_cascades;
        inputs.shared = shared;
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
            // five stage accesses + two shared uniforms + three IBL images + the shadow cascades
            CHECK(params && plan->passes[lit->pass.index()].accesses.size() == 11);
            CHECK(params && params->shared && params->shared->ibl && params->shared->ibl->environment.resource == env);
            CHECK(params && params->shared->shadow_cascades && params->shared->shadow_cascades->resource == shadow_cascades);
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
    {
        // THE GPU-DRIVEN GEOMETRY INPUTS (adoption stage 2C): the mesh stage reads the meshlet records and this
        // frame's host-culled run, and ATOMICALLY updates the per-slot statistics counters (InterlockedAdd) -
        // that is why the statistics carry a read-write declaration instead of joining the read-only bundle.
        rg::builder graph;
        rg::geometry::inputs in{};
        in.color = *graph.import_texture({51, 1}, rg::initial_contents::undefined, "HDR", {}, rg::resource_state{});
        in.velocity = *graph.import_texture({51, 2}, rg::initial_contents::undefined, "velocity", {}, rg::resource_state{});
        in.depth = *graph.import_texture({51, 3}, rg::initial_contents::undefined, "depth", {1, 1, rg::texture_aspect::depth}, rg::resource_state{});
        for (unsigned i = 0; i < 3; ++i)
            in.surface[i] = *graph.import_texture({51, 4 + i}, rg::initial_contents::undefined, "surface", {}, rg::resource_state{});
        std::array<rg::buffer_ref, 7> shared_buffers;
        for (unsigned i = 0; i < shared_buffers.size(); ++i)
            shared_buffers[i] = *graph.import_buffer({51, 10 + i}, rg::initial_contents::defined, "shared", {64},
                                                     rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::mesh_shader | rg::pipeline_stage::fragment_shader,
                                                                        rg::memory_access::read});
        rg::resource_state const read_state{rg::resource_layout::general, rg::pipeline_stage::mesh_shader, rg::memory_access::read};
        rg::resource_state const read_write_state{rg::resource_layout::general, rg::pipeline_stage::mesh_shader,
                                                  rg::memory_access::read | rg::memory_access::write};
        auto table = *graph.import_buffer({51, 20}, rg::initial_contents::defined, "meshlet table", {64}, read_state);
        auto culled = *graph.import_buffer({51, 21}, rg::initial_contents::defined, "host-culled meshlets", {64}, read_state);
        auto stats = *graph.import_buffer({51, 22}, rg::initial_contents::defined, "meshlet statistics", {32}, read_write_state);
        auto indirect = *graph.import_buffer({51, 23}, rg::initial_contents::defined, "mesh indirect commands", {64},
                                             rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::draw_indirect, rg::memory_access::read});
        in.shared = rg::geometry::shared_inputs{shared_buffers[0], shared_buffers[1], shared_buffers[2],
                                                shared_buffers[3], shared_buffers[4], shared_buffers[5], shared_buffers[6]};
        in.shared->meshlets = rg::geometry::meshlet_inputs{table, culled, stats, indirect};
        auto drawn = rg::geometry::append(graph, in);
        CHECK(drawn && graph.export_resource(in.color));
        auto plan = graph.compile({true});
        CHECK(plan);
        if (plan && drawn) {
            CHECK(plan->passes[drawn->pass.index()].accesses.size() == 17); // six stage writes + seven shared reads + four meshlet inputs
            auto params = plan->passes[drawn->pass.index()].parameters.get<rg::geometry::parameters>();
            CHECK(params && params->shared && params->shared->meshlets);
            bool statistics_read_write = false, table_read = false, indirect_draw_stage = false;
            for (auto const& access : plan->passes[drawn->pass.index()].accesses) {
                if (access.resource == stats.index() && access.mode == rg::content_access::read_write)
                    statistics_read_write = true;
                if (access.resource == table.index() && access.mode == rg::content_access::read)
                    table_read = true;
                if (access.resource == indirect.index() && (access.scope.stages & rg::pipeline_stage::draw_indirect) != rg::pipeline_stage::none)
                    indirect_draw_stage = true;
            }
            CHECK(statistics_read_write && table_read && indirect_draw_stage);
        }
    }
    {
        // THE CLUSTERED-LIGHT SORT AS A GRAPH PASS (adoption stage 2F): the producer declares the two per-slot
        // buffers (atomically incremented bins over a host-zeroed table, and the wholesale light lists) and the
        // lighting stage declares their reads - so the graph, not the pass's two hand-issued barriers, owns the
        // producer -> consumer dependency.
        rg::builder graph;
        auto counts = *graph.import_buffer({61, 1}, rg::initial_contents::defined, "cluster counts", {64},
                                           rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::compute_shader,
                                                              rg::memory_access::read | rg::memory_access::write});
        auto indices = *graph.import_buffer({61, 2}, rg::initial_contents::undefined, "cluster indices", {64}, rg::resource_state{});
        auto sort = cluster::append(graph, {counts, indices});
        CHECK(sort);
        if (sort) {
            rg::resource_state const attached{rg::resource_layout::color_attachment, rg::pipeline_stage::color_output, rg::memory_access::write};
            lighting::inputs inputs{};
            inputs.color = *graph.import_texture({61, 3}, rg::initial_contents::defined, "emissive", {}, attached);
            for (unsigned i = 0; i < 3; ++i)
                inputs.surface[i] = *graph.import_texture({61, 4 + i}, rg::initial_contents::defined, "surface", {}, attached);
            inputs.depth = *graph.import_texture({61, 7}, rg::initial_contents::defined, "depth", {1, 1, rg::texture_aspect::depth},
                                                 rg::resource_state{rg::resource_layout::depth_stencil_attachment, rg::pipeline_stage::depth_stencil,
                                                                    rg::memory_access::write});
            auto camera = *graph.import_buffer({61, 8}, rg::initial_contents::defined, "camera", {64},
                                               rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::fragment_shader, rg::memory_access::read});
            auto light = *graph.import_buffer({61, 9}, rg::initial_contents::defined, "light", {64},
                                              rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::fragment_shader, rg::memory_access::read});
            lighting::shared_inputs shared{camera, light};
            shared.cluster = lighting::cluster_inputs{counts, indices};
            inputs.shared = shared;
            auto lit = lighting::append(graph, inputs);
            CHECK(lit && graph.export_resource(inputs.color));
            auto plan = graph.compile({true});
            CHECK(plan);
            if (plan && lit) {
                bool counts_ordered = false, indices_ordered = false;
                for (auto const& barrier : plan->barriers) {
                    if (barrier.before_pass != lit->pass.index() || !barrier.memory_dependency)
                        continue;
                    if (barrier.resource == counts.index())
                        counts_ordered = true;
                    if (barrier.resource == indices.index())
                        indices_ordered = true;
                }
                CHECK(counts_ordered && indices_ordered);
                std::size_t sort_position = plan->execution_order.size(), lit_position = plan->execution_order.size();
                for (std::size_t i = 0; i < plan->execution_order.size(); ++i) {
                    if (plan->execution_order[i] == sort->pass.index())
                        sort_position = i;
                    if (plan->execution_order[i] == lit->pass.index())
                        lit_position = i;
                }
                CHECK(sort_position < lit_position);
            }
        }
    }
    rg::builder aliases;
    auto ref = *aliases.import_texture({2, 1}, rg::initial_contents::defined, "alias", {}, rg::resource_state{rg::resource_layout::shader_read_only, rg::pipeline_stage::fragment_shader, rg::memory_access::read});
    CHECK(!lighting::append(aliases, {ref, {ref, ref, ref}, ref}));
    deren::engine::pass::deferred_pass deferred;
    CHECK(deferred.record_content({}) == rhi::error::invalid_argument);
    return deren::vk_test::finish("test_render_graph_lighting");
}
