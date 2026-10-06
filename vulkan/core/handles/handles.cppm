module;

#include <vulkan/vulkan.h>

export module deren.vulkan.core.handles;

export import deren.vstd;

/**
 * @defgroup vulkan_handles Vulkan Main Handles' RAII Wrapper
 * @file handles.cppm
 */
namespace deren::vulkan {
    /**
     * @ingroup vulkan_handles
     * @brief raii wrapper of VkCommandBuffer - and of the VkCommandPool it was allocated from
     * @note
     *     - use operator* or get() to get naked handle
     *     - SOLE OWNERSHIP, OF BOTH: the wrapper owns the command buffer and, on the one path that
     *       creates it (make_command_buffer below), THE POOL TOO. release() destroys the pool, which
     *       frees its buffers with it - so a command buffer is ONE self-contained device resource and
     *       the caller has no second object to keep alive. A pool per command buffer is the canonical
     *       Vulkan shape for per-slot / per-thread recording, because a VkCommandPool is not thread
     *       safe and every recording thread must own its own.
     *     - THE POOL CARRIES VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT (constant_init's
     *       make_command_pool_info), which is what lets vkBeginCommandBuffer implicitly reset a buffer
     *       that has already been recorded - the frame loop re-records the same primary every frame.
     *     - the pool is reachable only through this wrapper: nothing outside owns or destroys it
     */
    export class vk_command_buffer {
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkCommandPool command_pool = VK_NULL_HANDLE;
        /// whether THIS wrapper created command_pool and must destroy it (see make_command_buffer)
        bool owns_pool = false;

    public:
        [[nodiscard]] VkCommandBuffer const& get() const noexcept;
        [[nodiscard]] VkCommandBuffer const& operator*() const noexcept;
        void release() noexcept;
        explicit vk_command_buffer(VkCommandBuffer command_buffer, VkDevice device, VkCommandPool pool, bool owns_pool) noexcept;
        ~vk_command_buffer() noexcept;

        explicit vk_command_buffer(vk_command_buffer& command_buffer) = delete;
        vk_command_buffer(vk_command_buffer&& other) noexcept;
        vk_command_buffer& operator=(vk_command_buffer& other) = delete;
        vk_command_buffer& operator=(vk_command_buffer&& other) noexcept;
    };

    /**
     * @ingroup vulkan_handles
     * @param device valid VkDevice
     * @param queue_family the queue family the buffer's own command pool belongs to
     * @param level VK_COMMAND_BUFFER_LEVEL_PRIMARY or _SECONDARY
     * @return raii VkCommandBuffer wrapper that OWNS its command pool: it creates the pool, allocates
     *         the buffer from it, and `release()`/destruction destroys the pool (freeing the buffer)
     */
    export vk_command_buffer make_command_buffer(VkDevice device, uint32_t queue_family, VkCommandBufferLevel level) noexcept;

    /**
     * @ingroup vulkan_handles
     * @brief raii wrapper of VkShaderModule
     * @note
     *     - use operator* or get() to get naked handle
     *     - sole ownership
     */
    export class vk_shader_module {
        VkShaderModule shader_module = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;

    public:
        [[nodiscard]] VkShaderModule const& get() const noexcept;
        [[nodiscard]] VkShaderModule const& operator*() const noexcept;
        void release();
        explicit vk_shader_module(VkShaderModule shader_module, VkDevice device) noexcept;
        ~vk_shader_module() noexcept;

        explicit vk_shader_module(vk_shader_module& shader_module) = delete;
        vk_shader_module(vk_shader_module&& other) noexcept;
        vk_shader_module& operator=(vk_shader_module& other) = delete;
        vk_shader_module& operator=(vk_shader_module&& other) noexcept;
    };

    /**
     * @ingroup vulkan_handles
     * @param shader binary shader code
     * @param device valid VkDevice
     * @return success: raii wrapper of VkShaderModule
     *     fail: std::nullopt
     */
    export std::optional<vk_shader_module> make_shader_module(std::span<uint8_t const> shader,
                                                              VkDevice device) noexcept;

    /**
     * @ingroup vulkan_handles
     * @brief raii wrapper of VkPipeline
     * @note
     *     - use operator* or get() to get naked handle
     *     - sole ownership
     *     - viewport/scissor hold the full-screen viewport for this pipeline;
     *       set at creation by core::make_pipeline, use them with vkCmdSetViewport/Scissor
     */
    export struct vk_pipeline {
        VkPipeline pipeline = VK_NULL_HANDLE;
        // every pipeline is heap-native and is created with VK_NULL_HANDLE as its layout (the heap flag
        // requires it), so no pipeline layout object exists to keep here
        VkDevice device = VK_NULL_HANDLE;

        // Per-pipeline viewport/scissor (dynamic state; vkCmdSetViewport/Scissor required before drawing)
        VkViewport viewport = {};
        VkRect2D scissor = {};

        explicit vk_pipeline(VkPipeline pipeline, VkDevice device) noexcept;
        ~vk_pipeline();
        [[nodiscard]] VkPipeline get_pipeline() const noexcept;

        /**
         * @ingroup vulkan_handles
         * @brief bind the pipeline and set the stored viewport/scissor (dynamic states)
         * @param command_buffer the command buffer being recorded
         */
        void begin_pipeline(VkCommandBuffer command_buffer) const;

        vk_pipeline(vk_pipeline&) = delete;
        vk_pipeline(vk_pipeline&& other) noexcept;
        vk_pipeline& operator=(vk_pipeline& other) = delete;
        vk_pipeline& operator=(vk_pipeline&& other) noexcept;
        void release() noexcept;
    };

    /**
     * @ingroup vulkan_handles
     * @brief raii wrapper of VkImageView
     * @note
     *     - use operator* or get() to get naked handle
     *     - sole ownership
     */
    export class vk_image_view {
        VkImageView image_view = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;

    public:
        vk_image_view() noexcept = default;
        [[nodiscard]] VkImageView const& get() const noexcept;
        [[nodiscard]] VkImageView const& operator*() const noexcept;
        void release() noexcept;
        explicit vk_image_view(VkImageView image_view, VkDevice device) noexcept;
        ~vk_image_view() noexcept;

        explicit vk_image_view(vk_image_view&) = delete;
        vk_image_view(vk_image_view&& other) noexcept;
        vk_image_view& operator=(vk_image_view&) = delete;
        vk_image_view& operator=(vk_image_view&& other) noexcept;
    };

    /**
     * @ingroup vulkan_handles
     * @brief raii wrapper of VkSampler
     * @note
     *     - use operator* or get() to get naked handle
     *     - sole ownership
     */
    export class vk_sampler {
        VkSampler sampler = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;

    public:
        vk_sampler() noexcept = default;
        [[nodiscard]] VkSampler const& get() const noexcept;
        [[nodiscard]] VkSampler const& operator*() const noexcept;
        void release() noexcept;
        explicit vk_sampler(VkSampler sampler, VkDevice device) noexcept;
        ~vk_sampler() noexcept;

        explicit vk_sampler(vk_sampler&) = delete;
        vk_sampler(vk_sampler&& other) noexcept;
        vk_sampler& operator=(vk_sampler&) = delete;
        vk_sampler& operator=(vk_sampler&& other) noexcept;
    };
} // namespace deren::vulkan
