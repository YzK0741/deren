module;
#include "../vma/texture_upload_layout.h"

#include <cstddef>
#include <cstdint>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.vstd;
import deren.utility.shared;
import :declarations;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;

    rhi::error core::buffer::unload() noexcept {
        if (!source) {
            return rhi::error::unsupported;
        }
        return loading.unload([&] {
            allocation = {};
            native = VK_NULL_HANDLE;
            mapped_bytes = nullptr;
        });
    }
    rhi::error core::image::unload() noexcept {
        if (!source) {
            return rhi::error::unsupported;
        }
        return loading.unload([&] {
            allocation = {};
            native_handle = VK_NULL_HANDLE;
        });
    }

    std::shared_ptr<void> core::depend_on(core::buffer const& resource) {
        if (!resource.source) {
            return {};
        }
        auto* value = const_cast<core::buffer*>(&resource);
        std::lock_guard lock(value->loading.mutex);
        value->references.fetch_add(1, std::memory_order_relaxed);
        value->loading.users.fetch_add(1, std::memory_order_relaxed);
        return std::shared_ptr<void>(value, [](core::buffer* item) {
            item->loading.users.fetch_sub(1, std::memory_order_release);
            item->release();
        });
    }
    std::shared_ptr<void> core::depend_on(core::image const& resource) {
        if (!resource.source) {
            return {};
        }
        auto* value = const_cast<core::image*>(&resource);
        std::lock_guard lock(value->loading.mutex);
        value->references.fetch_add(1, std::memory_order_relaxed);
        value->loading.users.fetch_add(1, std::memory_order_relaxed);
        return std::shared_ptr<void>(value, [](core::image* item) {
            item->loading.users.fetch_sub(1, std::memory_order_release);
            item->release();
        });
    }

    rhi::error core::materialize(core::buffer& value) noexcept {
        if (!value.source) {
            return rhi::error::ok;
        }
        return value.loading.run([&]() noexcept {
            auto desc = value.description;
            // The existing uniform GPU allocator supports staging initialization and storage descriptors.
            if (desc.usage == rhi::buffer_usage::storage_gpu_only) {
                desc.usage = rhi::buffer_usage::uniform_gpu_only;
                desc.flags |= rhi::to_bits(rhi::buffer_flag::storage);
            }
            desc.initial_bytes = std::as_bytes(std::span(*value.source));
            auto physical = this->resource.create_buffer(desc);
            if (!physical) {
                return rhi::error::operation_failed;
            }
            auto& concrete = *static_cast<core::buffer*>(physical.get());
            value.allocation = std::move(concrete.allocation);
            value.native = concrete.native;
            return rhi::error::ok;
        });
    }

    rhi::error core::materialize(core::image& value) noexcept {
        if (!value.source) {
            return rhi::error::ok;
        }
        return value.loading.run([&]() noexcept {
            auto desc = value.description;
            desc.initial_bytes = std::as_bytes(std::span(*value.source));
            auto physical = this->resource.create_image(desc);
            if (!physical) {
                return rhi::error::operation_failed;
            }
            auto& concrete = *static_cast<core::image*>(physical.get());
            value.allocation = std::move(concrete.allocation);
            value.native_handle = concrete.native_handle;
            return rhi::error::ok;
        });
    }

    rhi::error core::materialize(core::image_view& value) noexcept {
        std::lock_guard lock(value.loading_mutex);
        if (value.native_view) {
            return rhi::error::ok;
        }
        if (!value.parent_image) {
            return rhi::error::invalid_argument;
        }
        auto& image = *static_cast<core::image*>(value.parent_image.get());
        auto status = materialize(image);
        if (status != rhi::error::ok) {
            return status;
        }
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
        return vkCreateImageView(this->device.logical_device, &info, nullptr, &value.native_view) == VK_SUCCESS ? rhi::error::ok : rhi::error::operation_failed;
    }

    rhi::error core::materialize(core::texture_group& group) noexcept {
        return group.loading.run([&]() noexcept {
            decltype(group.deferred_images) images;
            {
                std::lock_guard lock(resource.texture_groups.mutex);
                images = group.deferred_images;
            }
            for (std::size_t i = 0; i < images.size(); ++i) {
                if (!images[i]) {
                    continue;
                }
                auto loaded = materialize(*static_cast<core::image*>(images[i].get()));
                if (loaded != rhi::error::ok) {
                    return loaded;
                }
                std::lock_guard lock(resource.texture_groups.mutex);
                loaded = write_group_texture_locked(group, static_cast<std::uint32_t>(i), *images[i]);
                if (loaded != rhi::error::ok) {
                    return loaded;
                }
            }
            return rhi::error::ok;
        });
    }

    rhi::error core::materialize(core::resource_group& group) noexcept {
        std::lock_guard lock(group.loading_mutex);
        if (!group.deferred || group.physical) {
            return rhi::error::ok;
        }
        for (auto const& entry : group.entries) {
            auto* resource = entry.resource.get();
            rhi::error loaded = rhi::error::ok;
            if (entry.kind == rhi::resource_kind::uniform_buffer || entry.kind == rhi::resource_kind::storage_buffer) {
                loaded = materialize(*static_cast<core::buffer*>(resource));
            } else if (entry.kind == rhi::resource_kind::sampled_image || entry.kind == rhi::resource_kind::storage_image) {
                loaded = materialize(*static_cast<core::image*>(resource));
            } else if (entry.kind == rhi::resource_kind::texture_group) {
                loaded = materialize(*static_cast<core::texture_group*>(resource));
            }
            if (loaded != rhi::error::ok) {
                return loaded;
            }
        }
        auto physical = this->resource.create_resource_group({.entries = group.entries});
        if (!physical) {
            return physical.error();
        }
        group.physical = std::move(*physical);
        group.values = static_cast<core::resource_group*>(group.physical.get())->values;
        return rhi::error::ok;
    }

    rhi::error core::prepare_lazy_descriptors(frame_commands& recorder) noexcept {
        std::lock_guard lock(resource.lazy_descriptors_mutex);
        for (auto& [slot, binding] : resource.lazy_descriptors) {
            rhi::error status;
            if (binding.image) {
                status = recorder.demand(*binding.image_write.resource);
                if (status == rhi::error::ok && !binding.published) {
                    status = resource.heap_view.write_image_native({.offset = static_cast<std::uint64_t>(slot) * heap_slot_stride,
                                                                    .resource = binding.image_write.resource,
                                                                    .view = &binding.view,
                                                                    .type = binding.image_write.type},
                                                                   true);
                }
            } else {
                status = recorder.demand(*binding.buffer_write.resource);
                if (status == rhi::error::ok && !binding.published) {
                    auto const& info = binding.buffer_write;
                    auto address = internal_address(*static_cast<core::buffer const*>(info.resource), info.offset);
                    status = address ? resource.heap_view.write_buffer({.offset = static_cast<std::uint64_t>(slot) * heap_slot_stride,
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

    std::uint64_t core::internal_address(core::buffer const& buffer, std::uint64_t offset) const noexcept {
        if (!buffer.addressable || (buffer.source && buffer.state() != rhi::lazy_state::ready) || !buffer.native) {
            return 0;
        }
        VkBufferDeviceAddressInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = buffer.native;
        return vkGetBufferDeviceAddress(this->device.logical_device, &info) + offset;
    }

    rhi::error core::frame_commands::demand(rhi::buffer const& resource) {
        {
            std::lock_guard lock(parent->resource.contract_buffers_mutex);
            if (!parent->resource.contract_buffers.contains(&resource)) {
                return rhi::error::invalid_argument;
            }
        }
        auto& value = const_cast<core::buffer&>(static_cast<core::buffer const&>(resource));
        retain_lazy(value);
        auto result = parent->materialize(value);
        if (result != rhi::error::ok) {
            recording_error = result;
            executable = false;
        }
        return result;
    }
    rhi::error core::frame_commands::demand(rhi::image const& resource) {
        {
            std::lock_guard lock(parent->resource.contract_images_mutex);
            if (!parent->resource.contract_images.contains(&resource)) {
                return rhi::error::invalid_argument;
            }
        }
        auto& value = const_cast<core::image&>(static_cast<core::image const&>(resource));
        retain_lazy(value);
        auto result = parent->materialize(value);
        if (result != rhi::error::ok) {
            recording_error = result;
            executable = false;
        }
        return result;
    }

    void core::frame_commands::retain_lazy(core::buffer const& resource) {
        if (!resource.source || std::any_of(lazy_refs.begin(), lazy_refs.end(), [&resource](auto const& ref) { return ref.get() == &resource; })) {
            return;
        }
        lazy_refs.push_back(parent->depend_on(resource));
    }
    void core::frame_commands::retain_lazy(core::image const& resource) {
        if (!resource.source || std::any_of(lazy_refs.begin(), lazy_refs.end(), [&resource](auto const& ref) { return ref.get() == &resource; })) {
            return;
        }
        lazy_refs.push_back(parent->depend_on(resource));
    }
} // namespace deren::vulkan
