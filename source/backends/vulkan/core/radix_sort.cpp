module;
#include <algorithm>
#include <cstdint>
#include <vulkan/vulkan.h>
#define VRDX_IMPLEMENTATION
#include <vrdx/include/vk_radix_sort.h>
module deren.vulkan.core;
import deren.promise.rhi;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;
    namespace {
        class owned_radix_sorter final : public rhi::radix_sorter {
        public:
            owned_radix_sorter(core& owner, VrdxSorter native) noexcept
                : rhi::radix_sorter("deren_radix_sorter_vulkan")
                , owner_(&owner)
                , native_(native) {
            }
            ~owned_radix_sorter() noexcept override {
                vrdxDestroySorter(native_);
            }
            void release() noexcept override {
                delete this;
            }
            rhi::radix_sort_requirements requirements(std::uint32_t count) const noexcept override {
                // Upstream RoundUp adds PARTITION_SIZE - 1 in uint32. Validate before entering it.
                std::uint64_t const partitions = (static_cast<std::uint64_t>(count) + 4095u) / 4096u;
                if (count > UINT32_MAX - 4095u || partitions > owner_->device_properties.limits.maxComputeWorkGroupCount[0])
                    return {};
                VrdxSorterStorageRequirements req{};
                vrdxGetSorterStorageRequirements(native_, count, VRDX_SORT_MODE_KEY_VALUE, &req);
                if (req.size > owner_->device_properties.limits.maxStorageBufferRange)
                    return {};
                return {.scratch_bytes = req.size, .buffer_alignment = std::max<std::uint64_t>(4u, owner_->device_properties.limits.minStorageBufferOffsetAlignment), .scratch_flags = rhi::to_bits(rhi::buffer_flag::storage) | rhi::to_bits(rhi::buffer_flag::transfer_destination)};
            }
            rhi::error record(rhi::command_buffer& commands, rhi::radix_sort_info const& info) override {
                if (commands.type != "deren_command_buffer_vulkan" && commands.type != "deren_frame_commands_vulkan") {
                    return rhi::error::invalid_argument;
                }
                auto& list = static_cast<core::frame_commands&>(commands);
                if (list.owner != owner_) {
                    return rhi::error::invalid_argument;
                }
                if (!list.recording || list.native() == VK_NULL_HANDLE) {
                    return rhi::error::not_ready;
                }
                if (info.element_count == 0u) {
                    return rhi::error::ok;
                }
                auto req = requirements(info.element_count);
                if (!req.scratch_bytes)
                    return rhi::error::unsupported;
                auto operand = [&](rhi::buffer* buffer, std::uint64_t offset, std::uint64_t bytes, rhi::buffer_flags flags) -> core::owned_buffer* {
                    if (!buffer || buffer->type != "deren_buffer_vulkan") {
                        return nullptr;
                    }
                    auto* owned = static_cast<core::owned_buffer*>(buffer);
                    if (owned->owner != owner_ || offset % req.buffer_alignment != 0u || offset > owned->size_bytes || bytes > owned->size_bytes - offset ||
                        (owned->declared_flags & flags) != flags || bytes > owner_->device_properties.limits.maxStorageBufferRange) {
                        return nullptr;
                    }
                    return owned;
                };
                std::uint64_t const bytes = static_cast<std::uint64_t>(info.element_count) * 4u;
                auto* keys = operand(info.keys, info.keys_offset, bytes, rhi::to_bits(rhi::buffer_flag::storage));
                auto* values = operand(info.values, info.values_offset, bytes, rhi::to_bits(rhi::buffer_flag::storage));
                auto* scratch = operand(info.scratch, info.scratch_offset, req.scratch_bytes, req.scratch_flags);
                if (!keys || !values || !scratch) {
                    return rhi::error::invalid_argument;
                }
                auto overlap = [](rhi::buffer* a, std::uint64_t ao, std::uint64_t as, rhi::buffer* b, std::uint64_t bo, std::uint64_t bs) {
                    return a == b && ao < bo + bs && bo < ao + as;
                };
                if (overlap(keys, info.keys_offset, bytes, values, info.values_offset, bytes) ||
                    overlap(keys, info.keys_offset, bytes, scratch, info.scratch_offset, req.scratch_bytes) ||
                    overlap(values, info.values_offset, bytes, scratch, info.scratch_offset, req.scratch_bytes)) {
                    return rhi::error::invalid_argument;
                }
                VkMemoryBarrier2 memory{};
                memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                memory.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                memory.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                memory.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                VkDependencyInfo dependency{};
                dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dependency.memoryBarrierCount = 1u;
                dependency.pMemoryBarriers = &memory;
                vkCmdPipelineBarrier2(list.native(), &dependency);
                VrdxSortInfo native{};
                native.elementCount = info.element_count;
                native.keysBuffer = keys->native;
                native.keysOffset = info.keys_offset;
                native.valuesBuffer = values->native;
                native.valuesOffset = info.values_offset;
                native.storageBuffer = scratch->native;
                native.storageOffset = info.scratch_offset;
                vrdxCmdSort(list.native(), native_, &native);
                memory.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
                memory.dstAccessMask |= VK_ACCESS_2_HOST_READ_BIT;
                vkCmdPipelineBarrier2(list.native(), &dependency);
                // VRDX uses traditional push descriptors, which invalidate descriptor-heap bindings.
                // Restore this device's heaps for subsequent native heap pipelines (including post-processing).
                if (owner_->descriptor_heaps.ready())
                    owner_->descriptor_heaps.record_bind(list.native());
                list.bound_pipeline = nullptr;
                return rhi::error::ok;
            }

        private:
            core* owner_;
            VrdxSorter native_;
        };
    } // namespace
    rhi::radix_sorter* core::frame_gpu_sorting::create_radix_sorter() {
        if (!owner || !owner->gpu_sorting_available) {
            return nullptr;
        }
        VrdxSorter native = VK_NULL_HANDLE;
        VrdxSorterCreateInfo info{.physicalDevice = owner->physical_device, .device = owner->logical_device, .pipelineCache = VK_NULL_HANDLE};
        if (vrdxCreateSorter(&info, &native) != VK_SUCCESS || native == VK_NULL_HANDLE) {
            return nullptr;
        }
        return new owned_radix_sorter(*owner, native);
    }
} // namespace deren::vulkan
