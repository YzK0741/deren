module;

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

module deren.vulkan.readback;

import deren.promise.rhi; // the contract this file's helpers name (imports are NOT transitive)
import deren.utility;

namespace deren::vulkan {
    namespace {
        /// The escape, obtained from the contract once and then used through ITS pointer. Every native
        /// handle in this file comes from here (③-D/E step 2): the class drives `rhi::api_core` now, so
        /// the VkDevice, the queue, a VkBuffer and a VkCommandBuffer are all borrowed through the
        /// contract's own raw-handle path rather than read off a backend class member.
        deren::promise::rhi::vulkan_escape* escape_of(deren::promise::rhi::api_core& gpu) {
            return static_cast<deren::promise::rhi::vulkan_escape*>(gpu.query_extension(deren::promise::rhi::extension_kind::vulkan_escape));
        }

        /// The VkDevice the contract's root drives; null when the backend announced no escape (it does,
        /// and the startup gate refuses a backend that does not).
        VkDevice native_device_of(deren::promise::rhi::api_core& gpu) {
            auto* const escape = escape_of(gpu);
            return escape == nullptr ? VK_NULL_HANDLE : static_cast<VkDevice>(escape->native_device());
        }

        /// The queue the backend submits on - what this class's one-shot copies go to.
        VkQueue native_queue_of(deren::promise::rhi::api_core& gpu) {
            auto* const escape = escape_of(gpu);
            return escape == nullptr ? VK_NULL_HANDLE : static_cast<VkQueue>(escape->native_queue());
        }

        /// The VkBuffer a contract buffer carries, through the escape - the contract's own rule for a
        /// native handle, and the reason the allocator's detail map is consulted nowhere in this class.
        /// Null when the backend announced no escape or when the buffer carries no handle at all.
        VkBuffer native_buffer_of(deren::promise::rhi::api_core& gpu, deren::promise::rhi::buffer const& buffer) {
            auto* const escape = escape_of(gpu);
            if (escape == nullptr) {
                return VK_NULL_HANDLE;
            }
            return reinterpret_cast<VkBuffer>(escape->native_buffer(buffer));
        }

        /// The VkCommandBuffer a contract COMMAND LIST names, through the same escape (abi 15). A list
        /// of a buffer the caller created is resolvable outside a frame - that is exactly what the
        /// read-back's one-shot buffer needs, because its read runs after the frame has landed.
        VkCommandBuffer native_command_buffer_of(deren::promise::rhi::api_core& gpu, deren::promise::rhi::command_list& list) {
            auto* const escape = escape_of(gpu);
            if (escape == nullptr) {
                return VK_NULL_HANDLE;
            }
            return static_cast<VkCommandBuffer>(escape->native_command_buffer(list));
        }
    } // namespace

    readback::readback(std::shared_ptr<deren::promise::rhi::api_core> device)
        : gpu(std::move(device)) {
    }

    readback::~readback() {
        // The staging buffer is about to be released, so any copy still referencing it must finish
        // first: wait() is what guarantees the GPU is done before the allocation goes away.
        this->wait();
        if (this->fence != VK_NULL_HANDLE && this->gpu != nullptr) {
            vkDestroyFence(native_device_of(*this->gpu), this->fence, nullptr);
            this->fence = VK_NULL_HANDLE;
        }
        // `staging` is a contract owner now: it drops this class's reference on destruction, and the
        // allocation the copies were writing into dies with the last reference.
    }

    void readback::wait() {
        if (!this->fence_pending || this->fence == VK_NULL_HANDLE || this->gpu == nullptr) {
            return;
        }
        VkDevice const device = native_device_of(*this->gpu);
        vkWaitForFences(device, 1, &this->fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &this->fence);
        this->fence_pending = false;
    }

    std::optional<readback::staged_target> readback::stage_for_copy(VkDeviceSize const size) {
        deren::promise::rhi::api_core& vk = *this->gpu;
        if (size == 0) {
            return std::nullopt;
        }
        if (this->staging && this->staging_size >= size) {
            std::span<std::byte> const current_mapping = this->staging->mapped();
            if (current_mapping.data() != nullptr) {
                return staged_target{.buffer = native_buffer_of(vk, *this->staging), .mapped = current_mapping.data(), .size = static_cast<std::size_t>(size)};
            }
        }
        // Growing replaces the buffer, so the copy that used the old one has to have completed:
        // assigning the new owner drops this class's reference to an allocation the GPU may still be
        // writing into (release is not destruction, but the reference is what keeps it alive).
        this->wait();
        this->staging = deren::promise::rhi::object_manager<deren::promise::rhi::buffer>{
            vk.create_buffer(deren::promise::rhi::buffer_desc{.size = size, .usage = deren::promise::rhi::buffer_usage::readback_coherent})};
        this->staging_size = 0;
        if (!this->staging) {
            deren::utility::log("readback: staging buffer creation failed ({} bytes)", size);
            return std::nullopt;
        }
        std::span<std::byte> const mapped = this->staging->mapped();
        if (mapped.data() == nullptr) {
            // A read-back buffer whose bytes cannot be mapped has nothing to hand the caller: drop the
            // reference rather than keep a staging buffer no read could ever answer from. EMPTY is the
            // contract's spelling of "this buffer is not host-visible" (buffer::mapped()).
            this->staging.reset();
            return std::nullopt;
        }
        this->staging_size = size;
        return staged_target{.buffer = native_buffer_of(vk, *this->staging), .mapped = mapped.data(), .size = static_cast<std::size_t>(size)};
    }

