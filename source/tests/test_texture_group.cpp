#include "vk_test.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>
import deren.promise.rhi;
import deren.engine.backend_loader;
namespace rhi = deren::promise::rhi;

struct queue_gate {
    rhi::api_core& core;
    VkDevice device;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    bool armed = false;
    queue_gate(rhi::api_core& context, rhi::vulkan_escape& escape)
        : core(context)
        , device(static_cast<VkDevice>(escape.native_device())) {
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        create.pNext = &type;
        CHECK(vkCreateSemaphore(device, &create, nullptr, &semaphore) == VK_SUCCESS);
        if (!semaphore)
            return;
        std::uint64_t value = 1;
        VkTimelineSemaphoreSubmitInfo timeline{};
        timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline.waitSemaphoreValueCount = 1;
        timeline.pWaitSemaphoreValues = &value;
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        wait.pNext = &timeline;
        wait.waitSemaphoreCount = 1;
        wait.pWaitSemaphores = &semaphore;
        wait.pWaitDstStageMask = &stage;
        armed = vkQueueSubmit(static_cast<VkQueue>(escape.native_queue()), 1, &wait, VK_NULL_HANDLE) == VK_SUCCESS;
        CHECK(armed);
    }
    void release() {
        if (!armed)
            return;
        VkSemaphoreSignalInfo signal{};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
        signal.semaphore = semaphore;
        signal.value = 1;
        CHECK(vkSignalSemaphore(device, &signal) == VK_SUCCESS);
        core.wait_idle();
        armed = false;
    }
    ~queue_gate() {
        release();
        if (semaphore)
            vkDestroySemaphore(device, semaphore, nullptr);
    }
};

