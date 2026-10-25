#include "../utility/shared/memory_test_hooks.hpp"
#include "vessel_child.hpp"
#include "vk_test.h"

#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
import deren.engine.render_graph;
namespace rg = deren::engine::render_graph;
namespace {
    struct inputs {
        rg::texture_read image;
        std::array<rg::buffer_read, 2> buffers;
        void visit_resources(rg::parameter_visitor& visit) const noexcept {
            visit(image, buffers);
        }
    };
    struct parameters {
        inputs source;
        std::optional<rg::texture_discard_write> output;
        int count = 0;
        void visit_resources(rg::parameter_visitor& visit) const noexcept {
            visit(source, output);
        }
    };
    struct move_only {
        rg::buffer_discard_write output;
        std::unique_ptr<int> value;
        void visit_resources(rg::parameter_visitor& visit) const noexcept {
            visit(output);
        }
    };
    struct invalid_parameters {};
    struct mutable_parameters {
        void visit_resources(rg::parameter_visitor&) noexcept {
        }
    };
    struct throwing_parameters {
        void visit_resources(rg::parameter_visitor&) const {
        }
    };
    static_assert(rg::pass_parameters<parameters>);
    static_assert(rg::pass_parameters<move_only>);
    static_assert(!rg::pass_parameters<invalid_parameters>);
    static_assert(!rg::pass_parameters<mutable_parameters>);
    static_assert(!rg::pass_parameters<throwing_parameters>);
    void snapshot_tests() {
        std::optional<rg::compiled_graph> saved;
        {
            rg::builder graph;
            auto image = *graph.import_texture({1, 1}, rg::initial_contents::defined, "input", {2, 1});
            auto first = *graph.import_buffer({1, 2}, rg::initial_contents::defined, "first", {32});
            auto second = *graph.import_buffer({1, 3}, rg::initial_contents::defined, "second", {32});
            auto output = *graph.import_texture({1, 4}, rg::initial_contents::undefined, "output", {2, 1});
            parameters args{{{image, {0, 1}}, {{{first, {0, 16}}, {second, {16, 16}}}}}, rg::texture_discard_write{output, {1, 1}}, 7};
            auto pass = graph.add_pass("typed", args);
            CHECK(pass && pass->index() == 0);
            args.count = 99;
            args.output.reset();
            CHECK(graph.export_resource(output, {1, 1}));
            auto plan = graph.compile();
            CHECK(plan && plan->execution_order.size() == 1);
            if (!plan)
                return;
            CHECK(plan->passes[0].accesses.size() == 4);
            auto const* snapshot = plan->passes[0].parameters.get<parameters>();
            CHECK(snapshot && snapshot->count == 7 && snapshot->output.has_value());
            CHECK(!plan->passes[0].parameters.get<inputs>());
            static_assert(std::is_same_v<decltype(snapshot), parameters const*>);
            saved = std::move(*plan);
        }
        CHECK(saved->passes[0].parameters.get<parameters>()->count == 7);
        auto copy = *saved;
        saved.reset();
        CHECK(copy.passes[0].parameters.get<parameters>()->output.has_value());
        CHECK((copy.passes[0].accesses[3].range == rg::resource_range{rg::texture_range{1, 1, 0, 1, rg::texture_aspect::color}}));
    }
    void validation_tests() {
        rg::builder graph, foreign;
        auto buffer = *graph.import_buffer({1, 1}, rg::initial_contents::undefined, "buffer", {16});
        auto other = *foreign.import_buffer({1, 2}, rg::initial_contents::undefined, "foreign", {16});
        move_only bad{{other}, std::make_unique<int>(1)};
        auto rejected = graph.add_pass("foreign", std::move(bad));
        CHECK(!rejected && rejected.error().code == rg::error_code::foreign_ref);
        move_only outside{{buffer, {15, 2}}, std::make_unique<int>(2)};
        rejected = graph.add_pass("outside", std::move(outside));
        CHECK(!rejected && rejected.error().code == rg::error_code::invalid_range);
        move_only good{{buffer}, std::make_unique<int>(42)};
        auto pass = graph.add_pass("move", std::move(good));
        CHECK(pass && pass->index() == 0);
        CHECK(graph.export_resource(buffer));
        auto plan = graph.compile();
        CHECK(plan && *plan->passes[0].parameters.get<move_only>()->value == 42);
        auto copy = *plan;
        CHECK(*copy.passes[0].parameters.get<move_only>()->value == 42);
        CHECK(!graph.add_pass("frozen", move_only{{buffer}, {}}));
    }
    struct optional_parameters {
        std::optional<rg::buffer_read> input;
        void visit_resources(rg::parameter_visitor& visit) const noexcept {
            visit(input);
        }
    };
    void empty_and_legacy_tests() {
        rg::builder graph;
        auto empty = graph.add_pass("empty", optional_parameters{}, {true});
        CHECK(empty);
        auto legacy = graph.add_pass("legacy", std::span<rg::resource_access const>{}, {true});
        CHECK(legacy);
        auto plan = graph.compile();
        CHECK(plan && plan->execution_order.size() == 2);
        CHECK(plan->passes[0].accesses.empty());
        CHECK(plan->passes[0].parameters.get<optional_parameters>());
        CHECK(!plan->passes[1].parameters.get<optional_parameters>());
    }
    template <rg::content_access Mode>
    struct mode_parameters {
        std::array<rg::buffer_parameter<Mode>, 2> buffers;
        rg::texture_parameter<Mode> image;
        void visit_resources(rg::parameter_visitor& visit) const noexcept {
            visit(std::span(buffers), image);
        }
    };
    template <rg::content_access Mode>
    void mode_tests() {
        rg::builder graph;
        auto bytes = *graph.import_buffer({2, 1}, rg::initial_contents::defined, "bytes", {32});
        auto image = *graph.import_texture({2, 2}, rg::initial_contents::defined, "image");
        CHECK(graph.add_pass("mode", mode_parameters<Mode>{{{{bytes, {0, 24}}, {bytes, {8, 24}}}}, {image}}, {true}));
        auto plan = graph.compile();
        CHECK(plan && plan->execution_order.size() == 1);
        if (!plan)
            return;
        CHECK(plan->passes[0].accesses.size() == 3);
        for (auto const& access : plan->passes[0].accesses)
            CHECK(access.mode == Mode);
        CHECK(plan->resources[0].last_version == (Mode == rg::content_access::read ? 0 : 1));
        CHECK(plan->resources[0].defined == (Mode != rg::content_access::invalidate));
        CHECK(plan->reads.size() == (Mode == rg::content_access::read || Mode == rg::content_access::preserve_write || Mode == rg::content_access::read_write ? 3 : 0));
        for (auto const& read : plan->reads)
            CHECK(read.producer == rg::invalid_index && read.version == 0);
    }
    struct tracked_parameters {
        rg::buffer_read read;
        rg::buffer_discard_write write;
        std::shared_ptr<int> lifetime;
        void visit_resources(rg::parameter_visitor& visit) const noexcept {
            visit(read, write);
        }
    };
    void rejection_lifetime_tests() {
        rg::builder graph;
        auto bytes = *graph.import_buffer({3, 1}, rg::initial_contents::defined, "bytes", {32});
        auto token = std::make_shared<int>(1);
        std::weak_ptr<int> observer = token;
        auto result = graph.add_pass("conflict", tracked_parameters{{bytes, {0, 24}}, {bytes, {16, 16}}, std::move(token)});
        CHECK(!result && result.error().code == rg::error_code::conflicting_access);
        CHECK(observer.expired());
        CHECK(graph.add_pass("valid", optional_parameters{}, {true})->index() == 0);
        auto plan = graph.compile();
        CHECK(plan && plan->passes.size() == 1);
    }
} // namespace
int main(int argc, char** argv) {
    if (argc > 1) {
        vessel_prepare_child();
        rg::builder graph;
        utility_memory_test_fail_after(0);
        graph.add_pass("oom snapshot", optional_parameters{}, {true});
        return 99;
    }
    snapshot_tests();
    validation_tests();
    empty_and_legacy_tests();
    mode_tests<rg::content_access::read>();
    mode_tests<rg::content_access::discard_write>();
    mode_tests<rg::content_access::preserve_write>();
    mode_tests<rg::content_access::read_write>();
    mode_tests<rg::content_access::invalidate>();
    rejection_lifetime_tests();
    std::remove("deren-crash.log");
    auto oom = vessel_run_child(argv[0], "--oom", "render-graph-parameters-oom.log");
    CHECK(oom.started && oom.code != 0 && oom.code != 99);
    auto* log = std::fopen("deren-crash.log", "rb");
    CHECK(log);
    if (log) {
        char text[1024]{};
        auto count = std::fread(text, 1, sizeof(text), log);
        std::fclose(log);
        CHECK(std::string_view(text, count).find("allocation failure: out_of_memory") != std::string_view::npos);
        CHECK(std::string_view(text, count).find("stacktrace") == std::string_view::npos);
    }
    auto* console = std::fopen("render-graph-parameters-oom.log", "rb");
    CHECK(console);
    if (console) {
        char text[1024]{};
        auto count = std::fread(text, 1, sizeof(text), console);
        std::fclose(console);
        CHECK(std::string_view(text, count).find("deren ") == std::string_view::npos);
    }
    return deren::vk_test::finish("test_render_graph_parameters");
}
