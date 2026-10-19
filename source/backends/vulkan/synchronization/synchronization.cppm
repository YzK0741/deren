module;
#include <cstdint>
#include <expected>
#include <limits>
#include <vulkan/vulkan.h>
export module deren.vulkan.synchronization;
export import deren.promise.rhi.synchronization;
export namespace deren::vulkan::synchronization {
    namespace contract = deren::promise::rhi::synchronization;
    /// Current backend policy: every ordinary abstract state uses GENERAL.
    constexpr std::expected<VkImageLayout, contract::error> map_layout(contract::image_layout layout) noexcept {
        switch (layout) {
        case contract::image_layout::undefined:
            return VK_IMAGE_LAYOUT_UNDEFINED;
        case contract::image_layout::present:
            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        case contract::image_layout::general:
        case contract::image_layout::shader_read_only:
        case contract::image_layout::color_attachment:
        case contract::image_layout::depth_stencil_attachment:
        case contract::image_layout::depth_stencil_read_only:
        case contract::image_layout::transfer_source:
        case contract::image_layout::transfer_destination:
            return VK_IMAGE_LAYOUT_GENERAL;
        default:
            return std::unexpected(contract::error::unsupported);
        }
    }
    constexpr std::expected<VkPipelineStageFlags2, contract::error> map_stages(contract::stage stages) noexcept {
        auto bits = static_cast<uint32_t>(stages);
        // RT stage 1: the ray-tracing pipeline stage bit (1024) is part of the vocabulary now, so the boundary is 2047.
        if (bits & ~2047U) {
            return std::unexpected(contract::error::unsupported);
        }
        VkPipelineStageFlags2 result = 0;
        if (bits & static_cast<uint32_t>(contract::stage::vertex_shader)) {
            result |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        }
        if (bits & static_cast<uint32_t>(contract::stage::fragment_shader)) {
            result |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        }
        if (bits & static_cast<uint32_t>(contract::stage::compute_shader)) {
            result |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        }
        // THE RAY-TRACING PIPELINE STAGES (RT stage 1/2b): the visibility image is written by a traceRays launch, and
        // the transition that carries that write names this stage. A ray query inside a compute shader stays 4U.
        if (bits & static_cast<uint32_t>(contract::stage::ray_tracing)) {
            result |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        }
        if (bits & static_cast<uint32_t>(contract::stage::color_output)) {
            result |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        }
        if (bits & static_cast<uint32_t>(contract::stage::depth_stencil)) {
            result |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        }
        if (bits & static_cast<uint32_t>(contract::stage::transfer)) {
            result |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        }
        if (bits & static_cast<uint32_t>(contract::stage::vertex_input)) {
            result |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        }
        if (bits & static_cast<uint32_t>(contract::stage::draw_indirect)) {
            result |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        }
        if (bits & static_cast<uint32_t>(contract::stage::task_shader)) {
            result |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT;
        }
        if (bits & static_cast<uint32_t>(contract::stage::mesh_shader)) {
            result |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
        }
        return result;
    }
    constexpr std::expected<VkAccessFlags2, contract::error> map_access(contract::access accesses) noexcept {
        auto bits = static_cast<uint32_t>(accesses);
        if (bits & ~3U) {
            return std::unexpected(contract::error::unsupported);
        }
        VkAccessFlags2 result = 0;
        if (bits & 1) {
            result |= VK_ACCESS_2_MEMORY_READ_BIT;
        }
        if (bits & 2) {
            result |= VK_ACCESS_2_MEMORY_WRITE_BIT;
        }
        return result;
    }
    namespace detail {
        constexpr bool valid_scope(contract::scope scope) noexcept {
            return scope.accesses == contract::access::none || scope.stages != contract::stage::none;
        }
        constexpr bool compatible(contract::image_layout layout, uint32_t aspects) noexcept {
            switch (layout) {
            case contract::image_layout::color_attachment:
            case contract::image_layout::present:
                return aspects == 1;
            case contract::image_layout::depth_stencil_attachment:
            case contract::image_layout::depth_stencil_read_only:
                return !(aspects & 1);
            default:
                return true;
            }
        }
    } // namespace detail
    /// Returns an unbound template. The binding/recorder must validate real objects and queue features.
    constexpr std::expected<VkImageMemoryBarrier2, contract::error> map_image(contract::image_transition const& input) noexcept {
        auto const& range = input.range;
        auto aspects = static_cast<uint32_t>(range.aspects);
        if (!range.mip_count || !range.layer_count || range.mip_count > UINT32_MAX - range.base_mip || range.layer_count > UINT32_MAX - range.base_layer || !aspects || (aspects & ~7U) || ((aspects & 1) && (aspects & 6))) {
            return std::unexpected(contract::error::invalid_argument);
        }
        if (range.mip_count == UINT32_MAX || range.layer_count == UINT32_MAX) {
            return std::unexpected(contract::error::unsupported); // Not a literal Vulkan count: REMAINING sentinel.
        }
        if (!detail::valid_scope(input.before.scope) || !detail::valid_scope(input.after.scope) || input.after.layout == contract::image_layout::undefined || !detail::compatible(input.before.layout, aspects) || !detail::compatible(input.after.layout, aspects)) {
            return std::unexpected(contract::error::invalid_argument);
        }
        if (input.before.layout == contract::image_layout::undefined && (input.before.scope.stages != contract::stage::none || input.before.scope.accesses != contract::access::none)) {
            return std::unexpected(contract::error::invalid_argument);
        }
        if (input.after.layout == contract::image_layout::present && (input.after.scope.stages != contract::stage::none || input.after.scope.accesses != contract::access::none)) {
            return std::unexpected(contract::error::invalid_argument);
        }
        if ((static_cast<uint32_t>(input.before.scope.stages) | static_cast<uint32_t>(input.after.scope.stages)) & (64U | 128U)) {
            return std::unexpected(contract::error::unsupported);
        }
        auto before = map_layout(input.before.layout), after = map_layout(input.after.layout);
        auto source = map_stages(input.before.scope.stages), destination = map_stages(input.after.scope.stages);
        auto source_access = map_access(input.before.scope.accesses), destination_access = map_access(input.after.scope.accesses);
        if (!before || !after || !source || !destination || !source_access || !destination_access) {
            return std::unexpected(contract::error::unsupported);
        }
        VkImageMemoryBarrier2 result{};
        result.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        result.srcStageMask = *source;
        result.dstStageMask = *destination;
        result.srcAccessMask = *source_access;
        result.dstAccessMask = *destination_access;
        result.oldLayout = *before;
        result.newLayout = *after;
        result.srcQueueFamilyIndex = result.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        VkImageAspectFlags native_aspects = 0;
        if (aspects & 1) {
            native_aspects |= VK_IMAGE_ASPECT_COLOR_BIT;
        }
        if (aspects & 2) {
            native_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if (aspects & 4) {
            native_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        result.subresourceRange = {native_aspects, range.base_mip, range.mip_count, range.base_layer, range.layer_count};
        return result;
    }
    constexpr std::expected<VkBufferMemoryBarrier2, contract::error> map_buffer(contract::buffer_transition const& input) noexcept {
        if (!input.size || input.size > UINT64_MAX - input.offset || !detail::valid_scope(input.before) || !detail::valid_scope(input.after)) {
            return std::unexpected(contract::error::invalid_argument);
        }
        if (input.size == UINT64_MAX || ((static_cast<uint32_t>(input.before.stages) | static_cast<uint32_t>(input.after.stages)) & (8U | 16U))) {
            return std::unexpected(contract::error::unsupported);
        }
        auto source = map_stages(input.before.stages), destination = map_stages(input.after.stages);
        auto source_access = map_access(input.before.accesses), destination_access = map_access(input.after.accesses);
        if (!source || !destination || !source_access || !destination_access) {
            return std::unexpected(contract::error::unsupported);
        }
        VkBufferMemoryBarrier2 result{};
        result.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        result.srcStageMask = *source;
        result.dstStageMask = *destination;
        result.srcAccessMask = *source_access;
        result.dstAccessMask = *destination_access;
        result.srcQueueFamilyIndex = result.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        result.offset = input.offset;
        result.size = input.size;
        return result;
    }
} // namespace deren::vulkan::synchronization
