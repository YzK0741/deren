// -*- C++ -*-
// ============================================================================
// module: deren.promise.rhi:api_core
//
// tier-1 of the promise contract: the backend's context and its factories (RHI
// plan v4, §1.11, §3.3, §4.1).
//
// `api_core` is the ONE object that crosses the boundary as a C++ type, and it is
// reached through a single C function, `deren_make_api_core` - declared in
// source/promise/rhi/backend_entry.hpp, the boundary's one C entry point, because an
// entry point is the ABI surface rather than a virtual base class (m03159). Everything
// below follows from the cross-boundary rules in §4.2:
//
//   - operations are virtual, the destructor is `virtual ... noexcept`, and the
//     common object root carries a static concrete implementation name (ABI30). No owning
//     data or non-inline definition crosses this interface: the vtable is the boundary,
//     and destruction has to reach the backend's own `operator delete`;
//   - an OWNED handle carries ONE REFERENCE, released by `release()`, once, inside the
//     backend; `unique_object<T>` is the owner spelling of that call (RAII, move-only).
//     RELEASE IS NOT NECESSARILY DESTRUCTION: the backend may serve the same resource to
//     several callers (a content-keyed registry + a reference count sit behind these
//     handles), so the object dies with its LAST reference and a caller must not read the
//     call as "the memory is free now" - nor touch the handle afterwards;
//   - a BORROWED view (`frame_image()`, `frame_readback_buffer()`, `begin_commands()`)
//     holds no reference at all and must never be managed;
//   - the backend hands the ROOT out as the contract's own `shared_object<api_core>` (abi 44),
//     whose control block's last copy calls the root's `release()` - the release runs inside the
//     backend, so no call site and no host ever names a deleter: the entry point returns the handle,
//     and nothing in the engine's ownership path frees DLL memory with the executable's allocator;
//   - `abilities()` is the tier-2 bit set (source/promise/rhi/rhi.extension.cppm) and
//     `query_extension()` is the only way to reach an ability, so adding one does
//     not grow this vtable;
//   - parameters are PODs, `std::span`, or opaque handles. No `deren::utility::vessel::string`, no
//     container, no exception, no `dynamic_cast` (`-fno-rtti`);
//   - the portable code touches nothing else, and this interface must never absorb
//     engine concepts: no scene, no pass, no camera, no toon (§4.1 item 2).
//
// The `*_desc` structs that have landed are exported by this partition, but their definitions live in
// the domain partitions (`resource/`, `pipeline/`, `recording/`, `frame/`, `core/`); their widths are
// part of the ABI, and the `struct_size` guards let callers omit trailing descriptor fields safely.
// ============================================================================
module;
#include <cstddef>
#include <cstdint>

export module deren.promise.rhi:api_core;
export import deren.vstd;
export import deren.utility.shared;

import :contract;
import :extension;
export import :resource;
export import :pipeline;
export import :recording;
export import :frame;
export import :core;

/**
 * @file source/promise/rhi/rhi.api_core.cppm
 * @brief tier-1 of the promise contract: the backend's context and its factories.
 * @ingroup promise
 *
 * `api_core` is the one object that crosses the boundary as a C++ type, and it is reached through a
 * single C function, `deren_make_api_core()` in `source/promise/rhi/backend_entry.hpp` (§1.11: the way to
 * obtain the context stays C++, the rest of the boundary is C). It is also the one interface the
 * plan lets grow "big" - it is the backend's whole context and factory set - under the one
 * restriction that it never learns an engine concept: no scene, no pass, no camera (§4.1 item 2).
 *
 * Applications and passes reach optional features through `abilities()` + `query_extension()`
 * (tier-2, `source/promise/rhi/rhi.extension.cppm`) rather than through new virtuals here, which is what
 * keeps this vtable stable as the backend gains extensions.
 */

export namespace deren::promise::rhi {

