#include "vk_test.h"

#include <array>
#include <memory>
import deren.engine.render_graph.post;
import deren.engine.pass.post;
import deren.engine.pass.fxaa;
import deren.engine.pass.upscale;
import deren.engine.pass;
import deren.engine.runtime;
import deren.engine.scene_tree;
import deren.engine.pass.chain;
import deren.engine.pass.output_targets;
import deren.engine.pass.scene;
import deren.engine.render_resource;
#include "probe_backend.hpp"
namespace rg = deren::engine::render_graph;
namespace post = rg::post;
namespace rhi = deren::promise::rhi;
namespace pass = deren::engine::pass;
struct test_pipeline final : rhi::pipeline {
    test_pipeline()
        : rhi::pipeline("post test") {
    }
    void release() noexcept override {
    }
};
struct test_composite final : pass::frame_pass {
    test_pipeline pipeline;
    bool pipeline_available = true;
    bool ready() const noexcept override {
        return pipeline_available;
    }
    deren::engine::render_resource::pass_io const& io() const noexcept override {
        return deren::engine::render_resource::post_composite_io;
    }
    pass::behaviour const& behaviour() const noexcept override {
        static pass::behaviour value{};
        return value;
    }
    std::string_view feature() const noexcept override {
        return {};
    }
    void create(pass::pass_context const&) override {
    }
    void on_swapchain_recreated(pass::pass_host const&) override {
    }
    void record(pass::resolved_io const&) override {
    }
    rhi::error record_content(pass::resolved_io const&) noexcept override {
        return rhi::error::ok;
    }
    pass::owned_pipeline named_pipeline(std::string_view name) const noexcept override {
        return name == "post_hdr" ? pass::owned_pipeline{.contract = const_cast<test_pipeline*>(&pipeline)} : pass::owned_pipeline{};
    }
};
struct test_deferred final : pass::frame_pass {
    test_pipeline pipeline;
    unsigned legacy = 0, graph = 0;
    bool fail = false;
    deren::engine::render_resource::pass_io const& io() const noexcept override {
        return deren::engine::render_resource::deferred_io;
    }
    pass::behaviour const& behaviour() const noexcept override {
        static pass::behaviour value{};
        return value;
    }
    std::string_view feature() const noexcept override {
        return {};
    }
    void create(pass::pass_context const&) override {
    }
    void on_swapchain_recreated(pass::pass_host const&) override {
    }
    void record(pass::resolved_io const&) override {
        ++legacy;
    }
    rhi::error record_content(pass::resolved_io const&) noexcept override {
        ++graph;
        return fail ? rhi::error::operation_failed : rhi::error::ok;
    }
    rhi::pipeline* pipeline_handle() const noexcept override {
        return const_cast<test_pipeline*>(&pipeline);
    }
};
int main() {
    {
        rg::builder combined;
        post::inputs inputs{};
        inputs.hdr = *combined.import_texture({1, 1}, rg::initial_contents::undefined, "upstream", {}, rg::resource_state{});
        inputs.frame = *combined.import_texture({1, 2}, rg::initial_contents::undefined, "output", {}, rg::resource_state{});
        for (unsigned i = 0; i < 4; ++i)
            inputs.bloom[i] = *combined.import_texture({1, 3 + i}, rg::initial_contents::undefined, "level", {}, rg::resource_state{});
        CHECK(combined.add_pass("upstream", std::array<rg::resource_access, 1>{{{inputs.hdr, rg::content_access::discard_write, {}, {rg::resource_usage::color_attachment, rg::pipeline_stage::color_output}}}}));
        auto output = post::append(combined, inputs, {});
        CHECK(output && combined.export_resource(output->color));
        auto plan = combined.compile({true});
        CHECK(plan && plan->execution_order.size() == 6 && plan->outputs.size() == 1);
        CHECK(output && output->composite.index() == 5 && !output->resolve);
    }
    post::description desc{};
    desc.input = {1, 1};
    desc.output = {1, 2};
    desc.ldr = {1, 3};
    for (unsigned i = 0; i < 4; ++i)
        desc.levels[i] = {1, 4 + i};
    desc.lut = rg::resource_identity{1, 8};
    for (auto resolve : {post::resolve::none, post::resolve::fxaa, post::resolve::upscale}) {
        desc.resolution = resolve;
        for (bool bloom : {false, true}) {
            desc.bloom_enabled = bloom;
            auto graph = post::compile(desc);
            CHECK(graph);
            if (!graph)
                continue;
            CHECK(graph->execution_order.size() == (resolve == post::resolve::none ? 5 : 6));
            CHECK(graph->outputs.size() == 1);
            auto const* composite = graph->passes[4].parameters.get<post::composite_parameters>();
            CHECK(composite && composite->reads.size() == 6);
            for (unsigned i = 0; i < 4; ++i) {
                CHECK(bloom ? graph->passes[i].parameters.get<post::bloom_parameters>() != nullptr : graph->passes[i].parameters.get<post::clear_parameters>() != nullptr);
            }
            CHECK(graph->barriers.back().after.layout != rg::resource_layout::present);
        }
    }
    desc.output = desc.levels[0];
    CHECK(!post::compile(desc));
    deren::engine::pass::post_composite_pass composite;
    deren::engine::pass::fxaa_pass fxaa;
    deren::engine::pass::upscale_pass upscale;
    CHECK(composite.record_content({}) == rhi::error::invalid_argument);
    CHECK(fxaa.record_content({}) == rhi::error::invalid_argument);
    CHECK(upscale.record_content({}) == rhi::error::invalid_argument);
    for (bool present : {false, true}) {
        auto finish = post::compile_finish({1, 2}, present);
        CHECK(finish && finish->barriers.size() == 1);
        if (finish) {
            auto const& state = finish->barriers[0].after;
            CHECK(state.layout == (present ? rg::resource_layout::present : rg::resource_layout::shader_read_only));
            if (present)
                CHECK(state.stages == rg::pipeline_stage::none && state.access == rg::memory_access::none);
        }
    }
    return deren::vk_test::finish("test_render_graph_post");
}
