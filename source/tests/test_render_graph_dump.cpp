#include "vk_test.h"

#include <array>
#include <limits>
#include <span>
#include <string_view>
import deren.engine.render_graph;
namespace rg = deren::engine::render_graph;
namespace {
    struct parameters {
        rg::texture_read image;
        rg::buffer_read bytes;
        void visit_resources(rg::parameter_visitor& visit) const noexcept {
            visit(image, bytes);
        }
    };
    auto sample() {
        rg::builder graph;
        char const name[] = "HDR \"line\"\n\t\\\0\xe6\xb5\x8b\xe8\xaf\x95\xff";
        auto image = *graph.import_texture({UINT64_MAX, UINT64_MAX - 1}, rg::initial_contents::defined,
                                           std::string_view(name, sizeof(name) - 1), {2, 1}, rg::resource_state{rg::resource_layout::shader_read_only, rg::pipeline_stage::fragment_shader, rg::memory_access::read});
        auto bytes = *graph.import_buffer({1, 2}, rg::initial_contents::defined, "bytes", {32}, rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::compute_shader, rg::memory_access::read});
        rg::resource_access dead[] = {{image, rg::content_access::read, {1, 1}}};
        graph.add_pass("dead", dead);
        rg::resource_access write[] = {{image, rg::content_access::discard_write, {0, 1}, {rg::resource_usage::storage, rg::pipeline_stage::compute_shader}}};
        auto producer = *graph.add_pass("producer", write);
        auto consumer = *graph.add_pass("consumer", parameters{{image, {0, 1}, {rg::resource_usage::sampled, rg::pipeline_stage::fragment_shader}}, {bytes, {8, 16}, {rg::resource_usage::uniform_buffer, rg::pipeline_stage::fragment_shader}}}, {true});
        graph.order_after(consumer, producer);
        graph.export_resource(image, {0, 1});
        return *graph.compile({true});
    }
    rg::graph_error sample_error() {
        return {rg::error_code::invalid_range, rg::invalid_index, 3, "bad\npass", "buffer", rg::buffer_range{UINT64_MAX, UINT64_MAX}};
    }
    void tests() {
        auto graph = sample();
        auto json = rg::dump_json(graph);
        CHECK(json == rg::dump_json(graph));
        CHECK(json.find("\"schema_version\":1") != json.npos);
        CHECK(json.find("18446744073709551615") != json.npos);
        CHECK(json.find("\\u0000") != json.npos);
        CHECK(json.find("\\ufffd") != json.npos);
        CHECK(json.find("not_reachable_from_outputs_or_side_effects") != json.npos);
        CHECK(json.find("\"parameters_present\":true") != json.npos);
        auto text = rg::dump_text(graph);
        CHECK(text == rg::dump_text(graph));
        CHECK(text.find("resource[0]") != text.npos && text.find("pass[2]") != text.npos);
        CHECK(text.find("barrier[0]") != text.npos && text.find("final_state[0]") != text.npos);
        CHECK(text.find("execution_order: [1,2]") != text.npos);
        auto error = sample_error();
        CHECK(rg::dump_json(error).find("invalid_range") != json.npos);
        CHECK(rg::dump_text(error).find("bad\\npass") != text.npos);
        CHECK(!rg::dump_json(rg::compiled_graph{}).empty());
        rg::builder unused;
        CHECK(unused.import_buffer({1, 1}, rg::initial_contents::defined, "unused"));
        auto unused_plan = *unused.compile();
        CHECK(rg::dump_json(unused_plan).find("\"use_interval\":null") != json.npos);
        CHECK(rg::dump_json(rg::graph_error{static_cast<rg::error_code>(999), rg::invalid_index, rg::invalid_index, {}, {}, {}}).find("unknown") != json.npos);
        // Diagnostics must not dereference foreign indices in a user-modified result.
        graph.execution_order.push_back(999);
        graph.passes[1].accesses[0].resource = 999;
        CHECK(!rg::dump_json(graph).empty());
    }
} // namespace
int main(int argc, char** argv) {
    if (argc > 1) {
        deren::utility::vessel::string output;
        auto mode = std::string_view(argv[1]);
        if (mode == "--sample-json")
            output = rg::dump_json(sample());
        else if (mode == "--sample-text")
            output = rg::dump_text(sample());
        else if (mode == "--error-json")
            output = rg::dump_json(sample_error());
        else if (mode == "--empty-json")
            output = rg::dump_json(rg::compiled_graph{});
        else if (mode == "--unicode-json") {
            auto error = sample_error();
            error.resource_name.clear();
            for (unsigned i = 0; i < 256; ++i)
                error.resource_name.push_back(static_cast<char>(i));
            error.resource_name.append("\xc2\xa2\xe6\xb5\x8b\xf0\x90\x80\x80\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80\xe2");
            output = rg::dump_json(error);
        } else
            return 2;
        std::fwrite(output.data(), 1, output.size(), stdout);
        return 0;
    }
    tests();
    return deren::vk_test::finish("test_render_graph_dump");
}
