// -*- C++ -*-
// ============================================================================
// module: deren.promise:api_core
//
// tier-1 of the promise contract: the backend's context and its factories (RHI
// plan v4, §1.11, §3.3, §4.1).
//
// `api_core` is the ONE object that crosses the boundary as a C++ type, and it is
// reached through a single C function, `deren_make_api_core` - declared with the
// other two entry points in boundary/backend_entry.hpp, because an entry point is
// the ABI surface rather than a virtual base class (m03159). Everything below
// follows from the cross-boundary rules in §4.2:
//
//   - every member is virtual, the destructor is `virtual ... noexcept`, and there
//     is no data member and no non-inline definition: the vtable is the boundary,
//     and destruction has to reach the backend's own `operator delete`;
//   - the engine builds `std::shared_ptr<api_core>(raw, &deren_destroy_api_core)`
//     (or the pointer it resolved from the loaded library) - never the default
//     deleter, which would free DLL memory with the executable's allocator;
//   - `abilities()` is the tier-2 bit set (promise/promise.extension.cppm) and
//     `query_extension()` is the only way to reach an ability, so adding one does
//     not grow this vtable;
//   - parameters are PODs, `std::span`, or opaque handles. No `std::string`, no
//     container, no exception, no `dynamic_cast` (`-fno-rtti`);
//   - the portable code touches nothing else, and this interface must never absorb
//     engine concepts: no scene, no pass, no camera, no toon (§4.1 item 2).
//
// The `*_desc` structs other than `buffer_desc` are forward-declared on purpose.
// This round lands the virtual base classes; the descriptor contents are S1 design
// surface (§5's capability table, §6.4's usage and barrier model). A reference to an
// incomplete type is all a virtual declaration needs, so the shape of the contract
// can be reviewed before the shapes it carries are frozen.
//
// The `std::shared_ptr` wrapper the engine uses belongs on the ENGINE side (plan
// §4.1 item 3: "住在模块里 inline 即可，不需要 DLL 配合"), so it is not here.
// ============================================================================
module;

#include <cstdint>

export module deren.promise:api_core;

import :extension;

/**
 * @file promise/promise.api_core.cppm
 * @brief tier-1 of the promise contract: the backend's context and its factories.
 * @ingroup promise
 *
 * `api_core` is the one object that crosses the boundary as a C++ type, and it is reached through a
 * single C function, `deren_make_api_core()` in `boundary/backend_entry.hpp` (§1.11: the way to
 * obtain the context stays C++, the rest of the boundary is C). It is also the one interface the
 * plan lets grow "big" - it is the backend's whole context and factory set - under the one
 * restriction that it never learns an engine concept: no scene, no pass, no camera (§4.1 item 2).
 *
 * Applications and passes reach optional features through `abilities()` + `query_extension()`
 * (tier-2, `promise/promise.extension.cppm`) rather than through new virtuals here, which is what
 * keeps this vtable stable as the backend gains extensions.
 */

export namespace deren::promise {

    /// The size of a buffer, in bytes.
    ///
    /// The only descriptor field this round fixes: `create_buffer()` needs something to
    /// carry across, and a byte count is not in doubt. The rest of the buffer
    /// descriptor (usage bits, memory properties) arrives with §6.4's usage model.
    struct buffer_desc {
        std::uint64_t size = 0; ///< bytes
    };

    /// A 3D extent in texels. POD, passed by value.
    struct image_extent {
        std::uint32_t width = 0;
        std::uint32_t height = 1;
        std::uint32_t depth = 1;
    };

    /// A buffer, owned by the backend.
    ///
    /// The engine holds it through this base and gives it back to the backend's own
    /// calls. Handing one back for destruction is S1's question, and the answer has to
    /// be the §4.2 rule unchanged: the release must run inside the backend, never as a
    /// `delete` from the engine's translation unit.
    struct buffer {
        virtual ~buffer() noexcept = default;

        [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    };

    /// An image, owned by the backend.
    struct image {
        virtual ~image() noexcept = default;

        [[nodiscard]] virtual image_extent extent() const noexcept = 0;
    };

