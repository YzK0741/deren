module;
#include <memory>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import :declarations;

namespace deren::vulkan {
    std::shared_ptr<core::owned_command_buffer> core::retain_command(owned_command_buffer& command, bool recorded_secondary) {
        command.references.fetch_add(1, std::memory_order_relaxed);
        if (recorded_secondary)
            command.recording_users.fetch_add(1, std::memory_order_relaxed);
        return std::shared_ptr<owned_command_buffer>(&command, [recorded_secondary](owned_command_buffer* value) {
            if (recorded_secondary)
                value->recording_users.fetch_sub(1, std::memory_order_acq_rel);
            value->release();
        });
    }

    core::pending_submission::~pending_submission() noexcept {
        if (submitted)
            for (auto const& secondary : secondaries)
                secondary->pending.fetch_sub(1, std::memory_order_acq_rel);
        if (submitted && primary)
            primary->pending.fetch_sub(1, std::memory_order_acq_rel);
        if (fence != VK_NULL_HANDLE)
            vkDestroyFence(owner->logical_device, fence, nullptr);
    }

    void core::poll_submissions(bool all) const noexcept {
        std::vector<std::unique_ptr<pending_submission>> completed;
        {
            std::lock_guard lock(submissions_mutex);
            for (auto i = pending_submissions.begin(); i != pending_submissions.end();) {
                auto const& submission = **i;
                bool done = all;
                if (!done && submission.fence != VK_NULL_HANDLE)
                    done = vkGetFenceStatus(logical_device, submission.fence) == VK_SUCCESS;
                if (!done && submission.timeline != VK_NULL_HANDLE) {
                    std::uint64_t reached = 0;
                    done = vkGetSemaphoreCounterValue(logical_device, submission.timeline, &reached) == VK_SUCCESS &&
                           reached >= submission.value;
                }
                if (done) {
                    completed.push_back(std::move(*i));
                    i = pending_submissions.erase(i);
                } else
                    ++i;
            }
        }
        // Destructors can enter the command/image/group registries, so unlock first.
    }
} // namespace deren::vulkan
