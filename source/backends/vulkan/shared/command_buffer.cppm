// -*- C++ -*-
// ============================================================================
// module: deren.vulkan.command_buffer
//
// THE SHARED CONCRETE HEAD OF A CONTRACT command_buffer THAT THIS BACKEND HANDS OUT.
//
// WHY IT EXISTS: the GUI plugin's Dear ImGui Vulkan backend records its own draw data and its API takes
// a `VkCommandBuffer`, but the contract must not name that type. So the contract passes the recording
// target as the RHI handle it already is (`rhi::command_buffer`), and the plugin - which IS the Vulkan
// plugin, it knows this API's vocabulary - checks the CONCRETE TAG and casts the handle to this shared
// layout to read the native target. The same "opaque contract handle + concrete tag + shared layout cast"
// rule `rhi::main_context` follows (abi 39); no conversion callback and no native type crosses the
// contract.
//
// BOTH concrete command-buffer forms derive from this base: the borrowed frame view (`frame_commands`)
// and an owned recording buffer (`owned_command_buffer`, which derives from the frame view). The BACKEND
// keeps `recording_target` in step (a frame slot's buffer while a frame is in flight, the owned buffer's
// own handle once it is allocated, VK_NULL_HANDLE when there is nothing to record into). Everything else
// here is declaration only - the recording logic stays in the backend DLL.
// ============================================================================
module;

#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.command_buffer;

export import deren.promise.rhi;

/**
 * @file source/backends/vulkan/shared/command_buffer.cppm
 * @brief the shared concrete base of the contract's `command_buffer` for the Vulkan backend (abi 43)
 * @ingroup promise
 */

export namespace deren::vulkan {

    /// The concrete identity the backend constructs the BORROWED frame view with.
    inline constexpr std::string_view frame_command_buffer_name = "deren_frame_commands_vulkan";
    /// The concrete identity the backend constructs a caller-OWNED recording buffer with.
    inline constexpr std::string_view owned_command_buffer_name = "deren_command_buffer_vulkan";

    /// THE SHARED CONCRETE HEAD: one native field, kept current by the backend, readable by any image that
    /// compiles this layout after checking the concrete tag. Abstract: every contract virtual stays the
    /// backend's own implementation.
    struct command_buffer_native : deren::promise::rhi::command_buffer {
        explicit command_buffer_native(std::string_view const name) noexcept
            : deren::promise::rhi::command_buffer(name) {
        }

        /// the recording target, as Vulkan spells it; VK_NULL_HANDLE = this handle names no open recording
        VkCommandBuffer recording_target = VK_NULL_HANDLE;
    };

    /// @brief whether @p commands is a command buffer THIS backend's shared layout describes
    [[nodiscard]] inline bool is_command_buffer(deren::promise::rhi::command_buffer const& commands) noexcept {
        return deren::promise::rhi::is_concrete(commands, frame_command_buffer_name) ||
               deren::promise::rhi::is_concrete(commands, owned_command_buffer_name);
    }

    /// @brief the recording target of a command buffer this backend handed out, or VK_NULL_HANDLE for a
    ///        foreign handle or a handle that names no open recording (the borrowed frame view outside a
    ///        frame in flight). The tag check is the cast's precondition - the same rule `is_main_context`
    ///        + `do_concrete` follow.
    [[nodiscard]] inline VkCommandBuffer native_command_buffer(deren::promise::rhi::command_buffer& commands) noexcept {
        if (!is_command_buffer(commands)) {
            return VK_NULL_HANDLE;
        }
        return static_cast<command_buffer_native&>(commands).recording_target;
    }

} // namespace deren::vulkan
