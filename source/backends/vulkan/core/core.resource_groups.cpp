// General immutable resource bindings. Descriptor storage and shader handles stay private here.
module;
#include <algorithm>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <spirv-reflect/spirv_reflect.h>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.utility.shared;
import :declarations;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;

    rhi::error core::validate_resource_group_pipeline(rhi::pipeline_desc const& desc) {
        if (rhi::validate_resource_group_layout(desc.resource_group) != rhi::error::ok)
            return rhi::error::invalid_argument;
        if (desc.resource_group.empty())
            return rhi::error::ok;
        if (desc.texture_group.enabled)
            return rhi::error::invalid_argument;
        if (!desc.ray_tracing_stages.empty())
            return rhi::error::unsupported;
        if (!descriptor_heaps.ready() || heap_grid_offset == VK_WHOLE_SIZE)
            return rhi::error::unsupported;
        auto inspect = [&](std::span<std::byte const> code, std::uint32_t stage) {
            SpvReflectShaderModule module{};
            if (spvReflectCreateShaderModule(code.size(), code.data(), &module) != SPV_REFLECT_RESULT_SUCCESS)
                return false;
            bool valid = true, heap = false;
            for (std::uint32_t i = 0; i < module.capability_count; ++i)
                heap |= module.capabilities[i].value == SpvCapabilityDescriptorHeapEXT;
            for (auto const& binding : desc.resource_group) {
                bool found = false;
                for (std::uint32_t p = 0; p < module.push_constant_block_count; ++p)
                    for (std::uint32_t m = 0; m < module.push_constant_blocks[p].member_count; ++m) {
                        auto const& member = module.push_constant_blocks[p].members[m];
                        if (member.absolute_offset >= binding.push_byte_offset + 4u ||
                            member.absolute_offset + member.size <= binding.push_byte_offset)
                            continue;
                        bool exact = member.absolute_offset == binding.push_byte_offset && member.size == 4 &&
                                     member.type_description && member.type_description->type_flags == SPV_REFLECT_TYPE_FLAG_INT &&
                                     member.numeric.scalar.width == 32 && member.numeric.scalar.signedness == 0;
                        valid &= exact && !found;
                        found = true;
                    }
                if ((binding.shader_stage_bits & stage) && !found)
                    valid = false;
            }
            spvReflectDestroyShaderModule(&module);
            bool consumes = std::any_of(desc.resource_group.begin(), desc.resource_group.end(),
                                        [stage](auto const& binding) { return (binding.shader_stage_bits & stage) != 0; });
            return valid && (!consumes || heap);
        };
        auto first = desc.first_stage == rhi::shader_stage::mesh ? 8u : 1u;
        auto allowed = desc.first_stage == rhi::shader_stage::compute ? 4u : first | (desc.fragment_code.empty() ? 0u : 2u);
        for (auto const& binding : desc.resource_group)
            if ((binding.shader_stage_bits & ~allowed) ||
                rhi::validate_push_constants_range(binding.push_byte_offset, 4, descriptor_heaps.limits().max_push_data) != rhi::error::ok)
                return rhi::error::invalid_argument;
        if (desc.first_stage == rhi::shader_stage::compute)
            return inspect(desc.compute_code, 4u) ? rhi::error::ok : rhi::error::invalid_argument;
        return inspect(desc.vertex_code, first) && (desc.fragment_code.empty() || inspect(desc.fragment_code, 2u)) ? rhi::error::ok : rhi::error::invalid_argument;
    }

    core::owned_resource_group::~owned_resource_group() noexcept {
        if (!owner)
            return;
        // Unlock before member-owner destruction: those destructors enter other registries.
        std::lock_guard lock(owner->resource_groups_mutex);
        owner->resource_groups.erase(this);
        for (auto allocation : allocations)
            (allocation.sampler ? owner->sampler_slots : owner->resource_slots).push_back(allocation.offset);
    }

    std::expected<rhi::resource_group_ref, rhi::error> core::create_resource_group(rhi::resource_group_desc const& desc) {
        auto fail = [](rhi::error error) -> std::expected<rhi::resource_group_ref, rhi::error> { return std::unexpected(error); };
        if (desc.struct_size < sizeof(desc) || desc.entries.empty() || desc.entries.size() > rhi::resource_group_capacity)
            return fail(rhi::error::invalid_argument);
        if (!descriptor_heaps.ready() || heap_grid_offset == VK_WHOLE_SIZE)
            return fail(rhi::error::unsupported);
        auto group = std::unique_ptr<owned_resource_group>(new (std::nothrow) owned_resource_group);
        if (!group)
            return fail(rhi::error::out_of_host_memory);
        group->owner = this;
        group->entries.assign(desc.entries.begin(), desc.entries.end());
        std::sort(group->entries.begin(), group->entries.end(), [](auto const& a, auto const& b) { return a.binding < b.binding; });
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_device, &properties);
        for (auto const& entry : group->entries) {
            if (!entry.resource.owns_lifetime())
                return fail(rhi::error::invalid_argument);
            if (entry.kind == rhi::resource_kind::uniform_buffer || entry.kind == rhi::resource_kind::storage_buffer) {
                std::unique_lock lock(contract_buffers_mutex);
                auto found = std::find_if(contract_buffers.begin(), contract_buffers.end(), [&](auto* p) { return static_cast<rhi::object const*>(p) == entry.resource.get(); });
                if (found == contract_buffers.end())
                    return fail(rhi::error::invalid_argument);
                auto const& value = *static_cast<owned_buffer const*>(*found);
                if (value.source) {
                    lock.unlock();
                    group->lazy_references.push_back(depend_on(value));
                    group->deferred |= value.state() != rhi::lazy_state::ready;
                }
            } else if (entry.kind == rhi::resource_kind::sampled_image || entry.kind == rhi::resource_kind::storage_image) {
                std::unique_lock lock(contract_images_mutex);
                auto found = std::find_if(contract_images.begin(), contract_images.end(), [&](auto* p) { return static_cast<rhi::object const*>(p) == entry.resource.get(); });
                if (found == contract_images.end())
                    return fail(rhi::error::invalid_argument);
                auto const& value = *static_cast<owned_image const*>(*found);
                if (value.source) {
                    lock.unlock();
                    group->lazy_references.push_back(depend_on(value));
                    group->deferred |= value.state() != rhi::lazy_state::ready;
                }
            } else if (entry.kind == rhi::resource_kind::texture_group) {
                std::lock_guard lock(texture_groups.mutex);
                if (!texture_groups.tokens_by_group.contains(static_cast<rhi::texture_group const*>(entry.resource.get())))
                    return fail(rhi::error::invalid_argument);
                auto const& value = *static_cast<owned_texture_group const*>(entry.resource.get());
                group->deferred |= std::any_of(value.deferred_images.begin(), value.deferred_images.end(), [](auto const& p) { return bool(p); });
            }
        }
        for (std::size_t index = 0; index < group->entries.size(); ++index) {
            auto const& entry = group->entries[index];
            if (!entry.resource.owns_lifetime() || entry.kind > rhi::resource_kind::acceleration_structure ||
                (index && entry.binding == group->entries[index - 1].binding))
                return fail(rhi::error::invalid_argument);
            auto const* resource = entry.resource.get();
            if (entry.kind == rhi::resource_kind::texture_group) {
                std::lock_guard lock(texture_groups.mutex);
                auto found = std::find_if(texture_groups.tokens_by_group.begin(), texture_groups.tokens_by_group.end(),
                                          [resource](auto const& item) { return static_cast<rhi::object const*>(item.first) == resource; });
                if (found == texture_groups.tokens_by_group.end())
                    return fail(rhi::error::invalid_argument);
                group->values.push_back(found->second);
                continue;
            }
            VkDeviceSize address = 0, size = 0;
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
            switch (entry.kind) {
            case rhi::resource_kind::uniform_buffer:
            case rhi::resource_kind::storage_buffer: {
                std::lock_guard lock(contract_buffers_mutex);
                auto found = std::find_if(contract_buffers.begin(), contract_buffers.end(),
                                          [resource](auto* item) { return static_cast<rhi::object const*>(item) == resource; });
                if (found == contract_buffers.end())
                    return fail(rhi::error::invalid_argument);
                auto* buffer = *found;
                auto const& owned = *static_cast<owned_buffer const*>(buffer);
                bool uniform = entry.kind == rhi::resource_kind::uniform_buffer;
                bool usage = uniform ? (owned.declared_usage == rhi::buffer_usage::uniform_gpu_only ||
                                        owned.declared_usage == rhi::buffer_usage::uniform_coherent || owned.declared_usage == rhi::buffer_usage::uniform_cached)
                                     : (owned.declared_usage == rhi::buffer_usage::storage_gpu_only || owned.declared_usage == rhi::buffer_usage::storage_coherent);
                if (!usage || !owned.addressable)
                    return fail(rhi::error::unsupported);
                if (entry.offset >= owned.size_bytes)
                    return fail(rhi::error::invalid_argument);
                size = entry.size ? entry.size : owned.size_bytes - entry.offset;
                auto alignment = uniform ? properties.limits.minUniformBufferOffsetAlignment : properties.limits.minStorageBufferOffsetAlignment;
                auto limit = uniform ? properties.limits.maxUniformBufferRange : properties.limits.maxStorageBufferRange;
                if (size > owned.size_bytes - entry.offset || size > limit || (alignment && entry.offset % alignment))
                    return fail(rhi::error::invalid_argument);
                address = internal_address(owned, entry.offset);
                // Uniforms lower to native storage descriptors; satisfy both sets of constraints.
                if ((!group->deferred && !address) || (address && address % properties.limits.minStorageBufferOffsetAlignment) ||
                    size > properties.limits.maxStorageBufferRange || (address && size > UINT64_MAX - address))
                    return fail(rhi::error::invalid_argument);
                // Heap ConstantBuffer is an OpBufferPointerEXT StorageBuffer in the current shader ABI.
                type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                break;
            }
            case rhi::resource_kind::sampled_image:
            case rhi::resource_kind::storage_image: {
                std::lock_guard lock(contract_images_mutex);
                if (std::none_of(contract_images.begin(), contract_images.end(),
                                 [resource](auto* item) { return static_cast<rhi::object const*>(item) == resource; }))
                    return fail(rhi::error::invalid_argument);
                auto const& image = *static_cast<owned_image const*>(resource);
                auto const& view = entry.view;
                bool sampled = entry.kind == rhi::resource_kind::sampled_image;
                if (view.struct_size < sizeof(view) || view.base_mip >= image.mip_levels || view.base_layer >= image.array_layers ||
                    (view.mip_count && view.mip_count > image.mip_levels - view.base_mip) ||
                    (view.layer_count && view.layer_count > image.array_layers - view.base_layer))
                    return fail(rhi::error::invalid_argument);
                if (view.role != (sampled ? rhi::view_role::sampled : rhi::view_role::storage) ||
                    !rhi::has_flag(image.declared_flags, sampled ? rhi::image_flag::sampled : rhi::image_flag::storage))
                    return fail(rhi::error::unsupported);
                break;
            }
            case rhi::resource_kind::sampler:
            case rhi::resource_kind::acceleration_structure: {
                std::lock_guard lock(group_members_mutex);
                if (!group_members.contains(resource))
                    return fail(rhi::error::invalid_argument);
                if (entry.kind == rhi::resource_kind::sampler) {
                    if (!rhi::is_concrete(*resource, concrete::sampler))
                        return fail(rhi::error::invalid_argument);
                } else {
                    if (!rhi::is_concrete(*resource, concrete::acceleration_structure))
                        return fail(rhi::error::invalid_argument);
                    auto const& owned = *static_cast<owned_acceleration_structure const*>(resource);
                    if (!owned.top_level || !owned.address || !owned.structure_size)
                        return fail(rhi::error::invalid_argument);
                    address = owned.address;
                    size = owned.structure_size;
                    type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                }
                break;
            }
            default:
                return fail(rhi::error::invalid_argument);
            }
            if (group->deferred) {
                group->values.push_back(0);
                continue;
            }
            bool sampler = entry.kind == rhi::resource_kind::sampler;
            VkDeviceSize offset;
            {
                std::lock_guard lock(resource_groups_mutex);
                auto& free = sampler ? sampler_slots : resource_slots;
                if (!free.empty()) {
                    offset = free.back();
                    free.pop_back();
                } else if (sampler) {
                    if (!next_sampler)
                        next_sampler = heap_sampler_base + static_cast<std::uint32_t>(shared_sampler_infos.size());
                    offset = VkDeviceSize(next_sampler) * 32u;
                    if (offset + 32u > descriptor_heaps.sampler_size())
                        return fail(rhi::error::out_of_device_memory);
                    ++next_sampler;
                } else {
                    offset = descriptor_heaps.reserve_bytes(64u, 64u);
                    if (offset == VK_WHOLE_SIZE)
                        return fail(rhi::error::out_of_device_memory);
                }
            }
            group->allocations.push_back({offset, sampler});
            if (sampler) {
                auto const& info = static_cast<owned_sampler const*>(resource)->info;
                if (!descriptor_heaps.write_samplers(offset, std::span(&info, 1)))
                    return fail(rhi::error::operation_failed);
            } else if (entry.kind == rhi::resource_kind::sampled_image || entry.kind == rhi::resource_kind::storage_image) {
                auto type = entry.kind == rhi::resource_kind::sampled_image ? rhi::descriptor_type::sampled_image : rhi::descriptor_type::storage_image;
                rhi::heap_image_write_info info{.offset = offset, .resource = static_cast<rhi::image const*>(resource), .view = &entry.view, .type = type};
                auto status = heap_view.write_image_native(info, true);
                if (status != rhi::error::ok)
                    return fail(status);
            } else if (!address || !descriptor_heaps.write_buffer(offset, address, size, type))
                return fail(rhi::error::operation_failed);
            group->values.push_back(static_cast<std::uint32_t>(offset / (sampler ? 32u : 64u)));
        }
        {
            std::lock_guard lock(resource_groups_mutex);
            resource_groups.insert(group.get());
        }
        return rhi::resource_group_ref(group.release());
    }

    rhi::error core::frame_commands::load_resource_group(rhi::resource_group_ref const& group) {
        ++owner->operation_requests[31]; // load_resource_group
        if (recording_error != rhi::error::ok)
            return recording_error;
        if (!recording || native() == VK_NULL_HANDLE)
            return rhi::error::not_ready;
        if (!group.owns_lifetime())
            return rhi::error::invalid_argument;
        {
            std::lock_guard lock(owner->resource_groups_mutex);
            if (!owner->resource_groups.contains(group.get()))
                return rhi::error::invalid_argument;
        }
        if (!bound_pipeline)
            return rhi::error::not_ready;
        {
            std::lock_guard lock(owner->contract_pipelines_mutex);
            if (!owner->contract_pipelines.contains(bound_pipeline))
                return rhi::error::invalid_argument;
        }
        auto const& owned = *static_cast<owned_resource_group const*>(group.get());
        auto const& layout = bound_pipeline->resource_layout;
        if (layout.empty())
            return rhi::error::unsupported;
        if (layout.size() != owned.entries.size())
            return rhi::error::invalid_argument;
        // Check every member before any native call or recorded reference is published.
        for (std::size_t i = 0; i < layout.size(); ++i)
            if (layout[i].binding != owned.entries[i].binding || layout[i].kind != owned.entries[i].kind)
                return rhi::error::invalid_argument;
        operations.append<deren::promise::rhi::recording::operation_kind::resource_group>({}, {}, group);
        ++owner->deferred_requested;
        return rhi::error::ok;
    }
} // namespace deren::vulkan
