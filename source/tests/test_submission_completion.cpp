#include "vk_test.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <thread>
#include <vulkan/vulkan.h>
import deren.promise.rhi;
import deren.engine.backend_loader;
import deren.vulkan.main_context;
namespace rhi = deren::promise::rhi;

#include "queue_gate.hpp"

int main(int argc, char** argv) {
    bool with_device = false;
    for (int i = 1; i < argc; ++i)
        with_device |= std::string_view(argv[i]) == "--with-device";
    if (!with_device)
        return deren::vk_test::skip("test_submission_completion", "--with-device was not passed: the device lane did not run");
    rhi::create_info creation{};
    creation.window_width = 64;
    creation.window_height = 64;
    creation.window_visible = false;
    creation.validation_layers = true;
    auto core = deren::engine::load_api_core(creation);
    CHECK(core);
    if (!core)
        return deren::vk_test::finish("test_submission_completion");
    auto* submission = rhi::query_extension<rhi::command_submission>(*core);
    CHECK(submission);
    auto* context = core->main_context();
    CHECK(context && deren::vulkan::is_main_context(*context));
    if (!submission || !context || !deren::vulkan::is_main_context(*context))
        return deren::vk_test::finish("test_submission_completion");
    auto& native = static_cast<deren::vulkan::main_context&>(*context);
    CHECK(submission->poll({}) == rhi::error::invalid_argument);
    CHECK(submission->wait({}, 0) == rhi::error::invalid_argument);
    auto empty = submission->submit(std::span<rhi::mergeable_command_buffer_ref const>{});
    CHECK(!empty && empty.error() == rhi::error::invalid_argument);
    auto commands = core->commands->make_command_buffer({});
    CHECK(commands);
    if (!commands)
        return deren::vk_test::finish("test_submission_completion");
    auto unrecorded = submission->submit(*commands);
    CHECK(!unrecorded && unrecorded.error() == rhi::error::not_ready);
    std::uint32_t const value = 0x1234abcdu;
    rhi::unique_object<rhi::buffer> source{core->resources->create_buffer({.size = 4, .usage = rhi::buffer_usage::storage_coherent, .flags = rhi::to_bits(rhi::buffer_flag::transfer_source), .initial_bytes = std::as_bytes(std::span(&value, 1))})};
    rhi::unique_object<rhi::buffer> destination{core->resources->create_buffer({.size = 12, .usage = rhi::buffer_usage::readback_coherent})};
    CHECK(source && destination);
    if (!source || !destination)
        return deren::vk_test::finish("test_submission_completion");
    auto staging = core->resources->create_buffer({.size = 4, .usage = rhi::buffer_usage::staging_source});
    auto uniform = core->resources->create_buffer({.size = 4, .usage = rhi::buffer_usage::uniform_coherent, .flags = rhi::to_bits(rhi::buffer_flag::transfer_source)});
    auto cached = core->resources->create_buffer({.size = 4, .usage = rhi::buffer_usage::uniform_cached, .flags = rhi::to_bits(rhi::buffer_flag::transfer_destination)});
    CHECK(staging && uniform && cached);
    if (!staging || !uniform || !cached)
        return deren::vk_test::finish("test_submission_completion");
    // Mutate after factory return, with no explicit flush. Initial upload behavior
    // cannot mask missing coherence on the exposed persistent mapping.
    std::uint32_t const mapped_values[] = {0xabcddcbau, 0x98765432u, 0x13572468u};
    rhi::buffer* uploads[] = {source.get(), staging.get(), uniform.get()};
    for (std::size_t i = 0; i < std::size(uploads); ++i) {
        auto bytes = uploads[i]->mapped();
        CHECK(bytes.size() == 4);
        if (bytes.size() != 4)
            return deren::vk_test::finish("test_submission_completion");
        std::memcpy(bytes.data(), &mapped_values[i], 4);
    }
    CHECK(commands->begin_recording({}) == rhi::error::ok);
    CHECK(commands->copy_buffer(*destination, *source, 4, 0, 0) == rhi::error::ok);
    CHECK(commands->copy_buffer(*destination, *staging, 4, 0, 4) == rhi::error::ok);
    CHECK(commands->copy_buffer(*destination, *uniform, 4, 0, 8) == rhi::error::ok);
    CHECK(commands->copy_buffer(*cached, *staging, 4, 0, 0) == rhi::error::ok);
    rhi::buffer_barrier const host_barrier{.resource = destination.get(), .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::host_read};
    rhi::barrier_group const barriers{.buffers = std::span(&host_barrier, 1)};
    CHECK(commands->barrier(barriers) == rhi::error::ok);
    rhi::buffer_barrier const cached_barrier{.resource = cached.get(), .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::host_read};
    CHECK(commands->barrier(rhi::barrier_group{.buffers = std::span(&cached_barrier, 1)}) == rhi::error::ok);
    CHECK(commands->end_recording() == rhi::error::ok);
    rhi::submission_token saved{};
    {
        queue_gate gate(native.device, native.graphics_queue);
        auto submitted = submission->submit(*commands);
        CHECK(submitted);
        if (submitted) {
            saved = *submitted;
            CHECK(saved.device_id != 0 && saved.sequence == 1);
            CHECK(submission->poll(saved) == rhi::error::not_ready);
            CHECK(submission->wait(saved, 0) == rhi::error::timeout);
            CHECK(submission->wait(saved, 1000000) == rhi::error::timeout);
            CHECK(commands->begin_recording({}) == rhi::error::not_ready);
            auto duplicate = submission->submit(*commands);
            CHECK(!duplicate && duplicate.error() == rhi::error::not_ready);
            CHECK(submission->poll({saved.device_id, saved.sequence + 1}) == rhi::error::invalid_argument);
            CHECK(submission->poll({saved.device_id + 1, saved.sequence}) == rhi::error::invalid_argument);
            // A blocking waiter must not prevent another thread from submitting a new command.
            std::atomic<bool> started = false;
            rhi::error waited = rhi::error::not_ready;
            std::thread waiter([&] {started.store(true);waited=submission->wait(saved,2000000000); });
            while (!started.load())
                std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            auto second = core->commands->make_command_buffer({});
            CHECK(second);
            if (second) {
                CHECK(second->begin_recording({}) == rhi::error::ok);
                CHECK(second->end_recording() == rhi::error::ok);
                auto next = submission->submit(*second);
                CHECK(next && next->sequence == saved.sequence + 1);
            }
            gate.release();
            waiter.join();
            CHECK(waited == rhi::error::ok);
            CHECK(submission->poll(saved) == rhi::error::ok);
            std::uint32_t observed = 0;
            std::memcpy(&observed, destination->mapped().data(), 4);
            CHECK(observed == mapped_values[0]);
            for (std::size_t i = 1; i < std::size(mapped_values); ++i) {
                std::memcpy(&observed, destination->mapped().data() + i * 4, 4);
                CHECK(observed == mapped_values[i]);
            }
            CHECK(cached->mapped().size() == 4);
            if (cached->mapped().size() == 4) {
                std::memcpy(&observed, cached->mapped().data(), 4);
                CHECK(observed == mapped_values[1]);
            }
        }
        gate.release();
        core->wait_idle();
    }
    // The pending record has been collected. Keep only the receipt and query it repeatedly.
    commands.reset();
    CHECK(submission->poll(saved) == rhi::error::ok);
    CHECK(submission->wait(saved, 0) == rhi::error::ok);
    auto segment = core->commands->make_mergeable_command_buffer({});
    CHECK(segment);
    if (segment) {
        CHECK(segment->begin_recording({}) == rhi::error::ok);
        CHECK(segment->end_recording() == rhi::error::ok);
        queue_gate gate(native.device, native.graphics_queue);
        rhi::mergeable_command_buffer_ref batch[] = {segment};
        auto result = submission->submit(batch);
        CHECK(result);
        segment.reset();
        batch[0].reset();
        if (result) {
            CHECK(result->sequence == saved.sequence + 2);
            CHECK(submission->poll(*result) == rhi::error::not_ready);
            // Waiting for an old completed value must not wait for this later blocked submission.
            CHECK(submission->poll(saved) == rhi::error::ok);
            CHECK(submission->wait(saved, 0) == rhi::error::ok);
            gate.release();
            CHECK(submission->wait(*result, 2000000000) == rhi::error::ok);
            CHECK(submission->poll(*result) == rhi::error::ok);
        }
        gate.release();
        core->wait_idle();
    }
    // Core recreation must not recycle a device ID, even when a native address is reused.
    auto other = deren::engine::load_api_core(creation);
    rhi::submission_token retired{};
    CHECK(other);
    if (other) {
        auto* foreign = rhi::query_extension<rhi::command_submission>(*other);
        CHECK(foreign && foreign->poll(saved) == rhi::error::invalid_argument);
        auto foreign_commands = other->commands->make_command_buffer({});
        CHECK(foreign_commands);
        if (foreign_commands) {
            auto wrong = submission->submit(*foreign_commands);
            CHECK(!wrong && wrong.error() == rhi::error::invalid_argument);
            CHECK(foreign_commands->begin_recording({}) == rhi::error::ok);
            CHECK(foreign_commands->end_recording() == rhi::error::ok);
            if (foreign) {
                auto other_token = foreign->submit(*foreign_commands);
                CHECK(other_token && other_token->device_id != saved.device_id);
                if (other_token) {
                    retired = *other_token;
                    CHECK(foreign->wait(*other_token, 2000000000) == rhi::error::ok);
                    CHECK(submission->poll(*other_token) == rhi::error::invalid_argument);
                }
            }
        }
    }
    other.reset();
    auto replacement = deren::engine::load_api_core(creation);
    CHECK(replacement);
    if (replacement) {
        auto* fresh = rhi::query_extension<rhi::command_submission>(*replacement);
        CHECK(fresh && fresh->poll(retired) == rhi::error::invalid_argument);
        auto fresh_commands = replacement->commands->make_command_buffer({});
        CHECK(fresh_commands);
        if (fresh && fresh_commands) {
            CHECK(fresh_commands->begin_recording({}) == rhi::error::ok);
            CHECK(fresh_commands->end_recording() == rhi::error::ok);
            auto fresh_token = fresh->submit(*fresh_commands);
            CHECK(fresh_token && fresh_token->device_id != retired.device_id && fresh_token->device_id != saved.device_id);
            if (fresh_token)
                CHECK(fresh->wait(*fresh_token, 2000000000) == rhi::error::ok);
        }
    }
    core->wait_idle();
    return deren::vk_test::finish("test_submission_completion");
}
