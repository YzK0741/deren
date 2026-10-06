module;

#include <algorithm> // std::max/min in the buffer-creation helpers
#include <cstddef>
#include <cstdint>
#include <source_location>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

module deren.vulkan.init_utils;

import deren.utility;
import deren.vulkan.core;

namespace deren::vulkan::init_utils {

    void create_host_buffer(core& device,
                            std::span<std::byte const> const initial,
                            buffer_type const type,
                            std::string_view const what,
                            vk_buffer& buffer,
                            void*& mapped,
                            VkBufferUsageFlags const extra_usage) {
        // The raw pointer overload, not the span template: the template wants a non-const span (it
        // reinterpret_casts the data to `uint8_t*`), and every caller here has const bytes.
        buffer = device.vma.create_buffer(reinterpret_cast<uint8_t const*>(initial.data()), initial.size_bytes(), type, extra_usage);
        if (!buffer.valid()) {
            deren::utility::panic(std::source_location::current(), "failed to create {}", what);
        }
        auto const* detail = device.vma.get_buffer_detail(buffer.handle());
        if (detail == nullptr) {
            deren::utility::panic(std::source_location::current(), "failed to get {} detail", what);
        }
        mapped = detail->allocation_info.pMappedData;
    }

    void create_host_buffers(core& device,
                             uint32_t const slots,
                             std::span<std::byte const> const initial,
                             buffer_type const type,
                             std::string_view const what,
                             std::vector<vk_buffer>& buffers,
                             std::vector<void*>* const mapped,
                             VkBufferUsageFlags const extra_usage) {
        buffers.reserve(buffers.size() + slots);
        if (mapped != nullptr) {
            mapped->reserve(mapped->size() + slots);
        }
        for (uint32_t slot = 0; slot < slots; ++slot) {
            vk_buffer buffer = {};
            void* mapped_pointer = nullptr;
            create_host_buffer(device, initial, type, what, buffer, mapped_pointer, extra_usage);
            buffers.push_back(std::move(buffer));
            if (mapped != nullptr) {
                mapped->push_back(mapped_pointer);
            }
        }
    }

    texture_2d create_texture_2d(core& device,
                                 std::span<std::byte const> const pixels,
                                 image_create_info const& info,
                                 std::string_view const what) {
        texture_2d texture = {};
        // the SPAN overload: it takes span<T const> and does the byte cast itself, which also means this
        // call site is the one that keeps that overload instantiated (see its comment in vma.cppm)
        texture.image = device.vma.create_image(pixels, info, image_type::texture_2d);
        if (!texture.image.valid()) {
            deren::utility::panic(std::source_location::current(), "failed to create {}", what);
        }
        auto const* detail = device.vma.get_image_detail(texture.image.handle());
        if (detail == nullptr) {
            deren::utility::panic(std::source_location::current(), "failed to get {} detail", what);
        }
        texture.view = device.make_image_view(detail->image, info.format, VK_IMAGE_VIEW_TYPE_2D);
        return texture;
    }
} // namespace deren::vulkan::init_utils
