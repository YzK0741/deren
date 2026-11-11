// General immutable resource bindings. Descriptor storage and shader handles stay private here.
module;
#include <cstddef>
#include <cstdint>
#include <spirv-reflect/spirv_reflect.h>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.vstd;
import deren.utility.shared;
import :declarations;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;

    rhi::error core::validate_resource_group_pipeline(rhi::pipeline_desc const& desc) const {
        if (rhi::validate_resource_group_layout(desc.resource_group) != rhi::error::ok) {
            return rhi::error::invalid_argument;
        }
        if (desc.resource_group.empty()) {
            return rhi::error::ok;
        }
        if (desc.texture_group.enabled) {
            return rhi::error::invalid_argument;
        }
        if (!desc.ray_tracing_stages.empty()) {
            return rhi::error::unsupported;
        }
        if (!resource.descriptor_heaps.ready() || resource.heap_grid_offset == VK_WHOLE_SIZE) {
            return rhi::error::unsupported;
        }
        auto inspect = [&](std::span<std::byte const> code, std::uint32_t stage) {
            SpvReflectShaderModule module{};
            if (spvReflectCreateShaderModule(code.size(), code.data(), &module) != SPV_REFLECT_RESULT_SUCCESS) {
                return false;
            }
            bool valid = true, heap = false;
            for (std::uint32_t i = 0; i < module.capability_count; ++i) {
                heap |= module.capabilities[i].value == SpvCapabilityDescriptorHeapEXT;
            }
            for (auto const& binding : desc.resource_group) {
                bool found = false;
                for (std::uint32_t p = 0; p < module.push_constant_block_count; ++p) {
                    for (std::uint32_t m = 0; m < module.push_constant_blocks[p].member_count; ++m) {
                        auto const& member = module.push_constant_blocks[p].members[m];
                        if (member.absolute_offset >= binding.push_byte_offset + 4u ||
                            member.absolute_offset + member.size <= binding.push_byte_offset) {
                            continue;
                        }
                        bool exact = member.absolute_offset == binding.push_byte_offset && member.size == 4 &&
                                     member.type_description && member.type_description->type_flags == SPV_REFLECT_TYPE_FLAG_INT &&
                                     member.numeric.scalar.width == 32 && member.numeric.scalar.signedness == 0;
                        valid &= exact && !found;
                        found = true;
                    }
                }
                if ((binding.shader_stage_bits & stage) && !found) {
                    valid = false;
                }
            }
            spvReflectDestroyShaderModule(&module);
            bool const consumes = std::ranges::any_of(desc.resource_group,
                                                      [stage](auto const& binding) { return (binding.shader_stage_bits & stage) != 0; });
            return valid && (!consumes || heap);
        };
        auto const first = desc.first_stage == rhi::shader_stage::mesh ? 8u : 1u;
        auto const allowed = desc.first_stage == rhi::shader_stage::compute ? 4u : first | (desc.fragment_code.empty() ? 0u : 2u);
        for (auto const& binding : desc.resource_group) {
            if ((binding.shader_stage_bits & ~allowed) ||
                rhi::validate_push_constants_range(binding.push_byte_offset, 4, resource.descriptor_heaps.limits().max_push_data) != rhi::error::ok) {
                return rhi::error::invalid_argument;
            }
        }
        if (desc.first_stage == rhi::shader_stage::compute) {
            return inspect(desc.compute_code, 4u) ? rhi::error::ok : rhi::error::invalid_argument;
        }
        return inspect(desc.vertex_code, first) && (desc.fragment_code.empty() || inspect(desc.fragment_code, 2u)) ? rhi::error::ok : rhi::error::invalid_argument;
    }

    core::resource_group::~resource_group() noexcept {
        if (!parent) {
            return;
        }
        // Unlock before the members are destroyed: those destructors enter other registries.
        std::lock_guard lock(parent->resource.resource_groups_mutex);
        parent->resource.resource_groups.erase(this);
        for (auto allocation : allocations) {
            (allocation.sampler ? parent->resource.sampler_slots : parent->resource.resource_slots).push_back(allocation.offset);
        }
    }

    rhi::error core::frame_commands::load_resource_group(rhi::resource_group_ref const& group) {
        ++parent->operation_requests[31]; // load_resource_group
        if (recording_error != rhi::error::ok) {
            return recording_error;
        }
        if (!recording || native() == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (!group.is_live()) {
            return rhi::error::invalid_argument;
        }
        {
            std::lock_guard lock(parent->resource.resource_groups_mutex);
            if (!parent->resource.resource_groups.contains(group.get())) {
                return rhi::error::invalid_argument;
            }
        }
        if (!bound_pipeline) {
            return rhi::error::not_ready;
        }
        {
            std::lock_guard lock(parent->resource.contract_pipelines_mutex);
            if (!parent->resource.contract_pipelines.contains(bound_pipeline)) {
                return rhi::error::invalid_argument;
            }
        }
        auto const& concrete = *static_cast<core::resource_group const*>(group.get());
        auto const& layout = bound_pipeline->resource_layout;
        if (layout.empty()) {
            return rhi::error::unsupported;
        }
        if (layout.size() != concrete.entries.size()) {
            return rhi::error::invalid_argument;
        }
        // Check every member before any native call is made or the group is recorded.
        for (std::size_t i = 0; i < layout.size(); ++i) {
            if (layout[i].binding != concrete.entries[i].binding || layout[i].kind != concrete.entries[i].kind) {
                return rhi::error::invalid_argument;
            }
        }
        operations.append<deren::promise::rhi::recording::operation_kind::resource_group>({}, {}, group);
        ++parent->deferred_requested;
        return rhi::error::ok;
    }
} // namespace deren::vulkan
