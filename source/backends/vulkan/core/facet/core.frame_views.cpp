// -*- C++ -*-
// ============================================================================
// file: source/backends/vulkan/core/facet/core.frame_views.cpp
//
// The frame face's views and the frame/profiler entry points, split
// out of core.api_core.cpp. This translation unit defines no separate state:
// every definition here is a member of `core` over the frame and command
// facets, exactly as before the move.
// ============================================================================
module;

#include <vulkan/vulkan.h>

module deren.vulkan.core;
import deren.vstd;
import deren.utility.shared;
import deren.promise.rhi;
import deren.promise.render_operations;
import deren.vulkan.constant_init;
import deren.vulkan.core.pipeline;
import :declarations;

namespace deren::vulkan {

    namespace rhi = deren::promise::rhi;

    VkResult core::acquire_next_image(std::uint32_t& image_index) {
        frame.frame_acquire_consumed = false;
        if (this->create_options.native_window == nullptr) {
            image_index = static_cast<std::uint32_t>(frame.current_frame);
            frame.acquired_image_index = image_index;
            frame.frame_in_flight = frame.frame_acquired = true;
            ++frame.frame_acquire_epoch;
            frame.async_present_queued = false;
            return VK_SUCCESS;
        }
        // The frame slot in progress: submit() and wait_frame_slot() use `frame.current_frame` for it, and
        // to_next_frame() advances it, so the acquire semaphore below is the one that slot's submission
        // will wait on (VUID-vkAcquireNextImageKHR-semaphore-01779 needs the slot to be idle - the
        // caller's wait_frame_slot() is what guarantees that).
        std::uint32_t const slot = static_cast<std::uint32_t>(this->frame.current_frame);
        VkResult result = VK_SUCCESS;
        if (this->frame.prefetched_slot == slot) {
            image_index = this->frame.prefetched_image;
            this->frame.prefetched_slot = std::numeric_limits<std::uint32_t>::max();
        } else {
            // acquire requires exclusive host access to this swapchain. If prefetch could not
            // acquire an image, wait for the worker before accessing the WSI object again.
            if (this->frame.present_pending.valid()) {
                this->frame.present_pending.wait();
            }
            result = vkAcquireNextImageKHR(this->device.logical_device, this->frame.swap_chain, std::numeric_limits<std::uint64_t>::max(),
                                           this->frame.image_available_semaphores[slot], VK_NULL_HANDLE, &image_index);
        }
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            // THE FRAME IN FLIGHT IS NOW THIS IMAGE, and that is all `frame.frame_in_flight` means: from here
            // until submit() hands the frame over, `begin_commands()` and `frame_image()` answer for it.
            // Both spellings go through this one primitive - the contract's frame_begin() and the
            // runtime's pacing path - so neither can be "in flight" without the other knowing.
            this->frame.acquired_image_index = image_index;
            this->frame.async_present_queued = false;
            this->frame.frame_in_flight = true;
            // THE FRAME'S RECORDING TARGET: while this frame is in flight the view's shared head
            // names the frame slot's buffer; the frame's submit/advance clears it. (The GUI overlay no
            // longer records here - gui abi 5 gives the plugin its two mergeable segments.)
            this->frame.commands_view.set_target(this->frame_command_buffer());
            // and this is what makes `frame_image()` answer at all: before the first successful acquire
            // there is no last-acquired image, only a default index that happens to name one.
            this->frame.frame_acquired = true;
            ++this->frame.frame_acquire_epoch;
        }
        return result;
    }

    void core::wait_idle() {
        // The const facade call (source/backends/vulkan/core/core.cpp) is the implementation; the contract's virtual is
        // not const, so this overload exists to forward into it.
        core const& self = *this;
        self.wait_idle();
    }

    rhi::error core::submit_commands(rhi::command_buffer& commands, rhi::submission_token* token) {
        if (token) {
            *token = {};
            if (&commands == &frame.commands_view) {
                return rhi::error::unsupported;
            }
        }
        // THE FRAME'S LIST first, which is the shape this verb had from the start.
        if (&commands != static_cast<rhi::command_buffer*>(&this->frame.commands_view)) {
            // ---- AND THE CALLER'S LIST (plan X4) ------------------------------------------------------
            //
            // WHAT THIS WIDENING IS FOR, and it is a WIDENING rather than a new slot (the signature does not
            // move, so abi 27 stands): the probes record their read-back into a buffer THEY created
            // (`create_command_buffer()`), and they used to hand it to the queue through the escape with a raw
            // `vkQueueSubmit` (the escape is deleted in abi 43) - the last Vulkan call the engine half made.
            // A one-shot list the caller records into is the same operation the frame's list is (record -> submit to
            // the graphics queue); what differs is
            // that NOTHING IS PRESENTED, so the acquire state, the swapchain image and the semaphores are not
            // involved. THE ORDERING STAYS THE CALLER'S (`wait_idle()`), exactly as it was when it submitted by
            // hand - the contract has no fence for a caller-created buffer, and inventing one is not this batch's call.
            {
                std::lock_guard const lock(this->command.contract_command_buffers_mutex);
                if (!this->command.contract_command_buffers.contains(&commands)) {
                    return rhi::error::invalid_argument; // a list this backend did not hand out (provenance)
                }
            }
            auto& concrete = *rhi::do_concrete<core::command_buffer>(commands, concrete::command_buffer);
            if (concrete.kind != rhi::command_buffer_kind::primary) {
                return rhi::error::invalid_argument;
            }
            if (!concrete.executable || concrete.recording || concrete.pending.load(std::memory_order_acquire)) {
                return rhi::error::not_ready;
            }
            VkCommandBuffer const native = *concrete.buffer;
            if (native == VK_NULL_HANDLE || this->device.graphics_queue_handle == VK_NULL_HANDLE) {
                return rhi::error::not_ready;
            }
            VkSubmitInfo one_shot = make_submit_info(&native);
            auto submission = std::make_unique<pending_submission>();
            submission->parent = this;
            submission->primary = reference_command(concrete, false);
            submission->groups = concrete.group_refs;
            submission->resources = concrete.resource_refs;
            submission->lazy_refs = concrete.lazy_refs;
            submission->secondaries = concrete.secondary_refs;
            std::lock_guard lock(command.submissions_mutex);
            if (token) {
                if (command.completion_failure != rhi::error::ok) {
                    return command.completion_failure;
                }
                if (command.completion_sequence == std::numeric_limits<std::uint64_t>::max()) {
                    return rhi::error::unsupported;
                }
                if (command.completion_timeline == VK_NULL_HANDLE) {
                    constexpr VkSemaphoreTypeCreateInfo type = make_timeline_semaphore_type_info();
                    VkSemaphoreCreateInfo create = make_binary_semaphore_info();
                    create.pNext = &type;
                    auto const created = vkCreateSemaphore(this->device.logical_device, &create, nullptr, &command.completion_timeline);
                    if (created != VK_SUCCESS) {
                        return generic_error(created);
                    }
                }
                submission->timeline = command.completion_timeline;
                submission->value = command.completion_sequence + 1;
            } else {
                constexpr VkFenceCreateInfo fence_info = make_fence_info();
                auto const created = vkCreateFence(this->device.logical_device, &fence_info, nullptr, &submission->fence);
                if (created != VK_SUCCESS) {
                    return generic_error(created);
                }
            }
            VkTimelineSemaphoreSubmitInfo const signal =
                make_timeline_semaphore_info(0, nullptr, token ? 1u : 0u, &submission->value);
            if (token) {
                one_shot.pNext = &signal;
                one_shot.signalSemaphoreCount = 1;
                one_shot.pSignalSemaphores = &command.completion_timeline;
            }
            for (auto const& secondary : submission->secondaries) {
                if (!secondary->executable || (!secondary->simultaneous_use && secondary->pending.load(std::memory_order_acquire))) {
                    return rhi::error::not_ready;
                }
            }
            command.pending_submissions.reserve(command.pending_submissions.size() + 1);
            std::lock_guard queue_lock(this->device.vma.queue_access());
            auto const submitted = vkQueueSubmit(this->device.graphics_queue_handle, 1, &one_shot, submission->fence);
            if (submitted == VK_SUCCESS) {
                concrete.submission_timestamp_submitted = true;
                concrete.pending.fetch_add(1, std::memory_order_release);
                if (concrete.one_time_submit) {
                    concrete.executable = false;
                }
                for (auto const& secondary : submission->secondaries) {
                    secondary->pending.fetch_add(1, std::memory_order_release);
                    if (secondary->one_time_submit) {
                        secondary->executable = false;
                    }
                }
                if (token) {
                    command.completion_sequence = submission->value;
                    *token = {command.completion_device_id, command.completion_sequence};
                }
                submission->submitted = true;
                command.pending_submissions.push_back(std::move(submission));
            }
            if (submitted == VK_ERROR_DEVICE_LOST) {
                command.completion_failure = rhi::error::device_lost;
            }
            return generic_error(submitted);
        }
        if (!this->frame.frame_in_flight) {
            return rhi::error::not_ready;
        }
        // The image and the present-ready semaphore are this backend's acquire state - never
        // caller data (the contract's note). The raw failure becomes the generic translation; the
        // device-level codes travel as themselves.
        report_operation_counts();
        return generic_error(this->submit_frame(this->frame_command_buffer(), this->frame.acquired_image_index));
    }

    // ---- the frame face (abi 13) ----------------------------------------------------------------
    //
    // THE ACCESSORS hand out this core's two views - the same object every call, with `parent` set by
    // the constructor. The views' methods compose the machinery this class already had: the walker is
    // today's frame prologue fused (wait -> latch timings -> acquire -> report), the profiler reads the
    // timing snapshot the walker's latch produced.

    std::uint32_t core::frame_walker_view::slot_count() const noexcept {
        return static_cast<std::uint32_t>(this->parent->frame.frames_in_flight);
    }

    std::uint32_t core::frame_walker_view::position() const noexcept {
        // THE one authority: this is the backend's cursor, read without advancing.
        return static_cast<std::uint32_t>(this->parent->frame.current_frame);
    }

    rhi::frame_open_info core::frame_walker_view::wait_and_acquire() {
        // THE FIVE STEPS in the order the contract's note spells out - each position is load-bearing.
        std::uint32_t const slot = static_cast<std::uint32_t>(this->parent->frame.current_frame);

        // 1 + 2. the cursor (read, not advanced) and its timeline. A never-submitted slot (value 0)
        // has nothing to wait for - VK_SUCCESS by definition, the same early return as today.
        VkResult const waited = this->parent->wait_frame_slot_result(slot);
        if (waited != VK_SUCCESS) {
            // The wait's VkResult, REPORTED instead of dropped (today `wait_frame_slot` voids it):
            // the code says what class of failure, the message names the step, native_code carries
            // the raw VkResult for whoever needs the exact number.
            return {.frame = {}, .result = deren::vulkan::failed(deren::vulkan::generic_error(waited), waited, "waiting the frame slot's timeline failed")};
        }

        // 3. LATCH this slot's previous frame's GPU timings, here between the wait and the acquire -
        // exactly where the engine's collect_gpu_timings sits today, so the wait already guarantees
        // the timestamps are readable AND a frame the acquire kills on OUT_OF_DATE is still
        // collected. Behaviour unchanged; the position is the backend's now.
        this->parent->frame.profiler_view.latch(slot);

        // 4. the acquire - the same primitive `frame_begin()` goes through (one mechanism, two
        // spellings), translated AT THIS CALL SITE: SUBOPTIMAL is ok here (the acquired image
        // renders), OUT_OF_DATE is the rebuild signal the caller's classifier acts on.
        std::uint32_t image_index = 0;
        VkResult const acquired = this->parent->acquire_next_image(image_index);
        rhi::error const opened = deren::vulkan::acquire_error(acquired);
        if (opened != rhi::error::ok) {
            // The zero-frame rule: `frame` is zeroed and unusable, and the caller now knows WHY
            // (out_of_date => rebuild and skip; device_lost => fatal; ...) - the information
            // `frame_begin()`'s zeroed submit_info could not carry.
            return {.frame = {}, .result = deren::vulkan::failed(opened, acquired, "acquiring the next swapchain image failed")};
        }

        // 5. the frame is open: this slot, the acquired image, and a zeroed diagnostic saying ok.
        return {.frame = {.frame_index = slot, .image_index = image_index}, .result = {}};
    }

    void core::frame_walker_view::walk_to_next() noexcept {
        // The present-side close: advance ONE slot. It does not promise "the frame is over" - only
        // that the ring moved (the engine calls this after its present recipe).
        this->parent->to_next_frame();
    }

    void core::gpu_profiler_view::latch(std::uint32_t const slot) noexcept {
        // Non-const on purpose: the latch writes the read-once guard (a slot's timings are fetched
        // at most once per submission), the same bookkeeping read_gpu_timings keeps.
        core& self = *this->parent;
        this->latch_failed = false;
        if (!self.frame.gpu_timing_supported) {
            this->latched_mark_count = 0; // the device cannot timestamp: the report is honestly empty
            return;
        }
        std::uint64_t const submitted = self.frame.frame_done_values[slot];
        if (submitted == 0 || submitted <= self.frame.gpu_timing_read_value[slot]) {
            return; // nothing new since the previous latch: the snapshot stays as it was
        }
        self.frame.gpu_timing_read_value[slot] = submitted; // this submission is now accounted for (once)
        std::uint32_t const marks = self.frame.gpu_timing_marks[slot];
        this->latched_mark_count = marks;
        if (marks == 0) {
            return; // a submission with no marks has nothing to fetch
        }
        deren::utility::vessel::array<std::uint64_t, gpu_timing_mark_capacity> ticks = {};
        VkResult const status = vkGetQueryPoolResults(self.device.logical_device, self.frame.timestamp_query_pool, slot * gpu_timing_mark_capacity, marks,
                                                      sizeof(std::uint64_t) * marks, ticks.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
        if (status == VK_NOT_READY) {
            // The timeline wait already said the submission completed, so this is the same defensive
            // reading read_gpu_timings has: report no measurement rather than waiting or stalling.
            this->latched_mark_count = 0;
            return;
        }
        if (status != VK_SUCCESS) {
            // A real read-back failure: the mark count is host-known, the ticks are not - the
            // getters answer `device_lost` for it instead of reporting silence.
            this->latch_failed = true;
            return;
        }
        this->latched_ticks = ticks;
        for (std::uint32_t mark = 0; mark < marks; ++mark) {
            // The names are by-value string_views of the engine's static text (the `window_title`
            // rule), copied out at latch time - the array below may be overwritten by the next
            // recording, the snapshot must not move with it.
            this->latched_names[mark] = self.frame.gpu_timing_names[slot][mark];
        }
    }

    std::uint32_t core::gpu_profiler_view::stage_count() const noexcept {
        if (!this->parent->frame.gpu_timing_supported) {
            return 0; // "no timing => always 0": the caller reads this as unsupported via get_stage_info
        }
        return this->latched_mark_count;
    }

    rhi::error core::gpu_profiler_view::get_stage_info(std::uint32_t const index, std::string_view* const name,
                                                       std::uint64_t* const duration_ns) const noexcept {
        if (!this->parent->frame.gpu_timing_supported) {
            return rhi::error::unsupported; // the device or configuration has no timing to report
        }
        if (index >= this->latched_mark_count) {
            return rhi::error::invalid_argument;
        }
        if (this->latch_failed) {
            return rhi::error::device_lost; // the timestamp read-back failed; the count is all we know
        }
        if (index + 1 >= this->latched_mark_count) {
            return rhi::error::not_ready; // the last mark closes the frame and opens no stage
        }
        if (name != nullptr) {
            *name = this->latched_names[index];
        }
        if (duration_ns != nullptr) {
            // The same masking the milliseconds reader does: the counter is a modulo-2^valid_bits
            // ring, so the delta is taken inside the width (a wrap inside the span comes out right),
            // and the device's timestampPeriod converts ticks to nanoseconds (1 ns/tick with 64
            // valid bits on the device this backend ships on - the integer is lossless there).
            std::uint64_t const mask = this->parent->frame.timestamp_valid_bits >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << this->parent->frame.timestamp_valid_bits) - 1);
            std::uint64_t const delta = (this->latched_ticks[index + 1] - this->latched_ticks[index]) & mask;
            *duration_ns = static_cast<std::uint64_t>(static_cast<double>(delta) * static_cast<double>(this->parent->frame.timestamp_period_ns));
        }
        return rhi::error::ok;
    }

    rhi::stage_time_map core::get_time_profiling() const {
        std::lock_guard lock(this->time_profiling_mutex);
        return this->time_profiling;
    }

    void core::record_stage_time(std::string_view const name, double const milliseconds) {
        std::lock_guard lock(this->time_profiling_mutex);
        auto [slot, inserted] = this->time_profiling.try_emplace(std::string(name));
        slot->second.tick(milliseconds);
    }

    void core::reset_time_profiling_window() {
        std::lock_guard lock(this->time_profiling_mutex);
        for (auto& info : this->time_profiling | std::views::values) {
            info.reset_window();
        }
    }
} // namespace deren::vulkan

