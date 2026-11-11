// -*- C++ -*-
// ============================================================================
// file: source/backends/vulkan/core/facet/core.frame_facet.cpp
//
// The frame face stores the swapchain, the frame ring and the frame's non-counted
// views. Its virtual entry points implement the frame lifecycle here; the root
// keeps thin forwarders for its callers.
// ============================================================================
module;

#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.vstd;
import deren.utility.shared;
import deren.promise.rhi;
import :declarations;

namespace deren::vulkan {

    namespace rhi = deren::promise::rhi;

    rhi::command_buffer* core::frame_facet::begin_commands() {
        // THE RECORDING VIEW OF THE FRAME IN FLIGHT, OR nullptr WHEN THERE IS NONE. The verb is not
        // literal yet: the frame's vkBeginCommandBuffer/vkEndCommandBuffer still belong to the
        // engine, which also decides the present recipe - this call starts nothing, it hands out the
        // list the frame's recording is already going into. The `nullptr` when no frame is in flight
        // is the contract's way of saying "there is nothing to record into".
        if (!this->frame_in_flight) {
            return nullptr;
        }
        return &this->commands_view;
    }

    rhi::image* core::frame_facet::frame_image() noexcept {
        // THE IMAGE THE LAST ACQUIRE RETURNED, and it stays answerable AFTER the frame is submitted -
        // which is one step longer than the contract's minimum ("valid until that frame is submitted")
        // and is exactly what the read-back needs: its COPY is recorded inside the frame, but its READ
        // runs after the frame has landed and then asks this image for the extent and the format of the
        // frame it captured (runtime.readback.cppm). MEASURED: with the frame-scoped answer the read
        // stage got nullptr and every scenario reported "no screenshot produced". What the pointer may
        // not outlive is the CONTEXT; the frame-scoped window remains the caller-side rule for the
        // RECORDING verbs, and those still refuse when no frame is in flight.
        //
        // BEFORE THE FIRST ACQUIRE THERE IS NO "LAST" IMAGE, and that is a separate state from the index:
        // `acquired_image_index` starts at 0, which names a real swapchain image, so the index alone would
        // answer with image 0 before any frame ever began. `frame_acquired` is what tells the two apart.
        if (!this->frame_acquired || static_cast<std::size_t>(this->acquired_image_index) >= this->swap_chain_images.size()) {
            return nullptr;
        }
        return &this->frame_image_view;
    }

    rhi::buffer* core::frame_facet::frame_readback_buffer() noexcept {
        // THE BACKEND'S HOST-VISIBLE READ-BACK SLOT, at least one texel per frame pixel at four bytes
        // each (the formats the contract can unpack are all 8-bit RGBA/BGRA). Grown on demand: this is
        // the slot the read-back copy is recorded into, and its size follows the swapchain extent.
        rhi::image_extent const extent = this->swap_chain_extent;
        VkDeviceSize const needed = static_cast<VkDeviceSize>(extent.width) * static_cast<VkDeviceSize>(extent.height) * 4u;
        if (needed == 0) {
            return nullptr; // no swapchain extent yet: there is nothing a copy could write into
        }
        bool const fits = this->readback_slot_buffer.valid() && this->readback_slot_size >= needed && this->readback_slot_mapped != nullptr;
        if (!fits) {
            // REPLACING AN ALLOCATION THE GPU MAY STILL BE READING FROM IS THE ONE THING THAT NEEDS A
            // WAIT, and it happens when the needed size GROWS - i.e. after a swapchain recreation,
            // whose extent is this slot's whole size. The wait is on the replacement only; the
            // steady-state call is the three comparisons above and adds nothing to the frame.
            if (this->readback_slot_buffer.valid()) {
                this->full_core.wait_idle();
            }
            this->readback_slot_buffer.reset();
            this->readback_slot_handle = VK_NULL_HANDLE;
            this->readback_slot_mapped = nullptr;
            this->readback_slot_size = 0;
            this->readback_slot_buffer = this->full_core.device.vma.create_buffer(nullptr, needed, buffer_type::readback_coherent);
            if (!this->readback_slot_buffer.valid()) {
                deren::utility::log("rhi: read-back slot creation failed ({} bytes)", needed);
                return nullptr;
            }
            auto const* const detail = this->full_core.device.vma.get_buffer_detail(this->readback_slot_buffer.handle());
            if (detail == nullptr) {
                this->readback_slot_buffer.reset();
                return nullptr;
            }
            this->readback_slot_handle = detail->buffer;
            this->readback_slot_mapped = detail->allocation_info.pMappedData;
            this->readback_slot_size = needed;
            deren::utility::log("rhi: read-back slot {}x{} -> {} B (host-visible, coherent, TRANSFER_DST)", extent.width, extent.height, needed);
        }
        return &this->readback_slot_view;
    }

    rhi::submit_info core::frame_facet::frame_begin() {
        std::uint32_t image_index = 0;
        VkResult const acquired = this->full_core.acquire_next_image(image_index);
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            // tier-1's frame_begin() has no error channel (plan §3.3): a zeroed answer is the contract's
            // way of saying "no frame started", and the caller's pacing path has the diagnosis.
            return rhi::submit_info{};
        }
        // The contract's "index in the frame-in-flight ring" IS this core's frame slot.
        return rhi::submit_info{.frame_index = static_cast<std::uint32_t>(this->current_frame), .image_index = image_index};
    }

    rhi::error core::frame_facet::present() {
        if (this->full_core.create_options.native_window == nullptr) {
            return rhi::error::unsupported;
        }
        // The contract's present() has no argument: it presents the image its acquire took, and
        // the PRESENT call site's translation answers (out_of_date => the caller rebuilds; a failed
        // presentation never reports silence - the abi 14 note on the verb).
        //
        // NO ACQUIRED FRAME IS A NAMED REFUSAL, not a queue call: `acquired_image_index` starts at 0,
        // which NAMES a real image, so forwarding it before any acquire would reach
        // vkQueuePresentKHR waiting on a present-ready semaphore nothing has signalled (and present
        // image 0, which this frame never wrote). `frame_acquired` is what tells "the last acquire"
        // from "there has never been one" - the same flag `frame_image()` answers on - so a caller
        // that presents before opening a frame is told `not_ready` instead of being run into the WSI.
        if (!this->frame_acquired) {
            return rhi::error::not_ready;
        }
        return this->full_core.present(this->acquired_image_index);
    }

    rhi::swapchain* core::frame_facet::frame_swapchain() noexcept {
        return &this->presentation_surface;
    }

    rhi::error core::frame_facet::submit() {
        return this->full_core.submit_commands(this->commands_view, nullptr);
    }

    // ---- the frame face (abi 13) ----------------------------------------------------------------
    //
    // THE ACCESSORS hand out this core's two views - the same object every call, with `parent` set by
    // the constructor. The views' methods compose the machinery this class already had: the walker is
    // today's frame prologue fused (wait -> latch timings -> acquire -> report), the profiler reads the
    // timing snapshot the walker's latch produced.

    rhi::frame_walker* core::frame_facet::walk_frames() noexcept {
        return &this->frames_view;
    }

    rhi::gpu_profiler* core::frame_facet::profiler() noexcept {
        return &this->profiler_view;
    }

} // namespace deren::vulkan