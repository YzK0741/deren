// Immutable texture-group retention and allocation. GPU publication is separate.
module;
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <spirv-reflect/spirv_reflect.h>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.vstd;
import deren.utility.shared;
import deren.vulkan.constant_init;
import :declarations;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;

    rhi::error core::initialize_texture_groups() {
        std::lock_guard lock(resource.texture_groups.mutex);
        if (resource.texture_groups.gpu_ready) {
            return rhi::error::ok;
        }
        if (!resource.descriptor_heaps.ready()) {
            return rhi::error::unsupported;
        }
        auto const base = VkDeviceSize(texture_group_state::table_slot) * 64;
        if (resource.texture_groups.reserved_offset != base) {
            return rhi::error::unsupported;
        }
        rhi::unique_object<rhi::buffer> records{this->resource.create_buffer({.size = static_cast<std::uint64_t>(1025u) * 80u,
                                                                              .usage = rhi::buffer_usage::storage_coherent,
                                                                              .flags = rhi::to_bits(rhi::buffer_flag::device_address)})};
        if (!records || records->mapped().size() < static_cast<std::uint64_t>(1025u) * 80u) {
            return rhi::error::out_of_device_memory;
        }
        std::memset(records->mapped().data(), 0, records->mapped().size());
        std::uint32_t pixel = 0;
        rhi::image_desc desc{};
        desc.extent = {.width = 1, .height = 1, .depth = 1};
        desc.format = rhi::image_format::rgba8_unorm;
        desc.flags = rhi::to_bits(rhi::image_flag::sampled);
        desc.initial_bytes = std::as_bytes(std::span(&pixel, 1));
        rhi::unique_object<rhi::image> dummy{this->resource.create_image(desc)};
        if (!dummy) {
            return rhi::error::out_of_device_memory;
        }
        rhi::unique_object<rhi::image_view> view{dummy->make_view({})};
        if (!view) {
            return rhi::error::operation_failed;
        }
        auto const& image = *static_cast<core::image const*>(dummy.get());
        VkImageViewCreateInfo const info = make_image_view_info(image.native_handle, image.resolved_format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
        if (!resource.descriptor_heaps.write_image(VkDeviceSize(texture_group_state::texture_base) * 64, info, VK_IMAGE_LAYOUT_GENERAL) ||
            !resource.descriptor_heaps.write_buffer(base, resource.address_view.buffer_address(*records, 0), records->size(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)) {
            return rhi::error::operation_failed;
        }
        resource.texture_groups.records = std::move(records);
        resource.texture_groups.dummy_image = std::move(dummy);
        resource.texture_groups.dummy_view = std::move(view);
        resource.texture_groups.gpu_ready = true;
        return rhi::error::ok;
    }

    rhi::error core::validate_texture_group_pipeline(rhi::pipeline_desc const& desc) {
        auto const& binding = desc.texture_group;
        if (rhi::validate_push_constants_range(binding.push_byte_offset, 4, this->device.capabilities_view.max_push_constants_size()) != rhi::error::ok) {
            return rhi::error::invalid_argument;
        }
        if (binding.profile != rhi::texture_group_profile::sampled_2d_16 || !desc.ray_tracing_stages.empty()) {
            return rhi::error::unsupported;
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(this->device.physical_device, &properties);
        if ((binding.push_byte_offset & 3u) != 0 || binding.push_byte_offset > properties.limits.maxPushConstantsSize - 4 ||
            binding.shader_stage_bits == 0 || (binding.shader_stage_bits & ~15u)) {
            return rhi::error::invalid_argument;
        }
        auto inspect = [&binding](std::span<std::byte const> code, std::uint32_t stage) {
            SpvReflectShaderModule module{};
            if (spvReflectCreateShaderModule(code.size(), code.data(), &module) != SPV_REFLECT_RESULT_SUCCESS) {
                return false;
            }
            bool token = false, valid = true, heap = false;
            for (std::uint32_t c = 0; c < module.capability_count; ++c) {
                heap |= module.capabilities[c].value == SpvCapabilityDescriptorHeapEXT;
            }
            bool const reads = (binding.shader_stage_bits & stage) != 0;
            for (std::uint32_t p = 0; p < module.push_constant_block_count; ++p) {
                auto const& block = module.push_constant_blocks[p];
                for (std::uint32_t m = 0; m < block.member_count; ++m) {
                    auto const& member = block.members[m];
                    bool overlap = member.absolute_offset < binding.push_byte_offset + 4 &&
                                   member.absolute_offset + member.size > binding.push_byte_offset;
                    if (!overlap) {
                        continue;
                    }
                    bool exact = reads && member.absolute_offset == binding.push_byte_offset && member.size == 4 &&
                                 member.type_description && member.type_description->type_flags == SPV_REFLECT_TYPE_FLAG_INT &&
                                 member.numeric.scalar.width == 32 && member.numeric.scalar.signedness == 0;
                    if (exact && !token) {
                        token = true;
                    } else {
                        valid = false;
                    }
                }
            }
            spvReflectDestroyShaderModule(&module);
            return valid && heap && (!reads || token);
        };
        if (desc.first_stage == rhi::shader_stage::compute) {
            if (binding.shader_stage_bits != 4 || !inspect(desc.compute_code, 4)) {
                return rhi::error::invalid_argument;
            }
        } else {
            auto first = desc.first_stage == rhi::shader_stage::mesh ? 8u : 1u;
            if (binding.shader_stage_bits & ~(first | 2u)) {
                return rhi::error::invalid_argument;
            }
            if (!inspect(desc.vertex_code, first) || !inspect(desc.fragment_code, 2)) {
                return rhi::error::invalid_argument;
            }
        }
        return initialize_texture_groups();
    }

    rhi::error core::validate_texture_group_image(core::image const& image) const noexcept {
        if (image.parent != this || image.array_layers != 1 || image.cube_compatible ||
            (image.declared_flags & rhi::to_bits(rhi::image_flag::sampled)) == 0 ||
            image.declared_format == rhi::image_format::depth) {
            return rhi::error::invalid_argument;
        }
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(this->device.physical_device, image.resolved_format, &properties);
        return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0 ? rhi::error::ok
                                                                                             : rhi::error::unsupported;
    }

    rhi::error core::write_group_texture_locked(core::texture_group& group, std::uint32_t const index,
                                                rhi::image& texture) noexcept {
        // THE ONE WRITE PATH (registration + descriptor_space::write): the caller holds resource.texture_groups.mutex,
        // has established that `texture` is this backend's image, and has checked the group/index.
        if (group.allocation >= texture_group_state::group_capacity || !resource.texture_groups.group_live(group.allocation) ||
            index >= rhi::texture_group_capacity) {
            return rhi::error::invalid_argument;
        }
        // The lease cache is keyed by the image alone because the group profile's view is fixed (all mips,
        // one layer, sampled role) - two groups sharing an image therefore share one descriptor slot, which
        // is exactly the sharing the pre-descriptor-space code did.
        auto const& lazy = static_cast<core::image const&>(texture);
        group.lazy_references[index] = depend_on(lazy);
        if (lazy.source && lazy.state() != rhi::lazy_state::ready) {
            group.deferred_images[index] = texture.share();
            group.loading.value.store(rhi::lazy_state::unloaded, std::memory_order_release);
            return rhi::error::ok;
        }
        auto& cached = resource.texture_groups.cache[&texture];
        auto lease = cached.lock();
        if (!lease) {
            auto const free = resource.texture_groups.first_free_texture_slot();
            if (free == texture_group_state::texture_capacity) {
                return rhi::error::out_of_device_memory;
            }
            auto fresh = std::shared_ptr<texture_group_state::sampled_view_lease>(
                new (std::nothrow) texture_group_state::sampled_view_lease);
            if (!fresh) {
                return rhi::error::out_of_host_memory;
            }
            fresh->view = rhi::unique_object<rhi::image_view>(texture.make_view({.mip_count = 0}));
            if (!fresh->view) {
                return rhi::error::operation_failed;
            }
            fresh->slot = free;
            auto const& image = static_cast<core::image const&>(texture);
            VkImageViewCreateInfo const view_info = make_image_view_info(image.native_handle, image.resolved_format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, image.mip_levels, 1);
            if (!resource.descriptor_heaps.write_image((VkDeviceSize(texture_group_state::texture_base) + fresh->slot) * 64,
                                                       view_info, VK_IMAGE_LAYOUT_GENERAL)) {
                return rhi::error::operation_failed;
            }
            resource.texture_groups.set_texture_slot_live(free, true);
            fresh->state = &resource.texture_groups;
            cached = fresh;
            lease = std::move(fresh);
        }
        group.record.indices[index] = lease->slot;
        group.record.present_mask |= 1u << index;
        group.views[index] = std::move(lease);
        group.deferred_images[index] = {};
        // THE RECORD IS THE SHADER-FACING HALF of the write: the group table descriptor (slot 32768) points at
        // this buffer, and the pushed token selects the record. Publishing here keeps "one slot written" and
        // "the record that names it" from ever being observed apart.
        if (group.allocation < 1024 && resource.texture_groups.records) {
            auto const offset = (group.allocation + 1u) * sizeof(texture_group_record);
            if (offset + sizeof(texture_group_record) <= resource.texture_groups.records->mapped().size()) {
                std::memcpy(resource.texture_groups.records->mapped().data() + offset, &group.record, sizeof(group.record));
            }
        }
        return rhi::error::ok;
    }

    core::texture_group_state::sampled_view_lease::~sampled_view_lease() noexcept {
        if (state != nullptr) {
            std::lock_guard lock(state->mutex);
            state->set_texture_slot_live(slot, false);
        }
        // Destroy the native view and its parent outside the pool lock.
    }

    core::texture_group::~texture_group() noexcept {
        if (parent != nullptr) {
            std::lock_guard lock(parent->resource.texture_groups.mutex);
            if (generation != 0) {
                parent->resource.texture_groups.groups_by_token.erase(allocation + 1u);
            }
            parent->resource.texture_groups.tokens_by_group.erase(this);
            if (allocation < texture_group_state::group_capacity) {
                parent->resource.texture_groups.set_group_live(allocation, false);
            }
        }
    }

    rhi::error core::frame_commands::load_texture_group(rhi::texture_group_ref const& group) {
        ++parent->operation_requests[32]; // load_texture_group
        if (recording_error != rhi::error::ok) {
            return recording_error;
        }
        if (!recording || native() == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        parent->poll_submissions();
        if (!bound_pipeline) {
            return rhi::error::unsupported;
        }
        {
            std::lock_guard lock(parent->resource.contract_pipelines_mutex);
            if (!parent->resource.contract_pipelines.contains(bound_pipeline)) {
                return rhi::error::invalid_argument;
            }
        }
        if (!bound_pipeline->group_binding.enabled) {
            return rhi::error::unsupported;
        }
        std::uint32_t token = 0;
        if (group) {
            std::lock_guard lock(parent->resource.texture_groups.mutex);
            // THE POINTER IS LOOKED UP, NOT CAST: a foreign `texture_group` implementation is answered
            // the contract's invalid_argument instead of being static_cast into this backend's layout.
            auto const entry = parent->resource.texture_groups.tokens_by_group.find(group.get());
            if (entry == parent->resource.texture_groups.tokens_by_group.end()) {
                return rhi::error::invalid_argument;
            }
            token = entry->second;
        }
        if (!parent->resource.descriptor_heaps.ready()) {
            return rhi::error::not_ready;
        }
        operations.append<deren::promise::rhi::recording::operation_kind::texture_group>({token}, {}, {}, group);
        ++parent->deferred_requested;
        return rhi::error::ok;
    }
} // namespace deren::vulkan
