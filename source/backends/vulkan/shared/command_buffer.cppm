// -*- C++ -*-
// ============================================================================
// module: deren.vulkan.command_buffer
//
// THE VULKAN CLASS A CONTRACT `command_buffer` IS, for the backend and the GUI plugin.
//
// The contract passes the RHI type (`rhi::command_buffer`); this is the class the Vulkan side casts it to
// after checking the concrete tag, and `get()` is how the native handle is read:
//
//     if (deren::vulkan::is_command_buffer(commands)) {
//         VkCommandBuffer native = static_cast<deren::vulkan::command_buffer const&>(commands).get();
//     }
//
// WHY THE CAST NEEDS A SHARED DECLARATION AT ALL: the backend DLL exports exactly one C name, so a module
// attachment cannot be served from it - both images compile this interface unit instead (the same shape
// `denen.vulkan.main_context` and the other shared layout units use). The class is deliberately the
// SMALLEST possible: one private native handle, `get()`, and the backend-side setter. No ownership flag,
// no borrowed/owned kind, no conversion callback - the backend's two concrete command-buffer classes
// derive from it and keep the handle current.
// ============================================================================
module;

#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.command_buffer;

export import deren.promise.rhi;

/**
 * @file source/backends/vulkan/shared/command_buffer.cppm
 * @brief the Vulkan classes behind the contract's `command_buffer` and `mergeable_command_buffer`
 *        (abi 43): tag checks, casts, `get()`
 * @ingroup promise
 */

export namespace deren::vulkan {

    /// The concrete identity the backend constructs the BORROWED frame view with.
    inline constexpr std::string_view frame_command_buffer_name = "deren_frame_commands_vulkan";
    /// The concrete identity the backend constructs a caller-OWNED recording buffer with.
    inline constexpr std::string_view owned_command_buffer_name = "deren_command_buffer_vulkan";

    /// THE VULKAN CLASS: a contract `command_buffer` the Vulkan side casts to after checking the concrete
    /// tag. `get()` is the native handle; `set_target()` is the backend's own way of keeping it current
    /// (a frame slot's buffer while a frame is in flight, an owned buffer's own handle once allocated).
    struct command_buffer : deren::promise::rhi::command_buffer {
        explicit command_buffer(std::string_view const name) noexcept
            : deren::promise::rhi::command_buffer(name) {
        }

        /// @brief the native recording target (VK_NULL_HANDLE = nothing to record into right now)
        [[nodiscard]] VkCommandBuffer get() const noexcept {
            return this->native;
        }

        /// @brief backend only: publish the current recording target
        void set_target(VkCommandBuffer const target) noexcept {
            this->native = target;
        }

    private:
        VkCommandBuffer native = VK_NULL_HANDLE;
    };

    /// THE VULKAN CLASS A CONTRACT `mergeable_command_buffer` IS. The GUI overlay records its own ImGui draw
    /// data into one of these, and `get()` is how the recording target is read - the same opaque-handle +
    /// concrete-tag + shared-layout rule the primary `command_buffer` class follows. The backend keeps the
    /// native handle current (a mergeable's target is the secondary buffer it was allocated with).
    struct mergeable_command_buffer : deren::promise::rhi::mergeable_command_buffer {
        explicit mergeable_command_buffer(std::string_view const name) noexcept
            : deren::promise::rhi::mergeable_command_buffer(name) {
        }

        /// @brief the native recording target (VK_NULL_HANDLE = not allocated / released)
        [[nodiscard]] VkCommandBuffer get() const noexcept {
            return this->native;
        }

        /// @brief backend only: publish the recording target
        void set_target(VkCommandBuffer const target) noexcept {
            this->native = target;
        }

    private:
        VkCommandBuffer native = VK_NULL_HANDLE;
    };

    /// The concrete identity the backend constructs a mergeable recording target with.
    inline constexpr std::string_view mergeable_command_buffer_name = "deren_mergeable_command_buffer_vulkan";

    /// @brief whether @p commands is one of THIS backend's mergeable recording targets
    [[nodiscard]] inline bool is_mergeable_command_buffer(deren::promise::rhi::mergeable_command_buffer const& commands) noexcept {
        return deren::promise::rhi::is_concrete(commands, mergeable_command_buffer_name);
    }

    /// @brief whether @p commands is one of THIS backend's command buffers (the cast's precondition)
    [[nodiscard]] inline bool is_command_buffer(deren::promise::rhi::command_buffer const& commands) noexcept {
        return deren::promise::rhi::is_concrete(commands, frame_command_buffer_name) ||
               deren::promise::rhi::is_concrete(commands, owned_command_buffer_name);
    }

} // namespace deren::vulkan
