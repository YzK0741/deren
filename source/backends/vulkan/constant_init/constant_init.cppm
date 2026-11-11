// ============================================================================
// module: deren.vulkan.constant_init
// module version: 0.13.0  (independent of the app version in CMakeLists project(VERSION))
//
// Compile-time Vulkan info-struct conventions: constexpr factories + constinit
// "transition" defaults for the structs the engine fills identically everywhere
// (object create infos, command-buffer / secondary inheritance, fixed-function
// pipeline state, per-frame layout transitions). Top-level module: depends on
// nothing but the Vulkan headers, so any Vulkan module can embed it.
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

#include <cstddef>
#include <cstdint>
#include <vulkan/vulkan.h>

export module deren.vulkan.constant_init;

/**
 * @defgroup vulkan_constant_init Vulkan Info-Struct Builders (fixed conventions)
 * @file constant_init.cppm
 *
 * @brief constexpr constructors and constinit defaults for the Vulkan info structs the engine
 *        fills the same way everywhere.
 *
 * Top-level module (sibling of deren.vulkan.core): it depends on nothing but the Vulkan headers, so
 * any Vulkan module can use it. The name sets it apart from deren.vulkan.core:init_utils - that
 * module performs the initialization PROCEDURES (instance/device/swapchain), while this one
 * carries the compile-time CONSTANTS of those calls: the fixed field values ("constant init").
 *
 * The engine never hand-fills these structs at call sites: every fill is either
 *  - a constexpr factory returning the struct by value (each factory lists EVERY member with
 *    designated initializers, so the zero/unused fields are explicit and identical to a
 *    `= {}` zero-init), or
 *  - a constinit "transition" default the caller copies and then overrides only the fields
 *    that actually differ (e.g. the target image of a per-frame layout transition).
 *
 * Pointer members always point at caller-provided data (never at locals of the factory itself).
 * This module is header-only in effect: all definitions live in the interface, so callers can
 * constant-fold the factories.
 */
export namespace deren::vulkan {
    // ---- Object create infos (one line per object; fields fixed by engine convention) ----

    /**
     * @brief whether a colour format's attachment write path encodes linear -> sRGB in HARDWARE
     *
     * WHY IT IS HERE rather than in the renderer that used to define it: two PASSES need the answer (the composite
     * and FXAA, which push the same block's `encode_gamma` lane) and a third thing needs it for the same reason
     * (the swapchain's format decides whether the display transfer function is hardware's or the shader's).
     * Writing a gamma-encoded value into one of these formats double-encodes it, and writing it into a UNORM
     * target under-encodes it - so the question is asked on every post frame, and a second copy of the switch is a
     * second place for the list of formats to fall behind.
     */
    [[nodiscard]] constexpr bool is_srgb_format(VkFormat const format) noexcept {
        switch (format) {
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        case VK_FORMAT_R8G8B8_SRGB:
        case VK_FORMAT_B8G8R8_SRGB:
            return true;
        default:
            return false;
        }
    }