void test_heap_sampling(rhi::api_core& core, rhi::vulkan_escape& escape, bool use_secondary) {
    std::ifstream input(VR_TEXTURE_GROUP_SHADER_PATH, std::ios::binary | std::ios::ate);
    CHECK(input.good());
    if (!input.good())
        return;
    std::vector<std::byte> code(static_cast<std::size_t>(input.tellg()));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(code.size()));
    rhi::pipeline_desc desc{};
    desc.first_stage = rhi::shader_stage::compute;
    desc.compute_code = code;
    desc.texture_group = {.enabled = true, .push_byte_offset = 8, .shader_stage_bits = 4};
    rhi::object_manager<rhi::pipeline> pipeline{core.create_pipeline(desc)};
    CHECK(pipeline);
    auto wrong = desc;
    wrong.texture_group.push_byte_offset = 0; // destination is uint64, not the group token
    CHECK(!core.create_pipeline(wrong));
    if (!pipeline)
        return;
    auto make_image = [&core](std::uint32_t pixel) {
        rhi::image_desc info{};
        info.extent = {.width = 1, .height = 1, .depth = 1};
        info.format = rhi::image_format::rgba8_unorm;
        info.flags = rhi::to_bits(rhi::image_flag::sampled);
        info.initial_bytes = std::as_bytes(std::span(&pixel, 1));
        return rhi::object_manager<rhi::image>(core.create_image(info));
    };
    auto red = make_image(0xff0000ffu), blue = make_image(0xffff0000u), green = make_image(0xff00ff00u);
    CHECK(red && blue && green);
    if (!red || !blue || !green)
        return;
    rhi::texture_group_info a{};
    a.textures[0] = red.get();
    a.textures[15] = blue.get();
    rhi::texture_group_info b{};
    b.textures[0] = green.get();
    b.textures[15] = green.get();
    auto group_a = core.assign_texture_group(a), group_b = core.assign_texture_group(b);
    CHECK(group_a && group_b);
    if (!group_a || !group_b)
        return;
    red.reset();
    blue.reset();
    green.reset();
    rhi::object_manager<rhi::buffer> output{core.create_buffer({.size = 60, .usage = rhi::buffer_usage::storage_coherent, .flags = rhi::to_bits(rhi::buffer_flag::device_address)})};
    auto commands = core.make_command_buffer({.kind = use_secondary ? rhi::command_buffer_kind::secondary : rhi::command_buffer_kind::primary});
    auto* heap = rhi::query_extension<rhi::descriptor_heap>(core);
    auto* addresses = rhi::query_extension<rhi::device_address>(core);
    CHECK(output && commands && heap && addresses);
    if (!output || !commands || !heap || !addresses)
        return;
    std::memset(output->mapped().data(), 0, 60);
    CHECK(commands->load_texture_group(group_a) == rhi::error::not_ready);
    CHECK(commands->begin_recording({}) == rhi::error::ok);
    CHECK(commands->load_texture_group(group_a) == rhi::error::unsupported);
    CHECK(commands->bind_pipeline(*pipeline) == rhi::error::ok);
    struct parameters {
        std::uint64_t destination;
        std::uint32_t token;
        std::uint32_t index;
    };
    parameters data{addresses->buffer_address(*output, 0), 0, 0};
    CHECK(heap->push_data({.commands = commands.get(), .data = std::as_bytes(std::span(&data.destination, 1))}) == rhi::error::ok);
    CHECK(heap->push_data({.commands = commands.get(), .offset = 8, .data = std::as_bytes(std::span(&data.token, 1))}) == rhi::error::invalid_argument);
    for (std::uint32_t i = 0; i < 5; ++i) {
        data.index = i;
        CHECK(heap->push_data({.commands = commands.get(), .offset = 12, .data = std::as_bytes(std::span(&data.index, 1))}) == rhi::error::ok);
        if (i == 0 || i == 4)
            CHECK(commands->load_texture_group(group_a) == rhi::error::ok);
        if (i == 0) {
            struct foreign_group : rhi::texture_group {
                foreign_group()
                    : texture_group("deren_texture_group_vulkan") {
                }
                void release() noexcept override {
                }
            } foreign;
            rhi::texture_group_ref wrong(&foreign, [](rhi::texture_group*) {});
            CHECK(commands->load_texture_group(wrong) == rhi::error::invalid_argument);
        }
        if (i == 1)
            CHECK(commands->load_texture_group(group_b) == rhi::error::ok);
        if (i == 2)
            CHECK(commands->load_texture_group({}) == rhi::error::ok);
        if (i == 3)
            CHECK(commands->bind_pipeline(*pipeline) == rhi::error::ok);
        commands->dispatch(1, 1, 1);
    }
    auto native = static_cast<VkCommandBuffer>(escape.native_command_buffer(*commands));
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(native, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    CHECK(commands->end_recording() == rhi::error::ok);
    auto submitted = commands;
    if (use_secondary) {
        submitted = core.make_command_buffer({});
        CHECK(submitted->begin_recording({}) == rhi::error::ok);
        CHECK(submitted->execute(*commands) == rhi::error::ok);
        CHECK(submitted->end_recording() == rhi::error::ok);
    }
    auto extra_image = make_image(0xff00ffffu);
    rhi::texture_group_info extra_info{};
    extra_info.textures[0] = extra_image.get();
    std::weak_ptr<rhi::texture_group> lifetime_a = group_a, lifetime_b = group_b;
    queue_gate gate(core, escape);
    CHECK(core.submit(*submitted) == rhi::error::ok);
    group_a.reset();
    group_b.reset();
    commands.reset();
    submitted.reset();
    CHECK(!lifetime_a.expired() && !lifetime_b.expired());
    auto new_group = core.assign_texture_group(extra_info);
    CHECK(new_group); // new records/descriptors must not overwrite the delayed groups
    gate.release();
    CHECK(lifetime_a.expired() && lifetime_b.expired());
    std::array<std::uint32_t, 15> actual{};
    std::memcpy(actual.data(), output->mapped().data(), 60);
    std::array<std::uint32_t, 15> const expected{0x8001, 0xff, 0xff0000, 0x8001, 0xff00, 0xff00,
                                                 0, 0, 0, 0, 0, 0, 0x8001, 0xff, 0xff0000};
    CHECK(actual == expected);
    if (actual != expected)
        for (std::size_t i = 0; i < actual.size(); ++i)
            deren::vk_test::write_line("heap output[{}] = {:#x}, expected {:#x}", i, actual[i], expected[i]);
}

