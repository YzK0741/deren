// -*- C++ -*-
// ============================================================================
// module: deren.promise.rhi:error
// ============================================================================
module;
#include <cstddef>
#include <cstdint>

export module deren.promise.rhi:error;
export import deren.vstd;

import deren.utility.shared;

export namespace deren::promise::rhi {

    inline constexpr std::uint32_t abi_version = 61u;

    /// Why a promise entry point could not do what it was asked.
    ///
    /// An exception must not cross the boundary (plan §4.2), so every failure a
    /// backend can report travels as one of these through an out-parameter.
    ///
    /// `abi_mismatch` = 7 is not a system error number: it is the code the minimal
    /// use case measured in plan §10.3 reports, and it is kept here so that the
    /// refusal stays observable from the engine side.
    ///
    /// Values 13-18 are APPENDED (2026-10-05, the error-mechanism batch) - never renumber, never
    /// reuse: an old engine compiled against 0-12 receives a code it has never seen and still has to
    /// route it somewhere, which appending keeps possible. Each value names the class of failure the
    /// CALLER acts on, because that is what a code crossing a boundary is for; the raw API-specific
    /// number travels beside it in `error_info::native_code`.
    ///
    /// `suboptimal` (Vulkan's present/acquire state) deliberately has NO value here: it is a state,
    /// not a failure, and the same VkResult means "carry on" at acquire and "rebuild" at present -
    /// the translation is the CALL SITE's decision, not the enum's.
    enum class error : std::uint32_t {
        ok = 0,                     ///< the call did what it was asked
        abi_mismatch = 7,           ///< the caller's abi_version is not the backend's
        unsupported = 8,            ///< the backend has no mechanism that can serve THIS resource/format
        invalid_argument = 9,       ///< the region does not fit the image, or the destination is too small
        not_ready = 10,             ///< no frame is in flight, or the frame that drew it is not done
        device_lost = 11,           ///< the device refused the submission/copy (VkResult failure)
        operation_failed = 12,      ///< underlying operation failed without a more precise error channel
        out_of_date = 13,           ///< the swapchain/target expired: rebuild it and retry, skipping this frame
        surface_lost = 14,          ///< the window/surface is gone: rebuild it; if the window closed, exit the loop
        timeout = 15,               ///< a wait or a query ran out of time: retryable, or degrade and carry on
        out_of_device_memory = 16,  ///< device memory exhausted: UNRECOVERABLE - the engine panics (the host is healthy, so the message and crash log can still be written)
        out_of_host_memory = 17,    ///< host memory exhausted: UNRECOVERABLE - the engine fast-panics (no allocation is left to write a message or run the crash machinery)
        initialization_failed = 18, ///< creation failed and no code above names the cause: report and exit
    };

    /// Shader parameter writes use four-byte units. Subtract only after comparing offset to avoid
    /// overflow even for a UINT64_MAX byte count. Empty writes are invalid, not silent no-ops.
    [[nodiscard]] constexpr error validate_push_constants_range(std::uint32_t offset, std::uint64_t size,
                                                                std::uint32_t limit) noexcept {
        return size == 0 || (offset & 3u) != 0 || (size & 3u) != 0 || offset > limit || size > limit - offset
                   ? error::invalid_argument
                   : error::ok;
    }

    /// Which subsystem an `error` belongs to - as the CONTRACT derives it, never as a stored field.
    ///
    /// A zone stored next to the code would be a second source of truth able to contradict it (the
    /// same reason `validate_structure` answers a code rather than filling in a struct): inside the
    /// contract the zone is a pure function of the code, so `zone_of` is the only way it exists.
    /// Zones that genuinely carry independent information - the application's graphics / asset /
    /// config / platform / internal vocabulary - live at the application layer, where the caller knows
    /// WHICH subystem asked for the work; the contract does not carry that field for them.
    enum class error_zone : std::uint8_t {
        api = 0,      ///< the device / window / frame pipeline refused or deferred the call
        resource = 1, ///< the backend has no mechanism that can serve the request
        argument = 2, ///< the caller passed something that cannot work as given
        internal = 3, ///< the backend's machinery refused, mismatched or broke
    };

