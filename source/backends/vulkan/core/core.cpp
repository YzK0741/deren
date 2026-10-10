module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

// LOAD-BEARING, and it is the same trap chores.cpp documents at length: with -fno-exceptions and the vendored
// std module, a TU that instantiates std::vector sees TWO 'operator new(size_t, align_val_t)' declarations -
// module std's and the textual libc++ copy baked into utility:data_block.pcm - and resolves neither, which is
// "call to operator new is ambiguous" at allocate.h. This file instantiates plenty of std::vector (the device
// extension-name list, the descriptor pool sizes), and it began seeing both the moment deren.vulkan.core gained an
// import edge it did not have before: descriptor_heap, whose own module carries a textual Vulkan header in its
// global module fragment. Textually including glm here makes clang MERGE the two copies, exactly as it does for
// chores.cpp and source/engine/animation/controller.cpp. Do not remove this include to "clean up".
#include <glm/glm.hpp>
module deren.vulkan.core;
import deren.vulkan.core.pipeline;
import :init_utils;
import deren.vulkan.constant_init;

// The contract's types are named by the tables' declarations, so the import comes first (see the header).
import deren.promise.rhi;

namespace deren::vulkan {
    // The contract's spelling, local to this TU: the frame verbs below return `rhi::error` /
    // `rhi::image_extent`, and core.api_core.cpp carries the same alias (core.entry.cpp has it at
    // global scope). Without it every `rhi::` in this file is "did you mean promise::rhi?".
    namespace rhi = deren::promise::rhi;

    void core::begin_gpu_timing(VkCommandBuffer const command_buffer, uint32_t const slot) noexcept {
        this->gpu_timing_marks[slot] = 0;
        if (!this->gpu_timing_supported) {
            return;
        }
        // Reset on the GPU timeline: the query range may still be "in use" from the host's point of
        // view, and a recorded reset is ordered against the writes that follow it in the same
        // command buffer - a host-side vkResetQueryPool would need the slot to be idle, which is a
        // constraint the caller would have to remember on every path.
        vkCmdResetQueryPool(command_buffer, this->timestamp_query_pool, slot * gpu_timing_mark_capacity, gpu_timing_mark_capacity);
    }

    void core::mark_gpu_timing(VkCommandBuffer const command_buffer, uint32_t const slot, VkPipelineStageFlagBits const stage,
                               std::string_view const stage_name) noexcept {
        if (!this->gpu_timing_supported || this->gpu_timing_marks[slot] >= gpu_timing_mark_capacity) {
            return;
        }
        // The name rides the mark: static text the caller owns, stored as a view and reported
        // verbatim by the profiler's latched snapshot (the `window_title` rule - the characters
        // never move into this class).
        this->gpu_timing_names[slot][this->gpu_timing_marks[slot]] = stage_name;
        vkCmdWriteTimestamp(command_buffer, stage, this->timestamp_query_pool, slot * gpu_timing_mark_capacity + this->gpu_timing_marks[slot]);
        ++this->gpu_timing_marks[slot];
    }

    gpu_timing_result core::read_gpu_timings(uint32_t const slot) {
        gpu_timing_result result = {};
        if (!this->gpu_timing_supported) {
            return result;
        }
        // Only read a submitted slot, and only once per submission: the caller paced the slot, so
        // the queries of its last submission are complete, while a slot whose frame failed before
        // recording has nothing new to report.
        uint64_t const submitted = this->frame_done_values[slot];
        if (submitted == 0 || submitted <= this->gpu_timing_read_value[slot]) {
            return result;
        }
        this->gpu_timing_read_value[slot] = submitted; // this submission is now accounted for
        uint32_t const marks = this->gpu_timing_marks[slot];
        if (marks < 2) {
            return result; // a single mark has no interval to report
        }

        std::array<uint64_t, gpu_timing_mark_capacity> ticks = {};
        VkResult const status = vkGetQueryPoolResults(this->logical_device,
                                                      this->timestamp_query_pool,
                                                      slot * gpu_timing_mark_capacity,
                                                      marks,
                                                      sizeof(uint64_t) * marks,
                                                      ticks.data(),
                                                      sizeof(uint64_t),
                                                      VK_QUERY_RESULT_64_BIT);
        if (status != VK_SUCCESS) {
            // VK_NOT_READY (or a lost pool): report "no measurement" rather than waiting - a frame
            // without a timing line is fine, a stalled frame is not.
            return result;
        }

        result.mark_count = marks;
        for (uint32_t mark = 0; mark + 1 < marks; ++mark) {
            result.milliseconds[mark] = deren::utility::timestamp_delta_milliseconds(ticks[mark], ticks[mark + 1], this->timestamp_valid_bits, this->timestamp_period_ns);
        }
        return result;
    }

