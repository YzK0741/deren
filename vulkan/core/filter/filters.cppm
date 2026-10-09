// module version: 0.3.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/core/filter/filters.cppm
 * @brief The filtered views over a device root: one for the application (`user_filter`) and one for a pass's
 *        initialization (`pass_filter`).
 * @defgroup vulkan_core_filters Vulkan Core Filters
 *
 * WHY FILTERS AT ALL, and why two: the device root holds the instance, the device, the swapchain, the
 * allocator, every image and every descriptor pool. Handing that whole interface to a consumer hands it the
 * ability to do anything to the device, and the consumer then has no way to say what it actually needs. A
 * filter is a NAMED, narrow view of that root: the app gets one, a pass's init gets another, and each can
 * grow the operations its consumer is allowed to have.
 *
 * THE ROOT IS THE CONTRACT'S `rhi::api_core` NOW (dynamic-backend migration, step 2 / S2's shared batch).
 * This file used to hold `std::shared_ptr<core>` / `core*` and to reach the concrete class for the facts
 * behind a handful of accessors (`logical_device`, `window`, `swap_chain_extent`,
 * `swap_chain_image_format`, `vma`). MEASURED BEFORE CONVERTING, and the measurement is why this batch is
 * mostly a DELETION: of the eleven accessors those facts answered, TEN had ZERO callers anywhere in the
 * repository, and the eleventh is `user_filter::wait_idle()` - main.cpp's one call through `operator->`.
 * In particular `get_vma()` and `vma()` had no caller at all: the allocator never escapes a filter, and a
 * pass that allocates goes through `register_resource()`/`resource()`, the owner's published-resource
 * channel, which touches no backend type. So the concrete class leaves WITHOUT a replacement contract
 * virtual and WITHOUT an ABI bump - adding a `vma_allocator` face to the contract would have been adding an
 * interface for a caller that does not exist.
 *
 * THE TWO FILTERS, and the line between them:
 *
 *  * `user_filter` - what the RUNTIME exposes to the application through its `operator->`. It holds a
 *    `std::shared_ptr<rhi::api_core>` (so it keeps the device alive while it exists) and forwards
 *    `wait_idle()`, the one accessor a consumer actually calls. It used to forward the window, the
 *    swapchain's extent and format, the current frame, the device and the allocator as well; every one of
 *    those had zero callers.
 *
 *  * `pass_filter` - what a PASS's create step is given. It answers exactly the questions a pass cannot
 *    answer from its own declaration: session-stable handles of the resources the owner registered. It
 *    holds NO device root at all - its two live members touch nothing but its own table, which is what
 *    makes it identical on both sides of the boundary.
 */

module;

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

export module deren.vulkan.core.filters;
// THIS FILE NO LONGER EXPORTS A BACKEND MODULE. It used to `export import deren.vulkan.core;`, which is
// what made `deren.vulkan.core.filters` a re-porter of the whole concrete class (and why a consumer could
// reach `core::MAX_FRAMES_IN_FLIGHT` through it transitively). The contract is imported PLAINLY below: the
// two names a consumer needs are `rhi::max_frames_in_flight` (re-exported as this facade's own constant)
// and `rhi::api_core` (a member type, not part of any consumer's spelling).
export import deren.vstd;
import deren.promise.rhi;                   // the contract's device root, and its ring depth
export import deren.vulkan.render_resource; // resource_id: what a pass asks for, in the declaration's own vocabulary

export namespace deren::vulkan {

    /**
     * @brief one resource's device handles, as a pass sees them
     *
     * The same three handles `deren::vulkan::pass::resolved_binding` carries at record time - a descriptor takes a
     * VIEW, a barrier takes an IMAGE, and a buffer binding takes a BUFFER - but declared here, on the core side,
     * because `render_resource` is deliberately pure CPU data (no Vulkan type in it) and the framework must not
     * depend on this module. The runtime copies the three fields across, which is the whole conversion.
     */
    struct resource_handles {
        /// THE CONTRACT LANE, AND ONLY IT (plan X5 B2): the native trio that stood here is gone, so a
        /// publisher hands over the object a pass's recording verbs take rather than a spelling of it.
        deren::promise::rhi::image_view* view = nullptr;
        deren::promise::rhi::buffer* buffer = nullptr;
        deren::promise::rhi::image* image = nullptr;
    };
    /**
     * @ingroup vulkan_core_filters
     * @brief the application's view of the device root, exposed by `runtime::operator->`
     * @note
     *      - holds a `std::shared_ptr<rhi::api_core>`, so it keeps the device alive while it exists and it
     *        is the SAME interface on both sides of the boundary (before the flip the vtable points into
     *        the static archive, after it into the loaded library)
     *      - the runtime exposes it via `operator->`, so external code never sees the raw device root
     *      - frame management (acquire/submit/present) is deliberately not forwarded: it belongs to the
     *        runtime
     */
    class user_filter {
        // called owner_share, not owner: the owner parameter of the constructor would hide a member of that name and
        // MSVC /W4 reports C4458, an error under /WX
        /// the share that keeps the device alive, and the interface pointer the one forwarded call goes through
        std::shared_ptr<deren::promise::rhi::api_core> owner_share;
        deren::promise::rhi::api_core* core_face = nullptr;

