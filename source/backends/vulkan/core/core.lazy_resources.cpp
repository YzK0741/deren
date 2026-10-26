module;
#include "vma/texture_upload_layout.h"

#include <algorithm>
#include <bit>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.utility.shared;
import :declarations;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;

    std::expected<rhi::unique_object<rhi::lazy_buffer>, rhi::error>
    core::create_lazy_buffer(rhi::lazy_buffer_desc const& request) {
        auto checked = rhi::validate_lazy_buffer(request);
        if (checked != rhi::error::ok)
            return std::unexpected(checked);
        auto const& desc = request.resource;
        constexpr auto allowed = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::storage);
        if (desc.flags & ~allowed)
            return std::unexpected(rhi::error::unsupported);
        auto value = std::unique_ptr<owned_buffer>(new (std::nothrow) owned_buffer);
        if (!value)
            return std::unexpected(rhi::error::out_of_host_memory);
        value->owner = this;
        value->source = request.data;
        value->description = desc;

        value->size_bytes = desc.size;
        value->declared_usage = desc.usage;
        value->declared_flags = desc.flags;
        value->addressable = rhi::has_flag(desc.flags, rhi::buffer_flag::device_address);
        value->loading.value.store(rhi::lazy_state::unloaded);
        {
            std::lock_guard lock(contract_buffers_mutex);
            value->identity = resource_identity.fetch_add(1, std::memory_order_relaxed);
            contract_buffers.insert(value.get());
        }
        return rhi::unique_object<rhi::lazy_buffer>(value.release());
    }

    std::expected<rhi::unique_object<rhi::lazy_image>, rhi::error>
    core::create_lazy_image(rhi::lazy_image_desc const& request) {
        if (request.struct_size < sizeof(request) || request.resource.struct_size < sizeof(rhi::image_desc) ||
            !request.data || request.data->empty() || !request.resource.initial_bytes.empty())
            return std::unexpected(rhi::error::invalid_argument);
        auto desc = request.resource;
        auto const format = resolve_format(desc.format);
        auto const allowed = rhi::to_bits(rhi::image_flag::sampled) | rhi::to_bits(rhi::image_flag::cube_compatible);
        if (!rhi::has_flag(desc.flags, rhi::image_flag::sampled) || (desc.flags & ~allowed) ||
            desc.format == rhi::image_format::depth || format == VK_FORMAT_UNDEFINED)
            return std::unexpected(rhi::error::unsupported);
        if (!desc.extent.width || !desc.extent.height || desc.extent.depth != 1 || !desc.array_layers ||
            (rhi::has_flag(desc.flags, rhi::image_flag::cube_compatible) &&
             (desc.array_layers != 6 || desc.extent.width != desc.extent.height)))
            return std::unexpected(rhi::error::invalid_argument);
        auto max_mips = static_cast<std::uint32_t>(std::bit_width(std::max(desc.extent.width, desc.extent.height)));
        if (!desc.mip_levels)
            desc.mip_levels = max_mips;
        if (desc.mip_levels > max_mips)
            return std::unexpected(rhi::error::invalid_argument);
        auto packed = detail::packed_image_size(format, desc.extent.width, desc.extent.height, desc.array_layers, desc.mip_levels);
        if (!packed || *packed != request.data->size())
            return std::unexpected(rhi::error::invalid_argument);
        VkImageFormatProperties properties{};
        auto flags = rhi::has_flag(desc.flags, rhi::image_flag::cube_compatible) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u;
        auto supported = vkGetPhysicalDeviceImageFormatProperties(physical_device, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT,
                                                                  flags, &properties);
        if (supported != VK_SUCCESS)
            return std::unexpected(rhi::error::unsupported);
        if (desc.extent.width > properties.maxExtent.width || desc.extent.height > properties.maxExtent.height ||
            desc.array_layers > properties.maxArrayLayers || desc.mip_levels > properties.maxMipLevels)
            return std::unexpected(rhi::error::invalid_argument);
        auto value = std::unique_ptr<owned_image>(new (std::nothrow) owned_image);
        if (!value)
            return std::unexpected(rhi::error::out_of_host_memory);
        value->owner = this;
        value->source = request.data;
        value->description = desc;
        value->debug_name = desc.debug_name ? desc.debug_name : "lazy image";
        value->description.debug_name = value->debug_name.c_str();
        value->width = desc.extent.width;
        value->height = desc.extent.height;
        value->mip_levels = desc.mip_levels;
        value->array_layers = desc.array_layers;
        value->resolved_format = format;
        value->declared_format = desc.format;
        value->declared_flags = desc.flags;
        value->cube_compatible = rhi::has_flag(desc.flags, rhi::image_flag::cube_compatible);
        value->loading.value.store(rhi::lazy_state::unloaded);
        {
            std::lock_guard lock(contract_images_mutex);
            contract_images.insert(value.get());
        }
        return rhi::unique_object<rhi::lazy_image>(value.release());
    }

    rhi::error core::owned_buffer::unload() noexcept {
        if (!source)
            return rhi::error::unsupported;
        return loading.unload([&] {
            owned = {};
            native = VK_NULL_HANDLE;
            mapped_bytes = nullptr;
        });
    }
    rhi::error core::owned_image::unload() noexcept {
        if (!source)
            return rhi::error::unsupported;
        return loading.unload([&] {
            owned = {};
            native_handle = VK_NULL_HANDLE;
        });
    }

    std::shared_ptr<void> core::depend_on(owned_buffer const& resource) {
        if (!resource.source)
            return {};
        auto* value = const_cast<owned_buffer*>(&resource);
        std::lock_guard lock(value->loading.mutex);
        value->references.fetch_add(1, std::memory_order_relaxed);
        value->loading.users.fetch_add(1, std::memory_order_relaxed);
        return std::shared_ptr<void>(value, [](owned_buffer* item) {
            item->loading.users.fetch_sub(1, std::memory_order_release);
            item->release();
        });
    }
    std::shared_ptr<void> core::depend_on(owned_image const& resource) {
        if (!resource.source)
            return {};
        auto* value = const_cast<owned_image*>(&resource);
        std::lock_guard lock(value->loading.mutex);
        value->references.fetch_add(1, std::memory_order_relaxed);
        value->loading.users.fetch_add(1, std::memory_order_relaxed);
        return std::shared_ptr<void>(value, [](owned_image* item) {
            item->loading.users.fetch_sub(1, std::memory_order_release);
            item->release();
        });
    }

    rhi::error core::materialize(owned_buffer& value) noexcept {
        if (!value.source)
            return rhi::error::ok;
        return value.loading.run([&]() noexcept {
            auto desc = value.description;
            // The existing uniform GPU allocator supports staging initialization and storage descriptors.
            if (desc.usage == rhi::buffer_usage::storage_gpu_only) {
                desc.usage = rhi::buffer_usage::uniform_gpu_only;
                desc.flags |= rhi::to_bits(rhi::buffer_flag::storage);
            }
            desc.initial_bytes = std::as_bytes(std::span(*value.source));
            auto physical = create_buffer(desc);
            if (!physical) {
                return rhi::error::operation_failed;
            }
            auto& concrete = *static_cast<owned_buffer*>(physical.get());
            value.owned = std::move(concrete.owned);
            value.native = concrete.native;
            return rhi::error::ok;
        });
    }

    rhi::error core::materialize(owned_image& value) noexcept {
        if (!value.source)
            return rhi::error::ok;
        return value.loading.run([&]() noexcept {
            auto desc = value.description;
            desc.initial_bytes = std::as_bytes(std::span(*value.source));
            auto physical = create_image(desc);
            if (!physical) {
                return rhi::error::operation_failed;
            }
            auto& concrete = *static_cast<owned_image*>(physical.get());
            value.owned = std::move(concrete.owned);
            value.native_handle = concrete.native_handle;
            return rhi::error::ok;
        });
    }

    rhi::error core::materialize(owned_image_view& value) noexcept {
        std::lock_guard lock(value.loading_mutex);
        if (value.native_view)
            return rhi::error::ok;
        if (!value.parent_image)
            return rhi::error::invalid_argument;
        auto& image = *static_cast<owned_image*>(value.parent_image.get());
        auto status = materialize(image);
        if (status != rhi::error::ok)
            return status;
        auto desc = value.description;
        auto layers = desc.layer_count ? desc.layer_count : image.array_layers - desc.base_layer;
        auto mips = desc.mip_count ? desc.mip_count : image.mip_levels - desc.base_mip;
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = image.native_handle;
        info.viewType = layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : image.cube_compatible && layers == 6 && desc.base_layer == 0 ? VK_IMAGE_VIEW_TYPE_CUBE
                                                                                                                           : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        info.format = image.resolved_format;
        info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, desc.base_mip, mips, desc.base_layer, layers};
        return vkCreateImageView(logical_device, &info, nullptr, &value.native_view) == VK_SUCCESS ? rhi::error::ok : rhi::error::operation_failed;
    }

    rhi::error core::materialize(owned_texture_group& group) noexcept {
        return group.loading.run([&]() noexcept {
            decltype(group.deferred_images) images;
            {
                std::lock_guard lock(texture_groups.mutex);
                images = group.deferred_images;
            }
            for (std::size_t i = 0; i < images.size(); ++i) {
                if (!images[i])
                    continue;
                auto loaded = materialize(*static_cast<owned_image*>(images[i].get()));
                if (loaded != rhi::error::ok)
                    return loaded;
                std::lock_guard lock(texture_groups.mutex);
                loaded = write_group_texture_locked(group, static_cast<std::uint32_t>(i), *images[i]);
                if (loaded != rhi::error::ok)
                    return loaded;
            }
            return rhi::error::ok;
        });
    }

    rhi::error core::materialize(owned_resource_group& group) noexcept {
        std::lock_guard lock(group.loading_mutex);
        if (!group.deferred || group.physical)
            return rhi::error::ok;
        for (auto const& entry : group.entries) {
            auto* resource = entry.resource.get();
            rhi::error loaded = rhi::error::ok;
            if (entry.kind == rhi::resource_kind::uniform_buffer || entry.kind == rhi::resource_kind::storage_buffer)
                loaded = materialize(*static_cast<owned_buffer*>(resource));
            else if (entry.kind == rhi::resource_kind::sampled_image || entry.kind == rhi::resource_kind::storage_image)
                loaded = materialize(*static_cast<owned_image*>(resource));
            else if (entry.kind == rhi::resource_kind::texture_group)
                loaded = materialize(*static_cast<owned_texture_group*>(resource));
            if (loaded != rhi::error::ok)
                return loaded;
        }
        auto physical = create_resource_group({.entries = group.entries});
        if (!physical)
            return physical.error();
        group.physical = std::move(*physical);
        group.values = static_cast<owned_resource_group*>(group.physical.get())->values;
        return rhi::error::ok;
    }

    rhi::error core::prepare_lazy_descriptors(frame_commands& recorder) noexcept {
        std::lock_guard lock(lazy_descriptors_mutex);
        for (auto& [slot, binding] : lazy_descriptors) {
            rhi::error status;
            if (binding.image) {
                status = recorder.demand(*binding.image_write.resource);
                if (status == rhi::error::ok && !binding.published) {
                    status = heap_view.write_image_native({.offset = static_cast<std::uint64_t>(slot) * heap_slot_stride,
                                                           .resource = binding.image_write.resource,
                                                           .view = &binding.view,
                                                           .type = binding.image_write.type},
                                                          true);
                }
            } else {
                status = recorder.demand(*binding.buffer_write.resource);
                if (status == rhi::error::ok && !binding.published) {
                    auto const& info = binding.buffer_write;
                    auto address = internal_address(*static_cast<owned_buffer const*>(info.resource), info.offset);
                    status = address ? heap_view.write_buffer({.offset = static_cast<std::uint64_t>(slot) * heap_slot_stride,
                                                               .address = address,
                                                               .size = info.size,
                                                               .type = rhi::descriptor_type::storage_buffer})
                                     : rhi::error::operation_failed;
                }
            }
            if (status != rhi::error::ok) {
                recorder.recording_error = status;
                recorder.executable = false;
                return status;
            }
            binding.published = true;
        }
        return rhi::error::ok;
    }

    std::uint64_t core::internal_address(owned_buffer const& buffer, std::uint64_t offset) const noexcept {
        if (!buffer.addressable || (buffer.source && buffer.state() != rhi::lazy_state::ready) || !buffer.native)
            return 0;
        VkBufferDeviceAddressInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = buffer.native;
        return vkGetBufferDeviceAddress(logical_device, &info) + offset;
    }

    rhi::error core::frame_commands::demand(rhi::buffer const& resource) {
        {
            std::lock_guard lock(owner->contract_buffers_mutex);
            if (!owner->contract_buffers.contains(&resource))
                return rhi::error::invalid_argument;
        }
        auto& value = const_cast<owned_buffer&>(static_cast<owned_buffer const&>(resource));
        retain_lazy(value);
        auto result = owner->materialize(value);
        if (result != rhi::error::ok) {
            recording_error = result;
            executable = false;
        }
        return result;
    }
    rhi::error core::frame_commands::demand(rhi::image const& resource) {
        {
            std::lock_guard lock(owner->contract_images_mutex);
            if (!owner->contract_images.contains(&resource))
                return rhi::error::invalid_argument;
        }
        auto& value = const_cast<owned_image&>(static_cast<owned_image const&>(resource));
        retain_lazy(value);
        auto result = owner->materialize(value);
        if (result != rhi::error::ok) {
            recording_error = result;
            executable = false;
        }
        return result;
    }

    void core::frame_commands::retain_lazy(owned_buffer const& resource) {
        if (!resource.source || std::any_of(lazy_refs.begin(), lazy_refs.end(), [&resource](auto const& ref) { return ref.get() == &resource; }))
            return;
        lazy_refs.push_back(owner->depend_on(resource));
    }
    void core::frame_commands::retain_lazy(owned_image const& resource) {
        if (!resource.source || std::any_of(lazy_refs.begin(), lazy_refs.end(), [&resource](auto const& ref) { return ref.get() == &resource; }))
            return;
        lazy_refs.push_back(owner->depend_on(resource));
    }
} // namespace deren::vulkan