    /// The descriptors of the remaining factories. Opaque until S1 (see the banner).
    struct image_desc;
    struct sampler_desc;
    struct shader_desc;
    struct pipeline_desc;
    struct swapchain_desc;
    struct query_desc;

    /// The remaining tier-1 objects: samplers, shaders, pipelines, swapchains, queries
    /// and command lists.
    ///
    /// Each one is a polymorphic handle and not much more yet: the operations the engine
    /// performs on them (bind, draw, dispatch, barrier, present) are exactly the S1
    /// design surface, held back by §6.4's "explicit barrier first, then shadow mode"
    /// decision. What they carry today is ownership: destroying one through this base
    /// has to reach the backend's destructor, which is the property the boundary rests
    /// on.
    struct sampler {
        virtual ~sampler() noexcept = default;
    };

    struct shader {
        virtual ~shader() noexcept = default;
    };

    struct pipeline {
        virtual ~pipeline() noexcept = default;
    };

    struct swapchain {
        virtual ~swapchain() noexcept = default;
    };

    struct query {
        virtual ~query() noexcept = default;
    };

    struct command_list {
        virtual ~command_list() noexcept = default;
    };

    /// What starting a frame hands back.
    ///
    /// §3.3 fixes the name and the fact that it is a value, not a handle; the fields the
    /// frame-in-flight ring and the timeline machinery need are §6.5's, so this is the
    /// smallest version that is still worth returning.
    struct submit_info {
        std::uint32_t frame_index = 0; ///< index in the frame-in-flight ring
        std::uint32_t image_index = 0; ///< the swapchain image this frame draws into
    };

    /// The backend's context and its factories - today's `vulkan::core`, behind the
    /// boundary.
    ///
    /// One object per backend instance, created by `deren_make_api_core()` and owned by
    /// the engine's `std::shared_ptr`. It is deliberately allowed to grow (it is the one
    /// interface the plan lets be "big"), and deliberately not allowed to learn engine
    /// concepts (§4.1 item 2).
    struct api_core {
        virtual ~api_core() noexcept = default;

        /// tier-2: the abilities this backend has, as `extension_kind` bits. Queried
        /// once at startup, then compared against what the passes need (§3.6).
        [[nodiscard]] virtual ability_bits abilities() const noexcept = 0;

        /// tier-2: the ability itself, or `nullptr` when it was not announced. The only
        /// entry point to abilities, which is what keeps this vtable stable when one is
        /// added. The caller `static_cast`s the result it asked for (no RTTI).
        [[nodiscard]] virtual extension* query_extension(extension_kind kind) noexcept = 0;

        // ---- factories ---------------------------------------------------------
        // A factory returns nullptr when it cannot honour the descriptor: the plan has
        // no throwing path across the boundary (§4.2), and the caller is expected to
        // have checked `abilities()` before asking for something optional (§3.6).
        [[nodiscard]] virtual swapchain* create_swapchain(swapchain_desc const& desc) = 0;
        [[nodiscard]] virtual buffer* create_buffer(buffer_desc const& desc) = 0;
        [[nodiscard]] virtual image* create_image(image_desc const& desc) = 0;
        [[nodiscard]] virtual sampler* create_sampler(sampler_desc const& desc) = 0;
        [[nodiscard]] virtual shader* create_shader(shader_desc const& desc) = 0;
        [[nodiscard]] virtual pipeline* create_pipeline(pipeline_desc const& desc) = 0;
        [[nodiscard]] virtual query* create_query(query_desc const& desc) = 0;

        // ---- frame -------------------------------------------------------------
        /// Start recording one frame's commands.
        [[nodiscard]] virtual command_list* begin_commands() = 0;

        /// Open a frame: the backend advances its frame-in-flight state and says which
        /// ring slot and which swapchain image this frame may use.
        [[nodiscard]] virtual submit_info frame_begin() = 0;

        /// Hand the recorded frame to the presentation engine.
        virtual void present() = 0;

        /// Block until nothing is in flight.
        virtual void wait_idle() = 0;
    };

} // namespace deren::promise
