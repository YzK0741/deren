#include "../utility/shared/memory_test_hooks.hpp"
#include "vessel_child.hpp"
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <type_traits>
import deren.engine.render_graph;
namespace rg = deren::engine::render_graph;
namespace {
    using deren::utility::vessel::vector;
    auto add(rg::builder& b, char const* name, std::initializer_list<rg::resource_access> accesses, bool side_effect = false) {
        return b.add_pass(name, {accesses.begin(), accesses.size()}, {side_effect});
    }
    void content_tests() {
        {
            rg::builder b;
            auto image = *b.import_texture({1, 1}, rg::initial_contents::undefined, "hdr");
            auto old = *add(b, "old", {{image, rg::content_access::discard_write}});
            auto latest = *add(b, "latest", {{image, rg::content_access::discard_write}});
            CHECK(b.export_resource(image));
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>{latest.index()});
            if (plan)
                CHECK(!plan->passes[old.index()].live && plan->resources[0].last_version == 2);
        }
        for (auto mode : {rg::content_access::preserve_write, rg::content_access::read_write}) {
            rg::builder b;
            auto image = *b.import_texture({1, 1}, rg::initial_contents::undefined, "hdr");
            add(b, "producer", {{image, rg::content_access::discard_write}});
            add(b, "consumer", {{image, mode}});
            b.export_resource(image);
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>({0, 1}));
            if (plan)
                CHECK(plan->reads.size() == 1 && plan->reads[0].producer == 0 && plan->reads[0].version == 1);
        }
        for (auto mode : {rg::content_access::read, rg::content_access::preserve_write, rg::content_access::read_write}) {
            rg::builder b;
            auto image = *b.import_texture({1, 1}, rg::initial_contents::undefined, "missing");
            add(b, "bad", {{image, mode}}, true);
            auto error = b.compile();
            CHECK(!error && error.error().code == rg::error_code::undefined_read);
            CHECK(!b.compile());
        }
        {
            rg::builder b;
            auto image = *b.import_texture({1, 1}, rg::initial_contents::undefined, "dead");
            add(b, "dead bad read", {{image, rg::content_access::read}});
            auto plan = b.compile();
            CHECK(plan && plan->execution_order.empty());
        }
        for (auto initial : {rg::initial_contents::undefined, rg::initial_contents::defined}) {
            rg::builder b;
            auto image = *b.import_texture({1, 1}, initial, "output");
            b.export_resource(image);
            auto plan = b.compile();
            CHECK(initial == rg::initial_contents::defined ? plan && plan->execution_order.empty() : !plan && plan.error().code == rg::error_code::undefined_export);
        }
        {
            rg::builder b;
            auto image = *b.import_texture({1, 1}, rg::initial_contents::defined, "discarded");
            add(b, "invalidate", {{image, rg::content_access::invalidate}});
            b.export_resource(image);
            auto result = b.compile();
            CHECK(!result && result.error().code == rg::error_code::undefined_export);
        }
        {
            rg::builder b;
            auto image = *b.import_texture({1, 1}, rg::initial_contents::undefined, "shared");
            auto alias = *b.import_texture({1, 1}, rg::initial_contents::undefined, "alias");
            add(b, "write", {{image, rg::content_access::discard_write}});
            add(b, "side effect A", {{alias, rg::content_access::read}}, true);
            add(b, "side effect B", {{image, rg::content_access::read}}, true);
            b.export_resource(alias);
            b.export_resource(alias);
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>({0, 1, 2}));
        }
    }
} // namespace
namespace {
    bool has_edge(rg::compiled_graph const& graph, uint32_t from, uint32_t to, rg::dependency_reason reason) {
        return std::any_of(graph.dependencies.begin(), graph.dependencies.end(), [&](auto const& edge) { return edge.from == from && edge.to == to && edge.reason == reason; });
    }
    void hazard_tests() {
        {
            rg::builder b;
            auto r = *b.import_buffer({1, 1}, rg::initial_contents::undefined, "buffer");
            add(b, "write", {{r, rg::content_access::discard_write}});
            add(b, "read", {{r, rg::content_access::read}}, true);
            add(b, "overwrite", {{r, rg::content_access::discard_write}}, true);
            b.export_resource(r);
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>({0, 1, 2}));
            if (plan) {
                CHECK(has_edge(*plan, 0, 1, rg::dependency_reason::raw));
                CHECK(has_edge(*plan, 1, 2, rg::dependency_reason::war));
                CHECK(has_edge(*plan, 0, 2, rg::dependency_reason::waw));
            }
        }
        {
            rg::builder b;
            auto r = *b.import_buffer({1, 1}, rg::initial_contents::defined, "read only");
            add(b, "read A", {{r, rg::content_access::read}}, true);
            add(b, "read B", {{r, rg::content_access::read}}, true);
            auto plan = b.compile();
            CHECK(plan && plan->dependencies.empty());
        }
        {
            rg::builder b;
            auto a = *b.import_texture({1, 1}, rg::initial_contents::undefined, "same name");
            auto c = *b.import_texture({1, 2}, rg::initial_contents::undefined, "same name");
            add(b, "A", {{a, rg::content_access::discard_write}});
            add(b, "B", {{c, rg::content_access::discard_write}});
            b.export_resource(a);
            b.export_resource(c);
            auto plan = b.compile();
            CHECK(plan && plan->dependencies.empty() && plan->execution_order == vector<uint32_t>({0, 1}));
        }
        {
            rg::builder b;
            auto a = *add(b, "A", {}, true);
            add(b, "B", {}, true);
            auto c = *add(b, "C", {}, true);
            b.order_after(a, c);
            b.order_after(a, c);
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>({1, 2, 0}));
            if (plan)
                CHECK(has_edge(*plan, 2, 0, rg::dependency_reason::explicit_order));
        }
        {
            rg::builder b;
            auto dead = *add(b, "dead", {});
            auto live = *add(b, "live", {}, true);
            b.order_after(live, dead);
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>{1} && plan->dependencies.empty());
        }
        {
            rg::builder b;
            auto a = *add(b, "A", {});
            auto c = *add(b, "B", {});
            b.order_after(a, c);
            b.order_after(c, a);
            auto result = b.compile();
            CHECK(!result && result.error().code == rg::error_code::cycle);
        }
        {
            rg::builder b;
            auto a = *add(b, "self", {});
            b.order_after(a, a);
            auto result = b.compile();
            CHECK(!result && result.error().code == rg::error_code::cycle);
        }
        {
            rg::builder b;
            auto r = *b.import_texture({1, 1}, rg::initial_contents::undefined, "cycle");
            auto writer = *add(b, "writer", {{r, rg::content_access::discard_write}});
            auto reader = *add(b, "reader", {{r, rg::content_access::read}}, true);
            b.order_after(writer, reader);
            auto result = b.compile();
            CHECK(!result && result.error().code == rg::error_code::cycle);
        }
        {
            rg::builder b;
            auto r = *b.import_texture({1, 1}, rg::initial_contents::undefined, "parallel reasons");
            auto writer = *add(b, "writer", {{r, rg::content_access::discard_write}});
            auto reader = *add(b, "reader", {{r, rg::content_access::read}}, true);
            b.order_after(reader, writer);
            b.order_after(reader, writer);
            auto plan = b.compile();
            CHECK(plan && plan->execution_order == vector<uint32_t>({0, 1}));
        }
    }
} // namespace
namespace {
    void lifetime_tests() {
        auto result = [] {
            rg::builder b;
            char resource_name[] = "persistent resource", pass_name[] = "persistent pass";
            auto r = *b.import_texture({1, 1}, rg::initial_contents::undefined, resource_name);
            add(b, pass_name, {{r, rg::content_access::discard_write}});
            b.export_resource(r);
            resource_name[0] = 'x';
            pass_name[0] = 'x';
            return b.compile();
        }();
        CHECK(result && result->resources[0].name == "persistent resource" && result->passes[0].name == "persistent pass");
        auto failure = [] {
            rg::builder b;
            char name[] = "invalid named resource";
            auto r = b.import_texture({0, 1}, rg::initial_contents::defined, name);
            name[0] = 'x';
            return r.error();
        }();
        CHECK(failure.code == rg::error_code::invalid_identity && failure.resource_name == "invalid named resource");
        {
            rg::builder b;
            auto previous = *b.import_buffer({1, 1}, rg::initial_contents::defined, "input");
            for (uint32_t i = 0; i < 20000; ++i) {
                auto next = *b.import_buffer({1, static_cast<uint64_t>(i) + 2}, rg::initial_contents::undefined, "link");
                add(b, "step", {{previous, rg::content_access::read}, {next, rg::content_access::discard_write}});
                previous = next;
            }
            b.export_resource(previous);
            auto deep = b.compile();
            CHECK(deep && deep->execution_order.size() == 20000 && deep->execution_order.front() == 0 && deep->execution_order.back() == 19999);
            if (deep)
                CHECK(deep->resources.back().last_producer == 19999 && deep->reads.back().producer == 19998);
        }
    }
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--oom") {
        vessel_prepare_child();
        rg::builder b;
        auto r = *b.import_texture({1, 1}, rg::initial_contents::undefined, "oom");
        add(b, "write", {{r, rg::content_access::discard_write}});
        b.export_resource(r);
        utility_memory_test_fail_after(0);
        auto unexpected = b.compile();
        return unexpected ? 99 : 98;
    }
    static_assert(!std::is_move_constructible_v<rg::builder>);
    rg::builder empty;
    auto none = empty.compile();
    CHECK(none && none->execution_order.empty());
    CHECK(!empty.compile());
    CHECK(!empty.import_texture({1, 1}, rg::initial_contents::defined, "late"));
    rg::builder b, other;
    char name[] = "owned";
    auto image = b.import_texture({1, 7}, rg::initial_contents::defined, name);
    CHECK(image.has_value());
    name[0] = 'x';
    auto alias = b.import_texture({1, 7}, rg::initial_contents::defined, "alias");
    CHECK(alias && *alias == *image);
    CHECK(!b.import_texture({1, 7}, rg::initial_contents::undefined, "conflict"));
    CHECK(!b.import_buffer({1, 7}, rg::initial_contents::defined, "wrong kind"));
    CHECK(!b.import_texture({0, 7}, rg::initial_contents::defined, "zero"));
    CHECK(b.import_texture({2, 7}, rg::initial_contents::defined, "owned"));
    CHECK(b.import_texture({1, 8}, rg::initial_contents::defined, "owned"));
    CHECK(!other.export_resource(*image));
    CHECK(!b.export_resource(rg::texture_ref{}));
    auto foreign = other.import_texture({1, 7}, rg::initial_contents::defined, "foreign");
    std::array invalid{rg::resource_access{*foreign, rg::content_access::read}};
    CHECK(!b.add_pass("foreign", invalid));
    std::array duplicate{rg::resource_access{*image, rg::content_access::read}, rg::resource_access{*alias, rg::content_access::read}};
    auto pass = b.add_pass("read", duplicate, {.side_effect = true});
    CHECK(pass && pass->index() == 0);
    std::array conflict{rg::resource_access{*image, rg::content_access::read}, rg::resource_access{*image, rg::content_access::discard_write}};
    CHECK(!b.add_pass("conflict", conflict));
    CHECK(b.export_resource(*image));
    auto plan = b.compile();
    CHECK(plan && plan->resources.size() == 3 && plan->passes.size() == 1);
    if (plan) {
        CHECK(plan->resources[0].name == "owned");
        CHECK(plan->passes[0].accesses.size() == 1);
    }
    content_tests();
    hazard_tests();
    lifetime_tests();
    std::remove("deren-crash.log");
    auto child = vessel_run_child(argv[0], "--oom", "render-graph-oom.log");
    CHECK(child.started && child.code != 0 && child.code != 99 && child.code != 98);
    // The fatal evidence is the emergency file; the redirected console capture must not carry library
    // diagnostics (only the child harness's abort marker may be there).
    auto* file = std::fopen("deren-crash.log", "rb");
    CHECK(file != nullptr);
    if (file) {
        char text[1024]{};
        auto count = std::fread(text, 1, sizeof(text), file);
        std::fclose(file);
        CHECK(std::string_view(text, count).find("allocation failure: out_of_memory") != std::string_view::npos);
        CHECK(std::string_view(text, count).find("stacktrace") == std::string_view::npos);
    }
    auto* console = std::fopen("render-graph-oom.log", "rb");
    CHECK(console != nullptr);
    if (console) {
        char text[1024]{};
        auto count = std::fread(text, 1, sizeof(text), console);
        std::fclose(console);
        CHECK(std::string_view(text, count).find("deren ") == std::string_view::npos);
    }
    return deren::vk_test::finish("test_render_graph");
}
