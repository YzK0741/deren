module;

#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.vstd;
import deren.utility.shared;
import deren.vulkan.constant_init;
import :declarations;

namespace deren::vulkan {
    std::shared_ptr<core::command_buffer> core::reference_command(core::command_buffer& command, bool recorded_secondary) {
        command.references.fetch_add(1, std::memory_order_relaxed);
        if (recorded_secondary) {
            command.recording_users.fetch_add(1, std::memory_order_relaxed);
        }
        return std::shared_ptr<core::command_buffer>(&command, [recorded_secondary](core::command_buffer* value) {
            if (recorded_secondary) {
                value->recording_users.fetch_sub(1, std::memory_order_acq_rel);
            }
            value->release();
        });
    }

    namespace rhi = deren::promise::rhi;

    rhi::error core::submission_timing_view::begin(rhi::command_buffer& commands) noexcept {
        rhi::error result{};
        auto* recorder = parent->resolve_recorder(&commands, result);
        if (!recorder) {
            return result;
        }
        if (recorder->target == VK_NULL_HANDLE || recorder->rendering_active || recorder->simultaneous_use) {
            return rhi::error::invalid_argument;
        }
        auto& primary = static_cast<core::command_buffer&>(*recorder);
        if (primary.kind != rhi::command_buffer_kind::primary || primary.submission_timestamp_marks != 0) {
            return rhi::error::invalid_argument;
        }
        if (auto applied = primary.apply_operations(); applied != rhi::error::ok) {
            return applied;
        }
        if (primary.submission_timestamps == VK_NULL_HANDLE) {
            VkQueryPoolCreateInfo const info = make_query_pool_info(VK_QUERY_TYPE_TIMESTAMP, 2);
            if (vkCreateQueryPool(parent->device.logical_device, &info, nullptr, &primary.submission_timestamps) != VK_SUCCESS) {
                return rhi::error::operation_failed;
            }
        }
        vkCmdResetQueryPool(primary.native(), primary.submission_timestamps, 0, 2);
        vkCmdWriteTimestamp(primary.native(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, primary.submission_timestamps, 0);
        primary.submission_timestamp_marks = 1;
        return rhi::error::ok;
    }

    rhi::error core::submission_timing_view::end(rhi::command_buffer& commands) noexcept {
        rhi::error result{};
        auto* recorder = parent->resolve_recorder(&commands, result);
        if (!recorder) {
            return result;
        }
        if (recorder->target == VK_NULL_HANDLE || recorder->rendering_active) {
            return rhi::error::invalid_argument;
        }
        auto& primary = static_cast<core::command_buffer&>(*recorder);
        if (primary.submission_timestamp_marks != 1) {
            return rhi::error::not_ready;
        }
        if (auto applied = primary.apply_operations(); applied != rhi::error::ok) {
            return applied;
        }
        vkCmdWriteTimestamp(primary.native(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, primary.submission_timestamps, 1);
        primary.submission_timestamp_marks = 2;
        return rhi::error::ok;
    }

    std::expected<std::uint64_t, rhi::error> core::submission_timing_view::elapsed_ns(rhi::command_buffer& commands) noexcept {
        std::lock_guard lock(parent->command.contract_command_buffers_mutex);
        if (!parent->command.contract_command_buffers.contains(&commands)) {
            return std::unexpected(rhi::error::invalid_argument);
        }
        auto& primary = static_cast<core::command_buffer&>(commands);
        if (primary.submission_timestamp_marks != 2 || !primary.submission_timestamp_submitted || primary.pending.load(std::memory_order_acquire) != 0) {
            return std::unexpected(rhi::error::not_ready);
        }
        std::uint64_t ticks[2]{};
        VkResult const result = vkGetQueryPoolResults(parent->device.logical_device, primary.submission_timestamps, 0, 2,
                                                      sizeof(ticks), ticks, sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT);
        if (result != VK_SUCCESS) {
            return std::unexpected(result == VK_NOT_READY ? rhi::error::not_ready : rhi::error::operation_failed);
        }
        auto const bits = parent->frame.timestamp_valid_bits;
        std::uint64_t const mask = bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
        return static_cast<std::uint64_t>(static_cast<double>((ticks[1] - ticks[0]) & mask) * parent->frame.timestamp_period_ns);
    }

    std::expected<deren::promise::rhi::submission_token, deren::promise::rhi::error>
    core::command_submission_view::submit(deren::promise::rhi::command_buffer& commands) {
        if (!parent) {
            return std::unexpected(rhi::error::not_ready);
        }
        rhi::submission_token token{};
        auto result = parent->submit_commands(commands, &token);
        if (result != rhi::error::ok) {
            return std::unexpected(result);
        }
        return token;
    }
    std::expected<deren::promise::rhi::submission_token, deren::promise::rhi::error>
    core::command_submission_view::submit(std::span<deren::promise::rhi::mergeable_command_buffer_ref const> batch) {
        if (!parent) {
            return std::unexpected(rhi::error::not_ready);
        }
        rhi::submission_token token{};
        auto result = parent->submit_batch(batch, &token);
        if (result != rhi::error::ok) {
            return std::unexpected(result);
        }
        return token;
    }
    rhi::error core::command_submission_view::poll(rhi::submission_token token) noexcept {
        return parent ? parent->check_completion(token, false, 0) : rhi::error::not_ready;
    }
    rhi::error core::command_submission_view::wait(rhi::submission_token token, std::uint64_t timeout_ns) noexcept {
        return parent ? parent->check_completion(token, true, timeout_ns) : rhi::error::not_ready;
    }
    rhi::error core::check_completion(rhi::submission_token token, bool wait, std::uint64_t timeout_ns) noexcept {
        VkSemaphore timeline;
        {
            std::lock_guard lock(command.submissions_mutex);
            if (token.device_id != command.completion_device_id || token.sequence == 0 || token.sequence > command.completion_sequence) {
                return rhi::error::invalid_argument;
            }
            if (command.completion_failure != rhi::error::ok) {
                return command.completion_failure;
            }
            timeline = command.completion_timeline;
        }
        // The semaphore stays valid until the core tears down; the core must not die before this call returns.
        // Never block with the queue/submission locks taken: another host thread can submit or signal.
        VkResult status;
        if (wait) {
            VkSemaphoreWaitInfo const info = make_semaphore_wait_info(1, &timeline, &token.sequence);
            status = vkWaitSemaphores(this->device.logical_device, &info, timeout_ns);
        } else {
            std::uint64_t reached = 0;
            status = vkGetSemaphoreCounterValue(this->device.logical_device, timeline, &reached);
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
            std::lock_guard lock(command.submissions_mutex);
            command.completion_failure = rhi::error::device_lost;
            return command.completion_failure;
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
            vkDestroyFence(parent->device.logical_device, fence, nullptr);
        }
    }

    void core::poll_submissions(bool all) const noexcept {
        deren::utility::vessel::vector<std::unique_ptr<pending_submission>> completed;
        {
            std::lock_guard lock(command.submissions_mutex);
            for (auto i = command.pending_submissions.begin(); i != command.pending_submissions.end();) {
                auto const& submission = **i;
                bool done = all;
                if (!done && submission.fence != VK_NULL_HANDLE) {
                    done = vkGetFenceStatus(this->device.logical_device, submission.fence) == VK_SUCCESS;
                }
                if (!done && submission.timeline != VK_NULL_HANDLE) {
                    std::uint64_t reached = 0;
                    done = vkGetSemaphoreCounterValue(this->device.logical_device, submission.timeline, &reached) == VK_SUCCESS &&
                           reached >= submission.value;
                }
                if (done) {
                    completed.push_back(std::move(*i));
                    i = command.pending_submissions.erase(i);
                } else {
                    ++i;
                }
            }
        }
        // Destructors can enter the command/image/group registries, so unlock first.
    }
} // namespace deren::vulkan
