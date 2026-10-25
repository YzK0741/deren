#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>

// Block the graphics queue deterministically; host signaling releases it without device-wide idle.
struct queue_gate {
    VkDevice device;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    bool armed = false;
    queue_gate(VkDevice dev, VkQueue queue)
        : device(dev) {
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        create.pNext = &type;
        CHECK(vkCreateSemaphore(device, &create, nullptr, &semaphore) == VK_SUCCESS);
        if (!semaphore)
            return;
        std::uint64_t const value = 1;
        VkTimelineSemaphoreSubmitInfo timeline{};
        timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline.waitSemaphoreValueCount = 1;
        timeline.pWaitSemaphoreValues = &value;
        VkPipelineStageFlags const stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.pNext = &timeline;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &semaphore;
        submit.pWaitDstStageMask = &stage;
        armed = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
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
        armed = false;
    }
    ~queue_gate() {
        release();
        if (semaphore) {
            // The test caller drains submitted work before this scope ends.
            vkDestroySemaphore(device, semaphore, nullptr);
        }
    }
};
