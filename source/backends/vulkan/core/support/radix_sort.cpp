module;
#include <cstdint>
#include <vulkan/vulkan.h>
#define VRDX_IMPLEMENTATION
#include <vrdx/include/vk_radix_sort.h>
module deren.vulkan.core;
import deren.vstd;
import deren.promise.rhi;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;
    namespace {
        class radix_sorter final : public rhi::radix_sorter {
        public:
            static constexpr std::string_view tag = concrete::radix_sorter;
            [[nodiscard]] std::string_view get_tag() const noexcept override {
                return tag;
            }
            radix_sorter(core& full_core, VrdxSorter native) noexcept
                : core_(&full_core)
                , native_(native) {
            }
            ~radix_sorter() noexcept override {
                vrdxDestroySorter(native_);
            }
            void release() noexcept override {
                delete this;
            }
            rhi::radix_sort_requirements requirements(std::uint32_t count) const noexcept override {
                // Upstream RoundUp adds PARTITION_SIZE - 1 in uint32. Validate before entering it.
                std::uint64_t const partitions = (static_cast<std::uint64_t>(count) + 4095u) / 4096u;
                if (count > UINT32_MAX - 4095u || partitions > core_->device.device_properties.limits.maxComputeWorkGroupCount[0]) {
                    return {};
                }
                VrdxSorterStorageRequirements req{};
                vrdxGetSorterStorageRequirements(native_, count, VRDX_SORT_MODE_KEY_VALUE, &req);
                if (req.size > core_->device.device_properties.limits.maxStorageBufferRange) {
                    return {};
                }
                return {.scratch_bytes = req.size, .buffer_alignment = std::max<std::uint64_t>(4u, core_->device.device_properties.limits.minStorageBufferOffsetAlignment), .scratch_flags = rhi::to_bits(rhi::buffer_flag::storage) | rhi::to_bits(rhi::buffer_flag::transfer_destination)};
            }
            rhi::error record(rhi::command_recorder& commands, rhi::radix_sort_info const& info) override {
                rhi::error resolved{};
                auto* const list = core_->resolve_recorder(&commands, resolved);
                if (resolved != rhi::error::ok) {
                    return resolved;
                }
                if (list->rendering_active) {
                    return rhi::error::not_ready;
                }
                if (info.element_count == 0u) {
                    return rhi::error::ok;
                }
                auto req = requirements(info.element_count);
                if (!req.scratch_bytes) {
                    return rhi::error::unsupported;
                }
                auto operand = [&](rhi::buffer* buffer, std::uint64_t offset, std::uint64_t bytes, rhi::buffer_flags flags) -> core::buffer* {
                    if (buffer == nullptr || !rhi::is_concrete(*buffer, concrete::buffer)) {
                        return nullptr;
                    }
                    auto* concrete_buffer = static_cast<core::buffer*>(buffer);
                    if (concrete_buffer->parent != core_ || offset % req.buffer_alignment != 0u || offset > concrete_buffer->size_bytes || bytes > concrete_buffer->size_bytes - offset ||
                        (concrete_buffer->declared_flags & flags) != flags || bytes > core_->device.device_properties.limits.maxStorageBufferRange) {
                        return nullptr;
                    }
                    return concrete_buffer;
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
                if (auto applied = list->prepare_native(); applied != rhi::error::ok) {
                    return applied;
                }
                // Acquire recording leases before reading native handles. Lazy operands may
                // materialize here, and cannot be unloaded while this recording can execute.
                for (auto* resource : {keys, values, scratch}) {
                    if (auto ready = list->demand(*resource); ready != rhi::error::ok) {
                        return ready;
                    }
                }
                vkCmdPipelineBarrier2(list->native(), &dependency);
                VrdxSortInfo native{};
                native.elementCount = info.element_count;
                native.keysBuffer = keys->native;
                native.keysOffset = info.keys_offset;
                native.valuesBuffer = values->native;
                native.valuesOffset = info.values_offset;
                native.storageBuffer = scratch->native;
                native.storageOffset = info.scratch_offset;
                vrdxCmdSort(list->native(), native_, &native);
                memory.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
                memory.dstAccessMask |= VK_ACCESS_2_HOST_READ_BIT;
                vkCmdPipelineBarrier2(list->native(), &dependency);
                // VRDX uses traditional push descriptors, which invalidate descriptor-heap bindings.
                // Restore this device's heaps for subsequent native heap pipelines (including post-processing).
                if (core_->resource.descriptor_heaps.ready()) {
                    core_->resource.descriptor_heaps.record_bind(list->native());
                }
                list->finish_native();
                return rhi::error::ok;
            }

        private:
            core* core_;
            VrdxSorter native_;
        };
    } // namespace
    rhi::radix_sorter* core::frame_gpu_sorting::create_radix_sorter() {
        if (!parent || !parent->resource.gpu_sorting_available) {
            return nullptr;
        }
        VrdxSorter native = VK_NULL_HANDLE;
        VrdxSorterCreateInfo info{.physicalDevice = parent->device.physical_device, .device = parent->device.logical_device, .pipelineCache = VK_NULL_HANDLE};
        if (vrdxCreateSorter(&info, &native) != VK_SUCCESS || native == VK_NULL_HANDLE) {
            return nullptr;
        }
        return new radix_sorter(*parent, native);
    }
} // namespace deren::vulkan
