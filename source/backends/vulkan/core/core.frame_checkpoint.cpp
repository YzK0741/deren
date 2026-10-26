module;
#include <memory>
#include <mutex>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.promise.rhi;
import deren.utility.shared;
import deren.vulkan.error_tables;
import :declarations;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;

    rhi::error core::frame_checkpoint_view::checkpoint_and_wait() {
        if (!owner || !owner->frame_in_flight || !owner->commands_view.recording)
            return rhi::error::not_ready;
        auto& commands = owner->commands_view;
        if (commands.rendering_active)
            return rhi::error::not_ready;
        if (commands.recording_error != rhi::error::ok)
            return commands.recording_error;
        // Allocate completion storage before closing the recording, so allocation failure is retryable.
        auto submission = std::make_unique<pending_submission>();
        submission->owner = owner;
        VkFenceCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        auto status = vkCreateFence(owner->logical_device, &create, nullptr, &submission->fence);
        if (status != VK_SUCCESS)
            return generic_error(status);
        if (auto closed = commands.end_recording(); closed != rhi::error::ok)
            return closed;
        submission->groups = commands.group_refs;
        submission->resources = commands.resource_refs;
        submission->lazy_refs = commands.lazy_refs;
        submission->secondaries = commands.secondary_refs;
        VkCommandBuffer native = commands.native();
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        bool const acquire = owner->create_options.native_window && !owner->frame_acquire_consumed;
        auto const slot = static_cast<std::uint32_t>(owner->current_frame);
        VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = nullptr, .waitSemaphoreCount = acquire ? 1u : 0u, .pWaitSemaphores = &owner->image_available_semaphores[slot], .pWaitDstStageMask = &stage, .commandBufferCount = 1, .pCommandBuffers = &native, .signalSemaphoreCount = 0, .pSignalSemaphores = nullptr};
        {
            std::lock_guard lock(owner->submissions_mutex);
            for (auto const& secondary : submission->secondaries) {
                if (!secondary->executable || (!secondary->simultaneous_use && secondary->pending.load(std::memory_order_acquire)))
                    return rhi::error::not_ready;
            }
            std::lock_guard queue_lock(owner->vma.queue_access());
            status = vkQueueSubmit(owner->graphics_queue_handle, 1, &submit, submission->fence);
            if (status == VK_SUCCESS) {
                submission->submitted = true;
                for (auto const& secondary : submission->secondaries) {
                    secondary->pending.fetch_add(1, std::memory_order_release);
                    if (secondary->one_time_submit)
                        secondary->executable = false;
                }
                owner->frame_acquire_consumed = owner->frame_acquire_consumed || acquire;
            }
        }
        if (status != VK_SUCCESS) {
            commands.recording_error = generic_error(status);
            commands.executable = false;
            return commands.recording_error;
        }
        // Never hold the queue lock during the host wait. A failed final wait cannot safely free/reset.
        status = vkWaitForFences(owner->logical_device, 1, &submission->fence, VK_TRUE, UINT64_MAX);
        if (status != VK_SUCCESS)
            deren::utility::fast_panic("frame checkpoint completion wait failed");
        owner->poll_submissions();
        submission.reset();
        status = vkResetCommandBuffer(native, 0);
        if (status != VK_SUCCESS) {
            commands.recording_error = generic_error(status);
            commands.executable = false;
            return commands.recording_error;
        }
        return commands.begin_recording({});
    }
} // namespace deren::vulkan