namespace deren::vulkan {

    void core::report_operation_counts() noexcept {
        if (std::getenv("DEREN_RENDER_OPERATION_STATS")) {
            std::uint64_t total = 0;
            for (auto count : operation_requests) {
                total += count;
            }
            deren::utility::log("render operations: total={}", total);
            deren::utility::log("render operations lowered: requested={} deferred={} setters={} work={}",
                                deferred_requested, deferred_referenced, emitted_setters, emitted_work);
            if (operation_requests[0]) {
                deren::utility::log("render operations: push_constants={}", operation_requests[0]);
            }
            if (operation_requests[1]) {
                deren::utility::log("render operations: execute={}", operation_requests[1]);
            }
            if (operation_requests[2]) {
                deren::utility::log("render operations: use={}", operation_requests[2]);
            }
            if (operation_requests[3]) {
                deren::utility::log("render operations: copy_image_to_buffer={}", operation_requests[3]);
            }
            if (operation_requests[4]) {
                deren::utility::log("render operations: barrier={}", operation_requests[4]);
            }
            if (operation_requests[5]) {
                deren::utility::log("render operations: begin_rendering={}", operation_requests[5]);
            }
            if (operation_requests[6]) {
                deren::utility::log("render operations: end_rendering={}", operation_requests[6]);
            }
            if (operation_requests[7]) {
                deren::utility::log("render operations: bind_pipeline={}", operation_requests[7]);
            }
            if (operation_requests[8]) {
                deren::utility::log("render operations: bind_vertex_buffer={}", operation_requests[8]);
            }
            if (operation_requests[9]) {
                deren::utility::log("render operations: bind_index_buffer={}", operation_requests[9]);
            }
            if (operation_requests[10]) {
                deren::utility::log("render operations: draw={}", operation_requests[10]);
            }
            if (operation_requests[11]) {
                deren::utility::log("render operations: draw_indexed={}", operation_requests[11]);
            }
            if (operation_requests[12]) {
                deren::utility::log("render operations: dispatch_indirect={}", operation_requests[12]);
            }
            if (operation_requests[13]) {
                deren::utility::log("render operations: dispatch={}", operation_requests[13]);
            }
            if (operation_requests[14]) {
                deren::utility::log("render operations: draw_mesh_tasks={}", operation_requests[14]);
            }
            if (operation_requests[15]) {
                deren::utility::log("render operations: trace_rays={}", operation_requests[15]);
            }
            if (operation_requests[16]) {
                deren::utility::log("render operations: build_micromap={}", operation_requests[16]);
            }
            if (operation_requests[17]) {
                deren::utility::log("render operations: build_acceleration_structure={}", operation_requests[17]);
            }
            if (operation_requests[18]) {
                deren::utility::log("render operations: refit_acceleration_structure={}", operation_requests[18]);
            }
            if (operation_requests[19]) {
                deren::utility::log("render operations: draw_indirect={}", operation_requests[19]);
            }
            if (operation_requests[20]) {
                deren::utility::log("render operations: draw_mesh_tasks_indirect={}", operation_requests[20]);
            }
            if (operation_requests[21]) {
                deren::utility::log("render operations: set_viewport={}", operation_requests[21]);
            }
            if (operation_requests[22]) {
                deren::utility::log("render operations: set_scissor={}", operation_requests[22]);
            }
            if (operation_requests[23]) {
                deren::utility::log("render operations: set_cull_mode={}", operation_requests[23]);
            }
            if (operation_requests[24]) {
                deren::utility::log("render operations: set_depth_write={}", operation_requests[24]);
            }
            if (operation_requests[25]) {
                deren::utility::log("render operations: set_depth_bias={}", operation_requests[25]);
            }
            if (operation_requests[26]) {
                deren::utility::log("render operations: copy_image={}", operation_requests[26]);
            }
            if (operation_requests[27]) {
                deren::utility::log("render operations: copy_buffer={}", operation_requests[27]);
            }
            if (operation_requests[28]) {
                deren::utility::log("render operations: clear_color_image={}", operation_requests[28]);
            }
            if (operation_requests[29]) {
                deren::utility::log("render operations: begin_gpu_timing={}", operation_requests[29]);
            }
            if (operation_requests[30]) {
                deren::utility::log("render operations: mark_gpu_timing={}", operation_requests[30]);
            }
            if (operation_requests[31]) {
                deren::utility::log("render operations: load_resource_group={}", operation_requests[31]);
            }
            if (operation_requests[32]) {
                deren::utility::log("render operations: load_texture_group={}", operation_requests[32]);
            }
        }
        operation_requests.fill(0);
        deferred_requested = deferred_referenced = emitted_setters = emitted_work = 0;
    }
} // namespace deren::vulkan