    /**
     * @brief command pool that allows per-buffer reset (the engine resets/re-records buffers)
     */
    constexpr VkCommandPoolCreateInfo make_command_pool_info(uint32_t const queue_family) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queue_family,
        };
    }
    /** @brief plain binary semaphore (vkAcquireNextImageKHR / vkQueuePresentKHR need binary) */
    constexpr VkSemaphoreCreateInfo make_binary_semaphore_info() noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };
    }
    /** @brief timeline semaphore starting at 0 (frame-slot submission counting + host pacing) */
    constexpr VkSemaphoreTypeCreateInfo make_timeline_semaphore_type_info() noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .pNext = nullptr,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0,
        };
    }
    /**
     * @brief linear/mipmap sampler with repeat-style addressing
     * @param address_mode applied to U/V/W
     * @param max_lod upper clamp (e.g. the texture's mip count - 1)
     */
    constexpr VkSamplerCreateInfo make_texture_sampler_info(VkSamplerAddressMode const address_mode, float const max_lod) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = address_mode,
            .addressModeV = address_mode,
            .addressModeW = address_mode,
            .mipLodBias = 0.0f,
            .anisotropyEnable = VK_FALSE,
            .maxAnisotropy = 1.0f,
            .compareEnable = VK_FALSE,
            .compareOp = VK_COMPARE_OP_NEVER,
            .minLod = 0.0f,
            .maxLod = max_lod,
            .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        };
    }
    /**
     * @brief shadow-map sampler: LINEAR min/mag gives HARDWARE percentage-closer filtering on a
     *        sampler2DShadow (compareOp matches pbr.frag's "not deeper than stored depth")
     */
    constexpr VkSamplerCreateInfo make_shadow_sampler_info() noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .mipLodBias = 0.0f,
            .anisotropyEnable = VK_FALSE,
            .maxAnisotropy = 1.0f,
            .compareEnable = VK_TRUE,
            .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
            .minLod = 0.0f,
            .maxLod = 0.0f,
            .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        };
    }
    /** @brief unsignaled fence (host waits after one-shot upload submits) */
    constexpr VkFenceCreateInfo make_fence_info() noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };
    }
    /**
     * @brief query pool of @p count queries, all of type @p query_type
     * @param query_type TIMESTAMP for the GPU pass timings, OCCLUSION for visibility queries
     * @param count number of queries in the pool (a timestamp pool also fixes how many marks a
     *        frame may write: see core::gpu_timing_mark_capacity)
     */
    constexpr VkQueryPoolCreateInfo make_query_pool_info(VkQueryType const query_type, uint32_t const count) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queryType = query_type,
            .queryCount = count,
            .pipelineStatistics = 0,
        };
    }
    /** @brief host-visible staging buffer: TRANSFER_SRC only, exclusive sharing */
    constexpr VkBufferCreateInfo make_staging_buffer_info(VkDeviceSize const size) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
    }
    /**
     * @brief one-queue device queue create info (one queue of @p queue_family)
     * @param queue_family
     * @param queue_priorities caller-provided array of @p queue_family's queue priorities
     */
    constexpr VkDeviceQueueCreateInfo make_device_queue_info(uint32_t const queue_family, float const* queue_priorities) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queueFamilyIndex = queue_family,
            .queueCount = 1,
            .pQueuePriorities = queue_priorities,
        };
    }
    /**
     * @brief 2D image view with identity component swizzle
     * @param image the image to view
     * @param format the image's format
     * @param view_type usually 2D
     * @param aspect_mask color or depth(-stencil)
     * @param level_count mip levels in the view (VK_REMAINING_MIP_LEVELS for the whole image)
     * @param layer_count array layers in the view (VK_REMAINING_ARRAY_LAYERS for the whole image)
     * @param base_mip_level first mip level the view addresses
     * @param base_array_layer first array layer the view addresses
     */
    constexpr VkImageViewCreateInfo make_image_view_info(VkImage const image, VkFormat const format, VkImageViewType const view_type, VkImageAspectFlags const aspect_mask, uint32_t const level_count, uint32_t const layer_count, uint32_t const base_mip_level = 0, uint32_t const base_array_layer = 0) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = image,
            .viewType = view_type,
            .format = format,
            .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange = {aspect_mask, base_mip_level, level_count, base_array_layer, layer_count},
        };
    }

    // ---- Barriers, copies, and the remaining call-site info structs ----
    //
    // These factories exist because the structures were still filled field by field at their
    // call sites: the type had no factory, so "use constant_init" had no answer. Each factory
    // lists every member, and the caller overrides only what varies (the same shape as
    // make_submit_info / make_attachment_info).

    /**
     * @brief one image memory barrier with the queue-family pair the engine always ignores
     * @param src_stage
     * @param src_access
     * @param dst_stage
     * @param dst_access
     * @param old_layout
     * @param new_layout
     * @param image target image (VK_NULL_HANDLE is allowed for a recipe template)
     * @param subresource_range
     * @param src_queue_family
     * @param dst_queue_family
     */
    constexpr VkImageMemoryBarrier2 make_image_memory_barrier_2(
        VkPipelineStageFlags2 const src_stage, VkAccessFlags2 const src_access,
        VkPipelineStageFlags2 const dst_stage, VkAccessFlags2 const dst_access,
        VkImageLayout const old_layout, VkImageLayout const new_layout,
        VkImage const image, VkImageSubresourceRange const& subresource_range,
        uint32_t const src_queue_family = VK_QUEUE_FAMILY_IGNORED,
        uint32_t const dst_queue_family = VK_QUEUE_FAMILY_IGNORED) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .srcQueueFamilyIndex = src_queue_family,
            .dstQueueFamilyIndex = dst_queue_family,
            .image = image,
            .subresourceRange = subresource_range,
        };
    }
    /** @brief one buffer memory barrier with the queue-family pair the engine always ignores */
    constexpr VkBufferMemoryBarrier2 make_buffer_memory_barrier_2(
        VkPipelineStageFlags2 const src_stage, VkAccessFlags2 const src_access,
        VkPipelineStageFlags2 const dst_stage, VkAccessFlags2 const dst_access,
        VkBuffer const buffer, VkDeviceSize const offset, VkDeviceSize const size,
        uint32_t const src_queue_family = VK_QUEUE_FAMILY_IGNORED,
        uint32_t const dst_queue_family = VK_QUEUE_FAMILY_IGNORED) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .srcQueueFamilyIndex = src_queue_family,
            .dstQueueFamilyIndex = dst_queue_family,
            .buffer = buffer,
            .offset = offset,
            .size = size,
        };
    }
    /** @brief buffer/image copy region with explicitly packed row and image strides */
    constexpr VkBufferImageCopy make_buffer_image_copy(
        VkDeviceSize const buffer_offset, uint32_t const buffer_row_length, uint32_t const buffer_image_height,
        VkImageSubresourceLayers const image_subresource, VkOffset3D const image_offset,
        VkExtent3D const image_extent) noexcept {
        return {
            .bufferOffset = buffer_offset,
            .bufferRowLength = buffer_row_length,
            .bufferImageHeight = buffer_image_height,
            .imageSubresource = image_subresource,
            .imageOffset = image_offset,
            .imageExtent = image_extent,
        };
    }
    /** @brief one host image-to-memory copy region, tightly packed (strides 0) */
    constexpr VkImageToMemoryCopy make_image_to_memory_copy(
        void* const host_pointer, VkImageSubresourceLayers const image_subresource,
        VkOffset3D const image_offset, VkExtent3D const image_extent) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY_EXT,
            .pNext = nullptr,
            .pHostPointer = host_pointer,
            .memoryRowLength = 0,
            .memoryImageHeight = 0,
            .imageSubresource = image_subresource,
            .imageOffset = image_offset,
            .imageExtent = image_extent,
        };
    }
    /** @brief one host memory-to-image copy region, tightly packed (strides 0) */
    constexpr VkMemoryToImageCopyEXT make_memory_to_image_copy(
        void const* const host_pointer, VkImageSubresourceLayers const image_subresource,
        VkOffset3D const image_offset, VkExtent3D const image_extent) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY_EXT,
            .pNext = nullptr,
            .pHostPointer = host_pointer,
            .memoryRowLength = 0,
            .memoryImageHeight = 0,
            .imageSubresource = image_subresource,
            .imageOffset = image_offset,
            .imageExtent = image_extent,
        };
    }
    /** @brief the head of one vkCopyImageToMemoryEXT call */
    constexpr VkCopyImageToMemoryInfo make_copy_image_to_memory_info(
        VkImage const src_image, VkImageLayout const src_layout,
        uint32_t const region_count, VkImageToMemoryCopy const* const regions) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO_EXT,
            .pNext = nullptr,
            .flags = 0,
            .srcImage = src_image,
            .srcImageLayout = src_layout,
            .regionCount = region_count,
            .pRegions = regions,
        };
    }
    /** @brief the head of one vkCopyMemoryToImageEXT call */
    constexpr VkCopyMemoryToImageInfoEXT make_copy_memory_to_image_info(
        VkImage const dst_image, VkImageLayout const dst_layout,
        uint32_t const region_count, VkMemoryToImageCopyEXT const* const regions) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO_EXT,
            .pNext = nullptr,
            .flags = 0,
            .dstImage = dst_image,
            .dstImageLayout = dst_layout,
            .regionCount = region_count,
            .pRegions = regions,
        };
    }
    /** @brief one descriptor heap binding window */
    constexpr VkBindHeapInfoEXT make_bind_heap_info(
        VkDeviceAddress const heap_address, VkDeviceSize const heap_size,
        VkDeviceSize const reserved_range_offset, VkDeviceSize const reserved_range_size) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
            .pNext = nullptr,
            .heapRange = {.address = heap_address, .size = heap_size},
            .reservedRangeOffset = reserved_range_offset,
            .reservedRangeSize = reserved_range_size,
        };
    }
    /** @brief one micromap build info; @p dst_micromap and @p scratch_address are null/0 for a size query */
    constexpr VkMicromapBuildInfoEXT make_micromap_build_info(
        VkMicromapTypeEXT const type, VkBuildMicromapFlagsEXT const flags, VkBuildMicromapModeEXT const mode,
        VkMicromapEXT const dst_micromap, uint32_t const usage_count,
        VkMicromapUsageEXT const* const usage, VkDeviceAddress const data_address,
        VkDeviceAddress const triangle_array_address, uint32_t const triangle_array_stride,
        VkDeviceAddress const scratch_address = 0) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT,
            .pNext = nullptr,
            .type = type,
            .flags = flags,
            .mode = mode,
            .dstMicromap = dst_micromap,
            .usageCountsCount = usage_count,
            .pUsageCounts = usage,
            .ppUsageCounts = nullptr,
            .data = {.deviceAddress = data_address},
            .scratchData = {.deviceAddress = scratch_address},
            .triangleArray = {.deviceAddress = triangle_array_address},
            .triangleArrayStride = triangle_array_stride,
        };
    }
    /** @brief the zeroed size-query answer struct (the driver fills the three sizes) */
    constexpr VkMicromapBuildSizesInfoEXT make_micromap_build_sizes_info() noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT,
            .pNext = nullptr,
            .micromapSize = 0,
            .buildScratchSize = 0,
            .discardable = 0,
        };
    }
    /** @brief one acceleration-structure build/refit geometry info */
    constexpr VkAccelerationStructureBuildGeometryInfoKHR make_acceleration_structure_build_info(
        VkAccelerationStructureTypeKHR const type, VkBuildAccelerationStructureFlagsKHR const flags,
        VkBuildAccelerationStructureModeKHR const mode, VkAccelerationStructureKHR const src,
        VkAccelerationStructureKHR const dst, uint32_t const geometry_count,
        VkAccelerationStructureGeometryKHR const* const geometries,
        VkDeviceAddress const scratch_address,
        VkAccelerationStructureGeometryKHR const* const* const pp_geometries = nullptr) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .pNext = nullptr,
            .type = type,
            .flags = flags,
            .mode = mode,
            .srcAccelerationStructure = src,
            .dstAccelerationStructure = dst,
            .geometryCount = geometry_count,
            .pGeometries = geometries,
            .ppGeometries = pp_geometries,
            .scratchData = {.deviceAddress = scratch_address},
        };
    }
    /** @brief the zeroed size-query answer struct (the driver fills the three sizes) */
    constexpr VkAccelerationStructureBuildSizesInfoKHR make_acceleration_structure_build_sizes_info() noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
            .pNext = nullptr,
            .accelerationStructureSize = 0,
            .updateScratchSize = 0,
            .buildScratchSize = 0,
        };
    }
    /** @brief one acceleration-structure binding at a constant heap offset */
    constexpr VkDescriptorSetAndBindingMappingEXT make_descriptor_set_and_binding_mapping(
        uint32_t const descriptor_set, uint32_t const first_binding, uint32_t const binding_count,
        VkSpirvResourceTypeFlagsEXT const resource_mask, VkDescriptorMappingSourceEXT const source,
        uint32_t const heap_offset, uint32_t const heap_array_stride) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
            .pNext = nullptr,
            .descriptorSet = descriptor_set,
            .firstBinding = first_binding,
            .bindingCount = binding_count,
            .resourceMask = resource_mask,
            .source = source,
            .sourceData = {
                .constantOffset = {
                    .heapOffset = heap_offset,
                    .heapArrayStride = heap_array_stride,
                    .pEmbeddedSampler = nullptr,
                    .samplerHeapOffset = 0,
                    .samplerHeapArrayStride = 0,
                },
            },
        };
    }
    /** @brief host wait on a semaphore value array */
    constexpr VkSemaphoreWaitInfo make_semaphore_wait_info(
        uint32_t const semaphore_count, VkSemaphore const* const semaphores,
        uint64_t const* const values, VkSemaphoreWaitFlags const flags = 0) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .pNext = nullptr,
            .flags = flags,
            .semaphoreCount = semaphore_count,
            .pSemaphores = semaphores,
            .pValues = values,
        };
    }
    /** @brief one swapchain present call */
    constexpr VkPresentInfoKHR make_present_info(
        uint32_t const wait_count, VkSemaphore const* const waits,
        uint32_t const swapchain_count, VkSwapchainKHR const* const swapchains,
        uint32_t const* const image_indices, VkResult* const results = nullptr) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = nullptr,
            .waitSemaphoreCount = wait_count,
            .pWaitSemaphores = waits,
            .swapchainCount = swapchain_count,
            .pSwapchains = swapchains,
            .pImageIndices = image_indices,
            .pResults = results,
        };
    }
    /** @brief the timeline value arrays attached to a submit */
    constexpr VkTimelineSemaphoreSubmitInfo make_timeline_semaphore_info(
        uint32_t const wait_count, uint64_t const* const wait_values,
        uint32_t const signal_count, uint64_t const* const signal_values) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreValueCount = wait_count,
            .pWaitSemaphoreValues = wait_values,
            .signalSemaphoreValueCount = signal_count,
            .pSignalSemaphoreValues = signal_values,
        };
    }
    /** @brief one shader module create info over caller-provided SPIR-V words */
    constexpr VkShaderModuleCreateInfo make_shader_module_info(
        std::size_t const code_size, uint32_t const* const code,
        VkShaderModuleCreateFlags const flags = 0) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext = nullptr,
            .flags = flags,
            .codeSize = code_size,
            .pCode = code,
        };
    }

    // ---- Device query chains (properties / features) ----
    //
    // WHY THESE LIVE HERE, and it is a MEASURED reason rather than tidiness: every structure a query chain
    // carries must be TOLD which type it is, and `= {}` writes that field as ZERO - which IS
    // `VK_STRUCTURE_TYPE_APPLICATION_INFO` (value 0). A chained member nobody `sType`d is therefore read as an
    // APPLICATION_INFO, and validation says so: "pNext chain includes a structure with unexpected
    // VkStructureType VK_STRUCTURE_TYPE_APPLICATION_INFO" (VUID-VkPhysicalDeviceProperties2-pNext-pNext),
    // measured on the acceleration-structure module's property query before
    // `make_acceleration_structure_properties()` was the only way to build it. This module's header already
    // states the rule ("the engine never hand-fills these structs at call sites"); these factories are what makes
    // the rule enforced for a query chain, where the WHOLE POINT is a list of tagged members.
    //
    // THE HEAD AND THE MEMBER ARE TWO CALLS, deliberately: a head takes the member's address and cannot be
    // built before it exists, so `make_properties_2(&member)` is the shape a caller writes - and neither call
    // can forget its sType, because neither call site names one.

    /**
     * @brief the HEAD of a `VkPhysicalDeviceProperties2` chain
     * @param next the first chained property structure (used for the duration of the call; null = no chain)
     */
    constexpr VkPhysicalDeviceProperties2 make_properties_2(void* const next = nullptr) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = next, .properties = {}};
    }

    /**
     * @brief the HEAD of a `VkPhysicalDeviceFeatures2` chain (the same shape, the query's other half)
     * @param next the first chained feature structure (used for the duration of the call; null = no chain)
     */
    constexpr VkPhysicalDeviceFeatures2 make_features_2(void* const next = nullptr) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = next, .features = {}};
    }

    // THE FOUR CHAINED BLOCKS BELOW ARE QUERY OUTPUTS, so only the tagged fields are named and the warning that
    // wants every member spelled is turned off for them - the SAME exception, with the same reason, that
    // `deren.vulkan.core:init_utils` takes for its property chain: the other fields are the DEVICE's answer,
    // written by the driver, and listing them as zeroes here would be a caller claiming to fill them. The sType
    // and pNext are OURS, and they are the two the type system cannot check.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
    /// @brief the acceleration-structure property block (query only; zeroed fields = "the device did not answer")
    constexpr VkPhysicalDeviceAccelerationStructurePropertiesKHR make_acceleration_structure_properties() noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR, .pNext = nullptr};
    }

    /// @brief the shader-binding-table numbers a ray-tracing pipeline is built against (query only)
    constexpr VkPhysicalDeviceRayTracingPipelinePropertiesKHR make_ray_tracing_pipeline_properties() noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR, .pNext = nullptr};
    }

    /// @brief the ray-query feature block (one bool, and it is the renderer's gate for the traced path)
    constexpr VkPhysicalDeviceRayQueryFeaturesKHR make_ray_query_features() noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .pNext = nullptr};
    }

    /// @brief the mesh-shader feature block (the other ability the renderer gates a whole path on)
    constexpr VkPhysicalDeviceMeshShaderFeaturesEXT make_mesh_shader_features() noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, .pNext = nullptr};
    }
