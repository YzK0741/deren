module;

export module deren.vulkan.core:vma_handles;

import deren.vstd;

/**
 * @defgroup vulkan_vma_handles Vulkan VMA Handles' RAII Wrapper
 * @file vma_handles.cppm
 * @brief RAII wrappers for GPU buffers / images allocated through vma_allocator
 *
 * The allocator hands out raw uint64 handles today, which makes the free rule implicit: every
 * call site must remember to free exactly once, and shared (deduplicated) images rely on a
 * count nobody can see. These wrappers make the rule structural:
 *   - copying a vk_buffer / vk_image shares the underlying resource (the allocator's
 *     count is incremented through the injected add_use callback);
 *   - destroying one releases exactly one share (the release callback; the allocator
 *     really destroys the GPU object when the count reaches zero).
 * The add_use/release callbacks are injected by vma_allocator when it creates the wrapper
 * (see deren.vulkan.core:vma), which lets the allocator keep `free` and `add_use` private - there is no
 * public raw-handle release path left to misuse.
 *
 * vk_buffer / vk_image deliberately know nothing about vma_allocator (they only store the
 * handle + the two callbacks), so this module does not depend on vulkan.core:vma.
 */
namespace deren::vulkan {
    /**
     * @ingroup vulkan_vma_handles
     * @brief a wrapper for one GPU buffer allocated through vma_allocator
     * @note copy = share (the allocator's count goes up by one), destroy = release (the count goes
     *       down by one); default-constructed / moved-from wrappers are empty
     */
    export class vk_buffer {
    public:
        vk_buffer() noexcept = default;

        /**
         * @brief adopt one share of @p handle; @p add_use / @p release adjust the
         *        allocator's count (injected by vma_allocator::create_buffer)
         */
        explicit vk_buffer(std::uint64_t handle,
                           std::function<void(std::uint64_t)> add_use,
                           std::function<void(std::uint64_t)> release) noexcept
            : native_handle{handle}
            , add_use_fn{std::move(add_use)}
            , release_fn{std::move(release)} {
        }

        ~vk_buffer() noexcept {
            this->release();
        }

        vk_buffer(vk_buffer const& other)
            : native_handle{other.native_handle}
            , add_use_fn{other.add_use_fn}
            , release_fn{other.release_fn} {
            if (this->native_handle != 0 && this->add_use_fn) {
                this->add_use_fn(this->native_handle); // share: raise the allocator's count
            }
        }

        vk_buffer& operator=(vk_buffer const& other) {
            if (this != &other) {
                this->release();
                this->native_handle = other.native_handle;
                this->add_use_fn = other.add_use_fn;
                this->release_fn = other.release_fn;
                if (this->native_handle != 0 && this->add_use_fn) {
                    this->add_use_fn(this->native_handle);
                }
            }
            return *this;
        }

        vk_buffer(vk_buffer&& other) noexcept
            : native_handle{other.native_handle}
            , add_use_fn{std::move(other.add_use_fn)}
            , release_fn{std::move(other.release_fn)} {
            other.native_handle = 0; // moved-from is empty
        }

        vk_buffer& operator=(vk_buffer&& other) noexcept {
            if (this != &other) {
                this->release();
                this->native_handle = other.native_handle;
                this->add_use_fn = std::move(other.add_use_fn);
                this->release_fn = std::move(other.release_fn);
                other.native_handle = 0;
            }
            return *this;
        }

        /** @brief the underlying allocator handle (0 = empty) */
        [[nodiscard]] std::uint64_t handle() const noexcept {
            return this->native_handle;
        }

        /** @brief whether this wrapper is non-empty */
        [[nodiscard]] bool valid() const noexcept {
            return this->native_handle != 0;
        }

        /** @brief give up this share and become empty (idempotent) */
        void reset() noexcept {
            this->release();
        }

    private:
        void release() noexcept {
            if (this->native_handle != 0 && this->release_fn) {
                this->release_fn(this->native_handle);
            }
            this->native_handle = 0;
            this->add_use_fn = {};
            this->release_fn = {};
        }

        // called native_handle, not handle: the handle() accessor of this class would collide with a member of that name
        // called add_use_fn, not add_use: the add_use parameter of the constructor would hide a member of that name and
        // MSVC /W4 reports C4458, an error under /WX
        // called release_fn, not release: the release() method of this class would collide with a member of that name
        std::uint64_t native_handle = 0;
        std::function<void(std::uint64_t)> add_use_fn = {};
        std::function<void(std::uint64_t)> release_fn = {};
    };

    /**
     * @ingroup vulkan_vma_handles
     * @brief a wrapper for one GPU image allocated through vma_allocator
     * @note same copy and destroy rules as vk_buffer: copy = share, destroy = release
     */
    export class vk_image {
    public:
        vk_image() noexcept = default;

        explicit vk_image(std::uint64_t handle,
                          std::function<void(std::uint64_t)> add_use,
                          std::function<void(std::uint64_t)> release) noexcept
            : native_handle{handle}
            , add_use_fn{std::move(add_use)}
            , release_fn{std::move(release)} {
        }

        ~vk_image() noexcept {
            this->release();
        }

        vk_image(vk_image const& other)
            : native_handle{other.native_handle}
            , add_use_fn{other.add_use_fn}
            , release_fn{other.release_fn} {
            if (this->native_handle != 0 && this->add_use_fn) {
                this->add_use_fn(this->native_handle);
            }
        }

        vk_image& operator=(vk_image const& other) {
            if (this != &other) {
                this->release();
                this->native_handle = other.native_handle;
                this->add_use_fn = other.add_use_fn;
                this->release_fn = other.release_fn;
                if (this->native_handle != 0 && this->add_use_fn) {
                    this->add_use_fn(this->native_handle);
                }
            }
            return *this;
        }

        vk_image(vk_image&& other) noexcept
            : native_handle{other.native_handle}
            , add_use_fn{std::move(other.add_use_fn)}
            , release_fn{std::move(other.release_fn)} {
            other.native_handle = 0;
        }

        vk_image& operator=(vk_image&& other) noexcept {
            if (this != &other) {
                this->release();
                this->native_handle = other.native_handle;
                this->add_use_fn = std::move(other.add_use_fn);
                this->release_fn = std::move(other.release_fn);
                other.native_handle = 0;
            }
            return *this;
        }

        /** @brief the underlying allocator handle (0 = empty) */
        [[nodiscard]] std::uint64_t handle() const noexcept {
            return this->native_handle;
        }

        /** @brief whether this wrapper is non-empty */
        [[nodiscard]] bool valid() const noexcept {
            return this->native_handle != 0;
        }

        /** @brief give up this share and become empty (idempotent) */
        void reset() noexcept {
            this->release();
        }

    private:
        void release() noexcept {
            if (this->native_handle != 0 && this->release_fn) {
                this->release_fn(this->native_handle);
            }
            this->native_handle = 0;
            this->add_use_fn = {};
            this->release_fn = {};
        }

        // called native_handle, not handle: the handle() accessor of this class would collide with a member of that name
        // called add_use_fn, not add_use: the add_use parameter of the constructor would hide a member of that name and
        // MSVC /W4 reports C4458, an error under /WX
        // called release_fn, not release: the release() method of this class would collide with a member of that name
        std::uint64_t native_handle = 0;
        std::function<void(std::uint64_t)> add_use_fn = {};
        std::function<void(std::uint64_t)> release_fn = {};
    };
} // namespace deren::vulkan
