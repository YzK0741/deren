#include "vk_test.h"

#include <array>
#include <cstring>
#include <memory>
import deren.engine.render_graph.bloom;
import deren.engine.pass.post;
import deren.engine.pass;
import deren.engine.runtime;
import deren.engine.scene_tree;
import deren.engine.pass.chain;
import deren.engine.pass.output_targets;
import deren.engine.render_resource;
#include "probe_backend.hpp"
namespace rg = deren::engine::render_graph;
namespace bloom = rg::bloom;
namespace rhi = deren::promise::rhi;
namespace pass = deren::engine::pass;
struct test_pipeline final : rhi::pipeline {
    test_pipeline()
        : rhi::pipeline("bloom test") {
    }
    void release() noexcept override {
    }
};
struct test_composite final : pass::frame_pass {
    test_pipeline pipeline;
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
    pass::owned_pipeline named_pipeline(std::string_view name) const noexcept override {
        return name == "post_hdr" ? pass::owned_pipeline{.contract = const_cast<test_pipeline*>(&pipeline)} : pass::owned_pipeline{};
    }
};
int main() {
    auto core = deren::vk_test::probe_make_core({}, rhi::abi_version, true);
    auto cluster = deren::engine::resource_cluster::create(core);
    rhi::image_desc desc{};
    desc.extent = {16, 16, 1};
    desc.format = rhi::image_format::r16g16b16a16_sfloat;
    desc.flags = rhi::to_bits(rhi::image_flag::sampled) | rhi::to_bits(rhi::image_flag::color_attachment);
    auto input = *cluster->create_image(desc);
    std::array<deren::engine::resource_lease<rhi::image>, 4> levels;
    std::array<deren::engine::resource_lease<rhi::image_view>, 4> views;
    bloom::description description{};
    description.input = rg::execution::identify(input);
    description.input_state = {rg::resource_layout::shader_read_only, rg::pipeline_stage::fragment_shader, rg::memory_access::read};
    for (unsigned i = 0; i < 4; ++i) {
        desc.extent.width = desc.extent.height = 8U >> i;
        levels[i] = *cluster->create_image(desc);
        views[i] = *cluster->create_image_view(levels[i], {});
        description.levels[i] = rg::execution::identify(levels[i]);
    }
    auto graph = bloom::compile(description);
    CHECK(graph);
    if (!graph)
        return deren::vk_test::finish("test_render_graph_bloom");
    CHECK(graph->resources.size() == 5 && graph->execution_order.size() == 5 && graph->outputs.size() == 4);
    CHECK(graph->barriers.back().before_pass == 4);
    for (auto const& state : graph->final_states)
        CHECK(state.state.layout == rg::resource_layout::shader_read_only);
    auto reused = description;
    for (auto& state : reused.level_states)
        state = {rg::resource_layout::shader_read_only, rg::pipeline_stage::fragment_shader | rg::pipeline_stage::color_output, rg::memory_access::read | rg::memory_access::write};
    auto reuse_plan = bloom::compile(reused);
    CHECK(reuse_plan);
    if (reuse_plan) {
        auto const& barrier = reuse_plan->barriers.front();
        CHECK(barrier.before.layout == rg::resource_layout::shader_read_only);
        CHECK(barrier.before.stages == (rg::pipeline_stage::fragment_shader | rg::pipeline_stage::color_output));
        CHECK(barrier.before.access == (rg::memory_access::read | rg::memory_access::write));
    }
    auto aliased = description;
    aliased.levels[1] = aliased.levels[0];
    CHECK(!bloom::compile(aliased));
    auto prepared = bloom::prepared_chain::prepare(core, input, levels, description.input_state, description.level_states);
    CHECK(prepared);
    if (!prepared)
        return deren::vk_test::finish("test_render_graph_bloom");
    (void)core->frame_begin();
    auto commands = core->begin_commands();
    auto command_owner = std::shared_ptr<rhi::command_buffer>(core, commands);
    test_pipeline pipeline;
    std::array<rhi::pipeline*, 1> pipelines{&pipeline};
    unsigned calls = 0;
    auto result = prepared->record(*commands, [&](unsigned level, rg::execution::record_context const& context) noexcept {
        CHECK(level == calls && context.image(level + 1) == levels[level].get());
        pass::post_bloom_pass stage(level);
        pass::resolved_io io{};
        std::array<pass::resolved_binding, 1> target{{{.image_handle = levels[level].get(), .view_handle = views[level].get()}}};
        io.cmd = command_owner;
        io.list = commands;
        io.targets = target;
        io.pipelines = pipelines;
        io.extent = {8U >> level, 8U >> level, 1};
        io.push_block = {&calls, [](void* owner, rhi::command_buffer&, std::span<std::byte const> bytes, uint32_t lane) {
                             auto& count = *static_cast<unsigned*>(owner);
                             pass::post_push_constants push{};
                             CHECK(bytes.size() == sizeof(push));
                             std::memcpy(&push, bytes.data(), sizeof(push));
                             CHECK(lane == count && push.mode == (lane == 0 ? 0.0f : 1.0f));
                             ++count;
                             return true;
                         }};
        return stage.record_content(io);
    });
    CHECK(result == rhi::error::ok && calls == 4);
    CHECK(deren::vk_test::probe_scoped_record_count(*core) == 5);
    pass::post_bloom_pass invalid_stage(0);
    CHECK(invalid_stage.record_content({}) == rhi::error::invalid_argument);
    return deren::vk_test::finish("test_render_graph_bloom");
}