    public:
        explicit user_filter(std::shared_ptr<deren::promise::rhi::api_core> owner) noexcept;

        /// the ring depth the engine sizes its per-slot arrays by. THIS STAYS, and it is why the contract
        /// import above is not an `export import`: the value is re-exported as this facade's own constant,
        /// so a consumer names `user_filter::max_frames_in_flight` and never a backend module.
        static constexpr int32_t max_frames_in_flight = static_cast<int32_t>(deren::promise::rhi::max_frames_in_flight);

        /// the ONE forwarded operation a consumer calls (main.cpp at shutdown): a contract virtual, so this
        /// emits no backend symbol on either side of the boundary.
        void wait_idle() const noexcept;
    };

    /**
     * @ingroup vulkan_core_filters
     * @brief the view a PASS's create step is handed: the resources the owner published, and nothing else
     *
     * WHAT IT IS FOR, in one sentence: a pass must be able to NAME the session-stable resources its own
     * declaration lists, without being handed the device root - which is what the runtime did on its behalf
     * before this filter existed (see `runtime::create_mask_bake` and `runtime::create_compute_skin`, whose
     * bespoke input structs this replaces).
     *
     * IT HOLDS NO DEVICE ROOT, and that is a measured fact rather than a design preference (S2's shared
     * batch): its two live members - `register_resource` (called by the runtime) and `resource` (called by
     * the pass framework) - touch nothing but the table below. It used to hoist `shared_ptr<core>` for a
     * device, a surface format, a surface extent and an allocator, none of which had a caller.
     *
     * THE LIFETIME CONTRACT, and it is the reason this class is small: what `resource()` answers at CREATE time
     * is a SESSION-STABLE handle. The runtime's own material table and its bindless texture array are created
     * once and only ever have their CONTENTS rewritten; its per-frame-slot buffers (the skin matrices, the
     * cluster bins) are created once and rewritten per slot. A per-swapchain-image VIEW is NOT stable that way -
     * it is rebuilt with every generation - so those are deliberately not served here: a pass receives them per
     * frame through `resolved_io` (the framework's `own` / `own_per_image` / `barrier_images` channels). Handing
     * one out at create time would be the per-image-lifetime trap this branch has already paid for twice.
     */
    class pass_filter {
        /**
         * What the OWNER (the runtime) has published for its passes to name.
         *
         * A vector rather than a map: the resources a pass may ask for are a handful, this is filled once at
         * startup, and the lookup happens during create - so the smallest container that works is the honest
         * one. A `resource_id` that was never registered resolves to a null handle, which is what a pass's
         * "this owner has none" branch already handles.
         */
        std::vector<std::pair<uint32_t, resource_handles>> registered;

    public:
        /// an empty table: the runtime publishes what it has through `register_resource`
        pass_filter() noexcept = default;

        /// @brief publish one of the owner's resources, by the DECLARATION's identity (resource + element)
        void register_resource(render_resource::resource_id id, uint32_t element, resource_handles handles) noexcept;
        /**
         * @brief what this pass declared, if the owner has it: session-stable handles, or all-null
         * @param id the resource, from the same `resource_id` vocabulary the declaration uses
         * @param element which one of the family: the frame SLOT for a per-frame-slot resource (the skin
         *        matrices, the cluster bins), or 0 for a device-wide one
         */
        [[nodiscard]] resource_handles resource(render_resource::resource_id id, uint32_t element) const noexcept;
    };

} // namespace deren::vulkan
