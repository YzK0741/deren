module;
#include <cstdint>
#include <limits>
#include <mutex>
#include <span>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.promise.rhi;
import deren.vulkan.synchronization;
import deren.utility.shared;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;
    namespace scopes = rhi::synchronization;
    namespace native_sync = deren::vulkan::synchronization;
    namespace {
        constexpr uint32_t bit(scopes::usage value) noexcept {
            return static_cast<uint32_t>(value);
        }
        constexpr scopes::aspect format_aspects(VkFormat format) noexcept {
            switch (format) {
            case VK_FORMAT_D16_UNORM:
            case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_D32_SFLOAT:
                return scopes::aspect::depth;
            case VK_FORMAT_S8_UINT:
                return scopes::aspect::stencil;
            case VK_FORMAT_D16_UNORM_S8_UINT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return scopes::aspect::depth | scopes::aspect::stencil;
            default:
                return scopes::aspect::color;
            }
        }
        constexpr bool state_usage(scopes::image_state state, uint32_t usage) noexcept {
            switch (state.layout) {
            case scopes::image_layout::undefined:
                return true;
            case scopes::image_layout::general:
                return true;
            case scopes::image_layout::shader_read_only:
                return (usage & bit(scopes::usage::sampled)) != 0;
            case scopes::image_layout::color_attachment:
                return (usage & bit(scopes::usage::color_attachment)) != 0;
            case scopes::image_layout::depth_stencil_attachment:
                return (usage & bit(scopes::usage::depth_stencil_attachment)) != 0;
            case scopes::image_layout::depth_stencil_read_only:
                return (usage & bit(scopes::usage::depth_stencil_read)) != 0;
            case scopes::image_layout::transfer_source:
                return (usage & bit(scopes::usage::transfer_source)) != 0;
            case scopes::image_layout::transfer_destination:
                return (usage & bit(scopes::usage::transfer_destination)) != 0;
            case scopes::image_layout::present:
                return (usage & bit(scopes::usage::present)) != 0;
            default:
                return false;
            }
        }
        rhi::error mapped_error(scopes::error error) noexcept {
            return error == scopes::error::invalid_argument ? rhi::error::invalid_argument : rhi::error::unsupported;
        }
    } // namespace

    scopes::stage core::scoped_sync_view::supported_stages() const noexcept {
        if (!owner)
            return scopes::stage::none;
        auto flags = owner->synchronization_queue_flags;
        uint32_t result = 0;
        if (flags & VK_QUEUE_GRAPHICS_BIT)
            result |= 1U | 2U | 8U | 16U | 64U;
        if ((flags & VK_QUEUE_GRAPHICS_BIT) && owner->mesh_shader_available)
            result |= 512U;
        if ((flags & VK_QUEUE_GRAPHICS_BIT) && owner->task_shader_available)
            result |= 256U;
        if (flags & VK_QUEUE_COMPUTE_BIT)
            result |= 4U;
        if (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            result |= 128U;
        if (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))
            result |= 32U;
        // RT stage 1: the ray-tracing PIPELINE stages, reported only when the device can run them. A ray query inside
        // a compute shader needs none of this bit (it is a COMPUTE_SHADER site, and 4U already says so); this is the
        // traceRays launch's stage, which the RT shadow pass's visibility write names.
        if ((owner->ray_query_available || owner->ray_tracing_pipeline_available) && (flags & VK_QUEUE_GRAPHICS_BIT))
            result |= 1024U;
        return static_cast<scopes::stage>(result);
    }

    rhi::error core::scoped_sync_view::describe(rhi::image const& resource, rhi::scoped_image_info& output) const noexcept {
        if (!owner)
            return rhi::error::not_ready;
        std::lock_guard const lock(owner->contract_images_mutex);
        if (owner->frame_snapshots.contains(&resource)) {
            auto const& image = static_cast<frame_snapshot const&>(resource);
            if (image.owner.get() != owner || !image.current())
                return rhi::error::invalid_argument;
            uint32_t usage = bit(scopes::usage::color_attachment);
            if (owner->swapchain_transfer_src_supported)
                usage |= bit(scopes::usage::transfer_source);
            usage |= bit(owner->create_options.native_window ? scopes::usage::present : scopes::usage::sampled);
            output = {1, 1, scopes::aspect::color, usage, reinterpret_cast<std::uintptr_t>(&image)};
            return rhi::error::ok;
        }
        if (!owner->contract_images.contains(&resource))
            return rhi::error::invalid_argument;
        auto const& image = static_cast<owned_image const&>(resource);
        if (image.owner != owner || image.native_handle == VK_NULL_HANDLE)
            return rhi::error::invalid_argument;
        uint32_t usage = 0;
        auto add = [&](rhi::image_flag flag, scopes::usage target) noexcept { if (rhi::has_flag(image.declared_flags, flag)) usage |= bit(target); };
        add(rhi::image_flag::sampled, scopes::usage::sampled);
        add(rhi::image_flag::storage, scopes::usage::storage);
        add(rhi::image_flag::color_attachment, scopes::usage::color_attachment);
        add(rhi::image_flag::depth_attachment, scopes::usage::depth_stencil_attachment);
        add(rhi::image_flag::depth_attachment, scopes::usage::depth_stencil_read);
        add(rhi::image_flag::transfer_source, scopes::usage::transfer_source);
        add(rhi::image_flag::transfer_destination, scopes::usage::transfer_destination);
        auto const* detail = owner->vma.get_image_detail(image.owned.handle());
        if (!detail)
            return rhi::error::invalid_argument;
        output = {image.mip_levels, image.array_layers, format_aspects(image.resolved_format), usage, reinterpret_cast<std::uintptr_t>(detail)};
        return rhi::error::ok;
    }

    rhi::error core::scoped_sync_view::describe(rhi::buffer const& resource, rhi::scoped_buffer_info& output) const noexcept {
        if (!owner)
            return rhi::error::not_ready;
        std::lock_guard const lock(owner->contract_buffers_mutex);
        if (!owner->contract_buffers.contains(&resource))
            return rhi::error::invalid_argument;
        auto const& buffer = static_cast<owned_buffer const&>(resource);
        if (buffer.owner != owner || buffer.native == VK_NULL_HANDLE)
            return rhi::error::invalid_argument;
        uint32_t usage = 0;
        switch (buffer.declared_usage) {
        case rhi::buffer_usage::vertex:
            usage = bit(scopes::usage::vertex_buffer) | bit(scopes::usage::transfer_destination);
            break;
        case rhi::buffer_usage::index:
            usage = bit(scopes::usage::index_buffer) | bit(scopes::usage::transfer_destination);
            break;
        case rhi::buffer_usage::uniform_gpu_only:
            usage = bit(scopes::usage::uniform_buffer) | bit(scopes::usage::transfer_destination);
            break;
        case rhi::buffer_usage::uniform_coherent:
        case rhi::buffer_usage::uniform_cached:
            usage = bit(scopes::usage::uniform_buffer);
            break;
        case rhi::buffer_usage::storage_coherent:
        case rhi::buffer_usage::storage_gpu_only:
        case rhi::buffer_usage::acceleration_structure_scratch:
            usage = bit(scopes::usage::storage);
            break;
        case rhi::buffer_usage::readback_coherent:
            usage = bit(scopes::usage::transfer_destination);
            break;
        case rhi::buffer_usage::staging_source:
            usage = bit(scopes::usage::transfer_source);
            break;
        case rhi::buffer_usage::acceleration_structure_storage:
            break;
        }
        auto add = [&](rhi::buffer_flag flag, scopes::usage target) noexcept { if (rhi::has_flag(buffer.declared_flags, flag)) usage |= bit(target); };
        add(rhi::buffer_flag::storage, scopes::usage::storage);
        add(rhi::buffer_flag::indirect, scopes::usage::indirect_buffer);
        add(rhi::buffer_flag::transfer_source, scopes::usage::transfer_source);
        add(rhi::buffer_flag::transfer_destination, scopes::usage::transfer_destination);
        auto const* detail = owner->vma.get_buffer_detail(buffer.owned.handle());
        if (!detail)
            return rhi::error::invalid_argument;
        output = {buffer.size_bytes, usage, reinterpret_cast<std::uintptr_t>(detail)};
        return rhi::error::ok;
    }

    rhi::error core::scoped_sync_view::validate(std::span<rhi::scoped_image_barrier const> images, std::span<rhi::scoped_buffer_barrier const> buffers) const noexcept {
        if (!owner)
            return rhi::error::not_ready;
        if (images.size() > UINT32_MAX || buffers.size() > UINT32_MAX)
            return rhi::error::invalid_argument;
        auto supported = static_cast<uint32_t>(supported_stages());
        for (auto const& one : images) {
            if (!one.resource)
                return rhi::error::invalid_argument;
            rhi::scoped_image_info info;
            auto error = describe(*one.resource, info);
            if (error != rhi::error::ok)
                return error;
            auto mapped = native_sync::map_image(one.transition);
            if (!mapped)
                return mapped_error(mapped.error());
            auto const& range = one.transition.range;
            if (range.base_mip >= info.mip_levels || range.mip_count > info.mip_levels - range.base_mip || range.base_layer >= info.array_layers || range.layer_count > info.array_layers - range.base_layer || (static_cast<uint32_t>(range.aspects) & ~static_cast<uint32_t>(info.aspects)))
                return rhi::error::invalid_argument;
            // Separate depth/stencil layouts are not enabled by this backend. Never widen the requested range.
            if (static_cast<uint32_t>(info.aspects) == 6U && range.aspects != info.aspects)
                return rhi::error::unsupported;
            auto stages = static_cast<uint32_t>(one.transition.before.scope.stages) | static_cast<uint32_t>(one.transition.after.scope.stages);
            if (stages & ~supported)
                return rhi::error::unsupported;
            if (!state_usage(one.transition.before, info.usage_mask) || !state_usage(one.transition.after, info.usage_mask))
                return rhi::error::invalid_argument;
        }
        for (auto const& one : buffers) {
            if (!one.resource)
                return rhi::error::invalid_argument;
            rhi::scoped_buffer_info info;
            auto error = describe(*one.resource, info);
            if (error != rhi::error::ok)
                return error;
            auto mapped = native_sync::map_buffer(one.transition);
            if (!mapped)
                return mapped_error(mapped.error());
            if (one.transition.offset >= info.byte_size || one.transition.size > info.byte_size - one.transition.offset)
                return rhi::error::invalid_argument;
            auto stages = static_cast<uint32_t>(one.transition.before.stages) | static_cast<uint32_t>(one.transition.after.stages);
            if (stages & ~supported)
                return rhi::error::unsupported;
        }
        return rhi::error::ok;
    }

    rhi::error core::scoped_sync_view::record(rhi::command_buffer& commands, std::span<rhi::scoped_image_barrier const> images, std::span<rhi::scoped_buffer_barrier const> buffers) const noexcept {
        if (!owner)
            return rhi::error::not_ready;
        frame_commands* list = nullptr;
        if (&commands == &owner->commands_view)
            list = &owner->commands_view;
        else {
            std::lock_guard const lock(owner->contract_command_buffers_mutex);
            if (!owner->contract_command_buffers.contains(&commands))
                return rhi::error::invalid_argument;
            list = static_cast<owned_command_buffer*>(&commands);
        }
        if (list->owner != owner)
            return rhi::error::invalid_argument;
        if (!list->recording || list->rendering_active || list->native() == VK_NULL_HANDLE)
            return rhi::error::not_ready;
        auto error = validate(images, buffers);
        if (error != rhi::error::ok)
            return error;
        // Frame targets may only be recorded into the acquired frame's primary commands.
        if (list != &owner->commands_view) {
            std::lock_guard const lock(owner->contract_images_mutex);
            for (auto const& one : images)
                if (owner->frame_snapshots.contains(one.resource))
                    return rhi::error::invalid_argument;
        }
        if (images.empty() && buffers.empty())
            return rhi::error::ok;
        deren::utility::vessel::vector<VkImageMemoryBarrier2> native_images;
        deren::utility::vessel::vector<VkBufferMemoryBarrier2> native_buffers;
        native_images.reserve(images.size());
        native_buffers.reserve(buffers.size());
        for (auto const& one : images) {
            auto barrier = *native_sync::map_image(one.transition);
            {
                std::lock_guard const lock(owner->contract_images_mutex);
                if (owner->frame_snapshots.contains(one.resource)) {
                    auto const& snapshot = *static_cast<frame_snapshot const*>(one.resource);
                    if (!snapshot.current())
                        return rhi::error::invalid_argument;
                    barrier.image = snapshot.native_handle;
                    // Discarding texels does not discard the WSI acquire dependency.
                    // The graph's UNDEFINED frontier has no previous GPU access;
                    // the backend additionally chains its layout transition after
                    // this submission's acquire-semaphore wait (color output).
                    if (owner->create_options.native_window && barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
                        barrier.srcStageMask |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                } else if (owner->contract_images.contains(one.resource)) {
                    barrier.image = static_cast<owned_image const*>(one.resource)->native_handle;
                } else {
                    return rhi::error::invalid_argument;
                }
            }
            native_images.push_back(barrier);
        }
        for (auto const& one : buffers) {
            auto barrier = *native_sync::map_buffer(one.transition);
            barrier.buffer = static_cast<owned_buffer const*>(one.resource)->native;
            native_buffers.push_back(barrier);
        }
        VkDependencyInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        info.imageMemoryBarrierCount = static_cast<uint32_t>(native_images.size());
        info.pImageMemoryBarriers = native_images.data();
        info.bufferMemoryBarrierCount = static_cast<uint32_t>(native_buffers.size());
        info.pBufferMemoryBarriers = native_buffers.data();
        vkCmdPipelineBarrier2(list->native(), &info);
        return rhi::error::ok;
    }
} // namespace deren::vulkan
