// Boost.Graph dependency contract probe; this is not the render graph implementation.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <vector>
#if !defined(BOOST_NO_EXCEPTIONS) || !defined(BOOST_ALL_NO_LIB)
#error Boost.Graph must use the private no-exceptions header target
#endif
#if defined(BOOST_GRAPH_USE_MPI)
#error Distributed Boost.Graph is not part of this vendored subset
#endif
#include "../third_party/boost/boost/version.hpp"
static_assert(BOOST_VERSION == 109200);
#include "../utility/shared/memory_test_hooks.hpp"
#include "vessel_child.hpp"

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/breadth_first_search.hpp>
#include <boost/graph/filtered_graph.hpp>
#include <boost/graph/reverse_graph.hpp>
#include <boost/throw_exception.hpp>
import deren.utility.shared;
import deren.engine.render_graph.rhi;
namespace mem = deren::utility::memory;
namespace vessel = deren::utility::vessel;
struct graph_probe_parameters {
    deren::engine::render_graph::buffer_read input;
    vessel::vector<int> constants;
    void visit_resources(deren::engine::render_graph::parameter_visitor& visit) const noexcept {
        visit(input);
    }
};
static std::atomic_size_t ordinary_new{0};
void* operator new(std::size_t n) {
    ++ordinary_new;
    if (auto p = std::malloc(n == 0 ? 1 : n))
        return p;
    std::abort();
}
void* operator new[](std::size_t n) {
    return ::operator new(n);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
void* operator new(std::size_t n, std::align_val_t alignment) {
    ++ordinary_new;
    return mem::allocate(n, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t n, std::align_val_t alignment) {
    return ::operator new(n, alignment);
}
void operator delete(void* p, std::align_val_t) noexcept {
    mem::deallocate(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    mem::deallocate(p);
}
#include "../engine/render_graph/graph_backend.hpp"
static int failures = 0, checks = 0;
#define CHECK(x)                                                      \
    do {                                                              \
        ++checks;                                                     \
        if (!(x)) {                                                   \
            ++failures;                                               \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
        }                                                             \
    } while (false)
int main(int argc, char** argv) {
    if (argc > 1) {
        vessel_prepare_child();
        utility_memory_test_fail_after(0);
        graph must_fail(3);
        std::fputs("OOM injection unexpectedly returned\n", stderr);
        return 99;
    }
    auto const control_before = ordinary_new.load();
    {
        boost::adjacency_list<boost::vecS, boost::vecS, boost::bidirectionalS> control(3);
        boost::add_edge(0, 1, control);
        CHECK(boost::num_edges(control) == 1);
    }
    CHECK(ordinary_new.load() > control_before); // Check that the allocation observer is effective.
    auto const baseline = ordinary_new.load();
    {
        graph g(7);
        boost::add_edge(0, 2, g);
        boost::add_edge(1, 2, g);
        boost::add_edge(2, 4, g);
        boost::add_edge(3, 5, g); // disconnected dead branch
        vertex roots[] = {4, 4, 6};
        auto live = reachable(g, roots);
        CHECK(live[0] != boost::white_color && live[1] != boost::white_color && live[2] != boost::white_color);
        CHECK(live[3] == boost::white_color && live[5] == boost::white_color);
        auto sorted = stable_order(g, {live.data()});
        CHECK(sorted && *sorted == vessel::vector<vertex>({0, 1, 2, 4, 6}));
        // Parallel edges must be balanced by the indegree algorithm.
        boost::add_edge(0, 2, g);
        auto parallel = stable_order(g, {live.data()});
        CHECK(parallel && *parallel == *sorted);
        // Insertion order must not change stable execution order.
        graph reordered(7);
        boost::add_edge(2, 4, reordered);
        boost::add_edge(1, 2, reordered);
        boost::add_edge(0, 2, reordered);
        auto alternative = stable_order(reordered, {live.data()});
        CHECK(alternative && *alternative == *sorted);
        boost::add_edge(4, 0, g);
        auto cycle = stable_order(g, {live.data()});
        CHECK(!cycle && cycle.error() == error::cycle);
        graph self(1);
        boost::add_edge(0, 0, self);
        vertex self_root[] = {0};
        auto self_live = reachable(self, self_root);
        CHECK(!stable_order(self, {self_live.data()}));
        graph empty;
        auto none = reachable(empty, {});
        CHECK(stable_order(empty, {none.data()})->empty());
        // No recursive DFS: large chains use explicit project-owned workspace.
        graph deep(20000);
        for (vertex i = 1; i < 20000; ++i)
            boost::add_edge(i - 1, i, deep);
        vertex deep_root[] = {19999};
        auto deep_live = reachable(deep, deep_root);
        auto deep_order = stable_order(deep, {deep_live.data()});
        CHECK(deep_order && deep_order->size() == 20000 && deep_order->front() == 0 && deep_order->back() == 19999);
        // Observe allocations in the actual C++ module, not only the shared adapter.
        namespace rg = deren::engine::render_graph;
        rg::builder actual;
        auto previous = *actual.import_buffer({77, 1}, rg::initial_contents::defined, "input");
        for (uint32_t i = 0; i < 256; ++i) {
            auto next = *actual.import_buffer({77, static_cast<uint64_t>(i) + 2}, rg::initial_contents::undefined, "output");
            rg::resource_access accesses[] = {{previous, rg::content_access::read}, {next, rg::content_access::discard_write}};
            CHECK(actual.add_pass("step", accesses));
            previous = next;
        }
        CHECK(actual.export_resource(previous));
        auto compiled = actual.compile();
        CHECK(compiled && compiled->execution_order.size() == 256);
        rg::builder ranges;
        auto bytes = *ranges.import_buffer({78, 1}, rg::initial_contents::defined, "bytes", {64});
        rg::resource_access readers[] = {{bytes, rg::content_access::read, rg::buffer_range{0, 32}}};
        CHECK(ranges.add_pass("read", readers, {true}));
        rg::resource_access writers[] = {{bytes, rg::content_access::discard_write, rg::buffer_range{16, 24}}, {bytes, rg::content_access::discard_write, rg::buffer_range{24, 24}}};
        CHECK(ranges.add_pass("overlapping union", writers, {true}));
        auto image = *ranges.import_texture({78, 2}, rg::initial_contents::undefined, "image", {3, 3, rg::texture_aspect::depth | rg::texture_aspect::stencil});
        rg::texture_range center{1, 1, 1, 1, rg::texture_aspect::depth};
        rg::resource_access image_write[] = {{image, rg::content_access::discard_write, center}};
        CHECK(ranges.add_pass("center", image_write));
        CHECK(ranges.export_resource(image, center));
        auto ranged = ranges.compile();
        CHECK(ranged && ranged->execution_order.size() == 3 && ranged->resources[1].final_ranges.size() <= 6);
        rg::builder typed;
        auto input = *typed.import_buffer({79, 1}, rg::initial_contents::defined, "typed input", {16});
        CHECK(typed.add_pass("typed snapshot", graph_probe_parameters{{input, {4, 8}}, {1, 2, 3}}, {true}));
        auto typed_plan = typed.compile();
        CHECK(typed_plan && typed_plan->passes[0].parameters.get<graph_probe_parameters>()->constants.size() == 3);
        rg::builder sync;
        auto sync_image = *sync.import_texture({80, 1}, rg::initial_contents::undefined, "sync", {2, 2}, rg::resource_state{});
        rg::resource_access sync_write[] = {{sync_image, rg::content_access::discard_write, {}, {rg::resource_usage::storage, rg::pipeline_stage::compute_shader}}};
        CHECK(sync.add_pass("sync write", sync_write, {true}));
        rg::resource_access sync_read[] = {{sync_image, rg::content_access::read, {0, 1}, {rg::resource_usage::sampled, rg::pipeline_stage::fragment_shader}}};
        CHECK(sync.add_pass("sync read", sync_read, {true}));
        auto sync_plan = sync.compile({true});
        CHECK(sync_plan && sync_plan->synchronization_planned && sync_plan->barriers.size() == 2 && sync_plan->final_states.size() == 2);
        auto lowered_scopes = rg::rhi_bridge::lower_scopes(*sync_plan);
        CHECK(lowered_scopes && lowered_scopes->barriers.size() == sync_plan->barriers.size());
        auto json_dump = rg::dump_json(*sync_plan);
        auto text_dump = rg::dump_text(*sync_plan);
        CHECK(json_dump.find("\"synchronization_planned\":true") != json_dump.npos && text_dump.find("barrier[0]") != text_dump.npos);
    }
    CHECK(ordinary_new.load() == baseline);
    std::printf("Boost %s probe: %d checks, %d failures, ordinary new delta=%zu\n", BOOST_LIB_VERSION, checks, failures, ordinary_new.load() - baseline);
    std::remove("deren-crash.log");
    auto oom = vessel_run_child(argv[0], "--oom", "boost-graph-oom.log");
    CHECK(oom.started && oom.code != 0 && oom.code != 99);
    auto* log = std::fopen("deren-crash.log", "rb");
    CHECK(log != nullptr);
    if (log) {
        char text[1024]{};
        auto count = std::fread(text, 1, sizeof(text), log);
        std::fclose(log);
        CHECK(std::string_view(text, count).find("allocation failure: out_of_memory") != std::string_view::npos);
        CHECK(std::string_view(text, count).find("stacktrace") == std::string_view::npos);
    }
    auto* console = std::fopen("boost-graph-oom.log", "rb");
    CHECK(console != nullptr);
    if (console) {
        char text[1024]{};
        auto count = std::fread(text, 1, sizeof(text), console);
        std::fclose(console);
        CHECK(std::string_view(text, count).find("deren ") == std::string_view::npos);
    }
    std::printf("[test_boost_graph] %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