    // ================================ ABI 53 CONTRACT CLARIFICATIONS ================================
    // These are normative for every backend and complement the per-method comments. They clarify behavior
    // that was previously only in the reference backend or in docs, without changing the ABI 53 vtable.
    //
    // OWNERSHIP. Every handle derives from `object`; `release()` releases the caller's ONE reference and the
    // object dies with the last reference. A borrowed view carries no caller-owned reference: callers MUST
    // NOT invoke its underlying release(), and the backend MAY terminate on that ownership violation. A
    // borrow() wrapper's destruction/reset releases no reference. A backend MAY keep the underlying resource
    // alive for shared/content-keyed reuse, but MUST NOT expose a dead handle. Directly bound pipelines and
    // borrowed buffer/image operands remain caller-owned through pending CPU recording and GPU completion;
    // only verbs with explicit retention guarantees transfer that lifetime responsibility to the backend.
    // The backend core MUST outlive every handle, group, command buffer and pending submission.
    //
    //
    // RECORDING ERROR RULE. A handle-taking verb answers `error`; a value-only verb answers `void`. A void
    // verb latches its failure into the recording, and `end_recording()`/`apply_operations()` reports it and
    // makes the recording non-executable. Callers MUST serialize calls on one recorder; the contract does not
    // grant concurrent calls on the same command buffer.
    //
    // COMMAND DATA. `copy_buffer` MUST reject size==0, offsets past either buffer, or a range past either
    // buffer with `invalid_argument`; a backend MUST NOT pass an overrun to the native API. `bind_vertex_buffer`
    // and `bind_index_buffer` MUST reject an offset >= the buffer size. `clear_color_image`'s four components
    // are RGBA, and an all-zero subresource range means every mip/layer of the image. `mark_gpu_timing`'s
    // label text is borrowed static text that MUST stay valid for the process lifetime; the backend does not
    // copy it. `begin_rendering`/`end_rendering` MUST be paired and MUST NOT nest; an open scope at
    // `end_recording` fails that recording and nothing is published. `dispatch`/`draw_mesh_tasks` require an
    // open recording with the matching pipeline; a device that cannot run mesh work is reported through
    // `device_capabilities`, and a void mesh verb latches `invalid_argument` rather than drawing nothing
    // silently.
    //
    // API CORE. `wait_idle()` waits for all submitted work; it has no error channel (device loss surfaces on
    // later calls) and is legal outside a recording (inside one it does not end the recording).
    // `main_context()` is optional: null means the backend exposes no native/plugin interop context; a
    // non-null pointer is borrowed until api_core::release() and must be validated by tag before casting.
    //
    // FRAME VIEWS. `begin_commands()` returns the frame's primary (null outside recording) borrowed until the
    // frame is submitted; `frame_image()` is valid from acquire through submit until the next acquire;
    // `frame_readback_buffer()` is borrowed for the same window. Using any of them outside its window is a
    // contract violation the backend may refuse.
    //
    // ASSIGN TEXTURE GROUP. On success the returned shared handle owns one reference and `*result` (when
    // non-null) is `ok`; on failure the returned handle is empty and `*result` is the named error. Passing a
    // null `result` is allowed; empty (all-null) slots are legal and preserve group capacity. A backend that
    // does not support groups returns an empty handle with `unsupported`.
    //
    // RADIX SORT. `requirements(count)` reports scratch bytes, scratch alignment and scratch buffer flags; a
    // nonzero count with scratch_bytes==0 means the backend cannot serve that count. `record` validates the
    // operands and returns `invalid_argument` for a null/foreign buffer, an offset/size out of range or a
    // scratch size below the requirement. Internal native bindings may be overwritten, but the recorder's
    // logical pipeline/group/push state is preserved and re-established for later RHI consumers. All operand
    // buffers are borrowed through GPU completion.
    //
    // ==============================================================================================

    /// Read-only binding to a core-owned interface. Only api_core may bind it during construction.
    /// No ownership, allocation, release, reset, rebinding, or implicit pointer conversion.
    /// Arrow access requires a fully constructed/bound core. Const freezes binding, not device activity.
    template <class T>
    class context_view final {
        T* target = nullptr;
        friend struct api_core;
        context_view() noexcept = default;
        void bind(T& value) noexcept {
            target = &value;
        }