int main(int argc, char** argv) {
    bool with_device = false;
    for (int i = 1; i < argc; ++i)
        with_device |= std::string_view(argv[i]) == "--with-device";
    if (!with_device)
        return deren::vk_test::finish("test_texture_group (device skipped)");
    rhi::create_info creation{};
    creation.window_width = 64;
    creation.window_height = 64;
    creation.window_visible = false;
    creation.validation_layers = true;
    auto core = deren::engine::load_api_core(creation);
    CHECK(core);
    if (!core)
        return deren::vk_test::finish("test_texture_group");
    auto* escape = rhi::query_extension<rhi::vulkan_escape>(*core);
    CHECK(escape);
    if (!escape)
        return deren::vk_test::finish("test_texture_group");
    auto device = static_cast<VkDevice>(escape->native_device());
    auto queue = static_cast<VkQueue>(escape->native_queue());
    test_heap_sampling(*core, *escape, false);
    test_heap_sampling(*core, *escape, true);
    rhi::object_manager<rhi::buffer> output{core->create_buffer({.size = 4, .usage = rhi::buffer_usage::readback_coherent})};
    auto secondary = core->make_command_buffer({.kind = rhi::command_buffer_kind::secondary});
    auto primary = core->make_command_buffer({});
    CHECK(output && secondary && primary);
    if (!output || !secondary || !primary)
        return deren::vk_test::finish("test_texture_group");
    std::memset(output->mapped().data(), 0, 4);
    CHECK(secondary->begin_recording({}) == rhi::error::ok);
    auto native_secondary = static_cast<VkCommandBuffer>(escape->native_command_buffer(*secondary));
    vkCmdFillBuffer(native_secondary, static_cast<VkBuffer>(escape->native_buffer(*output)), 0, 4, 0x11223344u);
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(native_secondary, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         1, &barrier, 0, nullptr, 0, nullptr);
    CHECK(secondary->end_recording() == rhi::error::ok);
    CHECK(primary->begin_recording({}) == rhi::error::ok);
    CHECK(primary->execute(*secondary) == rhi::error::ok);
    CHECK(secondary->begin_recording({}) == rhi::error::not_ready);
    CHECK(primary->end_recording() == rhi::error::ok);

    VkSemaphoreTypeCreateInfo type{};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    create.pNext = &type;
    VkSemaphore gate = VK_NULL_HANDLE;
    CHECK(vkCreateSemaphore(device, &create, nullptr, &gate) == VK_SUCCESS);
    if (gate == VK_NULL_HANDLE)
        return deren::vk_test::finish("test_texture_group");
    std::uint64_t const value = 1;
    VkTimelineSemaphoreSubmitInfo timeline{};
    timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline.waitSemaphoreValueCount = 1;
    timeline.pWaitSemaphoreValues = &value;
    VkPipelineStageFlags const stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    wait.pNext = &timeline;
    wait.waitSemaphoreCount = 1;
    wait.pWaitSemaphores = &gate;
    wait.pWaitDstStageMask = &stage;
    CHECK(vkQueueSubmit(queue, 1, &wait, VK_NULL_HANDLE) == VK_SUCCESS);
    CHECK(core->submit(*primary) == rhi::error::ok);
    CHECK(primary->begin_recording({}) == rhi::error::not_ready);
    CHECK(core->submit(*primary) == rhi::error::not_ready);
    secondary.reset();
    primary.reset(); // both command pools must survive the caller until completion
    VkSemaphoreSignalInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    signal.semaphore = gate;
    signal.value = value;
    CHECK(vkSignalSemaphore(device, &signal) == VK_SUCCESS);
    core->wait_idle();
    std::uint32_t actual = 0;
    std::memcpy(&actual, output->mapped().data(), sizeof(actual));
    CHECK(actual == 0x11223344u);
    vkDestroySemaphore(device, gate, nullptr);
    auto recycled = core->make_command_buffer({});
    CHECK(recycled);
    if (recycled) {
        CHECK(recycled->begin_recording({}) == rhi::error::ok);
        CHECK(recycled->end_recording() == rhi::error::ok);
        CHECK(core->submit(*recycled) == rhi::error::ok);
        core->wait_idle();
        CHECK(recycled->begin_recording({}) == rhi::error::ok);
        CHECK(recycled->end_recording() == rhi::error::ok);
    }
    return deren::vk_test::finish("test_texture_group");
}
