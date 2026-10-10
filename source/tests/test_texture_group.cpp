#include "vk_test.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <vulkan/vulkan.h>
import deren.promise.rhi;
import deren.engine.backend_loader;
namespace rhi = deren::promise::rhi;

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
