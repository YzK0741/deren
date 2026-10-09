#pragma once

#include <array>
#include <filesystem>
#include <fstream>

// The caller has already built the indexed triangle and waited for it. Tests may use the native
// queue to submit caller-owned commands; resource creation and recording stay on the RHI surface.
static void check_rt_traversal(deren::promise::rhi::api_core& core,
                               deren::promise::rhi::vulkan_escape& escape,
                               deren::promise::rhi::device_address& addresses,
                               deren::promise::rhi::device_capabilities& capabilities,
                               deren::promise::rhi::acceleration_structure& blas,
                               deren::promise::rhi::acceleration_structure& first_tlas,
                               char const* executable, bool native_heap) {
    namespace rhi = deren::promise::rhi;
    auto* const heap = rhi::query_extension<rhi::descriptor_heap>(core);
    CHECK(heap != nullptr && heap->ready());
    if (heap == nullptr || !heap->ready()) {
        return;
    }
    auto const read_shader = [executable](char const* name) {
        auto const path = std::filesystem::absolute(executable).parent_path() / "shaders" / name;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) { return std::vector<std::byte>{}; }
        auto const size = file.tellg();
        std::vector<std::byte> code(static_cast<std::size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(code.data()), size);
        return file ? code : std::vector<std::byte>{};
    };
    auto const raygen = read_shader(native_heap ? "rt_traversal_probe.native.spv" : "rt_traversal_probe.rgen.spv");
    auto const miss = read_shader("rt_traversal_probe.rmiss.spv");
    auto const hit = read_shader("rt_traversal_probe.rchit.spv");
    CHECK(!raygen.empty() && !miss.empty() && !hit.empty());
    if (raygen.empty() || miss.empty() || hit.empty()) { return; }
    std::array<rhi::ray_tracing_stage, 3> const stages = {
        rhi::ray_tracing_stage{.stage = rhi::shader_stage::ray_generation, .code = raygen},
        rhi::ray_tracing_stage{.stage = rhi::shader_stage::miss, .code = miss},
        rhi::ray_tracing_stage{.stage = rhi::shader_stage::closest_hit, .code = hit},
    };
    std::array<rhi::ray_tracing_group, 3> const groups = {
        rhi::ray_tracing_group{.general = 0}, rhi::ray_tracing_group{.general = 1},
        rhi::ray_tracing_group{.closest_hit = 2},
    };
    constexpr std::uint32_t offset = (16384u + 1000u) * 64u; // deliberately different from the renderer's TLAS slots
    auto const heap_binding = heap->bindings().resource;
    CHECK(heap_binding.address != 0 && heap_binding.size >= offset + 64u + sizeof(std::uint64_t));
    if (heap_binding.address == 0 || heap_binding.size < offset + 64u + sizeof(std::uint64_t)) { return; }
    std::array<rhi::acceleration_structure_heap_binding, 1> const bindings = {
        rhi::acceleration_structure_heap_binding{.binding = 7, .byte_offset = offset, .array_stride = 64, .array_count = 2},
    };
    rhi::pipeline_desc desc{};
    desc.debug_name = "deterministic RT traversal probe";
    desc.ray_tracing_stages = stages;
    desc.ray_tracing_groups = groups;
    if (!native_heap) {
        auto invalid = bindings;
        invalid[0].byte_offset = static_cast<std::uint32_t>(heap->properties().resource_size - 64);
        desc.acceleration_structure_bindings = invalid;
        rhi::object_manager<rhi::pipeline> out_of_range{core.create_pipeline(desc)};
        CHECK(!out_of_range); // second array element must fit too
        invalid = bindings;
        invalid[0].array_count = 0xFFFFFFFFu;
        rhi::object_manager<rhi::pipeline> overflowing{core.create_pipeline(desc)};
        CHECK(!overflowing);
        std::array<rhi::acceleration_structure_heap_binding, 2> const duplicates{bindings[0], bindings[0]};
        desc.acceleration_structure_bindings = duplicates;
        rhi::object_manager<rhi::pipeline> duplicate{core.create_pipeline(desc)};
        CHECK(!duplicate);
        desc.acceleration_structure_bindings = bindings;
    }
    rhi::object_manager<rhi::pipeline> pipeline{core.create_pipeline(desc)};
    CHECK(pipeline);
    if (!pipeline) { return; }

    auto const properties = capabilities.shader_binding_table();
    CHECK(properties.handle_size != 0 && properties.base_alignment != 0);
    if (properties.handle_size == 0 || properties.base_alignment == 0) { return; }
    std::uint32_t const stride = ((properties.handle_size + properties.base_alignment - 1) / properties.base_alignment) * properties.base_alignment;
    std::vector<std::uint8_t> handles(3u * properties.handle_size);
    auto* const group_access = rhi::query_extension<rhi::shader_group_access>(core);
    CHECK(group_access != nullptr);
    if (group_access == nullptr) { return; }
    bool const queried = group_access->read(*pipeline, 0, 3, handles) == rhi::error::ok;
    CHECK(queried);
    if (!queried) { return; }
    CHECK(group_access->read(*pipeline, 3, 1, handles) == rhi::error::invalid_argument);
    CHECK(group_access->read(*pipeline, 0, 0, handles) == rhi::error::invalid_argument);
    CHECK(group_access->read(*pipeline, 0, 3, std::span<std::uint8_t>{handles}.first(handles.size() - 1)) == rhi::error::invalid_argument);
    CHECK(group_access->read(*pipeline, 0xFFFFFFFFu, 2, handles) == rhi::error::invalid_argument);
    struct foreign_pipeline final : rhi::pipeline { void release() noexcept override {} } foreign;
    CHECK(group_access->read(foreign, 0, 1, handles) == rhi::error::invalid_argument);
    std::vector<std::uint8_t> legacy_handles(handles.size());
    auto* const basis = escape.get_basis();
    CHECK(basis != nullptr && escape.shader_group_handles(*basis, *pipeline, 0, 3, legacy_handles));
    CHECK(handles == legacy_handles);
    rhi::object_manager<rhi::buffer> table{core.create_buffer({
        .size = 3u * stride + properties.base_alignment - 1u, .usage = rhi::buffer_usage::storage_coherent,
        .flags = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::shader_binding_table),
    })};
    rhi::object_manager<rhi::buffer> output{core.create_buffer({
        .size = 6u * sizeof(std::uint32_t), .usage = rhi::buffer_usage::storage_coherent,
        .flags = rhi::to_bits(rhi::buffer_flag::device_address),
    })};
    rhi::object_manager<rhi::acceleration_structure> second_tlas{core.create_acceleration_structure({
        .type = rhi::acceleration_structure_type::top_level, .instance_capacity = 1,
    })};
    CHECK(table && output && second_tlas);
    if (!table || !output || !second_tlas) { return; }
    auto const base = addresses.buffer_address(*table, 0);
    auto const aligned = ((base + properties.base_alignment - 1) / properties.base_alignment) * properties.base_alignment;
    auto const table_offset = static_cast<std::size_t>(aligned - base);
    for (std::size_t group = 0; group < 3; ++group) {
        std::memcpy(table->mapped().data() + table_offset + group * stride, handles.data() + group * properties.handle_size, properties.handle_size);
    }
    std::memset(output->mapped().data(), 0xCD, output->mapped().size());
    rhi::acceleration_structure_instance moved{};
    moved.transform[0] = moved.transform[5] = moved.transform[10] = 1;
    moved.transform[3] = 3; // slot 1 hits only if the correct descriptor array element was read
    moved.structure_reference = blas.device_address();
    auto const checked = [](rhi::error result) { CHECK(result == rhi::error::ok); return result == rhi::error::ok; };
    if (!checked(second_tlas->write_instances(std::span(&moved, 1)))) { return; }
    if (!checked(heap->write_buffer({.offset = offset, .address = first_tlas.device_address(), .size = first_tlas.size_bytes(),
                              .type = rhi::descriptor_type::acceleration_structure}))) { return; }
    if (!checked(heap->write_buffer({.offset = offset + 64, .address = second_tlas->device_address(), .size = second_tlas->size_bytes(),
                              .type = rhi::descriptor_type::acceleration_structure}))) { return; }
    rhi::object_manager<rhi::command_buffer> commands{core.create_command_buffer({})};
    CHECK(commands);
    if (!commands) { return; }
    if (!checked(commands->begin_recording({}))) { return; }
    if (!checked(commands->build_acceleration_structure(*second_tlas))) { return; }
    if (!checked(commands->barrier({.stage = rhi::stage_hint::ray_tracing, .has_memory = true,
                             .memory = {.from = rhi::buffer_use::acceleration_structure_write, .to = rhi::buffer_use::acceleration_structure_read}}))) { return; }
    if (!checked(heap->bind({.commands = commands.get()}))) { return; }
    if (!checked(commands->bind_pipeline(*pipeline))) { return; }
    struct push { std::uint64_t destination; std::uint32_t slot; std::uint32_t padding = 0; std::uint64_t resource_heap; };
    static_assert(sizeof(push) == 24 && offsetof(push, resource_heap) == 16);
    auto const region = [stride](std::uint64_t address) { return rhi::shader_binding_table_region{.address = address, .size = stride, .stride = stride}; };
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
        push const data{.destination = addresses.buffer_address(*output, 0), .slot = slot, .resource_heap = heap_binding.address};
        if (!checked(heap->push_data({.commands = commands.get(), .data = std::as_bytes(std::span(&data, 1))}))) { return; }
        commands->trace_rays(region(aligned), region(aligned + stride), region(aligned + 2 * stride), {}, 3, 1, 1);
    }
    if (!checked(commands->end_recording())) { return; }
    VkDevice const device = static_cast<VkDevice>(escape.native_device());
    auto const native = static_cast<VkCommandBuffer>(escape.native_command_buffer(*commands));
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    VkResult const fence_result = vkCreateFence(device, &fence_info, nullptr, &fence);
    CHECK(fence_result == VK_SUCCESS);
    if (fence_result != VK_SUCCESS) { return; }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &native;
    VkResult const submitted = vkQueueSubmit(static_cast<VkQueue>(escape.native_queue()), 1, &submit, fence);
    CHECK(submitted == VK_SUCCESS);
    VkResult const waited = submitted == VK_SUCCESS ? vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000ull) : submitted;
    CHECK(waited == VK_SUCCESS);
    deren::vk_test::write_line("rt_traversal: submit={} wait={} ({})", static_cast<int>(submitted), static_cast<int>(waited),
                               native_heap ? "native heap" : "mapped heap");
    if (waited != VK_SUCCESS) { core.wait_idle(); }
    vkDestroyFence(device, fence, nullptr);
    if (waited != VK_SUCCESS) { return; }
    std::array<std::uint32_t, 6> values{};
    std::memcpy(values.data(), output->mapped().data(), sizeof(values));
    for (std::size_t slot = 0; slot < 2; ++slot) {
        CHECK(values[slot * 3] == 1);     // known triangle hit
        CHECK(values[slot * 3 + 1] == 0); // geometric miss
        CHECK(values[slot * 3 + 2] == 0); // instance mask discards a known hit
    }
    deren::vk_test::write_line("rt_traversal: GPU results {},{},{} / {},{},{} (expected 1,0,0 / 1,0,0; {})",
        values[0], values[1], values[2], values[3], values[4], values[5], native_heap ? "native heap" : "mapped heap");
}
