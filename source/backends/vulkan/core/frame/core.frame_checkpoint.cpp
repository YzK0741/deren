module;
#include <cstdint>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.vstd;
import deren.promise.rhi;
import deren.utility.shared;
import deren.vulkan.error_tables;
import deren.vulkan.constant_init;
import :declarations;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;

    rhi::error core::frame_checkpoint_view::checkpoint_and_wait() {
        if (!parent || !parent->frame.frame_in_flight || !parent->frame.commands_view.recording) {
            return rhi::error::not_ready;
        }
        auto& commands = parent->frame.commands_view;
        if (commands.rendering_active) {
            return rhi::error::not_ready;
        }
        if (commands.recording_error != rhi::error::ok) {
            return commands.recording_error;
        }
        // Allocate completion storage before closing the recording, so allocation failure is retryable.
        auto submission = std::make_unique<pending_submission>();
        submission->parent = parent;
        VkFenceCreateInfo const create = make_fence_info();
        auto status = vkCreateFence(parent->device.logical_device, &create, nullptr, &submission->fence);
        if (status != VK_SUCCESS) {
            return generic_error(status);
        }
        if (auto closed = commands.end_recording(); closed != rhi::error::ok) {
            return closed;
        }
        submission->groups = commands.group_refs;
        submission->resources = commands.resource_refs;
        submission->lazy_refs = commands.lazy_refs;
        submission->secondaries = commands.secondary_refs;
        VkCommandBuffer native = commands.native();
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        bool const acquire = parent->create_options.native_window && !parent->frame.frame_acquire_consumed;
        auto const slot = static_cast<std::uint32_t>(parent->frame.current_frame);
        VkSubmitInfo submit = make_submit_info(&native);
        submit.waitSemaphoreCount = acquire ? 1u : 0u;
        submit.pWaitSemaphores = &parent->frame.image_available_semaphores[slot];
        submit.pWaitDstStageMask = &stage;
        {
            std::lock_guard lock(parent->command.submissions_mutex);
            for (auto const& secondary : submission->secondaries) {
                if (!secondary->executable || (!secondary->simultaneous_use && secondary->pending.load(std::memory_order_acquire))) {
                    return rhi::error::not_ready;
                }
            }
            std::lock_guard queue_lock(parent->device.vma.queue_access());
            status = vkQueueSubmit(parent->device.graphics_queue_handle, 1, &submit, submission->fence);
            if (status == VK_SUCCESS) {
                submission->submitted = true;
                for (auto const& secondary : submission->secondaries) {
                    secondary->pending.fetch_add(1, std::memory_order_release);
                    if (secondary->one_time_submit) {
                        secondary->executable = false;
                    }
                }
                parent->frame.frame_acquire_consumed = parent->frame.frame_acquire_consumed || acquire;
            }
        }
        if (status != VK_SUCCESS) {
            commands.recording_error = generic_error(status);
            commands.executable = false;
            return commands.recording_error;
        }
        // Never hold the queue lock during the host wait. A failed final wait cannot safely free/reset.
        status = vkWaitForFences(parent->device.logical_device, 1, &submission->fence, VK_TRUE, UINT64_MAX);
        if (status != VK_SUCCESS) {
            deren::utility::fast_panic("frame checkpoint completion wait failed");
        }
        parent->poll_submissions();
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