    public:
        context_view(context_view const&) = delete ("context_view is bound once to its owning api_core and cannot be copied");
        context_view& operator=(context_view const&) = delete ("context_view is bound once to its owning api_core and cannot be reassigned");
        [[nodiscard]] T* operator->() const noexcept {
            return target;
        }
        [[nodiscard]] T* get() const noexcept {
            return target;
        }
        [[nodiscard]] explicit operator bool() const noexcept {
            return target != nullptr;
        }
    };

    /// One measured CPU/GPU stage: latest sample, window average, and a stable volatility.
    /// All members are plain values; api_core serializes access to the owning map.
    struct stage_time_info {
        double last_ms = 0.0;
        double mean_ms = 0.0;
        double m2_ms2 = 0.0;
        double min_ms = std::numeric_limits<double>::infinity();
        double max_ms = 0.0;
        std::uint64_t samples = 0;

        void tick(double const ms) noexcept {
            this->last_ms = ms;
            ++this->samples;
            double const delta = ms - this->mean_ms;
            this->mean_ms += delta / static_cast<double>(this->samples);
            this->m2_ms2 += delta * (ms - this->mean_ms);
            this->min_ms = std::min(ms, this->min_ms);
            this->max_ms = std::max(ms, this->max_ms);
        }

        [[nodiscard]] double stddev_ms() const noexcept {
            return this->samples < 2 ? 0.0 : std::sqrt(this->m2_ms2 / static_cast<double>(this->samples - 1));
        }

        [[nodiscard]] double volatility() const noexcept {
            return this->samples == 0 || this->mean_ms == 0.0 ? 0.0 : this->stddev_ms() / this->mean_ms;
        }

        void reset_window() noexcept {
            this->mean_ms = 0.0;
            this->m2_ms2 = 0.0;
            this->min_ms = std::numeric_limits<double>::infinity();
            this->max_ms = 0.0;
            this->samples = 0;
        }
    };

    using stage_time_map = std::map<std::string, stage_time_info, std::less<>>;

    /// Device ownership, capabilities and global control. Public subviews borrow the same core.
    /// Derived constructors bind fully constructed interface implementations before publication.
    struct api_core : object {
        context_view<resource_factory> resources;
        context_view<command_context> commands;
        context_view<frame_context> frames;
        api_core() noexcept = default;
        virtual ~api_core() noexcept = default;
        virtual void release() noexcept = 0;
        [[nodiscard]] virtual ability_bits abilities() const noexcept = 0;
        [[nodiscard]] virtual extension* query_extension(extension_kind kind) noexcept = 0;
        virtual void wait_idle() = 0;
        [[nodiscard]] virtual std::uint32_t api_version() const noexcept = 0;
        [[nodiscard]] virtual main_context_view* main_context() noexcept {
            return nullptr;
        }

        /// A copy of the timing map, taken under the backend's lock.
        [[nodiscard]] virtual stage_time_map get_time_profiling() const = 0;
        /// Record one CPU or GPU stage sample under the backend's lock.
        virtual void record_stage_time(std::string_view name, double milliseconds) = 0;
        /// Clear the rolling window statistics, keeping each stage's latest sample.
        virtual void reset_time_profiling_window() = 0;

    protected:
        void bind_views(resource_factory& resource, command_context& command, frame_context& frame) noexcept {
            if (resources || commands || frames) {
                deren::utility::fast_panic("RHI core subviews already bound");
            }
            resources.bind(resource);
            commands.bind(command);
            frames.bind(frame);
        }
    };

    /// Checks the extension identity on a valid object that has completed the ABI handshake; it cannot
    /// validate a dangling pointer or an untrusted backend.
    template <typename ability>
    [[nodiscard]] ability* query_extension(api_core& core) noexcept {
        extension* const result = core.query_extension(ability::extension_id);
        if (result == nullptr || result->kind() != ability::extension_id) {
            return nullptr;
        }
        return static_cast<ability*>(result);
    }

} // namespace deren::promise::rhi
