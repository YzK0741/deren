module;
#include <memory>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.utility.shared;
import :declarations;

namespace deren::vulkan {
    std::shared_ptr<core::owned_command_buffer> core::retain_command(owned_command_buffer& command, bool recorded_secondary) {
        command.references.fetch_add(1, std::memory_order_relaxed);
        if (recorded_secondary) {
            command.recording_users.fetch_add(1, std::memory_order_relaxed);
        }
        return std::shared_ptr<owned_command_buffer>(&command, [recorded_secondary](owned_command_buffer* value) {
            if (recorded_secondary) {
                value->recording_users.fetch_sub(1, std::memory_order_acq_rel);
            }
            value->release();
        });
    }

    namespace rhi = deren::promise::rhi;

    std::expected<rhi::submission_token, rhi::error> core::command_submission_view::submit(rhi::command_buffer& commands) {
        if (!owner) {
            return std::unexpected(rhi::error::not_ready);
        }
        rhi::submission_token token{};
        auto result = owner->submit_commands(commands, &token);
        if (result != rhi::error::ok) {
            return std::unexpected(result);
        }
        return token;
    }
    std::expected<rhi::submission_token, rhi::error> core::command_submission_view::submit(std::span<rhi::mergeable_command_buffer_ref const> batch) {
        if (!owner) {
            return std::unexpected(rhi::error::not_ready);
        }
        rhi::submission_token token{};
        auto result = owner->submit_batch(batch, &token);
        if (result != rhi::error::ok) {
            return std::unexpected(result);
        }
        return token;
    }
    rhi::error core::command_submission_view::poll(rhi::submission_token token) noexcept {
        return owner ? owner->check_completion(token, false, 0) : rhi::error::not_ready;
    }
    rhi::error core::command_submission_view::wait(rhi::submission_token token, std::uint64_t timeout_ns) noexcept {
        return owner ? owner->check_completion(token, true, timeout_ns) : rhi::error::not_ready;
    }
    rhi::error core::check_completion(rhi::submission_token token, bool wait, std::uint64_t timeout_ns) noexcept {
        VkSemaphore timeline;
        {
            std::lock_guard lock(submissions_mutex);
            if (token.device_id != completion_device_id || token.sequence == 0 || token.sequence > completion_sequence) {
                return rhi::error::invalid_argument;
            }
            if (completion_failure != rhi::error::ok) {
                return completion_failure;
            }
            timeline = completion_timeline;
        }
        // The core owns this semaphore until teardown; callers keep the core alive.
        // Never block while holding queue/submission locks: another host thread can submit or signal.
        VkResult status;
        if (wait) {
            VkSemaphoreWaitInfo info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                     .pNext = nullptr,
                                     .flags = 0,
                                     .semaphoreCount = 1,
                                     .pSemaphores = &timeline,
                                     .pValues = &token.sequence};
            status = vkWaitSemaphores(logical_device, &info, timeout_ns);
        } else {
            std::uint64_t reached = 0;
            status = vkGetSemaphoreCounterValue(logical_device, timeline, &reached);
            if (status == VK_SUCCESS && reached < token.sequence) {
                return rhi::error::not_ready;
            }
        }
        if (status == VK_SUCCESS) {
            poll_submissions();
            return rhi::error::ok;
        }
        if (status == VK_TIMEOUT) {
            return rhi::error::timeout;
        }
        if (status == VK_ERROR_DEVICE_LOST) {
            std::lock_guard lock(submissions_mutex);
            completion_failure = rhi::error::device_lost;
            return completion_failure;
        }
        if (status == VK_ERROR_OUT_OF_HOST_MEMORY) {
            return rhi::error::out_of_host_memory;
        }
        if (status == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            return rhi::error::out_of_device_memory;
        }
        return rhi::error::operation_failed;
    }

    core::pending_submission::~pending_submission() noexcept {
        if (submitted) {
            for (auto const& secondary : secondaries) {
                secondary->pending.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
        if (submitted && primary) {
            primary->pending.fetch_sub(1, std::memory_order_acq_rel);
        }
        if (fence != VK_NULL_HANDLE) {
            vkDestroyFence(owner->logical_device, fence, nullptr);
        }
    }

    void core::poll_submissions(bool all) const noexcept {
        deren::utility::vessel::vector<std::unique_ptr<pending_submission>> completed;
        {
            std::lock_guard lock(submissions_mutex);
            for (auto i = pending_submissions.begin(); i != pending_submissions.end();) {
                auto const& submission = **i;
                bool done = all;
                if (!done && submission.fence != VK_NULL_HANDLE) {
                    done = vkGetFenceStatus(logical_device, submission.fence) == VK_SUCCESS;
                }
                if (!done && submission.timeline != VK_NULL_HANDLE) {
                    std::uint64_t reached = 0;
                    done = vkGetSemaphoreCounterValue(logical_device, submission.timeline, &reached) == VK_SUCCESS &&
                           reached >= submission.value;
                }
                if (done) {
                    completed.push_back(std::move(*i));
                    i = pending_submissions.erase(i);
                } else {
                    ++i;
                }
            }
        }
        // Destructors can enter the command/image/group registries, so unlock first.
    }
} // namespace deren::vulkan
