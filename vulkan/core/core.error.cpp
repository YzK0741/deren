// ============================================================================
// file: vulkan/core/core.error.cpp
//
// THE ERROR MECHANISM'S TRANSLATION TABLES (the error-mechanism batch, DYNAMIC_LINK
// handoff §4.5): VkResult -> contract `error`, ONCE per call-site family instead of
// one global function, because the SAME VkResult means different things depending on
// where it landed - VK_SUBOPTIMAL_KHR is "carry on" at acquire and "rebuild" at
// present, and folding that decision into the enum or into a single map would move
// the call site's knowledge into the type system's way.
//
// WHAT IS DELIBERATELY NOT HERE: a code table of all 41 VkResults (23 of them in
// extension ranges), a trimmed header, or a generated .inc. Measured reality is that
// almost none of those codes can reach this backend's call sites; every unnamed code
// falls to `operation_failed` and keeps its raw value for the caller (the producer
// side attaches it as error_info::native_code). The tables name exactly the codes
// this backend can plausibly receive; the four headers of the fall-through are the
// contract's own four zones.
//
// The mappings' caller-action rationale lives on the enum values themselves
// (promise/rhi/rhi.contract.cppm); this file only decides WHICH VkResult is WHICH
// action at WHICH call site. The batch lands the tables and their unit test; the
// call sites adopt them as their error channels appear (the frame face: acquire,
// present and wait gain contract-borne error paths there), which is why nothing in
// this translation unit is called from this backend yet - the tables are the
// mechanism, the frame face is the wiring.
// ============================================================================
module;

#include <vulkan/vulkan.h>

module deren.vulkan.core;

namespace deren::vulkan {

    // The rows the three tables share, in the order the contract's zones read: the suboptimal
    // exception is the CALLER's row (each table's first line), everything below is common.
    [[nodiscard]] static deren::promise::rhi::error shared_error(VkResult const result) noexcept {
        switch (result) {
        case VK_SUCCESS:
            return deren::promise::rhi::error::ok;
        case VK_TIMEOUT:
            return deren::promise::rhi::error::timeout;
        case VK_ERROR_SURFACE_LOST_KHR:
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
            return deren::promise::rhi::error::surface_lost;
        case VK_ERROR_DEVICE_LOST:
            return deren::promise::rhi::error::device_lost;
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        case VK_ERROR_OUT_OF_POOL_MEMORY:
        case VK_ERROR_FRAGMENTED_POOL:
            return deren::promise::rhi::error::out_of_device_memory;
        case VK_ERROR_OUT_OF_HOST_MEMORY:
        case VK_ERROR_MEMORY_MAP_FAILED:
            return deren::promise::rhi::error::out_of_host_memory;
        default:
            return deren::promise::rhi::error::operation_failed;
        }
    }

    deren::promise::rhi::error acquire_error(VkResult const result) noexcept {
        switch (result) {
        case VK_ERROR_OUT_OF_DATE_KHR:
            return deren::promise::rhi::error::out_of_date;
        case VK_SUBOPTIMAL_KHR:
            return deren::promise::rhi::error::ok; // the acquired image renders
        default:
            return shared_error(result);
        }
    }

    deren::promise::rhi::error present_error(VkResult const result) noexcept {
        switch (result) {
        case VK_ERROR_OUT_OF_DATE_KHR:
        case VK_SUBOPTIMAL_KHR:
            return deren::promise::rhi::error::out_of_date; // presentable, but rebuild
        default:
            return shared_error(result);
        }
    }

    deren::promise::rhi::error generic_error(VkResult const result) noexcept {
        switch (result) {
        case VK_ERROR_INITIALIZATION_FAILED:
        case VK_ERROR_INCOMPATIBLE_DRIVER:
            return deren::promise::rhi::error::initialization_failed;
        case VK_ERROR_EXTENSION_NOT_PRESENT:
        case VK_ERROR_FEATURE_NOT_PRESENT:
        case VK_ERROR_LAYER_NOT_PRESENT:
        case VK_ERROR_FORMAT_NOT_SUPPORTED:
            return deren::promise::rhi::error::unsupported;
        default:
            return shared_error(result);
        }
    }

} // namespace deren::vulkan
