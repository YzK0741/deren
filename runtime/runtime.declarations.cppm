// ============================================================================
// module: deren.vulkan.runtime:declarations  - the runtime's interface (step 2, S1)
//
// THE CLASS THE APPLICATION NAMES IS THE LEGACY CLASS'S NAME AND SHAPE (`deren::vulkan::runtime`): the file
// list is what changes at the swap, never the call sites. What is GONE is the backend's class: there is no
// `core&` member here and no `deren.vulkan.core` import - the device root is the contract's `api_core`,
// obtained through the C entry.
// ============================================================================
module;

#include <memory> // std::shared_ptr: the device root's owner

export module deren.vulkan.runtime:declarations;

import deren.promise.rhi; // the contract: create_info, error_info, the api_core face, the escape
import deren.utility;     // panic/log: the startup diagnosis a refused factory comes back with

namespace deren::vulkan {

    /// The contract's namespace under the short name every site in this module uses - the legacy runtime
    /// spells it the same way, so the two files read alike (`main.cpp` has an alias of its own).
    namespace rhi = deren::promise::rhi;

    /**
     * @ingroup vulkan_runtime
     * @brief THE RUNTIME THAT NEVER NAMES `core`: the renderer, built on the contract alone.
     *
     * @note S1 IS THE SCAFFOLDING - the construction, the contract face, the raw-handle escape and the
     *       teardown. The frame loop, the render chain's creation run and the passes' plumbing arrive with
     *       the partitions the port adds; until they do, this class is what the dynamic tree's scaffold test
     *       constructs and destroys, so that every slice of the port can say "the dynamic side still builds
     *       and still constructs" instead of hoping.
     */
    export class runtime {
    public:
        /// the standard creation: the contract's empty `create_info`, exactly as the legacy runtime's
        /// default constructor spells it
        runtime();
        /// the one construction the application uses: the contract's creation structure, handed to the C
        /// entry (`deren_make_api_core`) instead of to a backend constructor the engine could name
        explicit runtime(rhi::create_info const& options);
        ~runtime();

        // THE DEVICE ROOT IS ONE OWNER, so copying it is not a thing: a second copy would release a
        // reference it never took (the same rule the legacy runtime states at its own deletion lines).
        runtime(runtime const&) = delete;
        runtime& operator=(runtime const&) = delete;

        /**
         * @brief the contract face of the object the backend built
         *
         * @note this is the interface reference of the SAME object the C entry handed over: before the flip
         *       the vtable points into this archive, after it into the loaded library, and the call sites
         *       cannot tell the difference - which is what makes the two runtimes' bodies interchangeable.
         */
        [[nodiscard]] rhi::api_core& rhi_face() const noexcept;
        /**
         * @brief the raw-handle escape (`query_extension<vulkan_escape>`), for the code the contract cannot
         *        express yet - the same accessor the legacy runtime hands its call sites
         */
        [[nodiscard]] rhi::vulkan_escape& escape() const noexcept;
        /// whether this runtime holds a core at all. A refused factory PANICS rather than answering false
        /// here (see `:constructor`), so this is for diagnostics, not for error handling.
        [[nodiscard]] bool valid() const noexcept {
            return static_cast<bool>(this->core_owner);
        }

    private:
        /**
         * THE DEVICE ROOT, AND WHY IT IS A `shared_ptr` WITH THE BACKEND'S DELETER: the object lives in the
         * backend's image, so the destruction has to run THERE - `deren_destroy_api_core` is the backend's
         * own symbol, from the same entry header `deren_make_api_core` came from (plan §4.1 item 3), and
         * `delete` would free memory this image never allocated.
         *
         * IT IS DECLARED FIRST SO IT IS DESTROYED LAST: every member the later partitions add holds handles
         * derived from this object (images, views, the swapchain) and must be released before it goes.
         */
        std::shared_ptr<rhi::api_core> core_owner = {};
    };
} // namespace deren::vulkan