    std::expected<std::vector<uint8_t>, std::string> readback::read(VkBuffer const source, VkDeviceSize const size, VkDeviceSize const offset) {
        deren::promise::rhi::api_core& vk = *this->gpu;
        this->last_read_size = 0;
        if (source == VK_NULL_HANDLE || size == 0) {
            return std::unexpected(std::string("readback: nothing to read (null buffer or zero size)"));
        }
        if (this->fence == VK_NULL_HANDLE) {
            VkFenceCreateInfo const fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
            if (vkCreateFence(native_device_of(vk), &fence_info, nullptr, &this->fence) != VK_SUCCESS) {
                return std::unexpected(std::string("readback: fence creation failed"));
            }
        }
        auto const target = this->stage_for_copy(size);
        if (!target) {
            return std::unexpected(std::string("readback: staging buffer unavailable"));
        }

        // ONE COMMAND BUFFER PER CALL, OUT OF THE CONTRACT (abi 15): `create_command_buffer()` hands out
        // a buffer that OWNS ITS OWN COMMAND POOL (handles/handles.cppm), and `object_manager` releases
        // it when this function returns - release destroys the pool, which frees the buffer with it - so
        // nothing accumulates across calls. (The fence wait below is what makes that destruction legal:
        // a pool must not go away while its buffer is pending.)
        deren::promise::rhi::object_manager<deren::promise::rhi::command_buffer> commands{
            vk.create_command_buffer({.kind = deren::promise::rhi::command_buffer_kind::primary})};
        if (!commands) {
            return std::unexpected(std::string("readback: create_command_buffer refused"));
        }
        // THE RECORDING LIFECYCLE IS THE CONTRACT'S, because this buffer is the CALLER's own (the
        // frame's primaries belong to the backend, so this is the one engine-side session that goes
        // through `begin_recording`/`end_recording`); the raw handle the copy commands are recorded on
        // comes from the contract's native-handle path - the same escape this file already uses for
        // buffers (see native_buffer_of above).
        if (commands->begin_recording({.usage = deren::promise::rhi::to_bits(deren::promise::rhi::command_buffer_usage::one_time_submit)}) != deren::promise::rhi::error::ok) {
            return std::unexpected(std::string("readback: begin_recording failed"));
        }
        VkCommandBuffer const command_buffer = native_command_buffer_of(vk, *commands->recording());
        if (command_buffer == VK_NULL_HANDLE) {
            return std::unexpected(std::string("readback: the escape answered no command buffer"));
        }

        // The transfer needs the source's writes visible and finished, and it must not start while an
        // earlier transfer/stage is still writing. A buffer barrier (not an image one) is what carries
        // srcAccess/dstAccess for buffers; ALL_COMMANDS as the source is deliberate: the caller may
        // have written the buffer from any stage, and this is a one-shot path where a conservative
        // source costs nothing measurable.
        std::array<VkBufferMemoryBarrier2, 1> barriers = {};
        barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barriers[0].pNext = nullptr;
        barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].buffer = source;
        barriers[0].offset = offset;
        barriers[0].size = size;
        VkDependencyInfo const dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                             .pNext = nullptr,
                                             .dependencyFlags = 0,
                                             .memoryBarrierCount = 0,
                                             .pMemoryBarriers = nullptr,
                                             .bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
                                             .pBufferMemoryBarriers = barriers.data(),
                                             .imageMemoryBarrierCount = 0,
                                             .pImageMemoryBarriers = nullptr};
        vkCmdPipelineBarrier2(command_buffer, &dependency);

        VkBufferCopy const region = {.srcOffset = offset, .dstOffset = 0, .size = size};
        vkCmdCopyBuffer(command_buffer, source, target->buffer, 1, &region);
        if (commands->end_recording() != deren::promise::rhi::error::ok) {
            return std::unexpected(std::string("readback: end_recording failed"));
        }

        // the fence has just been reset by wait()/creation, so it is unsignaled as submit requires
        VkSubmitInfo const submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                          .pNext = nullptr,
                                          .waitSemaphoreCount = 0,
                                          .pWaitSemaphores = nullptr,
                                          .pWaitDstStageMask = nullptr,
                                          .commandBufferCount = 1,
                                          .pCommandBuffers = &command_buffer,
                                          .signalSemaphoreCount = 0,
                                          .pSignalSemaphores = nullptr};
        if (vkQueueSubmit(native_queue_of(vk), 1, &submit_info, this->fence) != VK_SUCCESS) {
            return std::unexpected(std::string("readback: vkQueueSubmit failed"));
        }
        this->fence_pending = true;

        VkDevice const device = native_device_of(vk);
        vkWaitForFences(device, 1, &this->fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &this->fence);
        this->fence_pending = false;

        // NO INVALIDATE HERE, and it is a deliberate removal rather than an omission. The dropped call
        // asked the allocation's MEMORY TYPE and answered a no-op for this buffer: `readback_coherent`
        // is documented host-visible and HOST_COHERENT (vulkan/core/vma/vma.cppm's buffer_type table) and
        // is allocated through VMA_MEMORY_USAGE_CPU_ONLY, whose memory properties are that same pair
        // (vma.cppm's allocation table), so `invalidate_if_not_coherent` never invalidated anything for
        // it. The reliance is not new: the backend's own frame read-back slot hands out its mapping with
        // no invalidate at all (core.api_core.cpp's frame_readback_slot::mapped), and the contract has
        // no invalidate to carry. A device that served a non-coherent type here would be a violation of
        // the allocator's type table - the table is what the host read below now rests on.
        std::vector<uint8_t> out(static_cast<std::size_t>(size));
        std::memcpy(out.data(), target->mapped, static_cast<std::size_t>(size));
        this->last_read_size = out.size();
        return out;
    }
} // namespace deren::vulkan
