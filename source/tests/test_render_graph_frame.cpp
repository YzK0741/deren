#include "../backends/vulkan/core/vma/resource_memory_tracker.h"
#include "vk_test.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <array>
#include <barrier>
#include <bit>
#include <cstring>
#include <fstream>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>
import deren.engine.backend_loader;
import deren.engine.render_graph.execute;
import deren.engine.visibility;
import deren.engine.texture_residency;
import deren.engine.render_layout;
#include "probe_backend.hpp"
namespace rg = deren::engine::render_graph;
namespace rhi = deren::promise::rhi;
#include "frame_checkpoint_window_contract.hpp"
#include "resource_memory_contract.hpp"
#include "texture_feedback_contract.hpp"
int main(int argc, char** argv) {
    check_resource_memory_tracker();
    if (argc > 1 && std::string_view(argv[1]) == "--with-device") {
        check_window_checkpoints();
        rhi::create_info options{};
        options.window_width = options.window_height = 16;
        options.validation_layers = true;
        auto core = deren::engine::load_api_core(options);
        CHECK(core);
        if (!core)
            return deren::vk_test::finish("test_render_graph_frame");
        auto* checkpoint = rhi::query_extension<rhi::frame_checkpoint>(*core);
        CHECK(checkpoint);
        if (!checkpoint)
            return deren::vk_test::finish("test_render_graph_frame");
        CHECK(checkpoint->checkpoint_and_wait() == rhi::error::not_ready);
        check_resource_memory_device(*core);
        check_texture_feedback(*core);
        (void)core->frames->frame_begin();
        auto frames = rhi::query_extension<rhi::frame_resources>(*core);
        auto snapshot = frames ? frames->retain_image() : rhi::shared_object<rhi::image>{};
        auto* commands = core->frames->begin_commands();
        CHECK(snapshot && commands);
        if (snapshot && commands) {
            CHECK(commands->begin_recording({}) == rhi::error::ok);
            std::array<std::byte, 4> initial{std::byte{37}};
            auto upload = core->resources->create_buffer({.size = 4, .usage = rhi::buffer_usage::staging_source, .initial_bytes = initial});
            auto readback = core->resources->create_buffer({.size = 4, .usage = rhi::buffer_usage::readback_coherent});
            CHECK(upload && readback);
            if (upload && readback) {
                CHECK(commands->copy_buffer(*readback, *upload, 4, 0, 0) == rhi::error::ok);
                rhi::buffer_barrier host{.resource = readback.get(), .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::host_read};
                CHECK(commands->barrier({.buffers = std::span(&host, 1)}) == rhi::error::ok);
                CHECK(checkpoint->checkpoint_and_wait() == rhi::error::ok);
                CHECK(readback->mapped()[0] == std::byte{37});
                CHECK(core->frames->begin_commands() == commands);
                CHECK(frames->retain_image().get() == snapshot.get());
                CHECK(checkpoint->checkpoint_and_wait() == rhi::error::ok);
            }
            CHECK(commands->barrier(rhi::image_barrier{.resource = snapshot.get(), .from = rhi::image_use::undefined, .to = rhi::image_use::color_attachment}) == rhi::error::ok);
            CHECK(commands->barrier(rhi::image_barrier{.resource = snapshot.get(), .from = rhi::image_use::color_attachment, .to = rhi::image_use::color_attachment}) == rhi::error::ok);
            auto target_view = snapshot->make_view({.role = rhi::view_role::color_attachment});
            CHECK(target_view);
            if (target_view) {
                rhi::color_attachment attachment{.view = target_view.get()};
                rhi::rendering_info rendering{};
                rendering.area = {0, 0, 16, 16};
                rendering.colors = std::span(&attachment, 1);
                CHECK(commands->begin_rendering(rendering) == rhi::error::ok);
                CHECK(checkpoint->checkpoint_and_wait() == rhi::error::not_ready);
                commands->end_rendering();
            }
            auto reusable = core->commands->create_command_buffer({});
            CHECK(reusable);
            if (reusable) {
                CHECK(reusable->begin_recording({}) == rhi::error::ok);
                CHECK(reusable->barrier(rhi::image_barrier{.resource = snapshot.get(), .from = rhi::image_use::color_attachment, .to = rhi::image_use::color_attachment}) == rhi::error::invalid_argument);
                CHECK(reusable->end_recording() == rhi::error::ok);
            }
            CHECK(commands->end_recording() == rhi::error::ok);
            CHECK(core->commands->submit(*commands) == rhi::error::invalid_argument);
            CHECK(core->frames->submit() == rhi::error::ok);
            core->wait_idle();
        }
        return deren::vk_test::finish("test_render_graph_frame");
    }
    auto core = deren::vk_test::probe_make_core({}, rhi::abi_version, true);
    CHECK(!core->resources->memory_statistics().supported);
    auto frames = rhi::query_extension<rhi::frame_resources>(*core);
    CHECK(frames);
    if (!frames)
        return deren::vk_test::finish("test_render_graph_frame");
    CHECK(!frames->retain_image());
    (void)core->frames->frame_begin();
    auto shared = frames->retain_image();
    CHECK(shared && shared.get() != core->frames->frame_image());
    CHECK(frames->retain_image().get() == shared.get());
    auto image = deren::engine::resource_lease<rhi::image>::from_shared(core, shared);
    auto identity = rg::execution::identify(image);
    CHECK(identity.object != 0);
    rg::builder graph;
    auto ref = *graph.import_texture(identity, rg::initial_contents::undefined, "frame", {}, rg::resource_state{});
    std::array<rg::resource_access, 1> write{{{ref, rg::content_access::discard_write, {}, {rg::resource_usage::color_attachment, rg::pipeline_stage::color_output}}}};
    std::array<rg::resource_access, 1> present{{{ref, rg::content_access::read, {}, {rg::resource_usage::present, rg::pipeline_stage::none}}}};
    CHECK(graph.add_pass("draw", write));
    CHECK(graph.add_pass("present", present, {true}));
    auto plan = graph.compile({true});
    CHECK(plan);
    if (!plan)
        return deren::vk_test::finish("test_render_graph_frame");
    CHECK(plan->barriers.back().after.layout == rg::resource_layout::present);
    CHECK(plan->barriers.back().after.access == rg::memory_access::none);
    std::array<rg::execution::resource_binding, 1> bindings{image};
    auto ready = rg::execution::prepared_graph::prepare(core, *plan, bindings);
    CHECK(ready);
    unsigned calls = 0;
    auto callback = [&](rg::execution::record_context const&) noexcept { ++calls; return rhi::error::ok; };
    if (ready)
        CHECK(ready->record(*core->frames->begin_commands(), callback) == rhi::error::ok);
    CHECK(calls == 2);
    if (ready) {
        rg::execution::prepared_graph prefix, suffix;
        CHECK(ready->partition_before(1, prefix, suffix) == rhi::error::ok);
        calls = 0;
        CHECK(prefix.record(*core->frames->begin_commands(), callback) == rhi::error::ok);
        CHECK(calls == 1);
        CHECK(suffix.record(*core->frames->begin_commands(), callback) == rhi::error::ok);
        CHECK(calls == 2);
        CHECK(ready->partition_before(UINT32_MAX, prefix, suffix) == rhi::error::invalid_argument);
    }
    CHECK(core->commands->submit(*core->frames->begin_commands()) == rhi::error::invalid_argument);
    CHECK(core->frames->submit() == rhi::error::ok);
    (void)core->frames->frame_begin();
    auto next = frames->retain_image();
    CHECK(next && next != shared);
    calls = 0;
    if (ready)
        CHECK(ready->record(*core->frames->begin_commands(), callback) != rhi::error::ok);
    CHECK(calls == 0);
    CHECK(!rg::execution::prepared_graph::prepare(core, *plan, bindings));
    // A read-only pass can require no barriers. Identity checks must still reject
    // a snapshot invalidated by an earlier callback in the same recording.
    auto next_lease = deren::engine::resource_lease<rhi::image>::from_shared(core, next);
    rg::builder reads;
    auto current = *reads.import_texture(rg::execution::identify(next_lease), rg::initial_contents::defined, "current", {},
                                         rg::resource_state{rg::resource_layout::present, rg::pipeline_stage::none, rg::memory_access::none});
    present[0] = {current, rg::content_access::read, {}, {rg::resource_usage::present, rg::pipeline_stage::none}};
    CHECK(reads.add_pass("first", present, {true}));
    CHECK(reads.add_pass("second", present, {true}));
    auto read_plan = reads.compile({true});
    CHECK(read_plan && read_plan->barriers.empty());
    if (read_plan) {
        bindings[0] = next_lease;
        auto read_ready = rg::execution::prepared_graph::prepare(core, *read_plan, bindings);
        CHECK(read_ready);
        calls = 0;
        if (read_ready) {
            auto error = read_ready->record(*core->frames->begin_commands(), [&](rg::execution::record_context const&) noexcept {
                ++calls;
                (void)core->frames->frame_begin();
                return rhi::error::ok;
            });
            CHECK(error == rhi::error::not_ready && calls == 1);
        }
    }
    return deren::vk_test::finish("test_render_graph_frame");
}