#ifdef __clang__
#pragma clang diagnostic pop
#endif

    // ---- Command buffer / submit infos ----

    /**
     * @brief command buffer allocation: @p level buffers from @p pool
     * @param pool command pool to allocate from
     * @param level primary or secondary
     */
    constexpr VkCommandBufferAllocateInfo make_command_buffer_allocate_info(VkCommandPool const pool, VkCommandBufferLevel const level) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = pool,
            .level = level,
            .commandBufferCount = 1,
        };
    }
    /**
     * @brief command buffer begin info
     * @param flags usage flags (0 for plain inline recording, ONE_TIME_SUBMIT for one-shot
     *        uploads, RENDER_PASS_CONTINUE for secondaries)
     * @param inheritance secondary-buffer inheritance info (nullptr for primary buffers)
     */
    constexpr VkCommandBufferBeginInfo make_command_buffer_begin_info(VkCommandBufferUsageFlags const flags, VkCommandBufferInheritanceInfo const* inheritance) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = flags,
            .pInheritanceInfo = inheritance,
        };
    }
    /**
     * @brief one-shot submit of a command buffer with no semaphores
     * @param command_buffers pointer to the caller's VkCommandBuffer (may be null when count is 0)
     * @param command_buffer_count number of command buffers to submit (0 for a semaphore-only drain)
     */
    constexpr VkSubmitInfo make_submit_info(VkCommandBuffer const* command_buffers, uint32_t const command_buffer_count = 1) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = 0,
            .pWaitSemaphores = nullptr,
            .pWaitDstStageMask = nullptr,
            .commandBufferCount = command_buffer_count,
            .pCommandBuffers = command_buffers,
            .signalSemaphoreCount = 0,
            .pSignalSemaphores = nullptr,
        };
    }

    // ---- Copy-command infos ----

    /**
     * @brief one image subresource range, the source or destination half of an image copy
     * @param aspect_mask colour or depth(-stencil)
     * @param mip_level mip level the copy addresses
     * @param base_array_layer first array layer
     * @param layer_count array layers copied (VK_REMAINING_ARRAY_LAYERS for the rest)
     */
    constexpr VkImageSubresourceLayers make_image_subresource_layers(VkImageAspectFlags const aspect_mask, uint32_t const mip_level, uint32_t const base_array_layer, uint32_t const layer_count) noexcept {
        return {.aspectMask = aspect_mask, .mipLevel = mip_level, .baseArrayLayer = base_array_layer, .layerCount = layer_count};
    }
    /**
     * @brief one vkCmdCopyImage2 region
     * @param src_subresource source mip/layer range
     * @param src_x source x offset in texels
     * @param src_y source y offset in texels
     * @param src_z source z offset in texels
     * @param dst_subresource destination mip/layer range
     * @param dst_x destination x offset in texels
     * @param dst_y destination y offset in texels
     * @param dst_z destination z offset in texels
     * @param width copied width in texels
     * @param height copied height in texels
     * @param depth copied depth in texels
     */
    constexpr VkImageCopy2 make_image_copy_2(VkImageSubresourceLayers const src_subresource, std::int32_t const src_x, std::int32_t const src_y, std::int32_t const src_z, VkImageSubresourceLayers const dst_subresource, std::int32_t const dst_x, std::int32_t const dst_y, std::int32_t const dst_z, std::uint32_t const width, std::uint32_t const height, std::uint32_t const depth) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
            .pNext = nullptr,
            .srcSubresource = src_subresource,
            .srcOffset = {src_x, src_y, src_z},
            .dstSubresource = dst_subresource,
            .dstOffset = {dst_x, dst_y, dst_z},
            .extent = {width, height, depth},
        };
    }
    /**
     * @brief one vkCmdCopyImage2 call
     * @param src_image source image
     * @param src_layout source image layout
     * @param dst_image destination image
     * @param dst_layout destination image layout
     * @param region_count number of regions
     * @param regions caller-provided image-copy array
     */
    constexpr VkCopyImageInfo2 make_copy_image_info_2(VkImage const src_image, VkImageLayout const src_layout, VkImage const dst_image, VkImageLayout const dst_layout, uint32_t const region_count, VkImageCopy2 const* regions) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
            .pNext = nullptr,
            .srcImage = src_image,
            .srcImageLayout = src_layout,
            .dstImage = dst_image,
            .dstImageLayout = dst_layout,
            .regionCount = region_count,
            .pRegions = regions,
        };
    }
    /**
     * @brief one vkCmdCopyBuffer2 region
     * @param src_offset source byte offset
     * @param dst_offset destination byte offset
     * @param size bytes copied
     */
    constexpr VkBufferCopy2 make_buffer_copy_2(VkDeviceSize const src_offset, VkDeviceSize const dst_offset, VkDeviceSize const size) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2, .pNext = nullptr, .srcOffset = src_offset, .dstOffset = dst_offset, .size = size};
    }
    /**
     * @brief one vkCmdCopyBuffer2 call
     * @param src_buffer source buffer
     * @param dst_buffer destination buffer
     * @param region_count number of regions
     * @param regions caller-provided buffer-copy array
     */
    constexpr VkCopyBufferInfo2 make_copy_buffer_info_2(VkBuffer const src_buffer, VkBuffer const dst_buffer, uint32_t const region_count, VkBufferCopy2 const* regions) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
            .pNext = nullptr,
            .srcBuffer = src_buffer,
            .dstBuffer = dst_buffer,
            .regionCount = region_count,
            .pRegions = regions,
        };
    }

    // ---- Secondary-command-buffer inheritance (dynamic rendering 1.3) ----

    /**
     * @brief VkCommandBufferInheritanceInfo shell; only the pNext chain (the rendering info)
     *        differs per buffer
     * @param p_next points at the VkCommandBufferInheritanceRenderingInfo (caller-provided)
     */
    constexpr VkCommandBufferInheritanceInfo make_inheritance_info(void const* p_next) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
            .pNext = p_next,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0,
            .framebuffer = VK_NULL_HANDLE,
            .occlusionQueryEnable = VK_FALSE,
            .queryFlags = 0,
            .pipelineStatistics = 0,
        };
    }
    /**
     * @brief dynamic-rendering inheritance: which attachments a secondary may assume
     * @param has_color_attachment depth-only secondaries (shadow pass) pass false
     * @param color_format_ptr caller-provided color format (ignored when has_color_attachment
     *        is false)
     * @param depth_format the depth attachment's format (VK_FORMAT_UNDEFINED if none)
     * @param rasterization_samples the instance's sample count (always 1 in this engine)
     */
    constexpr VkCommandBufferInheritanceRenderingInfo make_inheritance_rendering_info(bool const has_color_attachment, VkFormat const* color_format_ptr, VkFormat const depth_format, VkSampleCountFlagBits const rasterization_samples) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
            .pNext = nullptr,
            .flags = 0,
            .viewMask = 0,
            .colorAttachmentCount = has_color_attachment ? 1u : 0u,
            .pColorAttachmentFormats = has_color_attachment ? color_format_ptr : nullptr,
            .depthAttachmentFormat = depth_format,
            .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
            .rasterizationSamples = rasterization_samples,
        };
    }
    /**
     * @brief dynamic-rendering inheritance with N color attachments: a secondary recorded for a
     *        multi-target instance (the G-buffer) must declare every format it may write
     * @param color_formats caller-provided array of @p color_count formats, in attachment order
     * @param color_count number of color attachments declared
     * @param depth_format the depth attachment's format (VK_FORMAT_UNDEFINED if none)
     * @param rasterization_samples the instance's sample count (always 1 in this engine)
     */
    constexpr VkCommandBufferInheritanceRenderingInfo make_inheritance_rendering_info(VkFormat const* color_formats, uint32_t const color_count, VkFormat const depth_format, VkSampleCountFlagBits const rasterization_samples) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
            .pNext = nullptr,
            .flags = 0,
            .viewMask = 0,
            .colorAttachmentCount = color_count,
            .pColorAttachmentFormats = color_count > 0 ? color_formats : nullptr,
            .depthAttachmentFormat = depth_format,
            .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
            .rasterizationSamples = rasterization_samples,
        };
    }
    /**
     * @brief one barrier pass with any mix of memory, buffer, and image barriers
     * @param image_count number of image barriers
     * @param images caller-provided image barrier array
     * @param buffer_count number of buffer barriers
     * @param buffers caller-provided buffer barrier array
     * @param memory_count number of global memory barriers
     * @param memories caller-provided memory barrier array
     */
    constexpr VkDependencyInfo make_dependency_info(uint32_t const image_count, VkImageMemoryBarrier2 const* images, uint32_t const buffer_count, VkBufferMemoryBarrier2 const* buffers, uint32_t const memory_count = 0, VkMemoryBarrier2 const* memories = nullptr) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext = nullptr,
            .dependencyFlags = 0,
            .memoryBarrierCount = memory_count,
            .pMemoryBarriers = memories,
            .bufferMemoryBarrierCount = buffer_count,
            .pBufferMemoryBarriers = buffers,
            .imageMemoryBarrierCount = image_count,
            .pImageMemoryBarriers = images,
        };
    }
    /**
     * @brief one barrier pass: VkDependencyInfo with only image memory barriers
     * @param image_barrier_count number of barriers
     * @param barriers caller-provided barrier array
     */
    constexpr VkDependencyInfo make_image_dependency_info(uint32_t const image_barrier_count, VkImageMemoryBarrier2 const* barriers) noexcept {
        return make_dependency_info(image_barrier_count, barriers, 0, nullptr);
    }

    // ---- Dynamic rendering attachment infos ----

    /**
     * @brief one rendering attachment with caller-chosen load/store ops (the generic form of the
     *        fixed CLEAR/LOAD helpers below)
     * @param image_view the attachment's image view
     * @param image_layout the attachment's layout while rendering (the engine uses GENERAL)
     * @param load_op CLEAR, LOAD, or DONT_CARE
     * @param store_op STORE or DONT_CARE
     * @param clear_value used when @p load_op is CLEAR
     * @param resolve_mode the resolve mode (the engine always renders at 1x, so NONE)
     * @param resolve_image_view the resolve target when resolve_mode is not NONE, else VK_NULL_HANDLE
     */
    constexpr VkRenderingAttachmentInfo make_attachment_info(VkImageView const image_view, VkImageLayout const image_layout, VkAttachmentLoadOp const load_op, VkAttachmentStoreOp const store_op, VkClearValue const clear_value, VkResolveModeFlagBits const resolve_mode = VK_RESOLVE_MODE_NONE,
                                                             VkImageView const resolve_image_view = VK_NULL_HANDLE) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext = nullptr,
            .imageView = image_view,
            .imageLayout = image_layout,
            .resolveMode = resolve_mode,
            .resolveImageView = resolve_image_view,
            .resolveImageLayout = resolve_mode == VK_RESOLVE_MODE_NONE ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = load_op,
            .storeOp = store_op,
            .clearValue = clear_value,
        };
    }

    /**
     * @brief depth attachment of a rendering instance: loadOp CLEAR with the far-plane value
     *        (1.0, stencil 0 - the engine clears every attachment on load) and no resolve
     * @param image_view the depth image view
     * @param store_op DONT_CARE only for a depth buffer nothing ever reads back (the forward
     *        path's main depth); STORE for the shadow map and the G-buffer depth, whose contents
     *        a later pass in the same submission samples
     */
    constexpr VkRenderingAttachmentInfo make_depth_attachment_info(VkImageView const image_view, VkAttachmentStoreOp const store_op) noexcept {
        VkClearValue clear_value = {.depthStencil = {.depth = 1.0f, .stencil = 0}};
        return {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext = nullptr,
            .imageView = image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .resolveMode = VK_RESOLVE_MODE_NONE,
            .resolveImageView = VK_NULL_HANDLE,
            .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = store_op,
            .clearValue = clear_value,
        };
    }
    /**
     * @brief depth attachment of an instance that CONTINUES an existing depth buffer: loadOp LOAD,
     *        storeOp STORE, so the depth an earlier instance wrote stays intact
     * @param image_view the depth image view, already in GENERAL
     * @note the deferred path's transparent pass: it depth-tests alpha-blended geometry against the
     *       opaque surface the G-buffer pass wrote, and must not clear it (that depth is the only
     *       record of where the opaque geometry is)
     */
    constexpr VkRenderingAttachmentInfo make_load_depth_attachment_info(VkImageView const image_view) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext = nullptr,
            .imageView = image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .resolveMode = VK_RESOLVE_MODE_NONE,
            .resolveImageView = VK_NULL_HANDLE,
            .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {},
        };
    }
    /**
     * @brief color attachment of a rendering instance: GENERAL layout,
     *        loadOp CLEAR + storeOp STORE (the swapchain image is presented afterwards)
     * @param image_view the color image view
     * @param clear_value the runtime clear color
     * @param resolve_mode the resolve mode (the engine always renders at 1x, so NONE)
     * @param resolve_image_view the resolve target when resolve_mode is not NONE, else VK_NULL_HANDLE.
     *        NOTE: the resolve layout must not be PRESENT_SRC_KHR
     *        (VUID-VkRenderingAttachmentInfo-imageView-06146) - the swapchain image moves to
     *        PRESENT_SRC_KHR only after vkCmdEndRendering
     */
    constexpr VkRenderingAttachmentInfo make_color_attachment_info(VkImageView const image_view, VkClearValue const clear_value, VkResolveModeFlagBits const resolve_mode, VkImageView const resolve_image_view) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext = nullptr,
            .imageView = image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .resolveMode = resolve_mode,
            .resolveImageView = resolve_image_view,
            .resolveImageLayout = resolve_mode == VK_RESOLVE_MODE_NONE ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = clear_value,
        };
    }
    /**
     * @brief color attachment of a rendering instance that LOADS the image's existing contents
     *        (no clear) and stores the result
     * @param image_view the color image view to render into
     * @note the deferred path's HDR target: the background pass and the G-buffer pass's emissive
     *       already wrote it, and the lighting stage adds on top - clearing it would throw both away
     */
    constexpr VkRenderingAttachmentInfo make_load_color_attachment_info(VkImageView const image_view) noexcept {
        VkClearValue clear_value = {};
        return {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext = nullptr,
            .imageView = image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .resolveMode = VK_RESOLVE_MODE_NONE,
            .resolveImageView = VK_NULL_HANDLE,
            .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = clear_value,
        };
    }
    /**
     * @brief dynamic-rendering instance: one layer, at most one color attachment
     * @param flags e.g. VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT
     * @param render_area the area to render into (the pipelines apply their viewport/scissor)
     * @param has_color_attachment depth-only passes (the shadow map) pass false
     * @param color_attachments pointer to the single color attachment when present, else nullptr
     * @param depth_attachment the depth attachment, or nullptr
     */
    constexpr VkRenderingInfo make_rendering_info(VkRenderingFlags const flags, VkRect2D const render_area, bool const has_color_attachment, VkRenderingAttachmentInfo const* color_attachments, VkRenderingAttachmentInfo const* depth_attachment) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .pNext = nullptr,
            .flags = flags,
            .renderArea = render_area,
            .layerCount = 1,
            .viewMask = 0,
            .colorAttachmentCount = has_color_attachment ? 1u : 0u,
            .pColorAttachments = has_color_attachment ? color_attachments : nullptr,
            .pDepthAttachment = depth_attachment,
            .pStencilAttachment = nullptr,
        };
    }
    /**
     * @brief dynamic-rendering instance with N color attachments (the G-buffer pass writes three:
     *        albedo/metallic, normal/roughness, material id/AO/flags)
     * @param flags e.g. VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT
     * @param render_area the area to render into (the pipelines apply their viewport/scissor)
     * @param color_attachments caller-provided array of @p color_count attachments (in the order the
     *        fragment shader's layout(location = i) outputs address)
     * @param color_count number of color attachments (0 for a depth-only pass, same as passing
     *        false to the single-attachment overload above)
     * @param depth_attachment the depth attachment, or nullptr
     * @param layer_count number of layers rendered to (1 unless a multiview pass)
     */
    constexpr VkRenderingInfo make_rendering_info(VkRenderingFlags const flags, VkRect2D const render_area, VkRenderingAttachmentInfo const* color_attachments, uint32_t const color_count, VkRenderingAttachmentInfo const* depth_attachment, uint32_t const layer_count = 1) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .pNext = nullptr,
            .flags = flags,
            .renderArea = render_area,
            .layerCount = layer_count,
            .viewMask = 0,
            .colorAttachmentCount = color_count,
            .pColorAttachments = color_count > 0 ? color_attachments : nullptr,
            .pDepthAttachment = depth_attachment,
            .pStencilAttachment = nullptr,
        };
    }

    // ---- Fixed-function pipeline state (engine-wide conventions) ----

    /** @brief one shader stage of a graphics pipeline (entry point "main", no specialization) */
    constexpr VkPipelineShaderStageCreateInfo make_shader_stage(VkShaderModule const module, VkShaderStageFlagBits const stage) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = stage,
            .module = module,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        };
    }
    /** @brief triangle-list input assembly */
    constexpr VkPipelineInputAssemblyStateCreateInfo make_input_assembly_state() noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .primitiveRestartEnable = VK_FALSE,
        };
    }
    /** @brief viewport/scissor are dynamic state: only the counts are static */
    constexpr VkPipelineViewportStateCreateInfo make_viewport_state() noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .viewportCount = 1,
            .pViewports = nullptr,
            .scissorCount = 1,
            .pScissors = nullptr,
        };
    }
    /** @brief dynamic state list (caller-provided array) */
    constexpr VkPipelineDynamicStateCreateInfo make_dynamic_state(VkDynamicState const* states, uint32_t const count) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .dynamicStateCount = count,
            .pDynamicStates = states,
        };
    }
    /**
     * @brief fill rasterization, back-face cull, CCW front face, optional slope-scaled depth bias
     * @param depth_bias_enabled the bias is dynamic state on shadow pipelines; the static
     *        factors only matter while it is never set dynamically
     * @param depth_bias_constant_factor constant depth bias
     * @param depth_bias_slope_factor slope-scaled depth bias
     * @param depth_bias_clamp depth bias clamp, 0 disables clamping
     */
    constexpr VkPipelineRasterizationStateCreateInfo make_rasterization_state(bool const depth_bias_enabled, float const depth_bias_constant_factor, float const depth_bias_slope_factor, float const depth_bias_clamp) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_BACK_BIT,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = depth_bias_enabled ? VK_TRUE : VK_FALSE,
            .depthBiasConstantFactor = depth_bias_constant_factor,
            .depthBiasClamp = depth_bias_clamp,
            .depthBiasSlopeFactor = depth_bias_slope_factor,
            .lineWidth = 1.0f,
        };
    }
    /**
     * @brief depth test + write, with the COMPARE OPERATOR chosen by the caller
     * @param depth_test_enabled enable the test; DEPTH WRITE FOLLOWS IT, which is this family's rule and
     *        is why a pass that needs the test ON with the write OFF must turn the write off through the
     *        DYNAMIC setter instead: depth write is a dynamic state here (see the dynamic-state list in
     *        deren.vulkan.core.pipeline), the compare operator is not.
     * @param depth_compare_op the operator. LESS_OR_EQUAL is what every pipeline in the renderer used
     *        before this parameter existed; the CHARACTER-FORWARD pass is the first caller that needs a
     *        different one, and the reason is the whole point of that pass: it OVERWRITES the pixels the
     *        deferred stage already lit, and `EQUAL` is what restricts that overwrite to exactly the
     *        surface the G-buffer pass recorded. With LESS_OR_EQUAL it would also re-shade fragments that
     *        happen to be in front of the recorded depth, which is not a surface it was asked to draw.
     */
    constexpr VkPipelineDepthStencilStateCreateInfo make_depth_stencil_state(bool const depth_test_enabled, VkCompareOp const depth_compare_op) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .depthTestEnable = depth_test_enabled ? VK_TRUE : VK_FALSE,
            .depthWriteEnable = depth_test_enabled ? VK_TRUE : VK_FALSE,
            .depthCompareOp = depth_compare_op,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE,
            .front = {},
            .back = {},
            .minDepthBounds = 0.0f,
            .maxDepthBounds = 0.0f,
        };
    }
    /**
     * @brief depth test + write (disabled for background passes such as the skybox, which draw
     *        first and must not occlude later geometry); compare LESS_OR_EQUAL
     * @note DELEGATES to the two-argument form rather than repeating its ten initializers: the operator is
     *       the ONLY thing this overload fixes, and a second copy of the state would be a second place to
     *       keep in step (the failure mode this file's header warns about).
     */
    constexpr VkPipelineDepthStencilStateCreateInfo make_depth_stencil_state(bool const depth_test_enabled) noexcept {
        return make_depth_stencil_state(depth_test_enabled, VK_COMPARE_OP_LESS_OR_EQUAL);
    }
    /**
     * @brief standard alpha blending, ALWAYS enabled: with src alpha == 1 (an opaque material)
     *        the blend math reduces to the source color exactly, so opaque draws are
     *        pixel-identical whether or not blending is on. Blended (transparent) materials
     *        carry alpha < 1 and are drawn depth-write-off in the transparent pass - no second
     *        pipeline needed.
     */
    constexpr VkPipelineColorBlendAttachmentState make_color_blend_attachment() noexcept {
        return {
            .blendEnable = VK_TRUE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
    }
    /**
     * @brief blend state for a target that is OVERWRITTEN instead of blended (a G-buffer
     *        attachment): blending disabled, all four channels written
     * @note a G-buffer target must use this one. On the forward pipelines alpha is coverage, so
     *       "src alpha == 1" makes make_color_blend_attachment() reduce to an overwrite - but in a
     *       G-buffer alpha carries DATA (metallic, roughness, flags), so the same state scales and
     *       then MIXES the stored surface with the cleared target: an untextured/fully-rough
     *       fragment (alpha 0) would erase its albedo and material id. Measured symptom before
     *       the fix: the albedo target was ~0 everywhere and the material-id target entirely black,
     *       while the normal target (alpha = roughness, rarely 0) survived but was dimmed by it.
     */
    constexpr VkPipelineColorBlendAttachmentState make_color_blend_attachment_opaque() noexcept {
        return {
            .blendEnable = VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
    }
    /**
     * @brief blend state for a target that is ACCUMULATED INTO (src + dst): the G-buffer pass's
     *        emissive attachment, and the deferred lighting pass
     * @note the destination must be load-preserved by the rendering instance (VK_ATTACHMENT_LOAD_OP_LOAD),
     *       which is the point: the emissive and the lighting are added on top of the sky the
     *       background pass already wrote, and a pixel with no geometry contributes exactly 0
     */
    constexpr VkPipelineColorBlendAttachmentState make_color_blend_attachment_additive() noexcept {
        return {
            .blendEnable = VK_TRUE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
    }
    /**
     * @brief blend state for a target that is MULTIPLIED INTO (`dst = src * dst`): the article's two overlay
     *        masks, and nothing else in this renderer
     *
     * WHY IT IS A FACTOR PAIR AND NOT AN EXTENSION, which is the whole reason this late addition to the
     * renderer costs no device feature: the article's shaders are authored in Unity, where `BlendOp Multiply`
     * and `Blend [_BlendSrc=5][_BlendDst=1]` compile to the `KHR_blend_operation_advanced` equation
     * `MULTIPLY` - and an advanced equation IGNORES the source and destination factors. The same product is
     * reachable in core Vulkan by naming the FACTORS instead of the equation: with `blendOp = ADD`,
     * `srcColorBlendFactor = DST_COLOR` and `dstColorBlendFactor = ZERO` the blend reduces to
     * `src * dst + dst * 0`, i.e. exactly `src * dst`. So the port needs no extension, no capability query and
     * no fallback path - and the chapter's `MyZmdEyeDarkShader` / `MyZmdHairShadowShader` are reproduced by
     * the blend state rather than approximated by it.
     *
     * THE ALPHA CHANNEL IS DELIBERATELY UNTOUCHED (`ZERO`/`ONE`, an ADD of nothing): the article's fragment
     * stages also compute an output alpha, and under an advanced blend their alpha would multiply the target's
     * too - but the target here is the HDR colour image, whose alpha carries no coverage and is read by nobody.
     * Writing `dst` back is the choice that keeps this state from silently attenuating a channel that no
     * consumer of this pass ever asked about, and it is stated here rather than left to a default because it is
     * the one channel where "the article does something and we do not" is a deliberate difference.
     *
     * @note the destination must be LOAD-preserved by the rendering instance, which is the point: the multiply
     *       reads what the pass before it wrote, and a cleared target would turn the overlay into a black
     *       rectangle instead of a shadow
     */
    constexpr VkPipelineColorBlendAttachmentState make_color_blend_attachment_multiply() noexcept {
        return {
            .blendEnable = VK_TRUE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
    }
    /**
     * @brief blend state for one attachment; depth-only pipelines have no color attachment
     * @param has_color_attachment
     * @param attachment caller-provided blend attachment (ignored when has_color_attachment false)
     */
    constexpr VkPipelineColorBlendStateCreateInfo make_color_blend_state(bool const has_color_attachment, VkPipelineColorBlendAttachmentState const* attachment) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .logicOpEnable = VK_FALSE,
            .logicOp = VK_LOGIC_OP_COPY,
            .attachmentCount = has_color_attachment ? 1u : 0u,
            .pAttachments = has_color_attachment ? attachment : nullptr,
            .blendConstants = {1.0f, 1.0f, 1.0f, 1.0f},
        };
    }
    /**
     * @brief blend state with N attachments, same blend constants
     * @param attachments caller-provided array of @p count blend attachments, in attachment order
     * @param count number of attachments (0 for a depth-only pipeline)
     */
    constexpr VkPipelineColorBlendStateCreateInfo make_color_blend_state(VkPipelineColorBlendAttachmentState const* attachments, uint32_t const count) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .logicOpEnable = VK_FALSE,
            .logicOp = VK_LOGIC_OP_COPY,
            .attachmentCount = count,
            .pAttachments = count > 0 ? attachments : nullptr,
            .blendConstants = {1.0f, 1.0f, 1.0f, 1.0f},
        };
    }
    /** @brief multisample state (rasterizationSamples is the instance's sample count) */
    constexpr VkPipelineMultisampleStateCreateInfo make_multisample_state(VkSampleCountFlagBits const samples) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .rasterizationSamples = samples,
            .sampleShadingEnable = VK_FALSE,
            .minSampleShading = 0.0f,
            .pSampleMask = nullptr,
            .alphaToCoverageEnable = VK_FALSE,
            .alphaToOneEnable = VK_FALSE,
        };
    }
    /**
     * @brief dynamic-rendering attachment declaration (replaces render pass + subpass)
     * @param has_color_attachment
     * @param color_format_ptr pointer to the caller's color format: the returned info stores
     *        that pointer, so it must outlive the struct
     * @param depth_format the depth attachment format (VK_FORMAT_UNDEFINED for depth-only)
     */
    constexpr VkPipelineRenderingCreateInfo make_rendering_create_info(bool const has_color_attachment, VkFormat const* color_format_ptr, VkFormat const depth_format) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .pNext = nullptr,
            .viewMask = 0,
            .colorAttachmentCount = has_color_attachment ? 1u : 0u,
            .pColorAttachmentFormats = has_color_attachment ? color_format_ptr : nullptr,
            .depthAttachmentFormat = depth_format,
            .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
        };
    }
    /**
     * @brief dynamic-rendering attachment declaration for an N-target pipeline (the G-buffer
     *        pipeline declares its three targets, in the order its fragment outputs address them)
     * @param color_formats pointer to the caller's format array (stored by the struct, so it
     *        must outlive it - pass the core's static gbuffer_formats)
     * @param color_count number of color attachment formats declared
     * @param depth_format the depth attachment format (VK_FORMAT_UNDEFINED if none)
     */
    constexpr VkPipelineRenderingCreateInfo make_rendering_create_info(VkFormat const* color_formats, uint32_t const color_count, VkFormat const depth_format) noexcept {
        return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .pNext = nullptr,
            .viewMask = 0,
            .colorAttachmentCount = color_count,
            .pColorAttachmentFormats = color_count > 0 ? color_formats : nullptr,
            .depthAttachmentFormat = depth_format,
            .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
        };
    }

    // ---- Per-frame image layout transitions (constinit defaults) ----
    // VK_KHR_unified_image_layouts (REQUIRED, see init_utils) promises that VK_IMAGE_LAYOUT_GENERAL is
    // as efficient as the purpose-built layouts, so this renderer keeps EVERY image in GENERAL and
    // oldLayout == newLayout == GENERAL below. What is left of these barriers is the job they always
    // really had: ordering one stage's write before another stage's read. Only two other layouts
    // survive, and the extension does not replace either:
    //   * oldLayout UNDEFINED - the contents are discarded or were never defined (loadOp CLEAR, a
    //     transient target, a freshly created image), which is how an image's layout metadata is
    //     initialised and the reason no per-frame layout tracking is needed;
    //   * newLayout PRESENT_SRC_KHR - the presentation engine lives outside Vulkan, so present is the
    //     one consumer the extension cannot fold into GENERAL.
    // Every frame runs the same roles; only the target image differs per barrier, so each role below is
    // a default that call sites copy and then override .image on (immutable by design - the invariants
    // must not be retargeted in place). The NAMES keep the role each barrier used to play while the
    // layouts still differed (the far side was COLOR_ATTACHMENT_OPTIMAL / SHADER_READ_ONLY_OPTIMAL /
    // TRANSFER_*_OPTIMAL / ...), because that role is what decides which stages and access masks the
    // barrier has to name.
    /** @brief UNDEFINED -> GENERAL, color-attachment write (a scene target being rendered into)
     * @note The source stage also chains a swapchain image's acquire wait at COLOR_ATTACHMENT_OUTPUT.
     *       Discarding its contents does not permit the layout transition to race presentation. */
    inline constexpr VkImageMemoryBarrier2 color_attachment_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief same-layout dependency (GENERAL -> GENERAL): order one rendering instance's
     *         color-attachment WRITE before the next instance's LOAD of the same image (the scene
     *         color the G-buffer pass fills with the emissive and the deferred lighting stage then
     *         loads to add the lighting on top).
     * @note dynamic rendering inserts no separate dependency between two instances, and no layout
     *       changes here (none ever does any more), so this barrier exists purely for the write -> read
     *       visibility: without it the second instance's loadOp is not ordered after the first
     *       instance's storeOp - LOAD is a color-attachment access, not a fragment-shader read,
     *       which is why the sampling transitions of the same image cannot stand in for it. */
    inline constexpr VkImageMemoryBarrier2 color_attachment_dependency = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief UNDEFINED -> GENERAL, depth write (main depth buffer + the shadow map) */
    inline constexpr VkImageMemoryBarrier2 depth_attachment_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    /** @brief depth attachment -> GENERAL, sampled read (the shadow map back to the
     *         main pass, and the G-buffer depth to everything that reconstructs from it).
     * @note BOTH consumer stages are named: the G-buffer images have had a COMPUTE consumer since the
     *       screen-space GI passes started reading the stored surface directly (source/shaders/lighting/megalights_trace.slang
     *       and megalights_temporal.comp name the gbuffer slots from a compute stage), and a layout transition
     *       has to name every stage that reads the image afterwards. */
    inline constexpr VkImageMemoryBarrier2 shadow_map_sampling_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    /** @brief same-layout dependency (GENERAL -> GENERAL), depth test without depth
     *         write (the G-buffer depth handed back to an attachment for the deferred path's
     *         transparent pass, which depth-tests against the surface the lighting stage just
     *         sampled it for) */
    inline constexpr VkImageMemoryBarrier2 sampling_to_depth_attachment_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        // READ and not WRITE: every transparent leaf draws with depth writes disabled (see
        // primitive::draw), so nothing in that instance writes the depth it tests against
        .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    /** @brief UNDEFINED -> GENERAL, compute storage-image write (the half-res GI image, which is
     *         written as a storage image rather than rendered into, so it lives in GENERAL) */
    inline constexpr VkImageMemoryBarrier2 undefined_to_general_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief compute write -> sampled read (GENERAL -> GENERAL): the GI images handed to samplers by a compute
     *         SHADER_WRITE is not visible to a later read without this).
     * @note BOTH consumer stages are named, because the two GI images are handed to different ones:
     *       the raw trace goes to the denoiser's resolve, which is another COMPUTE dispatch, while the
     *       resolved image goes to the FRAGMENT composite. Naming only FRAGMENT would leave the
     *       trace -> resolve hand-off unordered - the resolve could sample a half-written trace. */
    inline constexpr VkImageMemoryBarrier2 general_to_sampling_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief sampled read -> compute write (GENERAL -> GENERAL, contents kept): an image read as a sample going back to being
     *         written as a compute storage image, KEEPING its contents.
     * @note the opposite of general_to_sampling_transition, and the reason it exists rather than the
     *       write simply claiming UNDEFINED (which is legal and cheaper): a history image is READ across
     *       frames, so a write that discarded its contents would throw away exactly what the next frame's
     *       reader needs.
     * @note the reading stages are named on the src side and COMPUTE on the dst: the previous frame's
     *       resolve is sampled by the tracer and by the spatial filter (COMPUTE), and by the composite
     *       (FRAGMENT). */
    inline constexpr VkImageMemoryBarrier2 sampling_to_general_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief compute write -> compute read+write (GENERAL -> GENERAL): one compute storage-image write
     *         followed by another dispatch that reads it and writes again - a ping-pong's two images.
     * @note a SAME-layout barrier, which is not a no-op: it is the memory dependency between two
     *       dispatches that touch the same image, and consecutive vkCmdDispatch calls in one command
     *       buffer have none. The layout is named anyway, as in every other barrier here.
     * @note COMPUTE on both sides, with SHADER_WRITE on the src and SHADER_READ | SHADER_WRITE on the
     *       dst: the next dispatch both samples the previous one's cells and overwrites them. */
    inline constexpr VkImageMemoryBarrier2 compute_storage_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief compute storage write -> transfer read (GENERAL -> GENERAL): an image written as a storage
     *         image is copied out of - the GI resolve becoming the next frame's history. The
     *         compute write has to be published to the transfer too, which is what the src masks say. */
    inline constexpr VkImageMemoryBarrier2 general_to_transfer_src_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief transfer read -> sampled read (GENERAL -> GENERAL): the same image, handed on to whatever
     *         samples it after the copy (the composite, for the GI resolve).
     * @note BOTH consumer stages are named, like every other transition that hands an image to a
     *       sampler (see shadow_map_sampling_transition). */
    inline constexpr VkImageMemoryBarrier2 transfer_src_to_sampling_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief attachment write -> sampled read (GENERAL -> GENERAL): the HDR scene target and the
     *         motion-vector / stored-surface targets into the passes that sample them)
     * @note BOTH consumer stages are named, as in shadow_map_sampling_transition: the motion-vector
     *       target is sampled by the GI denoiser's COMPUTE resolve as well as by the TAA fragment
     *       resolve, and a layout transition has to name every stage that reads the image afterwards. */
    inline constexpr VkImageMemoryBarrier2 hdr_sampling_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief UNDEFINED -> GENERAL for a DEPTH image: keep the shadow map's sampled
     *         descriptor valid on frames where the shadow pass does not run (shadows toggled off).
     *         pbr.frag always binds binding 8 and decides at runtime whether to sample it, and a
     *         descriptor must point at an image in the layout it declares - leaving the map in
     *         UNDEFINED made every such frame a VUID. Contents are irrelevant (the shader returns
     *         "fully lit"), so UNDEFINED as the old layout is correct.
     * @warning ONLY valid when the contents really are irrelevant. A depth image this command
     *          buffer just RENDERED and a later pass samples - the G-buffer depth, read by the
     *          deferred lighting stage, the TAA guard and the debug view - must go through
     *          shadow_map_sampling_transition instead: there the old layout is known to be
     *          GENERAL and the src masks publish the attachment write,
     *          so the sampled contents survive. UNDEFINED discards them. */
    inline constexpr VkImageMemoryBarrier2 undefined_to_depth_sampling_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    /** @brief UNDEFINED -> GENERAL: make a transient target readable without
     *         claiming a layout it may not be in (the bloom chain when the passes are skipped - the
     *         composite still samples those bindings statically, so the layout must be valid, but the
     *         contents are multiplied by zero; and a history image's very first use)
     * @note BOTH consumer stages are named, as everywhere else a transition hands an image to a
     *       sampler: the first frame of the GI history is read by a COMPUTE resolve. */
    inline constexpr VkImageMemoryBarrier2 undefined_to_sampling_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief sampled read -> transfer write (GENERAL -> GENERAL): a history image is overwritten with
     *         the newly resolved frame after the resolve sampled it
     * @note BOTH reading stages are named on the src side, for the same reason the sampling
     *       transitions name both on the dst side: the last reader of a history image before the copy
     *       is TAA's fragment resolve or the GI denoiser's compute one. */
    inline constexpr VkImageMemoryBarrier2 sampling_to_transfer_dst_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    }; /** @brief UNDEFINED -> GENERAL: the TAA history image receives the resolved frame
        *         through vkCmdCopyImage; its previous contents are irrelevant (the resolve only trusts a
        *         history it marked valid, and this transition is what starts a new one) */
    inline constexpr VkImageMemoryBarrier2 undefined_to_transfer_dst_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief transfer write -> sampled read (GENERAL -> GENERAL): a history image, written by the
     *         history copy at the end of a frame and sampled by the next frame's resolve
     * @note BOTH consumer stages are named (TAA's resolve is a fragment stage, the GI denoiser's is a
     *       compute one), because a layout transition has to name every stage that reads the image
     *       afterwards - see shadow_map_sampling_transition for the same note. */
    inline constexpr VkImageMemoryBarrier2 transfer_dst_to_sampling_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief attachment write -> transfer read (GENERAL -> GENERAL), screenshot read-back copy
     *         (vkCmdCopyImageToBuffer). Recorded INSIDE the frame's command buffer, while the
     *         swapchain image is still controlled by the app: after vkQueuePresentKHR the presentation
     *         engine takes it for display and transitioning it again violates the WSI rules. */
    inline constexpr VkImageMemoryBarrier2 color_attachment_to_transfer_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief transfer read -> attachment write (GENERAL -> GENERAL), hand the screenshotted image back
     *         to the frame so present_transition (GENERAL -> PRESENT_SRC) still applies */
    inline constexpr VkImageMemoryBarrier2 transfer_to_color_attachment_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief UNDEFINED -> PRESENT_SRC_KHR: present a frame whose post-process pass was skipped, so
     *         the swapchain image is still UNDEFINED (contents are undefined -
     *         this only exists to hand the WSI a validly-laid-out image instead of lying about the
     *         old layout, which is what present_transition assumes) */
    inline constexpr VkImageMemoryBarrier2 undefined_to_present_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /**
     * @brief GENERAL -> GENERAL, COLOR_ATTACHMENT_OUTPUT/COLOR_ATTACHMENT_WRITE -> HOST/HOST_READ (plan X4)
     *
     * THE ONE RECIPE THE ENGINE RECORDED BY HAND, and the reason the `image_use` census said "no HOST-access
     * masks (the two host-visible barrier sites stay in the escape bucket, runtime.probes.cppm / ray_tracing.cpp)"
     * - the census wording is quoted as it was written; those two sites use the contract's `host_read` roles
     * now and the escape is deleted in abi 43. The render's writes have to be visible to the HOST stage
     * before the implementation copies the image out
     * (`image::get_content()` and the descriptor-heap probes' read-back). It joins the table so the pair
     * (color_attachment, host_read) is a DECLARED one rather than a raw command somebody records.
     */
    inline constexpr VkImageMemoryBarrier2 color_attachment_to_host_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief GENERAL -> PRESENT_SRC_KHR (dynamic rendering has no finalLayout) */
    inline constexpr VkImageMemoryBarrier2 present_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
} // namespace deren::vulkan
