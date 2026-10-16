#include "vk_test.h"

#include <algorithm>
#include <array>
import deren.engine.render_graph;
namespace rg = deren::engine::render_graph;
namespace {
    constexpr auto compute = rg::pipeline_stage::compute_shader;
    constexpr auto fragment = rg::pipeline_stage::fragment_shader;
    constexpr rg::access_scope storage{rg::resource_usage::storage, compute};
    constexpr rg::access_scope sampled{rg::resource_usage::sampled, fragment};
    constexpr rg::resource_state undefined{};
    constexpr rg::resource_state shader_read{rg::resource_layout::shader_read_only, fragment, rg::memory_access::read};
    auto add(rg::builder& graph, char const* name, std::initializer_list<rg::resource_access> accesses, bool root = true) {
        return graph.add_pass(name, std::span(accesses.begin(), accesses.size()), {root});
    }
    void general_tests() {
        rg::builder graph;
        auto image = *graph.import_texture({1, 1}, rg::initial_contents::undefined, "image", {}, undefined);
        CHECK(add(graph, "write", {{image, rg::content_access::discard_write, {}, storage}}));
        CHECK(add(graph, "read-write", {{image, rg::content_access::read_write, {}, storage}}));
        auto plan = graph.compile({.synchronization = true});
        CHECK(plan && plan->synchronization_planned);
        if (!plan)
            return;
        CHECK(plan->barriers.size() == 2);
        auto const& barrier = plan->barriers[1];
        CHECK(barrier.before_pass == 1 && barrier.memory_dependency && !barrier.layout_transition);
        CHECK(barrier.before.layout == rg::resource_layout::general && barrier.after.layout == rg::resource_layout::general);
        CHECK(barrier.before.access == rg::memory_access::write);
        CHECK(barrier.after.access == (rg::memory_access::read | rg::memory_access::write));
        CHECK(plan->final_states.size() == 1 && plan->final_states[0].state == barrier.after);
    }
    void read_scope_tests() {
        rg::builder graph;
        auto image = *graph.import_texture({1, 1}, rg::initial_contents::undefined, "image", {}, undefined);
        add(graph, "write", {{image, rg::content_access::discard_write, {}, storage}});
        add(graph, "fragment", {{image, rg::content_access::read, {}, sampled}});
        add(graph, "compute", {{image, rg::content_access::read, {}, {rg::resource_usage::sampled, compute}}});
        add(graph, "fragment again", {{image, rg::content_access::read, {}, sampled}});
        add(graph, "overwrite", {{image, rg::content_access::discard_write, {}, storage}});
        auto plan = graph.compile({true});
        CHECK(plan && plan->barriers.size() == 4);
        if (!plan)
            return;
        CHECK(plan->barriers[2].before_pass == 2 && plan->barriers[2].memory_dependency && !plan->barriers[2].layout_transition);
        auto const& overwrite = plan->barriers.back();
        CHECK(overwrite.before_pass == 4);
        CHECK((overwrite.before.stages & fragment) == fragment && (overwrite.before.stages & compute) == compute);
        CHECK(overwrite.layout_transition && overwrite.memory_dependency);
        // Compatible read-only inputs need no barriers, but their stages must join.
        rg::builder reads;
        auto input = *reads.import_texture({1, 2}, rg::initial_contents::defined, "input", {}, shader_read);
        add(reads, "a", {{input, rg::content_access::read, {}, sampled}});
        add(reads, "b", {{input, rg::content_access::read, {}, {rg::resource_usage::sampled, compute}}});
        auto read_plan = reads.compile({true});
        CHECK(read_plan && read_plan->barriers.empty());
        CHECK(read_plan->final_states[0].state.stages == (fragment | compute));
    }
    void transition_and_range_tests() {
        rg::builder graph;
        auto image = *graph.import_texture({2, 1}, rg::initial_contents::defined, "image", {2, 1}, shader_read);
        auto first = *add(graph, "sample", {{image, rg::content_access::read, {0, 1}, sampled}});
        auto second = *add(graph, "storage read", {{image, rg::content_access::read, {0, 1}, storage}});
        CHECK(graph.order_after(first, second));
        auto cycle = graph.compile({true});
        CHECK(!cycle && cycle.error().code == rg::error_code::cycle);
        rg::builder disjoint;
        image = *disjoint.import_texture({2, 1}, rg::initial_contents::defined, "image", {2, 1}, shader_read);
        first = *add(disjoint, "sample", {{image, rg::content_access::read, {0, 1}, sampled}});
        second = *add(disjoint, "storage read", {{image, rg::content_access::read, {1, 1}, storage}});
        CHECK(disjoint.order_after(first, second));
        auto plan = disjoint.compile({true});
        CHECK(plan && plan->execution_order.front() == second.index());
        CHECK(plan->barriers.size() == 1 && plan->barriers[0].before_pass == second.index());
        CHECK((plan->barriers[0].range == rg::resource_range{rg::texture_range{1, 1, 0, 1, rg::texture_aspect::color}}));
        CHECK(plan->final_states.size() == 2);
    }
    void validation_tests() {
        rg::builder legacy;
        auto bytes = *legacy.import_buffer({3, 1}, rg::initial_contents::defined, "legacy");
        add(legacy, "read", {{bytes, rg::content_access::read}});
        auto cpu = legacy.compile();
        CHECK(cpu && !cpu->synchronization_planned && cpu->barriers.empty());
        rg::builder missing;
        bytes = *missing.import_buffer({3, 1}, rg::initial_contents::defined, "missing");
        add(missing, "read", {{bytes, rg::content_access::read, {}, storage}});
        auto failure = missing.compile({true});
        CHECK(!failure && failure.error().code == rg::error_code::missing_initial_state);
        rg::builder scopes;
        bytes = *scopes.import_buffer({3, 1}, rg::initial_contents::defined, "bytes", {16}, rg::resource_state{rg::resource_layout::general, compute, rg::memory_access::read});
        CHECK(!add(scopes, "bad role", {{bytes, rg::content_access::read, {}, sampled}}));
        CHECK(!add(scopes, "bad stage", {{bytes, rg::content_access::read, {}, {rg::resource_usage::storage, rg::pipeline_stage::none}}}));
        CHECK(!add(scopes, "bad mode", {{bytes, rg::content_access::discard_write, {}, {rg::resource_usage::uniform_buffer, compute}}}));
        CHECK(!add(scopes, "conflict", {{bytes, rg::content_access::read, {}, storage}, {bytes, rg::content_access::read, {}, {rg::resource_usage::uniform_buffer, compute}}}));
        CHECK(add(scopes, "unscoped", {{bytes, rg::content_access::read}})->index() == 0);
        failure = scopes.compile({true});
        CHECK(!failure && failure.error().code == rg::error_code::missing_scope);
    }
    struct parameters {
        rg::texture_discard_write target;
        void visit_resources(rg::parameter_visitor& visitor) const noexcept {
            visitor(target);
        }
    };
    void typed_and_culling_tests() {
        rg::builder graph;
        auto image = *graph.import_texture({4, 1}, rg::initial_contents::undefined, "target", {}, undefined);
        CHECK(graph.add_pass("dead legacy", parameters{{image}}));
        CHECK(graph.add_pass("write", parameters{{image, {}, storage}}));
        CHECK(graph.export_resource(image));
        auto plan = graph.compile({true});
        CHECK(plan && plan->execution_order.size() == 1 && plan->execution_order[0] == 1);
        CHECK(plan->barriers.size() == 1 && plan->barriers[0].before_pass == 1);
        rg::builder partial;
        image = *partial.import_texture({4, 2}, rg::initial_contents::defined, "partial", {2, 1}, shader_read);
        CHECK(add(partial, "live mip0", {{image, rg::content_access::discard_write, {0, 1}, storage}}, false));
        CHECK(add(partial, "dead mip1", {{image, rg::content_access::discard_write, {1, 1}, {rg::resource_usage::transfer_destination, rg::pipeline_stage::transfer}}}, false));
        CHECK(partial.export_resource(image, {0, 1}));
        plan = partial.compile({true});
        CHECK(plan && plan->execution_order.size() == 1);
        auto mip1 = std::find_if(plan->final_states.begin(), plan->final_states.end(), [](auto const& s) { return std::get<rg::texture_range>(s.range).base_mip == 1; });
        CHECK(mip1 != plan->final_states.end() && mip1->state == shader_read);
    }
    void boundary_and_buffer_tests() {
        rg::builder graph;
        auto image = *graph.import_texture({5, 1}, rg::initial_contents::undefined, "image", {}, undefined);
        add(graph, "write", {{image, rg::content_access::discard_write, {}, storage}});
        add(graph, "read", {{image, rg::content_access::read, {}, sampled}});
        auto first = graph.compile({true});
        CHECK(first && first->final_states.size() == 1);
        auto frontier = first->final_states[0].state;
        CHECK(frontier.layout == rg::resource_layout::shader_read_only);
        CHECK(frontier.access == (rg::memory_access::read | rg::memory_access::write));
        rg::builder next;
        auto imported = next.import_texture({5, 1}, rg::initial_contents::defined, "image", {}, frontier);
        CHECK(imported);
        add(next, "new stage", {{*imported, rg::content_access::read, {}, {rg::resource_usage::sampled, rg::pipeline_stage::vertex_shader}}});
        auto second = next.compile({true});
        CHECK(second && second->barriers.size() == 1 && second->barriers[0].memory_dependency);
        rg::builder buffers;
        auto bytes = *buffers.import_buffer({5, 2}, rg::initial_contents::defined, "bytes", {32}, rg::resource_state{rg::resource_layout::general, compute, rg::memory_access::read});
        add(buffers, "vertex", {{bytes, rg::content_access::read, {0, 16}, {rg::resource_usage::vertex_buffer, rg::pipeline_stage::vertex_input}}});
        add(buffers, "indirect", {{bytes, rg::content_access::read, {0, 16}, {rg::resource_usage::indirect_buffer, rg::pipeline_stage::draw_indirect}}});
        add(buffers, "write", {{bytes, rg::content_access::discard_write, {8, 16}, storage}});
        auto plan = buffers.compile({true});
        CHECK(plan && plan->barriers.size() == 2);
        CHECK(std::all_of(plan->barriers.begin(), plan->barriers.end(), [](auto const& b) { return b.before_pass == 2 && b.memory_dependency && !b.layout_transition; }));
        auto overlap = std::find_if(plan->barriers.begin(), plan->barriers.end(), [](auto const& b) { return std::get<rg::buffer_range>(b.range).offset == 8; });
        CHECK(overlap != plan->barriers.end() && (overlap->before.stages & (rg::pipeline_stage::vertex_input | rg::pipeline_stage::draw_indirect)) == (rg::pipeline_stage::vertex_input | rg::pipeline_stage::draw_indirect));
        // Untouched bytes retain the import frontier.
        auto untouched = std::find_if(plan->final_states.begin(), plan->final_states.end(), [](auto const& s) { return std::get<rg::buffer_range>(s.range).offset == 24; });
        CHECK(untouched != plan->final_states.end() && untouched->state.access == rg::memory_access::read);
    }
    void readonly_transition_tests() {
        rg::builder graph;
        auto image = *graph.import_texture({9, 1}, rg::initial_contents::defined, "input", {}, shader_read);
        add(graph, "layout change", {{image, rg::content_access::read, {}, storage}});
        add(graph, "later stage", {{image, rg::content_access::read, {}, {rg::resource_usage::storage, fragment}}});
        auto plan = graph.compile({true});
        CHECK(plan && !plan->barriers.empty());
        // A transition may write memory even though both logical uses are reads.
        CHECK(plan->barriers[0].memory_dependency);
        CHECK((plan->barriers[0].after.stages & (compute | fragment)) == (compute | fragment));
        // Published frontier must preserve this conservative transition scope.
        auto frontier = plan->final_states[0].state;
        CHECK((frontier.stages & rg::pipeline_stage::vertex_shader) == rg::pipeline_stage::vertex_shader);
        rg::builder next;
        auto imported = next.import_texture({9, 1}, rg::initial_contents::defined, "input", {}, frontier);
        CHECK(imported);
        add(next, "next graph", {{*imported, rg::content_access::read, {}, {rg::resource_usage::storage, rg::pipeline_stage::vertex_shader}}});
        auto result = next.compile({true});
        CHECK(result && result->synchronization_planned);
    }
    void supported_role_tests() {
        for (bool texture : {false, true}) {
            rg::builder graph;
            auto image = *graph.import_texture({6, 1}, rg::initial_contents::defined, "image", {}, shader_read);
            auto bytes = *graph.import_buffer({6, 2}, rg::initial_contents::defined, "bytes", {16}, rg::resource_state{rg::resource_layout::general, compute, rg::memory_access::read});
            rg::access_scope src{rg::resource_usage::transfer_source, rg::pipeline_stage::transfer};
            rg::access_scope dst{rg::resource_usage::transfer_destination, rg::pipeline_stage::transfer};
            if (texture) {
                CHECK(add(graph, "copy read", {{image, rg::content_access::read, {}, src}}));
                CHECK(add(graph, "copy write", {{image, rg::content_access::discard_write, {}, dst}}));
                CHECK(add(graph, "color", {{image, rg::content_access::preserve_write, {}, {rg::resource_usage::color_attachment, rg::pipeline_stage::color_output}}}));
            } else {
                CHECK(add(graph, "copy read", {{bytes, rg::content_access::read, {}, src}}));
                CHECK(add(graph, "copy write", {{bytes, rg::content_access::discard_write, {}, dst}}));
                CHECK(add(graph, "index", {{bytes, rg::content_access::read, {}, {rg::resource_usage::index_buffer, rg::pipeline_stage::vertex_input}}}));
                CHECK(add(graph, "uniform", {{bytes, rg::content_access::read, {}, {rg::resource_usage::uniform_buffer, compute | fragment}}}));
            }
            CHECK(graph.compile({true}));
        }
        rg::builder depth;
        auto image = *depth.import_texture({6, 3}, rg::initial_contents::undefined, "depth", {1, 1, rg::texture_aspect::depth | rg::texture_aspect::stencil}, undefined);
        CHECK(add(depth, "depth write", {{image, rg::content_access::discard_write, {}, {rg::resource_usage::depth_stencil_attachment, rg::pipeline_stage::depth_stencil}}}));
        CHECK(add(depth, "depth read", {{image, rg::content_access::read, {}, {rg::resource_usage::depth_stencil_read, rg::pipeline_stage::depth_stencil}}}));
        auto plan = depth.compile({true});
        CHECK(plan && plan->barriers.size() == 2);
        CHECK(plan->barriers.back().after.layout == rg::resource_layout::depth_stencil_read_only);
        rg::builder unsupported;
        auto buffer = *unsupported.import_buffer({6, 4}, rg::initial_contents::defined, "buffer", {}, rg::resource_state{rg::resource_layout::general, compute, rg::memory_access::read});
        CHECK(add(unsupported, "invalidate", {{buffer, rg::content_access::invalidate}}));
        auto error = unsupported.compile({true});
        CHECK(!error && error.error().code == rg::error_code::unsupported_access);
    }
    void initial_state_tests() {
        rg::builder graph;
        CHECK(!graph.import_texture({7, 1}, rg::initial_contents::defined, "undefined state", {}, undefined));
        CHECK(!graph.import_buffer({7, 2}, rg::initial_contents::defined, "bad layout", {}, shader_read));
        CHECK(!graph.import_texture({7, 3}, rg::initial_contents::defined, "bad stage", {}, rg::resource_state{rg::resource_layout::general, static_cast<rg::pipeline_stage>(1024), rg::memory_access::read}));
        auto first = graph.import_texture({7, 1}, rg::initial_contents::defined, "input", {}, shader_read);
        CHECK(first && first->index() == 0);
        auto alias = graph.import_texture({7, 1}, rg::initial_contents::defined, "alias", {}, shader_read);
        CHECK(alias && *alias == *first);
        CHECK(!graph.import_texture({7, 1}, rg::initial_contents::defined, "no state"));
        CHECK(!graph.import_texture({7, 1}, rg::initial_contents::defined, "different state", {}, rg::resource_state{rg::resource_layout::general, compute, rg::memory_access::write}));
        CHECK(graph.export_resource(*first));
        auto plan = graph.compile({true});
        CHECK(plan && plan->barriers.empty() && plan->final_states.size() == 1);
    }
    // Dense byte oracle checks hazard coverage, independent of sparse splitting.
    void byte_oracle_tests() {
        uint32_t random = 0x31415926U;
        auto roll = [&] { random = random * 1664525U + 1013904223U; return random; };
        constexpr unsigned cells = 16, count = 32;
        struct event {
            unsigned offset, size, stage;
            rg::content_access mode;
        };
        for (unsigned trial = 0; trial < 64; ++trial) {
            rg::builder graph;
            auto bytes = *graph.import_buffer({8, 1}, rg::initial_contents::defined, "bytes", {cells}, rg::resource_state{rg::resource_layout::general, compute, rg::memory_access::read});
            std::array<event, count> events{};
            for (auto& event : events) {
                event.offset = roll() % cells;
                event.size = 1 + roll() % (cells - event.offset);
                event.stage = 1U << (roll() % 3);
                event.mode = std::array{rg::content_access::read, rg::content_access::discard_write, rg::content_access::read_write}[roll() % 3];
                add(graph, "event", {{bytes, event.mode, {event.offset, event.size}, {rg::resource_usage::storage, static_cast<rg::pipeline_stage>(event.stage)}}});
            }
            auto plan = graph.compile({true});
            CHECK(plan && plan->execution_order.size() == count);
            if (!plan)
                continue;
            std::array<unsigned, cells> writer{}, visible{}, readers{};
            readers.fill(static_cast<unsigned>(compute));
            bool correct = true;
            for (auto p : plan->execution_order) {
                auto const& event = events[p];
                for (unsigned cell = event.offset; cell < event.offset + event.size; ++cell) {
                    bool raw = !writer[cell] || (visible[cell] & event.stage) == event.stage;
                    bool overwrite = !writer[cell] && !readers[cell];
                    for (auto const& barrier : plan->barriers) {
                        if (barrier.before_pass != p)
                            continue;
                        auto range = std::get<rg::buffer_range>(barrier.range);
                        if (cell < range.offset || cell >= range.offset + range.size)
                            continue;
                        auto stages = static_cast<unsigned>(barrier.before.stages);
                        auto access = static_cast<unsigned>(barrier.before.access);
                        correct = correct && !barrier.layout_transition && barrier.memory_dependency;
                        if ((access & 2) && (stages & writer[cell]) == writer[cell])
                            raw = true;
                        if ((stages & (writer[cell] | readers[cell])) == (writer[cell] | readers[cell]) && (!writer[cell] || (access & 2)) && (!readers[cell] || (access & 1)))
                            overwrite = true;
                    }
                    if (event.mode != rg::content_access::discard_write)
                        correct = correct && raw;
                    if (event.mode != rg::content_access::read) {
                        correct = correct && overwrite;
                        writer[cell] = event.stage;
                        readers[cell] = visible[cell] = 0;
                    } else {
                        readers[cell] |= event.stage;
                        visible[cell] |= event.stage;
                    }
                }
            }
            CHECK(correct);
            std::array<unsigned, cells> covered{};
            for (auto const& state : plan->final_states) {
                auto range = std::get<rg::buffer_range>(state.range);
                for (unsigned cell = static_cast<unsigned>(range.offset); cell < range.offset + range.size; ++cell) {
                    ++covered[cell];
                    correct = correct && (static_cast<unsigned>(state.state.stages) & (writer[cell] | readers[cell])) == (writer[cell] | readers[cell]);
                }
            }
            CHECK(correct && std::all_of(covered.begin(), covered.end(), [](auto value) { return value == 1; }));
        }
    }
} // namespace
int main() {
    general_tests();
    read_scope_tests();
    transition_and_range_tests();
    validation_tests();
    typed_and_culling_tests();
    boundary_and_buffer_tests();
    readonly_transition_tests();
    supported_role_tests();
    initial_state_tests();
    byte_oracle_tests();
    return deren::vk_test::finish("test_render_graph_sync");
}
