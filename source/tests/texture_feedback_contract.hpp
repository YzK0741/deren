#pragma once

inline void check_texture_feedback(rhi::api_core& core) {
    namespace tr = deren::engine::texture_residency;
    namespace layout = deren::engine::render_layout;
    auto* space = rhi::query_extension<rhi::descriptor_space>(core);
    auto* submission = rhi::query_extension<rhi::command_submission>(core);
    CHECK(space && submission);
    if (!space || !submission) return;
    std::ifstream input(VR_FEEDBACK_SHADER_PATH, std::ios::binary | std::ios::ate);
    CHECK(input.good());
    if (!input) return;
    std::vector<std::byte> code(static_cast<std::size_t>(input.tellg()));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(code.data()), code.size());
    rhi::pipeline_desc desc{};
    desc.first_stage = rhi::shader_stage::compute;
    desc.compute_code = code;
    auto pipeline = core.resources->create_pipeline(desc);
    std::array<deren::engine::visibility::draw_item, 2> items{};
    items[0].material_index = 2;
    items[1].material_index = 3;
    auto table = core.resources->create_buffer({.size = sizeof(items), .usage = rhi::buffer_usage::storage_coherent,
        .flags = rhi::to_bits(rhi::buffer_flag::device_address), .initial_bytes = std::as_bytes(std::span(items))});
    auto result = core.resources->create_buffer({.size = sizeof(tr::feedback), .usage = rhi::buffer_usage::storage_coherent,
        .flags = rhi::to_bits(rhi::buffer_flag::device_address)});
    CHECK(pipeline && table && result);
    if (!pipeline || !table || !result) return;
    auto image = [&](std::uint32_t value, rhi::image_format format) {
        return core.resources->create_image({.extent = {1, 1, 1}, .format = format,
            .flags = rhi::to_bits(rhi::image_flag::sampled), .initial_bytes = std::as_bytes(std::span(&value, 1))});
    };
    auto hw = image(deren::engine::visibility::pack_id(0, 0, 0)->value, rhi::image_format::r32_uint);
    auto sw = image(deren::engine::visibility::pack_id(1, 0, 0)->value, rhi::image_format::r32_uint);
    auto empty = image(0, rhi::image_format::r32_uint);
    auto depth = image(UINT32_MAX, rhi::image_format::rgba8_unorm);
    auto sw_depth = image(std::bit_cast<std::uint32_t>(0.25f), rhi::image_format::r32_uint);
    CHECK(hw && sw && depth && sw_depth && empty);
    if (!hw || !sw || !depth || !sw_depth || !empty) return;
    rhi::image_view_desc view{};
    for (auto [slot, resource] : std::array<std::pair<std::uint32_t, rhi::image*>, 4>{{
        {layout::heap_slots::visibility, hw.get()}, {layout::heap_slots::software_id, sw.get()},
        {layout::heap_slots::gbuffer_depth, depth.get()}, {layout::heap_slots::software_depth, sw_depth.get()}}})
        CHECK(space->write_image({.slot = slot, .resource = resource, .view = &view}) == rhi::error::ok);
    constexpr auto output_slot = layout::heap_slot_base + 1364;
    CHECK(space->write_buffer({.slot = layout::heap_slots::visibility_items, .resource = table.get(), .size = sizeof(items)}) == rhi::error::ok);
    CHECK(space->write_buffer({.slot = output_slot, .resource = result.get(), .size = sizeof(tr::feedback)}) == rhi::error::ok);
    for (auto test : std::array<std::array<std::uint32_t, 4>, 4>{{{0, 2, 1u << 2, 0}, {1, 2, 1u << 3, 0}, {0, 0, 0, 0}, {0, 0, 0, 1}}}) {
        if (test[3]) CHECK(space->write_image({.slot = layout::heap_slots::visibility, .resource = empty.get(), .view = &view}) == rhi::error::ok);
        std::memset(result->mapped().data(), 0, sizeof(tr::feedback));
        auto commands = core.commands->create_command_buffer({});
        CHECK(commands->begin_recording({}) == rhi::error::ok);
        CHECK(commands->bind_pipeline(*pipeline) == rhi::error::ok);
        CHECK(space->bind({.commands = commands.get()}) == rhi::error::ok);
        std::array<std::uint32_t, 6> push{0, 0, test[0], output_slot, 4, test[1]};
        CHECK(commands->push_constants(0, std::as_bytes(std::span(push))) == rhi::error::ok);
        commands->dispatch(1, 1, 1);
        rhi::buffer_barrier host{.resource = result.get(), .from = rhi::buffer_use::shader_write, .to = rhi::buffer_use::host_read};
        CHECK(commands->barrier({.buffers = std::span(&host, 1)}) == rhi::error::ok);
        CHECK(commands->end_recording() == rhi::error::ok);
        auto token = submission->submit(*commands);
        CHECK(token);
        if (token) CHECK(submission->wait(*token, UINT64_MAX) == rhi::error::ok);
        tr::feedback feedback;
        std::memcpy(&feedback, result->mapped().data(), sizeof(feedback));
        CHECK(feedback.materials[0] == test[2]);
        CHECK(feedback.invalid == (test[1] == 0 && !test[3] ? 1u : 0u));
    }
}
