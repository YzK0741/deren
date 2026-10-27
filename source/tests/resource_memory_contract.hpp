#pragma once

void check_resource_memory_tracker() {
    deren::vulkan::detail::resource_memory_tracker<rhi::resource_memory_statistics> tracker;
    tracker.add(false, 100, true);
    auto buffer = tracker.snapshot();
    CHECK(buffer.buffers.current_bytes == 100 && buffer.buffers.current_device_local_bytes == 100);
    tracker.remove(false, 100, true);
    tracker.add(true, 200, false);
    auto image = tracker.snapshot();
    CHECK(image.images.current_bytes == 200 && image.images.current_device_local_bytes == 0);
    CHECK(image.total.peak_bytes == 200); // disjoint peaks must not be summed to 300
    CHECK(image.total.peak_allocations == 1);
    tracker.add(false, 50, true);
    CHECK(tracker.snapshot().total.current_bytes == 250);
    CHECK(tracker.snapshot().total.peak_bytes == 250);
    tracker.remove(true, 200, false);
    tracker.remove(false, 50, true);
    auto final = tracker.snapshot();
    CHECK(final.total.current_bytes == 0 && final.total.current_allocations == 0);
    CHECK(final.total.peak_bytes == 250 && final.total.peak_device_local_bytes == 100);

    deren::vulkan::detail::resource_memory_tracker<rhi::resource_memory_statistics> concurrent;
    std::barrier rendezvous(5);
    std::array<std::thread, 4> workers;
    for (unsigned i = 0; i < workers.size(); ++i) {
        workers[i] = std::thread([&, i] {
            concurrent.add(i % 2, 64, true);
            rendezvous.arrive_and_wait();
            rendezvous.arrive_and_wait();
            concurrent.remove(i % 2, 64, true);
        });
    }
    rendezvous.arrive_and_wait();
    auto overlapping = concurrent.snapshot();
    CHECK(overlapping.total.current_bytes == 256 && overlapping.total.peak_bytes == 256);
    CHECK(overlapping.buffers.current_bytes + overlapping.images.current_bytes == overlapping.total.current_bytes);
    CHECK(overlapping.total.current_allocations == 4 && overlapping.total.peak_allocations == 4);
    rendezvous.arrive_and_wait();
    for (auto& worker : workers) worker.join();
    CHECK(concurrent.snapshot().total.current_bytes == 0 && concurrent.snapshot().total.peak_bytes == 256);
}

void check_resource_memory_device(rhi::api_core& core) {
    auto before = core.resources->memory_statistics();
    CHECK(before.supported);
    CHECK(before.total.current_bytes == before.buffers.current_bytes + before.images.current_bytes);
    CHECK(before.total.current_bytes == before.allocator_allocation_bytes);
    CHECK(before.allocator_reserved_bytes >= before.total.current_bytes);
    auto buffer = core.resources->create_buffer({.size = 4097, .usage = rhi::buffer_usage::storage_gpu_only});
    CHECK(buffer);
    auto live_buffer = core.resources->memory_statistics();
    CHECK(live_buffer.buffers.current_allocations == before.buffers.current_allocations + 1);
    CHECK(live_buffer.buffers.current_bytes >= before.buffers.current_bytes + 4097);
    buffer.reset();
    CHECK(core.resources->memory_statistics().buffers.current_bytes == before.buffers.current_bytes);
    CHECK(core.resources->memory_statistics().buffers.peak_bytes >= live_buffer.buffers.peak_bytes);

    std::array<std::uint8_t, 64*64*4> pixels{};
    rhi::image_desc desc{.extent = {64, 64, 1}, .mip_levels = 1, .array_layers = 1,
        .format = rhi::image_format::rgba8_unorm, .flags = rhi::to_bits(rhi::image_flag::sampled),
        .initial_bytes = std::as_bytes(std::span(pixels))};
    auto first = core.resources->create_image(desc);
    CHECK(first);
    auto live_image = core.resources->memory_statistics();
    CHECK(live_image.images.current_allocations == before.images.current_allocations + 1);
    CHECK(live_image.images.current_bytes >= before.images.current_bytes + pixels.size());
    auto second = core.resources->create_image(desc);
    auto view = first ? first->make_view({}) : rhi::unique_object<rhi::image_view>{};
    CHECK(second && view);
    CHECK(core.resources->memory_statistics().images.current_bytes == live_image.images.current_bytes);
    first.reset(); second.reset();
    CHECK(core.resources->memory_statistics().images.current_bytes == live_image.images.current_bytes);
    view.reset();
    CHECK(core.resources->memory_statistics().images.current_bytes == before.images.current_bytes);

    auto source = std::make_shared<deren::utility::vessel::vector<std::uint8_t> const>(pixels.begin(), pixels.end());
    desc.initial_bytes = {};
    auto lazy = core.resources->create_lazy_image({.resource = desc, .data = source});
    CHECK(lazy);
    CHECK(core.resources->memory_statistics().images.current_bytes == before.images.current_bytes);
    if (lazy) {
        auto commands = core.commands->create_command_buffer({});
        CHECK(commands && commands->begin_recording({}) == rhi::error::ok);
        if (commands) {
            // Use a supported barrier recipe to demand the physical allocation; this recording is discarded.
            CHECK(commands->barrier(rhi::image_barrier{.resource = lazy->get(), .from = rhi::image_use::shader_read, .to = rhi::image_use::transfer_destination}) == rhi::error::ok);
            CHECK(core.resources->memory_statistics().images.current_bytes > before.images.current_bytes);
            CHECK(commands->end_recording() == rhi::error::ok);
            commands.reset();
            CHECK((*lazy)->unload() == rhi::error::ok);
            CHECK(core.resources->memory_statistics().images.current_bytes == before.images.current_bytes);
        }
    }
    auto after = core.resources->memory_statistics();
    CHECK(after.total.current_bytes == after.allocator_allocation_bytes);
    CHECK(after.total.peak_bytes >= live_image.total.peak_bytes);
}
