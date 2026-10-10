// Immutable texture-group ownership and allocation. GPU publication is separate.
module;
#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <new>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import :declarations;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;

    core::texture_group_state::sampled_view_lease::~sampled_view_lease() noexcept {
        if (state != nullptr) {
            std::lock_guard lock(state->mutex);
            state->views[slot] = false;
        }
        // Destroy the native view and its parent outside the pool lock.
    }

    core::owned_texture_group::~owned_texture_group() noexcept {
        if (owner != nullptr && allocation < owner->texture_groups.groups.size()) {
            std::lock_guard lock(owner->texture_groups.mutex);
            owner->texture_groups.groups[allocation] = false;
        }
    }

    rhi::texture_group_ref core::assign_texture_group(rhi::texture_group_info const& info, rhi::error* result) {
        poll_submissions();
        auto fail = [result](rhi::error value) -> rhi::texture_group_ref {
            if (result != nullptr)
                *result = value;
            return {};
        };
        if (result != nullptr)
            *result = rhi::error::invalid_argument;
        if (info.struct_size < sizeof(info))
            return {};
        if (logical_device == VK_NULL_HANDLE)
            return fail(rhi::error::not_ready);

        std::array<std::shared_ptr<rhi::image>, 16> images{};
        {
            std::lock_guard lock(contract_images_mutex);
            // Validate every input before acquiring or allocating any group resources.
            for (auto const& input : info.textures) {
                auto* image = input.value_or(nullptr);
                if (image == nullptr)
                    continue;
                if (!contract_images.contains(image))
                    return {};
                auto const& owned = *static_cast<owned_image const*>(image);
                if (owned.owner != this || owned.array_layers != 1 || owned.cube_compatible ||
                    (owned.declared_flags & rhi::to_bits(rhi::image_flag::sampled)) == 0 ||
                    owned.declared_format == rhi::image_format::depth)
                    return {};
                VkFormatProperties properties{};
                vkGetPhysicalDeviceFormatProperties(physical_device, owned.resolved_format, &properties);
                if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0)
                    return fail(rhi::error::unsupported);
            }
            for (std::size_t i = 0; i < images.size(); ++i) {
                if (auto* image = info.textures[i].value_or(nullptr))
                    images[i] = image->share();
            }
        }
        auto answer = std::unique_ptr<owned_texture_group>(new (std::nothrow) owned_texture_group);
        if (!answer)
            return fail(rhi::error::out_of_host_memory);
        answer->owner = this;
        rhi::error status = rhi::error::ok;
        {
            std::lock_guard lock(texture_groups.mutex);
            std::erase_if(texture_groups.cache, [](auto const& item) { return item.second.expired(); });
            if (std::any_of(images.begin(), images.end(), [](auto const& image) { return bool(image); })) {
                auto free = std::find(texture_groups.groups.begin(), texture_groups.groups.end(), false);
                if (free == texture_groups.groups.end())
                    status = rhi::error::out_of_device_memory;
                else {
                    answer->allocation = static_cast<std::uint32_t>(free - texture_groups.groups.begin());
                    *free = true;
                }
            }
            for (std::size_t i = 0; status == rhi::error::ok && i < images.size(); ++i) {
                if (!images[i])
                    continue;
                auto& cached = texture_groups.cache[images[i].get()];
                auto lease = cached.lock();
                if (!lease) {
                    auto free = std::find(texture_groups.views.begin() + 1, texture_groups.views.end(), false);
                    if (free == texture_groups.views.end()) {
                        status = rhi::error::out_of_device_memory;
                        break;
                    }
                    // Construct before marking the lease as allocated; errors unwind outside this lock.
                    auto fresh = std::shared_ptr<texture_group_state::sampled_view_lease>(
                        new (std::nothrow) texture_group_state::sampled_view_lease);
                    if (!fresh) {
                        status = rhi::error::out_of_host_memory;
                        break;
                    }
                    fresh->view = rhi::object_manager<rhi::image_view>(images[i]->make_view({}));
                    if (!fresh->view) {
                        status = rhi::error::operation_failed;
                        break;
                    }
                    fresh->slot = static_cast<std::uint32_t>(free - texture_groups.views.begin());
                    *free = true;
                    fresh->state = &texture_groups;
                    cached = fresh;
                    lease = std::move(fresh);
                }
                answer->record.indices[i] = lease->slot;
                answer->record.present_mask |= 1u << i;
                answer->views[i] = std::move(lease);
            }
        }
        if (status != rhi::error::ok)
            return fail(status);
        if (result != nullptr)
            *result = rhi::error::ok;
        return rhi::texture_group_ref(answer.release(), [](rhi::texture_group* group) { group->release(); });
    }
} // namespace deren::vulkan
