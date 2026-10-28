#include "vk_test.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <thread>
#include <vulkan/vulkan.h>
import deren.promise.rhi;
import deren.engine.buffer_transfer;
import deren.engine.backend_loader;
import deren.vulkan.main_context;
#include "../engine/visibility/feedback_readback.h"
#include "queue_gate.hpp"
namespace rhi = deren::promise::rhi;
namespace sync = rhi::synchronization;
namespace engine = deren::engine;
constexpr sync::scope transfer_read{sync::stage::transfer, sync::access::read};
int main(int argc, char** argv) {
    engine::buffer_transfer_ticket empty;
    CHECK(empty.poll() == rhi::error::invalid_argument);
    CHECK(!empty.readback(0));
    engine::buffer_transfer_service unavailable({});
    auto unavailable_batch = unavailable.make_batch();
    CHECK(unavailable_batch.upload({}, 0, {}, {}, transfer_read) == rhi::error::not_ready);
    CHECK(!unavailable.submit(std::move(unavailable_batch)));
    bool device = false;
    for (int i = 1; i < argc; ++i)
        device |= std::string_view(argv[i]) == "--with-device";
    if (!device)
        return deren::vk_test::finish("test_buffer_transfer (device skipped)");
    rhi::create_info desc{};
    desc.window_width = desc.window_height = 64;
    desc.window_visible = false;
    desc.validation_layers = true;
    auto core = engine::load_api_core(desc);
    CHECK(core);
    if (!core)
        return deren::vk_test::finish("test_buffer_transfer");
    auto& native = static_cast<deren::vulkan::main_context&>(*core->main_context());
    // Full mip-demand payload, two independent snapshots, then repeated zero-reset reuse.
    {
        constexpr std::uint64_t size = 65536;
        std::array<std::byte, size> zeros{}, sentinel{};
        for (std::size_t i = 0; i < size; ++i)
            sentinel[i] = std::byte((i * 17 + 3) & 255);
        auto zero = core->resources->create_buffer({.size = size, .usage = rhi::buffer_usage::staging_source, .initial_bytes = zeros});
        auto input = core->resources->create_buffer({.size = size, .usage = rhi::buffer_usage::staging_source, .initial_bytes = sentinel});
        std::array<rhi::unique_object<rhi::buffer>, 2> gpu, host;
        for (unsigned i = 0; i < 2; ++i) {
            gpu[i] = core->resources->create_buffer({.size = size, .usage = rhi::buffer_usage::storage_gpu_only, .flags = rhi::to_bits(rhi::buffer_flag::transfer_source) | rhi::to_bits(rhi::buffer_flag::transfer_destination)});
            host[i] = core->resources->create_buffer({.size = size, .usage = rhi::buffer_usage::uniform_cached, .flags = rhi::to_bits(rhi::buffer_flag::transfer_destination)});
        }
        CHECK(zero && input && gpu[0] && gpu[1] && host[0] && host[1]);
        if (zero && input && gpu[0] && gpu[1] && host[0] && host[1]) {
            for (unsigned frame = 0; frame < 4; ++frame) {
                unsigned slot = frame % 2;
                auto command = core->commands->make_command_buffer({});
                CHECK(command && command->begin_recording({}) == rhi::error::ok);
                if (!command)
                    break;
                CHECK(engine::feedback_readback::clear(*command, *gpu[slot], *zero, size) == rhi::error::ok);
                if (frame < 2) {
                    rhi::buffer_barrier before{.resource = gpu[slot].get(), .from = rhi::buffer_use::shader_write, .to = rhi::buffer_use::transfer_destination};
                    CHECK(command->barrier({.buffers = std::span(&before, 1)}) == rhi::error::ok);
                    CHECK(command->copy_buffer(*gpu[slot], *input, size, 0, 0) == rhi::error::ok);
                    rhi::buffer_barrier after{.resource = gpu[slot].get(), .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::shader_write};
                    CHECK(command->barrier({.buffers = std::span(&after, 1)}) == rhi::error::ok);
                }
                CHECK(engine::feedback_readback::publish(*command, *host[slot], *gpu[slot], size) == rhi::error::ok);
                CHECK(command->end_recording() == rhi::error::ok);
                CHECK(core->commands->submit(*command) == rhi::error::ok);
                core->wait_idle(); // test oracle only; runtime uses the completed frame-slot wait.
                auto mapped = host[slot]->mapped();
                CHECK(mapped.size() == size);
                bool matches = mapped.size() == size;
                for (std::size_t i = 0; matches && i < size; ++i)
                    matches = mapped[i] == (frame < 2 ? sentinel[i] : std::byte{});
                CHECK(matches);
                if (frame == 2)
                    CHECK(host[1]->mapped()[0] == sentinel[0]);
            }
        }
    }
    engine::buffer_transfer_service service(core);
    auto cluster = engine::resource_cluster::create(core);
    auto allocation = cluster->create_buffer({.size = 64, .usage = rhi::buffer_usage::storage_gpu_only, .flags = rhi::to_bits(rhi::buffer_flag::transfer_source) | rhi::to_bits(rhi::buffer_flag::transfer_destination)});
    CHECK(allocation);
    if (!allocation)
        return deren::vk_test::finish("test_buffer_transfer");
    auto target = *allocation;
    std::array<std::byte, 16> bytes{}, expected{};
    for (unsigned i = 0; i < bytes.size(); ++i)
        bytes[i] = expected[i] = std::byte(i + 1);
    auto batch = service.make_batch();
    CHECK(batch.upload(target, 0, {}, {}, transfer_read) == rhi::error::invalid_argument);
    CHECK(batch.upload(target, UINT64_MAX, bytes, {}, transfer_read) == rhi::error::invalid_argument);
    CHECK(batch.upload(target, 60, bytes, {}, transfer_read) == rhi::error::invalid_argument);
    CHECK(batch.upload(target, 0, bytes, {}, {}) == rhi::error::invalid_argument);
    CHECK(batch.upload(target, 8, bytes, {}, transfer_read) == rhi::error::ok);
    bytes.fill(std::byte{0xff}); // accepted input is a snapshot, not a borrowed span.
    auto index = batch.readback(target, 8, 16, transfer_read, transfer_read);
    CHECK(index && *index == 0);
    CHECK(!batch.readback(target, 63, 2, transfer_read, transfer_read));
    auto no_flags = cluster->create_buffer({.size = 16});
    CHECK(no_flags);
    if (no_flags)
        CHECK(batch.upload(*no_flags, 0, bytes, {}, transfer_read) == rhi::error::invalid_argument);
    {
        queue_gate gate(native.device, native.graphics_queue);
        auto submitted = service.submit(std::move(batch));
        CHECK(submitted);
        if (submitted) {
            CHECK(submitted->receipt().sequence == 1);
            CHECK(submitted->poll() == rhi::error::not_ready);
            CHECK(submitted->wait(0) == rhi::error::timeout);
            CHECK(!submitted->readback(0) && submitted->readback(0).error() == rhi::error::not_ready);
            CHECK(!submitted->readback(1));
            CHECK(service.collect() == rhi::error::ok && service.pending_count() == 1);
            CHECK(service.drain(0) == rhi::error::timeout && service.pending_count() == 1);
            allocation = std::unexpected(rhi::error::not_ready);
            target = {};
            no_flags = std::unexpected(rhi::error::not_ready);
            cluster.reset(); // submitted work is now retained by the service/ticket.
            gate.release();
            CHECK(submitted->wait(2000000000) == rhi::error::ok);
            auto result = submitted->readback(0);
            CHECK(result && result->size() == expected.size());
            if (result && result->size() == expected.size())
                for (std::size_t i = 0; i < expected.size(); ++i)
                    CHECK((*result)[i] == expected[i]);
            CHECK(service.collect() == rhi::error::ok && service.pending_count() == 0);
        }
        gate.release();
    }
    CHECK(!service.submit(service.make_batch()));
    // A forged lease core does not replace the backend's actual provenance check.
    auto other = engine::load_api_core(desc);
    CHECK(other);
    if (other) {
        auto owned = other->resources->create_buffer({.size = 16, .flags = rhi::to_bits(rhi::buffer_flag::transfer_destination)});
        auto forged = engine::resource_lease<rhi::buffer>::from_shared(core, rhi::shared_object<rhi::buffer>(std::move(owned)));
        auto rejected = service.make_batch();
        CHECK(rejected.upload(forged, 0, bytes, {}, transfer_read) == rhi::error::invalid_argument);
    }
    // Drop a receipt while pending: the service must still own its input and staging.
    {
        auto resources = engine::resource_cluster::create(core);
        auto buffer = resources->create_buffer({.size = 16, .flags = rhi::to_bits(rhi::buffer_flag::transfer_destination)});
        CHECK(buffer);
        if (buffer) {
            auto upload = service.make_batch();
            CHECK(upload.upload(*buffer, 0, bytes, {}, transfer_read) == rhi::error::ok);
            queue_gate gate(native.device, native.graphics_queue);
            {
                auto ignored = service.submit(std::move(upload));
                CHECK(ignored);
            }
            buffer = std::unexpected(rhi::error::not_ready);
            resources.reset();
            CHECK(service.pending_count() == 1);
            gate.release();
            CHECK(service.drain() == rhi::error::ok && service.pending_count() == 0);
        }
    }
    // Service destruction drains its own work; completed readback survives in the ticket.
    engine::buffer_transfer_ticket survivor;
    queue_gate gate(native.device, native.graphics_queue);
    std::thread release_gate;
    {
        engine::buffer_transfer_service scoped(core);
        auto resources = engine::resource_cluster::create(core);
        auto buffer = resources->create_buffer({.size = 16, .flags = rhi::to_bits(rhi::buffer_flag::transfer_destination) | rhi::to_bits(rhi::buffer_flag::transfer_source)});
        CHECK(buffer);
        if (buffer) {
            auto upload = scoped.make_batch();
            CHECK(upload.upload(*buffer, 0, expected, {}, transfer_read) == rhi::error::ok);
            CHECK(upload.readback(*buffer, 0, 16, transfer_read, transfer_read));
            auto result = scoped.submit(std::move(upload));
            CHECK(result);
            if (result)
                survivor = *result;
        }
        release_gate = std::thread([&] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); gate.release(); });
    }
    release_gate.join();
    CHECK(survivor.poll() == rhi::error::ok);
    auto result = survivor.readback(0);
    CHECK(result && result->size() == 16 && (*result)[0] == expected[0]);
    return deren::vk_test::finish("test_buffer_transfer");
}
