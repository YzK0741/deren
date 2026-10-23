// Headless unit tests: the visibility lanes' graph declarations ==================
// Lane R (the raster) is ONE pass with two attachments and three table reads; lane V (the resolve, v0.4c) is ONE
// compute pass with two sampled reads, five storage writes and six buffer reads. These checks pin both declarations
// so the runtime wiring cannot silently widen or mis-scope them. The full-frame acceptance is the capture gate (a
// GPU artifact); what can run in CI is the graph's own arithmetic.
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

import deren.engine.render_graph.visibility;

namespace rg = deren::engine::render_graph;
namespace visibility = rg::visibility;

namespace {
    struct buffer_writer {
        rg::buffer_discard_write buffer;
        void visit_resources(rg::parameter_visitor& visit) const noexcept {
            visit(buffer);
        }
    };
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
    void gpu_software_route_resources() {
        rg::builder graph;
        std::array<rg::buffer_ref, 14> b{};
        for (unsigned i = 0; i < b.size(); ++i)
            b[i] = *graph.import_buffer({95, 100 + i}, rg::initial_contents::defined, "route table", {256},
                                        rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::compute_shader, rg::memory_access::read});
        visibility::software_routing_inputs routing{b[0], b[10], b[11], b[12]};
        auto cull = visibility::append_hzb_cull(graph, b[6], b[7], b[8], b[3], b[9], b[5], b[4], b[2], routing);
        CHECK(cull.has_value());
        auto image = [&](unsigned id, bool depth = false) { return *graph.import_texture({95, id}, rg::initial_contents::undefined, "route image", {1, 1, depth ? rg::texture_aspect::depth : rg::texture_aspect::color}, rg::resource_state{}); };
        auto vid = image(1), depth = image(2, true), sd = image(3), si = image(4);
        visibility::shared_inputs shared{b[0], b[1], b[2]};
        shared.culled_meshlets = b[3];
        shared.hzb_survivors = b[4];
        shared.hzb_results = b[5];
        auto hw = visibility::append(graph, {vid, depth, shared});
        CHECK(hw.has_value());
        auto recovery = visibility::append_hzb_recovery(graph, b[6], b[7], b[8], b[3], b[9], b[5], b[4], b[2], routing);
        CHECK(recovery.has_value());
        visibility::software_raster_inputs in{sd, si, b[0], b[9], b[1], b[11], b[13], b[3], b[5]};
        in.indirect_arguments = b[12];
        auto sw = visibility::append_software_raster(graph, in, hw->pass);
        CHECK(sw.has_value());
        CHECK(graph.export_resource(si).has_value());
        auto compiled = graph.compile({true});
        CHECK(compiled.has_value());
        if (compiled && sw) {
            bool args_barrier = false, work_barrier = false;
            for (auto const& barrier : compiled->barriers) {
                if (barrier.before_pass != sw->pass.index() || !barrier.memory_dependency)
                    continue;
                if (barrier.resource == b[12].index() && barrier.after.stages == rg::pipeline_stage::draw_indirect)
                    args_barrier = true;
                if (barrier.resource == b[11].index() && barrier.after.stages == rg::pipeline_stage::compute_shader)
                    work_barrier = true;
            }
            CHECK(args_barrier && work_barrier);
        }
    }

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
    // ============================================================================================
    // M4 SOFTWARE LANES (Task 2): the declaration shapes and the DECLARED ORDER
    //
    // The design's correctness argument is an ORDER (spec §3.2/§5.1): hardware raster -> software raster ->
    // RESOLVE -> MERGE, and the resolve reads the UNMERGED hardware depth. Task 4 wires it, but the declarations
    // own the order NOW: `append_software_raster` explicitly follows the hardware raster, and
    // `append_software_merge` orders the resolve after the software raster and itself after the resolve. This
    // builds exactly that four-lane chain and inspects the compiled plan's explicit edges and execution order,
    // plus the merge's declared depth read/write shape.
    // ============================================================================================
    void software_lanes() {
        rg::builder graph;
        auto image = [&](unsigned const id, bool const depth = false) {
            return *graph.import_texture({41, id}, rg::initial_contents::undefined, "software chain image",
                                         depth ? rg::texture_shape{1, 1, rg::texture_aspect::depth} : rg::texture_shape{}, rg::resource_state{});
        };
        auto table = [&](unsigned const id) {
            return *graph.import_buffer({41, 100 + id}, rg::initial_contents::defined, "software chain table", {64},
                                        rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::mesh_shader, rg::memory_access::read});
        };
        auto const vis_id = image(1);
        auto const hw_depth = image(2, true);
        auto const sw_depth = image(3);
        auto const sw_id = image(4);
        std::array<rg::texture_ref, 3> surface = {image(5), image(6), image(7)};
        auto const velocity = image(8);
        auto const color = image(9);
        auto const camera = table(0);
        auto const meshlets = table(1);
        auto const indirect = table(2);
        auto const items = table(3);
        auto const tiles = table(4);
        auto const statistics = table(5);
        auto const culled = table(6);
        auto const materials = table(7);
        auto const previous = table(8);
        auto const hzb_results = table(9);
        // ---- hardware raster (the software lanes' predecessor) ----
        visibility::inputs hw_inputs{vis_id, hw_depth, visibility::shared_inputs{camera, meshlets, indirect}};
        hw_inputs.shared->culled_meshlets = culled;
        auto hardware = visibility::append(graph, hw_inputs);
        CHECK(hardware.has_value());
        CHECK(graph.export_resource(vis_id).has_value()); // keep the producer live for the liveness pass
        // ---- software raster: AFTER the hardware raster ----
        visibility::software_raster_inputs software_inputs{};
        software_inputs.software_depth = sw_depth;
        software_inputs.software_id = sw_id;
        software_inputs.camera = camera;
        software_inputs.items = items;
        software_inputs.meshlets = meshlets;
        software_inputs.tiles = tiles;
        software_inputs.statistics = statistics;
        software_inputs.culled_meshlets = culled;
        software_inputs.hzb_results = hzb_results;
        constexpr rg::access_scope compute{rg::resource_usage::storage, rg::pipeline_stage::compute_shader};
        auto recovery = graph.add_pass("visibility recovery", buffer_writer{{hzb_results, {}, compute}});
        CHECK(recovery.has_value());
        if (recovery && hardware) {
            CHECK(graph.order_after(*recovery, hardware->pass).has_value());
        }
        auto software = visibility::append_software_raster(graph, software_inputs, hardware->pass, recovery ? std::optional<rg::pass_ref>{*recovery} : std::nullopt);
        CHECK(software.has_value());
        // ---- resolve: its two OPTIONAL software reads turn the candidate selection on, nothing else changes ----
        visibility::resolve_inputs resolve_inputs{};
        resolve_inputs.vis_id = vis_id;
        resolve_inputs.depth = hw_depth;
        resolve_inputs.surface = surface;
        resolve_inputs.velocity = velocity;
        resolve_inputs.color = color;
        resolve_inputs.shared = visibility::resolve_shared_inputs{camera, materials, items, meshlets, previous, statistics};
        resolve_inputs.shared->culled_meshlets = culled;
        resolve_inputs.software_depth = sw_depth;
        resolve_inputs.software_id = sw_id;
        auto resolve = visibility::append_resolve(graph, resolve_inputs);
        CHECK(resolve.has_value());
        CHECK(graph.export_resource(color).has_value());
        // ---- merge: after the resolve, reading the software images and writing the G-buffer depth ----
        auto const merge = visibility::append_software_merge(graph, {sw_depth, sw_id, hw_depth}, resolve->pass, software->pass);
        CHECK(merge.has_value());
        CHECK(graph.export_resource(hw_depth).has_value());
        // ---- the compiled plan: the three explicit order edges and the execution order ----
        auto plan = graph.compile({true});
        CHECK(plan.has_value());
        if (!plan || !hardware || !recovery || !software || !resolve || !merge) {
            return;
        }
        auto const explicit_edge = [&](rg::pass_ref const from, rg::pass_ref const to) {
            for (auto const& dependency : plan->dependencies) {
                if (dependency.from == from.index() && dependency.to == to.index() && dependency.reason == rg::dependency_reason::explicit_order) {
                    return true;
                }
            }
            return false;
        };
        CHECK(explicit_edge(hardware->pass, software->pass)); // software raster after hardware raster
        CHECK(explicit_edge(*recovery, software->pass));      // HZB-recovered geometry is included before software resolve
        CHECK(explicit_edge(software->pass, resolve->pass));  // resolve after software raster
        CHECK(explicit_edge(resolve->pass, merge->pass));     // merge after resolve (the winner rule needs it)
        auto const position = [&](rg::pass_ref const ref) {
            auto const found = std::find(plan->execution_order.begin(), plan->execution_order.end(), ref.index());
            return found == plan->execution_order.end() ? std::numeric_limits<std::size_t>::max() : static_cast<std::size_t>(found - plan->execution_order.begin());
        };
        CHECK(position(hardware->pass) < position(software->pass));
        CHECK(position(*recovery) < position(software->pass));
        CHECK(position(software->pass) < position(resolve->pass));
        CHECK(position(resolve->pass) < position(merge->pass));
        // ---- the declarations themselves: the software pass names + the merge's reads/writes ----
        CHECK(plan->passes[software->pass.index()].name == "software visibility");
        CHECK(plan->passes[merge->pass.index()].name == "software merge");
        auto const* merge_parameters = plan->passes[merge->pass.index()].parameters.get<visibility::software_merge_parameters>();
        CHECK(merge_parameters != nullptr);
        if (merge_parameters != nullptr) {
            CHECK(merge_parameters->software_depth.resource == sw_depth); // sampled (fragment)
            CHECK(merge_parameters->software_id.resource == sw_id);       // sampled (fragment): id 0 skips the write
            CHECK(merge_parameters->gbuffer_depth.resource == hw_depth);  // read + depth write
        }
        auto const* resolve_parameters = plan->passes[resolve->pass.index()].parameters.get<visibility::resolve_parameters>();
        CHECK(resolve_parameters != nullptr);
        if (resolve_parameters != nullptr) {
            CHECK(resolve_parameters->software_depth.has_value() && resolve_parameters->software_depth->resource == sw_depth);
            CHECK(resolve_parameters->software_id.has_value() && resolve_parameters->software_id->resource == sw_id);
        }
    }

    /// THE OFF PATH DECLARES NOTHING NEW: without the two optional software reads the resolve's access list is the
    /// pre-M4 one and no software pass is added. This is what "default OFF leaves the frozen gates byte-identical"
    /// rests on at the declaration level.
    void software_absent_when_off() {
        struct counted {
            std::size_t accesses = 0;
            std::size_t software_passes = 0;
        };
        auto const compile = [](bool const with_software) {
            rg::builder graph;
            // DEFINED + a sampled-compute initial state: this scenario declares no producer for the visibility id or
            // the hardware depth (its subject is the resolve's ACCESS LIST, not the chain), and a read of an imported,
            // never-written resource is the builder's `undefined_read`. In the real chain the hardware raster is that
            // producer; here the imports say so, with the state the resolve reads them in.
            constexpr rg::resource_state const visibility_state{rg::resource_layout::shader_read_only, rg::pipeline_stage::compute_shader, rg::memory_access::read};
            auto const vis_id = *graph.import_texture({52, 1}, rg::initial_contents::defined, "visibility", {}, visibility_state);
            auto const depth = *graph.import_texture({52, 2}, rg::initial_contents::defined, "depth", {1, 1, rg::texture_aspect::depth}, visibility_state);
            std::array<rg::texture_ref, 3> surface{};
            for (unsigned i = 0; i < surface.size(); ++i) {
                surface[i] = *graph.import_texture({52, 3 + i}, rg::initial_contents::undefined, "surface", {}, rg::resource_state{});
            }
            auto const velocity = *graph.import_texture({52, 6}, rg::initial_contents::undefined, "velocity", {}, rg::resource_state{});
            auto const color = *graph.import_texture({52, 7}, rg::initial_contents::undefined, "colour", {}, rg::resource_state{});
            // SIX HANDLES, not one reused six times: the graph layer refuses the SAME resource declared with two
            // different access scopes in one pass (builder::add_pass's conflicting_access rule - one resource cannot be
            // both a uniform buffer and a storage buffer in one declaration), and these roles ask for exactly that. The
            // scenario's subject is the resolve's access COUNT, so the fixture is six minimal imports.
            std::array<rg::buffer_ref, 6> tables{};
            for (unsigned i = 0; i < tables.size(); ++i) {
                tables[i] = *graph.import_buffer({52, 20 + i}, rg::initial_contents::defined, "table", {64},
                                                 rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::mesh_shader, rg::memory_access::read});
            }
            visibility::resolve_inputs resolve_inputs{};
            resolve_inputs.vis_id = vis_id;
            resolve_inputs.depth = depth;
            resolve_inputs.surface = surface;
            resolve_inputs.velocity = velocity;
            resolve_inputs.color = color;
            resolve_inputs.shared = visibility::resolve_shared_inputs{tables[0], tables[1], tables[2], tables[3], tables[4], tables[5]};
            if (with_software) {
                // DEFINED + a real initial state: this scenario declares no software pass (its subject is the resolve's
                // access list), so nothing in THIS graph produces the software images before the resolve reads them -
                // and a read of an imported, never-written resource is the builder's `undefined_read`. In the real frame
                // the software raster is that producer; here the imports say "some producer made them earlier, read as
                // sampled compute data", which is what the builder's valid_initial_state rule asks a defined import for.
                constexpr rg::resource_state const software_state{rg::resource_layout::shader_read_only, rg::pipeline_stage::compute_shader, rg::memory_access::read};
                resolve_inputs.software_depth = *graph.import_texture({52, 8}, rg::initial_contents::defined, "software depth", {}, software_state);
                resolve_inputs.software_id = *graph.import_texture({52, 9}, rg::initial_contents::defined, "software id", {}, software_state);
            }
            auto const resolve = visibility::append_resolve(graph, resolve_inputs);
            CHECK(resolve.has_value());
            CHECK(graph.export_resource(color).has_value());
            auto plan = graph.compile({});
            counted result{};
            if (plan) {
                for (auto const& pass : plan->passes) {
                    if (pass.name == "visibility resolve") {
                        result.accesses = pass.accesses.size();
                    }
                    if (pass.name == "software visibility" || pass.name == "software merge") {
                        ++result.software_passes;
                    }
                }
            }
            return result;
        };
        auto const off = compile(false);
        auto const on = compile(true);
        CHECK(off.software_passes == 0u);
        CHECK(on.software_passes == 0u);         // the optional reads add accesses to the RESOLVE, not new passes
        CHECK(on.accesses == off.accesses + 2u); // exactly the two sampled software reads, nothing widened
        // ---- both-or-neither: one software input alone is refused (a half-wired candidate selector cannot read) ----
        {
            rg::builder graph;
            visibility::resolve_inputs resolve_inputs{};
            resolve_inputs.vis_id = *graph.import_texture({53, 1}, rg::initial_contents::undefined, "visibility", {}, rg::resource_state{});
            resolve_inputs.depth = *graph.import_texture({53, 2}, rg::initial_contents::undefined, "depth", {1, 1, rg::texture_aspect::depth}, rg::resource_state{});
            for (unsigned i = 0; i < resolve_inputs.surface.size(); ++i) {
                resolve_inputs.surface[i] = *graph.import_texture({53, 3 + i}, rg::initial_contents::undefined, "surface", {}, rg::resource_state{});
            }
            resolve_inputs.velocity = *graph.import_texture({53, 6}, rg::initial_contents::undefined, "velocity", {}, rg::resource_state{});
            resolve_inputs.color = *graph.import_texture({53, 7}, rg::initial_contents::undefined, "colour", {}, rg::resource_state{});
            resolve_inputs.software_depth = *graph.import_texture({53, 8}, rg::initial_contents::undefined, "software depth", {}, rg::resource_state{});
            CHECK(!visibility::append_resolve(graph, resolve_inputs).has_value());
        }
    }
} // namespace

int main() {
    test_full_recovery_chain();
    gpu_software_route_resources();
    test_hzb_consumers();
    software_lanes();
    software_absent_when_off();
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
