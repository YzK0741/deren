// -*- C++ -*-
// ============================================================================
// module: deren.vulkan.main_context
//
// THE CONCRETE LAYOUT BEHIND `rhi::main_context` FOR THIS BACKEND (abi 39).
//
// The contract's handle is opaque on purpose, so the layout it names has to live somewhere BOTH halves
// that touch it compile: `deren_vulkan.dll` creates and fills it, `deren_gui_vulkan.dll` casts it back.
// A STATIC library carrying this interface unit is the shape this repository already uses for shared
// backend vocabulary (`vulkan_constant_init`, `renderer_layout`): the backend's DLL exports exactly one
// name, so a module attachment cannot be served from it - both images compile the same interface unit
// instead. The pair (backend, plugin) is versioned together by `rhi::abi_version`, which both sides already
// compare at their own handshakes.
//
// WHAT IT CARRIES: exactly what the GUI plugin's ImGui Vulkan backend needs to come up - the RHI core
// (for the version handshake) and the native handles/numbers that core already owns. Nothing here is
// contract vocabulary. The per-frame RECORDING TARGET is not a callback here: the contract passes the RHI
// `command_buffer` itself, and the plugin casts it to the shared `denen.vulkan.command_buffer` head after
// checking its concrete tag (the same opaque-handle + tag + shared-layout rule).
// ============================================================================
module;

#include <cstdint>
#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.main_context;

export import deren.promise.rhi;

/**
 * @file source/backends/vulkan/main_context/main_context.cppm
 * @brief the concrete Vulkan layout of the contract's opaque `rhi::main_context` (abi 39)
 * @ingroup promise
 */

export namespace deren::vulkan {

    /// The concrete identity string the backend constructs its context with and the GUI plugin checks.
    inline constexpr std::string_view main_context_name = "deren_main_context_vulkan";

    /// THE MAIN-THREAD CONTEXT, as this backend hands it out: everything the GUI plugin's Vulkan init needs,
    /// plus the RHI core the overlay keeps for the API-version handshake.
    struct main_context final : deren::promise::rhi::main_context {
        explicit main_context() noexcept
            : deren::promise::rhi::main_context(main_context_name) {
        }

        /// borrowed: the backend owns the RHI root and the overlay must keep it alive (it does - the host
        /// owns both for the session, and the overlay is released before the runtime)
        deren::promise::rhi::api_core* core = nullptr;
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue graphics_queue = VK_NULL_HANDLE;
        std::uint32_t graphics_queue_family = 0;
        /// the presentation surface's format, as the backend resolved it (a VkFormat value)
        std::uint32_t color_format = 0;
        std::uint32_t frames_in_flight = 0;
    };

    /// @brief whether @p context is THIS backend's main context (the contract's provenance question)
    [[nodiscard]] inline bool is_main_context(deren::promise::rhi::main_context const& context) noexcept {
        return deren::promise::rhi::is_concrete(context, main_context_name);
    }

} // namespace deren::vulkan
