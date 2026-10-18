// The servable face of the DESCRIPTOR SPACE (rhi::descriptor_space): positions for immutable texture groups,
// and the one portable sampled-descriptor write. The group storage itself lives in core.texture_groups.cpp -
// this file is the vocabulary layer over it, so the two semantics ("write into the space" and "where the group
// is") have ONE implementation each.
module;
#include <cstdint>
#include <mutex>

module deren.vulkan.core;
import deren.utility.shared;
import :declarations;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;

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
        if (self == nullptr)
            return {};
        // THE POSITION IS ONLY VALID FOR A GROUP THIS SPACE REGISTERED: the comparison walks the live-token
        // map (at most `group_capacity` entries, and the call is a registration/description-time operation,
        // never per draw), so a foreign `texture_group` implementation - or another core's group - is
        // answered with the empty position instead of being static_cast into this backend's layout.
        std::lock_guard lock(self->texture_groups.mutex);
        auto const token = self->texture_groups.tokens_by_group.find(&group);
        if (token == self->texture_groups.tokens_by_group.end())
            return {};
        auto const live = self->texture_groups.groups_by_token.find(token->second);
        if (live == self->texture_groups.groups_by_token.end())
            return {};
        return {
            .space = 0u,
            .base = token->second,
            .count = rhi::texture_group_capacity,
            .generation = live->second->generation,
        };
    }

    rhi::error core::frame_descriptor_space::write(rhi::descriptor_write const& info) noexcept {
        core* const self = this->owner;
        if (self == nullptr)
            return rhi::error::not_ready;
        // THE TAGGED WRITE STRUCTURE, exactly the rule the heap's own requests follow: the tag and the size
        // the caller compiled are checked before any field is read, and a chain is refused rather than
        // silently ignored.
        if (rhi::validate_structure(info.header, rhi::structure_type::descriptor_write, sizeof(info), false) != rhi::error::ok)
            return rhi::error::invalid_argument;
        // A POSITION IS VALID ONLY WHEN IT NAMES THIS SPACE, THE GROUP PROFILE'S WIDTH, AND A GENERATION that
        // registration could have produced (0 is the empty position, never a registered one).
        if (info.where.space != 0u || info.where.count != rhi::texture_group_capacity || info.where.base == 0u ||
            info.where.generation == 0u)
            return rhi::error::invalid_argument;
        if (info.index >= info.where.count)
            return rhi::error::invalid_argument;
        if (info.type != rhi::descriptor_type::sampled_image && info.type != rhi::descriptor_type::combined_image_sampler)
            return rhi::error::unsupported;
        if (info.resource == nullptr)
            return rhi::error::invalid_argument;
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
            if (!self->contract_images.contains(info.resource))
                return rhi::error::invalid_argument;
            auto const valid = self->validate_texture_group_image(static_cast<owned_image const&>(*info.resource));
            if (valid != rhi::error::ok)
                return valid;
        }
        std::lock_guard lock(self->texture_groups.mutex);
        auto const entry = self->texture_groups.groups_by_token.find(info.where.base);
        if (entry == self->texture_groups.groups_by_token.end() || entry->second->generation != info.where.generation)
            return rhi::error::invalid_argument; // a stale position: the token was reused (or never registered)
        return self->write_group_texture_locked(*entry->second, info.index, *info.resource);
    }
} // namespace deren::vulkan