    VkCommandBuffer core::frame_commands::native() const noexcept {
        // A LIST KNOWS THE BUFFER IT RECORDS INTO (abi 15): `target` is what an owned command buffer's
        // `recording()` sets, and a null target means the FRAME's slot buffer - the meaning every abi
        // 13/14 call site already has. The frame's answer comes from the same method `submit_frame` and
        // the heap requests use, so there is no second source of truth for it.
        if (this->target != VK_NULL_HANDLE) {
            return this->target;
        }
        return this->owner != nullptr ? this->owner->frame_command_buffer() : VK_NULL_HANDLE;
    }

    rhi::error core::frame_commands::begin_gpu_timing() noexcept {
        core& owner = *this->owner;
        if (!owner.gpu_timing_supported) {
            return rhi::error::unsupported; // the device cannot timestamp: the report stays honestly empty
        }
        if (!owner.frame_in_flight) {
            return rhi::error::not_ready; // the same window `use()` refuses in
        }
        if (this->target != VK_NULL_HANDLE) {
            // THE TIMING RANGE IS THE FRAME SLOT'S (abi 15): the queries live in that slot's slice of
            // the query pool and the profiler reads that slot, so a list recording into another buffer
            // has no range to open - the frame-scoped verbs' own `not_ready` window.
            return rhi::error::not_ready;
        }
        owner.begin_gpu_timing(this->native(), static_cast<uint32_t>(owner.current_frame));
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::mark_gpu_timing(std::uint32_t const mark_index, std::string_view const stage_name) noexcept {
        core& owner = *this->owner;
        if (!owner.gpu_timing_supported) {
            return rhi::error::unsupported;
        }
        if (!owner.frame_in_flight) {
            return rhi::error::not_ready;
        }
        if (this->target != VK_NULL_HANDLE) {
            return rhi::error::not_ready; // the frame slot's range again (see begin_gpu_timing)
        }
        uint32_t const slot = static_cast<uint32_t>(owner.current_frame);
        // THE MARKS ARE POSITIONAL: an out-of-order index would mislabel every later interval, so it
        // is refused by name instead of being accepted silently (the check gpu_mark used to make by
        // reading this class's count field - the field read leaves the engine with the check).
        if (owner.gpu_timing_marks[slot] != mark_index) {
            return rhi::error::invalid_argument;
        }
        // WHICH PIPELINE STAGE the timestamp resolves at is the backend's measurement policy, not a
        // caller decision: the frame's first mark anchors at the top of the pipe (it measures from
        // "nothing yet recorded"), every pass boundary resolves at the bottom (it measures everything
        // submitted so far) - the one pattern this renderer's marks ever expressed, now owned here.
        VkPipelineStageFlagBits const stage = mark_index == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        owner.mark_gpu_timing(this->native(), slot, stage, stage_name);
        return rhi::error::ok;
    }

    vk_command_buffer core::make_command_buffer() const {
        // EACH BUFFER OWNS ITS OWN POOL (handles/handles.cppm): the wrapper creates the pool, allocates
        // the buffer from it and destroys the pool on release - there is no shared pool member left for
        // a caller to keep alive, and no pool shared between recording threads.
        return ::deren::vulkan::make_command_buffer(this->logical_device, this->graphics_queue_family_index, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    }

    vk_command_buffer core::make_secondary_command_buffer() const {
        return ::deren::vulkan::make_command_buffer(this->logical_device, this->graphics_queue_family_index, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    }

    vk_image_view core::make_image_view(VkImage const image, VkFormat const format, VkImageViewType const type) const {
        VkImageViewCreateInfo const view_info = make_image_view_info(image, format, type, VK_IMAGE_ASPECT_COLOR_BIT, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(this->logical_device, &view_info, nullptr, &view);
        return vk_image_view(view, this->logical_device);
    }

    vk_sampler core::make_sampler(VkSamplerAddressMode const address_mode, float const max_lod) const {
        VkSamplerCreateInfo info = make_texture_sampler_info(address_mode, max_lod);
        VkSampler sampler = VK_NULL_HANDLE;
        vkCreateSampler(this->logical_device, &info, nullptr, &sampler);
        return vk_sampler(sampler, this->logical_device);
    }

    std::optional<vk_shader_module> core::make_shader_module(std::span<uint8_t> const shader) const noexcept {
        return ::deren::vulkan::make_shader_module(shader, this->logical_device);
    }

    void core::wait_frame_slot(uint32_t const slot) const {
        // The transitional spelling: the engine's engine-side pacing call. The result it drops is
        // exactly what `wait_frame_slot_result` reports - see the frame walker's wait_and_acquire.
        static_cast<void>(this->wait_frame_slot_result(slot));
    }

    VkResult core::wait_frame_slot_result(uint32_t const slot) const noexcept {
        // Host pacing: wait until this slot's last submission (its timeline value) completed.
        // Value 0 means the slot was never submitted — nothing to wait for, SUCCESS by definition.
        uint64_t const value = this->frame_done_values[slot];
        if (value == 0) {
            return VK_SUCCESS;
        }
        VkSemaphoreWaitInfo wait_info = {};
        wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait_info.semaphoreCount = 1;
        wait_info.pSemaphores = &this->frame_done_semaphores[slot];
        wait_info.pValues = &value;
        auto const result = vkWaitSemaphores(this->logical_device, &wait_info, UINT64_MAX);
        if (result == VK_SUCCESS)
            poll_submissions();
        return result;
    }

    void core::to_next_frame() noexcept {
        current_frame = (current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    VkResult core::submit_frame(VkCommandBuffer const command_buffer, uint32_t const image_index) {
        // Signal this frame slot's TIMELINE to the next value (GPU completion + host pacing,
        // see wait_frame_slot) and the image's binary present-ready semaphore (vkQueuePresentKHR
        // requires a binary wait; per-image so a separate present queue cannot race a re-signal).
        // VUID-VkSubmitInfo-pNext-03240 / -03241: with a VkTimelineSemaphoreSubmitInfo in the pNext
        // chain BOTH counts must equal their semaphore counts - the wait side too, even though the
        // semaphore being waited on is binary and its value is ignored. The count is what validation
        // checks, so a zero waitSemaphoreValueCount next to waitSemaphoreCount = 1 is an error on
        // every frame; only the signal side was handled before.
        uint32_t const slot = static_cast<uint32_t>(this->current_frame);
        // The value this submission asks the slot's timeline to take. It is recorded only once
        // vkQueueSubmit has accepted the submission (below): wait_frame_slot() waits on the RECORDED
        // value, so recording one that no submission will ever signal would block this slot forever.
        uint64_t const signal_value = this->frame_done_values[slot] + 1;

        constexpr VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore signal_semaphores[2] = {this->frame_done_semaphores[slot], this->present_ready_semaphores[image_index]};
        uint64_t signal_values[2] = {signal_value, 0};
        // The wait side's value array, for the count rule above: the element is ignored (the semaphore
        // is binary) but the count has to be there.
        uint64_t const wait_value = 0;
        VkTimelineSemaphoreSubmitInfo timeline_info = {};
        timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline_info.waitSemaphoreValueCount = 1;
        timeline_info.pWaitSemaphoreValues = &wait_value;
        timeline_info.signalSemaphoreValueCount = 2;
        timeline_info.pSignalSemaphoreValues = signal_values;

        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.pNext = &timeline_info;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &this->image_available_semaphores[slot];
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;
        submit_info.signalSemaphoreCount = 2;
        submit_info.pSignalSemaphores = signal_semaphores;
        auto submission = std::make_unique<pending_submission>();
        submission->owner = this;
        submission->timeline = frame_done_semaphores[slot];
        submission->value = signal_value;
        submission->groups = commands_view.group_refs;
        submission->secondaries = commands_view.secondary_refs;
        std::lock_guard lock(submissions_mutex);
        for (auto const& secondary : submission->secondaries)
            if (!secondary->executable || (!secondary->simultaneous_use && secondary->pending.load(std::memory_order_acquire)))
                return VK_NOT_READY;
        pending_submissions.reserve(pending_submissions.size() + 1);
        VkResult const result = vkQueueSubmit(this->graphics_queue_handle, 1, &submit_info, VK_NULL_HANDLE);
        if (result == VK_SUCCESS) {
            submission->submitted = true;
            for (auto const& secondary : submission->secondaries) {
                secondary->pending.fetch_add(1, std::memory_order_release);
                if (secondary->one_time_submit)
                    secondary->executable = false;
            }
            pending_submissions.push_back(std::move(submission));
            this->frame_done_values[slot] = signal_value;
            // THE FRAME IS HANDED OVER, so there is no frame in flight to record into any more:
            // begin_commands()/frame_image() answer nullptr until the next acquire. This is the other
            // half of acquire_next_image() (core.api_core.cpp), which is what set the flag.
            this->frame_in_flight = false;
        }
        return result;
    }

    deren::promise::rhi::error core::present(uint32_t const image_index) const {
        // Wait this image's binary present-ready semaphore (signaled by submit() above);
        // vkQueuePresentKHR requires binary wait semaphores (VUID-vkQueuePresentKHR-pWaitSemaphores-03267).
        VkPresentInfoKHR present_info = {};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &this->present_ready_semaphores[image_index];
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &this->swap_chain;
        present_info.pImageIndices = &image_index;
        // THE TRANSLATION IS THE BACKEND'S, at the present call site: the engine never sees a VkResult
        // (and the engine referencing a translator would JOIN the boundary worklist - the direction
        // the flip measures against).
        return present_error(vkQueuePresentKHR(this->present_queue_handle, &present_info));
    }

    void core::swapchain_view::release() noexcept {
        // A BORROWED VIEW CARRIES NO REFERENCE (the frame_image_slot rule): the presentation surface
        // is the core's and dies with its teardown, so a release() here is a wrapped-borrow bug and
        // the answer is a ONE-TIME NAMED LOG, not a panic and not a silent no-op.
        if (!this->borrowed_release_logged) {
            this->borrowed_release_logged = true;
            deren::utility::log("rhi: release() on the swapchain view - it is BORROWED from the backend and carries no reference, so nothing was released; the object_manager that wrapped it is the bug");
        }
    }

    rhi::error core::swapchain_view::recreate() {
        // ok = a new generation was built; not_ready = the window reports a zero extent and nothing
        // was built - a DEFERRED recreate is a state, not a failure, and the caller retries when the
        // window is sized. Real build failures never reach here: they are the backend's named
        // startup-style panics, the same rule the constructor's build path follows.
        return this->owner->recreate_swap_chain() ? rhi::error::ok : rhi::error::not_ready;
    }

    rhi::image_extent core::swapchain_view::extent() const noexcept {
        // What presentation SHOWS; the caller's render scale is the caller's own decision and is
        // deliberately not pre-multiplied here (the contract's note).
        return {.width = this->owner->swap_chain_extent.width, .height = this->owner->swap_chain_extent.height};
    }

    rhi::image_format core::swapchain_view::format() const noexcept {
        // The four 8-bit shapes a screenshot can be unpacked from; anything else is `unknown`, and the
        // engine's read stage turns that into the one-time "unsupported swapchain format" it already
        // had (runtime.readback.cppm).
        switch (this->owner->swap_chain_image_format) {
        case VK_FORMAT_B8G8R8A8_SRGB:
            return rhi::image_format::bgra8_srgb;
        case VK_FORMAT_B8G8R8A8_UNORM:
            return rhi::image_format::bgra8_unorm;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return rhi::image_format::rgba8_srgb;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return rhi::image_format::rgba8_unorm;
        case VK_FORMAT_R16G16_SFLOAT:
            return rhi::image_format::r16g16_sfloat;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return rhi::image_format::r16g16b16a16_sfloat;
        case VK_FORMAT_R32G32B32_SFLOAT:
            return rhi::image_format::r32g32b32_sfloat;
        case VK_FORMAT_R16_SFLOAT:
            return rhi::image_format::r16_sfloat;
        default:
            return rhi::image_format::unknown;
        }
    }

    bool core::recreate_swap_chain() {
        // 0. A minimized (or otherwise not-yet-sized) window reports currentExtent (0, 0). Building a
        //    swapchain and the per-image targets from that is invalid - vkCreateSwapchainKHR
        //    (VUID-VkSwapchainCreateInfoKHR-imageExtent-01689) and every vkCreateImage
        //    (VUID-VkImageCreateInfo-extent-00944/-00945) reject a zero extent - and there is nothing
        //    to render into anyway. Keep the current generation untouched and let the caller retry:
        //    the frame loop already skips frames whose swapchain extent is zero
        //    (runtime::pace_and_acquire), and a restore / resize produces a sized window shortly.
        swap_chain_support_details const support = query_swap_chain_support(this->physical_device, this->surface);
        if (support.capabilities.currentExtent.width == 0 || support.capabilities.currentExtent.height == 0) {
            if (!this->zero_extent_recreation_logged) {
                this->zero_extent_recreation_logged = true;
                deren::utility::log("swapchain recreation deferred: the window has no drawable size yet (minimized / live resize)");
            }
            return false; // NOTHING was rebuilt: the caller must not invalidate the generation's state
        }
        this->zero_extent_recreation_logged = false;

        // 1. Wait for the device to be idle
        vkDeviceWaitIdle(logical_device);
        poll_submissions(true);

        // 2. AND EVERY RENDER TARGET BELONGS TO THE ENGINE (③-D/E A1.7), so this function destroys NOTHING
        //    of the chain: the HDR/LDR pair (A1.3), the G-buffer cluster (A1.4), the stochastic-chain trio
        //    and the four bloom levels (A1.5) and the furnace environment cube (A1.6) are the engine's, and
        //    `runtime::create_render_chain_targets()` releases the old generation and creates the new one
        //    right after this rebuild answered `ok` (see on_swapchain_recreated). What remains backend-owned
        //    and is rebuilt below is the swapchain itself and the forward path's depth image.

        // 3. Destroy depth resources
        for (auto const& view : depth_image_views) {
            vkDestroyImageView(logical_device, view, nullptr);
        }
        depth_image_views.clear();

        for (auto const& image : depth_images) {
            vkDestroyImage(logical_device, image, nullptr);
        }
        depth_images.clear();

        for (auto const& memory : depth_image_memories) {
            vkFreeMemory(logical_device, memory, nullptr);
        }
        depth_image_memories.clear();

        // 4. Destroy swapchain image views
        for (auto const& image_view : swap_chain_image_views) {
            vkDestroyImageView(logical_device, image_view, nullptr);
        }
        swap_chain_image_views.clear();

        // 5. Destroy the swapchain itself
        if (swap_chain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(logical_device, swap_chain, nullptr);
            swap_chain = VK_NULL_HANDLE;
        }

        // 6. Recreate all resources THE BACKEND OWNS
        this->init_swap_chain();        // rebuild swapchain
        this->init_image_views();       // rebuild image views
        this->create_depth_resources(); // rebuild depth resources
        // (the render chain's targets are rebuilt by the ENGINE right after this function returns `true`:
        // `on_swapchain_recreated()` calls runtime::create_render_chain_targets(), which releases the old
        // generation first - ③-D/E A1.7)

        // Present-ready semaphores are allocated per image index; destroy and rebuild when the
        // count changes (device is idle here). The per-slot timeline + binary acquire
        // semaphores are independent of the image count and survive untouched.
        for (auto const& semaphore : present_ready_semaphores) {
            vkDestroySemaphore(logical_device, semaphore, nullptr);
        }
        present_ready_semaphores.resize(swap_chain_images.size());
        VkSemaphoreCreateInfo binary_info = make_binary_semaphore_info();
        for (auto& semaphore : present_ready_semaphores) {
            if (vkCreateSemaphore(logical_device, &binary_info, nullptr, &semaphore) != VK_SUCCESS) {
                deren::utility::panic("failed to recreate present-ready semaphore!");
            }
        }
        return true; // a new generation exists: every per-image target and its state must be rebuilt
    }

    vk_image_view core::make_depth_image_view(VkImage const image, VkFormat const format) const {
        VkImageViewCreateInfo const view_info = make_image_view_info(image, format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(this->logical_device, &view_info, nullptr, &view);
        return vk_image_view(view, this->logical_device);
    }

    vk_image_view core::make_depth_array_view(VkImage const image, VkFormat const format) const {
        // every layer in one view: this is what sample2DArrayShadow reads (see make_depth_layer_view
        // for the per-layer views the shadow pass renders into)
        VkImageViewCreateInfo const view_info = make_image_view_info(image, format, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_DEPTH_BIT, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(this->logical_device, &view_info, nullptr, &view);
        return vk_image_view(view, this->logical_device);
    }

    vk_image_view core::make_depth_layer_view(VkImage const image, VkFormat const format, uint32_t const layer) const {
        // one layer, as a plain 2D depth view: a dynamic rendering instance renders into exactly one
        // cascade, and a 2D view keeps that pass identical to the single-shadow-map one
        VkImageViewCreateInfo view_info = make_image_view_info(image, format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, VK_REMAINING_MIP_LEVELS, 1);
        view_info.subresourceRange.baseArrayLayer = layer;
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(this->logical_device, &view_info, nullptr, &view);
        return vk_image_view(view, this->logical_device);
    }

    vk_sampler core::make_shadow_sampler() const {
        // Shadow map sampler: depth-compare + LINEAR filtering gives HARDWARE percentage-closer
        // filtering - one texture() in pbr.frag (sampler2DShadow with a reference depth) returns
        // the lit fraction of the 2x2 texel neighborhood, so the shader no longer hand-loops a
        // 3x3 PCF. compareOp matches pbr.frag's test: lit when the fragment is not deeper than
        // the stored depth (ref <= stored).
        VkSamplerCreateInfo info = make_shadow_sampler_info();
        VkSampler sampler = VK_NULL_HANDLE;
        vkCreateSampler(this->logical_device, &info, nullptr, &sampler);
        return vk_sampler(sampler, this->logical_device);
    }

    void core::create_samplers() {
        // THEY ARE TORN DOWN BY A CLEANUP LAMBDA, not by their members' destructors, and the ordering is the whole
        // reason: cleanup runs LIFO from the destructor BODY, and the device's own cleanup is registered before this
        // one - while a MEMBER's destructor runs after that body, i.e. after vkDestroyDevice. The first version of
        // this move left the samplers to their destructors and validation named exactly seven leaked objects.
        this->register_cleanup([this] {
            this->texture_sampler.release();
            this->gbuffer_sampler.release();
            this->taa_sampler.release();
            this->post_sampler.release();
            this->post_nearest_sampler.release();
            this->shadow_sampler.release();
        });
        // The seven shared samplers, in one place: each is a device-level object a pass DECLARES by hint, so their
        // creation belongs with the device rather than with whichever subsystem happened to need one first (see
        // core.cppm's block for why, and for the one sampler that deliberately stays out).
        this->texture_sampler_info = make_texture_sampler_info(VK_SAMPLER_ADDRESS_MODE_REPEAT, 12.0f);
        this->texture_sampler = this->make_sampler(VK_SAMPLER_ADDRESS_MODE_REPEAT, 12.0f);
        this->post_sampler = this->make_sampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);
        this->shadow_sampler = this->make_shadow_sampler();

        // NEAREST, clamp: the G-buffer's stored surface is read at exact texel centres - an interpolated normal or a
        // filterable material id is a different surface, not a smoother one.
        VkSamplerCreateInfo gbuffer_info = make_texture_sampler_info(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f);
        gbuffer_info.magFilter = VK_FILTER_NEAREST;
        gbuffer_info.minFilter = VK_FILTER_NEAREST;
        VkSampler gbuffer = VK_NULL_HANDLE;
        if (vkCreateSampler(this->logical_device, &gbuffer_info, nullptr, &gbuffer) == VK_SUCCESS) {
            this->gbuffer_sampler = vk_sampler(gbuffer, this->logical_device);
        }

        // ... and the same thing for the composite's GI upsample: it taps the depth and the normal at centres, and an
        // averaged depth invents a surface between two samples, which is exactly what an edge-aware test must not see.
        VkSampler nearest = VK_NULL_HANDLE;
        if (vkCreateSampler(this->logical_device, &gbuffer_info, nullptr, &nearest) == VK_SUCCESS) {
            this->post_nearest_sampler = vk_sampler(nearest, this->logical_device);
        }

        // The resolve upsamples the scene colour but must NOT average neighbouring history texels: linear
        // magnification, nearest minification.
        VkSamplerCreateInfo taa_info = make_texture_sampler_info(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f);
        taa_info.magFilter = VK_FILTER_LINEAR;
        taa_info.minFilter = VK_FILTER_NEAREST;
        VkSampler taa = VK_NULL_HANDLE;
        if (vkCreateSampler(this->logical_device, &taa_info, nullptr, &taa) == VK_SUCCESS) {
            this->taa_sampler = vk_sampler(taa, this->logical_device);
        }

        // THE HEAP'S COPY OF THESE, in the order source/shaders/heap_slots.glsl names them (see core.cppm's
        // shared_sampler_infos): the heap descriptor for a sampler is the create info, and the heap itself is
        // created later in the constructor than this function runs - so the infos are kept here and written onto
        // the sampler grid afterwards. Recomputed rather than stored one by one because two of the six share
        // gbuffer_info (the G-buffer read and the composite's nearest tap are the same sampler twice).
        this->shared_sampler_infos[0] = this->texture_sampler_info;
        this->shared_sampler_infos[1] = make_texture_sampler_info(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);
        this->shared_sampler_infos[2] = gbuffer_info;
        this->shared_sampler_infos[3] = gbuffer_info;
        this->shared_sampler_infos[4] = taa_info;
        this->shared_sampler_infos[5] = make_shadow_sampler_info();
    }
    std::expected<vk_pipeline, std::string_view> core::make_gbuffer_pipeline(
        std::span<uint8_t const> const vertex_shader_code,
        std::span<uint8_t const> const fragment_shader_code,
        VkShaderStageFlagBits const first_stage) const {
        // Five color targets: the three surface targets, the motion vectors, and the scene color the
        // pass ADDS the emissive term into (lighting-independent, and it needs the emissive texture and
        // the UVs the G-buffer does not store - see core::gbuffer_pass_attachment_count). The first
        // four are overwritten, the scene color accumulates, so the blend states differ per attachment.
        std::array<VkFormat, gbuffer_pass_attachment_count> const formats = {
            gbuffer_formats[0],
            gbuffer_formats[1],
            gbuffer_formats[2],
            gbuffer_velocity_format,
            hdr_format,
        };
        std::array<VkPipelineColorBlendAttachmentState, gbuffer_pass_attachment_count> const blends = {
            make_color_blend_attachment_opaque(),
            make_color_blend_attachment_opaque(),
            make_color_blend_attachment_opaque(),
            make_color_blend_attachment_opaque(), // motion vectors are data, not coverage
            make_color_blend_attachment_additive(),
        };
        auto result = deren::vulkan::make_pipeline(
            this->logical_device,
            std::span<VkFormat const>(formats),
            this->depth_attachment_format,
            vertex_shader_code,
            fragment_shader_code,
            VK_SAMPLE_COUNT_1_BIT, // a G-buffer is never multisampled (see gbuffer_formats)
            true,                  // depth test + write: opaque geometry, and the lighting pass needs depth
            0.0f,
            0.0f,
            0.0f,
            std::span<VkPipelineColorBlendAttachmentState const>(blends),
            // ... and the stage that emits the geometry: MESH when the caller passes a mesh entry (then the vertex
            // input state is not derived from it at all - a mesh stage declares no Input variables).
            first_stage);
        if (result) {
            // same fullscreen viewport/scissor default as the forward pipelines (the frame path
            // re-syncs it on every swapchain recreation). The RENDER extent, because this pipeline draws
            // into a render target from the scene chain - see `update_pass_geometry`, which resyncs exactly
            // the pipelines that carry these stored values.
            result->viewport = {
                0.0f,
                0.0f,
                static_cast<float>(this->render_extent().width),
                static_cast<float>(this->render_extent().height),
                0.0f,
                1.0f,
            };
            result->scissor = {{0, 0}, this->render_extent_2d()};
        }
        return result;
    }

    std::expected<vk_pipeline, std::string_view> core::make_character_forward_pipeline(
        std::span<uint8_t const> const vertex_shader_code,
        std::span<uint8_t const> const fragment_shader_code,
        VkShaderStageFlagBits const first_stage) const {
        // ONE colour target, and it is the HDR one: this pass runs inside the HDR chain (after the lighting
        // stage, before the resolve), so the tonemap stays the post chain's - see the declaration's note.
        std::array<VkFormat, 1> const formats = {hdr_format};
        // STANDARD ALPHA BLENDING, AND FOR AN OPAQUE MATERIAL IT IS BIT-IDENTICAL TO THE OVERWRITE THIS
        // REPLACES: `src * srcAlpha + dst * (1 - srcAlpha)` at `srcAlpha == 1` is `src`, and the character stage
        // writes an alpha of exactly 1 for every material that is not the article's transparent variant (see
        // `toon_inputs::alpha_blend`). That is why one pipeline can serve both: the blend state is per-PASS in
        // this renderer, and the PER-MATERIAL half of the author's `Blend [_SrcBlend] [_DstBlend]` - chen's
        // `cloth_02` is `SrcAlpha` / `OneMinusSrcAlpha` - travels as the fragment stage's coverage instead.
        //
        // THE COST OF SHARING THE PIPELINE, stated rather than hidden: the HDR target's ALPHA channel is now
        // read-modify-written by this pass instead of overwritten. Nothing consumes it - `post.slang` returns
        // `float4(color, 1.0)` and samples `.rgb` - and the A/B that proves the rest of the character is
        // unaffected (0 px) is in `remaining_port_spec.md`'s "其余部位按参考对齐（续）" item 10.
        std::array<VkPipelineColorBlendAttachmentState, 1> const blends = {make_color_blend_attachment()};
        auto result = deren::vulkan::make_pipeline(
            this->logical_device,
            std::span<VkFormat const>(formats),
            this->depth_attachment_format,
            vertex_shader_code,
            fragment_shader_code,
            VK_SAMPLE_COUNT_1_BIT, // the HDR chain is single-sampled, like the G-buffer it re-shades
            true,                  // the depth TEST is on; the WRITE is turned off per draw (dynamic state)
            0.0f,
            0.0f,
            0.0f,
            std::span<VkPipelineColorBlendAttachmentState const>(blends),
            first_stage,
            // THE ONE OPERATOR IN THE RENDERER THAT IS NOT LESS_OR_EQUAL: it confines the overwrite to the
            // surface the G-buffer pass recorded, which is what makes this pass a replacement rather than a
            // second layer over geometry that is merely in front.
            VK_COMPARE_OP_EQUAL);
        if (result) {
            result->viewport = {
                0.0f,
                0.0f,
                static_cast<float>(this->render_extent().width),
                static_cast<float>(this->render_extent().height),
                0.0f,
                1.0f,
            };
            result->scissor = {{0, 0}, this->render_extent_2d()};
        }
        return result;
    }

    std::expected<vk_pipeline, std::string_view> core::make_overlay_pipeline(
        std::span<uint8_t const> const vertex_shader_code,
        std::span<uint8_t const> const fragment_shader_code,
        VkShaderStageFlagBits const first_stage) const {
        // ONE colour target, and it is the HDR one - the same target the character-forward stage just wrote, so
        // that the multiply lands on the TOON result rather than on a resolved copy of it. The grade and the
        // tonemap stay the post chain's, exactly as they are for the toon stage itself.
        std::array<VkFormat, 1> const formats = {hdr_format};
        // THE MULTIPLY (see make_color_blend_attachment_multiply): this is the state that makes the article's
        // two `Trick` shaders overlays rather than surfaces, and the reason the port needs no blend extension.
        std::array<VkPipelineColorBlendAttachmentState, 1> const blends = {make_color_blend_attachment_multiply()};
        auto result = deren::vulkan::make_pipeline(
            this->logical_device,
            std::span<VkFormat const>(formats),
            this->depth_attachment_format,
            vertex_shader_code,
            fragment_shader_code,
            VK_SAMPLE_COUNT_1_BIT, // single-sampled, like the toon stage and the G-buffer it re-shades
            true,                  // the depth TEST is on; the WRITE is turned off per draw (dynamic state)
            0.0f,
            0.0f,
            0.0f,
            std::span<VkPipelineColorBlendAttachmentState const>(blends),
            first_stage,
            // LESS_OR_EQUAL, AND NOT THE TOON STAGE'S EQUAL - the one place the two pipelines differ beyond the
            // blend, and the difference is a property of the geometry rather than a preference. The toon stage
            // draws the SAME triangles the G-buffer recorded, so `EQUAL` is what confines its overwrite to that
            // surface. An overlay mask is a DIFFERENT mesh (36 and 246 vertices on `chars\chen_full2.glb`) whose
            // quads sit a little IN FRONT of the surface they darken - an `EQUAL` test would reject almost every
            // fragment of it and the masks would draw nothing at all. `LESS_OR_EQUAL` is also the article's own
            // state (`ZTest` default, `ZWrite Off`), so the port is reproducing it rather than working around it.
            VK_COMPARE_OP_LESS_OR_EQUAL);
        if (result) {
            result->viewport = {
                0.0f,
                0.0f,
                static_cast<float>(this->render_extent().width),
                static_cast<float>(this->render_extent().height),
                0.0f,
                1.0f,
            };
            result->scissor = {{0, 0}, this->render_extent_2d()};
        }
        return result;
    }

    std::expected<vk_pipeline, std::string_view> core::make_outline_pipeline(
        std::span<uint8_t const> const vertex_shader_code,
        std::span<uint8_t const> const fragment_shader_code,
        VkShaderStageFlagBits const first_stage) const {
        // ONE colour target, and it is the HDR one - the same target the character-forward stage has just
        // written, so the hull lands in the TOON result rather than on a resolved copy of it. The grade and the
        // tonemap stay the post chain's, exactly as they are for the toon stage and for the overlay group.
        std::array<VkFormat, 1> const formats = {hdr_format};
        // OVERWRITE, NOT MULTIPLY: the article's outline is an OPAQUE surface - `MyZmdOutlineShader`'s SubShader
        // states no `Blend` at all and its fragment returns alpha 1 - so this is the state the TOON stage uses
        // (`make_color_blend_attachment_opaque`), not the overlay group's multiply.
        std::array<VkPipelineColorBlendAttachmentState, 1> const blends = {make_color_blend_attachment_opaque()};
        auto result = deren::vulkan::make_pipeline(
            this->logical_device,
            std::span<VkFormat const>(formats),
            this->depth_attachment_format,
            vertex_shader_code,
            fragment_shader_code,
            VK_SAMPLE_COUNT_1_BIT, // the HDR chain is single-sampled, like the G-buffer it re-shades
            true,                  // the depth TEST is on; the WRITE is turned off per draw (dynamic state)
            0.0f,
            0.0f,
            0.0f,
            std::span<VkPipelineColorBlendAttachmentState const>(blends),
            first_stage,
            // LESS_OR_EQUAL, AND THE ARTICLE'S OWN `ZTest` DEFAULT rather than the toon stage's `EQUAL`. The
            // difference is the geometry: a hull is the same mesh pushed OUTWARD, so the fragments that survive
            // front-face culling are the ring just OUTSIDE the silhouette - where the depth buffer holds whatever
            // is behind the character (nothing, or a farther surface). `EQUAL` would reject exactly those
            // fragments (they are not the surface the G-buffer recorded) and the outline would draw nothing at
            // all; `LESS_OR_EQUAL` keeps the ring and is what confines the hull's interior to the surface that
            // already covers it. See `character_forward.cpp`, which states the depth-write half of this.
            VK_COMPARE_OP_LESS_OR_EQUAL);
        if (result) {
            result->viewport = {
                0.0f,
                0.0f,
                static_cast<float>(this->render_extent().width),
                static_cast<float>(this->render_extent().height),
                0.0f,
                1.0f,
            };
            result->scissor = {{0, 0}, this->render_extent_2d()};
        }
        // CULL FRONT IS NOT HERE, AND THAT IS THE POINT RATHER THAN AN OMISSION: the rasterization state is
        // DYNAMIC in this renderer (every leaf's draw() calls `set_cull_mode` with its own `doubleSided` flag), so
        // a Cull Front stated in the pipeline would be overwritten by the first hull drawn with it. The front-face
        // culling that makes an inverted hull an outline is `render_environment::forced_cull_front`, which the
        // character-forward pass sets around this group alone (see character_forward.cpp).
        return result;
    }

    std::expected<vk_pipeline, std::string_view> core::make_depth_pipeline(
        std::span<uint8_t const> vertex_shader_code,
        std::span<uint8_t const> const fragment_shader_code,
        VkFormat const depth_format,
        float const depth_bias_constant_factor,
        float const depth_bias_slope_factor,
        float const depth_bias_clamp) const {
        auto result = deren::vulkan::make_pipeline(
            this->logical_device,
            VK_FORMAT_UNDEFINED, // no color attachment
            depth_format,
            vertex_shader_code,
            fragment_shader_code,
            VK_SAMPLE_COUNT_1_BIT, // the shadow map is single-sampled
            true,                  // depth test + write
            false,                 // no color attachment
            depth_bias_constant_factor,
            depth_bias_slope_factor,
            depth_bias_clamp);
        // viewport/scissor are dynamic states set by the caller before drawing (the shadow map
        // is a fixed-size target, so core::make_pipeline's swapchain-size defaults do not apply)
        return result;
    }

    void core::wait_idle() const noexcept {
        vkDeviceWaitIdle(this->logical_device);
        poll_submissions(true);
    }

} // namespace deren::vulkan
