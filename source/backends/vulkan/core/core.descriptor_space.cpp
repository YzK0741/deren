// The servable face of the DESCRIPTOR SPACE (rhi::descriptor_space): positions for immutable texture groups,
// and the one portable sampled-descriptor write. The group storage itself lives in core.texture_groups.cpp -
// this file is the vocabulary layer over it, so the two semantics ("write into the space" and "where the group
// is") have ONE implementation each.
module;
#include <mutex>
#include <vulkan/vulkan.h>

module deren.vulkan.core;
import deren.utility.shared;
import :declarations;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;

    rhi::descriptor_slot_range core::frame_descriptor_space::fixed_slots() const noexcept {
        if (!ready() || owner->heap_grid_offset == VK_WHOLE_SIZE)
            return {};
        return {heap_slot_base, heap_slot_count};
    }
    namespace {
        bool fixed_slot(rhi::descriptor_slot_range range, std::uint32_t space, std::uint32_t slot) {
            return space == 0 && slot >= range.base && slot - range.base < range.count;
        }
    } // namespace
    rhi::error core::frame_descriptor_space::write_image(rhi::descriptor_image_write const& info) noexcept {
        auto checked = rhi::validate_structure(info.header, rhi::structure_type::descriptor_image_write, sizeof(info), false);
        if (checked != rhi::error::ok)
            return checked;
        if (!fixed_slots().count)
            return rhi::error::unsupported;
        if (!fixed_slot(fixed_slots(), info.space, info.slot))
            return rhi::error::invalid_argument;
        if (!info.resource || !info.view || info.view->struct_size < sizeof(rhi::image_view_desc))
            return rhi::error::invalid_argument;
        {
            std::lock_guard lock(owner->contract_images_mutex);
            if (!owner->contract_images.contains(info.resource))
                return rhi::error::invalid_argument;
        }
        auto& value = const_cast<owned_image&>(static_cast<owned_image const&>(*info.resource));
        auto const& view = *info.view;
        if (view.base_mip >= value.mip_levels || view.base_layer >= value.array_layers ||
            (view.mip_count && view.mip_count > value.mip_levels - view.base_mip) ||
            (view.layer_count && view.layer_count > value.array_layers - view.base_layer))
            return rhi::error::invalid_argument;
        bool sampled = info.type == rhi::descriptor_type::sampled_image;
        if (!sampled && info.type != rhi::descriptor_type::storage_image)
            return rhi::error::unsupported;
        if (view.role != (sampled ? rhi::view_role::sampled : rhi::view_role::storage) ||
            !rhi::has_flag(value.declared_flags, sampled ? rhi::image_flag::sampled : rhi::image_flag::storage))
            return rhi::error::unsupported;
        if (value.source) {
            lazy_descriptor saved;
            saved.image = true;
            saved.image_write = info;
            saved.image_write.view = nullptr;
            saved.view = view;
            saved.reference = owner->depend_on(value);
            std::lock_guard lock(owner->lazy_descriptors_mutex);
            owner->lazy_descriptors[info.slot] = std::move(saved);
            return rhi::error::ok;
        }
        auto status = owner->heap_view.write_image({.offset = static_cast<std::uint64_t>(info.slot) * heap_slot_stride,
                                                    .resource = info.resource,
                                                    .view = info.view,
                                                    .type = info.type});
        if (status == rhi::error::ok) {
            std::lock_guard lock(owner->lazy_descriptors_mutex);
            owner->lazy_descriptors.erase(info.slot);
        }
        return status;
    }
    rhi::error core::frame_descriptor_space::write_buffer(rhi::descriptor_buffer_write const& info) noexcept {
        auto checked = rhi::validate_structure(info.header, rhi::structure_type::descriptor_buffer_write, sizeof(info), false);
        if (checked != rhi::error::ok)
            return checked;
        if (!fixed_slots().count)
            return rhi::error::unsupported;
        if (!fixed_slot(fixed_slots(), info.space, info.slot) || !info.resource || !info.size)
            return rhi::error::invalid_argument;
        bool const uniform = info.type == rhi::descriptor_type::uniform_buffer;
        if (!uniform && info.type != rhi::descriptor_type::storage_buffer)
            return rhi::error::unsupported;
        std::unique_lock lock(owner->contract_buffers_mutex);
        if (!owner->contract_buffers.contains(info.resource))
            return rhi::error::invalid_argument;
        auto const& buffer = *static_cast<owned_buffer const*>(info.resource);
        bool const uniform_usage = buffer.declared_usage == rhi::buffer_usage::uniform_gpu_only ||
                                   buffer.declared_usage == rhi::buffer_usage::uniform_coherent || buffer.declared_usage == rhi::buffer_usage::uniform_cached;
        bool const storage_usage = uniform_usage || buffer.declared_usage == rhi::buffer_usage::storage_gpu_only ||
                                   buffer.declared_usage == rhi::buffer_usage::storage_coherent || rhi::has_flag(buffer.declared_flags, rhi::buffer_flag::storage);
        if (!buffer.addressable || (uniform ? !uniform_usage : !storage_usage))
            return rhi::error::unsupported;
        if (info.offset >= buffer.size_bytes || info.size > buffer.size_bytes - info.offset)
            return rhi::error::invalid_argument;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(owner->physical_device, &properties);
        auto alignment = uniform ? properties.limits.minUniformBufferOffsetAlignment : properties.limits.minStorageBufferOffsetAlignment;
        auto limit = uniform ? properties.limits.maxUniformBufferRange : properties.limits.maxStorageBufferRange;
        if ((alignment && info.offset % alignment) || info.size > limit)
            return rhi::error::invalid_argument;
        // This shader profile lowers uniform pointers through StorageBuffer too.
        if (buffer.source) {
            lazy_descriptor saved;
            saved.buffer_write = info;
            lock.unlock();
            saved.reference = owner->depend_on(buffer);
            std::lock_guard descriptors_lock(owner->lazy_descriptors_mutex);
            owner->lazy_descriptors[info.slot] = std::move(saved);
            return rhi::error::ok;
        }
        auto address = owner->internal_address(buffer, info.offset);
        if (!address || (properties.limits.minStorageBufferOffsetAlignment && address % properties.limits.minStorageBufferOffsetAlignment) ||
            info.size > properties.limits.maxStorageBufferRange || info.size > UINT64_MAX - address)
            return rhi::error::invalid_argument;
        auto status = owner->heap_view.write_buffer({.offset = static_cast<std::uint64_t>(info.slot) * heap_slot_stride,
                                                     .address = address,
                                                     .size = info.size,
                                                     .type = rhi::descriptor_type::storage_buffer});
        lock.unlock();
        if (status == rhi::error::ok) {
            std::lock_guard descriptors_lock(owner->lazy_descriptors_mutex);
            owner->lazy_descriptors.erase(info.slot);
        }
        return status;
    }
    rhi::error core::frame_descriptor_space::write_acceleration_structure(rhi::descriptor_acceleration_structure_write const& info) noexcept {
        auto checked = rhi::validate_structure(info.header, rhi::structure_type::descriptor_acceleration_structure_write, sizeof(info), false);
        if (checked != rhi::error::ok)
            return checked;
        if (!fixed_slots().count)
            return rhi::error::unsupported;
        if (!fixed_slot(fixed_slots(), info.space, info.slot) || !info.resource)
            return rhi::error::invalid_argument;
        std::lock_guard lock(owner->contract_structures_mutex);
        if (!owner->contract_structures.contains(info.resource))
            return rhi::error::invalid_argument;
        auto const& structure = *static_cast<owned_acceleration_structure const*>(info.resource);
        if (structure.owner != owner || !structure.native)
            return rhi::error::invalid_argument;
        auto status = owner->heap_view.write_buffer({.offset = static_cast<std::uint64_t>(info.slot) * heap_slot_stride,
                                                     .address = structure.device_address(),
                                                     .size = structure.size_bytes(),
                                                     .type = rhi::descriptor_type::acceleration_structure});
        if (status == rhi::error::ok) {
            std::lock_guard descriptors_lock(owner->lazy_descriptors_mutex);
            owner->lazy_descriptors.erase(info.slot);
        }
        return status;
    }
    rhi::error core::frame_descriptor_space::bind(rhi::descriptor_bind_info const& info) noexcept {
        auto checked = rhi::validate_structure(info.header, rhi::structure_type::descriptor_bind, sizeof(info), false);
        if (checked != rhi::error::ok)
            return checked;
        if (!ready())
            return rhi::error::not_ready;
        return owner->heap_view.bind({.commands = info.commands});
    }

    bool core::frame_descriptor_space::ready() const noexcept {
        // THE SAME CONDITION THE ABILITY BIT IS ANNOUNCED UNDER (core::abilities): the space is the heap's
        // slot pool + the group table, so "no heap" is the one state in which nothing can be served.
        return this->owner != nullptr && this->owner->descriptor_heaps.ready();
    }

    rhi::descriptor_space_properties core::frame_descriptor_space::properties() const noexcept {
        // The answers are the BACKEND's capacities, not a frozen contract constant: a second backend may
        // size its table differently, and a caller that wants to know asks rather than assumes.
        return {
            .group_capacity = texture_group_state::group_capacity,
            .texture_capacity = texture_group_state::texture_capacity,
            .group_record_bytes = texture_group_state::group_record_bytes,
            .texture_slot_bytes = texture_group_state::texture_slot_bytes,
        };
    }

    rhi::descriptor_position core::frame_descriptor_space::position(rhi::texture_group const& group) const noexcept {
        core* const self = this->owner;
        if (self == nullptr) {
            return {};
        }
        // THE POSITION IS ONLY VALID FOR A GROUP THIS SPACE REGISTERED: the comparison walks the live-token
        // map (at most `group_capacity` entries, and the call is a registration/description-time operation,
        // never per draw), so a foreign `texture_group` implementation - or another core's group - is
        // answered with the empty position instead of being static_cast into this backend's layout.
        std::lock_guard lock(self->texture_groups.mutex);
        auto const token = self->texture_groups.tokens_by_group.find(&group);
        if (token == self->texture_groups.tokens_by_group.end()) {
            return {};
        }
        auto const live = self->texture_groups.groups_by_token.find(token->second);
        if (live == self->texture_groups.groups_by_token.end()) {
            return {};
        }
        return {
            .space = 0u,
            .base = token->second,
            .count = rhi::texture_group_capacity,
            .generation = live->second->generation,
        };
    }

    rhi::error core::frame_descriptor_space::write(rhi::descriptor_write const& info) noexcept {
        core* const self = this->owner;
        if (self == nullptr) {
            return rhi::error::not_ready;
        }
        // THE TAGGED WRITE STRUCTURE, exactly the rule the heap's own requests follow: the tag and the size
        // the caller compiled are checked before any field is read, and a chain is refused rather than
        // silently ignored.
        if (rhi::validate_structure(info.header, rhi::structure_type::descriptor_write, sizeof(info), false) != rhi::error::ok) {
            return rhi::error::invalid_argument;
        }
        // A POSITION IS VALID ONLY WHEN IT NAMES THIS SPACE, THE GROUP PROFILE'S WIDTH, AND A GENERATION that
        // registration could have produced (0 is the empty position, never a registered one).
        if (info.where.space != 0u || info.where.count != rhi::texture_group_capacity || info.where.base == 0u ||
            info.where.generation == 0u) {
            return rhi::error::invalid_argument;
        }
        if (info.index >= info.where.count) {
            return rhi::error::invalid_argument;
        }
        if (info.type != rhi::descriptor_type::sampled_image && info.type != rhi::descriptor_type::combined_image_sampler) {
            return rhi::error::unsupported;
        }
        if (info.resource == nullptr) {
            return rhi::error::invalid_argument;
        }
        if (info.view != nullptr) {
            // THE GROUP PROFILE'S VIEW IS FIXED (all mips, one array layer, sampled role), so an explicit
            // view that asks for anything else is refused BY NAME: accepting it would alias a single lease
            // slot with a different descriptor shape (the cache is keyed by image, not by view).
            auto const& view = *info.view;
            if (view.struct_size < sizeof(rhi::image_view_desc) || view.role != rhi::view_role::sampled ||
                view.base_mip != 0u || view.mip_count != 0u || view.base_layer != 0u || view.layer_count > 1u) {
                return rhi::error::unsupported;
            }
        }
        {
            // Provenance first, the same order `assign_texture_group` uses: the image must be one THIS
            // backend handed out, and it must satisfy the immutable-group source rules.
            std::lock_guard lock(self->contract_images_mutex);
            if (!self->contract_images.contains(info.resource)) {
                return rhi::error::invalid_argument;
            }
            auto const valid = self->validate_texture_group_image(static_cast<owned_image const&>(*info.resource));
            if (valid != rhi::error::ok) {
                return valid;
            }
        }
        std::lock_guard lock(self->texture_groups.mutex);
        auto const entry = self->texture_groups.groups_by_token.find(info.where.base);
        if (entry == self->texture_groups.groups_by_token.end() || entry->second->generation != info.where.generation) {
            return rhi::error::invalid_argument; // a stale position: the token was reused (or never registered)
        }
        return self->write_group_texture_locked(*entry->second, info.index, *info.resource);
    }
} // namespace deren::vulkan
