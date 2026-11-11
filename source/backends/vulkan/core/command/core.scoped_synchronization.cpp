module;

#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.vstd;
import deren.promise.rhi;
import deren.vulkan.constant_init;
import deren.vulkan.synchronization;
import deren.utility.shared;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;
    namespace scopes = rhi::synchronization;
    namespace native_sync = deren::vulkan::synchronization;
    namespace {
        constexpr std::uint32_t bit(scopes::usage value) noexcept {
            return static_cast<std::uint32_t>(value);
        }
        constexpr scopes::aspect format_aspects(VkFormat const format) noexcept {
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
        constexpr bool state_usage(scopes::image_state const state, std::uint32_t const usage) noexcept {
            switch (state.layout) {
            case scopes::image_layout::undefined:
                [[fallthrough]];
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
        if (!parent) {
            return scopes::stage::none;
        }
        auto const flags = parent->command.synchronization_queue_flags;
        std::uint32_t result = 0;
        if (flags & VK_QUEUE_GRAPHICS_BIT) {
            result |= 1U | 2U | 8U | 16U | 64U;
        }
        if ((flags & VK_QUEUE_GRAPHICS_BIT) && parent->device.mesh_shader_available) {
            result |= 512U;
        }
        if ((flags & VK_QUEUE_GRAPHICS_BIT) && parent->device.task_shader_available) {
            result |= 256U;
        }
        if (flags & VK_QUEUE_COMPUTE_BIT) {
            result |= 4U;
        }
        if (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            result |= 128U;
        }
        if (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) {
            result |= 32U;
        }
        // RT stage 1: the ray-tracing PIPELINE stages, reported only when the device can run them. A ray query inside
        // a compute shader needs none of this bit (it is a COMPUTE_SHADER site, and 4U already says so); this is the
        // traceRays launch's stage, which the RT shadow pass's visibility write names.
        if ((parent->device.ray_query_available || parent->device.ray_tracing_pipeline_available) && (flags & VK_QUEUE_GRAPHICS_BIT)) {
            result |= 1024U;
        }
        return static_cast<scopes::stage>(result);
    }

    rhi::error core::scoped_sync_view::describe(rhi::image const& resource, rhi::scoped_image_info& output) const noexcept {
        if (!parent) {
            return rhi::error::not_ready;
        }
        std::lock_guard const lock(parent->resource.contract_images_mutex);
        if (parent->frame.frame_snapshots.contains(&resource)) {
            auto const& image = static_cast<frame_snapshot const&>(resource);
            if (image.parent != parent || !image.current()) {
                return rhi::error::invalid_argument;
            }
            std::uint32_t usage = bit(scopes::usage::color_attachment);
            if (parent->frame.swapchain_transfer_src_supported) {
                usage |= bit(scopes::usage::transfer_source);
            }
            usage |= bit(parent->create_options.native_window ? scopes::usage::present : scopes::usage::sampled);
            output = {1, 1, scopes::aspect::color, usage, reinterpret_cast<std::uintptr_t>(&image)};
            return rhi::error::ok;
        }
        if (!parent->resource.contract_images.contains(&resource)) {
            return rhi::error::invalid_argument;
        }
        auto const& image = static_cast<core::image const&>(resource);
        if (image.parent != parent || (!image.source && image.native_handle == VK_NULL_HANDLE)) {
            return rhi::error::invalid_argument;
        }
        std::uint32_t usage = 0;
        auto add = [&](rhi::image_flag const flag, scopes::usage const target) noexcept { if (rhi::has_flag(image.declared_flags, flag)) { usage |= bit(target);
} };
        add(rhi::image_flag::sampled, scopes::usage::sampled);
        add(rhi::image_flag::storage, scopes::usage::storage);
        add(rhi::image_flag::color_attachment, scopes::usage::color_attachment);
        add(rhi::image_flag::depth_attachment, scopes::usage::depth_stencil_attachment);
        add(rhi::image_flag::depth_attachment, scopes::usage::depth_stencil_read);
        add(rhi::image_flag::transfer_source, scopes::usage::transfer_source);
        add(rhi::image_flag::transfer_destination, scopes::usage::transfer_destination);
        if (image.source) {
            output = {image.mip_levels, image.array_layers, format_aspects(image.resolved_format), usage, reinterpret_cast<std::uintptr_t>(&image)};
            return rhi::error::ok;
        }
        auto const* detail = parent->device.vma.get_image_detail(image.allocation.handle());
        if (!detail) {
            return rhi::error::invalid_argument;
        }
        output = {.mip_levels = image.mip_levels, .array_layers = image.array_layers, .aspects = format_aspects(image.resolved_format), .usage_mask = usage, .canonical_object = reinterpret_cast<std::uintptr_t>(detail)};
        return rhi::error::ok;
    }

    rhi::error core::scoped_sync_view::describe(rhi::buffer const& resource, rhi::scoped_buffer_info& output) const noexcept {
        if (!parent) {
            return rhi::error::not_ready;
        }
        std::lock_guard const lock(parent->resource.contract_buffers_mutex);
        if (!parent->resource.contract_buffers.contains(&resource)) {
            return rhi::error::invalid_argument;
        }
        auto const& buffer = static_cast<core::buffer const&>(resource);
        if (buffer.parent != parent || (!buffer.source && buffer.native == VK_NULL_HANDLE)) {
            return rhi::error::invalid_argument;
        }
        std::uint32_t usage = 0;
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
        auto add = [&](rhi::buffer_flag flag, scopes::usage target) noexcept { if (rhi::has_flag(buffer.declared_flags, flag)) { usage |= bit(target); 
} };
        add(rhi::buffer_flag::storage, scopes::usage::storage);
        add(rhi::buffer_flag::indirect, scopes::usage::indirect_buffer);
        add(rhi::buffer_flag::transfer_source, scopes::usage::transfer_source);
        add(rhi::buffer_flag::transfer_destination, scopes::usage::transfer_destination);
        if (buffer.source) {
            output = {buffer.size_bytes, usage, reinterpret_cast<std::uintptr_t>(&buffer)};
            return rhi::error::ok;
        }
        auto const* detail = parent->device.vma.get_buffer_detail(buffer.allocation.handle());
        if (!detail) {
            return rhi::error::invalid_argument;
        }
        output = {buffer.size_bytes, usage, reinterpret_cast<std::uintptr_t>(detail)};
        return rhi::error::ok;
    }

    rhi::error core::scoped_sync_view::validate(std::span<rhi::scoped_image_barrier const> images, std::span<rhi::scoped_buffer_barrier const> buffers) const noexcept {
        if (!parent) {
            return rhi::error::not_ready;
        }
        if (images.size() > std::numeric_limits<std::uint32_t>::max() || buffers.size() > std::numeric_limits<std::uint32_t>::max()) {
            return rhi::error::invalid_argument;
        }
        auto supported = static_cast<std::uint32_t>(supported_stages());
        for (auto const& [resource, transition] : images) {
            if (!resource) {
                return rhi::error::invalid_argument;
            }
            rhi::scoped_image_info info;
            auto error = describe(*resource, info);
            if (error != rhi::error::ok) {
                return error;
            }
            auto mapped = native_sync::map_image(transition);
            if (!mapped) {
                return mapped_error(mapped.error());
            }
            auto const& range = transition.range;
            if (range.base_mip >= info.mip_levels || range.mip_count > info.mip_levels - range.base_mip || range.base_layer >= info.array_layers || range.layer_count > info.array_layers - range.base_layer || (static_cast<std::uint32_t>(range.aspects) & ~static_cast<std::uint32_t>(info.aspects))) {
                return rhi::error::invalid_argument;
            }
            // Separate depth/stencil layouts are not enabled by this backend. Never widen the requested range.
            if (static_cast<std::uint32_t>(info.aspects) == 6U && range.aspects != info.aspects) {
                return rhi::error::unsupported;
            }
            auto stages = static_cast<std::uint32_t>(transition.before.scope.stages) | static_cast<std::uint32_t>(transition.after.scope.stages);
            if (stages & ~supported) {
                return rhi::error::unsupported;
            }
            if (!state_usage(transition.before, info.usage_mask) || !state_usage(transition.after, info.usage_mask)) {
                return rhi::error::invalid_argument;
            }
        }
        for (auto const& [resource, transition] : buffers) {
            if (!resource) {
                return rhi::error::invalid_argument;
            }
            rhi::scoped_buffer_info info;
            auto const error = describe(*resource, info);
            if (error != rhi::error::ok) {
                return error;
            }
            if (auto mapped = native_sync::map_buffer(transition); !mapped) {
                return mapped_error(mapped.error());
            }
            if (transition.offset >= info.byte_size || transition.size > info.byte_size - transition.offset) {
                return rhi::error::invalid_argument;
            }
            auto stages = static_cast<std::uint32_t>(transition.before.stages) | static_cast<std::uint32_t>(transition.after.stages);
            if (stages & ~supported) {
                return rhi::error::unsupported;
            }
        }
        return rhi::error::ok;
    }

    rhi::error core::scoped_sync_view::record(rhi::command_recorder& recorder, std::span<rhi::scoped_image_barrier const> images, std::span<rhi::scoped_buffer_barrier const> buffers) const noexcept {
        if (!parent) {
            return rhi::error::not_ready;
        }
        rhi::error resolved{};
        frame_commands* const list = parent->resolve_recorder(&recorder, resolved);
        if (resolved != rhi::error::ok) {
            return resolved;
        }
        if (!list->recording || list->rendering_active || list->native() == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        auto error = validate(images, buffers);
        if (error != rhi::error::ok) {
            return error;
        }
        // Frame targets may only be recorded into the acquired frame's primary commands.
        if (list != &parent->frame.commands_view) {
            std::lock_guard const lock(parent->resource.contract_images_mutex);
            for (auto const& one : images) {
                if (parent->frame.frame_snapshots.contains(one.resource)) {
                    return rhi::error::invalid_argument;
                }
            }
        }
        if (images.empty() && buffers.empty()) {
            return rhi::error::ok;
        }
        for (auto const& [resource, transition] : images) {
            bool registered;
            {
                std::lock_guard lock(parent->resource.contract_images_mutex);
                registered = parent->resource.contract_images.contains(resource);
            }
            if (registered) {
                auto loaded = list->demand(*resource);
                if (loaded != rhi::error::ok) {
                    return loaded;
                }
            }
        }
        for (auto const& [resource, transition] : buffers) {
            auto const loaded = list->demand(*resource);
            if (loaded != rhi::error::ok) {
                return loaded;
            }
        }
        deren::utility::vessel::vector<VkImageMemoryBarrier2> native_images;
        deren::utility::vessel::vector<VkBufferMemoryBarrier2> native_buffers;
        native_images.reserve(images.size());
        native_buffers.reserve(buffers.size());
        for (auto const& one : images) {
            auto barrier = *native_sync::map_image(one.transition);
            {
                std::lock_guard const lock(parent->resource.contract_images_mutex);
                if (parent->frame.frame_snapshots.contains(one.resource)) {
                    auto const& snapshot = *static_cast<frame_snapshot const*>(one.resource);
                    if (!snapshot.current()) {
                        return rhi::error::invalid_argument;
                    }
                    barrier.image = snapshot.native_handle;
                    // Discarding texels does not discard the WSI acquire dependency.
                    // The graph's UNDEFINED frontier has no previous GPU access;
                    // the backend additionally chains its layout transition after
                    // this submission's acquire-semaphore wait (color output).
                    if (parent->create_options.native_window && barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
                        barrier.srcStageMask |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    }
                } else if (parent->resource.contract_images.contains(one.resource)) {
                    barrier.image = static_cast<core::image const*>(one.resource)->native_handle;
                } else {
                    return rhi::error::invalid_argument;
                }
            }
            native_images.push_back(barrier);
        }
        for (auto const& one : buffers) {
            auto barrier = *native_sync::map_buffer(one.transition);
            barrier.buffer = static_cast<core::buffer const*>(one.resource)->native;
            native_buffers.push_back(barrier);
        }
        VkDependencyInfo const info = make_dependency_info(static_cast<std::uint32_t>(native_images.size()), native_images.data(), static_cast<std::uint32_t>(native_buffers.size()), native_buffers.data());
        if (auto applied = list->apply_operations(); applied != rhi::error::ok) {
            return applied;
        }
        vkCmdPipelineBarrier2(list->native(), &info);
        return rhi::error::ok;
    }
} // namespace deren::vulkan
