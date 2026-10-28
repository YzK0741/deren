#pragma once

inline void check_lazy_resource_contract(rhi::api_core& core) {
    using bytes = deren::utility::vessel::vector<std::uint8_t>;
    auto source = std::make_shared<bytes const>(64, std::uint8_t{23});
    std::weak_ptr<bytes const> weak = source;
    auto made = core.resources->create_lazy_buffer({.resource = {.size = 64, .usage = rhi::buffer_usage::vertex}, .data = source});
    CHECK(made);
    if (!made) return;
    auto value = std::move(*made);
    CHECK(value->state() == rhi::lazy_state::unloaded);
    CHECK(value->unload() == rhi::error::ok);
    CHECK(value->size() == 64);
    CHECK(value->mapped().empty());
    source.reset();
    CHECK(!weak.expired());
    auto commands = core.commands->create_command_buffer({});
    CHECK(commands);
    if (!commands) return;
    CHECK(commands->begin_recording({}) == rhi::error::ok);
    CHECK(commands->bind_vertex_buffer(*value, 0) == rhi::error::ok);
    CHECK(value->state() == rhi::lazy_state::unloaded);
    rhi::buffer_barrier barrier{.resource = value.get(), .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::shader_read};
    CHECK(commands->barrier({.buffers = std::span(&barrier, 1)}) == rhi::error::ok);
    CHECK(value->state() == rhi::lazy_state::ready);
    CHECK(value->unload() == rhi::error::not_ready);
    CHECK(commands->end_recording() == rhi::error::ok);
    commands.reset();
    CHECK(value->unload() == rhi::error::ok);
    CHECK(value->state() == rhi::lazy_state::unloaded);
    CHECK(!weak.expired());
    commands = core.commands->create_command_buffer({});
    CHECK(commands->begin_recording({}) == rhi::error::ok);
    CHECK(commands->barrier({.buffers = std::span(&barrier, 1)}) == rhi::error::ok);
    CHECK(value->state() == rhi::lazy_state::ready);
    CHECK(commands->end_recording() == rhi::error::ok);
    value.reset();
    CHECK(!weak.expired()); // executable recording retains the lazy source/resource
    commands.reset();
    CHECK(weak.expired());

    auto pixels = std::make_shared<bytes const>(16, std::uint8_t{127});
    auto image = core.resources->create_lazy_image({.resource = {.extent = {2, 2, 1}, .format = rhi::image_format::rgba8_unorm,
        .flags = rhi::to_bits(rhi::image_flag::sampled), .initial_bytes = {}}, .data = pixels});
    CHECK(image);
    if (!image) return;
    CHECK((*image)->state() == rhi::lazy_state::unloaded);
    auto view = (*image)->make_view({});
    CHECK(view);
    CHECK((*image)->state() == rhi::lazy_state::unloaded);
    CHECK(!(*image)->make_view({.base_mip = UINT32_MAX}));
    auto* host_copy = rhi::query_extension<rhi::host_image_copy>(core);
    std::array<std::byte, 16> copied{};
    if (host_copy) CHECK(host_copy->copy_image_to_memory(**image, copied, {}) == rhi::error::unsupported);
    auto rejected = core.commands->create_command_buffer({});
    CHECK(rejected->begin_recording({}) == rhi::error::ok);
    CHECK(rejected->clear_color_image(**image, {}, {}) == rhi::error::unsupported);
    CHECK((*image)->state() == rhi::lazy_state::unloaded);
    rhi::image_barrier invalid_transition{.resource = image->get(), .from = rhi::image_use::undefined, .to = rhi::image_use::undefined};
    CHECK(rejected->barrier(invalid_transition) == rhi::error::unsupported);
    CHECK((*image)->state() == rhi::lazy_state::unloaded);
    invalid_transition.from = rhi::image_use::shader_write;
    invalid_transition.to = rhi::image_use::shader_read;
    invalid_transition.range.base_mip = 1;
    CHECK(rejected->barrier(invalid_transition) == rhi::error::invalid_argument);
    CHECK((*image)->state() == rhi::lazy_state::unloaded);
    CHECK(rejected->end_recording() == rhi::error::ok);
    auto* space = rhi::query_extension<rhi::descriptor_space>(core);
    if (!space || !space->fixed_slots().count) return;
    auto slot = space->fixed_slots().base + space->fixed_slots().count - 1;
    rhi::image_view_desc range{};
    CHECK(space->write_image({.slot = slot, .resource = image->get(), .view = &range}) == rhi::error::ok);
    CHECK((*image)->state() == rhi::lazy_state::unloaded);
    auto descriptor_commands = core.commands->create_command_buffer({});
    CHECK(descriptor_commands->begin_recording({}) == rhi::error::ok);
    CHECK(space->bind({.commands = descriptor_commands.get()}) == rhi::error::ok);
    CHECK((*image)->state() == rhi::lazy_state::ready);
    if (auto* heap = rhi::query_extension<rhi::descriptor_heap>(core))
        CHECK(heap->write_image({.resource = image->get(), .view = &range}) == rhi::error::unsupported);
    CHECK((*image)->unload() == rhi::error::not_ready);
    if (host_copy) CHECK(host_copy->copy_image_to_memory(**image, copied, {}) == rhi::error::unsupported);
    CHECK(descriptor_commands->end_recording() == rhi::error::ok);
    descriptor_commands.reset();
    CHECK((*image)->unload() == rhi::error::not_ready); // view and fixed descriptor still retain it
    view.reset();
    CHECK((*image)->unload() == rhi::error::not_ready); // the fixed slot is a separate dependency
    rhi::image_desc replacement_desc{};
    replacement_desc.extent = {2, 2, 1};
    replacement_desc.format = rhi::image_format::rgba8_unorm;
    replacement_desc.flags = rhi::to_bits(rhi::image_flag::sampled);
    replacement_desc.initial_bytes = copied;
    auto replacement = core.resources->create_image(replacement_desc);
    CHECK(replacement);
    CHECK(space->write_image({.slot = slot, .resource = replacement.get(), .view = &range}) == rhi::error::ok);
    CHECK((*image)->unload() == rhi::error::ok);
    CHECK((*image)->state() == rhi::lazy_state::unloaded);
    descriptor_commands = core.commands->create_command_buffer({});
    CHECK(descriptor_commands->begin_recording({}) == rhi::error::ok);
    CHECK(space->write_image({.slot = slot, .resource = image->get(), .view = &range}) == rhi::error::ok);
    CHECK((*image)->state() == rhi::lazy_state::unloaded);
    CHECK(space->bind({.commands = descriptor_commands.get()}) == rhi::error::ok);
    CHECK((*image)->state() == rhi::lazy_state::ready);
    CHECK(descriptor_commands->end_recording() == rhi::error::ok);
    descriptor_commands.reset();
    CHECK(space->write_image({.slot = slot, .resource = replacement.get(), .view = &range}) == rhi::error::ok);
    auto retained_view = (*image)->make_view({});
    CHECK(retained_view);
    CHECK((*image)->unload() == rhi::error::not_ready);
    retained_view.reset();
    rhi::texture_group_info texture_info{};
    texture_info.textures[0] = image->get();
    auto retained_textures = core.resources->assign_texture_group(texture_info);
    CHECK(retained_textures);
    CHECK((*image)->unload() == rhi::error::not_ready);
    retained_textures.reset();
    CHECK((*image)->unload() == rhi::error::ok);

    auto grouped = core.resources->create_lazy_buffer({.resource = {.size = 16, .usage = rhi::buffer_usage::uniform_gpu_only,
        .flags = rhi::to_bits(rhi::buffer_flag::device_address)}, .data = pixels});
    CHECK(grouped);
    auto* grouped_raw = grouped->get();
    rhi::shared_object<rhi::buffer> grouped_owner(std::move(*grouped));
    rhi::resource_group_entry entry{.binding = 1, .kind = rhi::resource_kind::uniform_buffer, .resource = grouped_owner};
    auto group = core.resources->create_resource_group({.entries = std::span(&entry, 1)});
    CHECK(group);
    CHECK(grouped_raw->unload() == rhi::error::ok); // no physical allocation yet
    auto group_commands = core.commands->create_command_buffer({});
    CHECK(group_commands->begin_recording({}) == rhi::error::ok);
    rhi::buffer_barrier grouped_barrier{.resource = grouped_raw, .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::shader_read};
    CHECK(group_commands->barrier({.buffers = std::span(&grouped_barrier, 1)}) == rhi::error::ok);
    CHECK(group_commands->end_recording() == rhi::error::ok);
    group_commands.reset();
    CHECK(grouped_raw->unload() == rhi::error::not_ready); // logical group caches/owns this resource
    group = std::unexpected(rhi::error::unsupported);
    CHECK(grouped_raw->unload() == rhi::error::ok);

    if (auto* sorting = rhi::query_extension<rhi::gpu_sorting>(core)) {
        rhi::unique_object<rhi::radix_sorter> sorter(sorting->create_radix_sorter());
        if (sorter) {
            auto req = sorter->requirements(4);
            auto keys = core.resources->create_lazy_buffer({.resource = {.size = 16, .usage = rhi::buffer_usage::vertex,
                .flags = rhi::to_bits(rhi::buffer_flag::storage)}, .data = pixels});
            auto values = core.resources->create_lazy_buffer({.resource = {.size = 16, .usage = rhi::buffer_usage::vertex,
                .flags = rhi::to_bits(rhi::buffer_flag::storage)}, .data = pixels});
            auto scratch = core.resources->create_buffer({.size = req.scratch_bytes, .usage = rhi::buffer_usage::storage_gpu_only,
                .flags = req.scratch_flags});
            CHECK(keys && values && scratch);
            if (keys && values && scratch) {
                auto warm = core.commands->create_command_buffer({});
                CHECK(warm->begin_recording({}) == rhi::error::ok);
                std::array<rhi::buffer_barrier, 2> warm_barriers{{
                    {.resource = keys->get(), .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::shader_read_write},
                    {.resource = values->get(), .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::shader_read_write}}};
                CHECK(warm->barrier({.buffers = warm_barriers}) == rhi::error::ok);
                CHECK(warm->end_recording() == rhi::error::ok);
                warm.reset();
                auto sorted = core.commands->create_command_buffer({});
                CHECK(sorted->begin_recording({}) == rhi::error::ok);
                CHECK(sorter->record(*sorted, {.element_count = 4, .keys = keys->get(), .values = values->get(), .scratch = scratch.get()}) == rhi::error::ok);
                CHECK((*keys)->state() == rhi::lazy_state::ready);
                CHECK((*values)->state() == rhi::lazy_state::ready);
                CHECK((*keys)->unload() == rhi::error::not_ready);
                CHECK((*values)->unload() == rhi::error::not_ready);
                CHECK(sorted->end_recording() == rhi::error::ok);
                sorted.reset();
                CHECK((*keys)->unload() == rhi::error::ok);
                CHECK((*values)->unload() == rhi::error::ok);
            }
        }
    }

    auto a = core.resources->create_lazy_buffer({.resource = {.size = 16, .usage = rhi::buffer_usage::vertex}, .data = pixels});
    auto b = core.resources->create_lazy_buffer({.resource = {.size = 16, .usage = rhi::buffer_usage::vertex}, .data = pixels});
    CHECK(a && b);
    auto simplified = core.commands->create_command_buffer({});
    CHECK(simplified->begin_recording({}) == rhi::error::ok);
    CHECK(simplified->bind_vertex_buffer(**a, 0) == rhi::error::ok);
    CHECK(simplified->bind_vertex_buffer(**b, 0) == rhi::error::ok);
    CHECK(simplified->apply_operations() == rhi::error::ok);
    // The developer control deliberately retains dead setters and materializes both buffers.
    auto const first_state = std::getenv("DEREN_RENDER_OPERATIONS_UNOPTIMIZED") ? rhi::lazy_state::ready : rhi::lazy_state::unloaded;
    CHECK((*a)->state() == first_state);
    CHECK((*b)->state() == rhi::lazy_state::ready);
    CHECK(simplified->end_recording() == rhi::error::ok);
}
