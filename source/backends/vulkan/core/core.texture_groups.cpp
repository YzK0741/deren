// Immutable texture-group ownership and allocation. GPU publication is separate.
module;
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <spirv-reflect/spirv_reflect.h>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import :declarations;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;

    rhi::error core::initialize_texture_groups() {
        std::lock_guard lock(texture_groups.mutex);
        if (texture_groups.gpu_ready)
            return rhi::error::ok;
        if (!descriptor_heaps.ready())
            return rhi::error::unsupported;
        auto const base = VkDeviceSize(texture_group_state::table_slot) * 64;
        if (texture_groups.reserved_offset == VK_WHOLE_SIZE) {
            auto offset = descriptor_heaps.reserve_bytes(2049u * 64u, base);
            if (offset != base)
                return rhi::error::unsupported;
            texture_groups.reserved_offset = offset;
        }
        rhi::object_manager<rhi::buffer> records{create_buffer({.size = 1025u * 80u,
                                                                .usage = rhi::buffer_usage::storage_coherent,
                                                                .flags = rhi::to_bits(rhi::buffer_flag::device_address)})};
        if (!records || records->mapped().size() < 1025u * 80u)
            return rhi::error::out_of_device_memory;
        std::memset(records->mapped().data(), 0, records->mapped().size());
        std::uint32_t pixel = 0;
        rhi::image_desc desc{};
        desc.extent = {.width = 1, .height = 1, .depth = 1};
        desc.format = rhi::image_format::rgba8_unorm;
        desc.flags = rhi::to_bits(rhi::image_flag::sampled);
        desc.initial_bytes = std::as_bytes(std::span(&pixel, 1));
        rhi::object_manager<rhi::image> dummy{create_image(desc)};
        if (!dummy)
            return rhi::error::out_of_device_memory;
        rhi::object_manager<rhi::image_view> view{dummy->make_view({})};
        if (!view)
            return rhi::error::operation_failed;
        auto const& image = *static_cast<owned_image const*>(dummy.get());
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = image.native_handle;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = image.resolved_format;
        info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!descriptor_heaps.write_image(VkDeviceSize(texture_group_state::texture_base) * 64, info, VK_IMAGE_LAYOUT_GENERAL) ||
            !descriptor_heaps.write_buffer(base, address_view.buffer_address(*records, 0), records->size(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER))
            return rhi::error::operation_failed;
        texture_groups.records = std::move(records);
        texture_groups.dummy_image = std::move(dummy);
        texture_groups.dummy_view = std::move(view);
        texture_groups.gpu_ready = true;
        return rhi::error::ok;
    }

    rhi::error core::validate_texture_group_pipeline(rhi::pipeline_desc const& desc) {
        auto const& binding = desc.texture_group;
        if (rhi::validate_heap_push_range(binding.push_byte_offset, 4, heap_view.properties().max_push_data) != rhi::error::ok)
            return rhi::error::invalid_argument;
        if (binding.profile != rhi::texture_group_profile::sampled_2d_16 || !desc.ray_tracing_stages.empty())
            return rhi::error::unsupported;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_device, &properties);
        if ((binding.push_byte_offset & 3u) != 0 || binding.push_byte_offset > properties.limits.maxPushConstantsSize - 4 ||
            binding.shader_stage_bits == 0 || (binding.shader_stage_bits & ~15u))
            return rhi::error::invalid_argument;
        auto inspect = [&binding](std::span<std::byte const> code, std::uint32_t stage) {
            SpvReflectShaderModule module{};
            if (spvReflectCreateShaderModule(code.size(), code.data(), &module) != SPV_REFLECT_RESULT_SUCCESS)
                return false;
            bool token = false, valid = true, heap = false;
            for (std::uint32_t c = 0; c < module.capability_count; ++c)
                heap |= module.capabilities[c].value == SpvCapabilityDescriptorHeapEXT;
            bool const reads = (binding.shader_stage_bits & stage) != 0;
            for (std::uint32_t p = 0; p < module.push_constant_block_count; ++p) {
                auto const& block = module.push_constant_blocks[p];
                for (std::uint32_t m = 0; m < block.member_count; ++m) {
                    auto const& member = block.members[m];
                    bool overlap = member.absolute_offset < binding.push_byte_offset + 4 &&
                                   member.absolute_offset + member.size > binding.push_byte_offset;
                    if (!overlap)
                        continue;
                    bool exact = reads && member.absolute_offset == binding.push_byte_offset && member.size == 4 &&
                                 member.type_description && member.type_description->type_flags == SPV_REFLECT_TYPE_FLAG_INT &&
                                 member.numeric.scalar.width == 32 && member.numeric.scalar.signedness == 0;
                    if (exact && !token)
                        token = true;
                    else
                        valid = false;
                }
            }
            spvReflectDestroyShaderModule(&module);
            return valid && heap && (!reads || token);
        };
        if (desc.first_stage == rhi::shader_stage::compute) {
            if (binding.shader_stage_bits != 4 || !inspect(desc.compute_code, 4))
                return rhi::error::invalid_argument;
        } else {
            auto first = desc.first_stage == rhi::shader_stage::mesh ? 8u : 1u;
            if (binding.shader_stage_bits & ~(first | 2u))
                return rhi::error::invalid_argument;
            if (!inspect(desc.vertex_code, first) || !inspect(desc.fragment_code, 2))
                return rhi::error::invalid_argument;
        }
        return initialize_texture_groups();
    }

    core::texture_group_state::sampled_view_lease::~sampled_view_lease() noexcept {
        if (state != nullptr) {
            std::lock_guard lock(state->mutex);
            state->views[slot] = false;
        }
        // Destroy the native view and its parent outside the pool lock.
    }

    core::owned_texture_group::~owned_texture_group() noexcept {
        if (owner != nullptr) {
            std::lock_guard lock(owner->texture_groups.mutex);
            owner->texture_groups.live_groups.erase(this);
            if (allocation < owner->texture_groups.groups.size())
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
        auto initialized = initialize_texture_groups();
        if (initialized != rhi::error::ok)
            return fail(initialized);
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
                    fresh->view = rhi::object_manager<rhi::image_view>(images[i]->make_view({.mip_count = 0}));
                    if (!fresh->view) {
                        status = rhi::error::operation_failed;
                        break;
                    }
                    fresh->slot = static_cast<std::uint32_t>(free - texture_groups.views.begin());
                    auto const& image = *static_cast<owned_image const*>(images[i].get());
                    VkImageViewCreateInfo view_info{};
                    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                    view_info.image = image.native_handle;
                    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    view_info.format = image.resolved_format;
                    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image.mip_levels, 0, 1};
                    if (!descriptor_heaps.write_image((VkDeviceSize(texture_group_state::texture_base) + fresh->slot) * 64,
                                                      view_info, VK_IMAGE_LAYOUT_GENERAL)) {
                        status = rhi::error::operation_failed;
                        break;
                    }
                    *free = true;
                    fresh->state = &texture_groups;
                    cached = fresh;
                    lease = std::move(fresh);
                }
                answer->record.indices[i] = lease->slot;
                answer->record.present_mask |= 1u << i;
                answer->views[i] = std::move(lease);
            }
            if (status == rhi::error::ok) {
                if (answer->allocation < 1024) {
                    auto offset = (answer->allocation + 1u) * sizeof(texture_group_record);
                    std::memcpy(texture_groups.records->mapped().data() + offset, &answer->record, sizeof(answer->record));
                }
                texture_groups.live_groups.insert(answer.get());
            }
        }
        if (status != rhi::error::ok)
            return fail(status);
        if (result != nullptr)
            *result = rhi::error::ok;
        return rhi::texture_group_ref(answer.release(), [](rhi::texture_group* group) { group->release(); });
    }
    rhi::error core::frame_commands::load_texture_group(rhi::texture_group_ref const& group) {
        if (!recording || native() == VK_NULL_HANDLE)
            return rhi::error::not_ready;
        owner->poll_submissions();
        if (!bound_pipeline)
            return rhi::error::unsupported;
        {
            std::lock_guard lock(owner->contract_pipelines_mutex);
            if (!owner->contract_pipelines.contains(bound_pipeline))
                return rhi::error::invalid_argument;
        }
        if (!bound_pipeline->group_binding.enabled)
            return rhi::error::unsupported;
        std::uint32_t token = 0;
        if (group) {
            std::lock_guard lock(owner->texture_groups.mutex);
            if (!owner->texture_groups.live_groups.contains(group.get()))
                return rhi::error::invalid_argument;
            auto const& owned = *static_cast<owned_texture_group const*>(group.get());
            if (owned.allocation < 1024)
                token = owned.allocation + 1u;
        }
        if (!owner->descriptor_heaps.ready())
            return rhi::error::not_ready;
        bool retained = group && std::find(group_refs.begin(), group_refs.end(), group) == group_refs.end();
        if (retained)
            group_refs.push_back(group);
        owner->descriptor_heaps.record_bind(native());
        if (!owner->descriptor_heaps.push_data(native(), bound_pipeline->group_binding.push_byte_offset,
                                               std::as_bytes(std::span(&token, 1)))) {
            if (retained)
                group_refs.pop_back();
            return rhi::error::operation_failed;
        }
        return rhi::error::ok;
    }
} // namespace deren::vulkan