    /// The zone of @p code - total over the enum, so a new value forces its way through this switch
    /// (-Werror) and cannot silently inherit another zone.
    [[nodiscard]] constexpr error_zone zone_of(error const code) noexcept {
        switch (code) {
        case error::unsupported:
            return error_zone::resource;
        case error::invalid_argument:
            return error_zone::argument;
        // The api zone is "the device/window/frame state refused or deferred the call": a lost
        // device, an expired swapchain, a gone surface, a wait that ran out, memory the device or
        // host no longer has - and `not_ready`, which is the same family (the frame pipeline is
        // simply not there yet). Most of these mean retry / rebuild / degrade, not "fix the
        // arguments" and not "report a backend bug". THE TWO MEMORY CODES ARE THE EXCEPTION: memory
        // exhaustion is UNRECOVERABLE by the engine's policy, which terminates - `panic` for a device
        // OOM (the host can still format a message and write the crash log) and `fast_panic` for a
        // host OOM (there is no allocation left to do either). See docs/error_handling.md.
        case error::not_ready:
        case error::device_lost:
        case error::out_of_date:
        case error::surface_lost:
        case error::timeout:
        case error::out_of_device_memory:
        case error::out_of_host_memory:
            return error_zone::api;
        // internal = the machinery, not the request: `ok` (a successful call's zone when a caller
        // wants one), the ABI handshake's refusal, a catch-all failure and a creation that
        // failed under no heading above.
        case error::ok:
        case error::abi_mismatch:
        case error::operation_failed:
        case error::initialization_failed:
            return error_zone::internal;
        }
        return error_zone::internal; // unreachable for a valid enumerator; -Wswitch still guards above
    }

    /// Which graphics API produced a diagnostic. Append-only, same rule as `error` itself.
    enum class graphics_api : std::uint8_t {
        unknown = 0,
        vulkan = 1,
    };

    /// The decision AND the diagnosis of one failure, as the backend's failure point reported it.
    ///
    /// FROZEN BY RULE - the contract's by-value PODs are frozen (`submit_info` set the precedent):
    /// this struct is returned and carried BY VALUE, so its size is part of the calling convention an
    /// old engine and a new backend would disagree on. Any field addition moves `abi_version` - it is
    /// the same reason the factory's out-parameter changes `error*` -> `error_info*` only together
    /// with that bump. (An out-parameter structure the CALLER allocates could carry a `struct_size`
    /// header and grow safely; a by-value one cannot.)
    ///
    /// Three premises this definition rests on, each one a reason the next edit should think twice:
    /// 1. BOTH HALVES COMPILE WITH THE SAME TOOLCHAIN. `std::source_location`'s layout is
    ///    implementation-defined; the engine and a backend DLL compare layouts, not definitions.
    ///    That was already implied by `vstd` (the trimmed std module both halves share) and by the
    ///    BMI exchange; stating it here makes it a clause. The static_asserts below pin the layout
    ///    clang64 compiles, so a toolchain change fails loudly here instead of at a call site.
    /// 2. `message` and `where`'s text point into the BACKEND's static storage - which outlives the
    ///    call only because invariant 4 holds: the backend DLL is loaded once and NEVER unloaded
    ///    (the loader's `detach()` exists for exactly this). A backend that unloads hands its host
    ///    dangling text.
    /// 3. `native_code` is SIGNED on purpose: the Vulkan codes it carries are negative
    ///    (VK_ERROR_OUT_OF_DATE_KHR = -1000001004), and that number stored in an unsigned field is a
    ///    different number by the time a human reads it.
    struct error_info {
        error code = error::ok;                   ///< the decision layer: what the caller acts on
        graphics_api api = graphics_api::unknown; ///< who produced the diagnostic
        std::int32_t native_code = 0;             ///< the raw API code (signed; see premise 3)
        std::string_view message = {};            ///< the backend's static text, possibly empty
        std::source_location where = {};          ///< captured at the BACKEND's failure point, not the engine's call site
    };

    // The pinned layout (clang64: clang 22.x, libc++, x86-64). `code` 4 bytes, `api` 1 + 3 padding,
    // `native_code` 4, then `message` (a char const* + a size_t) and `where` land on their 8-byte
    // alignments. `where` is 8 bytes, not 24: libc++'s std::source_location holds ONE pointer to a
    // static __impl record (file/function/line/column), which also means the text it names lives in
    // static storage - premise 2's guarantee, measured rather than assumed. Trivially copyable is the
    // DESIGN constraint - the struct crosses a C-shaped boundary by value - and standard layout is
    // what makes offsetof below defined at all.
    static_assert(std::is_trivially_copyable_v<error_info>, "error_info crosses the boundary by value; it must stay trivially copyable");
    static_assert(std::is_standard_layout_v<error_info>, "error_info's pinned offsets below need standard layout");
    static_assert(sizeof(error_info) == 40, "error_info changed size - by-value PODs are frozen: moving abi_version is part of the change");
    static_assert(offsetof(error_info, code) == 0, "error_info's pinned layout moved");
    static_assert(offsetof(error_info, api) == 4, "error_info's pinned layout moved");
    static_assert(offsetof(error_info, native_code) == 8, "error_info's pinned layout moved");
    static_assert(offsetof(error_info, message) == 16, "error_info's pinned layout moved");
    static_assert(offsetof(error_info, where) == 32, "error_info's pinned layout moved");

} // namespace deren::promise::rhi
