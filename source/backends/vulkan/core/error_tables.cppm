// ============================================================================
// module: deren.vulkan.error_tables
// module version: 0.1.0a
//
// THE ERROR MECHANISM'S TRANSLATION TABLES, IN A MODULE OF THEIR OWN - which is what makes them
// reachable from BOTH halves of the RHI boundary now that the backend is a DLL (the SHARED flip).
//
// WHY THEY LEFT `deren.vulkan.core` (measured, not tidiness): until the flip these were `export`
// declarations of the backend's module with definitions in one of its implementation units. A module
// attachment is a LINK-TIME fact: a consumer that imports the module needs that module's initializer
// and its decorated symbols, and a DLL can serve those only by exporting its whole module surface -
// while `deren_vulkan.dll` exports EXACTLY ONE name by design (source/promise/rhi/backend_entry.hpp). The
// tables' unit test (source/tests/test_error_mapping.cpp) legitimately wants the REAL translators, and the
// backend itself calls them (core.api_core.cpp, core.cpp), so the shape that serves both is the one
// this repository already uses for `vulkan_constant_init`: a NEUTRAL MODULE OWNED BY A TARGET BOTH
// SIDES LINK. Neither side has to be a DLL consumer of the other.
//
// WHY A MODULE AND NOT A HEADER WITH THE SAME DECLARATIONS: the translators take `VkResult`, and
// `#include <vulkan/vulkan.h>` cannot appear in a module purview (libc++ and the Vulkan headers then
// collide with the global module: "declaration ... follows declaration in the global module"), while
// after the imports it is exactly where it would have to go. A module interface unit puts that
// include where it belongs - the global module fragment.
//
// WHAT IS HERE: the three per-call-site translators and the diagnostic builder (`failed`). The
// mappings' caller-action rationale lives on the enum values themselves
// (source/promise/rhi/rhi.contract.cppm); this module only decides WHICH VkResult is WHICH action at WHICH
// call site. The rationale for the per-call-site split, in one line: the SAME VkResult means
// different things depending on where it landed - VK_SUBOPTIMAL_KHR is "carry on" at acquire and
// "rebuild" at present - and folding that into the enum or into a single map would move the call
// site's knowledge into the type system's way.
//
// WHAT IS DELIBERATELY NOT HERE: a code table of all 41 VkResults (23 of them in extension ranges), a
// trimmed header, or a generated .inc. Almost none of those codes can reach this backend's call
// sites; every unnamed code falls to `operation_failed` and keeps its raw value for the caller (the
// producer side attaches it as error_info::native_code). The tables name exactly the codes this
// backend can plausibly receive; the four headers of the fall-through are the contract's own four
// zones.
// ============================================================================
module;

#include <cstdint>
#include <source_location>
#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.error_tables;

export import deren.promise.rhi;

namespace deren::vulkan {
    /**
     * @brief build the contract's diagnostic for one failure, at the point that FAILED
     * @param code the decision the caller acts on
     * @param native_code the raw VkResult, SIGNED (Vulkan's codes are negative)
     * @param message the backend's static text (a literal; it outlives the call because the backend is
     *        never unloaded - invariant 4)
     * @param where just use the default argument: it captures THIS call site, which is the backend's
     *        failure point - the reason this helper exists instead of callers filling `error_info{}`
     *        themselves, and the reason a contract virtual must never take a default `where` (a default
     *        argument evaluates at the CALL site, which would name the engine)
     * @return the filled error_info (trivially copyable; api is this backend's graphics_api::vulkan)
     */
    export [[nodiscard]] constexpr deren::promise::rhi::error_info failed(deren::promise::rhi::error const code,
                                                                          std::int32_t const native_code = 0,
                                                                          std::string_view const message = {},
                                                                          std::source_location const where = std::source_location::current()) noexcept {
        return {.code = code, .api = deren::promise::rhi::graphics_api::vulkan, .native_code = native_code, .message = message, .where = where};
    }

    /// The rows the three tables share, in the order the contract's zones read: the suboptimal
    /// exception is the CALLER's row (each table's first line), everything below is common.
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

    /// Translate a raw VkResult at the ACQUIRE call site (vkAcquireNextImageKHR). SUBOPTIMAL is a
    /// state here, not a failure: the acquired image renders fine, so it translates to ok - the same
    /// VkResult translates to out_of_date at the PRESENT site, which is exactly why the translation is
    /// per call site rather than one global function.
    export [[nodiscard]] deren::promise::rhi::error acquire_error(VkResult const result) noexcept {
        switch (result) {
        case VK_ERROR_OUT_OF_DATE_KHR:
            return deren::promise::rhi::error::out_of_date;
        case VK_SUBOPTIMAL_KHR:
            return deren::promise::rhi::error::ok; // the acquired image renders
        default:
            return shared_error(result);
        }
    }

    /// Translate a raw VkResult at the PRESENT call site (vkQueuePresentKHR). SUBOPTIMAL means the
    /// presentation still showed but the surface is one resize from gone: out_of_date, so the caller
    /// rebuilds - acquire must NOT inherit this reading, and present must NOT inherit acquire's.
    export [[nodiscard]] deren::promise::rhi::error present_error(VkResult const result) noexcept {
        switch (result) {
        case VK_ERROR_OUT_OF_DATE_KHR:
        case VK_SUBOPTIMAL_KHR:
            return deren::promise::rhi::error::out_of_date; // presentable, but rebuild
        default:
            return shared_error(result);
        }
    }

    /// Translate a raw VkResult everywhere else (creation, queries, submission): this site has no
    /// swapchain-shaped knowledge, so neither SUBOPTIMAL (a state the acquire/present sites each read
    /// their own way) nor OUT_OF_DATE is reinterpreted here - both fall through with everything else
    /// the tables do not name, keeping the raw value for the caller. INITIALIZATION_FAILED and
    /// INCOMPATIBLE_DRIVER are creation-shaped failures, hence initialization_failed; the
    /// *_NOT_PRESENT family and FORMAT_NOT_SUPPORTED are the unsupported mechanism's own codes.
    export [[nodiscard]] deren::promise::rhi::error generic_error(VkResult const result) noexcept {
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
