#include "vk_test.h"

#include <array>
#include <limits>
#include <variant>
#include <vulkan/vulkan.h>
import deren.engine.render_graph.rhi;
import deren.vulkan.synchronization;
namespace rg = deren::engine::render_graph;
namespace bridge = deren::engine::render_graph::rhi_bridge;
namespace rhi_sync = deren::promise::rhi::synchronization;
namespace native = deren::vulkan::synchronization;
namespace {
    constexpr rhi_sync::scope compute_write{rhi_sync::stage::compute_shader, rhi_sync::access::write};
    constexpr rhi_sync::scope fragment_read{rhi_sync::stage::fragment_shader, rhi_sync::access::read};
    void image_tests() {
        constexpr std::array ordinary{rhi_sync::image_layout::general, rhi_sync::image_layout::shader_read_only,
                                      rhi_sync::image_layout::color_attachment, rhi_sync::image_layout::transfer_source, rhi_sync::image_layout::transfer_destination};
        for (auto before : ordinary)
            for (auto after : ordinary) {
                rhi_sync::image_transition input{{before, compute_write}, {after, fragment_read}, {1, 2, 3, 4, rhi_sync::aspect::color}};
                auto mapped = native::map_image(input);
                CHECK(mapped);
                if (!mapped)
                    continue;
                CHECK(mapped->oldLayout == VK_IMAGE_LAYOUT_GENERAL && mapped->newLayout == VK_IMAGE_LAYOUT_GENERAL);
                CHECK(mapped->srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT && mapped->dstStageMask == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
                CHECK(mapped->srcAccessMask == VK_ACCESS_2_MEMORY_WRITE_BIT && mapped->dstAccessMask == VK_ACCESS_2_MEMORY_READ_BIT);
                CHECK(mapped->subresourceRange.baseMipLevel == 1 && mapped->subresourceRange.levelCount == 2 && mapped->subresourceRange.baseArrayLayer == 3 && mapped->subresourceRange.layerCount == 4);
                CHECK(mapped->image == VK_NULL_HANDLE && mapped->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && mapped->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
            }
        rhi_sync::image_transition fresh{{}, {rhi_sync::image_layout::color_attachment, {rhi_sync::stage::color_output, rhi_sync::access::write}}, {0, 1, 0, 1, rhi_sync::aspect::color}};
        auto mapped = native::map_image(fresh);
        CHECK(mapped && mapped->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && mapped->newLayout == VK_IMAGE_LAYOUT_GENERAL);
        fresh.before = fresh.after;
        fresh.after = {rhi_sync::image_layout::present, {}};
        mapped = native::map_image(fresh);
        CHECK(mapped && mapped->newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR && mapped->dstStageMask == VK_PIPELINE_STAGE_2_NONE);
        fresh.after = {};
        CHECK(!native::map_image(fresh));
        fresh.after = {rhi_sync::image_layout::general, fragment_read};
        fresh.range.aspects = rhi_sync::aspect::stencil;
        fresh.before = {rhi_sync::image_layout::depth_stencil_attachment, {rhi_sync::stage::depth_stencil, rhi_sync::access::write}};
        mapped = native::map_image(fresh);
        CHECK(mapped && mapped->subresourceRange.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT);
        CHECK(mapped->srcStageMask == (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT));
        fresh.range.aspects = rhi_sync::aspect::depth | rhi_sync::aspect::stencil;
        CHECK(native::map_image(fresh)->subresourceRange.aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
        fresh.range.mip_count = 0;
        CHECK(!native::map_image(fresh));
        fresh.range.mip_count = 1;
        fresh.before.scope.stages = static_cast<rhi_sync::stage>(1024);
        CHECK(!native::map_image(fresh));
        fresh.before.scope = compute_write;
        fresh.range.base_mip = UINT32_MAX;
        CHECK(!native::map_image(fresh));
        fresh.range.base_mip = 0;
        fresh.range.mip_count = UINT32_MAX;
        CHECK(!native::map_image(fresh));
        fresh.range.mip_count = 1;
        fresh.range.aspects = rhi_sync::aspect::color | rhi_sync::aspect::depth;
        CHECK(!native::map_image(fresh));
        fresh.range.aspects = rhi_sync::aspect::depth;
        fresh.after.layout = static_cast<rhi_sync::image_layout>(999);
        CHECK(!native::map_image(fresh));
    }
    void buffer_tests() {
        rhi_sync::buffer_transition input{compute_write, {rhi_sync::stage::vertex_input | rhi_sync::stage::draw_indirect | rhi_sync::stage::fragment_shader, rhi_sync::access::read}, 8, 24};
        auto mapped = native::map_buffer(input);
        CHECK(mapped);
        CHECK(mapped->srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        CHECK(mapped->dstStageMask == (VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT));
        CHECK(mapped->srcAccessMask == VK_ACCESS_2_MEMORY_WRITE_BIT && mapped->dstAccessMask == VK_ACCESS_2_MEMORY_READ_BIT);
        CHECK(mapped->offset == 8 && mapped->size == 24 && mapped->buffer == VK_NULL_HANDLE);
        input.after = {rhi_sync::stage::fragment_shader, rhi_sync::access::read | rhi_sync::access::write};
        CHECK(native::map_buffer(input)->dstAccessMask == (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT));
        input.offset = UINT64_MAX;
        CHECK(!native::map_buffer(input));
        input.offset = 8;
        input.size = 0;
        CHECK(!native::map_buffer(input));
        input.offset = 0;
        input.size = UINT64_MAX;
        CHECK(!native::map_buffer(input));
        input.size = 16;
        input.before.stages = rhi_sync::stage::none;
        CHECK(!native::map_buffer(input));
        input.before = {rhi_sync::stage::compute_shader, static_cast<rhi_sync::access>(4)};
        CHECK(!native::map_buffer(input));
        input.before = compute_write;
        input.after.stages = rhi_sync::stage::color_output;
        CHECK(!native::map_buffer(input));
        CHECK(*native::map_stages(rhi_sync::stage::vertex_shader | rhi_sync::stage::transfer) == (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT));
    }
    void graph_tests() {
        rg::builder graph;
        auto image = *graph.import_texture({1, 1}, rg::initial_contents::defined, "image", {}, rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::compute_shader, rg::memory_access::write});
        rg::resource_access read[] = {{image, rg::content_access::read, {}, {rg::resource_usage::sampled, rg::pipeline_stage::fragment_shader}}};
        CHECK(graph.add_pass("sample", read, {true}));
        auto plan = *graph.compile({true});
        CHECK(plan.barriers[0].after.layout == rg::resource_layout::shader_read_only);
        auto lowered = bridge::lower_scopes(plan);
        CHECK(lowered && lowered->barriers.size() == 1);
        auto const& packet = lowered->barriers[0];
        CHECK(packet.resource == 0 && packet.before_pass == 0);
        auto const& transition = std::get<rhi_sync::image_transition>(packet.transition);
        CHECK(transition.after.layout == rhi_sync::image_layout::shader_read_only);
        auto actual = native::map_image(transition);
        CHECK(actual && actual->oldLayout == VK_IMAGE_LAYOUT_GENERAL && actual->newLayout == VK_IMAGE_LAYOUT_GENERAL);
        CHECK(actual->srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT && actual->dstStageMask == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        CHECK(actual->srcAccessMask == VK_ACCESS_2_MEMORY_WRITE_BIT && actual->dstAccessMask == VK_ACCESS_2_MEMORY_READ_BIT);
        CHECK(plan.final_states[0].state.layout == rg::resource_layout::shader_read_only);
        CHECK(plan.barriers[0].after.layout == rg::resource_layout::shader_read_only);
        auto text = rg::dump_text(plan);
        CHECK(text.find("shader_read_only") != text.npos);
        plan.synchronization_planned = false;
        CHECK(!bridge::lower_scopes(plan));
        plan.synchronization_planned = true;
        plan.barriers[0].resource = 999;
        auto failed = bridge::lower_scopes(plan);
        CHECK(!failed && failed.error().barrier == 0 && failed.error().resource == 999);
    }
    void graph_buffer_tests() {
        rg::builder graph;
        auto buffer = *graph.import_buffer({2, 1}, rg::initial_contents::undefined, "buffer", {64}, rg::resource_state{});
        rg::resource_access write[] = {{buffer, rg::content_access::discard_write, {}, {rg::resource_usage::storage, rg::pipeline_stage::compute_shader}}};
        rg::resource_access vertex[] = {{buffer, rg::content_access::read, {8, 16}, {rg::resource_usage::vertex_buffer, rg::pipeline_stage::vertex_input}}};
        rg::resource_access indirect[] = {{buffer, rg::content_access::read, {8, 16}, {rg::resource_usage::indirect_buffer, rg::pipeline_stage::draw_indirect}}};
        CHECK(graph.add_pass("write", write, {true}));
        CHECK(graph.add_pass("vertices", vertex, {true}));
        CHECK(graph.add_pass("indirect", indirect, {true}));
        auto plan = *graph.compile({true});
        auto scopes = bridge::lower_scopes(plan);
        CHECK(scopes && scopes->barriers.size() == plan.barriers.size() && scopes->barriers.size() == 2);
        for (auto const& packet : scopes->barriers) {
            auto value = native::map_buffer(std::get<rhi_sync::buffer_transition>(packet.transition));
            CHECK(value && value->offset == 8 && value->size == 16 && (value->srcAccessMask & VK_ACCESS_2_MEMORY_WRITE_BIT));
        }
        plan.barriers[0].range = rg::buffer_range{63, 2};
        CHECK(!bridge::lower_scopes(plan));
        plan.barriers[0].range = rg::buffer_range{8, 16};
        plan.barriers[0].after.layout = rg::resource_layout::shader_read_only;
        CHECK(!bridge::lower_scopes(plan));
        plan.barriers[0].after.layout = rg::resource_layout::general;
        std::swap(plan.barriers[0], plan.barriers[1]);
        CHECK(!bridge::lower_scopes(plan));
    }
    void readonly_transition_tests() {
        rg::builder graph;
        auto image = *graph.import_texture({3, 1}, rg::initial_contents::defined, "input", {}, rg::resource_state{rg::resource_layout::shader_read_only, rg::pipeline_stage::fragment_shader, rg::memory_access::read});
        rg::resource_access read[] = {{image, rg::content_access::read, {}, {rg::resource_usage::storage, rg::pipeline_stage::compute_shader}}};
        CHECK(graph.add_pass("abstract transition", read, {true}));
        auto plan = *graph.compile({true});
        auto scopes = *bridge::lower_scopes(plan);
        CHECK(scopes.barriers.size() == 1);
        auto mapped = native::map_image(std::get<rhi_sync::image_transition>(scopes.barriers[0].transition));
        CHECK(mapped && mapped->oldLayout == mapped->newLayout && mapped->oldLayout == VK_IMAGE_LAYOUT_GENERAL);
        CHECK(mapped->srcAccessMask == VK_ACCESS_2_MEMORY_READ_BIT && mapped->dstAccessMask == VK_ACCESS_2_MEMORY_READ_BIT);
        CHECK((mapped->dstStageMask & (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT)) == (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT));
        CHECK(plan.final_states[0].state.layout == rg::resource_layout::general);
        plan.barriers[0].range = rg::texture_range{0, 1, 0, 1, static_cast<rg::texture_aspect>(8)};
        CHECK(!bridge::lower_scopes(plan));
    }
    void mixed_frontier_tests() {
        rg::builder graph;
        auto image = *graph.import_texture({4, 1}, rg::initial_contents::undefined, "image", {}, rg::resource_state{});
        rg::resource_access write[] = {{image, rg::content_access::discard_write, {}, {rg::resource_usage::storage, rg::pipeline_stage::compute_shader}}};
        rg::resource_access read[] = {{image, rg::content_access::read, {}, {rg::resource_usage::sampled, rg::pipeline_stage::fragment_shader}}};
        rg::resource_access transfer[] = {{image, rg::content_access::discard_write, {}, {rg::resource_usage::transfer_destination, rg::pipeline_stage::transfer}}};
        CHECK(graph.add_pass("compute", write, {true}));
        CHECK(graph.add_pass("fragment", read, {true}));
        CHECK(graph.add_pass("transfer", transfer, {true}));
        auto plan = *graph.compile({true});
        auto scopes = *bridge::lower_scopes(plan);
        CHECK(scopes.barriers.size() == 3 && scopes.barriers.back().before_pass == 2);
        auto value = native::map_image(std::get<rhi_sync::image_transition>(scopes.barriers.back().transition));
        CHECK(value);
        CHECK(value->srcStageMask == (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT));
        CHECK(value->srcAccessMask == (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT));
        CHECK(value->dstStageMask == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT && value->dstAccessMask == VK_ACCESS_2_MEMORY_WRITE_BIT);
        CHECK(value->oldLayout == VK_IMAGE_LAYOUT_GENERAL && value->newLayout == VK_IMAGE_LAYOUT_GENERAL);
        CHECK(plan.final_states[0].state.layout == rg::resource_layout::transfer_destination);
    }
} // namespace
int main() {
    {
        auto mapped = native::map_stages(rhi_sync::stage::task_shader | rhi_sync::stage::mesh_shader);
        CHECK(mapped && *mapped == (VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT));
        rg::builder graph;
        auto buffer = *graph.import_buffer({19, 1}, rg::initial_contents::defined, "shared", {64},
                                           rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::compute_shader, rg::memory_access::write});
        rg::resource_access accesses[] = {{buffer, rg::content_access::read, {}, {rg::resource_usage::storage, rg::pipeline_stage::task_shader | rg::pipeline_stage::mesh_shader}}};
        CHECK(graph.add_pass("mesh read", accesses, {true}));
        auto plan = graph.compile({true});
        CHECK(plan);
        if (plan) {
            auto lowered = bridge::lower_scopes(*plan);
            CHECK(lowered && lowered->barriers.size() == 1);
            if (lowered) {
                auto barrier = native::map_buffer(std::get<rhi_sync::buffer_transition>(lowered->barriers[0].transition));
                CHECK(barrier && barrier->dstStageMask == *mapped);
            }
            auto dump = rg::dump_json(*plan);
            CHECK(dump.find("task_shader") != dump.npos && dump.find("mesh_shader") != dump.npos);
        }
    }
    image_tests();
    for (auto extra : {rg::pipeline_stage::none, rg::pipeline_stage::task_shader | rg::pipeline_stage::mesh_shader}) {
        rg::builder graph;
        auto image = *graph.import_texture({20, 1}, rg::initial_contents::defined, "sampled", {},
                                           rg::resource_state{rg::resource_layout::general, rg::pipeline_stage::compute_shader, rg::memory_access::read});
        rg::resource_access fragment[] = {{image, rg::content_access::read, {}, {rg::resource_usage::sampled, rg::pipeline_stage::fragment_shader}}};
        CHECK(graph.add_pass("fragment read", fragment, {true}));
        if (extra != rg::pipeline_stage::none) {
            rg::resource_access mesh[] = {{image, rg::content_access::read, {}, {rg::resource_usage::sampled, extra}}};
            CHECK(graph.add_pass("mesh read", mesh, {true}));
        }
        auto plan = graph.compile({true});
        CHECK(plan && plan->barriers.size() == 1);
        if (plan) {
            auto stages = static_cast<unsigned>(plan->barriers[0].after.stages);
            CHECK((stages & 768U) == static_cast<unsigned>(extra));
            auto lowered = bridge::lower_scopes(*plan);
            CHECK(lowered && native::map_image(std::get<rhi_sync::image_transition>(lowered->barriers[0].transition)));
        }
    }
    buffer_tests();
    graph_tests();
    graph_buffer_tests();
    readonly_transition_tests();
    mixed_frontier_tests();
    return deren::vk_test::finish("test_render_graph_lowering");
}
